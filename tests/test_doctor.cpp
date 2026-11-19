#include "cli/doctor.hpp"
#include "config.hpp"
#include "helpers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace omenkbd {
namespace {

using test::TempDir;
using test::write_file;

struct DoctorFixture : ::testing::Test {
    TempDir t;
    DoctorEnv env{.sys_root = t.path() / "sys",
                  .proc_root = t.path() / "proc",
                  .dkms_root = t.path() / "dkms",
                  .hypr_dir = t.path() / "hypr",
                  .socket = t.path() / "omen-kbd.sock"};

    void make_dir(const std::filesystem::path &p) { std::filesystem::create_directories(p); }

    void hp_omen_kbd(std::string_view info) {
        const auto d = env.sys_root / "devices/platform/hp-omen-kbd";
        write_file(d / "zones", "000000 000000 000000 000000\n");
        write_file(d / "backlight", "1\n");
        write_file(d / "info", info);
    }

    void mainline_leds() {
        for (const char *z : {"right", "center", "left", "wasd"}) {
            const auto d =
                env.sys_root / "class/leds" / (std::string("hp::kbd_zoned_backlight-") + z);
            write_file(d / "multi_intensity", "0 0 0\n");
            write_file(d / "brightness", "255\n");
        }
    }
};

/// The checks at `level` whose message contains `needle`.
std::size_t count(const std::vector<Check> &checks, Level level, std::string_view needle) {
    return static_cast<std::size_t>(std::ranges::count_if(checks, [&](const Check &c) {
        return c.level == level && c.msg.find(needle) != std::string::npos;
    }));
}

std::string dump(const std::vector<Check> &checks) {
    std::string s;
    for (const auto &c : checks) {
        s += std::string(level_name(c.level)) + "  " + c.msg + "\n";
    }
    return s;
}

using Doctor = DoctorFixture;

TEST_F(Doctor, FlagsCompetingDrivers) {
    hp_omen_kbd("board=8A4E readonly=0\n");
    write_file(env.proc_root / "modules", "hp_wmi 61440 0 - Live 0x0\n"
                                          "hp_omen_extra 20480 0 - Live 0x0\n");
    write_file(env.proc_root / "4242/comm", "omen-space-daem\n");
    write_file(env.proc_root / "4243/comm", "openrgb\n");
    write_file(env.proc_root / "self/comm", "omen-kbd-tests\n");
    make_dir(env.sys_root / "devices/platform/hp-wmi/rgb_zones");
    const auto checks = run_checks(env);
    EXPECT_EQ(count(checks, Level::Fail, "hp_omen_extra"), 1U) << dump(checks);
    EXPECT_EQ(count(checks, Level::Fail, "omen-space"), 1U) << dump(checks);
    EXPECT_EQ(count(checks, Level::Warn, "openrgb"), 1U) << dump(checks);
    EXPECT_EQ(count(checks, Level::Warn, "rgb_zones"), 1U) << dump(checks);
    EXPECT_EQ(count(checks, Level::Fail, "hp_wmi"), 0U) << dump(checks);
}

TEST_F(Doctor, QuietSystemHasNoCompetingWriters) {
    hp_omen_kbd("board=8A4E readonly=0\n");
    write_file(env.proc_root / "modules", "hp_wmi 61440 0 - Live 0x0\n");
    write_file(env.proc_root / "1/comm", "systemd\n");
    const auto checks = run_checks(env);
    EXPECT_EQ(count(checks, Level::Ok, "no competing keyboard writers"), 1U) << dump(checks);
    EXPECT_EQ(count(checks, Level::Ok, "backend hp-omen-kbd"), 1U) << dump(checks);
    EXPECT_EQ(count(checks, Level::Ok, "writable"), 1U) << dump(checks);
    EXPECT_EQ(count(checks, Level::Fail, ""), 0U) << dump(checks);
    EXPECT_EQ(count(checks, Level::Warn, "readonly"), 0U) << dump(checks);
    EXPECT_EQ(count(checks, Level::Warn, "daemon not running"), 1U) << dump(checks);
}

TEST_F(Doctor, ReadonlyModuleWarns) {
    hp_omen_kbd("board=8A4E readonly=1 min_interval_ms=50 writes=0\n");
    const auto checks = run_checks(env);
    EXPECT_EQ(count(checks, Level::Warn, "readonly=1"), 1U) << dump(checks);
}

TEST_F(Doctor, FlagsHpwmiDkms) {
    write_file(env.dkms_root / "hp-omen-extra/2.1.2/source/dkms.conf",
               "PACKAGE_NAME=\"hp-omen-extra\"\n"
               "BUILT_MODULE_NAME[0]=\"hp-omen-extra\"\n"
               "BUILT_MODULE_NAME[1]=\"hp-wmi\"\n");
    write_file(env.dkms_root / "nvidia/580.1/source/dkms.conf",
               "BUILT_MODULE_NAME[0]=\"nvidia\"\n# BUILT_MODULE_NAME[1]=\"hp_wmi\"\n");
    write_file(env.dkms_root / "mok.key", "secret\n");
    const auto checks = run_checks(env);
    EXPECT_EQ(count(checks, Level::Warn, "hp-omen-extra/2.1.2"), 1U) << dump(checks);
    EXPECT_EQ(count(checks, Level::Warn, "nvidia"), 0U) << dump(checks);
}

TEST_F(Doctor, BothBackendsFail) {
    hp_omen_kbd("readonly=0\n");
    mainline_leds();
    const auto checks = run_checks(env);
    EXPECT_EQ(count(checks, Level::Fail, "both hp-omen-kbd and mainline LEDs"), 1U) << dump(checks);
}

TEST_F(Doctor, MissingBackendFails) {
    const auto checks = run_checks(env);
    EXPECT_EQ(count(checks, Level::Fail, "no keyboard backend"), 1U) << dump(checks);
}

TEST_F(Doctor, LockGateDetected) {
    write_file(env.hypr_dir / "hypridle.conf", "general {\n}\n");
    EXPECT_EQ(count(run_checks(env), Level::Warn, "lock gate not wired"), 1U);
    write_file(env.hypr_dir / "hypridle.conf",
               "general {\n    on_lock_cmd = omen-kbd flag locked on\n"
               "    on_unlock_cmd = omen-kbd flag locked off\n}\n");
    const auto checks = run_checks(env);
    EXPECT_EQ(count(checks, Level::Ok, "lock gate"), 1U) << dump(checks);
}

} // namespace
} // namespace omenkbd
