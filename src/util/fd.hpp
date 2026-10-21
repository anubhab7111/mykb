#pragma once

#include <utility>

namespace omenkbd {

/// Move-only owner of a file descriptor; closes it on destruction.
class UniqueFd {
  public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}
    UniqueFd &operator=(UniqueFd &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }

    /// Gives up ownership without closing.
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

    /// Closes the current descriptor (if any) and takes ownership of `fd`.
    void reset(int fd = -1) noexcept;

    explicit operator bool() const noexcept { return fd_ >= 0; }

  private:
    int fd_;
};

} // namespace omenkbd
