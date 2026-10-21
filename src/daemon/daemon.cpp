#include "daemon/daemon.hpp"

#include "backend/mock.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <print>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace omenkbd {

namespace {

using namespace std::chrono_literals;

constexpr Clock::duration kReplyTimeout = 2s;
constexpr Clock::duration kProbeEvery = 3s;
constexpr Clock::duration kSaveUserChange = 1s;
constexpr Clock::duration kSaveCounterOnly = 60s;
constexpr Clock::duration kAcceptRetry = 100ms;
/// A request line is well under 1 KiB; anything this long is not a client of ours.
constexpr std::size_t kMaxIn = 64 * 1024;
/// A subscriber this far behind is not reading; drop it rather than buffer forever.
constexpr std::size_t kMaxOut = 1024 * 1024;

void warn(std::string_view msg) { std::println(stderr, "omen-kbd: warning: {}", msg); }

/// Writes as much of `out` as the socket takes. False if the peer is gone.
bool flush(int fd, std::string &out) {
    while (!out.empty()) {
        const ssize_t n = ::send(fd, out.data(), out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            out.erase(0, static_cast<std::size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        }
    }
    return true;
}

} // namespace

Daemon::Daemon(Model model, std::unique_ptr<Backend> backend, UniqueFd listen_fd, RunOptions opts)
    : model_(std::move(model)), opts_(std::move(opts)),
      writer_(std::make_unique<Writer>(std::move(backend))), listen_(std::move(listen_fd)) {}

Daemon::~Daemon() = default;

int Daemon::run() {
    if (!loop_.valid()) {
        std::println(stderr, "omen-kbd: {}", loop_.error());
        return 1;
    }
    if (writer_->notify_fd() < 0) {
        std::println(stderr, "omen-kbd: cannot create the writer's eventfd");
        return 1;
    }
    loop_.watch(listen_.get(), EPOLLIN, [this](std::uint32_t) { on_accept(); });
    loop_.watch(writer_->notify_fd(), EPOLLIN, [this](std::uint32_t) { on_writer(); });
    loop_.on_before_poll([this] { arm_wake(); });
    recompute();

    loop_.run();

    save();
    for (const Pending &p : std::exchange(pending_, {})) {
        respond(p.client, Response::failure("daemon shutting down"));
    }
    for (auto &[id, c] : clients_) {
        flush(c.fd.get(), c.out);
        loop_.unwatch(c.fd.get());
    }
    clients_.clear();
    loop_.unwatch(listen_.get());
    listen_.reset();
    std::error_code ec;
    std::filesystem::remove(opts_.socket_path, ec);
    writer_.reset(); // joins: no backend call outlives run()
    return 0;
}

void Daemon::post(Event ev) {
    loop_.post([this, ev = std::move(ev)] { handle_event(ev); });
}

void Daemon::handle_event(const Event &ev) {
    std::visit(
        [this](const auto &e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, FactUpdate>) {
                model_.apply_fact(e);
            } else if constexpr (std::is_same_v<T, event::Sleep>) {
                model_.set_suspended(e.entering);
                if (e.entering) {
                    save();
                } else {
                    force_next_ = true; // the firmware may have reset the colors
                }
            } else if constexpr (std::is_same_v<T, event::Submap>) {
                model_.set_submap(e.name);
            } else if constexpr (std::is_same_v<T, event::ThemeChanged>) {
                if (auto p = Palette::load(model_.config().theme.dir)) {
                    model_.set_palette(std::move(*p));
                } else {
                    model_.add_warning(p.error());
                }
            } else if constexpr (std::is_same_v<T, event::HwBacklight>) {
                model_.set_hw_backlight(e.on);
            }
        },
        ev);
    recompute();
}

// ── frame ──────────────────────────────────────────────────────────────────

