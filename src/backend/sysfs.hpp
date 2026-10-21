#pragma once
// Internal: sysfs attribute I/O and the two real backends' factories.

#include "backend/backend.hpp"

namespace omenkbd::sysfs {

/// Writes an attribute in one write(2). Never creates files (no O_CREAT).
std::expected<void, BackendError> write_attr(const std::filesystem::path &p,
                                             std::string_view content);

std::expected<std::string, BackendError> read_attr(const std::filesystem::path &p);

bool hp_omen_kbd_present(const std::filesystem::path &sys_root);
std::expected<std::unique_ptr<Backend>, BackendError>
open_hp_omen_kbd(const std::filesystem::path &sys_root);

bool mainline_led_present(const std::filesystem::path &sys_root);
std::expected<std::unique_ptr<Backend>, BackendError>
open_mainline_led(const std::filesystem::path &sys_root);

} // namespace omenkbd::sysfs
