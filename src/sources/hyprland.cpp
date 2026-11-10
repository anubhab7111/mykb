#include "sources/hyprland.hpp"

#include "core/window_address.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace omenkbd {

namespace {

using namespace std::chrono_literals;

/// Hyprland lines carry window titles, so they can be long, but never this long.
constexpr std::size_t kMaxLine = std::size_t{64} * 1024;

/// A connected non-blocking socket, or an empty fd. A Unix-socket connect either completes at
/// once or fails (ECONNREFUSED = nothing listens any more, EAGAIN = backlog full).
UniqueFd connect_to(const std::filesystem::path &path) {
    sockaddr_un addr{};
    if (path.native().size() >= sizeof(addr.sun_path)) {
        return UniqueFd();
    }
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.native().size());
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd || ::connect(fd.get(), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
        return UniqueFd();
    }
    return fd;
}

/// The text before the first comma, or all of `s`. Later fields can hold a window title.
std::string_view before_comma(std::string_view s) { return s.substr(0, s.find(',')); }

/// A whole-text integer: no sign but `-`, no spaces, no trailing text, no overflow.
std::optional<int> parse_id(std::string_view s) {
    int id = 0;
    const auto [stop, ec] = std::from_chars(s.data(), s.data() + s.size(), id);
    if (ec != std::errc{} || stop != s.data() + s.size()) {
        return std::nullopt;
    }
    return id;
}

/// An event that names a window and nothing else we keep; a missing address makes it malformed.
template <class E> std::optional<HyprEvent> with_address(std::string_view data) {
    std::string address = normalize_window_address(before_comma(data));
    if (address.empty()) {
        return std::nullopt;
    }
    return HyprEvent{E{std::move(address)}};
}

} // namespace

std::optional<HyprEvent> parse_socket2_line(std::string_view line) {
    if (line.size() > kMaxLine) {
        return std::nullopt;
    }
    const std::size_t arrows = line.find(">>");
    if (arrows == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view name = line.substr(0, arrows);
    const std::string_view data = line.substr(arrows + 2);
    if (name == "submap") {
        if (data.empty()) {
            return HyprEvent{hypr::Submap{std::nullopt}};
        }
        return HyprEvent{hypr::Submap{std::string(data)}};
    }
    if (name == "workspacev2") {
        if (const auto id = parse_id(before_comma(data))) {
            return HyprEvent{hypr::WorkspaceV2{*id}};
        }
        return std::nullopt;
    }
    if (name == "openwindow") {
        return with_address<hypr::OpenWindow>(data);
    }
    if (name == "closewindow") {
        return with_address<hypr::CloseWindow>(data);
    }
    if (name == "urgent") {
        return with_address<hypr::Urgent>(data);
    }
    if (name == "activewindow") {
        return HyprEvent{hypr::ActiveWindow{std::string(before_comma(data))}};
    }
    if (name == "activewindowv2") {
        return HyprEvent{hypr::ActiveWindowV2{normalize_window_address(before_comma(data))}};
    }
    if (name == "fullscreen" && (data == "0" || data == "1")) {
        return HyprEvent{hypr::Fullscreen{data == "1"}};
    }
    return std::nullopt; // windowtitle, windowtitlev2 and everything else
}

std::vector<std::filesystem::path> socket2_paths() {
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || *runtime == '\0') {
        return {};
    }
    const std::filesystem::path hypr = std::filesystem::path(runtime) / "hypr";
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (const char *sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE"); sig != nullptr && *sig) {
        auto p = hypr / sig / ".socket2.sock";
        if (std::filesystem::exists(p, ec)) {
            out.push_back(std::move(p));
        }
    }
    using Candidate = std::pair<std::filesystem::file_time_type, std::filesystem::path>;
    std::vector<Candidate> others;
    // Not the range-for form: its increment throws when reading the directory fails.
    for (std::filesystem::directory_iterator it(hypr, ec), end; !ec && it != end;
         it.increment(ec)) {
        auto p = it->path() / ".socket2.sock";
        std::error_code time_ec;
        const auto t = std::filesystem::last_write_time(p, time_ec);
        if (!time_ec && (out.empty() || p != out.front())) {
            others.emplace_back(t, std::move(p));
        }
    }
    std::ranges::sort(others, std::ranges::greater{}, &Candidate::first);
    for (auto &c : others) {
        out.push_back(std::move(c.second));
    }
    return out;
}

HyprlandSource::HyprlandSource(EventLoop &loop, PathFn path_fn, Emit emit)
    : loop_(loop), path_fn_(std::move(path_fn)), emit_(std::move(emit)) {
    connect();
}

HyprlandSource::~HyprlandSource() {
    if (retry_) {
        loop_.cancel(*retry_);
    }
    if (fd_) {
        loop_.unwatch(fd_.get());
    }
}

void HyprlandSource::connect() {
    for (const auto &path : path_fn_()) {
        if (UniqueFd fd = connect_to(path)) {
            failures_ = 0;
            fd_ = std::move(fd);
            connected_path_ = path;
            loop_.watch(fd_.get(), EPOLLIN, [this](std::uint32_t) { on_readable(); });
            emit_(hypr::Connected{});
            return;
        }
    }
    retry_later();
}

void HyprlandSource::on_readable() {
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd_.get(), buf, sizeof(buf));
        if (n > 0) {
            lines_.append({buf, static_cast<std::size_t>(n)});
            while (auto line = lines_.next_line()) {
                if (auto ev = parse_socket2_line(*line)) {
                    emit_(*ev);
                }
            }
            if (lines_.pending() > kMaxLine) {
                lines_ = LineBuffer{};
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        disconnected(); // EOF or error
        return;
    }
}

void HyprlandSource::disconnected() {
    loop_.unwatch(fd_.get());
    fd_.reset();
    connected_path_.reset();
    lines_ = LineBuffer{};
    emit_(hypr::Disconnected{});
    retry_later();
}

void HyprlandSource::retry_later() {
    const auto delay = failures_ == 0 ? 1s : failures_ == 1 ? 2s : 5s;
    ++failures_;
    retry_ = loop_.at(Clock::now() + delay, [this] {
        retry_.reset();
        connect();
    });
}

} // namespace omenkbd
