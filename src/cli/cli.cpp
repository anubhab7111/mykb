#include "cli/cli.hpp"

#include "cli/doctor.hpp"
#include "cli/format.hpp"
#include "config.hpp"
#include "core/signals.hpp"
#include "core/window_address.hpp"
#include "daemon/daemon.hpp"
#include "ipc/socket.hpp"
#include "state.hpp"

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
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
    enum class Mode { None, Request, Daemon, Doctor };
    Mode mode = Mode::None;
    std::optional<Request> request;
    bool json = false;

    std::vector<std::string> colors;
    std::string effect;
    std::string a;
    std::string b;
    int speed = 0;

    std::string app;
    std::string desktop_entry;
    std::string urgency = "normal";
    std::string session;
    std::optional<std::string> window;
    std::string source = "claude";

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

    auto *set = app->add_subcommand("set", "Set the look: a profile or colors, and an effect");
    set->add_option(
           "look", x.colors,
           "A profile name, or #rrggbb: one for every zone, or four (left middle right wasd)")
        ->expected(0, 4);
    set->add_option("--effect", x.effect, "static, drift, glow, tide or dusk")
        ->transform(CLI::IsMember({"static", "drift", "glow", "tide", "dusk"}, CLI::ignore_case));
    set->add_option("--speed", x.speed, "1-100 (50 by default)")->check(CLI::Range(1, 100));
    sends(set, [&x] {
        if (x.colors.empty() && x.effect.empty() && x.speed == 0) {
            throw CLI::ValidationError("set", "needs a profile, colors, --effect or --speed");
        }
        return req::Set{
            x.colors, x.effect.empty() ? std::nullopt : std::optional<std::string>(x.effect),
            x.speed > 0 ? std::optional(static_cast<std::uint8_t>(x.speed)) : std::nullopt};
    });

    auto *brightness = app->add_subcommand("brightness", "Set brightness");
    brightness->add_option("value", x.a, "0-100, +N or -N")->required();
    sends(brightness, [&x] { return req::Brightness{x.a}; });

    sends(app->add_subcommand("on", "Turn the backlight on"),
          [] { return req::Power{req::PowerAction::On}; });
    sends(app->add_subcommand("off", "Turn the backlight off (colors are kept)"),
          [] { return req::Power{req::PowerAction::Off}; });
    sends(app->add_subcommand("toggle", "Toggle the backlight"),
          [] { return req::Power{req::PowerAction::Toggle}; });

    auto *idle = app->add_subcommand("idle", "Idle gate (used by hypridle)");
    idle->add_option("state", x.a, "on or off")->required()->check(on_off);
    sends(idle, [&x] { return req::Idle{x.a == "on"}; });

    auto *flag = app->add_subcommand("flag", "Set a flag: gamemode, locked");
    flag->add_option("name", x.a, "gamemode or locked")->required();
    flag->add_option("state", x.b, "on or off")->required()->check(on_off);
    sends(flag, [&x] { return req::Flag{x.a, x.b == "on"}; });

    // swaync passes its urgency capitalised (`Low`, `Normal`, `Critical`); the request carries the
    // canonical lowercase name, and a bad value is a usage error.
    const CLI::Validator urgency_name(
        [](std::string &value) {
            const auto urgency = urgency_from_name(value);
            if (!urgency) {
                return std::string("must be low, normal or critical");
            }
            value = std::string(name(*urgency));
            return std::string();
        },
        "low|normal|critical");

    auto *notify = app->add_subcommand(
        "notify", "Report a desktop notification so the keyboard can flash (used by swaync)");
    notify->add_option("--app", x.app, "The sending app's name");
    notify->add_option("--desktop-entry", x.desktop_entry, "The sending app's desktop file id");
    notify->add_option("--urgency", x.urgency, "low, normal or critical (any case)")
        ->transform(urgency_name)
        ->capture_default_str();
    sends(notify, [&x] { return req::Notify{x.app, x.desktop_entry, x.urgency}; });

    // The daemon refuses an empty or oversized session id (it is the lamp's key); say so here, as a
    // usage error, with the same bound.
    const std::string session_bytes = std::format("1 to {} bytes", kMaxSessionBytes);
    const CLI::Validator session_id(
        [session_bytes](const std::string &value) {
            return value.empty() || value.size() > kMaxSessionBytes
                       ? "must be " + session_bytes + ", got " + std::to_string(value.size())
                       : std::string();
        },
        session_bytes);

    // The daemon also refuses a window that is given but is not an address (T14-5); an empty one
    // names no window. Only `waiting` and `done` use a window, so only they are checked.
    const std::string window_form =
        std::format("1 to {} hex digits, optionally after 0x", kMaxWindowAddressDigits);
    const auto window_problem = [window_form](const std::string &value) {
        return value.empty() || !normalize_window_address(value).empty()
                   ? std::string()
                   : "must be " + window_form + ", got \"" + value + "\"";
    };

    auto *agent = app->add_subcommand(
        "agent", "Report what a Claude Code session is doing (used by its hook script)");
    agent->add_option("state", x.a, "waiting, working, done or end")
        ->required()
        ->check(CLI::IsMember({"waiting", "working", "done", "end"}));
    agent->add_option("--session", x.session, "The session's id, " + session_bytes)
        ->required()
        ->check(session_id);
    agent->add_option("--window", x.window,
                      "The Hyprland address of the session's terminal (waiting and done use it)");
    agent->add_option("--source", x.source, "Which tool reports this session")
        ->capture_default_str();
    sends(agent, [&x, window_problem] {
        // `working` and `end` send no window and ignore a bad one, so a wrapper that always adds
        // `--window` can still clear a lamp (review of the minors, Minor 1).
        std::optional<std::string> window;
        if (x.a == "waiting" || x.a == "done") {
            window = x.window;
            if (window) {
                if (const std::string problem = window_problem(*window); !problem.empty()) {
                    throw CLI::ValidationError("--window", problem); // a usage error: exit 64
                }
            }
        }
        return req::Agent{x.a, x.session, std::move(window), x.source};
    });

    sends(app->add_subcommand("reload", "Re-read the config file"), [] { return req::Reload{}; });

    app->add_subcommand("doctor", "Check the driver, permissions, competing writers and hooks")
        ->callback([&x] { x.mode = Args::Mode::Doctor; });
    auto *daemon = app->add_subcommand("daemon", "Run the daemon");
    daemon->add_option("--backend", x.backend, "auto, mock, hp-omen-kbd or mainline-led")
        ->check(CLI::IsMember({"auto", "mock", "hp-omen-kbd", "mainline-led"}));
    daemon->add_flag("--mock-render", x.mock_render, "Print mock frames as colored blocks");
    daemon->add_flag("--no-sources", x.no_sources,
                     "Ignore Hyprland, hwmon and system D-Bus events");
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

/// Prints `OK|WARN|FAIL  message` per check; exit 1 if anything failed. A broken config is a
/// failure of its own.
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
    std::ranges::move(run_checks(DoctorEnv::real()), std::back_inserter(checks));

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
        return x.request ? run_request(*x.request, x.json) : kExitUsage;
    case Args::Mode::Daemon:
        return run_daemon_cmd(x);
    case Args::Mode::Doctor:
        return run_doctor();
    case Args::Mode::None:
        break;
    }
    return kExitUsage;
}

} // namespace omenkbd
