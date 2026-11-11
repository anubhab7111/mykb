// Tests for packaging/agent-hook/omen-kbd-agent-hook and the snippets shipped next to it.
// The hook runs under `env -i` with a fake omen-kbd, a fake hyprctl and a fake /proc tree, so the
// real daemon, the real compositor and this machine's process tree are never involved.

#include "config.hpp"
#include "core/rules.hpp"
#include "helpers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <utility>
#include <vector>

namespace omenkbd {
namespace {

namespace fs = std::filesystem;
using test::read_file;
using test::TempDir;
using test::wait_until;
using test::write_file;

struct Result {
    int code = -1;
    std::string out; // stdout and stderr, interleaved
};

bool have(const char *tool) {
    return std::system((std::string("command -v ") + tool + " >/dev/null 2>&1").c_str()) == 0;
}

/// Runs `cmd` through the shell; returns its exit status and combined output.
Result run(const std::string &cmd) {
    Result r;
    FILE *p = ::popen((cmd + " 2>&1").c_str(), "r");
    if (p == nullptr) {
        return r;
    }
    std::array<char, 4096> buf{};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) {
        r.out.append(buf.data(), n);
    }
    const int status = ::pclose(p);
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

std::string source_path(const std::string &relative) {
    return std::string(OMEN_KBD_SOURCE_DIR) + "/" + relative;
}

std::string shq(const fs::path &p) { return "'" + p.string() + "'"; }

void write_exec(const fs::path &p, const std::string &content) {
    write_file(p, content);
    fs::permissions(p, fs::perms::owner_all, fs::perm_options::add);
}

std::vector<std::string> lines_of(const fs::path &p) {
    std::vector<std::string> out;
    std::istringstream in(read_file(p));
    for (std::string line; std::getline(in, line);) {
        out.push_back(line);
    }
    return out;
}

/// Sample hook input, shaped like the documented per-event payloads (hooks-facts.md).
std::string payload(const std::string &event, const std::string &session = "S1",
                    const std::string &notification_type = "") {
    std::string extra;
    if (event == "Notification") {
        extra =
            R"(,"message":"Claude needs your permission to use Bash","title":"Permission needed")";
        if (!notification_type.empty()) {
            extra += R"(,"notification_type":")" + notification_type + '"';
        }
    } else if (event == "UserPromptSubmit") {
        extra = R"(,"prompt":"please fix the build")";
    } else if (event == "PostToolUse") {
        extra = R"(,"tool_name":"Bash","tool_input":{"command":"ls"},)"
                R"("tool_response":{"stdout":"a b"},"tool_use_id":"toolu_01")";
    } else if (event == "PostToolUseFailure") {
        extra = R"(,"tool_name":"Bash","tool_input":{"command":"false"},)"
                R"("tool_use_id":"toolu_02","error":"exit 1")";
    } else if (event == "Stop") {
        extra = R"(,"stop_hook_active":false,"last_assistant_message":"Done.",)"
                R"("background_tasks":[],"session_crons":[])";
    } else if (event == "SessionEnd") {
        extra = R"(,"reason":"prompt_input_exit")";
    }
    return R"({"session_id":")" + session +
           R"(","transcript_path":"/tmp/t.jsonl","cwd":"/tmp","hook_event_name":")" + event + '"' +
           extra + "}";
}

class AgentHook : public testing::Test {
  protected:
    void SetUp() override {
        if (!have("jq") || !have("bash")) {
            GTEST_SKIP() << "jq and bash are needed";
        }
        write_exec(bin_dir() / "omen-kbd", fake_omen_kbd());
        write_exec(bin_dir() / "hyprctl", fake_hyprctl());
        fs::create_directories(proc_dir());
        set_clients({{100, "0xabc"}, {999, "0xdef"}});
    }

    [[nodiscard]] fs::path root() const { return dir_.path(); }
    [[nodiscard]] fs::path bin_dir() const { return root() / "bin"; }
    [[nodiscard]] fs::path proc_dir() const { return root() / "proc"; }
    [[nodiscard]] fs::path run_dir() const { return root() / "run"; }
    [[nodiscard]] fs::path agent_dir() const { return run_dir() / "omen-kbd" / "agent"; }
    [[nodiscard]] fs::path calls_log() const { return root() / "calls.log"; }
    [[nodiscard]] fs::path hyprctl_log() const { return root() / "hyprctl.log"; }
    [[nodiscard]] fs::path hook_script() const {
        return source_path("packaging/agent-hook/omen-kbd-agent-hook");
    }
    [[nodiscard]] fs::path marker(const std::string &session) const {
        return agent_dir() / (session + ".waiting");
    }
    [[nodiscard]] fs::path window_cache(const std::string &session) const {
        return agent_dir() / (session + ".window");
    }

