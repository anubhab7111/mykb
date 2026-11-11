#include "sources/hyprctl_query.hpp"

#include "core/window_address.hpp"
#include "ipc/socket.hpp"
#include "util/utf8.hpp"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include <nlohmann/json.hpp>

namespace omenkbd {

namespace {

using namespace std::chrono_literals;
using nlohmann::json;

constexpr Duration kDebounce = 100ms;
constexpr Duration kTimeout = 300ms;
/// A window's JSON is well under 1 KiB; anything this large is not Hyprland answering.
constexpr std::size_t kMaxReply = std::size_t{64} * 1024;
constexpr std::string_view kCommand = "j/activewindow";

/// The reply as a document, or a discarded value when it is not JSON. A title can hold bytes that
/// are not UTF-8, and nlohmann refuses the whole document for one, so they are replaced first
/// (T15-1): the title is not read, and the rest of the answer must not be lost to it.
json parse_reply(std::string_view reply) { return json::parse(scrub_utf8(reply), nullptr, false); }

/// The string at `key`, or nullopt when it is missing or not a string.
std::optional<std::string> string_at(const json &obj, const char *key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

/// The window an already parsed reply describes, or nullopt when it is not a window.
std::optional<ActiveWindowInfo> window_from(const json &doc) {
    if (!doc.is_object()) {
        return std::nullopt;
    }
    const auto address = string_at(doc, "address");
    const auto app_class = string_at(doc, "class");
    const auto mode = doc.find("fullscreen");
    // nlohmann keeps a non-negative integer as unsigned, so this refuses negatives, fractions,
    // strings and anything past an int in one test.
    if (!address || !app_class || mode == doc.end() || !mode->is_number_unsigned() ||
        mode->get<std::uint64_t>() > static_cast<std::uint64_t>(INT_MAX)) {
        return std::nullopt;
    }
    std::string normalized = normalize_window_address(*address);
    if (normalized.empty()) {
        return std::nullopt;
    }
    return ActiveWindowInfo{std::move(normalized), *app_class, mode->get<int>()};
}

/// What a finished reply means: the window, `ActiveWindowInfo{}` for "nothing is focused" (Hyprland
/// answers `{}`), nullopt for anything else. An empty reply is not "nothing focused", it is a
/// hiccup (P23): it parses to nothing and so fails.
std::optional<ActiveWindowInfo> interpret(std::string_view reply) {
    const json doc = parse_reply(reply);
    if (doc.is_object() && doc.empty()) {
        return ActiveWindowInfo{};
    }
    return window_from(doc);
}

} // namespace

std::optional<ActiveWindowInfo> parse_active_window(std::string_view json_text) {
    return window_from(parse_reply(json_text));
}

HyprctlQuery::HyprctlQuery(EventLoop &loop, Result on_result)
    : loop_(loop), on_result_(std::move(on_result)) {}

HyprctlQuery::~HyprctlQuery() { cancel(); }

void HyprctlQuery::request(const std::filesystem::path &request_socket) {
    cancel(); // whatever is waiting or in flight answers an older question
    socket_ = request_socket;
    debounce_ = loop_.at(Clock::now() + kDebounce, [this] {
        debounce_.reset();
        start();
    });
}

void HyprctlQuery::cancel() {
    if (debounce_) {
        loop_.cancel(*debounce_);
        debounce_.reset();
    }
    if (timeout_) {
        loop_.cancel(*timeout_);
        timeout_.reset();
    }
    if (fd_) {
        loop_.unwatch(fd_.get());
        fd_.reset();
    }
    reply_.clear();
}

void HyprctlQuery::start() {
    fd_ = connect_nonblocking(socket_);
    if (!fd_) {
        finish(std::nullopt);
        return;
    }
    // The command is a few bytes into an empty socket buffer, so this writes it whole; anything
    // else is an error, not a reason to wait for the socket to drain.
    const ssize_t n = ::send(fd_.get(), kCommand.data(), kCommand.size(), MSG_NOSIGNAL);
    if (std::cmp_not_equal(n, kCommand.size())) {
        finish(std::nullopt);
        return;
    }
    loop_.watch(fd_.get(), EPOLLIN, [this](std::uint32_t) { on_readable(); });
    timeout_ = loop_.at(Clock::now() + kTimeout, [this] {
        timeout_.reset();
        finish(std::nullopt);
    });
}

void HyprctlQuery::on_readable() {
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd_.get(), buf, sizeof(buf));
        if (n > 0) {
            reply_.append(buf, static_cast<std::size_t>(n));
            if (reply_.size() > kMaxReply) {
                finish(std::nullopt);
                return;
            }
            continue;
        }
        if (n == 0) {
            finish(interpret(reply_)); // Hyprland closes the connection after its reply
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; // the rest of the reply is not here yet
        }
        finish(std::nullopt);
        return;
    }
}

void HyprctlQuery::finish(std::optional<ActiveWindowInfo> result) {
    cancel(); // let go of the socket and the timers first: the callback may `request` again
    on_result_(std::move(result));
}

} // namespace omenkbd
