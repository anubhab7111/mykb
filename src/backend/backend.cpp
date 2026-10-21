#include "backend/backend.hpp"

#include "backend/mock.hpp"
#include "backend/sysfs.hpp"
#include "util/fd.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <format>
#include <unistd.h>

namespace omenkbd {

std::string BackendError::message() const {
    switch (kind) {
    case Kind::Budget:
        return "hourly write budget exhausted";
    case Kind::ReadOnly:
        return "hp_omen_kbd is read-only (readonly=1)";
    case Kind::Permission:
        return std::format("permission denied on {} (are you in group omen-kbd?)", detail);
    case Kind::Missing:
        return std::format("backend missing: {}", detail);
    case Kind::Io:
        return std::format("I/O error: {}", detail);
    }
    return detail;
}

std::optional<BackendKind> backend_kind_from_name(std::string_view s) {
    if (s == "auto") {
        return BackendKind::Auto;
    }
    if (s == "mock") {
        return BackendKind::Mock;
    }
    if (s == "hp-omen-kbd") {
        return BackendKind::HpOmenKbd;
    }
    if (s == "mainline-led") {
        return BackendKind::MainlineLed;
    }
    return std::nullopt;
}

std::expected<std::unique_ptr<Backend>, BackendError>
open_backend(BackendKind kind, const std::filesystem::path &sys_root) {
    switch (kind) {
    case BackendKind::Mock:
        return std::move(make_mock(false).first);
    case BackendKind::HpOmenKbd:
        return sysfs::open_hp_omen_kbd(sys_root);
    case BackendKind::MainlineLed:
        return sysfs::open_mainline_led(sys_root);
    case BackendKind::Auto:
        break;
    }
    const bool hp = sysfs::hp_omen_kbd_present(sys_root);
    const bool ml = sysfs::mainline_led_present(sys_root);
    if (hp && ml) {
        return std::unexpected(BackendError{
            BackendError::Kind::Io,
            "both hp-omen-kbd and mainline LEDs present; remove the hp-omen-kbd DKMS module"});
    }
    if (hp) {
        return sysfs::open_hp_omen_kbd(sys_root);
    }
    if (ml) {
        return sysfs::open_mainline_led(sys_root);
    }
    return std::unexpected(
        BackendError{BackendError::Kind::Missing, "no keyboard backend (is hp_omen_kbd loaded?)"});
}

BackendError map_errno(int err, const std::filesystem::path &path) {
    switch (err) {
    case EBUSY:
        return {BackendError::Kind::Budget, ""};
    case EPERM:
        return {BackendError::Kind::ReadOnly, ""};
    case EACCES:
        return {BackendError::Kind::Permission, path.string()};
    case ENOENT:
        return {BackendError::Kind::Missing, path.string()};
    default:
        return {BackendError::Kind::Io,
                std::format("{}: {}", path.string(), std::system_category().message(err))};
    }
}

namespace sysfs {

std::expected<void, BackendError> write_attr(const std::filesystem::path &p,
                                             std::string_view content) {
    UniqueFd fd(::open(p.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC));
    if (!fd) {
        return std::unexpected(map_errno(errno, p));
    }
    const ssize_t n = ::write(fd.get(), content.data(), content.size());
    if (n < 0) {
        return std::unexpected(map_errno(errno, p));
    }
    if (static_cast<std::size_t>(n) != content.size()) {
        return std::unexpected(
            BackendError{BackendError::Kind::Io, std::format("{}: short write", p.string())});
    }
    return {};
}

std::expected<std::string, BackendError> read_attr(const std::filesystem::path &p) {
    UniqueFd fd(::open(p.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd) {
        return std::unexpected(map_errno(errno, p));
    }
    std::array<char, 4096> buf{};
    const ssize_t n = ::read(fd.get(), buf.data(), buf.size());
    if (n < 0) {
        return std::unexpected(map_errno(errno, p));
    }
    return std::string(buf.data(), static_cast<std::size_t>(n));
}

} // namespace sysfs

} // namespace omenkbd