    /// What the fake omen-kbd received, one line per call.
    [[nodiscard]] std::vector<std::string> calls() const { return lines_of(calls_log()); }
    [[nodiscard]] std::string last_call() const {
        const auto all = calls();
        return all.empty() ? "(no call)" : all.back();
    }
    [[nodiscard]] std::vector<std::string> hyprctl_calls() const { return lines_of(hyprctl_log()); }

    /// A fake omen-kbd that logs its arguments. A `noisy` file makes it print on both streams.
    [[nodiscard]] std::string fake_omen_kbd(const std::string &tail = "") const {
        return "#!/bin/bash\n"
               "if [ -e " +
               shq(root() / "noisy") +
               " ]; then echo noise-out; echo noise-err >&2; fi\n"
               "echo \"$*\" >> " +
               shq(calls_log()) + "\n" + tail;
    }

    /// A fake hyprctl that prints clients.json. `fail` and `hang` files change its behavior.
    [[nodiscard]] std::string fake_hyprctl() const {
        return "#!/bin/bash\n"
               "echo \"$*\" >> " +
               shq(hyprctl_log()) +
               "\n"
               "if [ -e " +
               shq(root() / "hyprctl.fail") +
               " ]; then exit 1; fi\n"
               "if [ -e " +
               shq(root() / "hyprctl.hang") +
               " ]; then exec sleep 5; fi\n"
               "cat " +
               shq(root() / "clients.json") + "\n";
    }

    /// The windows `hyprctl clients -j` reports: (owner pid, address). Titles are never needed.
    void set_clients(const std::vector<std::pair<int, std::string>> &clients) {
        std::string json = "[";
        for (const auto &[pid, address] : clients) {
            json += std::string(json.size() > 1 ? "," : "") + R"({"address":")" + address +
                    R"(","mapped":true,"class":"Alacritty","title":"secret","pid":)" +
                    std::to_string(pid) + "}";
        }
        write_file(root() / "clients.json", json + "]\n");
    }

    /// A process in the fake /proc, in the layout of a real /proc/<pid>/stat.
    void add_proc(int pid, const std::string &comm, int ppid) {
        write_file(proc_dir() / std::to_string(pid) / "stat",
                   std::to_string(pid) + " (" + comm + ") S " + std::to_string(ppid) + " " +
                       std::to_string(pid) + " " + std::to_string(pid) +
                       " 0 -1 4194304 0 0 0 0 0 0 0 0 20 0 1 0 12345 0 0\n");
    }

    /// R12's interactive layout: 100:alacritty <- 101:bash <- 102:claude <- 110:sh <- hook.
    void interactive_layout() {
        add_proc(100, "alacritty", 1);
        add_proc(101, "bash", 100);
        add_proc(102, "claude", 101);
        add_proc(110, "sh", 102);
        start_pid_ = 110;
    }

    /// R12's background layout: ... 102:claude <- 103:claude (daemon) <- 104:claude (bg-pty-host)
    /// <- 105:claude (the session, which runs the hook directly).
    void background_layout() {
        add_proc(100, "alacritty", 1);
        add_proc(101, "bash", 100);
        add_proc(102, "claude", 101);
        add_proc(103, "claude", 102);
        add_proc(104, "claude", 103);
        add_proc(105, "claude", 104);
        start_pid_ = 105;
    }

    /// Runs the hook with `input` on stdin. Every run must exit 0 and print nothing at all, so
    /// that is checked here for all tests; `extra_env` (VAR=value ...) overrides the defaults.
    Result hook(const std::string &input, const std::string &extra_env = "") {
        write_file(root() / "in.json", input);
        Result r = run(hook_command(extra_env) + " < " + shq(root() / "in.json"));
        EXPECT_EQ(r.code, 0) << payload_summary(input);
        EXPECT_EQ(r.out, "") << "the hook must print nothing: " << payload_summary(input);
        return r;
    }

    /// The command line that runs the hook under `env -i`, without a stdin redirection.
    [[nodiscard]] std::string hook_command(const std::string &extra_env = "") const {
        return "env -i PATH='" + path_env() + "' HOME=" + shq(root() / "home") +
               " XDG_RUNTIME_DIR=" + shq(run_dir()) +
               " OMEN_KBD_BIN=" + shq(bin_dir() / "omen-kbd") +
               " OMEN_KBD_HYPRCTL=" + shq(bin_dir() / "hyprctl") +
               " OMEN_KBD_PROC=" + shq(proc_dir()) +
               " OMEN_KBD_START_PID=" + std::to_string(start_pid_) + " " + extra_env +
               " /bin/bash " + shq(hook_script());
    }

    /// Makes the fake omen-kbd take 5 s (and ignore SIGTERM when `ignore_term`).
    void hang_omen_kbd(bool ignore_term) {
        write_exec(
            bin_dir() / "omen-kbd",
            fake_omen_kbd(std::string(ignore_term ? "trap '' TERM\n" : "") + "exec sleep 5\n"));
    }

    /// The waiting episode of R12's interactive session, so a window is cached.
    void raise_waiting(const std::string &session = "S1") {
        hook(payload("Notification", session, "permission_prompt"));
    }

  private:
    static std::string path_env() {
        const char *p = std::getenv("PATH");
        return p != nullptr && *p != '\0' ? p : "/usr/bin:/bin";
    }
    static std::string payload_summary(const std::string &input) { return input.substr(0, 120); }

    TempDir dir_;
    int start_pid_ = 110;
};

