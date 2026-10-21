#include "core/palette.hpp"

#include "util/strings.hpp"

#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <toml++/toml.hpp>

namespace omenkbd {

namespace {

std::optional<std::string> read_text(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());
}

} // namespace

Palette Palette::from_waybar_css(std::string_view css) {
  Palette p;
  std::istringstream lines{std::string(css)};
  for (std::string line; std::getline(lines, line);) {
    std::string_view rest = trim(line);
    constexpr std::string_view kPrefix = "@define-color";
    if (!rest.starts_with(kPrefix)) {
      continue;
    }
    std::istringstream words{std::string(rest.substr(kPrefix.size()))};
    std::string name, value;
    if (!(words >> name >> value)) {
      continue;
    }
    while (value.ends_with(';')) {
      value.pop_back();
    }
    if (auto c = Rgb::parse(value)) {
      p.colors_.insert_or_assign(name, *c);
    }
  }
  return p;
}

std::expected<Palette, std::string>
Palette::from_theme_toml(std::string_view text) {
  toml::table root;
  try {
    root = toml::parse(text);
  } catch (const toml::parse_error &e) {
    return std::unexpected(std::string(e.description()));
  }
  Palette p;
  for (const auto &[key, node] : root) {
    if (key.str() != "palette") {
      return std::unexpected(std::format("unknown field `{}`", key.str()));
    }
    const toml::table *colors = node.as_table();
    if (colors == nullptr) {
      return std::unexpected(std::string("palette must be a table"));
    }
    for (const auto &[name, value] : *colors) {
      const auto s = value.value<std::string>();
      if (!s) {
        return std::unexpected(
            std::format("palette.{}: expected a color string", name.str()));
      }
      auto c = Rgb::parse(*s);
      if (!c) {
        return std::unexpected(
            std::format("palette.{}: {}", name.str(), c.error()));
      }
      p.colors_.insert_or_assign(std::string(name.str()), *c);
    }
  }
  return p;
}

std::expected<Palette, std::string>
Palette::load(const std::filesystem::path &theme_dir) {
  const auto toml_path = theme_dir / "omen-kbd.toml";
  std::error_code ec;
  if (std::filesystem::exists(toml_path, ec)) {
    const auto text = read_text(toml_path);
    if (!text) {
      return std::unexpected(
          std::format("{}: cannot read", toml_path.string()));
    }
    auto p = from_theme_toml(*text);
    if (!p) {
      return std::unexpected(
          std::format("{}: {}", toml_path.string(), p.error()));
    }
    return p;
  }
  const auto css_path = theme_dir / "waybar-colors.css";
  const auto css = read_text(css_path);
  if (!css) {
    return std::unexpected(
        std::format("no theme palette in {}", theme_dir.string()));
  }
  return from_waybar_css(*css);
}

std::optional<Rgb> Palette::get(std::string_view name) const {
  const auto it = colors_.find(name);
  if (it == colors_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<ThemeColors> theme_colors_from_name(std::string_view s) {
  const std::string lower = to_lower(s);
  if (lower == "vivid") {
    return ThemeColors::Vivid;
  }
  if (lower == "exact") {
    return ThemeColors::Exact;
  }
  return std::nullopt;
}

std::expected<ColorRef, std::string> ColorRef::parse(std::string_view s) {
  constexpr std::string_view kTheme = "theme:";
  if (s.starts_with(kTheme)) {
    const std::string_view name = trim(s.substr(kTheme.size()));
    if (name.empty()) {
      return std::unexpected(
          std::format("invalid color \"{}\": theme color needs a name", s));
    }
    return ColorRef(std::string(name));
  }
  if (auto c = Rgb::parse(s)) {
    return ColorRef(*c);
  }
  return std::unexpected(
      std::format("invalid color \"{}\": expected #RRGGBB or theme:<name>", s));
}

std::string ColorRef::to_string() const {
  if (const auto *c = std::get_if<Rgb>(&v_)) {
    return "#" + c->to_hex();
  }
  return "theme:" + std::get<std::string>(v_);
}

std::expected<Rgb, std::string> ColorRef::resolve(const Palette &p,
                                                  ThemeColors mode) const {
  if (const auto *c = std::get_if<Rgb>(&v_)) {
    return *c;
  }
  const auto &name = std::get<std::string>(v_);
  const auto c = p.get(name);
  if (!c) {
    return std::unexpected(name);
  }
  return mode == ThemeColors::Vivid ? c->vivid() : *c;
}

} // namespace omenkbd
