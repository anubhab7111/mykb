#pragma once
// Test-only helpers: temporary directories, file I/O and polling.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace omenkbd::test {

/// A fresh directory under $TMPDIR (or /tmp), removed recursively on destruction.
class TempDir {
  public:
    TempDir() {
        const char *base = std::getenv("TMPDIR");
        std::string tmpl =
            std::string(base != nullptr && *base != '\0' ? base : "/tmp") + "/omen-kbd-test-XXXXXX";
        if (::mkdtemp(tmpl.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = tmpl;
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    [[nodiscard]] const std::filesystem::path &path() const { return path_; }

  private:
    std::filesystem::path path_;
};

/// Writes `content` to `p`, creating parent directories.
inline void write_file(const std::filesystem::path &p, std::string_view content) {
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f) {
        throw std::runtime_error("cannot write " + p.string());
    }
}

inline std::string read_file(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

/// Polls `cond` every 10 ms until it holds or `timeout` passes; returns the final result.
inline bool wait_until(const std::function<bool()> &cond, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

} // namespace omenkbd::test
