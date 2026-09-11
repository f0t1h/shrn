/**
 * @file test_shrn.cpp
 * @brief Behavioral tests for shrn. Built twice: native std::expected and forced polyfill.
 */

#include <shrn.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
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

#if SHRN_HAS_ZLIB
static void write_gz(const fs::path& p, const std::string& content) {
    gzFile gz = gzopen(p.c_str(), "wb");
    gzwrite(gz, content.data(), static_cast<unsigned>(content.size()));
    gzclose(gz);
}
#endif

// ---------------------------------------------------------------------------
// expected
// ---------------------------------------------------------------------------
static void test_expected() {
    std::fprintf(stderr, "expected (%s)\n", SHRN_HAS_STD_EXPECTED ? "std" : "polyfill");

    shrn::result<int> ok = 3;
    shrn::result<int> bad = shrn::unexpected(std::make_error_code(std::errc::io_error));
    CHECK(ok && *ok == 3, "value accessible");
    CHECK(!bad && bad.error() == std::errc::io_error, "error accessible");
    CHECK(bad.value_or(7) == 7 && ok.value_or(7) == 3, "value_or");

    bool threw = false;
    try {
        (void) bad.value();
    } catch (const shrn::bad_expected_access<std::error_code>& e) {
        threw = e.error() == std::errc::io_error;
    }
    CHECK(threw, "value() on error throws bad_expected_access carrying the error");

    auto doubled = ok.transform([](int v) { return v * 2; });
    CHECK(doubled && *doubled == 6, "transform maps value");
    auto still_bad = bad.transform([](int v) { return v * 2; });
    CHECK(!still_bad && still_bad.error() == std::errc::io_error, "transform propagates error");

    auto chained = ok.and_then([](int v) -> shrn::result<std::string> { return std::to_string(v); });
    CHECK(chained && *chained == "3", "and_then chains");
    auto failed_chain = ok.and_then([](int) -> shrn::result<std::string> {
        return shrn::unexpected(std::make_error_code(std::errc::invalid_argument));
    });
    CHECK(!failed_chain && failed_chain.error() == std::errc::invalid_argument, "and_then can fail");

    auto recovered = bad.or_else([](std::error_code) -> shrn::result<int> { return 42; });
    CHECK(recovered && *recovered == 42, "or_else recovers");

    auto remapped = bad.transform_error([](std::error_code) { return 99; });
    CHECK(!remapped && remapped.error() == 99, "transform_error changes error type");

    shrn::result<void> vok;
    shrn::result<void> vbad = shrn::unexpected(std::make_error_code(std::errc::permission_denied));
    CHECK(vok.has_value() && !vbad.has_value(), "void expected states");
    auto after = vok.and_then([] { return shrn::result<int>(5); });
    CHECK(after && *after == 5, "void and_then");
    auto after_bad = vbad.and_then([] { return shrn::result<int>(5); });
    CHECK(!after_bad && after_bad.error() == std::errc::permission_denied, "void and_then propagates");

    CHECK(ok == 3, "compare with value");
    CHECK(bad == shrn::unexpected(std::make_error_code(std::errc::io_error)), "compare with unexpected");
    CHECK(ok != bad, "ok != bad");

    shrn::result<std::string> s = std::string("abc");
    std::string moved = std::move(s).value();
    CHECK(moved == "abc", "rvalue value()");
}

