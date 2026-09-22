// Tests for shrn: process execution, temp files, and staged commands.

#include <shrn.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                          \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(cond)) {                                                            \
            ++g_failures;                                                         \
            std::fprintf(stderr, "  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                                         \
    } while (0)

static void write_file(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary);
    out << content;
}

/// Local slurp helper (shrn no longer ships read_file).
static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}


// Run this binary with --helper=<mode> [arg] to test child-process behavior
// without depending on an external script.
static fs::path g_self_exe;

static void init_self_exe(const char* argv0) {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        g_self_exe = fs::path(buf);
        return;
    }
    if (auto found = shrn::which(argv0)) g_self_exe = *found;
    else g_self_exe = fs::absolute(argv0);
}

static void helper_write(int fd, const char* data, std::size_t n) {
    while (n > 0) {
        ssize_t w = ::write(fd, data, n);
        if (w <= 0) {
            if (w == -1 && errno == EINTR) continue;
            return;
        }
        data += w;
        n -= static_cast<std::size_t>(w);
    }
}

static void helper_sleep_ms(long ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1'000'000L};
    while (::nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

// Bound helper sleeps to keep failure cases short.
static long helper_bounded_ms(const char* text) {
    long ms = std::strtol(text, nullptr, 10);
    if (ms < 0) ms = 0;
    return ms > 5000 ? 5000 : ms;
}

// Publish via rename so the parent never reads a half-written marker.
static void helper_publish(const fs::path& p, const std::string& content) {
    fs::path tmp = p;
    tmp += ".tmp";
    write_file(tmp, content);
    std::error_code ec;
    fs::rename(tmp, p, ec);
}

static int helper_main(const std::string& mode, const char* arg, const char* arg2) {
    if (mode == "emit") {  // write both streams and exit with code 7
        helper_write(STDOUT_FILENO, "OUT\n", 4);
        helper_write(STDERR_FILENO, "ERR\n", 4);
        return 7;
    }
    if (mode == "emit-bytes") {  // arg = byte count written to stderr, then exit
        if (*arg2) helper_publish(std::string(arg2) + ".pid", std::to_string(::getpid()));
        std::size_t n = static_cast<std::size_t>(std::strtoul(arg, nullptr, 10));
        std::vector<char> buf(n, 'x');
        helper_write(STDERR_FILENO, buf.data(), n);
        return 0;
    }
    if (mode == "emit-out-bytes") {  // arg = byte count written to stdout, then exit
        std::size_t n = static_cast<std::size_t>(std::strtoul(arg, nullptr, 10));
        std::vector<char> buf(n, 'o');
        helper_write(STDOUT_FILENO, buf.data(), n);
        return 0;
    }
    if (mode == "close-stderr-sleep") {  // arg = ms to stay alive after closing stderr
        ::close(STDERR_FILENO);
        helper_sleep_ms(helper_bounded_ms(arg));
        return 0;
    }
    if (mode == "orphan-stderr") {  // arg = ms a grandchild keeps the stderr pipe open
        pid_t grandchild = ::fork();
        if (grandchild == 0) {
            ::close(STDOUT_FILENO);
            helper_sleep_ms(helper_bounded_ms(arg));
            ::_exit(0);
        }
        if (grandchild == -1) return 98;
        helper_write(STDERR_FILENO, "direct\n", 7);
        return 0;  // exits immediately; the grandchild still holds fd 2
    }
    if (mode == "pid-sleep-touch") {  // arg = base path, arg2 = ms
        std::string base(arg);        // publishes <base>.pid at once, <base>.late only if it finishes
        helper_publish(base + ".pid", std::to_string(::getpid()));
        helper_sleep_ms(helper_bounded_ms(arg2));
        helper_publish(base + ".late", "done\n");
        return 0;
    }
    if (mode == "sleep-then-write") {  // arg = path, arg2 = ms: append "first" only at the end
        helper_sleep_ms(helper_bounded_ms(arg2));
        int fd = ::open(arg, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd == -1) return 31;
        helper_write(fd, "first", 5);
        ::close(fd);
        return 0;
    }
    if (mode == "rendezvous") {  // arg = own marker, arg2 = peer marker
        helper_publish(arg, std::to_string(::getpid()));
        for (int waited = 0; waited < 4000; waited += 20) {
            if (fs::exists(arg2)) return 0;  // the peer was started before this child was awaited
            helper_sleep_ms(20);
        }
        return 21;  // peer never appeared: the two launches were not concurrent
    }
    if (mode == "open-fds") {  // report inherited descriptors above 2
        for (int fd = 3; fd < 256; ++fd) {
            if (::fcntl(fd, F_GETFD) == -1) continue;
            char line[32];
            int len = std::snprintf(line, sizeof line, "fd=%d\n", fd);
            helper_write(STDERR_FILENO, line, static_cast<std::size_t>(len));
        }
        return 0;
    }
    if (mode == "closed-stdio") {  // arg = temporary directory; close 0/1/2 before run()
        fs::path dir(arg);
        ::close(STDIN_FILENO);
        ::close(STDOUT_FILENO);
        ::close(STDERR_FILENO);
        shrn::RunOptions o;
        o.stdout_file = dir / "closed_stdio.out";  // forces a dup2 onto fd 1
        auto r = shrn::run({g_self_exe.string(), "--helper=emit"}, o);
        if (!r.launched()) return 11;
        if (r.exit_code != 7) return 12;
        if (r.stderr_output != "ERR\n") return 13;
        if (slurp(*o.stdout_file) != "OUT\n") return 14;
        // Launch errors must reach the status pipe, not the redirected stdout file.
        auto bad = shrn::run({"shrn-definitely-not-a-command"}, o);
        if (bad.launched() ||
            bad.error.find("No such file or directory") == std::string::npos) return 15;
        return 0;
    }
    return 99;
}

// Poll until `pred` holds; bounded so a broken implementation fails instead of hanging.
template <class Pred> static bool wait_until(Pred pred, std::chrono::milliseconds bound = 5000ms) {
    auto deadline = std::chrono::steady_clock::now() + bound;
    for (;;) {
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        helper_sleep_ms(10);
    }
}

// Wait for the start marker of a pid-sleep-touch helper, then read its pid.
static std::optional<pid_t> child_pid(const std::string& base) {
    if (!wait_until([&] { return fs::exists(base + ".pid"); })) return std::nullopt;
    auto text = slurp(base + ".pid");
    if (text.empty()) return std::nullopt;
    return static_cast<pid_t>(std::strtol(text.c_str(), nullptr, 10));
}

static bool pid_gone(pid_t pid) { return ::kill(pid, 0) == -1 && errno == ESRCH; }

// No zombie left behind: the implementation must have reaped the child itself.
static bool reaped(pid_t pid) {
    int status = 0;
    return ::waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD;
}

// Clean up only a child we still own; never signal an already-reaped pid.
static void force_kill(pid_t pid) {
    if (pid <= 0) return;
    int status = 0;
    pid_t state;
    do {
        state = ::waitpid(pid, &status, WNOHANG);
    } while (state == -1 && errno == EINTR);
    if (state == 0) {
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
    }
}


// files
static void test_files(const fs::path& dir) {
    std::fprintf(stderr, "files\n");

    auto missing = dir / "missing.txt";
    CHECK(!shrn::file_readable(missing), "missing not readable");

    auto text = dir / "text.txt";
    write_file(text, "a\nb\nc");
    CHECK(shrn::file_readable(text) && shrn::file_non_empty(text), "readable + non-empty");

    auto empty = dir / "empty.txt";
    write_file(empty, "");
    CHECK(shrn::file_readable(empty) && !shrn::file_non_empty(empty), "empty readable but not non-empty");

    auto sub = dir / "a" / "b" / "c";
    CHECK(shrn::ensure_directory(sub) && fs::is_directory(sub), "ensure_directory creates parents");
    CHECK(shrn::ensure_directory(sub), "ensure_directory idempotent");
    CHECK(!shrn::ensure_directory(text), "ensure_directory over a file fails");
}

// process
static void test_which(const fs::path& dir) {
    auto previous_dir = fs::current_path();
    const char* path_env = std::getenv("PATH");
    std::optional<std::string> previous_path;
    if (path_env) previous_path = path_env;
    auto tool = dir / "which-tool";
    write_file(tool, "#!/bin/sh\nexit 0\n");
    CHECK(::chmod(tool.c_str(), 0700) == 0, "make PATH fixture executable");
    fs::current_path(dir);
    for (const char* path : {"", "/shrn-nonexistent:", ":/shrn-nonexistent"}) {
        ::setenv("PATH", path, 1);
        auto found = shrn::which("which-tool");
        auto ran = shrn::run({"which-tool"});
        CHECK(found && fs::equivalent(*found, tool) && ran.launched() && ran.success(),
              "empty PATH components resolve the current directory like execvp");
    }
    CHECK(!shrn::which(dir.string()), "which rejects explicit directory paths");
    auto first = dir / "which-first";
    auto second = dir / "which-second";
    fs::create_directories(first / "which-tool");
    fs::create_directory(second);
    fs::copy_file(tool, second / "which-tool");
    ::setenv("PATH", (first.string() + ":" + second.string()).c_str(), 1);
    auto found = shrn::which("which-tool");
    CHECK(found && fs::equivalent(*found, second / "which-tool"),
          "PATH lookup skips a directory with the command name");
    ::unsetenv("PATH");
    auto shell = shrn::which("sh");
    auto ran = shrn::run({"sh", "-c", "exit 0"});
    CHECK(shell && ran.launched() && ran.success(), "unset PATH uses the system executable search path");
    if (previous_path) ::setenv("PATH", previous_path->c_str(), 1);
    else ::unsetenv("PATH");
    fs::current_path(previous_dir);
}

static void test_process(const fs::path& dir) {
    std::fprintf(stderr, "process\n");

    CHECK(shrn::format_command({"a", "b c", "it's", ""}) == "a 'b c' 'it'\\''s' ''", "format_command quoting");
    CHECK(static_cast<bool>(shrn::which("sh")), "which finds sh");
    CHECK(!shrn::which("shrn-definitely-not-a-command"), "which misses");
    CHECK(static_cast<bool>(shrn::which("/bin/sh")), "which accepts absolute path");

    auto ok = shrn::run({"true"});
    CHECK(ok.launched() && ok.success() && ok.exit_code == 0, "true succeeds");

    auto empty = shrn::run({});
    CHECK(!empty.launched() && empty.error == "empty command", "empty argv fails with 'empty command'");

    auto enoent = shrn::run({"shrn-definitely-not-a-command"});
    CHECK(!enoent.launched() &&
              enoent.error.find("No such file or directory") != std::string::npos,
          "missing binary -> exec error, not exit 127");

    auto code = shrn::run({"sh", "-c", "echo oops >&2; exit 3"});
    CHECK(code.launched() && !code.success() && code.exit_code == 3, "exit code propagated");
    CHECK(code.stderr_output == "oops\n", "stderr captured");

    shrn::RunOptions no_capture;
    no_capture.capture_stderr = false;
    no_capture.inherit_stdout = false;
    auto quiet = shrn::run({"sh", "-c", "echo not-captured >&2; echo swallowed"}, no_capture);
    CHECK(quiet.launched() && quiet.success() && quiet.stderr_output.empty(),
          "capture_stderr=false leaves stderr empty");

    auto sig = shrn::run({"sh", "-c", "kill -9 $$"});
    CHECK(sig.launched() && sig.signal == 9 && sig.exit_code == -1 && !sig.success(), "signal death reported");

    shrn::RunOptions redirect;
    redirect.stdout_file = dir / "out.txt";
    redirect.workdir = dir;
    std::string spawned;
    redirect.on_spawn = [&](std::string_view st, std::string_view cmd) { spawned = std::string(st) + "|" + std::string(cmd); };
    auto red = shrn::run({"sh", "-c", "pwd"}, redirect);
    CHECK(red.launched() && red.success(), "redirect run ok");
    CHECK(slurp(dir / "out.txt") == fs::canonical(dir).string() + "\n", "stdout_file + workdir");
    CHECK(spawned == "|sh -c pwd", "on_spawn receives empty stage and formatted command from run()");

    shrn::RunOptions badwd;
    badwd.workdir = dir / "nowhere";
    auto wd = shrn::run({"true"}, badwd);
    CHECK(!wd.launched() && wd.error.find("No such file or directory") != std::string::npos,
          "bad workdir -> launch error");
}

// process: stderr capture and file descriptors
static void test_process_edges(const fs::path& dir) {
    std::fprintf(stderr, "process stderr and descriptors\n");
    const std::string self = g_self_exe.string();

    // Capture all stderr, both when it fits in the pipe and when it exceeds capacity.
    auto buffered = shrn::run({self, "--helper=emit-bytes", "60000"});
    CHECK(buffered.launched() && buffered.success() && buffered.stderr_output == std::string(60000, 'x'),
          "stderr buffered by an exited child is drained completely");
    auto streamed = shrn::run({self, "--helper=emit-bytes", "1048576"});
    CHECK(streamed.launched() && streamed.success() && streamed.stderr_output == std::string(1048576, 'x'),
          "stderr larger than the pipe is captured completely");

    // Closing stderr early is not an exit: a timed wait must still expire.
    auto closed = shrn::spawn({self, "--helper=close-stderr-sleep", "600"});
    CHECK(closed.error().empty(), "spawn child that closes stderr");
    if (closed.error().empty()) {
        CHECK(closed.wait_for(100ms) == shrn::wait_status::timeout,
              "early stderr EOF is not mistaken for child exit");
        auto& r = closed.wait();
        CHECK(r.launched() && r.success(), "child that closed stderr reports its real exit status");
    }

    // Descendants holding stderr must not delay the direct child's result.
    auto t0 = std::chrono::steady_clock::now();
    auto orphan = shrn::run({self, "--helper=orphan-stderr", "3000"});
    auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(orphan.launched() && orphan.success() && orphan.stderr_output == "direct\n",
          "stderr written before exit survives a descendant holding the pipe");
    CHECK(elapsed < 1500ms, "run returns when the direct child exits, not when descendants do");

    auto held = shrn::spawn({self, "--helper=orphan-stderr", "3000"});
    CHECK(held.error().empty(), "spawn child with a descendant on stderr");
    if (held.error().empty()) {
        t0 = std::chrono::steady_clock::now();
        auto ready = held.wait_for(2s);
        auto& r = held.wait();
        elapsed = std::chrono::steady_clock::now() - t0;
        CHECK(ready == shrn::wait_status::ready,
              "a timed wait returns when the direct child exits");
        CHECK(r.launched() && r.success() && r.stderr_output == "direct\n",
              "buffered stderr survives a descendant keeping the pipe open");
        CHECK(elapsed < 1500ms, "waiting ends with the direct child, not the descendant");
    }

    // Internal pipe and redirection descriptors must be closed by exec.
    std::vector<int> baseline;
    for (int fd = 3; fd < 256; ++fd)
        if (::fcntl(fd, F_GETFD) != -1) baseline.push_back(fd);
    shrn::RunOptions redirect;
    redirect.stdout_file = dir / "fdprobe.out";
    auto probe = shrn::run({self, "--helper=open-fds"}, redirect);
    bool leaked = false;
    if (probe.launched()) {
        for (std::size_t i = 0; i + 3 < probe.stderr_output.size();) {
            std::size_t eol = probe.stderr_output.find('\n', i);
            if (eol == std::string::npos) break;
            int fd = std::atoi(probe.stderr_output.c_str() + i + 3);  // skip "fd="
            bool inherited = false;
            for (int b : baseline) inherited = inherited || b == fd;
            if (!inherited) leaked = true;
            i = eol + 1;
        }
    }
    CHECK(probe.launched() && probe.success() && !leaked, "run() leaks no pipe or redirection descriptor into the child");

    // Redirection must work even when the parent has closed descriptors 0/1/2.
    auto closed_stdio = shrn::run({self, "--helper=closed-stdio", dir.string()});
    CHECK(closed_stdio.launched() && closed_stdio.success(),
          "run() works with stdin/stdout/stderr closed (see helper exit code for the failing check)");
}

// process: spawn, wait, terminate
static void test_process_spawn(const fs::path& dir) {
    std::fprintf(stderr, "process spawn\n");
    const std::string self = g_self_exe.string();

    CHECK(std::is_default_constructible_v<shrn::Process>, "Process is default-constructible");
    CHECK(!std::is_copy_constructible_v<shrn::Process> && !std::is_copy_assignable_v<shrn::Process>,
          "Process is not copyable");
    CHECK(std::is_move_constructible_v<shrn::Process> && std::is_move_assignable_v<shrn::Process>,
          "Process is movable");
    {
        shrn::Process unstarted;  // destroying an unstarted Process must be harmless
        CHECK(!unstarted.running(), "running() on a Process without a child is false");
        CHECK(unstarted.wait_for(0ms) == shrn::wait_status::ready,
              "wait_for() on a Process without a child is ready");
        CHECK(unstarted.wait().error == "no child process",
              "wait() on a Process without a child records the error");
    }

    // Launch errors surface at spawn time, before any wait.
    auto empty = shrn::spawn({});
    CHECK(empty.error() == "empty command", "spawn: empty argv fails with 'empty command'");
    auto enoent = shrn::spawn({"shrn-definitely-not-a-command"});
    CHECK(enoent.error().find("No such file or directory") != std::string::npos,
          "spawn reports the exec failure, not a started process");
    shrn::RunOptions badwd;
    badwd.workdir = dir / "nowhere";
    auto wd = shrn::spawn({"true"}, badwd);
    CHECK(wd.error().find("No such file or directory") != std::string::npos, "spawn reports a bad workdir");

    // Both children must be alive at the same time: each waits for the other's marker.
    auto ma = (dir / "spawn_a.mark").string();
    auto mb = (dir / "spawn_b.mark").string();
    auto pa = shrn::spawn({self, "--helper=rendezvous", ma, mb});
    auto pb = shrn::spawn({self, "--helper=rendezvous", mb, ma});
    CHECK(pa.error().empty() && pb.error().empty(), "two children spawn without waiting");
    if (pa.error().empty() && pb.error().empty()) {
        auto& ra = pa.wait();
        auto& rb = pb.wait();
        CHECK(ra.launched() && ra.success() && rb.launched() && rb.success(),
              "spawn returns before completion, so both children run concurrently");
    }

    // A spawn while another child is live must not leak that child's capture pipe.
    std::vector<int> baseline;  // taken before any capture pipe exists
    for (int fd = 3; fd < 256; ++fd)
        if (::fcntl(fd, F_GETFD) != -1) baseline.push_back(fd);
    auto pbase = (dir / "proc_fds").string();
    auto holder = shrn::spawn({self, "--helper=pid-sleep-touch", pbase, "700"});
    auto probe = shrn::spawn({self, "--helper=open-fds"});
    CHECK(holder.error().empty() && probe.error().empty(),
          "spawn a descriptor probe while another child is live");
    if (holder.error().empty() && probe.error().empty()) {
        auto& pr = probe.wait();
        bool leaked = false;
        if (pr.launched()) {
            for (std::size_t i = 0; i + 3 < pr.stderr_output.size();) {
                std::size_t eol = pr.stderr_output.find('\n', i);
                if (eol == std::string::npos) break;
                int fd = std::atoi(pr.stderr_output.c_str() + i + 3);  // skip "fd="
                bool inherited = false;
                for (int b : baseline) inherited = inherited || b == fd;
                if (!inherited) leaked = true;
                i = eol + 1;
            }
        }
        CHECK(pr.launched() && pr.success() && !leaked,
              "a concurrent spawn inherits no descriptor of the live child");
        auto& hr = holder.wait();
        CHECK(hr.launched() && hr.success(), "the live child is unaffected by the concurrent spawn");
    }

    // running() / wait_for() observe a live child without killing it.
    auto lbase = (dir / "proc_live").string();
    auto live = shrn::spawn({self, "--helper=pid-sleep-touch", lbase, "700"});
    CHECK(live.error().empty(), "spawn a child that outlives the first check");
    if (live.error().empty()) {
        auto pid = child_pid(lbase);
        CHECK(pid.has_value(), "spawned child published its pid");
        CHECK(live.wait_for(50ms) == shrn::wait_status::timeout, "wait_for expires while the child runs");
        CHECK(live.running(), "running() is true before exit and wait_for did not kill");
        CHECK(live.wait_for(5s) == shrn::wait_status::ready, "wait_for observes the exit");
        CHECK(!live.running(), "running() is false after the child exits");
        auto& r = live.wait();
        CHECK(r.launched() && r.success(), "wait after wait_for reports the exit status");
        auto& again = live.wait();
        CHECK(again.launched() && again.exit_code == 0, "repeated wait returns the completed result");
        CHECK(fs::exists(lbase + ".late"), "an unterminated child runs to completion");
        live.terminate();  // void return; a finished child is simply re-waited
        if (pid) {
            CHECK(reaped(*pid), "a waited child leaves no zombie");
            force_kill(*pid);
        }
    }

    // terminate() kills the direct child and finalizes its status.
    auto kbase = (dir / "proc_kill").string();
    auto doomed = shrn::spawn({self, "--helper=pid-sleep-touch", kbase, "4000"});
    CHECK(doomed.error().empty(), "spawn a child to terminate");
    if (doomed.error().empty()) {
        auto pid = child_pid(kbase);
        auto t0 = std::chrono::steady_clock::now();
        doomed.terminate();
        auto& r = doomed.wait();
        auto elapsed = std::chrono::steady_clock::now() - t0;
        CHECK(r.launched() && r.signal == SIGKILL && !r.success(), "terminated child reports SIGKILL");
        CHECK(elapsed < 2s, "terminate does not wait out the child's sleep");
        CHECK(!fs::exists(kbase + ".late"), "terminated child never finished its work");
        if (pid) {
            CHECK(pid_gone(*pid) && reaped(*pid), "terminate reaps the child");
            force_kill(*pid);
        }
    }

    // Ownership and captured output survive moves; moved-from objects reap nothing.
    shrn::RunOptions to_file;
    to_file.stdout_file = dir / "moved.out";
    auto src = shrn::spawn({self, "--helper=emit"}, to_file);
    CHECK(src.error().empty(), "spawn a child for the move test");
    if (src.error().empty()) {
        shrn::Process slot;
        {
            shrn::Process moved(std::move(src));
            slot = std::move(moved);
        }  // the moved-from objects are destroyed while the child is still unwaited
        auto& r = slot.wait();
        CHECK(r.launched() && r.exit_code == 7 && r.stderr_output == "ERR\n",
              "a moved Process keeps its child and captured stderr");
        CHECK(slurp(dir / "moved.out") == "OUT\n",
              "stdout_file redirection survives the move");
    }

    // The destructor waits for the child and reaps it.
    auto dbase = (dir / "proc_dtor").string();
    std::optional<pid_t> dpid;
    {
        auto owned = shrn::spawn({self, "--helper=pid-sleep-touch", dbase, "300"});
        CHECK(owned.error().empty(), "spawn a child for the destructor test");
        dpid = child_pid(dbase);
    }
    CHECK(fs::exists(dbase + ".late"), "the destructor waits for the pending child");
    if (dpid) {
        CHECK(reaped(*dpid), "the destructor leaves no zombie behind");
        force_kill(*dpid);
    }

    // A capturing reader drains a full pipe while the caller does unrelated work.
    auto flood_base = (dir / "proc_flood").string();
    auto flood = shrn::spawn({self, "--helper=emit-bytes", "1048576", flood_base});
    CHECK(flood.error().empty(), "spawn a stderr flood");
    if (flood.error().empty()) {
        auto pid = child_pid(flood_base);
        bool finished_before_wait = wait_until([&] { return !flood.running(); }, 2s);
        CHECK(finished_before_wait, "capture reader lets the child finish before wait()");
        if (!finished_before_wait && pid) force_kill(*pid);
        auto& r = flood.wait();
        CHECK(r.launched() && r.success() && r.stderr_output == std::string(1048576, 'x'),
              "captured stderr past the pipe capacity is drained by the reader");
    }

    // stdout_file is plain redirection: no reader is needed to keep the child moving.
    shrn::RunOptions big_out;
    big_out.stdout_file = dir / "spawn_big.out";
    big_out.capture_stderr = false;
    auto redirected = shrn::spawn({self, "--helper=emit-out-bytes", "1048576"}, big_out);
    CHECK(redirected.error().empty(), "spawn a stdout flood without capture");
    if (redirected.error().empty()) {
        auto& r = redirected.wait();
        CHECK(r.launched() && r.success() && r.stderr_output.empty(),
              "capture_stderr=false leaves stderr_output empty");
        CHECK(slurp(dir / "spawn_big.out") == std::string(1048576, 'o'),
              "stdout_file receives the whole stream without a reader");
    }
}

// Outcome
static void test_outcome(const fs::path& dir) {
    std::fprintf(stderr, "outcome\n");

    auto present = dir / "present.txt";
    auto blank = dir / "blank.txt";
    auto absent = dir / "absent.txt";
    write_file(present, "@read\n");
    write_file(blank, "");

    CHECK(shrn::Outcome().ok() && shrn::stage("s").name() == "s", "empty stage is ok and keeps its name");
    CHECK(shrn::Outcome().expect_success(0).ok(), "expect_success(0) passes");
    auto es = shrn::Outcome().expect_success(2);
    CHECK(!es.ok() && es.detail() == "exited with code 2", "expect_success(nonzero) fails");

    auto o1 = shrn::Outcome().expect_file(present, shrn::file_non_empty, "non-empty");
    CHECK(o1.ok(), "existing non-empty passes");

    auto o2 = shrn::Outcome().expect_file(absent, shrn::file_non_empty, "non-empty");
    CHECK(!o2.ok() && o2.detail() == "missing: " + absent.string(), "missing reported before emptiness");

    auto o3 = shrn::Outcome().expect_file(blank, shrn::file_non_empty, "non-empty");
    CHECK(!o3.ok() && o3.detail() == "not non-empty: " + blank.string(), "empty reported");


    auto is_fastq = [](const fs::path& p) { return std::ifstream(p).get() == '@'; };
    auto o5 = shrn::Outcome().expect_file(present, is_fastq, "FASTQ");
    CHECK(o5.ok(), "predicate passes");
    auto o6 = shrn::Outcome().expect_file(blank, is_fastq, "FASTQ");
    CHECK(!o6.ok() && o6.detail() == "not FASTQ: " + blank.string(), "predicate failure message");

    auto o7 = shrn::Outcome().expect_file(absent).expect_file(blank, shrn::file_non_empty, "non-empty");
    CHECK(o7.detail() == "missing: " + absent.string(), "first failure wins");

    CHECK(shrn::Outcome().expect_which("sh").ok(), "expect_which finds sh");
    auto et = shrn::Outcome().expect_which("shrn-definitely-not-a-command");
    CHECK(!et.ok() && et.detail() == "executable not found: shrn-definitely-not-a-command", "expect_which failure");

    int ran = 0;
    shrn::Outcome().or_execute([&] { ++ran; });
    CHECK(ran == 0, "or_execute skipped on success");
    shrn::Outcome().expect(false, "x").or_execute([&] { ++ran; });
    CHECK(ran == 1, "or_execute runs on failure");

    auto o8 = shrn::Outcome().expect(false, "custom");
    CHECK(!o8.ok() && o8.detail() == "custom", "expect(bool) records detail");

    auto rc = shrn::proc({"sh", "-c", "echo bad >&2; exit 1"});
    CHECK(!rc.ok() && rc.detail() == "sh: exited with code 1" && rc.stderr_output() == "bad\n",
          "proc maps failed child with argv[0] prefix");
    auto rl = shrn::proc({"shrn-definitely-not-a-command"});
    CHECK(!rl.ok() && rl.detail().rfind("shrn-definitely-not-a-command: cannot execute: ", 0) == 0,
          "proc maps launch error");

    // Failed preconditions prevent execution; output errors include the command name.
    auto marker = dir / "marker.txt";
    auto gated = shrn::stage("gated")
                     .expect_file(absent)
                     .proc({"sh", "-c", "echo ran > " + marker.string()})
                     .expect_file(marker);
    CHECK(!gated.ok() && gated.detail() == "missing: " + absent.string(), "failed pre-check keeps its detail");
    CHECK(!fs::exists(marker), "failed pre-check prevents the spawn");

    auto post = shrn::stage("post").proc({"true"}).expect_file(absent);
    CHECK(!post.ok() && post.detail() == "true: missing: " + absent.string(), "post-run check prefixed with argv[0]");

    auto chain = shrn::stage("chain")
                     .proc({"sh", "-c", "echo one > " + marker.string()})
                     .expect_file(marker, shrn::file_non_empty, "non-empty")
                     .proc({"sh", "-c", "exit 4"})
                     .proc({"sh", "-c", "rm " + marker.string()});
    CHECK(!chain.ok() && chain.detail() == "sh: exited with code 4", "second command failure reported");
    CHECK(fs::exists(marker), "third command not spawned after failure");

    // Stage name flows to the spawn hook (default hook, then per-run override).
    std::string hooked;
    shrn::default_spawn_hook() = [&](std::string_view st, std::string_view cmd) {
        hooked = std::string(st) + "|" + std::string(cmd);
    };
    shrn::stage("hooked stage").proc({"true"});
    CHECK(hooked == "hooked stage|true", "default_spawn_hook receives stage name");
    shrn::RunOptions override_hook;
    override_hook.on_spawn = [&](std::string_view, std::string_view) { hooked = "override"; };
    shrn::stage("hooked stage").proc({"true"}, override_hook);
    CHECK(hooked == "override", "RunOptions::on_spawn overrides the default hook");
    shrn::default_spawn_hook() = nullptr;

    // or_die_if(false) warns and continues; or_die_if(true) exits — exercised in a child.
    int warned = 0;
    shrn::stage("soft stage").expect(false, "x").or_die_if(false).or_execute([&] { ++warned; });
    CHECK(warned == 1, "or_die_if(false) continues the chain");

    pid_t pid = fork();
    if (pid == 0) {
        shrn::stage("hard stage").with_error("boom").expect(false, "boom").or_die_if(true);
        _exit(0);  // not reached
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE, "or_die_if(true) exits with EXIT_FAILURE");
}

// Outcome: asynchronous commands
static void test_outcome_async(const fs::path& dir) {
    std::fprintf(stderr, "outcome async\n");
    const std::string self = g_self_exe.string();

    // Two stages start their commands before either result is awaited.
    auto ma = (dir / "out_a.mark").string();
    auto mb = (dir / "out_b.mark").string();
    auto first = shrn::stage("first").proc({self, "--helper=rendezvous", ma, mb});
    auto second = shrn::stage("second").proc({self, "--helper=rendezvous", mb, ma});
    first.wait();
    second.wait();
    CHECK(first.ok() && second.ok(), "proc() returns before the child exits");

    // The next proc waits for the previous command; expectations wait too.
    auto seq = dir / "seq.txt";
    auto ordered = shrn::stage("ordered")
                       .proc({self, "--helper=sleep-then-write", seq.string(), "250"})
                       .proc({"sh", "-c", "printf second >> " + seq.string()})
                       .expect_file(seq, shrn::file_non_empty, "non-empty");
    CHECK(ordered.ok(), "sequenced commands both succeed");
    CHECK(slurp(seq) == "firstsecond",
          "a following command starts only after the previous one finished");

    // A deferred predicate is evaluated once, after the command exits.
    auto late = dir / "deferred.txt";
    int evaluations = 0;
    auto deferred = shrn::stage("deferred")
                        .proc({self, "--helper=sleep-then-write", late.string(), "250"})
                        .expect([&] { ++evaluations; return fs::exists(late); }, "output written");
    CHECK(deferred.ok() && evaluations == 1, "deferred predicate runs once after the command exits");

    int skipped = 0;
    auto gated = shrn::stage("gated").expect(false, "pre").expect([&] { ++skipped; return true; }, "never");
    CHECK(!gated.ok() && skipped == 0, "deferred predicate is skipped after a recorded failure");

    // A failed command prevents the next one from starting.
    auto blocked = dir / "blocked.mark";
    auto failing = shrn::stage("failing")
                       .proc({"sh", "-c", "echo boom >&2; exit 4"})
                       .proc({"sh", "-c", "touch " + blocked.string()});
    CHECK(!failing.ok() && failing.detail() == "sh: exited with code 4", "failed command is reported");
    CHECK(failing.stderr_output() == "boom\n", "stderr of the failed command is kept");
    CHECK(!fs::exists(blocked), "no command starts after a failure");

    // running() and wait_for() observe a live child; expiry is not a failure and kills nothing.
    auto lbase = (dir / "out_live").string();
    auto live = shrn::stage("live").proc({self, "--helper=pid-sleep-touch", lbase, "700"});
    auto lpid = child_pid(lbase);
    CHECK(lpid.has_value(), "stage child published its pid");
    CHECK(live.wait_for(50ms) == shrn::wait_status::timeout, "wait_for expires while the child runs");
    CHECK(live.running(), "running() reports the live child");
    CHECK(live.wait_for(5s) == shrn::wait_status::ready, "wait_for observes the exit");
    CHECK(!live.running(), "running() is false after the child exits");
    live.wait();
    CHECK(live.ok(), "an expired wait_for is not a stage failure");
    CHECK(fs::exists(lbase + ".late"), "wait_for does not kill the child");
    if (lpid) {
        CHECK(reaped(*lpid), "the stage reaps its child");
        force_kill(*lpid);
    }

    // expect_done_within() records a failure on expiry without terminating anything.
    auto sbase = (dir / "out_slow").string();
    auto slow = shrn::stage("slow").proc({self, "--helper=pid-sleep-touch", sbase, "700"});
    auto spid = child_pid(sbase);
    auto t0 = std::chrono::steady_clock::now();
    slow.expect_done_within(50ms);
    int notified = 0;
    slow.or_execute([&] { ++notified; });
    auto elapsed = std::chrono::steady_clock::now() - t0;
    std::string timed_detail = slow.detail();
    CHECK(!slow.ok() && !timed_detail.empty(), "expect_done_within records a failure on expiry");
    CHECK(notified == 1 && elapsed < 500ms, "a timed failure is actionable without waiting for the child");
    CHECK(slow.running(), "expect_done_within does not terminate the child");
    slow.wait();
    CHECK(!slow.ok() && slow.detail() == timed_detail, "wait() keeps the recorded failure");
    CHECK(fs::exists(sbase + ".late"), "wait() after a failure still finishes the child");
    if (spid) {
        CHECK(reaped(*spid), "the child finished by wait() is reaped");
        force_kill(*spid);
    }

    // or_terminate() kills the active child and keeps the original failure.
    auto tbase = (dir / "out_term").string();
    auto doomed = shrn::stage("doomed").proc({self, "--helper=pid-sleep-touch", tbase, "4000"});
    auto tpid = child_pid(tbase);
    t0 = std::chrono::steady_clock::now();
    doomed.expect_done_within(100ms);
    std::string doomed_detail = doomed.detail();
    doomed.or_terminate();
    elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(!doomed.ok() && doomed.detail() == doomed_detail, "or_terminate preserves the original failure");
    CHECK(elapsed < 2s, "or_terminate does not wait the child out");
    CHECK(!fs::exists(tbase + ".late"), "or_terminate kills the child before it finishes");
    if (tpid) {
        CHECK(pid_gone(*tpid) && reaped(*tpid), "or_terminate reaps the child");
        force_kill(*tpid);
    }

    // With no recorded failure there is nothing to terminate.
    auto kbase = (dir / "out_keep").string();
    auto healthy = shrn::stage("healthy").proc({self, "--helper=pid-sleep-touch", kbase, "200"});
    healthy.or_terminate();
    healthy.wait();
    CHECK(healthy.ok() && fs::exists(kbase + ".late"), "or_terminate leaves a passing stage's child alone");

    // The destructor waits for the pending command; the moved-from temporary must not.
    auto obase = (dir / "out_dtor").string();
    std::optional<pid_t> opid;
    {
        auto job = shrn::stage("dtor").proc({self, "--helper=pid-sleep-touch", obase, "300"});
        opid = child_pid(obase);
        CHECK(job.running() && !fs::exists(obase + ".late"),
              "the moved stage owns a still-running command");
    }
    CHECK(fs::exists(obase + ".late"), "the Outcome destructor waits for the pending command");
    if (opid) {
        CHECK(reaped(*opid), "the Outcome destructor reaps the child");
        force_kill(*opid);
    }

    // Move assignment transfers the pending command without a second reap.
    auto mbase = (dir / "out_move").string();
    shrn::Outcome slot;
    slot = shrn::stage("moved").proc({self, "--helper=pid-sleep-touch", mbase, "200"});
    auto mpid = child_pid(mbase);
    slot.wait();
    CHECK(slot.ok() && slot.name() == "moved" && fs::exists(mbase + ".late"),
          "a move-assigned stage completes its pending command");
    if (mpid) {
        CHECK(reaped(*mpid), "a move-assigned stage reaps its child once");
        force_kill(*mpid);
    }
    slot = shrn::stage("reused").proc({"true"});
    CHECK(slot.ok() && slot.name() == "reused", "a completed stage can be replaced");

    // Calls wait for pending commands and forward callable arguments.
    auto appended = dir / "call_seq.txt";
    auto suffix = std::make_unique<std::string>("second");
    std::string echoed;
    auto sequenced = shrn::stage("calling")
                         .proc({self, "--helper=sleep-then-write", appended.string(), "250"})
                         .call(
                             [target = std::make_unique<fs::path>(appended)](
                                 std::unique_ptr<std::string> text, std::string& out) {
                                 std::ofstream file(*target, std::ios::binary | std::ios::app);
                                 file << *text;
                                 out = *text;
                                 return 0;
                             },
                             std::move(suffix), echoed);
    CHECK(sequenced.ok(), "a stage whose call() returns zero succeeds");
    CHECK(slurp(appended) == "firstsecond",
          "call() runs only after the previous command finished");
    CHECK(echoed == "second" && !suffix,
          "call() forwards a move-only callable, a move-only argument and an lvalue output");

    // A nonzero return gates later calls and commands.
    auto after_call = dir / "call_gate.mark";
    auto untouched = std::make_unique<std::string>("kept");
    int invocations = 0;
    auto call_gate = shrn::stage("call gate")
                         .proc({"true"})
                         .call([] { return 3; })
                         .call([&](std::unique_ptr<std::string>) { ++invocations; return 0; },
                               std::move(untouched))
                         .proc({"touch", after_call.string()});
    CHECK(!call_gate.ok(), "a nonzero function return fails the stage");
    CHECK(invocations == 0 && untouched && *untouched == "kept",
          "a skipped call() neither runs nor moves its arguments");
    CHECK(!fs::exists(after_call), "no command starts after a failed call()");

    // after() joins prerequisites that were started concurrently, with no prior wait.
    auto ja = (dir / "join_a.mark").string();
    auto jb = (dir / "join_b.mark").string();
    auto left = shrn::stage("left").proc({self, "--helper=rendezvous", ja, jb});
    auto right = shrn::stage("right").proc({self, "--helper=rendezvous", jb, ja});
    CHECK(shrn::stage("both").after(left, right).ok(),
          "after() joins prerequisites that were running concurrently");

    // Even a prerequisite already marked failed must be joined.
    auto jbase = (dir / "join_live").string();
    auto release = (dir / "join_release").string();
    auto timed = shrn::stage("timed").proc({self, "--helper=rendezvous", jbase + ".pid", release});
    auto jpid = child_pid(jbase);
    CHECK(jpid.has_value(), "the joining prerequisite is ready");
    timed.expect_done_within(0ms);
    CHECK(!timed.ok() && timed.running(), "the prerequisite failed its deadline while still running");
    auto original_failure = timed.detail();
    auto early = shrn::stage("early").call([] { return 5; });
    auto releaser = shrn::proc({self, "--helper=sleep-then-write", release, "250"});
    int dependent_calls = 0;
    auto merged = shrn::stage("merged").after(early, timed).call([&] { ++dependent_calls; return 0; });
    CHECK(!merged.ok() && dependent_calls == 0, "a failed prerequisite prevents dependent work");
    CHECK(!timed.running() && fs::exists(release),
          "after() waits for a failed-but-running prerequisite");
    releaser.wait();
    CHECK(releaser.ok(), "the release helper completed");
    if (jpid) {
        CHECK(reaped(*jpid), "after() reaps every joined prerequisite");
        force_kill(*jpid);
    }

    // The first failure in argument order wins; joined prerequisites stay usable.
    auto bad_one = shrn::stage("join-first").proc({"sh", "-c", "exit 2"});
    auto bad_two = shrn::stage("join-second").proc({"sh", "-c", "exit 3"});
    auto in_order = shrn::stage("in order").after(bad_one, bad_two);
    auto reversed = shrn::stage("reversed").after(bad_two, bad_one);
    CHECK(!in_order.ok() && in_order.detail().find(bad_one.name()) != std::string::npos,
          "after() keeps the first failure in argument order");
    CHECK(!reversed.ok() && reversed.detail().find(bad_two.name()) != std::string::npos,
          "an already joined prerequisite can be reused by another stage");

    // A fatal failure cleans up the stage's active child before exiting.
    auto fbase = (dir / "out_die").string();
    pid_t forked = ::fork();
    if (forked == 0) {
        auto fatal = shrn::stage("fatal").proc({self, "--helper=pid-sleep-touch", fbase, "4000"});
        if (!child_pid(fbase)) {
            fatal.expect_done_within(0ms).or_terminate();
            _exit(97);
        }
        fatal.expect_done_within(100ms).or_die_if(true);
        _exit(0);  // not reached
    }
    int status = 0;
    waitpid(forked, &status, 0);
    bool died = WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE;
    CHECK(died, "or_die_if(true) exits after a timed failure");
    auto fpid = died ? child_pid(fbase) : std::optional<pid_t>();
    CHECK(fpid.has_value(), "the dying stage's child published its pid");
    if (fpid) {
        CHECK(pid_gone(*fpid), "or_die_if(true) cleans up the active child before exiting");
        force_kill(*fpid);
    }
    CHECK(!fs::exists(fbase + ".late"), "the cleaned-up child never finished its work");
}

// Outcome: stage-scoped temp files
static void test_outcome_temps() {
    std::fprintf(stderr, "outcome temps\n");

    // Tokens resolve inside one private directory; the same name is the same file,
    // so a later command consumes an earlier command's output and sidecars sit beside it.
    fs::path dir;
    int measured = 0;
    {
        auto s = shrn::stage("map reads")
                     .proc({"sh", "-c", "printf hello > \"$0\"", shrn::temp_file{"aln.sam"}})
                     .expect_file(shrn::temp_file{"aln.sam"}, shrn::file_non_empty, "non-empty")
                     .proc({"sh", "-c", "cp \"$0\" \"$1\" && touch \"$1.bai\"",
                            shrn::temp_file{"aln.sam"}, shrn::temp_file{"aln.bam"}})
                     .expect_file(shrn::temp_file{"aln.bam.bai"})
                     .call([](const fs::path& p, int& out) {
                         out = static_cast<int>(slurp(p).size());
                         return 0;
                     }, shrn::temp_file{"aln.bam"}, measured);
        CHECK(s.ok(), "a stage chains commands through temp tokens");
        CHECK(measured == 5, "call() receives a temp token as its resolved path");
        dir = s.temp_path("aln.sam").parent_path();
        CHECK(fs::is_directory(dir) && dir.filename().string().rfind("shrn_map_reads_", 0) == 0,
              "the stage temp directory is named after the stage");
        CHECK(s.temp_path("aln.sam") == dir / "aln.sam" && s.temp_path("aln.bam") == dir / "aln.bam",
              "temp_path() exposes the resolved paths");
    }
    CHECK(!fs::exists(dir), "the stage destructor removes its temp directory");

    // keep_temps leaves the directory in place.
    fs::path kept;
    {
        auto k = shrn::stage("keep", {.keep_temps = true}).proc({"touch", shrn::temp_file{"x"}});
        CHECK(k.ok(), "keep_temps stage runs");
        kept = k.temp_path("x").parent_path();
    }
    CHECK(fs::exists(kept / "x"), "keep_temps preserves the directory and its files");
    fs::remove_all(kept);

    // Two stages with the same name never share a directory.
    auto a = shrn::stage("dup");
    auto b = shrn::stage("dup");
    CHECK(a.temp_path("f").parent_path() != b.temp_path("f").parent_path(),
          "same-named stages get distinct temp directories");

    // Invalid names fail the stage without creating anything.
    auto escape = shrn::stage("escape").proc({"true", shrn::temp_file{"../out"}});
    CHECK(!escape.ok() && escape.detail().find("invalid temp file name") != std::string::npos,
          "a temp name with a path separator is rejected");
    CHECK(!shrn::stage("blank").expect_file(shrn::temp_file{""}).ok(), "an empty temp name is rejected");

    // A moved stage carries its directory; the moved-from shell must not remove it.
    fs::path moved_dir;
    {
        auto src = shrn::stage("mover").proc({"touch", shrn::temp_file{"m"}}).wait();
        moved_dir = src.temp_path("m").parent_path();
        shrn::Outcome dst = std::move(src);
        CHECK(fs::exists(moved_dir / "m"), "moving a stage does not disturb its temp files");
    }
    CHECK(!fs::exists(moved_dir), "the moved-to stage owns and removes the temp directory");

    // Plain arguments still forward untouched through call(); vector<string> proc is unaffected.
    int side = 0;
    CHECK(shrn::stage("plain").call([](int& v) { v = 7; return 0; }, side).ok() && side == 7,
          "call() forwards non-token arguments as before");
    std::vector<std::string> cmd = {"true"};
    CHECK(shrn::proc(cmd).ok(), "the vector<string> proc overload still resolves");

    auto u = shrn::uuid4();
    CHECK(u.size() == 36 && u[14] == '4' && u != shrn::uuid4(), "uuid4() yields distinct version-4 text");
}

// StageTemplate: recorded stages with slots
static void test_stage_templates(const fs::path& dir) {
    std::fprintf(stderr, "stage templates\n");
    write_file(dir / "tpl_in1.txt", "one");
    write_file(dir / "tpl_in2.txt", "two");

    // One recipe launched twice: each launch gets its own temps and produces its own output.
    const auto copy = shrn::StageTemplate("copy")
                          .expect_which("cp")
                          .expect_file(shrn::slot{"src"}, shrn::file_non_empty, "non-empty")
                          .proc({"cp", shrn::slot{"src"}, shrn::temp_file{"scratch"}})
                          .proc({"cp", shrn::temp_file{"scratch"}, shrn::slot{"dst"}})
                          .expect_file(shrn::slot{"dst"});
    fs::path t1, t2;
    {
        auto a = copy.launch({{"src", dir / "tpl_in1.txt"}, {"dst", dir / "tpl_out1.txt"}}).wait();
        auto b = copy.launch({{"src", dir / "tpl_in2.txt"}, {"dst", dir / "tpl_out2.txt"}}).wait();
        CHECK(a.ok() && b.ok(), "a template executes as working stages");
        t1 = a.temp_path("scratch").parent_path();
        t2 = b.temp_path("scratch").parent_path();
        CHECK(t1 != t2, "each launch owns a separate temp directory");
    }
    CHECK(slurp(dir / "tpl_out1.txt") == "one" && slurp(dir / "tpl_out2.txt") == "two",
          "slots substitute per launch");
    CHECK(!fs::exists(t1) && !fs::exists(t2), "executed stages clean up their temps");

    // Binding errors are detected before anything runs.
    auto unbound = copy.launch({{"src", dir / "tpl_in1.txt"}});
    CHECK(!unbound.ok() && unbound.detail() == "unbound slot: 'dst'", "an unbound required slot fails launch");
    auto typo = copy.launch({{"src", dir / "tpl_in1.txt"}, {"dts", dir / "tpl_never"}});
    CHECK(!typo.ok() && typo.detail() == "unknown binding: 'dts'", "a binding no slot uses fails launch");
    CHECK(!fs::exists(dir / "tpl_never"), "a failed launch runs no command");

    // Defaults satisfy an unbound slot and yield to an explicit binding.
    const auto dflt = shrn::StageTemplate("default")
                          .proc({"sh", "-c", "printf \"$0\" > \"$1\"", shrn::slot{"text", "fallback"}, shrn::slot{"out"}});
    CHECK(dflt.launch({{"out", dir / "tpl_d1"}}).wait().ok() && slurp(dir / "tpl_d1") == "fallback",
          "an unbound slot uses its default");
    CHECK(dflt.launch({{"out", dir / "tpl_d2"}, {"text", "given"}}).wait().ok() && slurp(dir / "tpl_d2") == "given",
          "a binding overrides the default");

    // Optional groups are atomic: present only when every slot inside is bound.
    const auto opt = shrn::StageTemplate("optional")
                         .proc({"sh", "-c", "printf \"%s\" \"$@\" > \"$0\"", shrn::slot{"out"},
                                "-1", shrn::slot{"r1"}, shrn::optional{"-2", shrn::slot{"r2"}}});
    CHECK(opt.launch({{"out", dir / "tpl_o1"}, {"r1", "A"}}).wait().ok() && slurp(dir / "tpl_o1") == "-1A",
          "an optional group with an unbound slot is dropped whole");
    CHECK(opt.launch({{"out", dir / "tpl_o2"}, {"r1", "A"}, {"r2", "B"}}).wait().ok() && slurp(dir / "tpl_o2") == "-1A-2B",
          "an optional group with all slots bound is included whole");
    const auto mixed = shrn::StageTemplate("mixed").proc({"true", shrn::slot{"x"}, shrn::optional{"-x", shrn::slot{"x"}}});
    CHECK(!mixed.launch({}).ok(), "a slot also used outside a group stays required");

    // The caller decides at launch whether an output is scratch or a deliverable.
    const auto emit = shrn::StageTemplate("emit").proc({"sh", "-c", "printf hi > \"$0\"", shrn::slot{"out"}});
    {
        auto scratch = emit.launch({{"out", shrn::temp_file{"s.txt"}}}).wait();
        CHECK(scratch.ok() && slurp(scratch.temp_path("s.txt")) == "hi", "a slot bound to a temp token writes into the stage temps");
    }
    CHECK(emit.launch({{"out", dir / "tpl_deliver.txt"}}).wait().ok() && slurp(dir / "tpl_deliver.txt") == "hi",
          "the same slot bound to a path writes a deliverable");

    // proc_to redirects stdout into a slot or a temp token.
    const auto redirect = shrn::StageTemplate("redirect")
                              .proc_to(shrn::slot{"log"}, {"sh", "-c", "echo captured"})
                              .expect_file(shrn::slot{"log"}, shrn::file_non_empty, "non-empty");
    CHECK(redirect.launch({{"log", dir / "tpl_log.txt"}}).wait().ok() && slurp(dir / "tpl_log.txt") == "captured\n",
          "proc_to binds stdout to a slot");
    const auto redirect_tmp = shrn::StageTemplate("redirect tmp")
                                  .proc_to(shrn::temp_file{"o"}, {"echo", "x"})
                                  .expect_file(shrn::temp_file{"o"});
    CHECK(redirect_tmp.launch({}).wait().ok(), "proc_to binds stdout to a temp token");

    fs::path kept;
    {
        auto k = shrn::StageTemplate("keep", {.keep_temps = true}).proc({"touch", shrn::temp_file{"k"}}).launch({}).wait();
        kept = k.temp_path("k").parent_path();
    }
    CHECK(fs::exists(kept / "k"), "keep_temps carries through launch");
    fs::remove_all(kept);

    // many{} binds a list that expands in place; expect_file over it checks every element.
    write_file(dir / "tpl_p1", "a");
    write_file(dir / "tpl_p2", "b");
    write_file(dir / "tpl_q", "q");
    write_file(dir / "tpl_blank", "");
    const std::vector<fs::path> plasmids = {dir / "tpl_p1", dir / "tpl_p2"};
    const auto propagate = shrn::StageTemplate("propagate")
                               .expect_file(shrn::slot{"query"}, shrn::file_non_empty, "non-empty")
                               .expect_file(shrn::many{"plasmids"}, shrn::file_non_empty, "non-empty")
                               .proc({"sh", "-c", "printf \"%s|\" \"$@\" > \"$0\"", shrn::slot{"out"},
                                      shrn::slot{"query"}, shrn::many{"plasmids"}, "-t", shrn::slot{"threads", "4"}})
                               .expect_file(shrn::slot{"out"});
    CHECK(propagate.launch({{"query", dir / "tpl_q"}, {"plasmids", plasmids}, {"out", dir / "tpl_m1"}}).wait().ok() &&
              slurp(dir / "tpl_m1") == (dir / "tpl_q").string() + "|" + (dir / "tpl_p1").string() + "|" +
                                          (dir / "tpl_p2").string() + "|-t|4|",
          "a list expands in place between scalar slots");
    CHECK(propagate.launch({{"query", dir / "tpl_q"}, {"plasmids", std::vector<fs::path>{}}, {"out", dir / "tpl_m2"}}).wait().ok() &&
              slurp(dir / "tpl_m2") == (dir / "tpl_q").string() + "|-t|4|",
          "a bound empty list expands to nothing");
    auto element = propagate.launch({{"query", dir / "tpl_q"}, {"plasmids", std::vector<fs::path>{dir / "tpl_p1", dir / "tpl_blank"}},
                                   {"out", dir / "tpl_m3"}}).wait();
    CHECK(!element.ok() && element.detail() == "not non-empty: " + (dir / "tpl_blank").string(),
          "a per-element check names the failing element");
    CHECK(!fs::exists(dir / "tpl_m3"), "a failed element check runs no command");
    auto unbound_list = propagate.launch({{"query", dir / "tpl_q"}, {"out", dir / "tpl_m4"}});
    CHECK(!unbound_list.ok() && unbound_list.detail() == "unbound slot: 'plasmids'", "an unbound list is required");
    auto scalar_to_list = propagate.launch({{"query", dir / "tpl_q"}, {"plasmids", "one"}, {"out", dir / "tpl_m5"}});
    CHECK(!scalar_to_list.ok() && scalar_to_list.detail() == "scalar bound to list slot: 'plasmids'",
          "binding a scalar to many{} fails launch");
    auto list_to_scalar = propagate.launch({{"query", plasmids}, {"plasmids", plasmids}, {"out", dir / "tpl_m6"}});
    CHECK(!list_to_scalar.ok() && list_to_scalar.detail() == "list bound to slot: 'query'",
          "binding a list to slot{} fails launch");

    // many{} inside an optional group: dropped when unbound, present when bound even if empty.
    const auto extras = shrn::StageTemplate("extras")
                            .proc({"sh", "-c", "printf \"%s|\" \"$@\" > \"$0\"", shrn::slot{"out"}, "x",
                                   shrn::optional{"--extra", shrn::many{"more"}}});
    CHECK(extras.launch({{"out", dir / "tpl_x1"}}).wait().ok() && slurp(dir / "tpl_x1") == "x|",
          "an optional group with an unbound list is dropped");
    CHECK(extras.launch({{"out", dir / "tpl_x2"}, {"more", std::vector<fs::path>{dir / "tpl_p1"}}}).wait().ok() &&
              slurp(dir / "tpl_x2") == "x|--extra|" + (dir / "tpl_p1").string() + "|",
          "an optional group with a bound list is included");
    CHECK(extras.launch({{"out", dir / "tpl_x3"}, {"more", std::vector<fs::path>{}}}).wait().ok() &&
              slurp(dir / "tpl_x3") == "x|--extra|",
          "a bound empty list keeps its optional group");

    // A template call() step receives placeholder arguments as resolved paths.
    const auto filter = shrn::StageTemplate("filter")
                            .call([](const fs::path& scratch, const fs::path& tag) {
                                write_file(scratch, "filtered:" + tag.string());
                                return 0;
                            }, shrn::temp_file{"filtered"}, shrn::slot{"tag"})
                            .expect_file(shrn::temp_file{"filtered"}, shrn::file_non_empty, "non-empty")
                            .proc({"cp", shrn::temp_file{"filtered"}, shrn::slot{"out"}});
    CHECK(filter.launch({{"tag", "abc"}, {"out", dir / "tpl_call_out"}}).wait().ok() &&
              slurp(dir / "tpl_call_out") == "filtered:abc",
          "a template call() resolves temp tokens and slots to paths");
    auto failing = shrn::StageTemplate("failing").call([](const fs::path&) { return 4; }, shrn::slot{"p"}).launch({{"p", "x"}});
    CHECK(!failing.ok() && failing.detail() == "function returned code 4", "a nonzero template call() fails the stage");
    auto list_arg = shrn::StageTemplate("list arg").call([](const fs::path&) { return 0; }, shrn::many{"xs"})
                        .launch({{"xs", std::vector<fs::path>{dir / "tpl_p1"}}});
    CHECK(!list_arg.ok() && list_arg.detail().find("call() arguments must be") != std::string::npos,
          "a list placeholder is rejected as a call() argument");
}

// Stage temps placed under a caller-chosen parent directory
static void test_temp_dir_option(const fs::path& dir) {
    std::fprintf(stderr, "temp_dir option\n");
    const fs::path parent = dir / "custom" / "tmp";  // parents must be created on demand

    fs::path placed;
    {
        auto s = shrn::stage("placed", {.temp_dir = parent}).proc({"touch", shrn::temp_file{"f"}}).wait();
        CHECK(s.ok(), "a stage with temp_dir runs");
        placed = s.temp_path("f").parent_path();
        CHECK(placed.parent_path() == parent && fs::exists(placed / "f"),
              "the stage temp directory is created under temp_dir");
        CHECK(placed.filename().string().rfind("shrn_placed_", 0) == 0, "temp_dir keeps the unique directory name");
    }
    CHECK(!fs::exists(placed) && fs::is_directory(parent), "the stage directory is removed; the parent stays");

    fs::path kept;
    {
        auto k = shrn::stage("kept", {.keep_temps = true, .temp_dir = parent}).proc({"touch", shrn::temp_file{"k"}}).wait();
        kept = k.temp_path("k").parent_path();
    }
    CHECK(fs::exists(kept / "k") && kept.parent_path() == parent, "keep_temps preserves the directory under temp_dir");

    write_file(dir / "not_a_dir", "x");
    auto bad = shrn::stage("bad parent", {.temp_dir = dir / "not_a_dir" / "sub"}).proc({"touch", shrn::temp_file{"z"}});
    CHECK(!bad.ok() && bad.detail().find("cannot create temp dir") != std::string::npos,
          "an unusable temp_dir fails the stage before any command runs");

    fs::path templated;
    {
        auto t = shrn::StageTemplate("templated", {.temp_dir = parent}).proc({"touch", shrn::temp_file{"t"}}).launch({}).wait();
        CHECK(t.ok(), "a template with temp_dir launches");
        templated = t.temp_path("t").parent_path();
    }
    CHECK(templated.parent_path() == parent && !fs::exists(templated), "temp_dir flows through StageTemplate");
}

int main(int argc, char** argv) {
    init_self_exe(argv[0]);
    if (argc >= 2) {
        std::string_view first(argv[1]);
        constexpr std::string_view prefix = "--helper=";
        if (first.rfind(prefix, 0) == 0)
            return helper_main(std::string(first.substr(prefix.size())), argc >= 3 ? argv[2] : "",
                               argc >= 4 ? argv[3] : "");
    }
    std::string tmpl = (fs::temp_directory_path() / "shrn_test_XXXXXX").string();
    if (!::mkdtemp(tmpl.data())) {
        std::fprintf(stderr, "cannot create temp dir\n");
        return 2;
    }
    const fs::path root(tmpl);

    test_files(root);
    test_which(root);
    test_process(root);
    test_process_edges(root);
    test_process_spawn(root);
    test_outcome(root);
    test_outcome_async(root);
    test_outcome_temps();
    test_stage_templates(root);
    test_temp_dir_option(root);

    std::error_code ec;
    fs::remove_all(root, ec);
    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
