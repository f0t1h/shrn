# shrn

Run subcommands and verify their outputs. Single header, C++20, POSIX.

## Example

Sort an existing `input.txt` into `sorted.txt`. Failed input checks prevent the
command from running; a command or output-check failure is reported under the
stage name.

```cpp
#include <shrn.hpp>

int main() {
    shrn::RunOptions options;
    options.stdout_file = "sorted.txt";

    shrn::stage("sort input")
        .expect_which("sort")
        .expect_file("input.txt", shrn::file_non_empty, "non-empty")
        .proc({"sort", "input.txt"}, options)
        .expect_file("sorted.txt", shrn::file_non_empty, "non-empty")
        .or_die_if(true);
}
```

`or_die_if(true)` prints a failure and exits. To handle failures yourself, store
the `Outcome` and inspect `ok()`, `detail()`, and `stderr_output()` instead.

## Results

Failures are error strings carried by the value, not a separate result type.
`run(args)` returns a `RunResult` directly: `launched()` reports whether the
child started (`error` holds the OS failure text otherwise), `success()`
whether it exited with code 0, and `summary()` a human-readable description.
`spawn()` returns a `Process`; a failed launch yields a `Process` whose
`error()` is nonempty and which owns no child.

These functions do not print diagnostics or exit. `Outcome::or_die_if` handles
reporting; child programs and user callbacks control their own output.

No shrn function throws. Allocation failure during stderr capture and
thread-creation failure are reported through the same error strings as every
other failure. Exceptions from `call()` callbacks propagate to the caller;
that is the callback throwing, not shrn. The header also builds with
`-fno-exceptions`; in that mode thread-creation failure terminates the process.

## Processes

`spawn(args, options)` starts a child and returns `Process` after exec
succeeds. `run(args, options)` is the synchronous version: spawn, then wait.
Both use `fork` and `execvp`, search PATH, and inherit the
environment. Pass each argument as a separate string. The argument list is not
parsed as a shell command: pipes, wildcards, and redirection are not expanded.
`which(name)` searches for an executable; `format_command(args)` quotes arguments
for display, not execution.

| `RunOptions` field | Behavior |
|---|---|
| `workdir` | Change the child's directory before exec. |
| `capture_stderr` | Capture stderr using one reader thread; default `true`. |
| `inherit_stdout` | Default `true`; `false` sends stdout to `/dev/null`. |
| `stdout_file` | Create or truncate this file; overrides `inherit_stdout`. Relative paths use the caller's directory, not `workdir`. |
| `on_spawn` | Callback receiving the stage name and formatted command before launch. Uses `default_spawn_hook()` when unset. |

Launch errors (`error()` on the `Process`, e.g. "No such file or directory"
for a missing binary, bad `workdir`, pipe/fork failures) are detected via a
`CLOEXEC` status pipe, not reported as exit code 127. A child that ran and
failed has `!success()`; `summary()` describes its exit code or signal.

`Process` is move-only and owns its direct child:

| Method | Behavior |
|---|---|
| `running()` | Nonblocking status query. |
| `wait()` | Wait indefinitely and return a reference to the cached `RunResult`. |
| `wait_for(duration)` | Return `wait_status`: `ready` or `timeout`. Expiry neither kills nor fails the child. |
| `terminate()` | SIGKILL and reap the direct child; kill failures land in `wait()`'s error. |
| `error()` | The launch-failure text; empty when the child started. |

Durations are milliseconds measured from the wait call, not launch. A zero or
negative duration polls once. No background timer monitors the child.
Destruction waits and reaps silently; move assignment first waits for the
previously owned child. Do not call methods concurrently on the same handle.

Only stderr capture needs a reader thread; `stdout_file` is ordinary file
redirection. Waiting collects buffered stderr without waiting for descendants
to close inherited writers; later descendant output is not captured. Termination
does not kill a process group. Internal descriptors are closed across exec.
Spawn hooks run on the launching thread.

## Stages

A stage starts each command asynchronously. Its next `proc` or ordinary
expectation waits for that command before proceeding.
After the first failure, later checks and commands are skipped.
`detail()` describes that first failure. Checks before a `proc` validate its
inputs; checks after it verify its outputs. Once a command has run, its argv[0]
prefixes error details (for example, `"minimap2: missing: out.paf"`).
`or_die_if(cond)` reports under the stage
name (or the last command when the stage is anonymous); `or_die_if(cond, name)`
overrides it.

`expect_file(path)` requires a readable regular file. Add a predicate and a
label for anything stricter, e.g. `expect_file(path, shrn::file_non_empty,
"non-empty")` or `expect_file(path, is_fastq, "FASTQ")`. The predicate receives
`const std::filesystem::path&` and returns whether the file is acceptable; a
failed check is reported as `not <label>: <path>`.

Use `expect([&] { return check_output(); }, "output check")` to evaluate a check
after the command finishes. With `expect(bool, label)`, C++ evaluates the boolean
argument before the method can wait.

`call(fn, args...)` waits for the previous command, then invokes the function
on the caller's thread with perfectly forwarded arguments. Return `0` for success
or a nonzero integer for failure; later calls, commands, and checks are skipped
after failure. Pass output arguments by reference. Exceptions propagate.
Calls are synchronous: process deadlines and termination do not interrupt them.

