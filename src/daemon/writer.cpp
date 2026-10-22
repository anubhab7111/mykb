#include "daemon/writer.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

namespace omenkbd {

Writer::Writer(std::unique_ptr<Backend> backend)
    : backend_(std::move(backend)), notify_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      thread_([this] { run(); }) {}

Writer::~Writer() {
    {
        const std::lock_guard lock(mu_);
        stop_ = true;
    }
    cv_.notify_one();
    thread_.join();
}

void Writer::submit(const WriteJob &job) {
    {
        const std::lock_guard lock(mu_);
        // The reason to force a write does not go away when a newer frame replaces the job.
        const bool force = job.force || (job_ && job_->force);
        const bool force_backlight = job.force_backlight || (job_ && job_->force_backlight);
        job_ = job;
        job_->force = force;
        job_->force_backlight = force_backlight;
    }
    cv_.notify_one();
}

void Writer::probe() {
    {
        const std::lock_guard lock(mu_);
        probe_ = true;
    }
    cv_.notify_one();
}

std::vector<WriterMsg> Writer::drain() {
    // Reset the counter before taking the messages: one pushed in between re-arms the eventfd.
    std::uint64_t count = 0;
    [[maybe_unused]] const auto n = ::read(notify_.get(), &count, sizeof(count));
    std::vector<WriterMsg> out;
    const std::lock_guard lock(mu_);
    out.swap(outbox_);
    return out;
}

void Writer::run() {
    std::unique_lock lock(mu_);
    for (;;) {
        cv_.wait(lock, [this] { return stop_ || job_ || probe_; });
        if (stop_) {
            return;
        }
        // A probe goes first: probes are rare (every 3 s at most), while a fast effect can keep
        // the job slot full, and the probe is what lets the Fn key pause that effect.
        const bool probing = std::exchange(probe_, false);
        const std::optional<WriteJob> job =
            probing ? std::nullopt : std::exchange(job_, std::nullopt);
        lock.unlock();
        // Without a probe the wait returned for a job, so `job` is set here.
        WriterMsg msg = job ? WriterMsg(WriteOutcome{job->generation, apply(*job)})
                            : WriterMsg(BacklightProbe{backend_->read_backlight()});
        lock.lock();
        outbox_.push_back(std::move(msg));
        const std::uint64_t one = 1;
        [[maybe_unused]] const auto n = ::write(notify_.get(), &one, sizeof(one));
    }
}

std::expected<bool, BackendError> Writer::apply(const WriteJob &job) {
    // Forget what the hardware is believed to show wherever it may have changed; the memory stays
    // empty until a write succeeds, so a forced zone rewrite waits for the backlight to come on.
    if (job.force) {
        zones_.reset();
    }
    if (job.force || job.force_backlight) {
        backlight_.reset();
    }
    // "Off" is the firmware backlight only: the color table is never touched.
    if (!job.backlight) {
        if (backlight_ != false) {
            if (auto r = backend_->set_backlight(false); !r) {
                return std::unexpected(r.error());
            }
            backlight_ = false;
        }
        return false;
    }
    if (backlight_ != true) {
        if (auto r = backend_->set_backlight(true); !r) {
            return std::unexpected(r.error());
        }
        backlight_ = true;
    }
    if (zones_ != job.hw) {
        if (auto r = backend_->write_zones(job.hw); !r) {
            return std::unexpected(r.error());
        }
        zones_ = job.hw;
        return true;
    }
    return false;
}

} // namespace omenkbd
