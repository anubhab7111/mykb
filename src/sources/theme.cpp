#include "sources/theme.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace omenkbd {

namespace {

using namespace std::chrono_literals;

constexpr auto kDebounce = 200ms;
constexpr int kMaxHops = 40;
constexpr std::uint32_t kMask = IN_CREATE | IN_MOVED_TO | IN_DELETE | IN_CLOSE_WRITE | IN_ATTRIB;

} // namespace

std::vector<std::filesystem::path> link_chain(const std::filesystem::path &p) {
    std::vector<std::filesystem::path> chain{p};
    std::filesystem::path cur = p;
    for (int hop = 0; hop < kMaxHops; ++hop) {
        std::error_code ec;
        if (!std::filesystem::is_symlink(cur, ec)) {
            break;
        }
        std::filesystem::path target = std::filesystem::read_symlink(cur, ec);
        if (ec) {
            break;
        }
        if (target.is_relative()) {
            target = cur.parent_path() / target;
        }
        cur = target.lexically_normal();
        chain.push_back(cur);
    }
    return chain;
}

ThemeWatcher::ThemeWatcher(EventLoop &loop, std::filesystem::path dir, Emit emit)
    : loop_(loop), dir_(std::move(dir)), emit_(std::move(emit)),
      inotify_(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {
    if (!inotify_) {
        return; // no theme following; `reload` still works
    }
    loop_.watch(inotify_.get(), EPOLLIN, [this](std::uint32_t) { on_readable(); });
    rebuild();
}

ThemeWatcher::~ThemeWatcher() {
    if (debounce_) {
        loop_.cancel(*debounce_);
    }
    if (inotify_) {
        loop_.unwatch(inotify_.get());
    }
}

void ThemeWatcher::rebuild() {
    for (const auto &[wd, path] : watched_) {
        ::inotify_rm_watch(inotify_.get(), wd);
    }
    watched_.clear();
    chain_ = link_chain(dir_);
    for (const auto &p : chain_) {
        const auto parent = p.parent_path();
        const int wd = ::inotify_add_watch(inotify_.get(), parent.c_str(), kMask);
        if (wd >= 0) {
            watched_[wd] = parent; // two elements in one directory share a watch
        }
    }
}

void ThemeWatcher::on_readable() {
    alignas(inotify_event) char buf[4096];
    bool relevant = false;
    for (;;) {
        const ssize_t n = ::read(inotify_.get(), buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break; // EAGAIN: drained
        }
        for (ssize_t off = 0; off < n;) {
            inotify_event ev{};
            std::memcpy(&ev, buf + off, sizeof(ev));
            const char *name = buf + off + static_cast<ssize_t>(sizeof(ev));
            off += static_cast<ssize_t>(sizeof(ev) + ev.len);
            const auto dir = watched_.find(ev.wd);
            if (ev.len == 0 || dir == watched_.end()) {
                continue;
            }
            const auto path = dir->second / std::string(name, ::strnlen(name, ev.len));
            if (std::ranges::find(chain_, path) != chain_.end()) {
                relevant = true;
            }
        }
    }
    if (!relevant) {
        return;
    }
    if (debounce_) {
        loop_.cancel(*debounce_);
    }
    debounce_ = loop_.at(Clock::now() + kDebounce, [this] {
        debounce_.reset();
        rebuild();
        emit_(Event{event::ThemeChanged{}});
    });
}

} // namespace omenkbd