// ---------------------------------------------------------------------------
// files
// ---------------------------------------------------------------------------
static void test_files(const fs::path& dir) {
    std::fprintf(stderr, "files\n");

    auto missing = dir / "missing.txt";
    CHECK(!shrn::file_readable(missing), "missing not readable");
    auto r = shrn::read_file(missing);
    CHECK(!r && r.error() == std::errc::no_such_file_or_directory, "read_file missing -> ENOENT");

    auto text = dir / "text.txt";
    write_file(text, "a\nb\nc");
    CHECK(shrn::file_readable(text) && shrn::file_non_empty(text), "readable + non-empty");
    CHECK(shrn::read_file(text).value() == "a\nb\nc", "read_file content");
    CHECK(shrn::line_count(text).value() == 2, "line_count counts newlines only (no trailing)");
    CHECK(shrn::file_first_byte(text).value() == 'a', "first_byte plain");
    CHECK(!shrn::file_is_gzipped(text), "text not gzipped");

    auto empty = dir / "empty.txt";
    write_file(empty, "");
    CHECK(shrn::file_readable(empty) && !shrn::file_non_empty(empty), "empty readable but not non-empty");
    auto fb = shrn::file_first_byte(empty);
    CHECK(!fb && fb.error() == shrn::errc::empty_file, "first_byte empty -> errc::empty_file");
    CHECK(shrn::line_count(empty).value() == 0, "line_count empty");

    auto one = dir / "one.bin";
    write_file(one, "\x1f");
    CHECK(!shrn::file_is_gzipped(one), "1-byte file with half magic is not gzipped");

    auto longline = dir / "long.txt";
    write_file(longline, std::string(70000, 'x') + "\n" + std::string(70000, 'y') + "\n");
    CHECK(shrn::line_count(longline).value() == 2, "line_count with lines longer than any buffer");

    CHECK(shrn::line_count(missing).has_value() == false, "line_count missing is an error");

    auto sub = dir / "a" / "b" / "c";
    CHECK(shrn::ensure_directory(sub).has_value() && fs::is_directory(sub), "ensure_directory creates parents");
    CHECK(shrn::ensure_directory(sub).has_value(), "ensure_directory idempotent");
    auto as_file = shrn::ensure_directory(text);
    CHECK(!as_file, "ensure_directory over a file fails");

    CHECK(shrn::remove_if_exists(missing).has_value(), "remove_if_exists missing is ok");
    CHECK(shrn::remove_if_exists(one).has_value() && !fs::exists(one), "remove_if_exists removes");

    auto link = dir / "link";
    CHECK(shrn::force_symlink(text, link).has_value() && fs::is_symlink(link), "force_symlink creates");
    CHECK(shrn::force_symlink(longline, link).has_value() && fs::read_symlink(link) == longline,
          "force_symlink replaces existing link");

    auto cat = dir / "cat.txt";
    CHECK(shrn::concat_files({text, longline}, cat).has_value(), "concat raw ok");
    CHECK(shrn::read_file(cat).value() == "a\nb\nc" + shrn::read_file(longline).value(), "concat raw bytes");
    auto cat_bad = shrn::concat_files({text, missing}, cat);
    CHECK(!cat_bad && cat_bad.error() == std::errc::no_such_file_or_directory, "concat missing input fails");

#if SHRN_HAS_ZLIB
    auto gz = dir / "data.gz";
    write_gz(gz, "line1\nline2\n" + std::string(70000, 'z') + "\n");
    CHECK(shrn::file_is_gzipped(gz), "gz detected by magic");
    CHECK(shrn::line_count(gz).value() == 3, "line_count inflates gzip (long line)");
    CHECK(shrn::file_first_byte(gz).value() == 'l', "first_byte inflates gzip");

    auto gz_misnamed = dir / "misnamed.txt";
    fs::copy_file(gz, gz_misnamed);
    CHECK(shrn::line_count(gz_misnamed).value() == 3, "gzip detected regardless of extension");

    auto gz_empty = dir / "empty.gz";
    write_gz(gz_empty, "");
    auto fbz = shrn::file_first_byte(gz_empty);
    CHECK(!fbz && fbz.error() == shrn::errc::empty_file, "first_byte empty gz -> errc::empty_file");

    auto plain_out = dir / "inflated.txt";
    CHECK(shrn::concat_files({gz, text}, plain_out, shrn::concat_mode::decompress).has_value(), "concat decompress ok");
    CHECK(shrn::read_file(plain_out).value() == "line1\nline2\n" + std::string(70000, 'z') + "\na\nb\nc",
          "concat decompress inflates gz and passes plain through");

    auto multi = dir / "multi.gz";
    CHECK(shrn::concat_files({gz, gz}, multi).has_value(), "concat raw gz members");
    CHECK(shrn::line_count(multi).value() == 6, "raw-concatenated gz is a valid multi-member stream");
#else
    auto gz = dir / "fake.gz";
    write_file(gz, "\x1f\x8b\x08rest");
    auto lc = shrn::line_count(gz);
    CHECK(!lc && lc.error() == shrn::errc::zlib_unavailable, "gzip without zlib -> zlib_unavailable");
#endif
}

