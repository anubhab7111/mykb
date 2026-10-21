# omen-kbd C++ Port + Completion Implementation Plan (spec phase P2, C++)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `omen-kbd` (user daemon + CLI) in C++23 with every feature of the spec working against a **mock backend** and fake sysfs trees — porting the Rust prototype's behaviour test-for-test, then completing the features the prototype never reached.

**Architecture:** Same layering as the spec: pure `core` (color, zones, palette, effects, rules, compositor) → `daemon::Model` (pure state machine) → `daemon::Daemon` (single-threaded `epoll` loop: IPC socket, timers, sources incl. sdbus-c++) → `Writer` (one thread, sole owner of the `Backend`, latest-job slot = coalescing). CLI talks to the daemon over a Unix socket with JSON lines.

**Tech Stack:** C++23 (GCC 16 / Clang 23), CMake ≥ 3.28 + Ninja, GoogleTest 1.18, sdbus-c++ 2.3, nlohmann-json 3.12, toml++ 3.4, CLI11 2.7 — all already installed as system packages.

**Spec:** `docs/superpowers/specs/2026-10-21-omen-kbd-design.md`.
**Behavioural reference:** the Rust prototype at git tag **`rust-prototype`** (plan Tasks 1–13 done there, 108 tests) — read a file with `git show rust-prototype:<path>` — plus the Rust plan `docs/superpowers/plans/2026-10-21-omen-kbd-daemon.md` (exact semantics, pinned formulas, test lists for Tasks 14–18) and the ledger rulings in `.superpowers/sdd/2026-10-21-omen-kbd-daemon/progress.md`. Where this plan says "port", the Rust test names, inputs and literal expected values are the spec for the C++ tests.

## Global Constraints

- Never touch hardware: tests and demos use the mock backend or fake sysfs trees under a temp dir (`sys_root`). Nothing in this plan writes under `/sys`.
- The daemon runs as the user, never root; no evdev, no `input` group. The tool never writes `~/.config/omen-kbd/config.toml` or anything under `~/.config/hypr`.
- Socket `$XDG_RUNTIME_DIR/omen-kbd.sock`, mode `0600`, newline-delimited JSON. State `$XDG_STATE_HOME/omen-kbd/state.json` (default `~/.local/state/…`), atomic tmp+fsync+rename.
- **Wire formats are identical to the Rust prototype**: JSON requests/responses/status (field names, `"cmd"` tag, kebab-case values), `state.json` fields, and the config schema (`packaging/config.example.toml` is unchanged).
- Logical zone order `[left, middle, right, wasd]`; default zone map left→2, middle→1, right→0, wasd→3.
- hp-omen-kbd sysfs `<sys_root>/devices/platform/hp-omen-kbd/{zones,backlight}`, `zones` = `RRGGBB RRGGBB RRGGBB RRGGBB\n` in hardware order; errno EBUSY = budget, EPERM = readonly, EACCES = permission, ENOENT = missing. mainline-led `<sys_root>/class/leds/hp::kbd_zoned_backlight-{right,center,left,wasd}/{multi_intensity,brightness}` = hw 0..3.
- Backend `auto` picks a real backend only; mock only when requested; both real backends present ⇒ refuse.
- Effects `enabled = false` by default; fps default 10, range 1..=20; hint expiry 15 s; Lua hold delay 250 ms; theme colors vivid by default.
- CLI exit codes: 0 ok; 1 daemon/hardware error (message on stderr); 2 daemon unreachable.
- C++ rules: namespace `omenkbd`; `.hpp`/`.cpp` with `#pragma once`; errors as `std::expected<T, E>`; **no exception crosses a module boundary** (toml++/nlohmann/sdbus-c++ exceptions are caught where called and converted); RAII for every fd (`UniqueFd`); only the writer thread touches the `Backend`; only the loop thread touches `Model`/`Daemon` state; `std::chrono::steady_clock` for all timing.
- Build/test gate for every task: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build --output-on-failure` green with **zero warnings** (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`) under **ASan + UBSan** (Debug default), and `clang-format --dry-run --Werror` clean on changed files.
- Test naming: GoogleTest `TEST(<Module>, <RustTestNameInCamelCase>)`, e.g. Rust `parses_hex_with_and_without_hash` in `core::color` ⇒ `TEST(Color, ParsesHexWithAndWithoutHash)`.
- Install target for later phases: `cmake --install build --prefix ~/.local` (→ `~/.local/bin/omen-kbd`, on PATH).

## Review Focus

