#include "util/fd.hpp"

#include <unistd.h>

namespace omenkbd {

void UniqueFd::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

} // namespace omenkbd
