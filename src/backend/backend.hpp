#pragma once
// Hardware backends. The daemon talks to the keyboard only through Backend; the writer thread is
// its sole owner. Real backends write sysfs attributes under a configurable `sys_root` so tests
// can use fake trees.

#include "core/color.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace omenkbd {

struct BackendError {
    enum class Kind { Budget, ReadOnly, Permission, Missing, Io };
    Kind kind;
    std::string detail;

    /// Same texts as the Rust prototype's error messages.
    [[nodiscard]] std::string message() const;

    friend bool operator==(const BackendError &, const BackendError &) = default;
};

/// A keyboard backend. Colors are in **hardware** zone order.
class Backend {
  public:
    virtual ~Backend() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    virtual std::expected<void, BackendError> write_zones(const std::array<Rgb, 4> &hw) = 0;
    virtual std::expected<void, BackendError> set_backlight(bool on) = 0;
    virtual std::expected<bool, BackendError> read_backlight() = 0;
};

/// Backend selection. `Auto` only ever picks a real backend, never the mock.
enum class BackendKind { Auto, Mock, HpOmenKbd, MainlineLed };

/// `auto | mock | hp-omen-kbd | mainline-led`.
std::optional<BackendKind> backend_kind_from_name(std::string_view s);

std::expected<std::unique_ptr<Backend>, BackendError>
open_backend(BackendKind kind, const std::filesystem::path &sys_root);

/// Maps the errnos the kernel module uses onto backend errors.
BackendError map_errno(int err, const std::filesystem::path &path);

} // namespace omenkbd
