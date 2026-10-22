#include "state.hpp"

#include "config.hpp"
#include "util/fd.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace omenkbd {

namespace {

using nlohmann::json;

std::filesystem::path with_suffix(const std::filesystem::path &p, std::string_view suffix) {
    return std::filesystem::path(p.string() + std::string(suffix));
}

json to_json(const PersistedState &s) {
    json j;
    j["active_profile"] = s.active_profile;
    j["custom"] = s.custom ? json(*s.custom) : json(nullptr);
    j["effect_override"] = s.effect_override
                               ? json{{"effect", std::string(name(s.effect_override->effect))},
                                      {"speed", s.effect_override->speed}}
                               : json(nullptr);
    j["brightness"] = s.brightness;
    j["on"] = s.on;
    j["lifetime_writes"] = s.lifetime_writes;
    return j;
}

/// Throws nlohmann::json exceptions on wrong types (the caller treats that as corrupt).
PersistedState from_json(const json &j, std::string_view first_profile) {
    PersistedState s = PersistedState::default_for(first_profile);
    if (!j.is_object()) {
        throw std::runtime_error("state is not a JSON object");
    }
    s.active_profile = j.value("active_profile", std::string());
    if (s.active_profile.empty()) {
        s.active_profile = std::string(first_profile);
    }
    if (j.contains("custom") && !j["custom"].is_null()) {
        s.custom = j["custom"].get<std::vector<std::string>>();
    }
    if (j.contains("effect_override") && !j["effect_override"].is_null()) {
        const json &o = j["effect_override"];
        const auto e = effect_from_name(o.at("effect").get<std::string>());
        if (!e) {
            throw std::runtime_error("unknown effect in effect_override");
        }
        s.effect_override = EffectOverride{*e, o.at("speed").get<std::uint8_t>()};
    }
    s.brightness = static_cast<std::uint8_t>(std::clamp(j.value("brightness", 100), 0, 100));
    s.on = j.value("on", true);
    s.lifetime_writes = j.value("lifetime_writes", std::uint64_t{0});
    return s;
}

} // namespace

PersistedState PersistedState::default_for(std::string_view first_profile) {
    PersistedState s;
    s.active_profile = std::string(first_profile);
    return s;
}

std::pair<PersistedState, std::optional<std::string>>
PersistedState::load(const std::filesystem::path &path, std::string_view first_profile) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {default_for(first_profile), std::nullopt};
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {default_for(first_profile), std::format("cannot read {}", path.string())};
    }
    const std::string text{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    f.close();
    try {
        return {from_json(json::parse(text), first_profile), std::nullopt};
    } catch (const std::exception &e) {
        const auto bad = with_suffix(path, ".bad");
        std::filesystem::rename(path, bad, ec);
        const std::string note = ec ? std::string("could not move it aside")
                                    : std::format("moved it to {}", bad.string());
        return {default_for(first_profile),
                std::format("corrupt state file {} ({}); {}; using defaults", path.string(),
                            e.what(), note)};
    }
}

std::expected<void, std::string> PersistedState::save(const std::filesystem::path &path) const {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return std::unexpected(
                std::format("{}: {}", path.parent_path().string(), ec.message()));
        }
    }
    const auto tmp = with_suffix(path, ".tmp");
    // Invalid UTF-8 becomes U+FFFD: save() runs in a daemon timer and must not throw.
    const std::string text =
        to_json(*this).dump(2, ' ', false, json::error_handler_t::replace) + "\n";
    {
        UniqueFd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
        if (!fd) {
            return std::unexpected(std::format("{}: {}", tmp.string(), std::strerror(errno)));
        }
        std::size_t done = 0;
        while (done < text.size()) {
            const ssize_t n = ::write(fd.get(), text.data() + done, text.size() - done);
            if (n < 0) {
                return std::unexpected(std::format("{}: {}", tmp.string(), std::strerror(errno)));
            }
            done += static_cast<std::size_t>(n);
        }
        if (::fsync(fd.get()) != 0) {
            return std::unexpected(std::format("{}: {}", tmp.string(), std::strerror(errno)));
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        return std::unexpected(std::format("{}: {}", path.string(), ec.message()));
    }
    return {};
}

std::filesystem::path state_path() {
    const char *xdg = std::getenv("XDG_STATE_HOME");
    const std::filesystem::path base = (xdg != nullptr && *xdg != '\0')
                                           ? std::filesystem::path(xdg)
                                           : expand_home("~/.local/state");
    return base / "omen-kbd/state.json";
}

} // namespace omenkbd
