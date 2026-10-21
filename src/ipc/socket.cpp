#include "ipc/socket.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace omenkbd {

namespace {

IpcError io_error(std::string_view what) {
    return {IpcError::Kind::Io, std::format("{}: {}", what, std::strerror(errno))};
}

std::expected<sockaddr_un, IpcError> make_addr(const std::filesystem::path &path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string s = path.string();
    if (s.size() >= sizeof(addr.sun_path)) {
        return std::unexpected(IpcError{IpcError::Kind::Io, "socket path too long: " + s});
    }
    std::memcpy(addr.sun_path, s.c_str(), s.size() + 1);
    return addr;
}

/// Blocking connect. ENOENT/ECONNREFUSED ⇒ NotRunning.
std::expected<UniqueFd, IpcError> connect_to(const std::filesystem::path &path) {
    auto addr = make_addr(path);
    if (!addr) {
        return std::unexpected(addr.error());
    }
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd) {
        return std::unexpected(io_error("socket"));
    }
    if (::connect(fd.get(), reinterpret_cast<const sockaddr *>(&*addr), sizeof(*addr)) != 0) {
        if (errno == ENOENT || errno == ECONNREFUSED) {
            return std::unexpected(IpcError{IpcError::Kind::NotRunning, ""});
        }
        return std::unexpected(io_error("connect " + path.string()));
    }
    return fd;
}

std::expected<void, IpcError> send_line(int fd, const std::string &line) {
    std::string_view rest = line;
    while (!rest.empty()) {
        const ssize_t n = ::send(fd, rest.data(), rest.size(), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(io_error("send"));
        }
        rest.remove_prefix(static_cast<std::size_t>(n));
    }
    return {};
}

/// Reads until `lines` holds a complete line. nullopt on EOF.
std::expected<std::optional<std::string>, IpcError> read_line(int fd, LineBuffer &lines) {
    std::array<char, 4096> buf{};
    while (true) {
        if (auto l = lines.next_line()) {
            return l;
        }
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return std::unexpected(
                    IpcError{IpcError::Kind::Protocol, "timed out waiting for the daemon"});
            }
            return std::unexpected(io_error("recv"));
        }
        if (n == 0) {
            return std::optional<std::string>{};
        }
        lines.append(std::string_view(buf.data(), static_cast<std::size_t>(n)));
    }
}

} // namespace

std::string IpcError::message() const {
    switch (kind) {
    case Kind::AlreadyRunning:
        return "omen-kbd daemon already running";
    case Kind::NotRunning:
        return "daemon not running";
    case Kind::Io:
        return detail;
    case Kind::Protocol:
        return "protocol: " + detail;
    }
    return detail;
}

std::filesystem::path socket_path() {
    const char *dir = std::getenv("XDG_RUNTIME_DIR");
    if (dir != nullptr && *dir != '\0') {
        return std::filesystem::path(dir) / "omen-kbd.sock";
    }
    return std::filesystem::path(std::format("/tmp/omen-kbd-{}.sock", ::getuid()));
}

std::expected<UniqueFd, IpcError> bind_socket(const std::filesystem::path &path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        if (connect_to(path)) {
            return std::unexpected(IpcError{IpcError::Kind::AlreadyRunning, ""});
        }
        std::filesystem::remove(path, ec);
    }
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    auto addr = make_addr(path);
    if (!addr) {
        return std::unexpected(addr.error());
    }
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd) {
        return std::unexpected(io_error("socket"));
    }
    if (::bind(fd.get(), reinterpret_cast<const sockaddr *>(&*addr), sizeof(*addr)) != 0) {
        return std::unexpected(io_error("bind " + path.string()));
    }
    if (::chmod(path.c_str(), 0600) != 0) {
        return std::unexpected(io_error("chmod " + path.string()));
    }
    if (::listen(fd.get(), 16) != 0) {
        return std::unexpected(io_error("listen"));
    }
    return fd;
}

std::optional<std::string> LineBuffer::next_line() {
    const auto nl = buf_.find('\n');
    if (nl == std::string::npos) {
        return std::nullopt;
    }
    std::string line = buf_.substr(0, nl);
    buf_.erase(0, nl + 1);
    if (line.ends_with('\r')) {
        line.pop_back();
    }
    return line;
}

std::expected<Response, IpcError> request(const std::filesystem::path &path, const Request &r) {
    auto fd = connect_to(path);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    timeval tv{.tv_sec = 5, .tv_usec = 0};
    ::setsockopt(fd->get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (auto s = send_line(fd->get(), encode(r) + "\n"); !s) {
        return std::unexpected(s.error());
    }
    LineBuffer lines;
    auto line = read_line(fd->get(), lines);
    if (!line) {
        return std::unexpected(line.error());
    }
    if (!*line) {
        return std::unexpected(IpcError{IpcError::Kind::Protocol, "daemon closed the connection"});
    }
    auto resp = decode_response(**line);
    if (!resp) {
        return std::unexpected(IpcError{IpcError::Kind::Protocol, resp.error()});
    }
    return *resp;
}

std::expected<std::optional<Status>, IpcError> Subscription::next_status() {
    auto line = read_line(fd_.get(), lines_);
    if (!line) {
        return std::unexpected(line.error());
    }
    if (!*line) {
        return std::optional<Status>{};
    }
    auto resp = decode_response(**line);
    if (!resp) {
        return std::unexpected(IpcError{IpcError::Kind::Protocol, resp.error()});
    }
    if (resp->status) {
        return std::optional<Status>(std::move(*resp->status));
    }
    return std::unexpected(IpcError{IpcError::Kind::Protocol,
                                    resp->error.value_or("subscription message without status")});
}

std::expected<Subscription, IpcError> subscribe(const std::filesystem::path &path) {
    auto fd = connect_to(path);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    if (auto s = send_line(fd->get(), encode(Request{req::Subscribe{}}) + "\n"); !s) {
        return std::unexpected(s.error());
    }
    return Subscription(std::move(*fd));
}

} // namespace omenkbd