- Reloading a config with a syntax error → previous config stays active; `omen-kbd reload` exits 1 with the parse error (`TEST(Daemon, BadReloadKeepsPreviousConfig)`, Task 13).
- A `theme:` color missing from the theme → white + a status warning, no crash (`TEST(Model, UnknownThemeColorWarnsAndUsesWhite)`, Task 12).
- Stale socket from a crashed daemon / second daemon → stale replaced; second instance gets "already running" (`TEST(Ipc, BindReplacesStaleSocket)`, `TEST(Ipc, BindRefusesWhenRunning)`, Task 11).
- Resume with an unchanged frame → rewritten once (`TEST(Daemon, ResumeForcesIdenticalRewrite)`, Task 13).
- Hyprland restart / socket2 closed during a submap → submap hint cleared, reader reconnects (`TEST(Hyprland, Socket2DisconnectClearsSubmapAndReconnects)`, Task 14).

---

### Task 1: Project scaffold, utilities, test helpers

**Files:**
- Delete (preserved at tag `rust-prototype`): `Cargo.toml`, `Cargo.lock`, `src/**/*.rs`, `tests/daemon_mock.rs`.
- Create: `CMakeLists.txt`, `.clang-format`, `cmake/embed_text.cmake`, `src/util/fd.hpp`, `src/main.cpp`, `tests/helpers.hpp`, `tests/test_util.cpp`; Modify `.gitignore` (`/build`).

**Interfaces — Produces:**
- `class UniqueFd` (move-only RAII; `explicit UniqueFd(int fd = -1)`, `int get() const`, `int release()`, `explicit operator bool() const`, closes in dtor).
- Test helpers (`tests/helpers.hpp`): `class TempDir` (`mkdtemp` under `$TMPDIR`, `const std::filesystem::path& path() const`, recursive remove in dtor), `void write_file(const path&, std::string_view)`, `std::string read_file(const path&)`, `bool wait_until(std::function<bool()>, std::chrono::milliseconds timeout)` (polls every 10 ms).
- CMake: library `omenkbd` = `file(GLOB_RECURSE CONFIGURE_DEPENDS src/*.cpp)` minus `src/main.cpp`; `target_include_directories(omenkbd PUBLIC src ${CMAKE_BINARY_DIR}/generated)`; links `SDBusCpp::sdbus-c++ nlohmann_json::nlohmann_json tomlplusplus::tomlplusplus CLI11::CLI11 Threads::Threads`; PUBLIC options `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`; option `OMEN_KBD_SANITIZE` (ON) adds `-fsanitize=address,undefined -fno-omit-frame-pointer` (compile + link) to all targets in Debug. Executable `omen-kbd` (`src/main.cpp`). Tests `omen-kbd-tests` = `file(GLOB CONFIGURE_DEPENDS tests/*.cpp)` + `GTest::gtest_main`, registered with `gtest_discover_tests`. `cmake/embed_text.cmake` turns `packaging/config.example.toml` into `${CMAKE_BINARY_DIR}/generated/config_example.hpp` defining `inline constexpr std::string_view kConfigExample = R"omenkbd(…)omenkbd";` (re-generated when the file changes).
- `.clang-format`: `BasedOnStyle: LLVM`, `IndentWidth: 4`, `ColumnLimit: 100`.

- [ ] **Step 1:** `git rm` the Rust sources listed above (they stay reachable via the tag). Keep `packaging/`, `spikes/`, `docs/`.
- [ ] **Step 2: Failing tests** in `tests/test_util.cpp`: `TEST(Util, UniqueFdClosesOnDestruction)` (open `/dev/null`, let it go out of scope, `fcntl(fd, F_GETFD)` fails with `EBADF`); `TEST(Util, UniqueFdMoveTransfersOwnership)`; `TEST(Util, TempDirIsRemoved)`; `TEST(Util, EmbeddedExampleConfigMatchesFile)` (`kConfigExample` == contents of `packaging/config.example.toml`, path via a compile definition `OMEN_KBD_SOURCE_DIR`).
- [ ] **Step 3:** configure + build → FAIL (missing headers). **Step 4:** implement; `src/main.cpp` = `int main() { return 0; }` for now. **Step 5:** gate green. **Step 6: Commit** `build: C++23 scaffold (CMake, GoogleTest, sanitizers), RAII fd and test helpers`.

---

### Task 2: `core/color`

**Files:** `src/core/color.hpp`, `src/core/color.cpp`, `tests/test_color.cpp`

**Interfaces — Produces:** `struct Hsv { double h, s, v; };` `struct Rgb { std::uint8_t r{}, g{}, b{}; friend constexpr bool operator==(Rgb, Rgb) = default; static std::expected<Rgb, std::string> parse(std::string_view); std::string to_hex() const; Rgb scale(std::uint8_t pct) const; static Rgb lerp(Rgb a, Rgb b, double t); Hsv to_hsv() const; static Rgb from_hsv(double h, double s, double v); Rgb vivid() const; };` `inline constexpr Rgb kBlack{0,0,0}, kWhite{255,255,255};` `std::ostream& operator<<(std::ostream&, Rgb)` (prints `#RRGGBB`, for GoogleTest messages). Test helper in `tests/helpers.hpp`: `void expect_close(Rgb a, Rgb b, int tol)`.

