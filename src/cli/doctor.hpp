#pragma once
// `omen-kbd doctor`: read-only checks of the keyboard driver, permissions, competing writers, the
// daemon and the desktop hooks. It never writes anything and never opens a backend.

#include "config.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace omenkbd {

enum class Level { Ok, Warn, Fail };

struct Check {
    Level level;
    std::string msg;
};

/// Where the checks look; tests point every path into a temporary directory.
struct DoctorEnv {
    std::filesystem::path sys_root;
    std::filesystem::path proc_root;
    std::filesystem::path dkms_root;
    std::filesystem::path hypr_dir;
    std::filesystem::path theme_dir;
    std::filesystem::path socket;

    /// /sys, /proc, /var/lib/dkms, ~/.config/hypr, the config's theme dir and the daemon socket.
    static DoctorEnv real(const Config &cfg);
};

/// Backend, permissions, competing writers, hp-wmi DKMS packages, daemon, Hyprland hooks, theme.
std::vector<Check> run_checks(const DoctorEnv &env);

/// "OK", "WARN" or "FAIL".
std::string_view level_name(Level level);

} // namespace omenkbd
