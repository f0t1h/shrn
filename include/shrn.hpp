// shrn: run commands and check their outputs. C++20, POSIX.
// Failures are reported as error strings carried by the result value.

#ifndef SHRN_HPP
#define SHRN_HPP

#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>


namespace shrn {

namespace fs = std::filesystem;

namespace detail {

/// Human-readable last-error text: errno when set, else the fallback.
inline std::string errno_string(std::string_view fallback = "I/O error") noexcept {
    int e = errno;
    return e != 0 ? std::string(std::strerror(e)) : std::string(fallback);
}


struct unique_fd {
    int fd = -1;
    unique_fd() = default;
    explicit unique_fd(int f) noexcept : fd(f) {}
    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;
    unique_fd(unique_fd&& o) noexcept : fd(o.fd) { o.fd = -1; }
    unique_fd& operator=(unique_fd&& o) noexcept {
        if (this != &o) {
            reset();
            fd = o.fd;
            o.fd = -1;
        }
        return *this;
    }
    ~unique_fd() { reset(); }
    void reset() noexcept {
        if (fd != -1) {
            ::close(fd);
            fd = -1;
        }
    }
    int release() noexcept {
        int f = fd;
        fd = -1;
        return f;
    }
    explicit operator bool() const noexcept { return fd != -1; }
};


}  // namespace detail

// File queries

/// Regular file that the current process may read.
[[nodiscard]] inline bool file_readable(const fs::path& p) noexcept {
    std::error_code ec;
    auto st = fs::status(p, ec);
    return !ec && fs::is_regular_file(st) && ::access(p.c_str(), R_OK) == 0;
}

/// Readable regular file with size > 0.
[[nodiscard]] inline bool file_non_empty(const fs::path& p) noexcept {
    if (!file_readable(p)) return false;
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    return !ec && sz > 0;
}



/// Create directory and parents; success if it already exists.
[[nodiscard]] inline bool ensure_directory(const fs::path& p) noexcept {
    std::error_code ec;
    fs::create_directories(p, ec);
    return !ec;
}


// Temporary directories (stage-internal; see temp_file)

namespace detail {

/// $TMPDIR if set and non-empty, else /tmp.
[[nodiscard]] inline fs::path temp_directory() {
    const char* t = std::getenv("TMPDIR");
    return (t && *t) ? fs::path(t) : fs::path("/tmp");
}

/// Owns a temp directory tree; removed recursively on destruction unless released.
class TempDir {
public:
    TempDir() = default;

    /// Atomically create `$TMPDIR/<prefix>XXXXXX` (mkdtemp). Failure yields an
    /// empty owner (operator bool is false); errno_string() describes why.
    [[nodiscard]] static TempDir create(std::string_view prefix) {
        std::string tmpl = (temp_directory() / std::string(prefix)).string();
        tmpl += "XXXXXX";
        if (!::mkdtemp(tmpl.data())) return {};
        return TempDir(fs::path(std::move(tmpl)));
    }

    ~TempDir() { reset(); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&& o) noexcept : path_(std::move(o.path_)) { o.path_.clear(); }
    TempDir& operator=(TempDir&& o) noexcept {
        if (this != &o) {
            reset();
            path_ = std::move(o.path_);
            o.path_.clear();
        }
        return *this;
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    explicit operator bool() const noexcept { return !path_.empty(); }

    /// Remove the tree and clear ownership; ignore cleanup errors.
    void reset() noexcept {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove_all(path_, ec);
            path_.clear();
        }
    }
    /// Give up ownership without removing anything.
    fs::path release() noexcept {
        fs::path p = std::move(path_);
        path_.clear();
        return p;
    }

private:
    explicit TempDir(fs::path p) noexcept : path_(std::move(p)) {}
    fs::path path_;
};

}  // namespace detail

/// Random RFC 4122 version-4 UUID text; falls back to clock and pid entropy
/// if /dev/urandom is unavailable. Never throws.
[[nodiscard]] inline std::string uuid4() {
    unsigned char bytes[16];
    bool filled = false;
    if (int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC); fd != -1) {
        std::size_t have = 0;
        while (have < sizeof bytes) {
            ssize_t n = ::read(fd, bytes + have, sizeof bytes - have);
            if (n <= 0) {
                if (n == -1 && errno == EINTR) continue;
                break;
            }
            have += static_cast<std::size_t>(n);
        }
        ::close(fd);
        filled = have == sizeof bytes;
    }
    if (!filled) {
        auto seed = static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                    (static_cast<unsigned long long>(::getpid()) << 32);
        for (auto& b : bytes) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;  // xorshift64
            b = static_cast<unsigned char>(seed);
        }
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);  // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);  // variant 1
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += hex[bytes[i] >> 4];
        out += hex[bytes[i] & 0x0f];
    }
    return out;
}


// Process execution

/// Shell-style quoting for display only.
[[nodiscard]] inline std::string format_command(const std::vector<std::string>& args) {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) out += ' ';
        const std::string& a = args[i];
        bool quote = a.empty() || a.find_first_of(" \t\n\"'\\$`*?[]<>|;&(){}") != std::string::npos;
        if (!quote) {
            out += a;
            continue;
        }
        out += '\'';
        for (char c : a) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += '\'';
    }
    return out;
}

/// Resolve `name` against PATH (or verify it directly if it contains '/').
[[nodiscard]] inline std::optional<fs::path> which(std::string_view name) {
    if (name.empty()) return std::nullopt;
    auto executable = [](const fs::path& p) {
        std::error_code ec;
        return fs::is_regular_file(p, ec) && ::access(p.c_str(), X_OK) == 0;
    };
    if (name.find('/') != std::string_view::npos) {
        fs::path p(name);
        return executable(p) ? std::optional<fs::path>(std::move(p)) : std::nullopt;
    }
    const char* path_env = std::getenv("PATH");
    std::string default_path;
    if (!path_env) {
        auto size = ::confstr(_CS_PATH, nullptr, 0);
        if (size == 0) return std::nullopt;
        default_path.resize(size);
        ::confstr(_CS_PATH, default_path.data(), size);
        path_env = default_path.c_str();
    }
    std::string_view path(path_env);
    for (;;) {
        auto sep = path.find(':');
        std::string_view dir = path.substr(0, sep);
        fs::path candidate = fs::path(dir.empty() ? std::string_view(".") : dir) / name;
        if (executable(candidate)) return candidate;
        if (sep == std::string_view::npos) break;
        path.remove_prefix(sep + 1);
    }
    return std::nullopt;
}

/// Callback before starting a command: (stage name, formatted command).
using SpawnHook = std::function<void(std::string_view stage, std::string_view cmd)>;

