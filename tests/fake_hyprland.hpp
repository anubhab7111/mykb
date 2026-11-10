#pragma once
// Test doubles for the Hyprland sockets on a real loop: a loop that runs on its own thread, and a
// fake request socket (`.socket.sock`, the one `hyprctl` talks to) that replies, stays silent or
// floods, and counts what the code under test did to it.

#include "daemon/clock.hpp"
#include "helpers.hpp"

#include "daemon/event_loop.hpp"
#include "ipc/socket.hpp"
#include "util/fd.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace omenkbd::test {

/// Runs a loop on its own thread until destroyed.
class LoopThread {
  public:
    explicit LoopThread(EventLoop &loop) : loop_(loop), t_([&loop] { loop.run(); }) {}
    ~LoopThread() {
        loop_.stop();
        t_.join();
    }
    LoopThread(const LoopThread &) = delete;
    LoopThread &operator=(const LoopThread &) = delete;

  private:
    EventLoop &loop_;
    std::thread t_;
};

/// Listens on `path` and serves Hyprland's request protocol the way Hyprland does: read the
/// command, write the reply, close. What it does is set between connections: reply (the default,
/// `{}`), reply in two pieces, or accept and never answer (the connection is kept open until the
/// client closes it). Counts every connection and every one the client closed. Thread-safe.
class FakeRequestServer {
  public:
    explicit FakeRequestServer(const std::filesystem::path &path) {
        auto listener = bind_socket(path);
        if (!listener) {
            throw std::runtime_error(listener.error().message());
        }
        listener_ = std::move(*listener);
        thread_ = std::thread([this] { run(); });
    }
    ~FakeRequestServer() {
        stop_ = true;
        thread_.join();
    }
    FakeRequestServer(const FakeRequestServer &) = delete;
    FakeRequestServer &operator=(const FakeRequestServer &) = delete;

    /// The next connections get `reply`, then EOF. With a `gap`, it comes in two halves with `gap`
    /// between them.
    void reply_with(std::string reply,
                    std::chrono::milliseconds gap = std::chrono::milliseconds{0}) {
        const std::lock_guard lock(mu_);
        reply_ = std::move(reply);
        gap_ = gap;
        silent_ = false;
    }
    /// The next connections are accepted, read, and left open without a reply.
    void stay_silent() {
        const std::lock_guard lock(mu_);
        silent_ = true;
    }
    /// Answers every connection that is being kept open (see `stay_silent`) with `reply` and closes
    /// it: a reply that comes after the client has given up, or never gave up.
    void answer_held(std::string reply) {
        const std::lock_guard lock(mu_);
        late_reply_ = std::move(reply);
    }

    [[nodiscard]] int connections() const {
        const std::lock_guard lock(mu_);
        return connections_;
    }
    /// Connections that ended by the client closing them (before or after we replied).
    [[nodiscard]] int closed_by_client() const {
        const std::lock_guard lock(mu_);
        return closed_by_client_;
    }
    /// The command each connection sent, in order.
    [[nodiscard]] std::vector<std::string> requests() const {
        const std::lock_guard lock(mu_);
        return requests_;
    }
    /// Waits until `n` commands have been read. Until then the server has not yet decided what to
    /// do with the connection (it decides when it reads), so a test that changes the server's
    /// mode must wait for this, not for `connections()`.
    [[nodiscard]] bool
    wait_for_requests(std::size_t n,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) const {
        return wait_until([&] { return requests().size() >= n; }, timeout);
    }
    /// When the first connection was accepted; the client connected no later than that.
    [[nodiscard]] std::optional<TimePoint> first_accept() const {
        const std::lock_guard lock(mu_);
        return first_accept_;
    }

  private:
    struct Conn {
        UniqueFd fd;
        bool requested = false;
    };

    static void send_all(int fd, std::string_view data) {
        while (!data.empty()) {
            const ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
            if (n <= 0) {
                return; // the client went away, or stopped reading
            }
            data.remove_prefix(static_cast<std::size_t>(n));
        }
    }

    void reply(int fd, const std::string &text, std::chrono::milliseconds gap) {
        if (gap.count() <= 0) {
            send_all(fd, text);
            return;
        }
        const std::size_t half = text.size() / 2;
        send_all(fd, std::string_view(text).substr(0, half));
        std::this_thread::sleep_for(gap);
        send_all(fd, std::string_view(text).substr(half));
    }

    void accept_one(std::vector<Conn> &conns) {
        UniqueFd fd(::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC));
        if (!fd) {
            return;
        }
        const timeval limit{.tv_sec = 1, .tv_usec = 0}; // a stuck client must not stick the server
        ::setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
        {
            const std::lock_guard lock(mu_);
            ++connections_;
            if (!first_accept_) {
                first_accept_ = Clock::now();
            }
        }
        conns.push_back(Conn{std::move(fd), false});
    }

    /// Reads from `conns[i]`; erases it when it ended or was answered.
    void serve(std::vector<Conn> &conns, std::size_t i) {
        char buf[256];
        const ssize_t n = ::read(conns[i].fd.get(), buf, sizeof(buf));
        if (n <= 0) {
            {
                const std::lock_guard lock(mu_);
                ++closed_by_client_;
            }
            conns.erase(conns.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
        if (conns[i].requested) {
            return; // more bytes after the command: not part of the protocol, ignored
        }
        conns[i].requested = true;
        bool silent = false;
        std::string text;
        std::chrono::milliseconds gap{0};
        {
            const std::lock_guard lock(mu_);
            requests_.emplace_back(buf, static_cast<std::size_t>(n));
            silent = silent_;
            text = reply_;
            gap = gap_;
        }
        if (silent) {
            return;
        }
        reply(conns[i].fd.get(), text, gap);
        conns.erase(conns.begin() + static_cast<std::ptrdiff_t>(i));
    }

    void answer_held_connections(std::vector<Conn> &conns) {
        std::optional<std::string> late;
        {
            const std::lock_guard lock(mu_);
            late = std::exchange(late_reply_, std::nullopt);
        }
        if (!late) {
            return;
        }
        std::erase_if(conns, [&](const Conn &c) {
            if (c.requested) {
                send_all(c.fd.get(), *late);
            }
            return c.requested;
        });
    }

    void run() {
        std::vector<Conn> conns;
        while (!stop_) {
            std::vector<pollfd> fds{{listener_.get(), POLLIN, 0}};
            for (const Conn &c : conns) {
                fds.push_back({c.fd.get(), POLLIN, 0});
            }
            if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), 20) < 0) {
                continue;
            }
            for (std::size_t i = fds.size(); i-- > 1;) { // backwards: `serve` may erase
                if (fds[i].revents != 0) {
                    serve(conns, i - 1);
                }
            }
            if (fds[0].revents != 0) {
                accept_one(conns);
            }
            answer_held_connections(conns);
        }
    }

    UniqueFd listener_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    std::string reply_ = "{}";
    std::chrono::milliseconds gap_{0};
    bool silent_ = false;
    std::optional<std::string> late_reply_;
    int connections_ = 0;
    int closed_by_client_ = 0;
    std::vector<std::string> requests_;
    std::optional<TimePoint> first_accept_;
    std::thread thread_; // last: it runs `run`, which uses everything above
};

} // namespace omenkbd::test