// ---- window discovery -------------------------------------------------------------------------

TEST_F(AgentHook, PermissionPromptSendsWaitingWithTheTerminalWindow) {
    interactive_layout(); // hyprctl lists pid 100 (alacritty) as 0xabc
    hook(payload("Notification", "S1", "permission_prompt"));
    EXPECT_EQ(calls(), (std::vector<std::string>{
                           "agent waiting --session S1 --window 0xabc --source claude"}));
    EXPECT_TRUE(fs::exists(marker("S1")));
    EXPECT_EQ(hyprctl_calls(), (std::vector<std::string>{"clients -j"}));
}

TEST_F(AgentHook, BackgroundSessionGetsNoWindow) {
    background_layout(); // pid 100 still owns 0xabc, so only the nesting rule can explain no link
    hook(payload("Notification", "S1", "permission_prompt"));
    EXPECT_EQ(calls(), (std::vector<std::string>{"agent waiting --session S1 --source claude"}));
    EXPECT_TRUE(hyprctl_calls().empty()) << "a nested session needs no compositor query";
}

TEST_F(AgentHook, NoWindowOwnerMeansNoWindowArgument) {
    interactive_layout();
    set_clients({{999, "0xdef"}, {998, "0x123"}}); // none of the ancestors owns a window
    hook(payload("Notification", "S1", "permission_prompt"));
    EXPECT_EQ(calls(), (std::vector<std::string>{"agent waiting --session S1 --source claude"}));
}

TEST_F(AgentHook, TheNearestAncestorWithAWindowWins) {
    interactive_layout();
    set_clients({{100, "0xaaa"}, {101, "0xbbb"}}); // listed far-first; walk order decides
    hook(payload("Notification", "S1", "permission_prompt"));
    EXPECT_EQ(calls(), (std::vector<std::string>{
                           "agent waiting --session S1 --window 0xbbb --source claude"}));
}

TEST_F(AgentHook, AWindowOwnerWithSeveralWindowsMeansNoWindowArgument) {
    interactive_layout(); // a terminal server: the pid cannot say which window the session is in
    set_clients({{100, "0xabc"}, {100, "0xabd"}});
    hook(payload("Notification", "S1", "permission_prompt"));
    EXPECT_EQ(calls(), (std::vector<std::string>{"agent waiting --session S1 --source claude"}));
}

TEST_F(AgentHook, TheSessionClaudeCanBeTheHooksOwnParent) {
    add_proc(100, "alacritty", 1);
    add_proc(101, "bash", 100);
    add_proc(102, "claude", 101); // the shell exec'd the hook: no sh in between
    hook(payload("Notification", "S1", "permission_prompt"), "OMEN_KBD_START_PID=102");
    EXPECT_EQ(calls(), (std::vector<std::string>{
                           "agent waiting --session S1 --window 0xabc --source claude"}));
}

TEST_F(AgentHook, NoClaudeAmongTheAncestorsMeansNoWindowArgument) {
    add_proc(100, "alacritty", 1);
    add_proc(101, "bash", 100);
    hook(payload("Notification", "S1", "permission_prompt"), "OMEN_KBD_START_PID=101");
    EXPECT_EQ(calls(), (std::vector<std::string>{"agent waiting --session S1 --source claude"}));
}