/// Process-wide hook used when RunOptions::on_spawn is empty (e.g. a "[INFO] Running:" logger).
inline SpawnHook& default_spawn_hook() {
    static SpawnHook hook;
    return hook;
}

struct RunOptions {
    std::optional<fs::path> workdir;              ///< chdir before exec
    bool capture_stderr = true;                   ///< collect child's stderr into RunResult
    bool inherit_stdout = true;                   ///< false → /dev/null (ignored when stdout_file set)
    std::optional<fs::path> stdout_file;          ///< create/truncate; relative to caller, not workdir
    SpawnHook on_spawn;                           ///< overrides default_spawn_hook() for this run
};

struct RunResult {
    int exit_code = -1;        ///< WEXITSTATUS, or -1 if killed by a signal
    int signal = 0;            ///< WTERMSIG, 0 if exited normally
    std::string error;         ///< OS-level failure; empty when the child ran
    std::string stderr_output; ///< captured stderr (empty unless capture_stderr)

    /// True when the child was launched and no OS-level failure was recorded.
    [[nodiscard]] bool launched() const noexcept { return error.empty(); }
    /// True when launched and the child exited with code 0.
    [[nodiscard]] bool success() const noexcept { return launched() && exit_code == 0 && signal == 0; }

    /// "cannot launch: ..." / "exited with code N" / "killed by signal N (SIGKILL ...)"
    [[nodiscard]] std::string summary() const {
        if (!launched()) return "cannot launch: " + error;
        if (success()) return "success";
        if (signal != 0) {
            std::string msg = "killed by signal " + std::to_string(signal);
            switch (signal) {
                case SIGKILL: msg += " (SIGKILL - possibly OOM killed)"; break;
                case SIGTERM: msg += " (SIGTERM)"; break;
                case SIGSEGV: msg += " (SIGSEGV - segmentation fault)"; break;
                case SIGABRT: msg += " (SIGABRT - aborted)"; break;
                case SIGPIPE: msg += " (SIGPIPE - broken pipe)"; break;
                default: break;
            }
            return msg;
        }
        return "exited with code " + std::to_string(exit_code);
    }
};

/// Result of a bounded wait: the child was reaped, or the wait window elapsed.
enum class wait_status {
    ready,   ///< the child finished; its status is cached by Process::wait()
    timeout, ///< the window elapsed with the child still running (nothing was killed)
};

namespace detail {

inline bool set_cloexec(int fd) noexcept {
    int flags = ::fcntl(fd, F_GETFD);
    return flags != -1 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1;
}

/// Create a pipe with both ends close-on-exec; use pipe2() where available.
/// On failure `errno` describes the problem; the caller owns any opened ends.
inline bool make_cloexec_pipe(unique_fd& r, unique_fd& w) noexcept {
    int fds[2];
#if defined(__linux__) && defined(_GNU_SOURCE)
    if (::pipe2(fds, O_CLOEXEC) == 0) {
        r = unique_fd(fds[0]);
        w = unique_fd(fds[1]);
        return true;
    }
    if (errno != ENOSYS) return false;
#endif
    if (::pipe(fds) == -1) return false;
    r = unique_fd(fds[0]);
    w = unique_fd(fds[1]);
    return set_cloexec(r.fd) && set_cloexec(w.fd);
}

/// Move internal descriptors above 2 so dup2() cannot overwrite them
/// when the caller has closed a standard descriptor.
inline bool reserve_above_stdio(unique_fd& fd) noexcept {
    if (fd.fd < 0 || fd.fd > STDERR_FILENO) return true;
    int moved = ::fcntl(fd.fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved == -1) return false;
    fd = unique_fd(moved);  // closes the low descriptor
    return true;
}

inline pid_t waitpid_retry(pid_t pid, int* status, int options) noexcept {
    pid_t r;
    do {
        r = ::waitpid(pid, status, options);
    } while (r == -1 && errno == EINTR);
    return r;
}

/// Bound capture after child exit so descendants cannot delay the reader indefinitely.
inline std::size_t pipe_capacity(int fd) noexcept {
#ifdef F_GETPIPE_SZ
    int sz = ::fcntl(fd, F_GETPIPE_SZ);
    if (sz > 0) return static_cast<std::size_t>(sz);
#endif
    (void) fd;
    return std::size_t{1} << 20;
}


inline constexpr std::chrono::milliseconds wait_poll_interval{10};

/// Sleep at most `ms`; a signal may cut it short and the caller re-checks its deadline.
inline void nap(std::chrono::milliseconds ms) noexcept {
    if (ms.count() <= 0) return;
    timespec ts{static_cast<time_t>(ms.count() / 1000), static_cast<long>((ms.count() % 1000) * 1'000'000L)};
    ::nanosleep(&ts, nullptr);
}

// Stable storage for the reader. The owner reads data and errors only after join().
struct capture_state {
    unique_fd read_fd;             ///< stderr read end; closed by the worker when it stops
    unique_fd wake_r;              ///< stop-signal read end (polled by the worker)
    unique_fd wake_w;              ///< stop-signal write end (written by the owner)
    std::string data;              ///< captured stderr
    std::string error;             ///< poll/read failure description
};

// wake_r remains open until join, so this write cannot raise SIGPIPE.
inline void request_stop(capture_state& st) noexcept {
    if (st.wake_w.fd == -1) return;
    const char byte = 0;
    ssize_t n;
    do {
        n = ::write(st.wake_w.fd, &byte, 1);
    } while (n == -1 && errno == EINTR);
}

// Only the reader accesses read_fd, data, and error while active.
inline void capture_worker(capture_state* st) noexcept {
    char buffer[1 << 14];
    bool stopping = false;      ///< stop requested: take only what is already buffered
    std::size_t budget = 0;     ///< bytes still allowed while stopping
#if defined(__cpp_exceptions)
    try {
#endif
        for (;;) {
            pollfd pfds[2];
            int nfds = 0;
            pfds[nfds++] = pollfd{st->read_fd.fd, POLLIN, 0};
            if (!stopping && st->wake_r.fd != -1) pfds[nfds++] = pollfd{st->wake_r.fd, POLLIN, 0};

            int pr = ::poll(pfds, static_cast<nfds_t>(nfds), stopping ? 0 : -1);
            if (pr == -1) {
                if (errno == EINTR) continue;
                st->error = errno_string();
                break;
            }
            if (pr == 0) break;  // only possible while stopping: nothing left buffered
            if (nfds == 2 && pfds[1].revents != 0) {
                stopping = true;
                budget = pipe_capacity(st->read_fd.fd);
                continue;
            }
            if (pfds[0].revents == 0) continue;

            ssize_t got = ::read(st->read_fd.fd, buffer, sizeof buffer);
            if (got > 0) {
                st->data.append(buffer, static_cast<std::size_t>(got));
                if (stopping) {
                    if (static_cast<std::size_t>(got) >= budget) break;
                    budget -= static_cast<std::size_t>(got);
                }
                continue;
            }
            if (got == -1 && (errno == EINTR || errno == EAGAIN)) continue;
            if (got == -1) {
                st->error = errno_string();
                break;
            }
            break;  // EOF: every writer closed the pipe
        }
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        st->error = "cannot allocate memory for captured stderr";
    } catch (...) {
        st->error = "unknown capture failure";
    }
#endif
    st->read_fd.reset();
}

/// Kill and reap the child if spawn() fails after fork().
struct child_reaper {
    pid_t pid;
    explicit child_reaper(pid_t p) noexcept : pid(p) {}
    child_reaper(const child_reaper&) = delete;
    child_reaper& operator=(const child_reaper&) = delete;
    ~child_reaper() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int status;
            waitpid_retry(pid, &status, 0);
        }
    }
    void release() noexcept { pid = -1; }
};

}  // namespace detail