std::optional<std::uint64_t> Daemon::recompute() {
    const TimePoint now = Clock::now();
    std::optional<std::uint64_t> sent;
    if (const auto in = model_.inputs(now)) {
        Output out = compose(*in, std::chrono::duration<double>(now - start_).count());
        const auto hw = model_.config().device.zone_map.to_hw(out.zones);
        const std::pair key{out.backlight, hw};
        if (force_next_ || last_job_ != key) {
            ++generation_;
            writer_->submit(WriteJob{generation_, out.backlight, hw, force_next_});
            force_next_ = false;
            last_job_ = key;
            sent = generation_;
        }
        const auto period =
            std::chrono::milliseconds(1000 / std::max(1, int{model_.config().effects.fps}));
        next_tick_ = out.animated ? std::optional(now + period) : std::nullopt;
        last_output_ = out;
    } else {
        next_tick_.reset();
    }
    next_probe_ = model_.wants_animation() ? std::optional(next_probe_.value_or(now + kProbeEvery))
                                           : std::nullopt;
    broadcast();
    return sent;
}

void Daemon::broadcast() {
    Status s = status();
    if (last_status_ == s) {
        return;
    }
    const std::string line = encode(Response::success(s)) + "\n";
    last_status_ = std::move(s);
    std::vector<std::uint64_t> subs;
    for (auto &[id, c] : clients_) {
        if (c.subscriber) {
            c.out += line;
            subs.push_back(id);
        }
    }
    for (const auto id : subs) {
        service(id);
    }
}

void Daemon::schedule_save(Clock::duration within) {
    const TimePoint at = Clock::now() + within;
    save_at_ = save_at_ ? std::min(*save_at_, at) : at;
}

void Daemon::save() {
    save_at_.reset();
    if (auto r = model_.persisted().save(opts_.state_path); !r) {
        warn(std::format("saving {}: {}", opts_.state_path.string(), r.error()));
    }
}

// ── clients ────────────────────────────────────────────────────────────────

void Daemon::on_accept() {
    for (;;) {
        UniqueFd fd(::accept4(listen_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
        if (!fd) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                // e.g. EMFILE: the socket stays readable, so pause instead of spinning.
                warn(std::format("accept: {}", std::strerror(errno)));
                loop_.modify(listen_.get(), 0);
                loop_.at(Clock::now() + kAcceptRetry,
                         [this] { loop_.modify(listen_.get(), EPOLLIN); });
            }
            return;
        }
        const std::uint64_t id = next_client_++;
        const int raw = fd.get();
        Client c;
        c.fd = std::move(fd);
        c.events = EPOLLIN;
        clients_.emplace(id, std::move(c));
        loop_.watch(raw, EPOLLIN, [this, id](std::uint32_t ev) { on_client(id, ev); });
    }
}

void Daemon::on_client(std::uint64_t id, std::uint32_t events) {
    const auto it = clients_.find(id);
    if (it == clients_.end()) {
        return;
    }
    Client &c = it->second;
    if ((events & EPOLLERR) != 0) {
        close_client(id);
        return;
    }
    if ((events & (EPOLLIN | EPOLLHUP)) != 0) {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(c.fd.get(), buf, sizeof(buf));
            if (n > 0) {
                if (!c.subscriber) { // input after `subscribe` is ignored
                    c.in.append({buf, static_cast<std::size_t>(n)});
                }
                if (c.in.pending() > kMaxIn) {
                    break;
                }
                continue;
            }
            if (n == 0) {
                c.eof = true;
            } else if (errno == EINTR) {
                continue;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                close_client(id);
                return;
            }
            break;
        }
    }
    if ((events & EPOLLHUP) != 0) {
        c.hup = true;
    }
    service(id);
}

