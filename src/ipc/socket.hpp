#pragma once
// The user-only Unix socket (`$XDG_RUNTIME_DIR/omen-kbd.sock`, mode 0600): server bind, line
// framing, and the blocking client used by the CLI.

#include "ipc/protocol.hpp"
#include "util/fd.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace omenkbd {

struct IpcError {
    enum class Kind { AlreadyRunning, NotRunning, Io, Protocol };
    Kind kind;
    std::string detail;
    [[nodiscard]] std::string message() const;
};

/// `$XDG_RUNTIME_DIR/omen-kbd.sock` (falls back to `/tmp/omen-kbd-<uid>.sock`).
std::filesystem::path socket_path();

/// Binds and listens. A live daemon on `path` ⇒ AlreadyRunning; a stale socket file left by a
/// crashed daemon is replaced. The socket is user-only (0600), non-blocking and close-on-exec.
std::expected<UniqueFd, IpcError> bind_socket(const std::filesystem::path &path);

/// Accumulates bytes and hands out complete `\n`-terminated lines (without the newline).
class LineBuffer {
  public:
    void append(std::string_view data) { buf_.append(data); }
    std::optional<std::string> next_line();
    [[nodiscard]] std::size_t pending() const { return buf_.size(); }

  private:
    std::string buf_;
};

/// Sends one request and waits (up to 5 s) for its response.
std::expected<Response, IpcError> request(const std::filesystem::path &path, const Request &r);

/// A status stream from `subscribe`. Owns the connection.
class Subscription {
  public:
    explicit Subscription(UniqueFd fd) : fd_(std::move(fd)) {}
    /// The next status pushed by the daemon; nullopt when the daemon closed the stream.
    std::expected<std::optional<Status>, IpcError> next_status();

  private:
    UniqueFd fd_;
    LineBuffer lines_;
};

/// Sends `subscribe` and returns the stream of statuses that follows.
std::expected<Subscription, IpcError> subscribe(const std::filesystem::path &path);

} // namespace omenkbd
