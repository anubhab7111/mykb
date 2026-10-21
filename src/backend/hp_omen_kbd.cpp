// Our own kernel module's sysfs interface: one write sets all four zones atomically.

#include "backend/sysfs.hpp"
#include "util/strings.hpp"

#include <format>

namespace omenkbd::sysfs {

namespace {

constexpr std::string_view kBase = "devices/platform/hp-omen-kbd";

class HpOmenKbd final : public Backend {
  public:
    explicit HpOmenKbd(std::filesystem::path dir) : dir_(std::move(dir)) {}

    [[nodiscard]] std::string_view name() const override { return "hp-omen-kbd"; }

    std::expected<void, BackendError> write_zones(const std::array<Rgb, 4> &hw) override {
        return write_attr(dir_ / "zones",
                          std::format("{} {} {} {}\n", hw[0].to_hex(), hw[1].to_hex(),
                                      hw[2].to_hex(), hw[3].to_hex()));
    }

    std::expected<void, BackendError> set_backlight(bool on) override {
        return write_attr(dir_ / "backlight", on ? "1\n" : "0\n");
    }

    std::expected<bool, BackendError> read_backlight() override {
        const auto path = dir_ / "backlight";
        auto text = read_attr(path);
        if (!text) {
            return std::unexpected(text.error());
        }
        const std::string_view v = trim(*text);
        if (v == "1") {
            return true;
        }
        if (v == "0") {
            return false;
        }
        return std::unexpected(BackendError{
            BackendError::Kind::Io, std::format("{}: unexpected value \"{}\"", path.string(), v)});
    }

  private:
    std::filesystem::path dir_;
};

} // namespace

bool hp_omen_kbd_present(const std::filesystem::path &sys_root) {
    std::error_code ec;
    return std::filesystem::is_directory(sys_root / kBase, ec);
}

std::expected<std::unique_ptr<Backend>, BackendError>
open_hp_omen_kbd(const std::filesystem::path &sys_root) {
    const auto dir = sys_root / kBase;
    if (!hp_omen_kbd_present(sys_root)) {
        return std::unexpected(BackendError{
            BackendError::Kind::Missing, std::format("{} (is hp_omen_kbd loaded?)", dir.string())});
    }
    return std::make_unique<HpOmenKbd>(dir);
}

} // namespace omenkbd::sysfs