TEST_F(AgentHook, TheAgentCommNameCanBeOverridden) {
    interactive_layout();
    write_file(proc_dir() / "102" / "stat", "102 (node) S 101 102 102 0 -1 4194304\n");
    hook(payload("Notification", "S1", "permission_prompt"));
    EXPECT_EQ(calls(), (std::vector<std::string>{"agent waiting --session S1 --source claude"}));
    hook(payload("Notification", "S2", "permission_prompt"), "OMEN_KBD_AGENT_COMM=node");
    EXPECT_EQ(last_call(), "agent waiting --session S2 --window 0xabc --source claude");
}

TEST_F(AgentHook, ACommWithParenthesesAndSpacesDoesNotConfuseTheWalk) {
    interactive_layout();
    write_file(proc_dir() / "101" / "stat", "101 (my (odd) shell) S 100 101 101 0 -1 4194304\n");
    hook(payload("Notification", "S1", "permission_prompt"));
    EXPECT_EQ(calls(), (std::vector<std::string>{
                           "agent waiting --session S1 --window 0xabc --source claude"}));
}

TEST_F(AgentHook, AProcessCycleDoesNotHangTheWalk) {
    add_proc(100, "bash", 101);
    add_proc(101, "bash", 100);
    add_proc(102, "claude", 100);
    set_clients({{999, "0xdef"}});
    hook(payload("Notification", "S1", "permission_prompt"), "OMEN_KBD_START_PID=102");
    EXPECT_EQ(calls(), (std::vector<std::string>{"agent waiting --session S1 --source claude"}));
}

TEST_F(AgentHook, TheWindowIsDiscoveredOnceAndCachedPerSession) {
    interactive_layout();
    raise_waiting();
    EXPECT_EQ(read_file(window_cache("S1")), "0xabc\n");
    set_clients({{100, "0xfff"}}); // would change the answer if the hook asked again
    hook(payload("Stop"));
    EXPECT_EQ(last_call(), "agent done --session S1 --window 0xabc");
    EXPECT_EQ(hyprctl_calls().size(), 1U);
    raise_waiting("S2"); // another session finds its own window
    EXPECT_EQ(last_call(), "agent waiting --session S2 --window 0xfff --source claude");
}

TEST_F(AgentHook, ANestedSessionCachesItsMissingWindow) {
    background_layout();
    raise_waiting();
    EXPECT_EQ(read_file(window_cache("S1")), "\n");
    hook(payload("Stop"));
    EXPECT_EQ(last_call(), "agent done --session S1");
}

TEST_F(AgentHook, AFailedWindowLookupIsNotCached) {
    interactive_layout();
    write_file(root() / "hyprctl.fail", "");
    raise_waiting();
    EXPECT_EQ(last_call(), "agent waiting --session S1 --source claude");
    EXPECT_FALSE(fs::exists(window_cache("S1"))) << "a lookup that failed must be retried";
    fs::remove(root() / "hyprctl.fail");
    hook(payload("Stop"));
    EXPECT_EQ(last_call(), "agent done --session S1 --window 0xabc");
}

TEST_F(AgentHook, AMalformedWindowAddressIsNotPassedOn) {
    interactive_layout();
    set_clients({{100, "--help"}});
    raise_waiting();
    EXPECT_EQ(last_call(), "agent waiting --session S1 --source claude");
}

// ---- event mapping ----------------------------------------------------------------------------

TEST_F(AgentHook, EveryWaitingNotificationTypeSendsWaiting) {
    interactive_layout();
    for (const std::string type : {"permission_prompt", "idle_prompt", "elicitation_dialog"}) {
        hook(payload("Notification", "S-" + type, type));
        EXPECT_EQ(last_call(),
                  "agent waiting --session S-" + type + " --window 0xabc --source claude")
            << type;
        EXPECT_TRUE(fs::exists(marker("S-" + type))) << type;
    }
}

TEST_F(AgentHook, PostToolUseOnlyClearsAWaitingSession) {
    interactive_layout();
    hook(payload("PostToolUse"));
    EXPECT_TRUE(calls().empty()) << "no marker: nothing to clear";
    raise_waiting();
    hook(payload("PostToolUse"));
    EXPECT_EQ(calls().size(), 2U);
    EXPECT_EQ(last_call(), "agent working --session S1");
    EXPECT_FALSE(fs::exists(marker("S1")));
    hook(payload("PostToolUse"));
    EXPECT_EQ(calls().size(), 2U) << "the marker is gone: a second event sends nothing";
}

