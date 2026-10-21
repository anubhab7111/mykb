#include "sources/hwmon.hpp"

#include "util/strings.hpp"

#include <charconv>
#include <fstream>
#include <iterator>
#include <string>

namespace omenkbd {

namespace {

using namespace std::chrono_literals;

constexpr auto kEvery = 2s;

std::optional<std::string> read_trimmed(const std::filesystem::path &p) {
    std::ifstream f(p);
    if (!f) {
        return std::nullopt;
    }
    const std::string s{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    return std::string(trim(s));
}

} // namespace

std::optional<std::filesystem::path> find_package_temp(const std::filesystem::path &sys_root) {
    std::error_code ec;
    for (const auto &hw : std::filesystem::directory_iterator(sys_root / "class/hwmon", ec)) {
        if (read_trimmed(hw.path() / "name") != "coretemp") {
            continue;
        }
        for (const auto &f : std::filesystem::directory_iterator(hw.path(), ec)) {
            const std::string file = f.path().filename().string();
            if (!file.starts_with("temp") || !file.ends_with("_label") ||
                read_trimmed(f.path()) != "Package id 0") {
                continue;
            }
            const std::string sensor =
                file.substr(0, file.size() - std::string_view("_label").size());
            return hw.path() / (sensor + "_input");
        }
    }
    return std::nullopt;
}

std::optional<double> read_temp_c(const std::filesystem::path &p) {
    const auto s = read_trimmed(p);
    if (!s) {
        return std::nullopt;
    }
    long milli = 0;
    const auto [ptr, ec] = std::from_chars(s->data(), s->data() + s->size(), milli);
    if (ec != std::errc{} || ptr != s->data() + s->size()) {
        return std::nullopt;
    }
    return static_cast<double>(milli) / 1000.0;
}

HwmonPoller::HwmonPoller(EventLoop &loop, std::filesystem::path sys_root, Emit emit)
    : loop_(loop), sys_root_(std::move(sys_root)), emit_(std::move(emit)) {
    timer_ = loop_.at(Clock::now(), [this] { tick(); });
}

HwmonPoller::~HwmonPoller() {
    if (timer_) {
        loop_.cancel(*timer_);
    }
}

void HwmonPoller::tick() {
    if (!input_) {
        input_ = find_package_temp(sys_root_);
    }
    if (input_) {
        const auto t = read_temp_c(*input_);
        if (!t) {
            input_.reset(); // sensor gone; look it up again next time
        } else if (t != last_) {
            last_ = t;
            emit_(Event{FactUpdate{fact::CpuTemp{*t}}});
        }
    }
    timer_ = loop_.at(Clock::now() + kEvery, [this] { tick(); });
}

} // namespace omenkbd
