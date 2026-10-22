#include "sources/hyprland.hpp"

#include <algorithm>
#include <cerrno>
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

} // namespace

std::optional<Event> parse_socket2_line(std::string_view line) {
    constexpr std::string_view kPrefix = "submap>>";
    if (!line.starts_with(kPrefix)) {
        return std::nullopt;
    }
    const std::string_view name = line.substr(kPrefix.size());
    if (name.empty()) {
        return Event{event::Submap{std::nullopt}};
    }
    return Event{event::Submap{std::string(name)}};
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
            loop_.watch(fd_.get(), EPOLLIN, [this](std::uint32_t) { on_readable(); });
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
    lines_ = LineBuffer{};
    emit_(Event{event::Submap{std::nullopt}});
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