class Process;

/// Start a child and return after the exec handshake, without waiting for completion.
/// A failed launch yields a Process with error() set and no child.
[[nodiscard]] inline Process spawn(const std::vector<std::string>& args,
                                    const RunOptions& opts = {},
                                    std::string_view stage = {});

/// Move-only child owner. Methods are not concurrently callable.
/// Destruction waits for the child and joins the optional stderr reader.
class Process {
public:
    Process() = default;

    Process(Process&& o) noexcept
        : pid_(o.pid_),
          status_(o.status_),
          reaped_(o.reaped_),
          cap_(std::move(o.cap_)),
          reader_(std::move(o.reader_)),
          result_(std::move(o.result_)),
          finished_(o.finished_) {
        o.pid_ = -1;
        o.reaped_ = false;
        o.finished_ = false;
    }

    Process& operator=(Process&& o) noexcept {
        if (this != &o) {
            teardown();
            pid_ = o.pid_;
            status_ = o.status_;
            reaped_ = o.reaped_;
            cap_ = std::move(o.cap_);
            reader_ = std::move(o.reader_);
            result_ = std::move(o.result_);
            finished_ = o.finished_;
            o.pid_ = -1;
            o.reaped_ = false;
            o.finished_ = false;
        }
        return *this;
    }

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    ~Process() { teardown(); }

    /// Poll child exit without joining the capture reader.
    /// False for "not running"; no-child and waitpid failures fold into wait()'s error.
    [[nodiscard]] bool running() {
        if (finished_ || reaped_) return false;
        if (pid_ <= 0) return false;
        int status = 0;
        pid_t r = detail::waitpid_retry(pid_, &status, WNOHANG);
        if (r == -1) {
            fail(detail::errno_string());
            return false;
        }
        if (r == 0) return true;
        status_ = status;
        reaped_ = true;
        pid_ = -1;
        return false;
    }

    /// Wait without killing on expiry; nonpositive durations only poll.
    [[nodiscard]] wait_status wait_for(std::chrono::milliseconds budget) {
        if (finished_ || reaped_ || pid_ <= 0) {
            if (!finished_ && pid_ <= 0 && !reaped_) fail("no child process");
            (void) wait();
            return wait_status::ready;
        }
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            if (!running()) {
                (void) wait();  // reaped already: finalizes the capture
                return wait_status::ready;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (budget <= elapsed) return wait_status::timeout;
            const auto left = budget - elapsed;
            detail::nap(left < detail::wait_poll_interval ? left : detail::wait_poll_interval);
        }
    }

    /// Wait and collect captured stderr; later calls return the cached result.
    [[nodiscard]] RunResult& wait() {
        if (!finished_) {
            if (!reaped_) {
                if (pid_ <= 0) fail("no child process");
                else {
                    int status = 0;
                    if (detail::waitpid_retry(pid_, &status, 0) == -1) fail(detail::errno_string());
                    else {
                        status_ = status;
                        reaped_ = true;
                        pid_ = -1;
                    }
                }
            }
            if (!finished_) finalize();
        }
        if (cap_) {
            std::string discarded;
            collect_capture(discarded);
        }
        return result_;
    }

    /// The OS-level failure text; empty when the launch succeeded.
    [[nodiscard]] const std::string& error() const noexcept { return result_.error; }

    /// SIGKILL and reap the direct child; descendants are not signalled.
    /// The kill failure, if any, lands in wait()'s result error.
    void terminate() {
        if (pid_ > 0 && ::kill(pid_, SIGKILL) == -1 && errno != ESRCH) fail(detail::errno_string());
        (void) wait();
    }


private:
    Process(pid_t pid, std::unique_ptr<detail::capture_state> cap, std::thread reader) noexcept
        : pid_(pid), cap_(std::move(cap)), reader_(std::move(reader)) {}

    friend Process spawn(const std::vector<std::string>&, const RunOptions&, std::string_view);

    // Joining precedes access to the reader's state.
    void stop_and_join() noexcept {
        if (!reader_.joinable()) return;
        detail::request_stop(*cap_);
        reader_.join();
    }

    /// Hand the worker's buffer over after joining; report its failure, if any.
    std::string collect_capture(std::string& out) noexcept {
        stop_and_join();
        std::string err;
        if (cap_) {
            err = std::move(cap_->error);
            out = std::move(cap_->data);
            cap_.reset();
        }
        return err;
    }

    /// Turn the reaped status plus the captured stderr into the cached result.
    void finalize() {
        RunResult res;
        if (WIFEXITED(status_)) {
            res.exit_code = WEXITSTATUS(status_);
        } else if (WIFSIGNALED(status_)) {
            res.signal = WTERMSIG(status_);
        }
        std::string err = collect_capture(res.stderr_output);
        finished_ = true;
        if (!err.empty()) res.error = std::move(err);
        result_ = std::move(res);
    }

    // Cache a failure; wait() or destruction will join the reader.
    void fail(std::string error) {
        pid_ = -1;
        finished_ = true;
        result_.error = std::move(error);
    }

    // Destruction and move assignment wait without reporting errors.
    void teardown() noexcept {
        if (pid_ > 0) {
            int status = 0;
            detail::waitpid_retry(pid_, &status, 0);
            pid_ = -1;
        }
        stop_and_join();
        cap_.reset();
    }