TEST_F(AgentHook, EveryWorkingEventClearsAWaitingSessionAndOnlyThatOne) {
    interactive_layout();
    for (const std::string event : {"UserPromptSubmit", "PostToolUse", "PostToolUseFailure"}) {
        const std::string session = "S-" + event;
        hook(payload(event, session));
        EXPECT_TRUE(calls().empty()) << event << " without a marker";
        raise_waiting(session);
        raise_waiting("other");
        hook(payload(event, session));
        EXPECT_EQ(last_call(), "agent working --session " + session) << event;
        EXPECT_FALSE(fs::exists(marker(session))) << event;
        EXPECT_TRUE(fs::exists(marker("other"))) << "another session's marker stays";
        fs::remove_all(run_dir());
        fs::remove(calls_log());
    }
}

TEST_F(AgentHook, StopSendsDoneAndClearsTheMarker) {
    interactive_layout();
    raise_waiting();
    hook(payload("Stop"));
    EXPECT_EQ(last_call(), "agent done --session S1 --window 0xabc");
    EXPECT_FALSE(fs::exists(marker("S1")));
    hook(payload("Stop", "S9")); // done is sent even when the session never waited
    EXPECT_EQ(last_call(), "agent done --session S9 --window 0xabc");
}

TEST_F(AgentHook, SessionEndSendsEndAndRemovesCaches) {
    interactive_layout();
    raise_waiting();
    ASSERT_TRUE(fs::exists(marker("S1")));
    ASSERT_TRUE(fs::exists(window_cache("S1")));
    hook(payload("SessionEnd"));
    EXPECT_EQ(last_call(), "agent end --session S1");
    EXPECT_FALSE(fs::exists(marker("S1")));
    EXPECT_FALSE(fs::exists(window_cache("S1")));
    EXPECT_EQ(hyprctl_calls().size(), 1U) << "SessionEnd has a 1.5 s budget: no compositor query";
}

TEST_F(AgentHook, OtherNotificationTypesAreIgnored) {
    interactive_layout();
    for (const std::string type : {"auth_success", "elicitation_url_dialog", "agent_completed"}) {
        hook(payload("Notification", "S1", type));
    }
    hook(payload("Notification", "S1")); // no notification_type at all
    EXPECT_TRUE(calls().empty());
    EXPECT_FALSE(fs::exists(marker("S1")));
}

TEST_F(AgentHook, UnknownEventsAreIgnored) {
    interactive_layout();
    for (const std::string event : {"SessionStart", "PreToolUse", "SubagentStop", "Nonsense"}) {
        hook(payload(event));
    }
    EXPECT_TRUE(calls().empty());
}

// ---- robustness -------------------------------------------------------------------------------

TEST_F(AgentHook, IsSilentAndExitsZeroOnBadInput) {
    interactive_layout();
    const std::vector<std::string> bad = {
        "",                                                     // empty stdin
        "not json at all",                                      // invalid JSON
        R"({"hook_event_name":"Stop")",                         // truncated JSON
        R"({"hook_event_name":"Stop"})",                        // no session_id
        R"({"session_id":"","hook_event_name":"Stop"})",        // empty session_id
        R"({"session_id":7,"hook_event_name":"Stop"})",         // session_id of the wrong type
        R"({"session_id":"../evil","hook_event_name":"Stop"})", // would leave the marker directory
        R"({"session_id":"a/b","hook_event_name":"Stop"})",
        R"({"session_id":"a b","hook_event_name":"Stop"})",
        R"({"session_id":"x\ny","hook_event_name":"Stop"})",
        R"({"session_id":"S1","hook_event_name":["Stop"]})", // event of the wrong type
        R"([])",                                             // JSON, but not an object
        R"("Stop")",
        "null",
    };
    for (const auto &input : bad) {
        hook(input);
    }
    EXPECT_TRUE(calls().empty());
    EXPECT_FALSE(fs::exists(run_dir())) << "bad input must not create or touch any file";
}

TEST_F(AgentHook, IsSilentForEveryEventEvenWhenTheBinaryIsNoisy) {
    interactive_layout();
    write_file(root() / "noisy", ""); // the fake omen-kbd now prints on stdout and stderr too
    for (const std::string event : {"Notification", "UserPromptSubmit", "PostToolUse",
                                    "PostToolUseFailure", "Stop", "SessionEnd"}) {
        hook(payload(event, "S1", "permission_prompt"));
        raise_waiting(); // so that the working events have something to clear
        hook(payload(event, "S1", "permission_prompt"));
    }
    EXPECT_FALSE(calls().empty());
}