- [ ] **Step 1: Port tests** from `git show rust-prototype:src/core/color.rs` (all except `serde_uses_hash_hex_string` — `Rgb` has no JSON form in C++). **Step 2:** FAIL. **Step 3:** implement with the pinned rules from the Rust plan Task 2 (rounding half away from zero via `std::round`; `vivid`: `s < 0.1` unchanged, else `s = max(s, 0.85)`, `v = 1`). **Step 4:** gate green. **Step 5: Commit** `feat(core): Rgb color type`.

---

### Task 3: `core/zones`

**Files:** `src/core/zones.hpp/.cpp`, `tests/test_zones.cpp`

**Interfaces — Produces:** `enum class Zone : std::uint8_t { Left, Middle, Right, Wasd };` `inline constexpr std::array<Zone, 4> kAllZones`; `constexpr std::size_t index(Zone)`; `std::string_view name(Zone)`; `std::optional<Zone> zone_from_name(std::string_view)` (case-insensitive, `center` ⇒ `Middle`); `using Frame = std::array<Rgb, 4>;` `class ZoneMap { public: ZoneMap(); /* 2,1,0,3 */ static std::expected<ZoneMap, std::string> make(std::size_t left, std::size_t middle, std::size_t right, std::size_t wasd); std::array<Rgb, 4> to_hw(const Frame&) const; friend bool operator==(const ZoneMap&, const ZoneMap&) = default; private: std::array<std::size_t, 4> hw_; };`

- [ ] Port `default_map_matches_spec`, `rejects_non_permutation`, `zone_names`, and the name half of `zone_serde_is_lowercase_name_with_center_alias` (as `TEST(Zones, NamesAreLowercaseWithCenterAlias)` using `name`/`zone_from_name`). `zone_map_from_toml` moves to Task 7 (TOML lives in config). FAIL → implement → gate green → **Commit** `feat(core): zones and zone map`.

---

### Task 4: `core/palette`

**Files:** `src/core/palette.hpp/.cpp`, `tests/test_palette.cpp`

**Interfaces — Produces:** `class Palette { public: static Palette from_waybar_css(std::string_view); static std::expected<Palette, std::string> from_theme_toml(std::string_view); static std::expected<Palette, std::string> load(const std::filesystem::path& theme_dir); std::optional<Rgb> get(std::string_view) const; std::size_t size() const; bool empty() const; private: std::map<std::string, Rgb, std::less<>> colors_; };` `enum class ThemeColors { Vivid, Exact };` `std::optional<ThemeColors> theme_colors_from_name(std::string_view);` `class ColorRef { public: static std::expected<ColorRef, std::string> parse(std::string_view); static ColorRef hex(Rgb); static ColorRef theme(std::string name); std::string to_string() const; /* "#RRGGBB" | "theme:<name>" */ std::expected<Rgb, std::string> resolve(const Palette&, ThemeColors) const; /* error = missing name */ friend bool operator==(const ColorRef&, const ColorRef&) = default; private: std::variant<Rgb, std::string> v_; };`

- [ ] Port all six tests from `git show rust-prototype:src/core/palette.rs` (`serde_forms` ⇒ `TEST(Palette, StringForms)` using `parse`/`to_string`/`theme_colors_from_name`). Error texts as in the Rust code (`invalid color "…": expected #RRGGBB or theme:<name>`). toml++ exceptions from `from_theme_toml` are caught and returned as the error string. FAIL → implement → gate → **Commit** `feat(core): theme palette and color references`.

---

### Task 5: `core/effects`

**Files:** `src/core/effects.hpp/.cpp`, `tests/test_effects.cpp`

**Interfaces — Produces:** `enum class Effect { Static, Breathe, Wave, Cycle, Gradient };` `std::string_view name(Effect)`; `std::optional<Effect> effect_from_name(std::string_view)` (case-insensitive); `constexpr bool is_animated(Effect)`; `double period_secs(std::uint8_t speed)`; `Frame render(Effect, const Frame& base, double t, std::uint8_t speed)`; `double pulse_factor(double t)`; `Rgb multiply(Rgb, double f)`.

- [ ] Port all eight tests from `git show rust-prototype:src/core/effects.rs` with the same literals (formulas pinned in the Rust plan Task 5). FAIL → implement → gate → **Commit** `feat(core): zone-level software effects`.

---

### Task 6: `core/rules`

**Files:** `src/core/rules.hpp/.cpp`, `tests/test_rules.cpp`

