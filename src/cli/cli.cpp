#include "cli/cli.hpp"

#include "cli/doctor.hpp"
#include "cli/format.hpp"
#include "cli/gen_hyprland.hpp"
#include "config.hpp"
#include "daemon/daemon.hpp"
#include "ipc/socket.hpp"
#include "state.hpp"

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <print>
#include <thread>

namespace omenkbd {

namespace {

using namespace std::chrono_literals;

constexpr int kExitError = 1;
constexpr int kExitNotRunning = 2;
constexpr int kExitUsage = 64; // EX_USAGE; 2 already means "daemon not running"

/// What the command line asked for. Positionals are shared: only one subcommand runs.
struct Args {
    enum class Mode { None, Request, Waybar, Daemon, Doctor, GenHyprland };
    Mode mode = Mode::None;
    std::optional<Request> request;
    bool json = false;

    std::vector<std::string> colors;
    std::string a;
    std::string b;
    int speed = 0;

    std::string backend = "auto";
    bool mock_render = false;
    bool no_sources = false;
    std::string sys_root = "/sys";
};

std::unique_ptr<CLI::App> make_app(Args &x) {
    auto app = std::make_unique<CLI::App>(
        "Keyboard color manager for the HP OMEN 16 (4-zone RGB backlight)", "omen-kbd");
    app->require_subcommand(1);
    app->set_version_flag("--version", OMEN_KBD_VERSION);

    const auto sends = [&x](CLI::App *sub, std::function<Request()> make) {
        sub->callback([&x, make = std::move(make)] {
            x.mode = Args::Mode::Request;
            x.request = make();
        });
    };
    const auto on_off = CLI::IsMember({"on", "off"});

    auto *status = app->add_subcommand("status", "Show the current state");
    status->add_flag("--json", x.json, "Print the status as JSON");
    sends(status, [] { return req::Status{}; });

    auto *set = app->add_subcommand(
        "set", "Set custom colors: one for every zone, or four (left middle right wasd)");
    set->add_option("colors", x.colors, "#rrggbb or theme:<name>")->required()->expected(1, 4);
    sends(set, [&x] {
        if (x.colors.size() != 1 && x.colors.size() != 4) {
            throw CLI::ValidationError("set", "takes 1 or 4 colors (left middle right wasd), got " +
                                                  std::to_string(x.colors.size()));
        }
        return req::Set{x.colors};
    });

    auto *profile = app->add_subcommand("profile", "Switch profile");
    profile->add_option("name", x.a, "A profile from the config, custom, next or prev")->required();
    sends(profile, [&x] { return req::Profile{x.a}; });

    auto *brightness = app->add_subcommand("brightness", "Set brightness");
    brightness->add_option("value", x.a, "0-100, +N or -N")->required();
    sends(brightness, [&x] { return req::Brightness{x.a}; });

    sends(app->add_subcommand("on", "Turn the backlight on"),
          [] { return req::Power{req::PowerAction::On}; });
    sends(app->add_subcommand("off", "Turn the backlight off (colors are kept)"),
          [] { return req::Power{req::PowerAction::Off}; });
    sends(app->add_subcommand("toggle", "Toggle the backlight"),
          [] { return req::Power{req::PowerAction::Toggle}; });

    auto *effect = app->add_subcommand("effect", "Run an effect (needs [effects] enabled = true)");
    effect->add_option("name", x.a, "static, breathe, wave, cycle or gradient")->required();
    effect->add_option("--speed", x.speed, "1-100")->check(CLI::Range(1, 100));
    sends(effect, [&x] {
        return req::Effect{x.a, x.speed > 0 ? std::optional(static_cast<std::uint8_t>(x.speed))
                                            : std::nullopt};
    });

    auto *hint = app->add_subcommand("hint", "Show a mode color (used by the Hyprland hook)");
    hint->add_option("mods", x.a, "Modifier set like super+shift, or clear")->required();
    sends(hint, [&x] { return req::Hint{x.a}; });

    auto *idle = app->add_subcommand("idle", "Idle gate (used by hypridle)");
    idle->add_option("state", x.a, "on or off")->required()->check(on_off);
    sends(idle, [&x] { return req::Idle{x.a == "on"}; });

    auto *flag = app->add_subcommand("flag", "Set a flag: gamemode, locked");
    flag->add_option("name", x.a, "gamemode or locked")->required();
    flag->add_option("state", x.b, "on or off")->required()->check(on_off);
    sends(flag, [&x] { return req::Flag{x.a, x.b == "on"}; });

    sends(app->add_subcommand("reload", "Re-read the config file"), [] { return req::Reload{}; });

    app->add_subcommand("waybar", "Stream status lines for a Waybar custom module")->callback([&x] {
        x.mode = Args::Mode::Waybar;
    });

    app->add_subcommand("doctor", "Check the driver, permissions, competing writers and hooks")
        ->callback([&x] { x.mode = Args::Mode::Doctor; });
    app->add_subcommand("gen-hyprland", "Print the Hyprland Lua module for mode colors")
        ->callback([&x] { x.mode = Args::Mode::GenHyprland; });

    auto *daemon = app->add_subcommand("daemon", "Run the daemon");
    daemon->add_option("--backend", x.backend, "auto, mock, hp-omen-kbd or mainline-led")
        ->check(CLI::IsMember({"auto", "mock", "hp-omen-kbd", "mainline-led"}));
    daemon->add_flag("--mock-render", x.mock_render, "Print mock frames as colored blocks");
    daemon->add_flag("--no-sources", x.no_sources,
                     "Ignore Hyprland, theme, hwmon and system D-Bus events");
    daemon->add_option("--sys-root", x.sys_root)->group(""); // tests and demos only
    daemon->callback([&x] { x.mode = Args::Mode::Daemon; });

    return app;
}

int run_request(const Request &r, bool json) {
    const auto resp = request(socket_path(), r);
    if (!resp) {
        if (resp.error().kind == IpcError::Kind::NotRunning) {
            std::println(
                stderr, "omen-kbd: daemon not running (start it: systemctl --user start omen-kbd)");
            return kExitNotRunning;
        }
        std::println(stderr, "omen-kbd: {}", resp.error().message());
        return kExitError;
    }
    if (!resp->ok) {
        std::println(stderr, "omen-kbd: {}", resp->error.value_or("request failed"));
        return kExitError;
    }
    if (std::holds_alternative<req::Status>(r) && resp->status) {
        if (json) {
            std::println("{}", encode_status(*resp->status, 2));
        } else {
            std::print("{}", format_status(*resp->status));
        }
    }
    return 0;
}

/// Prints one line and flushes; false once stdout is gone (Waybar exited).
bool emit_line(std::string_view line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    return std::fflush(stdout) == 0;
}

int run_waybar() {
    bool down_shown = false;
    for (;;) {
        if (auto sub = subscribe(socket_path())) {
            down_shown = false;
            for (;;) {
                auto s = sub->next_status();
                if (!s || !*s) {
                    break;
                }
                if (!emit_line(waybar_json(**s).dump())) {
                    return 0;
                }
            }
        }
        if (!down_shown) {
            if (!emit_line(kWaybarNotRunning)) {
                return 0;
            }
            down_shown = true;
        }
        std::this_thread::sleep_for(5s);
    }
}

int run_daemon_cmd(const Args &x) {
    RunOptions o;
    o.backend = backend_kind_from_name(x.backend).value_or(BackendKind::Auto);
    o.sys_root = x.sys_root;
    o.config_path = config_path();
    o.state_path = state_path();
    o.socket_path = socket_path();
    o.mock_render = x.mock_render;
    o.with_sources = !x.no_sources;
    return run_daemon(o);
}

int run_gen_hyprland() {
    const auto cfg = load_config(config_path());
    if (!cfg) {
        std::println(stderr, "omen-kbd: {}", cfg.error());
        return kExitError;
    }
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    const auto command = hook_command(ec ? std::filesystem::path() : exe);
    if (command == "omen-kbd") {
        std::println(stderr, "omen-kbd: the hook calls `omen-kbd` by name, so it must be on "
                             "Hyprland's PATH (the binary's own path cannot be used in a hook)");
    }
    std::print("{}", gen_hyprland(*cfg, command));
    return 0;
}

/// Prints `OK|WARN|FAIL  message` per check; exit 1 if anything failed. A broken config is a
/// failure of its own; the remaining checks then use the built-in defaults.
int run_doctor() {
    std::vector<Check> checks;
    const auto path = config_path();
    auto cfg = load_config(path);
    std::error_code ec;
    if (!cfg) {
        checks.push_back({Level::Fail, cfg.error()});
    } else if (std::filesystem::exists(path, ec)) {
        checks.push_back({Level::Ok, "config " + path.string()});
    } else {
        checks.push_back(
            {Level::Ok, "no config file; using built-in defaults (" + path.string() + ")"});
    }
    const Config used = cfg ? std::move(*cfg) : embedded_default();
    std::ranges::move(run_checks(DoctorEnv::real(used)), std::back_inserter(checks));

    bool failed = false;
    for (const auto &c : checks) {
        std::println("{:<4}  {}", level_name(c.level), c.msg);
        failed = failed || c.level == Level::Fail;
    }
    return failed ? kExitError : 0;
}

} // namespace

std::optional<Request> parse_args_to_request(int argc, const char *const *argv,
                                             std::string *error) {
    Args x;
    const auto app = make_app(x);
    try {
        app->parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        if (error != nullptr) {
            *error = e.what();
        }
        return std::nullopt;
    }
    if (error != nullptr) {
        error->clear();
    }
    return x.request;
}

int cli_main(int argc, char **argv) {
    Args x;
    const auto app = make_app(x);
    try {
        app->parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        const int code = app->exit(e); // prints help, version or the error
        return code == 0 ? 0 : kExitUsage;
    }
    switch (x.mode) {
    case Args::Mode::Request:
        return run_request(*x.request, x.json);
    case Args::Mode::Waybar:
        return run_waybar();
    case Args::Mode::Daemon:
        return run_daemon_cmd(x);
    case Args::Mode::Doctor:
        return run_doctor();
    case Args::Mode::GenHyprland:
        return run_gen_hyprland();
    case Args::Mode::None:
        break;
    }
    return kExitUsage;
}

} // namespace omenkbd
