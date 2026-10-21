#include "daemon/event_loop.hpp"

#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace omenkbd {

namespace {

constexpr std::uint64_t kWakeToken = 0;

void log_errno(const char *what) {
    std::fprintf(stderr, "omen-kbd: %s: %s\n", what, std::strerror(errno));
}

} // namespace

EventLoop::EventLoop()
    : epoll_(::epoll_create1(EPOLL_CLOEXEC)), wake_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (!epoll_ || !wake_) {
        error_ = std::string("cannot create the event loop: ") + std::strerror(errno);
        return;
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = kWakeToken;
    if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, wake_.get(), &ev) != 0) {
        error_ = std::string("cannot watch the wake fd: ") + std::strerror(errno);
        epoll_.reset();
    }
}

void EventLoop::watch(int fd, std::uint32_t events, Callback cb) {
    const std::uint64_t token = next_token_++;
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = token;
    if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) {
        log_errno("epoll_ctl(ADD)");
        return;
    }
    auto shared = std::make_shared<Callback>(std::move(cb));
    by_token_[token] = shared;
    watches_[fd] = Watch{token, std::move(shared)};
}

void EventLoop::modify(int fd, std::uint32_t events) {
    const auto it = watches_.find(fd);
    if (it == watches_.end()) {
        return;
    }
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = it->second.token;
    if (::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, fd, &ev) != 0) {
        log_errno("epoll_ctl(MOD)");
    }
}

void EventLoop::unwatch(int fd) {
    const auto it = watches_.find(fd);
    if (it == watches_.end()) {
        return;
    }
    ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, fd, nullptr);
    by_token_.erase(it->second.token);
    watches_.erase(it);
}

EventLoop::TimerId EventLoop::at(TimePoint when, std::function<void()> fn) {
    const TimerId id = next_timer_++;
    timers_.emplace(std::pair{when, id}, std::move(fn));
    timer_at_.emplace(id, when);
    return id;
}

void EventLoop::cancel(TimerId id) {
    const auto it = timer_at_.find(id);
    if (it == timer_at_.end()) {
        return;
    }
    timers_.erase(std::pair{it->second, id});
    timer_at_.erase(it);
}

void EventLoop::on_before_poll(std::function<void()> fn) { before_poll_.push_back(std::move(fn)); }

void EventLoop::stop() {
    stop_ = true;
    wake();
}

void EventLoop::post(std::function<void()> fn) {
    {
        const std::lock_guard lock(mu_);
        posted_.push_back(std::move(fn));
    }
    wake();
}

void EventLoop::wake() {
    const std::uint64_t one = 1;
    // EAGAIN only if the counter is about to overflow, i.e. a wake-up is already pending.
    [[maybe_unused]] const auto n = ::write(wake_.get(), &one, sizeof(one));
}

int EventLoop::timeout_ms() const {
    if (timers_.empty()) {
        return -1;
    }
    const auto left = timers_.begin()->first.first - Clock::now();
    if (left <= Clock::duration::zero()) {
        return 0;
    }
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(left).count();
    return ms > INT_MAX ? INT_MAX : static_cast<int>(ms);
}

void EventLoop::drain_posted() {
    std::vector<std::function<void()>> batch;
    {
        const std::lock_guard lock(mu_);
        batch.swap(posted_);
    }
    for (auto &fn : batch) {
        fn();
    }
}

void EventLoop::run_due_timers() {
    const TimePoint now = Clock::now();
    while (!timers_.empty() && timers_.begin()->first.first <= now) {
        auto node = timers_.extract(timers_.begin());
        timer_at_.erase(node.key().second);
        node.mapped()();
    }
}

void EventLoop::run() {
    if (!valid()) {
        return;
    }
    std::array<epoll_event, 32> events{};
    while (!stop_) {
        for (auto &fn : before_poll_) {
            fn();
        }
        const int n = ::epoll_wait(epoll_.get(), events.data(), static_cast<int>(events.size()),
                                   timeout_ms());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_errno("epoll_wait");
            return;
        }
        for (int i = 0; i < n && !stop_; ++i) {
            const epoll_event &ev = events[static_cast<std::size_t>(i)];
            if (ev.data.u64 == kWakeToken) {
                std::uint64_t count = 0;
                [[maybe_unused]] const auto r = ::read(wake_.get(), &count, sizeof(count));
                continue;
            }
            const auto it = by_token_.find(ev.data.u64);
            if (it == by_token_.end()) {
                continue; // unwatched by an earlier callback in this batch
            }
            const std::shared_ptr<Callback> cb = it->second; // survives an unwatch inside the call
            (*cb)(ev.events);
        }
        drain_posted();
        run_due_timers();
    }
}

} // namespace omenkbd