**Interfaces — Produces:** `struct Facts { std::optional<std::string> power_profile; std::optional<bool> on_battery; std::optional<double> battery_pct, cpu_temp_c; bool gamemode = false, locked = false, idle = false; friend bool operator==(const Facts&, const Facts&) = default; };` `struct Condition { std::optional<std::string> power_profile; std::optional<bool> on_battery; std::optional<double> battery_below, cpu_temp_above; std::optional<bool> gamemode, locked, idle; bool holds(const Facts&) const; };` `struct OverlayCfg { std::vector<Zone> zones; ColorRef color; bool pulse = false; };` `struct TintCfg { ColorRef color; double strength; };` `struct Rule { std::string name; Condition when; std::optional<std::string> profile; std::optional<std::uint8_t> brightness_cap; std::optional<OverlayCfg> overlay; std::optional<TintCfg> tint; std::expected<void, std::string> validate() const; };` `std::expected<Rule, std::string> parse_rule(const toml::table&);` `struct ZonesOverlay { std::vector<Zone> zones; ColorRef color; bool pulse; };` `struct TintOverlay { ColorRef color; double strength; };` `using Overlay = std::variant<ZonesOverlay, TintOverlay>;` (both with defaulted `==`) `struct RuleOutcome { std::optional<std::string> profile; std::optional<std::uint8_t> brightness_cap; std::optional<Overlay> overlay; std::vector<std::string> active; };` `RuleOutcome evaluate(std::span<const Rule>, const Facts&);`
- `parse_rule` rejects unknown keys (in the rule and in `when`/`overlay`/`tint`) with `unknown field \`<key>\``; `when` omitted = always true.

- [ ] Port all eight tests from `git show rust-prototype:src/core/rules.rs` (the `rules(src)` helper parses `[[rules]]` with toml++ and calls `parse_rule` on each). FAIL → implement → gate → **Commit** `feat(core): declarative rules engine`.

---

### Task 7: `config`

**Files:** `src/config.hpp/.cpp`, `tests/test_config.cpp`

**Interfaces — Produces:** structs mirroring the Rust `Config` (Rust plan Task 7 + `git show rust-prototype:src/config.rs`): `DeviceCfg{ZoneMap zone_map}`, `EffectsCfg{bool enabled=false; std::uint8_t fps=10}`, `ThemeCfg{ThemeColors colors; std::filesystem::path dir}`, `enum class GateAction{Off, Dim, Keep}`, `GatesCfg{GateAction on_lock=Off, on_idle=Dim; std::uint8_t dim_brightness=20; bool pause_effects_on_battery=true}`, `HintsCfg{std::uint64_t timeout_s=15, delay_ms=250; std::map<std::string, ColorRef> modifiers, submaps}`, `ProfileCfg{std::string name; std::vector<ColorRef> colors; Effect effect=Effect::Static; std::uint8_t speed=50}`, `struct Config { DeviceCfg device; EffectsCfg effects; ThemeCfg theme; GatesCfg gates; HintsCfg hints; std::vector<ProfileCfg> profiles; std::vector<Rule> rules; const ProfileCfg* profile(std::string_view) const; bool needs_cpu_temp() const; };` free functions `std::expected<Config, std::string> parse_config(std::string_view)`, `std::expected<Config, std::string> load_config(const std::filesystem::path&)` (missing file ⇒ embedded default; errors prefixed with the path), `Config embedded_default()` (parses `kConfigExample`), `std::filesystem::path config_path()`, `std::expected<std::string, std::string> normalize_mods(std::string_view)`, `std::filesystem::path expand_home(std::string_view)`, `inline constexpr std::string_view kCustomProfile = "custom";`
- Unknown keys at any level ⇒ error containing ``unknown field `<key>` ``; type errors name the field. All validation messages contain the substrings the Rust tests check.

- [ ] Port all seven tests from `git show rust-prototype:src/config.rs`, plus `TEST(Config, ZoneMapFromToml)` (from the Rust zones test: `zone_map = { left = 0, middle = 1, right = 2, wasd = 3 }` maps identity; a duplicate index is an error). FAIL → implement → gate → **Commit** `feat: TOML config schema and validation`.

---

### Task 8: `core/compositor`

**Files:** `src/core/compositor.hpp/.cpp`, `tests/test_compositor.cpp`

**Interfaces — Produces:** `struct ResolvedZones { std::vector<Zone> zones; Rgb color; bool pulse; };` `struct ResolvedTint { Rgb color; double strength; };` `using ResolvedOverlay = std::variant<ResolvedZones, ResolvedTint>;` `struct Inputs { Frame base; Effect effect; std::uint8_t speed; bool animate; std::optional<ResolvedOverlay> overlay; std::optional<Rgb> hint; std::uint8_t brightness; bool backlight; };` `struct Output { bool backlight; Frame zones; bool animated; friend bool operator==(const Output&, const Output&) = default; };` `Output compose(const Inputs&, double t);`

- [ ] Port all nine tests from `git show rust-prototype:src/core/compositor.rs` (layer order pinned in the Rust plan Task 8). FAIL → implement → gate → **Commit** `feat(core): layer compositor`.

---

### Task 9: `backend`

**Files:** `src/backend/backend.hpp/.cpp` (interface, errors, `open_backend`, errno map, attr I/O), `src/backend/mock.hpp/.cpp`, `src/backend/hp_omen_kbd.cpp`, `src/backend/mainline_led.cpp`, `tests/test_backend.cpp`

