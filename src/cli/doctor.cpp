#include "cli/doctor.hpp"

#include "backend/sysfs.hpp"
#include "core/palette.hpp"
#include "ipc/socket.hpp"
#include "util/strings.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>

namespace omenkbd {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kHpOmenKbd = "devices/platform/hp-omen-kbd";
constexpr std::array<std::string_view, 4> kLedZones{"right", "center", "left", "wasd"};
constexpr std::uintmax_t kMaxHookFile = std::uintmax_t{1} << 20U;
constexpr int kMaxHookDepth = 4;
constexpr std::size_t kMaxHookFiles = 2000;

/// The whole file, or nullopt if it cannot be read (files in /proc vanish mid-scan).
std::optional<std::string> read_text(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    std::string s{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (f.bad()) {
        return std::nullopt;
    }
    return s;
}

/// Entries of `dir` (non-recursive); empty if it is missing or unreadable. Never throws.
std::vector<fs::directory_entry> list_dir(const fs::path &dir) {
    std::vector<fs::directory_entry> out;
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        out.push_back(*it);
    }
    return out;
}

bool is_dir(const fs::path &p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

/// The value of a `key=value` token in sysfs `info`, e.g. `readonly=1`.
std::optional<std::string> info_field(std::string_view info, std::string_view key) {
    std::istringstream in{std::string(info)};
    std::string token;
    while (in >> token) {
        if (token.size() > key.size() && token.starts_with(key) && token[key.size()] == '=') {
            return token.substr(key.size() + 1);
        }
    }
    return std::nullopt;
}

void check_backend(const DoctorEnv &env, std::vector<Check> &out) {
    const bool hp = sysfs::hp_omen_kbd_present(env.sys_root);
    const bool leds = sysfs::mainline_led_present(env.sys_root);
    std::vector<fs::path> attrs;
    if (hp && leds) {
        out.push_back({Level::Fail,
                       "both hp-omen-kbd and mainline LEDs present; remove the "
                       "hp-omen-kbd DKMS module (two drivers would write the keyboard)"});
        return;
    }
    if (hp) {
        const auto dir = env.sys_root / kHpOmenKbd;
        out.push_back({Level::Ok, "backend hp-omen-kbd (" + dir.string() + ")"});
        const auto info = sysfs::read_attr(dir / "info");
        if (!info) {
            out.push_back({Level::Warn, "cannot read " + (dir / "info").string() + ": " +
                                            info.error().message()});
        } else if (const auto ro = info_field(*info, "readonly"); !ro) {
            out.push_back({Level::Warn, "hp-omen-kbd info has no readonly= field"});
        } else if (*ro == "1") {
            out.push_back({Level::Warn, "hp-omen-kbd is loaded with readonly=1: color writes are "
                                        "refused until hardware validation is signed off"});
        } else {
            out.push_back({Level::Ok, "hp-omen-kbd accepts writes (readonly=" + *ro + ")"});
        }
        attrs = {dir / "zones", dir / "backlight"};
    } else if (leds) {
        out.push_back({Level::Ok, "backend mainline-led (kernel hp::kbd_zoned_backlight-* LEDs)"});
        for (const auto zone : kLedZones) {
            const auto dir =
                env.sys_root / "class/leds" / std::format("hp::kbd_zoned_backlight-{}", zone);
            attrs.push_back(dir / "multi_intensity");
            attrs.push_back(dir / "brightness");
        }
    } else {
        out.push_back({Level::Fail, "no keyboard backend (is hp_omen_kbd loaded?)"});
        return;
    }

    std::string denied;
    for (const auto &a : attrs) {
        if (::access(a.c_str(), W_OK) != 0) {
            denied += (denied.empty() ? "" : ", ") + a.string();
        }
    }
    if (denied.empty()) {
        out.push_back({Level::Ok, "keyboard attributes writable"});
    } else {
        out.push_back({Level::Fail, "not writable: sudo usermod -aG omen-kbd $USER, then log in "
                                    "again (" +
                                        denied + ")"});
    }
}

void check_competing_writers(const DoctorEnv &env, std::vector<Check> &out) {
    const std::size_t before = out.size();

    if (const auto modules = read_text(env.proc_root / "modules")) {
        std::istringstream in(*modules);
        std::string line;
        while (std::getline(in, line)) {
            const std::string name = line.substr(0, line.find(' '));
            if (name == "hp_omen_extra" || name == "omen_rgb_keyboard") {
                out.push_back({Level::Fail, std::format("competing keyboard driver {} is loaded: "
                                                        "remove its DKMS package and reboot",
                                                        name)});
            }
        }
    }

    std::set<std::string> seen;
    for (const auto &e : list_dir(env.proc_root)) {
        const std::string pid = e.path().filename().string();
        if (pid.empty() || !std::ranges::all_of(pid, [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        const auto comm = read_text(e.path() / "comm");
        if (!comm) {
            continue; // the process exited
        }
        const std::string name(trim(*comm));
        if (!seen.insert(name).second) {
            continue;
        }
        if (name == "omen-space-daem") { // comm is truncated to 15 characters
            out.push_back({Level::Fail, std::format("omen-space daemon is running (pid {}): "
                                                    "systemctl disable --now omen-space-daemon",
                                                    pid)});
        } else if (name == "openrgb") {
            out.push_back(
                {Level::Warn, std::format("openrgb is running (pid {}): it may write the keyboard "
                                          "colors too",
                                          pid)});
        }
    }

    std::error_code ec;
    if (fs::exists(env.sys_root / "devices/platform/hp-wmi/rgb_zones", ec)) {
        out.push_back({Level::Warn, "hp-wmi has rgb_zones: a patched hp-wmi with its own keyboard "
                                    "color code is loaded"});
    }

    if (out.size() == before) {
        out.push_back({Level::Ok, "no competing keyboard writers"});
    }
}

/// True if a dkms.conf line builds a module named hp-wmi (or hp_wmi).
bool builds_hp_wmi(std::string_view conf) {
    std::istringstream in{std::string(conf)};
    std::string line;
    while (std::getline(in, line)) {
        const std::string l(trim(line));
        const auto eq = l.find('=');
        if (!l.starts_with("BUILT_MODULE_NAME[") || eq == std::string::npos) {
            continue;
        }
        std::string value(trim(std::string_view(l).substr(eq + 1)));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        if (value == "hp-wmi" || value == "hp_wmi") {
            return true;
        }
    }
    return false;
}

void check_dkms(const DoctorEnv &env, std::vector<Check> &out) {
    const std::size_t before = out.size();
    for (const auto &pkg : list_dir(env.dkms_root)) {
        if (!is_dir(pkg.path())) {
            continue;
        }
        for (const auto &ver : list_dir(pkg.path())) {
            const auto conf = read_text(ver.path() / "source/dkms.conf");
            if (!conf || !builds_hp_wmi(*conf)) {
                continue;
            }
            const auto id = std::format("{}/{}", pkg.path().filename().string(),
                                        ver.path().filename().string());
            out.push_back(
                {Level::Warn, std::format("DKMS package {} builds its own hp-wmi (replaces the "
                                          "kernel's): sudo dkms remove {} --all",
                                          id, id)});
        }
    }
    if (out.size() == before) {
        out.push_back({Level::Ok, "no DKMS package replaces hp-wmi"});
    }
}

void check_daemon(const DoctorEnv &env, std::vector<Check> &out) {
    const auto resp = request(env.socket, req::Status{});
    if (!resp) {
        if (resp.error().kind == IpcError::Kind::NotRunning) {
            out.push_back(
                {Level::Warn, "daemon not running (start it: systemctl --user start omen-kbd)"});
        } else {
            out.push_back({Level::Warn, "daemon: " + resp.error().message()});
        }
        return;
    }
    if (!resp->ok || !resp->status) {
        out.push_back({Level::Warn, "daemon: " + resp->error.value_or("no status")});
        return;
    }
    const Status &s = *resp->status;
    out.push_back({Level::Ok, std::format("daemon running (backend {}, profile {}, {} writes)",
                                          s.backend, s.profile, s.lifetime_writes)});
    if (s.budget_exhausted) {
        out.push_back({Level::Warn, "hourly write budget exhausted; effects paused until it "
                                    "refills"});
    }
    if (s.last_error) {
        out.push_back({Level::Warn, "last keyboard write failed: " + *s.last_error});
    }
}

void check_hooks(const DoctorEnv &env, std::vector<Check> &out) {
    std::optional<fs::path> hook;
    std::optional<fs::path> lock;
    std::error_code ec;
    fs::recursive_directory_iterator it(env.hypr_dir,
                                        fs::directory_options::follow_directory_symlink |
                                            fs::directory_options::skip_permission_denied,
                                        ec);
    std::size_t files = 0;
    for (; !ec && it != fs::recursive_directory_iterator() && files < kMaxHookFiles;
         it.increment(ec)) {
        if (it.depth() >= kMaxHookDepth) {
            it.disable_recursion_pending();
        }
        std::error_code fe;
        if (!it->is_regular_file(fe) || it->file_size(fe) > kMaxHookFile || fe) {
            continue;
        }
        ++files;
        const auto text = read_text(it->path());
        if (!text) {
            continue;
        }
        if (!hook && text->find("omen-kbd hint") != std::string::npos) {
            hook = it->path();
        }
        if (!lock && text->find("omen-kbd flag locked") != std::string::npos) {
            lock = it->path();
        }
    }
    if (hook) {
        out.push_back({Level::Ok, "mode-colors hook installed (" + hook->string() + ")"});
    } else {
        out.push_back({Level::Warn, "mode-colors hook not installed (omen-kbd gen-hyprland)"});
    }
    if (lock) {
        out.push_back({Level::Ok, "lock gate wired (" + lock->string() + ")"});
    } else {
        out.push_back({Level::Warn, "lock gate not wired: add on_lock_cmd = omen-kbd flag locked "
                                    "on and on_unlock_cmd = omen-kbd flag locked off to "
                                    "hypridle.conf's general section"});
    }
}

void check_theme(const DoctorEnv &env, std::vector<Check> &out) {
    const auto palette = Palette::load(env.theme_dir);
    if (!palette) {
        out.push_back({Level::Warn, "no theme palette: " + palette.error() +
                                        " (theme: colors fall back to their defaults)"});
    } else if (palette->empty()) {
        out.push_back({Level::Warn, "theme palette in " + env.theme_dir.string() + " is empty"});
    } else {
        out.push_back({Level::Ok, std::format("theme palette: {} colors ({})", palette->size(),
                                              env.theme_dir.string())});
    }
}

} // namespace

DoctorEnv DoctorEnv::real(const Config &cfg) {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    const fs::path config_home = (xdg != nullptr && *xdg != '\0') ? fs::path(xdg)
                                 : home != nullptr                ? fs::path(home) / ".config"
                                                                  : fs::path(".config");
    return {.sys_root = "/sys",
            .proc_root = "/proc",
            .dkms_root = "/var/lib/dkms",
            .hypr_dir = config_home / "hypr",
            .theme_dir = cfg.theme.dir,
            .socket = socket_path()};
}

std::vector<Check> run_checks(const DoctorEnv &env) {
    std::vector<Check> out;
    check_backend(env, out);
    check_competing_writers(env, out);
    check_dkms(env, out);
    check_daemon(env, out);
    check_hooks(env, out);
    check_theme(env, out);
    return out;
}

std::string_view level_name(Level level) {
    switch (level) {
    case Level::Ok:
        return "OK";
    case Level::Warn:
        return "WARN";
    case Level::Fail:
        return "FAIL";
    }
    return "?";
}

} // namespace omenkbd
