#pragma once
// A single-threaded epoll loop: fd watches, one-shot timers, a before-poll hook, and a thread-safe
// post queue woken through one eventfd. Everything except stop() and post() runs on (and must be
// called from) the loop thread.

#include "daemon/clock.hpp"
#include "util/fd.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace omenkbd {

class EventLoop {
  public:
    using Callback = std::function<void(std::uint32_t events)>;
    using TimerId = std::uint64_t;

    EventLoop();
    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    /// False if the epoll fd or the eventfd could not be created (see error()); run() then
    /// returns at once.
    [[nodiscard]] bool valid() const { return epoll_ && wake_; }
    [[nodiscard]] const std::string &error() const { return error_; }

    /// Level-triggered watch. A callback may unwatch its own fd (or any other).
    void watch(int fd, std::uint32_t events, Callback cb);
    void modify(int fd, std::uint32_t events);
    void unwatch(int fd);

    /// Runs `fn` once at `when` (or on the next iteration if `when` has passed).
    TimerId at(TimePoint when, std::function<void()> fn);
    void cancel(TimerId id);

    /// Runs `fn` at the start of every iteration, before the poll timeout is computed.
    void on_before_poll(std::function<void()> fn);

    /// Runs until stop().
    void run();
    /// Thread-safe. The loop finishes its current iteration and run() returns.
    void stop();
    /// Thread-safe: queues `fn` to run on the loop thread.
    void post(std::function<void()> fn);

  private:
    struct Watch {
        std::uint64_t token;
        std::shared_ptr<Callback> cb;
    };

    [[nodiscard]] int timeout_ms() const;
    void wake();
    void drain_posted();
    void run_due_timers();

    UniqueFd epoll_;
    UniqueFd wake_;
    std::string error_;
    /// fd → registration. Events carry the token, so a stale event for a closed (and reused) fd
    /// is never delivered to the new owner.
    std::unordered_map<int, Watch> watches_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Callback>> by_token_;
    std::uint64_t next_token_ = 1;
    std::map<std::pair<TimePoint, TimerId>, std::function<void()>> timers_;
    std::unordered_map<TimerId, TimePoint> timer_at_;
    TimerId next_timer_ = 1;
    std::vector<std::function<void()>> before_poll_;
    std::mutex mu_;
    std::vector<std::function<void()>> posted_;
    std::atomic<bool> stop_{false};
};

} // namespace omenkbd
