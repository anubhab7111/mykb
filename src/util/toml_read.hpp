#pragma once
// Strict, exception-free helpers for reading toml++ tables: unknown keys are errors, and every
// error names the offending field.

#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <type_traits>

namespace omenkbd::tomlr {

/// Error ``unknown field `<key>` `` (prefixed with `where`) for any key not in `allowed`.
inline std::expected<void, std::string> check_keys(const toml::table &t,
                                                   std::initializer_list<std::string_view> allowed,
                                                   std::string_view where) {
    for (const auto &[key, node] : t) {
        bool known = false;
        for (std::string_view a : allowed) {
            known = known || key.str() == a;
        }
        if (!known) {
            return std::unexpected(where.empty()
                                       ? std::format("unknown field `{}`", key.str())
                                       : std::format("{}: unknown field `{}`", where, key.str()));
        }
    }
    return {};
}

/// Optional value of type T (`std::string`, `bool`, `double` (integers accepted) or
/// `std::int64_t`). Missing ⇒ nullopt; present with the wrong type ⇒ error.
template <class T>
std::expected<std::optional<T>, std::string> opt(const toml::table &t, std::string_view key,
                                                 std::string_view where) {
    const toml::node *n = t.get(key);
    if (n == nullptr) {
        return std::optional<T>{};
    }
    const char *kind = "a value";
    std::optional<T> v;
    if constexpr (std::is_same_v<T, std::string>) {
        kind = "a string";
        if (n->is_string()) {
            v = n->value<std::string>();
        }
    } else if constexpr (std::is_same_v<T, bool>) {
        kind = "true or false";
        if (n->is_boolean()) {
            v = n->value<bool>();
        }
    } else if constexpr (std::is_same_v<T, double>) {
        kind = "a number";
        if (n->is_number()) {
            v = n->value<double>();
        }
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
        kind = "an integer";
        if (n->is_integer()) {
            v = n->value<std::int64_t>();
        }
    } else {
        static_assert(!sizeof(T), "unsupported TOML value type");
    }
    if (!v) {
        return std::unexpected(where.empty()
                                   ? std::format("`{}` must be {}", key, kind)
                                   : std::format("{}: `{}` must be {}", where, key, kind));
    }
    return v;
}

/// Optional integer in [lo, hi].
inline std::expected<std::optional<std::int64_t>, std::string>
opt_int_in(const toml::table &t, std::string_view key, std::string_view where, std::int64_t lo,
           std::int64_t hi) {
    auto v = opt<std::int64_t>(t, key, where);
    if (v && *v && (**v < lo || **v > hi)) {
        return std::unexpected(
            where.empty() ? std::format("`{}` must be {}-{}, got {}", key, lo, hi, **v)
                          : std::format("{}: `{}` must be {}-{}, got {}", where, key, lo, hi, **v));
    }
    return v;
}

} // namespace omenkbd::tomlr
