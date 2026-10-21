#pragma once
// The writer thread: the only owner of the backend. Jobs land in a one-slot mailbox, so a burst
// of changes collapses to the latest frame; results come back through an outbox whose eventfd
// the daemon's loop watches.

#include "backend/backend.hpp"
#include "util/fd.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace omenkbd {

/// A frame to apply, in hardware zone order.
struct WriteJob {
    std::uint64_t generation;
    bool backlight;
    std::array<Rgb, 4> hw;
    /// Write the zones even if they equal the last written frame (e.g. after resume).
    bool force;
    friend bool operator==(const WriteJob &, const WriteJob &) = default;
};

struct WriteOutcome {
    std::uint64_t generation;
    /// `true` = zones were written; `false` = nothing (or only the backlight) changed.
    std::expected<bool, BackendError> result;
};

/// What the firmware reports for the backlight (the Fn key may have changed it).
struct BacklightProbe {
    std::expected<bool, BackendError> result;
};

using WriterMsg = std::variant<WriteOutcome, BacklightProbe>;

class Writer {
  public:
    explicit Writer(std::unique_ptr<Backend> backend);
    /// Finishes the backend call in progress (if any), drops a job not yet taken, and joins.
    ~Writer();
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    /// Replaces any job the thread has not taken yet.
    void submit(const WriteJob &job);
    /// Asks for one backlight read (requests made while one is pending collapse into it).
    void probe();
    /// An eventfd, readable while messages wait; -1 if it could not be created.
    [[nodiscard]] int notify_fd() const { return notify_.get(); }
    /// Takes every waiting message (resets the eventfd).
    std::vector<WriterMsg> drain();

  private:
    void run();
    /// Writer thread only.
    std::expected<bool, BackendError> apply(const WriteJob &job);

    std::unique_ptr<Backend> backend_;
    /// What the hardware is believed to show; updated only after a successful call.
    std::optional<bool> backlight_;
    std::optional<std::array<Rgb, 4>> zones_;

    UniqueFd notify_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::optional<WriteJob> job_;
    bool probe_ = false;
    bool stop_ = false;
    std::vector<WriterMsg> outbox_;
    std::thread thread_; // last: starts once everything above exists
};

} // namespace omenkbd