// ---------------------------------------------------------------------------
// temp
// ---------------------------------------------------------------------------
static void test_temp(const fs::path& dir) {
    std::fprintf(stderr, "temp\n");

    auto a = shrn::make_temp_file_in(dir, "t_", ".fq");
    auto b = shrn::make_temp_file_in(dir, "t_", ".fq");
    CHECK(a && b && *a != *b, "temp files unique");
    CHECK(a->extension() == ".fq" && a->filename().string().rfind("t_", 0) == 0, "prefix/suffix honored");
    CHECK(fs::exists(*a) && fs::file_size(*a) == 0, "temp file created empty");

    auto bad = shrn::make_temp_file_in(dir / "nope", "t_");
    CHECK(!bad && bad.error() == std::errc::no_such_file_or_directory, "temp in missing dir fails");

    fs::path kept;
    fs::path removed;
    {
        auto tf = shrn::TempFile::create_in(dir, ".tmp");
        CHECK(tf.has_value(), "TempFile created");
        removed = tf->path();
        auto tf2 = shrn::TempFile::create_in(dir);
        kept = tf2->release();
        CHECK(!*tf2, "released TempFile is empty");
    }
    CHECK(!fs::exists(removed), "TempFile removed on scope exit");
    CHECK(fs::exists(kept), "released file kept");

    fs::path tree;
    {
        auto td = shrn::TempDir::create_in(dir);
        CHECK(td.has_value() && fs::is_directory(td->path()), "TempDir created");
        tree = td->path();
        write_file(*td / "inner.txt", "x");
    }
    CHECK(!fs::exists(tree), "TempDir removed recursively");
}