TEST_F(AgentHook, SurvivesAMissingOrHangingBinary) {
    interactive_layout();
    hook(payload("Notification", "S1", "permission_prompt"),
         "OMEN_KBD_BIN=" + shq(root() / "no-such-binary"));
    EXPECT_TRUE(fs::exists(marker("S1"))) << "the marker is kept even if the call failed";

    for (const bool ignore_term : {false, true}) {
        fs::remove_all(run_dir());
        hang_omen_kbd(ignore_term);
        const auto t0 = std::chrono::steady_clock::now();
        hook(payload("Notification", "S1", "permission_prompt"));
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        EXPECT_LT(elapsed, std::chrono::milliseconds(3000))
            << (ignore_term ? "a call that ignores SIGTERM" : "a call that hangs");
        EXPECT_GE(elapsed, std::chrono::milliseconds(900)) << "the fake really did hang";
        EXPECT_TRUE(fs::exists(marker("S1")));
    }
}

TEST_F(AgentHook, SurvivesAHangingCompositorQuery) {
    interactive_layout();
    write_file(root() / "hyprctl.hang", ""); // Hyprland hangs after a wake on this machine
    const auto t0 = std::chrono::steady_clock::now();
    hook(payload("Notification", "S1", "permission_prompt"));
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(3000));
    EXPECT_EQ(calls(), (std::vector<std::string>{"agent waiting --session S1 --source claude"}));
}

TEST_F(AgentHook, ExitsZeroWithoutJq) {
    interactive_layout();
    hook(payload("Notification", "S1", "permission_prompt"), "PATH=/nonexistent");
    EXPECT_TRUE(calls().empty());
    EXPECT_FALSE(fs::exists(run_dir()));
}

TEST_F(AgentHook, DoesNothingWithoutARuntimeDirectory) {
    interactive_layout();
    hook(payload("Notification", "S1", "permission_prompt"), "XDG_RUNTIME_DIR=");
    EXPECT_TRUE(calls().empty());
}

// With fd 0 closed (not empty), bash hands the `$(jq ...)` pipe's read end the number 0, so jq
// would read its own output and never finish. `timeout 3` turns a regression into a failure (exit
// 124) instead of a test that never returns.
TEST_F(AgentHook, ExitsZeroPromptlyAndSilentlyWhenStdinIsClosed) {
    interactive_layout();
    const auto t0 = std::chrono::steady_clock::now();
    const Result r = run("timeout -k 1 3 " + hook_command() + " <&-");
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_EQ(r.code, 0) << "124 means the hook hung on its closed stdin";
    EXPECT_EQ(r.out, "");
    EXPECT_LT(elapsed, std::chrono::milliseconds(1000));
    EXPECT_TRUE(calls().empty());
    EXPECT_FALSE(fs::exists(run_dir())) << "no input: no marker, no cache";
}

// ---- shellcheck and the snippets --------------------------------------------------------------

TEST_F(AgentHook, PassesShellcheck) {
    if (!have("shellcheck")) {
        GTEST_SKIP() << "shellcheck not installed";
    }
    const auto r = run("shellcheck -x " + shq(hook_script()));
    EXPECT_EQ(r.code, 0) << r.out;
    EXPECT_EQ(r.out, "");
}

TEST_F(AgentHook, ShipsAsAnExecutableScript) {
    const auto perms = fs::status(hook_script()).permissions();
    EXPECT_NE(perms & fs::perms::owner_exec, fs::perms::none);
    EXPECT_TRUE(read_file(hook_script()).starts_with("#!/usr/bin/env bash\n"));
}

