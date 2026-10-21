#pragma once
// Theme palettes (from the rice's theming files) and color references (`#rrggbb` or
// `theme:<name>`).

#include "core/color.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace omenkbd {

/// Named colors of the current desktop theme.
class Palette {
  public:
    /// Parses `@define-color <name> #rrggbb;` lines; anything else is skipped.
    static Palette from_waybar_css(std::string_view css);

    /// Parses an `omen-kbd.toml` theme file: a `[palette]` table of `name = "#rrggbb"`.
    static std::expected<Palette, std::string> from_theme_toml(std::string_view text);

    /// Loads `<theme_dir>/omen-kbd.toml` if present, else `<theme_dir>/waybar-colors.css`.
    static std::expected<Palette, std::string> load(const std::filesystem::path &theme_dir);

    [[nodiscard]] std::optional<Rgb> get(std::string_view name) const;
    [[nodiscard]] std::size_t size() const { return colors_.size(); }
    [[nodiscard]] bool empty() const { return colors_.empty(); }

    friend bool operator==(const Palette &, const Palette &) = default;

  private:
    std::map<std::string, Rgb, std::less<>> colors_;
};

/// How `theme:` colors are rendered: boosted for LEDs (`vivid`) or as the theme defines them.
enum class ThemeColors { Vivid, Exact };

std::optional<ThemeColors> theme_colors_from_name(std::string_view s);

/// A color as written in config: an explicit `#rrggbb` or a `theme:<name>` palette reference.
class ColorRef {
  public:
    static std::expected<ColorRef, std::string> parse(std::string_view s);
    static ColorRef hex(Rgb c) { return ColorRef(c); }
    static ColorRef theme(std::string name) { return ColorRef(std::move(name)); }

    /// `#RRGGBB` or `theme:<name>`.
    [[nodiscard]] std::string to_string() const;

    /// Resolves against the palette; the error carries the missing theme color's name. Vivid
    /// mode applies to theme references only, never to explicit hex colors.
    [[nodiscard]] std::expected<Rgb, std::string> resolve(const Palette &p, ThemeColors mode) const;

    friend bool operator==(const ColorRef &, const ColorRef &) = default;

  private:
    explicit ColorRef(Rgb c) : v_(c) {}
    explicit ColorRef(std::string name) : v_(std::move(name)) {}
    std::variant<Rgb, std::string> v_;
};

} // namespace omenkbd
