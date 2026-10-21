// The upstream hp-wmi multicolor LED interface (pending patch series): one LED per zone,
// hp::kbd_zoned_backlight-{right,center,left,wasd} = hardware zones 0..3. Four writes per frame.

#include "backend/sysfs.hpp"
#include "util/strings.hpp"

#include <charconv>
#include <format>

namespace omenkbd::sysfs {

namespace {

constexpr std::array<std::string_view, 4> kZoneDirs{
    "class/leds/hp::kbd_zoned_backlight-right", "class/leds/hp::kbd_zoned_backlight-center",
    "class/leds/hp::kbd_zoned_backlight-left", "class/leds/hp::kbd_zoned_backlight-wasd"};

class MainlineLed final : public Backend {
  public:
    explicit MainlineLed(const std::filesystem::path &sys_root) {
        for (std::size_t i = 0; i < 4; ++i) {
            dirs_[i] = sys_root / kZoneDirs[i];
        }
    }

    [[nodiscard]] std::string_view name() const override { return "mainline-led"; }

    std::expected<void, BackendError> write_zones(const std::array<Rgb, 4> &hw) override {
        for (std::size_t i = 0; i < 4; ++i) {
            auto r = write_attr(dirs_[i] / "multi_intensity",
                                std::format("{} {} {}\n", hw[i].r, hw[i].g, hw[i].b));
            if (!r) {
                return r;
            }
        }
        return {};
    }

    std::expected<void, BackendError> set_backlight(bool on) override {
        for (const auto &dir : dirs_) {
            auto r = write_attr(dir / "brightness", on ? "255\n" : "0\n");
            if (!r) {
                return r;
            }
        }
        return {};
    }

    std::expected<bool, BackendError> read_backlight() override {
        for (const auto &dir : dirs_) {
            const auto path = dir / "brightness";
            auto text = read_attr(path);
            if (!text) {
                return std::unexpected(text.error());
            }
            const std::string_view v = trim(*text);
            unsigned value = 0;
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), value);
            if (ec != std::errc{} || ptr != v.data() + v.size()) {
                return std::unexpected(BackendError{
                    BackendError::Kind::Io, std::format("{}: not a number", path.string())});
            }
            if (value > 0) {
                return true;
            }
        }
        return false;
    }

  private:
    std::array<std::filesystem::path, 4> dirs_;
};

} // namespace

bool mainline_led_present(const std::filesystem::path &sys_root) {
    std::error_code ec;
    for (std::string_view d : kZoneDirs) {
        if (!std::filesystem::is_directory(sys_root / d, ec)) {
            return false;
        }
    }
    return true;
}

std::expected<std::unique_ptr<Backend>, BackendError>
open_mainline_led(const std::filesystem::path &sys_root) {
    if (!mainline_led_present(sys_root)) {
        return std::unexpected(BackendError{
            BackendError::Kind::Missing,
            std::format("{}/hp::kbd_zoned_backlight-* (kernel without HP keyboard LED support)",
                        (sys_root / "class/leds").string())});
    }
    return std::make_unique<MainlineLed>(sys_root);
}

} // namespace omenkbd::sysfs
