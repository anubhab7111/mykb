#include "sources/hyprland.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace omenkbd {

namespace {

using namespace std::chrono_literals;

/// Hyprland lines carry window titles, so they can be long, but never this long.
constexpr std::size_t kMaxLine = 64 * 1024;

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

std::optional<std::filesystem::path> socket2_path() {
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || *runtime == '\0') {
        return std::nullopt;
    }
    const std::filesystem::path hypr = std::filesystem::path(runtime) / "hypr";
    std::error_code ec;
    if (const char *sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE"); sig != nullptr && *sig) {
        const auto p = hypr / sig / ".socket2.sock";
        if (std::filesystem::exists(p, ec)) {
            return p;
        }
    }
    std::optional<std::filesystem::path> newest;
    std::filesystem::file_time_type newest_time{};
    for (const auto &entry : std::filesystem::directory_iterator(hypr, ec)) {
        const auto p = entry.path() / ".socket2.sock";
        const auto t = std::filesystem::last_write_time(p, ec);
        if (!ec && (!newest || t > newest_time)) {
            newest = p;
            newest_time = t;
        }
    }
    return newest;
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
    const auto path = path_fn_();
    sockaddr_un addr{};
    if (!path || path->native().size() >= sizeof(addr.sun_path)) {
        retry_later();
        return;
    }
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path->c_str(), path->native().size());
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    // A Unix-socket connect either completes at once or fails (EAGAIN = backlog full).
    if (!fd || ::connect(fd.get(), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
        retry_later();
        return;
    }
    failures_ = 0;
    fd_ = std::move(fd);
    loop_.watch(fd_.get(), EPOLLIN, [this](std::uint32_t) { on_readable(); });
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