TEST_F(AgentHook, ClaudeHooksSnippetIsValidJsonWiredToTheSixEvents) {
    const std::string snippet = shq(source_path("packaging/snippets/claude-code-hooks.json"));
    EXPECT_EQ(run("jq . " + snippet + " >/dev/null").code, 0) << "not valid JSON";
    const auto holds = [&](const std::string &filter) {
        const auto r = run("jq -e '" + filter + "' " + snippet);
        EXPECT_EQ(r.code, 0) << filter << "\n" << r.out;
    };
    // The user merges this file into ~/.claude/settings.json: it must be exactly a hooks block,
    // with no extra top-level key that Claude Code might reject.
    holds(R"(keys == ["hooks"])");
    holds(R"(.hooks | keys == ["Notification","PostToolUse","PostToolUseFailure",)"
          R"("SessionEnd","Stop","UserPromptSubmit"])");
    holds(R"([.hooks[][].hooks[] | .type] | all(. == "command"))");
    holds(R"([.hooks[][].hooks[] | .command] | all(. == "$HOME/.local/bin/omen-kbd-agent-hook"))");
    holds(R"(.hooks.Notification | length == 1 and
             .[0].matcher == "permission_prompt|idle_prompt|elicitation_dialog")");
    // Ruling P11: async on the three high-frequency events only, and never asyncRewake.
    holds(R"([.hooks | to_entries[] | select(any(.value[].hooks[]; .async == true)) | .key] | sort
             == ["PostToolUse","PostToolUseFailure","UserPromptSubmit"])");
    holds(R"([.hooks[][].hooks[] | select(has("async")) | .async] | all(. == true))");
    holds(R"([.. | objects | select(has("asyncRewake"))] | length == 0)");
}

TEST_F(AgentHook, ReactionsSnippetCarriesTheExampleDefaultsAndTheFullscreenRule) {
    // Not parsed with the config parser: the fullscreen_app condition arrives in another task.
    const std::string snippet = read_file(source_path("packaging/snippets/reactions-config.toml"));
    const std::string example = read_file(source_path("packaging/config.example.toml"));
    EXPECT_TRUE(snippet.starts_with("# omen-kbd: "));
    EXPECT_NE(snippet.find("power saver dims"), std::string::npos) << "say where it goes";
    // The [reactions] tables, word for word, up to the first [[profiles]].
    const auto begin = example.find("[reactions]\n");
    const auto end = example.find("[[profiles]]");
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const std::string block = example.substr(begin, end - begin);
    EXPECT_NE(snippet.find(block), std::string::npos) << "the snippet drifted from the example";
    const auto rule = snippet.find("name = \"fullscreen video dims\"");
    ASSERT_NE(rule, std::string::npos);
    EXPECT_NE(snippet.find("[[rules]]\nname = \"fullscreen video dims\"\n"
                           "when = { fullscreen_app = [\"mpv\", \"vlc\", \"firefox\", "
                           "\"chromium\"] }\nbrightness_cap = 15\n"),
              std::string::npos);
    EXPECT_GT(rule, snippet.find(block)) << "the rule follows the tables";
}

// The user pastes the snippet into a config shaped like this one (the example config's layout),
// just before "power saver dims". It is parsed here by the product parser, so a syntax slip, a key
// the parser rejects, or an order that lets "power saver dims" win would fail a test and not
// only the user's `omen-kbd reload`.
TEST(ReactionsSnippet, ParsesInAUsersConfigAndTheFullscreenCapBeatsPowerSaver) {
    constexpr std::string_view before_snippet = R"(
[device]
zone_map = { left = 2, middle = 1, right = 0, wasd = 3 }

[effects]
enabled = false
fps = 10

[theme]
colors = "vivid"
dir = "~/.config/theming/current"

[gates]
on_lock = "off"
on_idle = "dim"
dim_brightness = 20
pause_effects_on_battery = true

[hints]
timeout_s = 15
delay_ms = 250
[hints.modifiers]
super = "theme:mauve"
"super+shift" = "theme:blue"
ctrl = "theme:peach"
[hints.submaps]
resize = "theme:teal"

[[profiles]]
name = "default"
colors = ["theme:mauve", "theme:blue", "theme:teal", "theme:peach"]

[[profiles]]
name = "game"
colors = ["#202020", "#202020", "#202020", "#ff0000"]

[[rules]]
name = "gamemode"
when = { gamemode = true }
profile = "game"

)";
    constexpr std::string_view after_snippet = R"(
[[rules]]
name = "power saver dims"
when = { power_profile = "power-saver" }
brightness_cap = 40