```cpp
#include <shrn.hpp>

int main() {
    int output = 0;
    shrn::stage("calculate")
        .call([](int input, int& result) {
            result = input * 2;
            return 0;
        }, 21, output)
        .expect([&] { return output == 42; }, "incorrect result")
        .or_die_if(true);
}
```

| `Outcome` method | Behavior |
|---|---|
| `wait()` | Fluent untimed wait, even after a recorded failure. |
| `wait_for(duration)` | Return `wait_status::ready` or `timeout`; expiry does not fail the stage. |
| `running()` | Nonblocking query; does not wait for completion. |
| `expect_done_within(duration)` | Wait up to the duration and record failure on expiry, without killing. |
| `or_terminate()` | On recorded failure, kill and reap the active direct child without waiting first. Preserve the first failure. |

`ok()`, `code()`, `detail()`, `stderr_output()`, and failure handlers wait while
no failure is recorded. After a deadline failure they return or act immediately.
Captured stderr is finalized by waiting or termination, not exposed as a live
snapshot. A later successful wait does not clear a deadline failure.

Start separate stages before joining them with `after`; no prior `wait()` is needed:

```cpp
#include <shrn.hpp>

int main() {
    auto first = shrn::stage("first").proc({"sleep", "1"});
    auto second = shrn::stage("second").proc({"sleep", "1"});
    shrn::stage("dependent")
        .after(first, second)
        .proc({"echo", "both succeeded"})
        .or_die_if(true);
}
```

`after(a, b, ...)` waits for the receiving stage's active command and every
prerequisite, even if a failure has already been recorded. It permits subsequent
work only if all succeeded. Otherwise it preserves the receiving stage's existing
failure, or records the first failed prerequisite in argument order with its name
and failure detail. It does not combine stderr or cancel siblings.

Prerequisites are borrowed for the call, not moved or retained; their results
remain available and can be reused by other dependent stages. A prerequisite
with a recorded deadline failure is still joined, so terminate it explicitly
before `after` if an untimed wait is unwanted.

Stages are move-only. Their destructors wait silently, including after a failed
deadline expectation: use `or_terminate()` if waiting indefinitely is unwanted.
`or_die_if(true)` terminates its own active child before exiting on failure.
It does not clean up sibling stages; `std::exit` does not destroy local objects.

`or_die_if(false)` warns and continues. `or_execute(fn)` calls `fn` on failure
but does not clear the failed state. `with_error` and `with_stderr` replace the
stored text; they do not mark the stage as failed.

### Stage temp files

`temp_file{"name"}` names a file in a private directory owned by the stage.
Use it anywhere an argument or path goes; the same name always resolves to the
same file within one stage, so one command's output is the next one's input:

```cpp
#include <shrn.hpp>

int main() {
    shrn::stage("map reads")
        .expect_which("minimap2")
        .expect_which("samtools")
        .expect_file("reads.fq", shrn::file_non_empty, "non-empty")
        .proc({"minimap2", "-a", "ref.fa", "reads.fq", "-o", shrn::temp_file{"aln.sam"}})
        .expect_file(shrn::temp_file{"aln.sam"}, shrn::file_non_empty, "non-empty")
        .proc({"samtools", "sort", "-o", "aln.bam", shrn::temp_file{"aln.sam"}})
        .proc({"samtools", "index", "aln.bam"})
        .expect_file("aln.bam.bai")
        .or_die_if(true);
}
```

The directory is created on first use under `$TMPDIR` (or `/tmp`) as
`shrn_<stage>_<pid>_<uuid>_XXXXXX`, so concurrent runs and same-named stages
never collide. It is removed when the stage is destroyed. Files inside keep
the names you gave them, so tools that inspect extensions or write sidecar
files (an index beside its BAM) behave normally. Names are single path
components; separators and `..` fail the stage.

`call(fn, shrn::temp_file{"x"})` passes the resolved path as
`const std::filesystem::path&`. `temp_path("x")` returns it directly, for
example to hand a result to a later stage; a stage joined with `after` outlives
the reference. Tokens are stage-local and never resolve across stages.

Pass `{.keep_temps = true}` to keep the directory:
`shrn::stage("map reads", {.keep_temps = true})`. On a fatal `or_die_if(true)`
the process exits without running destructors; the directory survives and its
path is printed with the error so the artifacts can be inspected.

## Files

- `file_readable` and `file_non_empty` return booleans.
- `ensure_directory` creates missing parent directories.

## Integration

CMake, FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(shrn
    GIT_REPOSITORY https://github.com/f0t1h/shrn.git
    GIT_TAG        v0.5.0)
FetchContent_MakeAvailable(shrn)
target_link_libraries(your_target PRIVATE shrn::shrn)
```

Installed package: `find_package(shrn CONFIG REQUIRED)` then link `shrn::shrn`.
Or copy `include/shrn.hpp` and enable thread support (`-pthread` with
GCC/Clang on Linux). The CMake target carries the Threads dependency; shrn
needs nothing else — no Boost, no zlib.

## Tests

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

One binary, `shrn_tests`, built with `-Wall -Wextra -Wpedantic`.

GitHub Actions runs the test binary on Ubuntu 24.04 with GCC 14 and Clang 18.
Each job also installs to a custom header directory, then builds and runs a
separate `find_package` consumer. CI runs on
pushes, pull requests, and manual dispatch. Other POSIX platforms are not covered
by this workflow.

## License

MIT.
