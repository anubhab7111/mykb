#include "helpers.hpp"

#include "config_example.hpp"
#include "util/fd.hpp"

#include <cerrno>
#include <fcntl.h>
#include <gtest/gtest.h>

using namespace omenkbd;
using namespace omenkbd::test;

TEST(Util, UniqueFdClosesOnDestruction) {
    int raw = -1;
    {
        UniqueFd fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        ASSERT_TRUE(fd);
        raw = fd.get();
    }
    errno = 0;
    EXPECT_EQ(::fcntl(raw, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(Util, UniqueFdMoveTransfersOwnership) {
    UniqueFd a(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    const int raw = a.get();
    UniqueFd b(std::move(a));
    EXPECT_EQ(b.get(), raw);
    EXPECT_NE(::fcntl(raw, F_GETFD), -1) << "still open, now owned by b";
    UniqueFd c;
    c = std::move(b);
    EXPECT_EQ(c.get(), raw);
}

TEST(Util, TempDirIsRemoved) {
    std::filesystem::path p;
    {
        TempDir d;
        p = d.path();
        write_file(p / "a/b.txt", "x");
        EXPECT_EQ(read_file(p / "a/b.txt"), "x");
    }
    EXPECT_FALSE(std::filesystem::exists(p));
}

TEST(Util, EmbeddedExampleConfigMatchesFile) {
    const auto file =
        read_file(std::filesystem::path(OMEN_KBD_SOURCE_DIR) / "packaging/config.example.toml");
    EXPECT_FALSE(file.empty());
    EXPECT_EQ(std::string(kConfigExample), file);
}