void Daemon::service(std::uint64_t id) {
    const auto it = clients_.find(id);
    if (it == clients_.end()) {
        return;
    }
    Client &c = it->second;
    while (!c.awaiting && !c.subscriber) {
        auto line = c.in.next_line();
        if (!line) {
            break;
        }
        handle_line(id, c, *line);
    }
    if (!c.awaiting && !c.subscriber && c.in.pending() > kMaxIn) {
        respond(id, Response::failure("request line too long"));
        c.in = LineBuffer{};
        c.eof = true;
    }
    if (!flush(c.fd.get(), c.out) || c.hup || c.out.size() > kMaxOut ||
        (c.eof && !c.awaiting && c.out.empty())) {
        close_client(id);
        return;
    }
    const std::uint32_t want =
        (c.eof ? 0U : std::uint32_t{EPOLLIN}) | (c.out.empty() ? 0U : std::uint32_t{EPOLLOUT});
    if (want != c.events) {
        loop_.modify(c.fd.get(), want);
        c.events = want;
    }
}

void Daemon::handle_line(std::uint64_t id, Client &c, const std::string &line) {
    auto r = decode_request(line);
    if (!r) {
        respond(id, Response::failure(r.error()));
        return;
    }
    on_request(id, c, *r);
}

void Daemon::on_request(std::uint64_t id, Client &c, const Request &r) {
    if (std::holds_alternative<req::Status>(r)) {
        respond(id, Response::success(status()));
        return;
    }
    if (std::holds_alternative<req::Subscribe>(r)) {
        respond(id, Response::success(status()));
        c.subscriber = true;
        c.in = LineBuffer{};
        return;
    }
    if (std::holds_alternative<req::Reload>(r)) {
        auto config = load_config(opts_.config_path);
        if (!config) {
            respond(id, Response::failure(config.error()));
            return;
        }
        auto palette = Palette::load(config->theme.dir);
        if (!palette) {
            model_.add_warning(palette.error());
        }
        Palette p = palette ? std::move(*palette) : model_.palette();
        model_.replace_config(std::move(*config), std::move(p));
        recompute();
        respond(id, Response::success(status()));
        return;
    }
    const auto change = model_.handle(r, Clock::now());
    if (!change) {
        respond(id, Response::failure(change.error()));
        return;
    }
    if (*change == Change::None) {
        respond(id, Response::success(status()));
        return;
    }
    if (*change == Change::OutputAndPersist) {
        schedule_save(kSaveUserChange);
    }
    if (const auto generation = recompute()) {
        pending_.push_back({*generation, id, Clock::now() + kReplyTimeout});
        c.awaiting = true;
    } else {
        respond(id, Response::success(status()));
    }
}

void Daemon::respond(std::uint64_t id, const Response &r) {
    const auto it = clients_.find(id);
    if (it == clients_.end()) {
        return; // the client left before its reply
    }
    it->second.out += encode(r) + "\n";
    it->second.awaiting = false;
}

void Daemon::close_client(std::uint64_t id) {
    const auto it = clients_.find(id);
    if (it == clients_.end()) {
        return;
    }
    loop_.unwatch(it->second.fd.get());
    clients_.erase(it);
}

// ── writer results and timers ──────────────────────────────────────────────

void Daemon::on_writer() {
    for (WriterMsg &msg : writer_->drain()) {
        if (auto *o = std::get_if<WriteOutcome>(&msg)) {
            model_.record_write(o->result);
            if (o->result && *o->result) {
                schedule_save(kSaveCounterOnly);
            } else if (!o->result) {
                last_job_.reset(); // forget the frame so the next change re-sends it
            }
            const Response resp = o->result ? Response::success(status())
                                            : Response::failure(o->result.error().message());
            std::vector<std::uint64_t> done;
            std::erase_if(pending_, [&](const Pending &p) {
                if (p.generation > o->generation) {
                    return false;
                }
                respond(p.client, resp);
                done.push_back(p.client);
                return true;
            });
            // Never recompute here: after an error that would re-send the same frame at once and
            // spin while the error persists. The next change (or, during an effect, the next tick
            // — which shows the static frame once a budget error stops animation) retries.
            broadcast();
            for (const auto id : done) {
                service(id);
            }
        } else {
            const auto &probe = std::get<BacklightProbe>(msg);
            if (probe.result) {
                model_.set_hw_backlight(*probe.result);
                recompute();
            }
            // A failed probe is not actionable; the next one retries.
        }
    }
}