// ---------------------------------------------------------------------------
// process
// ---------------------------------------------------------------------------
static void test_process(const fs::path& dir) {
    std::fprintf(stderr, "process\n");

    CHECK(shrn::format_command({"a", "b c", "it's", ""}) == "a 'b c' 'it'\\''s' ''", "format_command quoting");
    CHECK(shrn::which("sh").has_value(), "which finds sh");
    CHECK(!shrn::which("shrn-definitely-not-a-command").has_value(), "which misses");
    CHECK(shrn::which("/bin/sh").has_value(), "which accepts absolute path");

    auto ok = shrn::run({"true"});
    CHECK(ok && ok->success() && ok->exit_code == 0, "true succeeds");

    auto empty = shrn::run({});
    CHECK(!empty && empty.error() == shrn::errc::empty_command, "empty argv -> errc::empty_command");

    auto enoent = shrn::run({"shrn-definitely-not-a-command"});
    CHECK(!enoent && enoent.error() == std::errc::no_such_file_or_directory, "missing binary -> ENOENT, not exit 127");

    auto code = shrn::run({"sh", "-c", "echo oops >&2; exit 3"});
    CHECK(code && !code->success() && code->exit_code == 3, "exit code propagated");
    CHECK(code->stderr_output == "oops\n", "stderr captured");
    CHECK(code->summary() == "exited with code 3", "summary exit");

    shrn::RunOptions no_capture;
    no_capture.capture_stderr = false;
    no_capture.inherit_stdout = false;
    auto quiet = shrn::run({"sh", "-c", "echo not-captured >&2; echo swallowed"}, no_capture);
    CHECK(quiet && quiet->success() && quiet->stderr_output.empty(), "capture_stderr=false leaves stderr empty");

    auto sig = shrn::run({"sh", "-c", "kill -9 $$"});
    CHECK(sig && sig->signal == 9 && sig->exit_code == -1 && !sig->success(), "signal death reported");
    CHECK(sig->summary().rfind("killed by signal 9", 0) == 0, "summary signal");

    shrn::RunOptions to;
    to.timeout = 200ms;
    auto t0 = std::chrono::steady_clock::now();
    auto slow = shrn::run({"sleep", "5"}, to);
    auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(slow && slow->timed_out && slow->signal == SIGKILL && !slow->success(), "timeout kills child (capturing)");
    CHECK(elapsed < 3s, "timeout enforced promptly (capturing)");

    to.capture_stderr = false;
    t0 = std::chrono::steady_clock::now();
    auto slow2 = shrn::run({"sleep", "5"}, to);
    elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(slow2 && slow2->timed_out && !slow2->success(), "timeout kills child (non-capturing)");
    CHECK(elapsed < 3s, "timeout enforced promptly (non-capturing)");
    auto fast = shrn::run({"true"}, to);
    CHECK(fast && fast->success() && !fast->timed_out, "fast child under timeout is not flagged");

    shrn::RunOptions redirect;
    redirect.stdout_file = dir / "out.txt";
    redirect.workdir = dir;
    std::string spawned;
    redirect.on_spawn = [&](std::string_view st, std::string_view cmd) { spawned = std::string(st) + "|" + std::string(cmd); };
    auto red = shrn::run({"sh", "-c", "pwd"}, redirect);
    CHECK(red && red->success(), "redirect run ok");
    CHECK(shrn::read_file(dir / "out.txt").value() == fs::canonical(dir).string() + "\n", "stdout_file + workdir");
    CHECK(spawned == "|sh -c pwd", "on_spawn receives empty stage and formatted command from run()");

    shrn::RunOptions badwd;
    badwd.workdir = dir / "nowhere";
    auto wd = shrn::run({"true"}, badwd);
    CHECK(!wd && wd.error() == std::errc::no_such_file_or_directory, "bad workdir -> launch error");
}

// ---------------------------------------------------------------------------
// Outcome
// ---------------------------------------------------------------------------
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

    auto o1 = shrn::Outcome().expect_file(present, shrn::Expect::NON_EMPTY);
    CHECK(o1.ok(), "existing non-empty passes");

    auto o2 = shrn::Outcome().expect_file(absent, shrn::Expect::NON_EMPTY);
    CHECK(!o2.ok() && o2.detail() == "missing: " + absent.string(), "missing reported before emptiness");

    auto o3 = shrn::Outcome().expect_file(blank, shrn::Expect::NON_EMPTY);
    CHECK(!o3.ok() && o3.detail() == "empty: " + blank.string(), "empty reported");

    auto o4 = shrn::Outcome().expect_file(present, shrn::Expect::GZIPPED);
    CHECK(!o4.ok() && o4.detail() == "not gzipped: " + present.string(), "gzip flag");

    auto is_fastq = [](const fs::path& p) { return shrn::file_first_byte(p).value_or(-1) == '@'; };
    auto o5 = shrn::Outcome().expect_file(present, is_fastq, "FASTQ");
    CHECK(o5.ok(), "predicate passes");
    auto o6 = shrn::Outcome().expect_file(blank, is_fastq, "FASTQ");
    CHECK(!o6.ok() && o6.detail() == "not FASTQ: " + blank.string(), "predicate failure message");

    auto o7 = shrn::Outcome().expect_file(absent).expect_file(blank, shrn::Expect::NON_EMPTY);
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

    // Pre-run checks gate the spawn; post-run checks carry the command prefix.
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
                     .expect_file(marker, shrn::Expect::NON_EMPTY)
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

int main() {
    auto root = shrn::TempDir::create("shrn_test_");
    if (!root) {
        std::fprintf(stderr, "cannot create temp dir: %s\n", root.error().message().c_str());
        return 2;
    }

    test_expected();
    test_files(root->path());
    test_temp(root->path());
    test_process(root->path());
    test_outcome(root->path());

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