**Interfaces — Produces:** `struct BackendError { enum class Kind { Budget, ReadOnly, Permission, Missing, Io } kind; std::string detail; std::string message() const; friend bool operator==(const BackendError&, const BackendError&) = default; };` (`message()` texts identical to the Rust `#[error]` strings). `class Backend { public: virtual ~Backend() = default; virtual std::string_view name() const = 0; virtual std::expected<void, BackendError> write_zones(const std::array<Rgb, 4>& hw) = 0; virtual std::expected<void, BackendError> set_backlight(bool on) = 0; virtual std::expected<bool, BackendError> read_backlight() = 0; };` `enum class BackendKind { Auto, Mock, HpOmenKbd, MainlineLed };` `std::optional<BackendKind> backend_kind_from_name(std::string_view)` (`auto|mock|hp-omen-kbd|mainline-led`); `std::expected<std::unique_ptr<Backend>, BackendError> open_backend(BackendKind, const std::filesystem::path& sys_root);` `BackendError map_errno(int err, const std::filesystem::path&);` Mock: `struct MockOp { enum class Kind { Zones, Backlight } kind; std::array<Rgb, 4> zones{}; bool on = false; friend bool operator==(const MockOp&, const MockOp&) = default; static MockOp zones_op(std::array<Rgb,4>); static MockOp backlight_op(bool); };` `class MockHandle { public: std::vector<MockOp> ops() const; void fail_with(std::optional<BackendError>); void set_hw_backlight(bool); void set_delay(std::chrono::milliseconds); };` (shared state behind `std::shared_ptr` + `std::mutex`) `std::pair<std::unique_ptr<Backend>, MockHandle> make_mock(bool render);`
- Attribute writes: `open(O_WRONLY | O_TRUNC | O_CLOEXEC)` (never `O_CREAT`), one `write(2)`, errno mapped via `map_errno`.

- [ ] Port all ten tests from `git show rust-prototype:src/backend/mod.rs` (`kind_cli_names` ⇒ `backend_kind_from_name`). FAIL → implement → gate → **Commit** `feat(backend): mock, hp-omen-kbd and mainline-led backends`.

---

### Task 10: `state`

**Files:** `src/state.hpp/.cpp`, `tests/test_state.cpp`

**Interfaces — Produces:** `struct EffectOverride { Effect effect; std::uint8_t speed; friend bool operator==(...) = default; };` `struct PersistedState { std::string active_profile; std::optional<std::vector<std::string>> custom; std::optional<EffectOverride> effect_override; std::uint8_t brightness = 100; bool on = true; std::uint64_t lifetime_writes = 0; static PersistedState default_for(std::string_view first_profile); static std::pair<PersistedState, std::optional<std::string>> load(const std::filesystem::path&, std::string_view first_profile); std::expected<void, std::string> save(const std::filesystem::path&) const; friend bool operator==(...) = default; };` `std::filesystem::path state_path();` JSON keys identical to Rust (`effect_override: {effect: "breathe", speed: 30}`); unknown keys ignored, missing keys defaulted.

- [ ] Port all four tests from `git show rust-prototype:src/state.rs`. FAIL → implement (`save`: write `<name>.tmp`, `fsync`, `rename`) → gate → **Commit** `feat: atomic persisted state`.

---

### Task 11: `ipc` — protocol and socket

**Files:** `src/ipc/protocol.hpp/.cpp`, `src/ipc/socket.hpp/.cpp`, `tests/test_ipc.cpp`

**Interfaces — Produces:**
- Protocol: `namespace req { struct Status{}; struct Set{std::vector<std::string> colors;}; struct Profile{std::string select;}; struct Brightness{std::string value;}; enum class PowerAction{On, Off, Toggle}; struct Power{PowerAction action;}; struct Effect{std::string name; std::optional<std::uint8_t> speed;}; struct Hint{std::string mods;}; struct Idle{bool on;}; struct Flag{std::string name; bool on;}; struct Reload{}; struct Subscribe{}; }` (each with defaulted `==`) `using Request = std::variant<req::Status, req::Set, req::Profile, req::Brightness, req::Power, req::Effect, req::Hint, req::Idle, req::Flag, req::Reload, req::Subscribe>;` `std::string encode(const Request&)` (one JSON line, no `\n`), `std::expected<Request, std::string> decode_request(std::string_view)`; `struct Status { … }` (fields exactly as the Rust `Status`, `zones` = `std::array<std::string,4>`), `struct Response { bool ok; std::optional<std::string> error; std::optional<Status> status; static Response success(std::optional<Status>); static Response failure(std::string); };` `std::string encode(const Response&)`, `std::expected<Response, std::string> decode_response(std::string_view)`.
- Socket: `struct IpcError { enum class Kind { AlreadyRunning, NotRunning, Io, Protocol } kind; std::string detail; std::string message() const; };` `std::filesystem::path socket_path();` `std::expected<UniqueFd, IpcError> bind_socket(const std::filesystem::path&)` (live daemon ⇒ AlreadyRunning; stale file removed; `0600`; non-blocking, `SOCK_CLOEXEC`); `class LineBuffer { public: void append(std::string_view); std::optional<std::string> next_line(); };` client (blocking): `std::expected<Response, IpcError> request(const std::filesystem::path&, const Request&);` `class Subscription { public: std::expected<std::optional<Status>, IpcError> next_status(); /* nullopt = closed */ };` `std::expected<Subscription, IpcError> subscribe(const std::filesystem::path&);`