std::optional<TimePoint> Daemon::next_wake() const {
    std::optional<TimePoint> w;
    const auto consider = [&w](std::optional<TimePoint> t) {
        if (t && (!w || *t < *w)) {
            w = t;
        }
    };
    consider(next_tick_);
    consider(model_.hint_deadline());
    consider(save_at_);
    consider(next_probe_);
    for (const Pending &p : pending_) {
        consider(p.deadline);
    }
    return w;
}

void Daemon::arm_wake() {
    const auto w = next_wake();
    if (w == wake_at_ && (wake_timer_ || !w)) {
        return;
    }
    if (wake_timer_) {
        loop_.cancel(*wake_timer_);
        wake_timer_.reset();
    }
    wake_at_ = w;
    if (w) {
        wake_timer_ = loop_.at(*w, [this] {
            wake_timer_.reset();
            wake_at_.reset();
            on_wake();
        });
    }
}

void Daemon::on_wake() {
    const TimePoint now = Clock::now();
    const auto due = [now](std::optional<TimePoint> t) { return t && *t <= now; };
    if (due(next_probe_)) {
        writer_->probe();
        next_probe_ = now + kProbeEvery;
    }
    if (due(save_at_)) {
        save();
    }
    std::vector<std::uint64_t> expired;
    std::erase_if(pending_, [&](const Pending &p) {
        if (p.deadline > now) {
            return false;
        }
        respond(p.client, Response::failure("timed out waiting for the keyboard write"));
        expired.push_back(p.client);
        return true;
    });
    for (const auto id : expired) {
        service(id);
    }
    if (due(next_tick_) || due(model_.hint_deadline())) {
        recompute();
    }
}

// ── entry point ────────────────────────────────────────────────────────────

int run_daemon(const RunOptions &opts) {
    // Block before any thread starts, so every thread inherits the mask and only the signalfd
    // sees these signals.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    UniqueFd signals(::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
    if (!signals) {
        std::println(stderr, "omen-kbd: signalfd: {}", std::strerror(errno));
        return 1;
    }

    auto config = load_config(opts.config_path);
    if (!config) {
        std::println(stderr, "omen-kbd: {}", config.error());
        return 1;
    }
    const std::string first = config->profiles.front().name;
    std::vector<std::string> warnings;
    Palette palette;
    if (auto p = Palette::load(config->theme.dir)) {
        palette = std::move(*p);
    } else {
        warnings.push_back(p.error());
    }
    auto [persisted, state_warning] = PersistedState::load(opts.state_path, first);
    if (state_warning) {
        warnings.push_back(*state_warning);
    }
    std::unique_ptr<Backend> backend;
    if (opts.backend == BackendKind::Mock) {
        backend = make_mock(opts.mock_render).first;
    } else if (auto b = open_backend(opts.backend, opts.sys_root)) {
        backend = std::move(*b);
    } else {
        std::println(stderr, "omen-kbd: {}", b.error().message());
        return 1;
    }
    Model model(std::move(*config), std::move(palette), std::move(persisted),
                std::string(backend->name()));
    for (const auto &w : warnings) {
        warn(w);
        model.add_warning(w);
    }
    auto listen = bind_socket(opts.socket_path);
    if (!listen) {
        std::println(stderr, "omen-kbd: binding {}: {}", opts.socket_path.string(),
                     listen.error().message());
        return 1;
    }

    Daemon d(std::move(model), std::move(backend), std::move(*listen), opts);
    d.loop().watch(signals.get(), EPOLLIN, [&](std::uint32_t) {
        signalfd_siginfo si{};
        while (::read(signals.get(), &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
        }
        d.stop();
    });
    // Event sources (Hyprland, theme, hwmon, system D-Bus) attach here when opts.with_sources.
    return d.run();
}

} // namespace omenkbd
