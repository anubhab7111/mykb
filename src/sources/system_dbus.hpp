#pragma once
// System facts from the system D-Bus, on the daemon's own loop (no D-Bus thread):
//   net.hadess.PowerProfiles ActiveProfile (tlp-pd here)      ⇒ Fact PowerProfile
//   UPower OnBattery, DisplayDevice Percentage                 ⇒ Fact OnBattery, BatteryPct
//   logind PrepareForSleep, LidClosed                          ⇒ Sleep, Fact LidClosed
//   logind session (User.Display): LockedHint, Unlock signal   ⇒ Fact Locked
// The session's Lock signal is deliberately not used: it is a request to lock, and with hyprlock
// nothing ever reverses it (hyprlock sets no LockedHint and unlocking emits no Unlock), so the
// keyboard would stay off after every unlock. The real lock state comes from hypridle's
// on_lock_cmd/on_unlock_cmd (`omen-kbd flag locked on|off`).
//
// A missing service logs one warning and that fact stays unknown; nothing here throws.

#include "daemon/event_loop.hpp"
#include "daemon/events.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sdbus {
class IConnection;
class IProxy;
class Variant;
class ObjectPath;
} // namespace sdbus

namespace omenkbd {

class SystemDbusSource {
  public:
    using Emit = std::function<void(const Event &)>;

    /// Must be destroyed before `loop` (its before-poll hook becomes a no-op once it is gone).
    SystemDbusSource(EventLoop &loop, Emit emit);
    ~SystemDbusSource();
    SystemDbusSource(const SystemDbusSource &) = delete;
    SystemDbusSource &operator=(const SystemDbusSource &) = delete;

  private:
    using OnValue = std::function<void(const sdbus::Variant &)>;

    /// Initial value of each property, then its PropertiesChanged updates (refetching values
    /// that are only invalidated).
    void follow(sdbus::IProxy &proxy, const std::string &iface,
                std::map<std::string, OnValue> props, const std::string &what);
    void fetch(sdbus::IProxy &proxy, const std::string &iface, const std::string &prop,
               OnValue on_value, const std::string &what);
    void find_session(sdbus::IProxy &login);
    void attach_session(const std::string &path);
    void warn_once(const std::string &what, const std::string &msg);
    void sync_poll();
    void process();
    void shutdown(const std::string &why);

    EventLoop &loop_;
    Emit emit_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::unique_ptr<sdbus::IConnection> bus_;
    std::vector<std::unique_ptr<sdbus::IProxy>> proxies_; // destroyed before bus_
    int fd_ = -1;
    std::uint32_t fd_events_ = 0;
    int event_fd_ = -1;
    std::optional<EventLoop::TimerId> timer_;
    std::optional<TimePoint> timer_at_;
    std::set<std::string> warned_;
};

} // namespace omenkbd
