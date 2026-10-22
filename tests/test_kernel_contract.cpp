// Contract between the kernel module's core (C, kernel/hp-omen-kbd/core.c) and the C++ side: the
// text the daemon writes is what the module accepts, and the module's info line is what doctor
// reads.

#include "helpers.hpp"

#include "backend/sysfs.hpp"
#include "cli/doctor.hpp"
#include "core.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace omenkbd;
using namespace omenkbd::test;

namespace {

std::filesystem::path module_dir(const std::filesystem::path &sys_root) {
    return sys_root / "devices/platform/hp-omen-kbd";
}

/// A fake hp-omen-kbd sysfs directory with empty `zones` and `backlight` files.
void fake_module(const std::filesystem::path &sys_root) {
    write_file(module_dir(sys_root) / "zones", "");
    write_file(module_dir(sys_root) / "backlight", "");
}

Rgb rgb(std::string_view hex) { return Rgb::parse(hex).value(); }

bool same(const hok_rgb &a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

} // namespace

TEST(KernelContract, DaemonZonesLineIsAccepted) {
    TempDir d;
    fake_module(d.path());
    auto backend = sysfs::open_hp_omen_kbd(d.path());
    ASSERT_TRUE(backend.has_value()) << backend.error().message();
    const std::array<Rgb, 4> hw{rgb("40E0D0"), rgb("40E0D0"), rgb("40E0D0"), rgb("0099ED")};
    ASSERT_TRUE((*backend)->write_zones(hw).has_value());
    const std::string text = read_file(module_dir(d.path()) / "zones");
    hok_rgb out[HOK_ZONES]{};
    ASSERT_EQ(hok_parse_zones(text.data(), text.size(), out), 0) << text;
    for (std::size_t i = 0; i < hw.size(); ++i) {
        EXPECT_TRUE(same(out[i], hw[i])) << "zone " << i << " of " << text;
    }
}

TEST(KernelContract, DaemonBacklightLineIsAccepted) {
    TempDir d;
    fake_module(d.path());
    auto backend = sysfs::open_hp_omen_kbd(d.path());
    ASSERT_TRUE(backend.has_value()) << backend.error().message();
    for (const bool on : {true, false}) {
        ASSERT_TRUE((*backend)->set_backlight(on).has_value());
        const std::string text = read_file(module_dir(d.path()) / "backlight");
        EXPECT_EQ(hok_parse_bool(text.data(), text.size()), on ? 1 : 0) << text;
    }
}

TEST(KernelContract, DoctorReadsTheModulesInfoLine) {
    for (const bool readonly : {true, false}) {
        TempDir t;
        const DoctorEnv env{.sys_root = t.path() / "sys",
                            .proc_root = t.path() / "proc",
                            .dkms_root = t.path() / "dkms",
                            .hypr_dir = t.path() / "hypr",
                            .theme_dir = t.path() / "theme",
                            .socket = t.path() / "omen-kbd.sock"};
        fake_module(env.sys_root);
        hok_info info{};
        info.board = "8A4E";
        info.kbd_type = 2;
        info.readonly = readonly;
        info.min_interval_ms = 50;
        info.max_writes_per_hour = 40000;
        char line[512];
        const int n = hok_format_info(line, sizeof(line), &info);
        ASSERT_GT(n, 0);
        write_file(module_dir(env.sys_root) / "info",
                   std::string_view(line, static_cast<std::size_t>(n)));

        const auto checks = run_checks(env);
        const auto has = [&checks](Level level, std::string_view needle) {
            return std::ranges::any_of(checks, [&](const Check &c) {
                return c.level == level && c.msg.find(needle) != std::string::npos;
            });
        };
        if (readonly) {
            EXPECT_TRUE(has(Level::Warn, "loaded with readonly=1")) << line;
        } else {
            EXPECT_TRUE(has(Level::Ok, "hp-omen-kbd accepts writes (readonly=0)")) << line;
        }
    }
}
