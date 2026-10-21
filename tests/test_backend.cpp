#include "helpers.hpp"

#include "backend/backend.hpp"
#include "backend/mock.hpp"

#include <cerrno>
#include <gtest/gtest.h>

using namespace omenkbd;
using namespace omenkbd::test;
namespace fs = std::filesystem;

namespace {

constexpr const char *kHp = "devices/platform/hp-omen-kbd";
constexpr std::array<const char *, 4> kLeds{
    "class/leds/hp::kbd_zoned_backlight-right", "class/leds/hp::kbd_zoned_backlight-center",
    "class/leds/hp::kbd_zoned_backlight-left", "class/leds/hp::kbd_zoned_backlight-wasd"};

void add_hp(const fs::path &root) {
    write_file(root / kHp / "zones", "");
    write_file(root / kHp / "backlight", "1\n");
}

void add_mainline(const fs::path &root) {
    for (const char *d : kLeds) {
        write_file(root / d / "multi_intensity", "0 0 0\n");
        write_file(root / d / "brightness", "255\n");
    }
}

std::unique_ptr<Backend> must_open(BackendKind k, const fs::path &root) {
    auto b = open_backend(k, root);
    EXPECT_TRUE(b.has_value()) << (b ? "" : b.error().message());
    return b ? std::move(*b) : nullptr;
}

} // namespace

TEST(Backend, HpWritesHwOrderLine) {
    TempDir root;
    add_hp(root.path());
    auto b = must_open(BackendKind::HpOmenKbd, root.path());
    ASSERT_TRUE(b);
    ASSERT_TRUE(b->write_zones({Rgb{1, 2, 3}, Rgb{255, 0, 0}, Rgb{0, 255, 0}, Rgb{0, 0, 255}}));
    EXPECT_EQ(read_file(root.path() / kHp / "zones"), "010203 FF0000 00FF00 0000FF\n");
}

TEST(Backend, HpBacklightRoundtrip) {
    TempDir root;
    add_hp(root.path());
    auto b = must_open(BackendKind::HpOmenKbd, root.path());
    ASSERT_TRUE(b);
    EXPECT_EQ(b->read_backlight().value(), true);
    ASSERT_TRUE(b->set_backlight(false));
    EXPECT_EQ(read_file(root.path() / kHp / "backlight"), "0\n");
    EXPECT_EQ(b->read_backlight().value(), false);
}

TEST(Backend, MainlineWritesEachLed) {
    TempDir root;
    add_mainline(root.path());
    auto b = must_open(BackendKind::MainlineLed, root.path());
    ASSERT_TRUE(b);
    ASSERT_TRUE(b->write_zones({Rgb{1, 2, 3}, kBlack, kBlack, kWhite}));
    EXPECT_EQ(read_file(root.path() / kLeds[0] / "multi_intensity"), "1 2 3\n");
    EXPECT_EQ(read_file(root.path() / kLeds[3] / "multi_intensity"), "255 255 255\n");
}

TEST(Backend, MainlineBacklightWritesEveryZone) {
    TempDir root;
    add_mainline(root.path());
    auto b = must_open(BackendKind::MainlineLed, root.path());
    ASSERT_TRUE(b);
    ASSERT_TRUE(b->set_backlight(false));
    for (const char *d : kLeds) {
        EXPECT_EQ(read_file(root.path() / d / "brightness"), "0\n");
    }
    EXPECT_EQ(b->read_backlight().value(), false);
    ASSERT_TRUE(b->set_backlight(true));
    EXPECT_EQ(read_file(root.path() / kLeds[2] / "brightness"), "255\n");
    EXPECT_EQ(b->read_backlight().value(), true);
}

TEST(Backend, AutoSelection) {
    TempDir hp, mainline, empty, both;
    add_hp(hp.path());
    add_mainline(mainline.path());
    add_hp(both.path());
    add_mainline(both.path());
    EXPECT_EQ(must_open(BackendKind::Auto, hp.path())->name(), "hp-omen-kbd");
    EXPECT_EQ(must_open(BackendKind::Auto, mainline.path())->name(), "mainline-led");
    const auto none = open_backend(BackendKind::Auto, empty.path());
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().kind, BackendError::Kind::Missing);
    EXPECT_FALSE(open_backend(BackendKind::Auto, both.path()).has_value());
}

TEST(Backend, ExplicitBackendMissingIsAnError) {
    TempDir empty;
    const auto hp = open_backend(BackendKind::HpOmenKbd, empty.path());
    ASSERT_FALSE(hp.has_value());
    EXPECT_EQ(hp.error().kind, BackendError::Kind::Missing);
    const auto ml = open_backend(BackendKind::MainlineLed, empty.path());
    ASSERT_FALSE(ml.has_value());
    EXPECT_EQ(ml.error().kind, BackendError::Kind::Missing);
}

TEST(Backend, KindCliNames) {
    EXPECT_EQ(backend_kind_from_name("hp-omen-kbd"), BackendKind::HpOmenKbd);
    EXPECT_EQ(backend_kind_from_name("mainline-led"), BackendKind::MainlineLed);
    EXPECT_EQ(backend_kind_from_name("auto"), BackendKind::Auto);
    EXPECT_EQ(backend_kind_from_name("mock"), BackendKind::Mock);
    EXPECT_EQ(backend_kind_from_name("usb"), std::nullopt);
}

TEST(Backend, ErrnoMapping) {
    const fs::path p = "/x";
    EXPECT_EQ(map_errno(EBUSY, p).kind, BackendError::Kind::Budget);
    EXPECT_EQ(map_errno(EPERM, p).kind, BackendError::Kind::ReadOnly);
    EXPECT_EQ(map_errno(EACCES, p).kind, BackendError::Kind::Permission);
    EXPECT_EQ(map_errno(ENOENT, p).kind, BackendError::Kind::Missing);
    EXPECT_EQ(map_errno(EIO, p).kind, BackendError::Kind::Io);
    EXPECT_EQ((BackendError{BackendError::Kind::Budget, ""}).message(),
              "hourly write budget exhausted");
    EXPECT_EQ((BackendError{BackendError::Kind::Permission, "/x"}).message(),
              "permission denied on /x (are you in group omen-kbd?)");
}

TEST(Backend, MockRecordsAndFailsOnDemand) {
    auto [b, h] = make_mock(false);
    ASSERT_TRUE(b->set_backlight(true));
    h.fail_with(BackendError{BackendError::Kind::Budget, ""});
    const auto r = b->write_zones({kBlack, kBlack, kBlack, kBlack});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BackendError::Kind::Budget);
    EXPECT_EQ(h.ops(), std::vector<MockOp>{MockOp::backlight_op(true)});
    h.fail_with(std::nullopt);
    ASSERT_TRUE(b->write_zones({kWhite, kWhite, kWhite, kWhite}));
    EXPECT_EQ(h.ops(), (std::vector<MockOp>{MockOp::backlight_op(true),
                                            MockOp::zones_op({kWhite, kWhite, kWhite, kWhite})}));
}

TEST(Backend, MockSimulatesFnKeyAndDelay) {
    auto [b, h] = make_mock(false);
    EXPECT_EQ(b->read_backlight().value(), true) << "initially on";
    ASSERT_TRUE(b->set_backlight(false));
    EXPECT_EQ(b->read_backlight().value(), false) << "follows the last set_backlight";
    h.set_hw_backlight(true);
    EXPECT_EQ(b->read_backlight().value(), true) << "Fn key turned it back on";
    h.set_delay(std::chrono::milliseconds(30));
    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(b->write_zones({kBlack, kBlack, kBlack, kBlack}));
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(30));
}