    pid_t pid_ = -1;                                ///< > 0 while the child is unreaped
    int status_ = 0;                                ///< waitpid() status, valid once reaped_
    bool reaped_ = false;                           ///< child collected, capture not yet finalized
    std::unique_ptr<detail::capture_state> cap_;
    std::thread reader_;                            ///< joinable iff cap_ is set
    RunResult result_;                              ///< valid once finished_
    bool finished_ = false;
};

inline Process spawn(const std::vector<std::string>& args, const RunOptions& opts, std::string_view stage) {
    auto aborted = [&](std::string error) {
        Process dead;  // never held a child; fail() marks the launch error
        dead.fail(std::move(error));
        return dead;
    };
    if (args.empty()) return aborted("empty command");
    if (opts.on_spawn) opts.on_spawn(stage, format_command(args));
    else if (const auto& hook = default_spawn_hook()) hook(stage, format_command(args));

    // Exec-status pipe: CLOEXEC, so a successful exec closes it (EOF); a failure writes errno.
    detail::unique_fd exec_r, exec_w;
    if (!detail::make_cloexec_pipe(exec_r, exec_w)) return aborted(detail::errno_string("cannot create pipe"));

    // Allocate capture storage and close-on-exec pipes before fork.
    std::unique_ptr<detail::capture_state> cap;
    detail::unique_fd stderr_w;
    if (opts.capture_stderr) {
        cap = std::make_unique<detail::capture_state>();
        if (!detail::make_cloexec_pipe(cap->read_fd, stderr_w)) return aborted(detail::errno_string("cannot create pipe"));
        if (!detail::make_cloexec_pipe(cap->wake_r, cap->wake_w)) return aborted(detail::errno_string("cannot create pipe"));
    }

    detail::unique_fd stdout_fd;
    if (opts.stdout_file) {
        stdout_fd = detail::unique_fd(::open(opts.stdout_file->c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
        if (!stdout_fd) return aborted(detail::errno_string("cannot open stdout file"));
    } else if (!opts.inherit_stdout) {
        stdout_fd = detail::unique_fd(::open("/dev/null", O_WRONLY | O_CLOEXEC));
        if (!stdout_fd) return aborted(detail::errno_string("cannot open /dev/null"));
    }

    if (!detail::reserve_above_stdio(exec_r) || !detail::reserve_above_stdio(exec_w) ||
        !detail::reserve_above_stdio(stderr_w) || !detail::reserve_above_stdio(stdout_fd))
        return aborted(detail::errno_string("cannot reserve descriptors"));
    if (cap && (!detail::reserve_above_stdio(cap->read_fd) || !detail::reserve_above_stdio(cap->wake_r) ||
                !detail::reserve_above_stdio(cap->wake_w)))
        return aborted(detail::errno_string("cannot reserve descriptors"));

    // Prepare argv before fork to avoid allocating in the child.
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const char* workdir = opts.workdir ? opts.workdir->c_str() : nullptr;
    const int exec_wfd = exec_w.fd;
    const int stderr_wfd = stderr_w.fd;
    const int stdout_wfd = stdout_fd.fd;

    pid_t pid = ::fork();
    if (pid == -1) return aborted(detail::errno_string("cannot fork"));


    if (pid == 0) {
        // Async-signal-safe only: no allocation, no locks, straight to exec.
        auto fail = [exec_wfd](int e) {
            ssize_t written;
            do {
                written = ::write(exec_wfd, &e, sizeof e);
            } while (written == -1 && errno == EINTR);
            ::_exit(127);
        };
        if (workdir && ::chdir(workdir) == -1) fail(errno);
        // Internal descriptors are above 2, separate from the dup2() destinations.
        if (stderr_wfd != -1) {
            if (::dup2(stderr_wfd, STDERR_FILENO) == -1) fail(errno);
            ::close(stderr_wfd);
        }
        if (stdout_wfd != -1) {
            if (::dup2(stdout_wfd, STDOUT_FILENO) == -1) fail(errno);
            ::close(stdout_wfd);
        }
        ::execvp(argv[0], argv.data());
        fail(errno);
    }

    // Parent.
    detail::child_reaper reaper(pid);
    exec_w.reset();
    stderr_w.reset();
    stdout_fd.reset();

    int child_errno = 0;
    {
        char buf[sizeof(int)];
        std::size_t have = 0;
        for (;;) {
            ssize_t n = ::read(exec_r.fd, buf + have, sizeof buf - have);
            if (n == -1) {
                if (errno == EINTR) continue;
                return aborted(detail::errno_string("cannot read exec status"));
            }
            if (n == 0) break;  // EOF: exec succeeded
            have += static_cast<std::size_t>(n);
            if (have == sizeof buf) break;
        }
        exec_r.reset();
        if (have != 0 && have != sizeof buf) return aborted("child wrote a partial exec status");
        if (have == sizeof buf) std::memcpy(&child_errno, buf, sizeof child_errno);
    }
    if (child_errno != 0) return aborted(std::string(std::strerror(child_errno)));

    std::thread reader;
    if (cap) {
#if defined(__cpp_exceptions)
        try {
            reader = std::thread(detail::capture_worker, cap.get());
        } catch (const std::exception& e) {
            cap->read_fd.reset();
            return aborted(e.what());
        }
#else
        // Without exceptions std::thread construction failure calls std::terminate.
        reader = std::thread(detail::capture_worker, cap.get());
#endif
    }


    reaper.release();  // the Process owns the child from here on
    return Process(pid, std::move(cap), std::move(reader));
}

/// Synchronous convenience: spawn() followed by wait().
[[nodiscard]] inline RunResult run(const std::vector<std::string>& args,
                                   const RunOptions& opts = {},
                                   std::string_view stage = {}) {
    auto proc = spawn(args, opts, stage);
    return std::move(proc.wait());
}

// Outcome: ordered checks and commands

/// Names a file inside the owning stage's private temp directory. The same
/// name always resolves to the same path within one stage; the directory is
/// created on first use and removed with the stage unless keep_temps is set.
struct temp_file {
    std::string name;  ///< single path component, e.g. "aln.bam"; sidecars like "aln.bam.bai" live beside it
};

struct slot;
struct optional;

/// One argv element for proc(): plain text or a stage temp token.
class Arg {
public:
    Arg(const char* text) : value_(std::string(text)) {}
    Arg(std::string text) : value_(std::move(text)) {}
    Arg(const fs::path& path) : value_(path.string()) {}
    Arg(temp_file token) : value_(std::move(token)) {}
    // Placeholders belong to StageTemplate, which records instead of executing.
    Arg(slot) = delete;      ///< use shrn::StageTemplate for stages with slots
    Arg(optional) = delete;  ///< use shrn::StageTemplate for stages with optional groups

private:
    friend class Outcome;
    friend class StageTemplate;
    std::variant<std::string, temp_file> value_;
};

struct StageOptions {
    bool keep_temps = false;  ///< leave the stage temp directory in place on destruction
};

/// Ordered asynchronous commands and checks; skip later operations after failure.
class Outcome {
public:
    Outcome() = default;
    explicit Outcome(std::string name, StageOptions options = {}) noexcept
        : name_(std::move(name)), options_(options) {}
    Outcome(const Outcome&) = delete;
    Outcome& operator=(const Outcome&) = delete;
    Outcome(Outcome&&) noexcept = default;
    Outcome& operator=(Outcome&&) noexcept = default;

    ~Outcome() {
        if (options_.keep_temps && temp_dir_) (void) temp_dir_->release();
    }

private:
    /// What a call() argument becomes at the callee: temp tokens turn into paths.
    template <class T>
    using resolved_t = std::conditional_t<std::is_same_v<std::remove_cvref_t<T>, temp_file>, const fs::path&, T>;

public:

    /// Wait for the previous command, then start args without waiting for completion.
    Outcome& proc(const std::vector<std::string>& args, const RunOptions& opts = {}) & {
        if (!ok()) return *this;
        cmd_ = args.empty() ? std::string() : args.front();
        auto child = spawn(args, opts, name_);
        if (!child.error().empty()) fail("cannot execute: " + child.error());
        else pending_.emplace(std::move(child));
        return *this;
    }
    Outcome&& proc(const std::vector<std::string>& args, const RunOptions& opts = {}) && {
        return std::move(proc(args, opts));
    }

    /// Braced argument lists may mix text with temp_file tokens.
    Outcome& proc(std::initializer_list<Arg> args, const RunOptions& opts = {}) & {
        return proc_args(args.begin(), args.end(), opts);
    }
    Outcome&& proc(std::initializer_list<Arg> args, const RunOptions& opts = {}) && {
        return std::move(proc(args, opts));
    }

    /// Wait for the previous command, then invoke on the caller's thread.
    /// temp_file arguments are resolved to `const fs::path&` before the call.
    template <class F, class... Args>
        requires std::is_invocable_r_v<int, F, resolved_t<Args>...>
    Outcome& call(F&& fn, Args&&... args) & {
        if (!ok()) return *this;
        cmd_.clear();
        bool resolvable = (resolve_arg(args) && ...);
        if (!resolvable) return *this;  // resolve_arg() recorded the failure
        int status = std::invoke(std::forward<F>(fn), forward_arg<Args>(args)...);
        if (status != 0) fail("function returned code " + std::to_string(status));
        return *this;
    }
    template <class F, class... Args>
        requires std::is_invocable_r_v<int, F, resolved_t<Args>...>
    Outcome&& call(F&& fn, Args&&... args) && {
        return std::move(call(std::forward<F>(fn), std::forward<Args>(args)...));
    }

    /// The resolved path for a temp name; creates the stage temp directory on
    /// first use. Empty (and the stage failed) if the directory cannot be made.
    [[nodiscard]] const fs::path& temp_path(std::string_view name) {
        static const fs::path none;
        const fs::path* p = resolve(temp_file{std::string(name)});
        return p ? *p : none;
    }

    /// Join all prerequisites, preserving the first failure in argument order.
    template <class... Stages>
        requires (std::is_same_v<Stages, Outcome> && ...)
    Outcome& after(const Stages&... prerequisites) & {
        finish();
        (prerequisites.finish(), ...);
        auto check = [this](const Outcome& prerequisite) {
            if (code_ != 0 || prerequisite.code_ == 0) return;
            const auto& label = prerequisite.name_.empty() ? prerequisite.cmd_ : prerequisite.name_;
            fail("prerequisite " + (label.empty() ? std::string("(anonymous)") : label) +
                 " failed: " + prerequisite.detail_);
        };
        (check(prerequisites), ...);
        return *this;
    }
    template <class... Stages>
        requires (std::is_same_v<Stages, Outcome> && ...)
    Outcome&& after(const Stages&... prerequisites) && {
        return std::move(after(prerequisites...));
    }

    /// Wait without a deadline, including after a failed expectation.
    Outcome& wait() & {
        finish();
        return *this;
    }
    Outcome&& wait() && { return std::move(wait()); }

    /// Expiry leaves the child running and does not record failure.
    [[nodiscard]] wait_status wait_for(std::chrono::milliseconds duration) {
        if (!pending_) return wait_status::ready;
        auto status = pending_->wait_for(duration);
        if (status == wait_status::ready) finish();
        return status;
    }

    /// Check child status without waiting or collecting captured output.
    [[nodiscard]] bool running() const {
        return pending_ && pending_->running();
    }

    /// Record failure on expiry; use or_terminate() to stop the child.
    Outcome& expect_done_within(std::chrono::milliseconds duration) & {
        if (code_ == 0 && wait_for(duration) == wait_status::timeout)
            fail("did not finish within " + std::to_string(duration.count()) + " ms");
        return *this;
    }
    Outcome&& expect_done_within(std::chrono::milliseconds duration) && {
        return std::move(expect_done_within(duration));
    }

    /// Kill and reap this stage's child on recorded failure, without waiting first.
    Outcome& or_terminate() & {
        if (code_ != 0) terminate_pending();
        return *this;
    }
    Outcome&& or_terminate() && { return std::move(or_terminate()); }

    Outcome& expect_success(int exit_code) & {
        if (ok() && exit_code != 0) fail("exited with code " + std::to_string(exit_code));
        return *this;
    }
    Outcome&& expect_success(int exit_code) && { return std::move(expect_success(exit_code)); }

    Outcome& expect_which(std::string_view name) & {
        if (ok() && !which(name)) fail("executable not found: " + std::string(name));
        return *this;
    }
    Outcome&& expect_which(std::string_view name) && { return std::move(expect_which(name)); }

    /// Replace the detail text without changing the success/failure state.
    Outcome& with_error(std::string detail) & {
        if (code_ == 0) finish();
        detail_ = std::move(detail);
        return *this;
    }
    Outcome&& with_error(std::string detail) && { return std::move(with_error(std::move(detail))); }

    Outcome& with_stderr(std::string captured) & {
        if (code_ == 0) finish();
        stderr_ = std::move(captured);
        return *this;
    }
    Outcome&& with_stderr(std::string captured) && { return std::move(with_stderr(std::move(captured))); }

    /// The bool argument is evaluated before the call; use a callable to defer it.
    Outcome& expect(bool cond, std::string_view detail) & {
        if (ok() && !cond) fail(std::string(detail));
        return *this;
    }
    Outcome&& expect(bool cond, std::string_view detail) && {
        return std::move(expect(cond, detail));
    }

    template <class Pred>
        requires std::is_invocable_r_v<bool, Pred>
    Outcome& expect(Pred&& pred, std::string_view detail) & {
        if (ok() && !std::invoke(std::forward<Pred>(pred))) fail(std::string(detail));
        return *this;
    }
    template <class Pred>
        requires std::is_invocable_r_v<bool, Pred>
    Outcome&& expect(Pred&& pred, std::string_view detail) && {
        return std::move(expect(std::forward<Pred>(pred), detail));
    }

    Outcome& expect_file(const fs::path& p) & {
        if (!ok()) return *this;
        if (!file_readable(p)) fail("missing: " + p.string());
        return *this;
    }
    Outcome&& expect_file(const fs::path& p) && {
        return std::move(expect_file(p));
    }
    Outcome& expect_file(temp_file token) & {
        if (const fs::path* p = ok() ? resolve(token) : nullptr) expect_file(*p);
        return *this;
    }
    Outcome&& expect_file(temp_file token) && { return std::move(expect_file(std::move(token))); }

    template <class Pred>
        requires std::is_invocable_r_v<bool, Pred, const fs::path&>
    Outcome& expect_file(const fs::path& p, Pred&& pred, std::string_view what) & {
        if (!ok()) return *this;
        if (!file_readable(p)) fail("missing: " + p.string());
        else if (!std::invoke(std::forward<Pred>(pred), p)) fail("not " + std::string(what) + ": " + p.string());
        return *this;
    }
    template <class Pred>
        requires std::is_invocable_r_v<bool, Pred, const fs::path&>
    Outcome&& expect_file(const fs::path& p, Pred&& pred, std::string_view what) && {
        return std::move(expect_file(p, std::forward<Pred>(pred), what));
    }
    template <class Pred>
        requires std::is_invocable_r_v<bool, Pred, const fs::path&>
    Outcome& expect_file(temp_file token, Pred&& pred, std::string_view what) & {
        if (const fs::path* p = ok() ? resolve(token) : nullptr) expect_file(*p, std::forward<Pred>(pred), what);
        return *this;
    }
    template <class Pred>
        requires std::is_invocable_r_v<bool, Pred, const fs::path&>
    Outcome&& expect_file(temp_file token, Pred&& pred, std::string_view what) && {
        return std::move(expect_file(std::move(token), std::forward<Pred>(pred), what));
    }

    template <class F>
        requires std::is_invocable_v<F>
    Outcome& or_execute(F&& fn) & {
        if (!ok()) std::invoke(std::forward<F>(fn));
        return *this;
    }
    template <class F>
        requires std::is_invocable_v<F>
    Outcome&& or_execute(F&& fn) && { return std::move(or_execute(std::forward<F>(fn))); }

    /// On fatal failure, reap this stage's child before exiting the caller.
    Outcome& or_die_if(bool condition, std::string_view stage = {}) & {
        if (ok()) return *this;
        if (condition) terminate_pending();
        if (stage.empty()) stage = !name_.empty() ? name_ : !cmd_.empty() ? cmd_ : std::string_view("command");
        if (condition) {
            std::fprintf(stderr, "\n[ERROR] %.*s failed", static_cast<int>(stage.size()), stage.data());
            if (!detail_.empty()) std::fprintf(stderr, " (%s)", detail_.c_str());
            std::fprintf(stderr, "\n");
            if (!stderr_.empty()) std::fprintf(stderr, "[ERROR] stderr:\n%s\n", stderr_.c_str());
            if (temp_dir_) std::fprintf(stderr, "[ERROR] temp files kept in %s\n", temp_dir_->path().c_str());
            std::fflush(stderr);
            std::exit(EXIT_FAILURE);
        }
        std::fprintf(stderr, "\n[WARNING] %.*s failed", static_cast<int>(stage.size()), stage.data());
        if (!detail_.empty()) std::fprintf(stderr, " (%s)", detail_.c_str());
        std::fprintf(stderr, " — soft-fail active\n");
        return *this;
    }
    Outcome&& or_die_if(bool condition, std::string_view stage = {}) && {
        return std::move(or_die_if(condition, stage));
    }

    /// Final-result access waits unless a failure has already been recorded.
    [[nodiscard]] bool ok() const {
        if (code_ == 0) finish();
        return code_ == 0;
    }
    explicit operator bool() const { return ok(); }
    [[nodiscard]] int code() const {
        if (code_ == 0) finish();
        return code_;
    }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& detail() const {
        if (code_ == 0) finish();
        return detail_;
    }
    [[nodiscard]] const std::string& stderr_output() const {
        if (code_ == 0) finish();
        return stderr_;
    }

private:
    friend class StageTemplate;  ///< launch() starts a stage failed on binding errors

    void fail(std::string detail) const {
        if (code_ != 0) return;
        code_ = EXIT_FAILURE;
        detail_ = cmd_.empty() ? std::move(detail) : cmd_ + ": " + detail;
    }

    void finish() const {
        if (!pending_) return;
        auto& completed = pending_->wait();
        stderr_ = std::move(completed.stderr_output);
        if (!completed.success()) fail(completed.summary());
        pending_.reset();
    }

    void terminate_pending() {
        if (!pending_) return;
        pending_->terminate();
        finish();
    }

    // --- stage temp files -------------------------------------------------

    /// Shared by the braced proc() and template replay: resolve tokens, then spawn.
    Outcome& proc_args(const Arg* first, const Arg* last, const RunOptions& opts) {
        if (!ok()) return *this;
        std::vector<std::string> resolved;
        resolved.reserve(static_cast<std::size_t>(last - first));
        for (const Arg* a = first; a != last; ++a) {
            if (const auto* token = std::get_if<temp_file>(&a->value_)) {
                const fs::path* p = resolve(*token);
                if (!p) return *this;  // resolve() recorded the failure
                resolved.push_back(p->string());
            } else {
                resolved.push_back(std::get<std::string>(a->value_));
            }
        }
        return proc(resolved, opts);
    }

    /// Resolve a token to its path inside the stage temp directory, creating the
    /// directory on first use. Records a stage failure and returns null on error.
    const fs::path* resolve(const temp_file& token) {
        const std::string& n = token.name;
        if (n.empty() || n == "." || n == ".." || n.find('/') != std::string::npos) {
            fail("invalid temp file name: '" + n + "'");
            return nullptr;
        }
        if (!temp_dir_) {
            std::string prefix = "shrn_";
            for (char c : name_.empty() ? std::string("stage") : name_)
                prefix += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
            prefix += '_' + std::to_string(::getpid()) + '_' + uuid4() + '_';
            temp_dir_ = detail::TempDir::create(prefix);
            if (!*temp_dir_) {
                temp_dir_.reset();
                fail("cannot create temp dir: " + detail::errno_string());
                return nullptr;
            }
        }
        auto [it, inserted] = temp_paths_.try_emplace(n, temp_dir_->path() / n);
        return &it->second;
    }

    /// call() support: non-token arguments always resolve; tokens must resolve.
    template <class T>
    bool resolve_arg(T& arg) {
        if constexpr (std::is_same_v<std::remove_cvref_t<T>, temp_file>) return resolve(arg) != nullptr;
        else return true;
    }
    template <class T, class U>
    decltype(auto) forward_arg(U& arg) {
        if constexpr (std::is_same_v<std::remove_cvref_t<T>, temp_file>) return static_cast<const fs::path&>(*resolve(arg));
        else return std::forward<T>(arg);
    }

    mutable int code_ = 0;
    std::string name_;
    StageOptions options_;
    std::string cmd_;
    mutable std::string detail_;
    mutable std::string stderr_;
    mutable std::optional<Process> pending_;
    std::optional<detail::TempDir> temp_dir_;            ///< created on first temp_file use
    std::unordered_map<std::string, fs::path> temp_paths_;  ///< name -> resolved path
};

/// Start a named stage.
[[nodiscard]] inline Outcome stage(std::string name, StageOptions options = {}) {
    return Outcome(std::move(name), options);
}

/// Anonymous single-command stage: proc(args, opts) == Outcome().proc(args, opts).
[[nodiscard]] inline Outcome proc(const std::vector<std::string>& args, const RunOptions& opts = {}) {
    Outcome outcome;
    outcome.proc(args, opts);
    return outcome;
}

// StageTemplate: a recorded stage with path placeholders

/// A placeholder in a template, bound to a path at launch. A slot with
/// a default is satisfied by the default when left unbound; one without is
/// required unless it appears only inside optional groups.
struct slot {
    std::string name;
    std::optional<std::string> default_value;
    explicit slot(std::string n) : name(std::move(n)) {}
    slot(std::string n, std::string d) : name(std::move(n)), default_value(std::move(d)) {}
};

/// A list placeholder, bound to a vector of paths at launch. It expands in
/// place in argv and, in expect_file, checks every element. Required unless
/// it appears only inside optional groups; a bound empty list is valid.
struct many {
    std::string name;
    explicit many(std::string n) : name(std::move(n)) {}
};

class TArg;

/// An argv fragment included whole when every slot inside it is bound (or
/// defaulted), and dropped whole otherwise: `optional{"-2", slot{"r2"}}`.
struct optional {
    std::vector<TArg> args;
    optional(std::initializer_list<TArg> a);
};

/// One template argv element: text, a temp token, a slot, a list, or an optional group.
class TArg {
public:
    TArg(const char* text) : value_(std::string(text)) {}
    TArg(std::string text) : value_(std::move(text)) {}
    TArg(const fs::path& path) : value_(path.string()) {}
    TArg(temp_file token) : value_(std::move(token)) {}
    TArg(slot s) : value_(std::move(s)) {}
    TArg(many m) : value_(std::move(m)) {}
    TArg(optional group) : value_(std::make_shared<optional>(std::move(group))) {}

private:
    friend class StageTemplate;
    std::variant<std::string, temp_file, slot, many, std::shared_ptr<optional>> value_;
};

inline optional::optional(std::initializer_list<TArg> a) : args(a) {}

/// A binding target: a concrete path, a temp token so the caller decides at
/// launch whether an output is scratch or a deliverable, or a list for `many`.
struct binding {
    using list = std::vector<std::string>;
    std::string name;
    std::variant<std::string, temp_file, list> value;
    binding(std::string n, const fs::path& p) : name(std::move(n)), value(p.string()) {}
    binding(std::string n, const char* p) : name(std::move(n)), value(std::string(p)) {}
    binding(std::string n, std::string p) : name(std::move(n)), value(std::move(p)) {}
    binding(std::string n, temp_file t) : name(std::move(n)), value(std::move(t)) {}
    binding(std::string n, const std::vector<fs::path>& paths) : name(std::move(n)), value(list{}) {
        auto& l = std::get<list>(value);
        l.reserve(paths.size());
        for (const auto& p : paths) l.push_back(p.string());
    }
    binding(std::string n, list items) : name(std::move(n)), value(std::move(items)) {}
};

/// Records checks and commands once; launch() replays them into a fresh
/// Outcome with slots substituted. Each launch owns its own temp files.
class StageTemplate {
public:
    explicit StageTemplate(std::string name, StageOptions options = {})
        : name_(std::move(name)), options_(options) {}

    StageTemplate& expect_which(std::string tool) & {
        Step& s = add(Step::which);
        s.text = std::move(tool);
        return *this;
    }
    StageTemplate&& expect_which(std::string tool) && { return std::move(expect_which(std::move(tool))); }

    StageTemplate& expect_file(TArg path) & {
        add(Step::file).args.push_back(std::move(path));
        return *this;
    }
    StageTemplate&& expect_file(TArg path) && { return std::move(expect_file(std::move(path))); }

    template <class Pred>
        requires std::is_invocable_r_v<bool, Pred, const fs::path&>
    StageTemplate& expect_file(TArg path, Pred&& pred, std::string what) & {
        Step& s = add(Step::file_pred);
        s.args.push_back(std::move(path));
        s.text = std::move(what);
        s.pred = std::forward<Pred>(pred);
        return *this;
    }
    template <class Pred>
        requires std::is_invocable_r_v<bool, Pred, const fs::path&>
    StageTemplate&& expect_file(TArg path, Pred&& pred, std::string what) && {
        return std::move(expect_file(std::move(path), std::forward<Pred>(pred), std::move(what)));
    }

    /// stdout_file in `opts` is fixed text; use `proc_to` to redirect stdout into a slot.
    StageTemplate& proc(std::initializer_list<TArg> args, RunOptions opts = {}) & {
        Step& s = add(Step::command);
        s.args.assign(args.begin(), args.end());
        s.opts = std::move(opts);
        return *this;
    }
    StageTemplate&& proc(std::initializer_list<TArg> args, RunOptions opts = {}) && {
        return std::move(proc(args, std::move(opts)));
    }

    /// Like proc(), redirecting the command's stdout to a slot or temp token.
    StageTemplate& proc_to(TArg stdout_target, std::initializer_list<TArg> args, RunOptions opts = {}) & {
        proc(args, std::move(opts));
        steps_.back().stdout_target = std::move(stdout_target);
        return *this;
    }
    StageTemplate&& proc_to(TArg stdout_target, std::initializer_list<TArg> args, RunOptions opts = {}) && {
        return std::move(proc_to(std::move(stdout_target), args, std::move(opts)));
    }

    /// Replay the recording with slots bound. Binding errors (an unbound
    /// required slot, a name no slot uses, or a list bound to a scalar slot and
    /// vice versa) start the stage failed; nothing runs.
    [[nodiscard]] Outcome launch(std::initializer_list<binding> bindings) const {
        Outcome out(name_, options_);
        Bound bound;
        for (const auto& b : bindings) bound.emplace(b.name, b.value);

        // Validate before anything runs.
        std::unordered_map<std::string, Placeholder> placeholders;
        for (const auto& s : steps_) collect(s, placeholders);
        for (const auto& [n, v] : bound) {
            auto it = placeholders.find(n);
            if (it == placeholders.end()) {
                out.fail("unknown binding: '" + n + "'");
                return out;
            }
            bool is_list = std::holds_alternative<binding::list>(v);
            if (is_list != it->second.list) {
                out.fail(std::string(is_list ? "list bound to slot: '" : "scalar bound to list slot: '") + n + "'");
                return out;
            }
        }
        for (const auto& [n, p] : placeholders) {
            if (p.required && !bound.count(n) && !defaults_.count(n)) {
                out.fail("unbound slot: '" + n + "'");
                return out;
            }
        }

        for (const auto& s : steps_) {
            switch (s.kind) {
                case Step::which:
                    out.expect_which(s.text);
                    break;
                case Step::file:
                case Step::file_pred: {
                    std::vector<Arg> targets;
                    append(s.args.front(), bound, targets);
                    for (const Arg& t : targets) {
                        const fs::path* p = nullptr;
                        fs::path text;
                        if (const auto* tok = std::get_if<temp_file>(&t.value_)) p = out.resolve(*tok);
                        else p = &(text = std::get<std::string>(t.value_));
                        if (!p) break;  // resolve() recorded the failure
                        if (s.kind == Step::file) out.expect_file(*p);
                        else out.expect_file(*p, s.pred, s.text);
                    }
                    break;
                }
                case Step::command: {
                    std::vector<Arg> argv;
                    for (const auto& a : s.args) append(a, bound, argv);
                    RunOptions opts = s.opts;
                    if (s.stdout_target) {
                        auto target = lower(*s.stdout_target, bound);
                        if (target) {
                            if (auto* t = std::get_if<temp_file>(&*target)) opts.stdout_file = out.temp_path(t->name);
                            else opts.stdout_file = std::get<std::string>(*target);
                        }
                    }
                    out.proc_args(argv.data(), argv.data() + argv.size(), opts);
                    break;
                }
            }
        }
        return out;
    }

private:
    struct Step {
        enum Kind { which, file, file_pred, command } kind;
        std::vector<TArg> args;
        std::string text;  ///< tool name, or predicate label
        RunOptions opts;
        std::function<bool(const fs::path&)> pred;
        std::optional<TArg> stdout_target;
    };

    Step& add(Step::Kind kind) {
        steps_.push_back(Step{});
        steps_.back().kind = kind;
        return steps_.back();
    }

    using Bound = std::unordered_map<std::string, std::variant<std::string, temp_file, binding::list>>;

    struct Placeholder {
        bool required = false;  ///< occurs outside every optional group
        bool list = false;      ///< declared with many{} rather than slot{}
    };

    /// Record every placeholder; a name is required if it occurs outside all optional groups.
    void collect(const Step& s, std::unordered_map<std::string, Placeholder>& out) const {
        for (const auto& a : s.args) collect(a, out, true);
        if (s.stdout_target) collect(*s.stdout_target, out, true);
    }
    void collect(const TArg& a, std::unordered_map<std::string, Placeholder>& out, bool top) const {
        if (const auto* s = std::get_if<slot>(&a.value_)) {
            if (s->default_value) defaults_.emplace(s->name, *s->default_value);
            auto& p = out[s->name];
            p.required = p.required || top;
        } else if (const auto* m = std::get_if<many>(&a.value_)) {
            auto& p = out[m->name];
            p.required = p.required || top;
            p.list = true;
        } else if (const auto* g = std::get_if<std::shared_ptr<optional>>(&a.value_)) {
            for (const auto& inner : (*g)->args) collect(inner, out, false);
        }
    }

    /// A scalar template argument as the concrete value it stands for.
    std::optional<std::variant<std::string, temp_file>> lower(const TArg& a, const Bound& bound) const {
        if (const auto* text = std::get_if<std::string>(&a.value_)) return *text;
        if (const auto* t = std::get_if<temp_file>(&a.value_)) return *t;
        if (const auto* s = std::get_if<slot>(&a.value_)) {
            if (auto it = bound.find(s->name); it != bound.end()) {
                if (const auto* str = std::get_if<std::string>(&it->second)) return *str;
                if (const auto* tok = std::get_if<temp_file>(&it->second)) return *tok;
            }
            if (s->default_value) return *s->default_value;
            return std::nullopt;  // only reachable inside an optional group
        }
        return std::nullopt;
    }

    /// Expand one template argument into argv. Lists expand in place; an optional
    /// group is dropped whole if any placeholder inside it is unbound.
    bool append(const TArg& a, const Bound& bound, std::vector<Arg>& argv) const {
        if (const auto* m = std::get_if<many>(&a.value_)) {
            auto it = bound.find(m->name);
            if (it == bound.end()) return false;
            for (const auto& item : std::get<binding::list>(it->second)) argv.emplace_back(item);
            return true;
        }
        if (const auto* g = std::get_if<std::shared_ptr<optional>>(&a.value_)) {
            std::vector<Arg> group;
            for (const auto& inner : (*g)->args)
                if (!append(inner, bound, group)) return true;  // unbound: drop the group, not an error
            for (auto& x : group) argv.push_back(std::move(x));
            return true;
        }
        auto v = lower(a, bound);
        if (!v) return false;
        std::visit([&](auto&& x) { argv.emplace_back(std::move(x)); }, std::move(*v));
        return true;
    }

    std::string name_;
    StageOptions options_;
    std::vector<Step> steps_;
    mutable std::unordered_map<std::string, std::string> defaults_;  ///< filled by collect()
};

}  // namespace shrn

#endif  // SHRN_HPP