- [ ] Port all eight tests from `git show rust-prototype:src/ipc.rs` (the round-trip/subscription tests use a `std::thread` as the tiny server; byte-for-byte wire strings as in `request_wire_format`). FAIL → implement → gate → **Commit** `feat(ipc): JSON-lines protocol over a user-only Unix socket`.

---

### Task 12: `daemon/model`

**Files:** `src/daemon/model.hpp/.cpp`, `tests/test_model.cpp`

**Interfaces — Produces:** `using Clock = std::chrono::steady_clock; using TimePoint = Clock::time_point;` `namespace fact { struct PowerProfile{std::string v;}; struct OnBattery{bool v;}; struct BatteryPct{double v;}; struct CpuTemp{double v;}; struct Locked{bool v;}; struct LidClosed{bool v;}; }` `using FactUpdate = std::variant<fact::PowerProfile, fact::OnBattery, fact::BatteryPct, fact::CpuTemp, fact::Locked, fact::LidClosed>;` `enum class Change { None, Output, OutputAndPersist };` `class Model` with the Rust API in C++ form: `Model(Config, Palette, PersistedState, std::string backend);` `std::expected<Change, std::string> handle(const Request&, TimePoint now);` `void replace_config(Config, Palette);` `void set_palette(Palette);` `void apply_fact(const FactUpdate&);` `void set_suspended(bool);` `void set_submap(std::optional<std::string>);` `void set_hw_backlight(bool);` `void add_warning(std::string);` `void record_write(const std::expected<bool, BackendError>&);` `std::optional<Inputs> inputs(TimePoint now);` `Status status(const Output*) const;` `const PersistedState& persisted() const;` `const Config& config() const;` `const Palette& palette() const;` `std::optional<TimePoint> hint_deadline() const;` `bool wants_animation() const;`
- Semantics, error texts and rulings exactly as the Rust model (`git show rust-prototype:src/daemon/model.rs`) and the ledger's Task 12 rulings.

- [ ] Port all nineteen tests from `git show rust-prototype:src/daemon/model.rs` (same `HEX_CFG`). FAIL → implement → gate → **Commit** `feat(daemon): pure state model`.

---

### Task 13: Event loop, writer thread, daemon

**Files:** `src/daemon/event_loop.hpp/.cpp`, `src/daemon/writer.hpp/.cpp`, `src/daemon/daemon.hpp/.cpp`, `tests/test_daemon.cpp`

**Interfaces — Produces:**
- `class EventLoop { public: using Callback = std::function<void(std::uint32_t events)>; void watch(int fd, std::uint32_t events, Callback); void modify(int fd, std::uint32_t events); void unwatch(int fd); using TimerId = std::uint64_t; TimerId at(TimePoint, std::function<void()>); void cancel(TimerId); void on_before_poll(std::function<void()>); void run(); /* until stop() */ void stop(); /* loop thread or any thread (eventfd) */ void post(std::function<void()>); /* thread-safe, runs on the loop thread */ };` (`epoll` + one `eventfd`; timeout = earliest timer).
- `struct WriteJob { std::uint64_t generation; bool backlight; std::array<Rgb, 4> hw; bool force; };` `struct WriteOutcome { std::uint64_t generation; std::expected<bool, BackendError> result; };` `struct BacklightProbe { std::expected<bool, BackendError> result; };` `using WriterMsg = std::variant<WriteOutcome, BacklightProbe>;` `class Writer { public: explicit Writer(std::unique_ptr<Backend>); ~Writer(); /* joins */ void submit(WriteJob); /* replaces any not-yet-taken job */ void probe(); int notify_fd() const; /* eventfd, readable when messages wait */ std::vector<WriterMsg> drain(); };` — `Hw::apply` logic exactly as the Rust writer (backlight off ⇒ zones untouched; force; memory updated only on success; probe results never adopted into memory).
- `namespace event { struct Sleep{bool entering;}; struct Submap{std::optional<std::string> name;}; struct ThemeChanged{}; struct HwBacklight{bool on;}; }` `using Event = std::variant<FactUpdate, event::Sleep, event::Submap, event::ThemeChanged, event::HwBacklight>;` `struct RunOptions { BackendKind backend; std::filesystem::path sys_root, config_path, state_path, socket_path; bool mock_render; bool with_sources; };` `class Daemon { public: Daemon(Model, std::unique_ptr<Backend>, UniqueFd listen_fd, RunOptions); EventLoop& loop(); void handle_event(const Event&); /* loop thread */ void post(Event); /* any thread */ void stop(); /* any thread */ int run(); /* returns 0 on clean stop */ };` `int run_daemon(const RunOptions&);` (loads config/palette/state, opens backend, binds socket, installs `signalfd` for SIGTERM/SIGINT, runs).
- Loop behaviour, reply-after-write, 2 s reply timeout, probes, debounce (user 1 s / counter-only 60 s), resume force, reload, **no recompute after a failed write**: exactly as `git show rust-prototype:src/daemon/run.rs` and the ledger's Task 13 rulings. Clients are non-blocking fds with a `LineBuffer` each; subscribers get one status line immediately, then one per status change.

