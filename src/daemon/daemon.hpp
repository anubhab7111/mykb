#pragma once
// The daemon: IPC clients, system events and writer results drive the pure Model on one epoll
// loop; every change recomputes one frame and hands it to the writer thread.

#include "backend/backend.hpp"
#include "core/signals.hpp"
#include "daemon/event_loop.hpp"
#include "daemon/events.hpp"
#include "daemon/model.hpp"
#include "daemon/writer.hpp"
#include "ipc/socket.hpp"
#include "util/fd.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace omenkbd {

struct RunOptions {
    BackendKind backend = BackendKind::Auto;
    std::filesystem::path sys_root;
    std::filesystem::path config_path;
    std::filesystem::path state_path;
    std::filesystem::path socket_path;
    bool mock_render = false;
    bool with_sources = false;
};

class Daemon {
  public:
    /// Starts the writer thread (which owns `backend`); the loop starts with run().
    Daemon(Model model, std::unique_ptr<Backend> backend, UniqueFd listen_fd, RunOptions opts);
    ~Daemon();
    Daemon(const Daemon &) = delete;
    Daemon &operator=(const Daemon &) = delete;

    /// For extra watches (signalfd, event sources) before run().
    EventLoop &loop() { return loop_; }
    /// Called on the loop thread after a successful `reload` with the new config, so event
    /// sources that depend on it (theme dir, hwmon) can be rebuilt.
    void on_config_replaced(std::function<void(const Config &)> fn) {
        on_config_replaced_ = std::move(fn);
    }
    /// Loop thread only.
    void handle_event(const Event &ev);
    /// Thread-safe.
    void post(Event ev);
    /// Thread-safe: run() saves the state, removes the socket and returns 0.
    void stop() { loop_.stop(); }
    /// Runs until stop(). Single use. Returns 0 on a clean stop, 1 if the loop could not start.
    int run();

  private:
    struct Client {
        UniqueFd fd;
        LineBuffer in;
        std::string out;
        std::uint32_t events = 0;
        /// A request waits for the writer; later lines wait too, so replies stay in order.
        bool awaiting = false;
        bool subscriber = false;
        /// The peer shut down its side for writing (it may still read the reply).
        bool eof = false;
        /// The peer is gone; nothing can be delivered.
        bool hup = false;
    };
    /// A state-changing request waiting for the writer to confirm its frame.
    struct Pending {
        std::uint64_t generation;
        std::uint64_t client;
        TimePoint deadline;
    };
    /// A request answered only after a fresh backlight read (the Fn key may have changed it).
    struct ProbeWaiter {
        std::uint64_t client;
        Request request;
        TimePoint deadline;
    };
    /// A signal delivered only after a fresh backlight read, or when the read times out.
    struct PendingSignal {
        Signal signal;
        TimePoint deadline;
    };

    /// Hands a signal to the model (R3, R4). Off, it is dropped before any read. A signal that
    /// could start a reaction while the backlight reading is stale (older than kBacklightFresh)
    /// waits for a read, and every later signal queues behind it so none overtakes another.
    void deliver_signal(const Signal &s);
    /// Applies the waiting signals in order, then shows them in one frame.
    void flush_pending_signals();
    /// Applies the waiting signals in order and empties the list, with no frame. False if none
    /// waited.
    bool apply_pending_signals();
    /// Shows what a signal changed: at once, unless an effect ticks (Ruling P16), whose next tick
    /// samples the reactions, so a signal adds no write of its own.
    void show_reactions();
    /// When the reaction layers next need a frame: only while no effect ticks (spec 5.7).
    [[nodiscard]] std::optional<TimePoint> reaction_wake() const;

    std::optional<std::uint64_t> recompute();
    [[nodiscard]] Status status() const {
        return model_.status(last_output_ ? &*last_output_ : nullptr);
    }
    void broadcast();
    void schedule_save(Clock::duration within);
    void save();

    void on_accept();
    void on_client(std::uint64_t id, std::uint32_t events);
    void service(std::uint64_t id);
    void handle_line(std::uint64_t id, Client &c, const std::string &line);
    /// `backlight_read`: the fresh backlight read this request waits for has happened (or timed
    /// out), so it is handled now.
    void on_request(std::uint64_t id, Client &c, const Request &r, bool backlight_read = false);
    void release_probe_waiters();
    void respond(std::uint64_t id, const Response &r);
    void close_client(std::uint64_t id);

    void on_writer();
    void arm_wake();
    [[nodiscard]] std::optional<TimePoint> next_wake() const;
    void on_wake();

    Model model_;
    RunOptions opts_;
    EventLoop loop_;
    std::unique_ptr<Writer> writer_;
    UniqueFd listen_;
    TimePoint start_ = Clock::now();

    std::map<std::uint64_t, Client> clients_;
    std::uint64_t next_client_ = 1;

    std::uint64_t generation_ = 0;
    /// The last frame handed to the writer, as (backlight, hardware zones).
    std::optional<std::pair<bool, std::array<Rgb, 4>>> last_job_;
    /// The next job is forced (see WriteJob), even if its frame equals the last one.
    bool force_next_ = false;
    bool force_backlight_next_ = false;
    std::optional<Output> last_output_;
    std::optional<Status> last_status_;
    std::vector<Pending> pending_;
    std::vector<ProbeWaiter> awaiting_probe_;
    /// In arrival order, so the oldest deadline is the first; never longer than kMaxPendingSignals.
    std::vector<PendingSignal> pending_signals_;
    /// The newest generation the writer reported on, and the newest job that writes the
    /// backlight: a read taken before that job landed may show the old state, so it is dropped.
    std::uint64_t completed_generation_ = 0;
    std::uint64_t backlight_generation_ = 0;
    std::optional<TimePoint> save_at_;
    /// Set while the last output was animated: an effect ticks.
    std::optional<TimePoint> next_tick_;
    std::optional<TimePoint> next_probe_;
    std::optional<EventLoop::TimerId> wake_timer_;
    std::optional<TimePoint> wake_at_;
    std::function<void(const Config &)> on_config_replaced_;
};

/// Loads config, palette and state, opens the backend, binds the socket, turns SIGTERM/SIGINT
/// into a clean stop, and runs. Returns the process exit code.
int run_daemon(const RunOptions &opts);

} // namespace omenkbd