[[rules]]
name = "low battery"
when = { on_battery = true, battery_below = 15 }
overlay = { zones = ["right"], color = "#ff0000", pulse = true }
)";
    const std::string snippet = read_file(source_path("packaging/snippets/reactions-config.toml"));
    ASSERT_FALSE(snippet.empty());

    const auto cfg =
        parse_config(std::string(before_snippet) + snippet + std::string(after_snippet));
    ASSERT_TRUE(cfg.has_value()) << cfg.error();
    EXPECT_TRUE(cfg->reactions.enabled);
    EXPECT_TRUE(cfg->needs_fullscreen()) << "the fullscreen query only runs for such a rule";

    std::vector<std::string> names;
    for (const Rule &r : cfg->rules) {
        names.push_back(r.name);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"gamemode", "fullscreen video dims",
                                               "power saver dims", "low battery"}));

    Facts both;
    both.fullscreen_app = "mpv";
    both.power_profile = "power-saver";
    const RuleOutcome out = evaluate(cfg->rules, both);
    EXPECT_EQ(out.brightness_cap, 15) << "rules are first-match: the fullscreen cap, not 40";
    EXPECT_EQ(out.active, (std::vector<std::string>{"fullscreen video dims"}));

    // Each rule on its own still caps as written, so the 15 above is the order, not a typo.
    Facts saver_only;
    saver_only.power_profile = "power-saver";
    EXPECT_EQ(evaluate(cfg->rules, saver_only).brightness_cap, 40);
    Facts video_only;
    video_only.fullscreen_app = "mpv";
    EXPECT_EQ(evaluate(cfg->rules, video_only).brightness_cap, 15);
}

TEST_F(AgentHook, NotifySnippetPassesShellcheck) {
    if (!have("shellcheck")) {
        GTEST_SKIP() << "shellcheck not installed";
    }
    const auto r =
        run("shellcheck -x " + shq(source_path("packaging/snippets/notify-reaction.sh")));
    EXPECT_EQ(r.code, 0) << r.out;
    EXPECT_EQ(r.out, "");
}

TEST_F(AgentHook, NotifySnippetFlashesOutsideDoNotDisturbOnly) {
    // The snippet's subshell, with fake swaync-client, paplay and ~/.local/bin/omen-kbd.
    const fs::path home = root() / "home";
    write_exec(home / ".local" / "bin" / "omen-kbd", fake_omen_kbd());
    write_exec(bin_dir() / "paplay",
               "#!/bin/bash\necho ping >> " + shq(root() / "ping.log") + "\n");
    const auto run_snippet = [&](const std::string &dnd) {
        write_exec(bin_dir() / "swaync-client", "#!/bin/bash\necho " + dnd + "\n");
        const auto r =
            run("env -i PATH='" + bin_dir().string() + ":/usr/bin:/bin' HOME=" + shq(home) +
                " SWAYNC_APP_NAME=ZapZap SWAYNC_DESKTOP_ENTRY=zapzap"
                " SWAYNC_URGENCY=Normal /bin/bash " +
                shq(source_path("packaging/snippets/notify-reaction.sh")));
        EXPECT_EQ(r.code, 0);
        EXPECT_EQ(r.out, "") << "the subshell's output goes to /dev/null";
    };
    run_snippet("true"); // do-not-disturb: no ping, no flash
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_TRUE(calls().empty());
    EXPECT_TRUE(lines_of(root() / "ping.log").empty());
    run_snippet("false");
    ASSERT_TRUE(
        wait_until([&] { return !calls().empty() && !lines_of(root() / "ping.log").empty(); },
                   std::chrono::seconds(3)));
    EXPECT_EQ(calls(), (std::vector<std::string>{
                           "notify --app ZapZap --desktop-entry zapzap --urgency Normal"}));
}

// Live check (2026-11-11): the CLI rejects an empty --urgency with exit 64, and the snippet hides
// its output, so a swaync that leaves SWAYNC_URGENCY empty would silently lose the flash.
TEST_F(AgentHook, NotifySnippetFallsBackToNormalWhenSwayncLeavesTheUrgencyEmpty) {
    const fs::path home = root() / "home";
    write_exec(home / ".local" / "bin" / "omen-kbd", fake_omen_kbd());
    write_exec(bin_dir() / "paplay", "#!/bin/bash\ntrue\n");
    write_exec(bin_dir() / "swaync-client", "#!/bin/bash\necho false\n");
    const auto r = run("env -i PATH='" + bin_dir().string() + ":/usr/bin:/bin' HOME=" + shq(home) +
                       " SWAYNC_APP_NAME=ZapZap SWAYNC_DESKTOP_ENTRY=zapzap SWAYNC_URGENCY= "
                       "/bin/bash " +
                       shq(source_path("packaging/snippets/notify-reaction.sh")));
    EXPECT_EQ(r.code, 0);
    ASSERT_TRUE(wait_until([&] { return !calls().empty(); }, std::chrono::seconds(3)));
    EXPECT_EQ(calls(), (std::vector<std::string>{
                           "notify --app ZapZap --desktop-entry zapzap --urgency normal"}));
}

} // namespace
} // namespace omenkbd