- [ ] **Step 1: Port all fifteen tests** from `git show rust-prototype:tests/daemon_mock.rs` into `tests/test_daemon.cpp` with a fixture that starts `Daemon::run()` on a `std::thread` (mock backend, temp dir) and `stop()`+`join()`s in `TearDown` (clean shutdown keeps ASan quiet); requests go through the blocking `ipc::request`; the burst test uses 100 `std::thread`s. **Step 2:** FAIL. **Step 3:** implement event loop, then writer, then daemon. **Step 4:** gate green, then `ctest --test-dir build -R Daemon --repeat until-fail:3` green. **Step 5: Commit** `feat(daemon): epoll loop, writer thread and daemon`.

---

### Task 14: Sources — Hyprland socket2, theme watcher, hwmon

**Files:** `src/sources/hyprland.hpp/.cpp`, `src/sources/theme.hpp/.cpp`, `src/sources/hwmon.hpp/.cpp`, `tests/test_sources.cpp`

**Interfaces — Produces:** `std::optional<Event> parse_socket2_line(std::string_view)`; `std::optional<std::filesystem::path> socket2_path()` (env var, else newest `$XDG_RUNTIME_DIR/hypr/*/.socket2.sock`); `class HyprlandSource { public: HyprlandSource(EventLoop&, std::function<std::optional<std::filesystem::path>()> path_fn, std::function<void(const Event&)> emit); };` (non-blocking connect; on EOF/error emit `Submap{nullopt}`, reconnect after 1 s, 2 s, then every 5 s via `EventLoop::at`). `std::vector<std::filesystem::path> link_chain(const std::filesystem::path&)`; `class ThemeWatcher { public: ThemeWatcher(EventLoop&, std::filesystem::path dir, std::function<void(const Event&)> emit); };` (inotify on each chain element's parent; 200 ms debounce; rebuild watches after a change). `std::optional<std::filesystem::path> find_package_temp(const std::filesystem::path& sys_root)`; `std::optional<double> read_temp_c(const std::filesystem::path&)`; `class HwmonPoller { public: HwmonPoller(EventLoop&, std::filesystem::path sys_root, std::function<void(const Event&)> emit); };` (every 2 s). `run_daemon` creates all three when `with_sources` (hwmon only if `config.needs_cpu_temp()`).

- [ ] Tests (from the Rust plan Task 14, real time): `TEST(Hyprland, ParseSubmapLines)`, `TEST(Hyprland, Socket2DisconnectClearsSubmapAndReconnects)` (Review Focus; fake `AF_UNIX` listener in the test), `TEST(Theme, LinkChainFollowsRiceLayout)`, `TEST(Theme, ThemeSwapEmitsEvent)`, `TEST(Hwmon, FindsPackageTemp)`. FAIL → implement → gate → **Commit** `feat(sources): hyprland socket2, theme watcher, hwmon`.

---

### Task 15: Sources — system D-Bus (sdbus-c++)

**Files:** `src/sources/system_dbus.hpp/.cpp`

**Interfaces — Produces:** `class SystemDbusSource { public: SystemDbusSource(EventLoop&, std::function<void(const Event&)> emit); };` — one `sdbus::createSystemBusConnection()`, proxies created with `sdbus::dont_run_event_loop_thread`; the connection's `getEventLoopPollData()` `fd`/`events`/`eventFd` are watched in `EventLoop` and refreshed in `on_before_poll` (timeout via `EventLoop::at`), `processPendingEvent()` drained on readiness. Sources and mappings exactly as the Rust plan Task 15 (PowerProfiles `ActiveProfile` via `tlp-pd`; UPower `OnBattery` + DisplayDevice `Percentage`; logind `PrepareForSleep`, `LidClosed`, session from `GetUser(getuid())` → `Display`, `Lock`/`Unlock` + `LockedHint`). A missing service logs one warning; no exception escapes the constructor or callbacks.

- [ ] **Step 1:** implement (thin glue; no unit test). **Step 2: Commit** `feat(sources): system D-Bus facts and sleep/lock signals` (the live check runs in Task 16).

---

### Task 16: CLI and Waybar stream

**Files:** `src/cli/cli.hpp/.cpp`, `src/cli/format.hpp/.cpp`, `src/main.cpp`, `tests/test_cli.cpp`

**Interfaces — Produces:** `int cli_main(int argc, char** argv);` (main calls it) `std::string format_status(const Status&);` `nlohmann::json waybar_json(const Status&);` `std::optional<Request> parse_args_to_request(int argc, const char* const* argv, std::string* error)` (testable parse layer). Commands, exit codes, labels and Waybar text/class rules exactly as the Rust plan Task 16, with CLI11; `brightness -10` must parse as a value (`allow_extras` off, positional accepts negative numbers). `daemon [--backend auto|mock|hp-omen-kbd|mainline-led] [--mock-render] [--no-sources]` (hidden `--sys-root`, default `/sys`). Per the ledger's pre-flight ruling, `doctor` and `gen-hyprland` are added in Task 17.

- [ ] Tests: `TEST(Cli, BrightnessAcceptsNegativeDelta)`, `TEST(Cli, SetRequiresOneOrFourColors)`, `TEST(Cli, WaybarOnOffHintError)`, `TEST(Cli, FormatStatusHasPinnedLabels)`. FAIL → implement → gate. **Smoke run** exactly as the Rust plan Task 16 Step 5 (mock backend, then the read-only D-Bus facts check against `busctl`/`upower`). **Commit** `feat(cli): commands, status and waybar stream`.

---

### Task 17: `gen-hyprland` and `doctor`

**Files:** `src/cli/gen_hyprland.hpp/.cpp`, `src/cli/doctor.hpp/.cpp`, modify `src/cli/cli.cpp`, `tests/test_gen_doctor.cpp`

**Interfaces — Produces:** `std::string gen_hyprland(const Config&);` `enum class Level { Ok, Warn, Fail };` `struct Check { Level level; std::string msg; };` `struct DoctorEnv { std::filesystem::path sys_root, proc_root, dkms_root, hypr_dir, theme_dir, socket; static DoctorEnv real(const Config&); };` `std::vector<Check> run_checks(const DoctorEnv&);`
- `gen_hyprland` emits the **spike-validated** algorithm from `spikes/kbdlight-spike.lua` with the Task 1 rulings applied: a self-disabling `repeat` ticker (50 ms × `delay_ms/50` ticks, never a `oneshot`), **a non-modifier key press cancels a pending hint**, a generation guard (`_G.omen_kbd_gen`), `hl.exec_cmd("omen-kbd hint " .. pending)` / `"omen-kbd hint clear"`, `WANTED` from `config.hints.modifiers` (sorted), no raw-key logging at all, and the header comment from the Rust plan Task 17.
- `doctor` checks 1–7 exactly as the Rust plan Task 17.

- [ ] Tests: `TEST(GenHyprland, ListsConfiguredSets)`, `TEST(GenHyprland, CompilesWithLuac)` (pipe through `luac -p -`; `GTEST_SKIP()` if absent), `TEST(GenHyprland, UsesRepeatTickerNotOneshot)` (output contains `type = "repeat"` and not `"oneshot"`), `TEST(Doctor, FlagsCompetingDrivers)`, `TEST(Doctor, FlagsHpwmiDkms)`, `TEST(Doctor, BothBackendsFail)`, `TEST(Doctor, HookDetected)`. FAIL → implement → gate. **Real read-only run:** `./build/omen-kbd doctor` — expected after P1: no competing writers, no hp-wmi DKMS package, a Fail for the missing backend. **Commit** `feat: gen-hyprland and doctor`.

---

### Task 18: Packaging snippets, README, end-to-end mock demo

**Files:** `packaging/omen-kbd.service`, `packaging/snippets/{autostart.lua,hypridle.conf,waybar-module.jsonc,rice-theme.md,gamemode.lua}`, `README.md`, `.clang-tidy`

- [ ] **Step 1:** unit file and snippets exactly as the Rust plan Task 18 (ExecStart `%h/.local/bin/omen-kbd daemon --backend auto`).
- [ ] **Step 2:** README (what it is; safety notes; build: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOMEN_KBD_SANITIZE=OFF && cmake --build build && cmake --install build --prefix ~/.local`; mock demo; commands; pointer to spec).
- [ ] **Step 3: End-to-end mock demo** exactly as the Rust plan Task 18 Step 4 (Release build; daemon idle CPU ≈ 0).
- [ ] **Step 4: Static analysis pass:** `.clang-tidy` with `bugprone-*,performance-*,modernize-use-*,-modernize-use-trailing-return-type`; `run-clang-tidy -p build src` → fix findings or record them as rulings.
- [ ] **Step 5: Full gate** (Debug + sanitizers) green. **Commit** `docs: packaging snippets, README and mock demo`.
