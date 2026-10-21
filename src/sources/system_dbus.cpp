#include "sources/system_dbus.hpp"

#include <sdbus-c++/sdbus-c++.h>

#include <cstdint>
#include <exception>
#include <poll.h>
#include <print>
#include <sys/epoll.h>
#include <unistd.h>

namespace omenkbd {

namespace {

constexpr const char *kProperties = "org.freedesktop.DBus.Properties";
constexpr const char *kLogin = "org.freedesktop.login1";
constexpr const char *kLoginManager = "org.freedesktop.login1.Manager";
constexpr const char *kLoginSession = "org.freedesktop.login1.Session";
constexpr const char *kLoginUser = "org.freedesktop.login1.User";
constexpr const char *kUPower = "org.freedesktop.UPower";
constexpr const char *kUPowerDevice = "org.freedesktop.UPower.Device";
constexpr const char *kProfiles = "net.hadess.PowerProfiles";

/// Runs a handler; nothing may unwind through sd-bus's C frames.
template <typename F> void guarded(F &&f) noexcept {
    try {
        f();
    } catch (const std::exception &e) {
        std::println(stderr, "omen-kbd: warning: D-Bus handler: {}", e.what());
    } catch (...) {
        std::println(stderr, "omen-kbd: warning: D-Bus handler failed");
    }
}

template <typename T> std::optional<T> value_of(const sdbus::Variant &v) {
    if (!v.containsValueOfType<T>()) {
        return std::nullopt;
    }
    return v.get<T>();
}

} // namespace

SystemDbusSource::SystemDbusSource(EventLoop &loop, Emit emit)
    : loop_(loop), emit_(std::move(emit)) {
    try {
        bus_ = sdbus::createSystemBusConnection();
    } catch (const sdbus::Error &e) {
        warn_once("system D-Bus", e.getMessage() + " (power profile, battery, lid, sleep and "
                                                   "lock facts are unavailable)");
        return;
    }
    loop_.on_before_poll([alive = std::weak_ptr<bool>(alive_), this] {
        if (alive.lock()) {
            sync_poll();
        }
    });
    guarded([&] {
        // Proxies on a shared connection run no event-loop thread of their own.
        auto proxy = [&](const char *service, const char *path) -> sdbus::IProxy & {
            proxies_.push_back(
                sdbus::createProxy(*bus_, sdbus::ServiceName{service}, sdbus::ObjectPath{path}));
            return *proxies_.back();
        };
        auto fact = [this](auto f) { emit_(Event{FactUpdate{f}}); };

        follow(proxy(kProfiles, "/net/hadess/PowerProfiles"), kProfiles,
               {{"ActiveProfile",
                 [fact](const sdbus::Variant &v) {
                     if (auto s = value_of<std::string>(v)) {
                         fact(fact::PowerProfile{*s});
                     }
                 }}},
               "power profiles (net.hadess.PowerProfiles)");

        follow(proxy(kUPower, "/org/freedesktop/UPower"), kUPower,
               {{"OnBattery",
                 [fact](const sdbus::Variant &v) {
                     if (auto b = value_of<bool>(v)) {
                         fact(fact::OnBattery{*b});
                     }
                 }}},
               "UPower");
        follow(proxy(kUPower, "/org/freedesktop/UPower/devices/DisplayDevice"), kUPowerDevice,
               {{"Percentage",
                 [fact](const sdbus::Variant &v) {
                     if (auto d = value_of<double>(v)) {
                         fact(fact::BatteryPct{*d});
                     }
                 }}},
               "UPower");

        sdbus::IProxy &login = proxy(kLogin, "/org/freedesktop/login1");
        follow(login, kLoginManager,
               {{"LidClosed",
                 [fact](const sdbus::Variant &v) {
                     if (auto b = value_of<bool>(v)) {
                         fact(fact::LidClosed{*b});
                     }
                 }}},
               "logind");
        login.uponSignal("PrepareForSleep").onInterface(kLoginManager).call([this](bool entering) {
            guarded([&] { emit_(Event{event::Sleep{entering}}); });
        });
        find_session(login);
    });
}

SystemDbusSource::~SystemDbusSource() {
    alive_.reset();
    if (timer_) {
        loop_.cancel(*timer_);
    }
    if (fd_ >= 0) {
        loop_.unwatch(fd_);
    }
    if (event_fd_ >= 0) {
        loop_.unwatch(event_fd_);
    }
    proxies_.clear();
    bus_.reset();
}

void SystemDbusSource::follow(sdbus::IProxy &proxy, const std::string &iface,
                              std::map<std::string, OnValue> props, const std::string &what) {
    for (const auto &[name, on_value] : props) {
        fetch(proxy, iface, name, on_value, what);
    }
    proxy.uponSignal("PropertiesChanged")
        .onInterface(kProperties)
        .call([this, p = &proxy, iface, props,
               what](const std::string &changed_iface,
                     const std::map<std::string, sdbus::Variant> &changed,
                     const std::vector<std::string> &invalidated) {
            guarded([&] {
                if (changed_iface != iface) {
                    return;
                }
                for (const auto &[name, value] : changed) {
                    if (const auto it = props.find(name); it != props.end()) {
                        it->second(value);
                    }
                }
                for (const auto &name : invalidated) {
                    if (const auto it = props.find(name); it != props.end()) {
                        fetch(*p, iface, name, it->second, what);
                    }
                }
            });
        });
}

void SystemDbusSource::fetch(sdbus::IProxy &proxy, const std::string &iface,
                             const std::string &prop, OnValue on_value, const std::string &what) {
    proxy.getPropertyAsync(prop).onInterface(iface).uponReplyInvoke(
        [this, on_value = std::move(on_value), what](std::optional<sdbus::Error> err,
                                                     sdbus::Variant value) {
            guarded([&] {
                if (err) {
                    warn_once(what, err->getMessage());
                    return;
                }
                on_value(value);
            });
        });
}

void SystemDbusSource::find_session(sdbus::IProxy &login) {
    // The daemon runs under user@.service, so GetSessionByPID would fail: ask for the user's
    // display session instead.
    login.callMethodAsync("GetUser")
        .onInterface(kLoginManager)
        .withArguments(static_cast<std::uint32_t>(::getuid()))
        .uponReplyInvoke([this](std::optional<sdbus::Error> err, sdbus::ObjectPath user_path) {
            guarded([&] {
                if (err) {
                    warn_once("logind session", err->getMessage());
                    return;
                }
                proxies_.push_back(
                    sdbus::createProxy(*bus_, sdbus::ServiceName{kLogin}, std::move(user_path)));
                fetch(
                    *proxies_.back(), kLoginUser, "Display",
                    [this](const sdbus::Variant &v) {
                        using Display = sdbus::Struct<std::string, sdbus::ObjectPath>;
                        if (auto d = value_of<Display>(v)) {
                            attach_session(std::get<1>(*d));
                        }
                    },
                    "logind session");
            });
        });
}

void SystemDbusSource::attach_session(const std::string &path) {
    if (path.empty() || path == "/") {
        warn_once("logind session", "no graphical session for this user; lock state comes only "
                                    "from `omen-kbd flag locked`");
        return;
    }
    proxies_.push_back(
        sdbus::createProxy(*bus_, sdbus::ServiceName{kLogin}, sdbus::ObjectPath{path}));
    sdbus::IProxy &session = *proxies_.back();
    follow(session, kLoginSession,
           {{"LockedHint",
             [this](const sdbus::Variant &v) {
                 if (auto b = value_of<bool>(v)) {
                     emit_(Event{FactUpdate{fact::Locked{*b}}});
                 }
             }}},
           "logind session");
    session.uponSignal("Unlock").onInterface(kLoginSession).call([this] {
        guarded([&] { emit_(Event{FactUpdate{fact::Locked{false}}}); });
    });
}

void SystemDbusSource::warn_once(const std::string &what, const std::string &msg) {
    if (warned_.insert(what).second) {
        std::println(stderr, "omen-kbd: warning: {}: {}", what, msg);
    }
}

void SystemDbusSource::sync_poll() {
    if (!bus_) {
        return;
    }
    sdbus::IConnection::PollData pd{};
    try {
        pd = bus_->getEventLoopPollData();
    } catch (const sdbus::Error &e) {
        shutdown(e.getMessage());
        return;
    }
    std::uint32_t events = 0;
    if ((pd.events & POLLIN) != 0) {
        events |= EPOLLIN;
    }
    if ((pd.events & POLLOUT) != 0) {
        events |= EPOLLOUT;
    }
    const auto on_ready = [this](std::uint32_t ev) {
        if ((ev & (EPOLLHUP | EPOLLERR)) != 0) {
            shutdown("connection closed");
            return;
        }
        process();
    };
    if (pd.fd != fd_) {
        if (fd_ >= 0) {
            loop_.unwatch(fd_);
        }
        fd_ = pd.fd;
        loop_.watch(fd_, events, on_ready);
        fd_events_ = events;
    } else if (events != fd_events_) {
        loop_.modify(fd_, events);
        fd_events_ = events;
    }
    if (pd.eventFd != event_fd_) {
        if (event_fd_ >= 0) {
            loop_.unwatch(event_fd_);
        }
        event_fd_ = pd.eventFd;
        if (event_fd_ >= 0) {
            loop_.watch(event_fd_, EPOLLIN, [this](std::uint32_t) { process(); });
        }
    }
    // The timeout is absolute CLOCK_MONOTONIC, which is steady_clock's clock on Linux.
    std::optional<TimePoint> at;
    const auto limit =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::duration::max() / 2);
    if (pd.timeout.count() >= 0 && pd.timeout < limit) {
        at = TimePoint(std::chrono::duration_cast<Clock::duration>(pd.timeout));
    }
    if (at != timer_at_) {
        if (timer_) {
            loop_.cancel(*timer_);
            timer_.reset();
        }
        timer_at_ = at;
        if (at) {
            timer_ = loop_.at(*at, [this] {
                timer_.reset();
                timer_at_.reset();
                process();
            });
        }
    }
}

void SystemDbusSource::process() {
    if (!bus_) {
        return;
    }
    try {
        while (bus_->processPendingEvent()) {
        }
    } catch (const sdbus::Error &e) {
        shutdown(e.getMessage());
    }
}

void SystemDbusSource::shutdown(const std::string &why) {
    warn_once("system D-Bus", why + " (system facts stop updating until omen-kbd restarts)");
    if (timer_) {
        loop_.cancel(*timer_);
        timer_.reset();
        timer_at_.reset();
    }
    if (fd_ >= 0) {
        loop_.unwatch(fd_);
        fd_ = -1;
    }
    if (event_fd_ >= 0) {
        loop_.unwatch(event_fd_);
        event_fd_ = -1;
    }
    proxies_.clear();
    bus_.reset();
}

} // namespace omenkbd
