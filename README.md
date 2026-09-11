# shrn

Run subcommands and verify their outputs. Single header, C++20, POSIX.

```cpp
#include <shrn.hpp>

shrn::stage("minimap2 alignment")
    .expect_which("minimap2")                        // pre-run: fail fast, nothing spawned
    .expect_file(ref, shrn::Expect::NON_EMPTY)
    .expect_file(reads, is_fastq, "FASTQ")          // any bool(const fs::path&)
    .proc({"minimap2", "-x", "map-ont", ref, reads}, opts)
    .expect_file(paf, shrn::Expect::NON_EMPTY)      // post-run
    .proc({"sort", "-k1,1", "-o", paf, paf})    // only if everything above passed
    .or_die_if(!soft_fail)                          // report + exit under the stage name, or warn and continue
    .or_execute([&] { recover(); });
```

Everything except `Outcome::or_die_if` is silent and never exits: failures come
back as `shrn::result<T>` = `expected<T, std::error_code>`.

## Contents

| Area | API |
|---|---|
| `expected` | `shrn::expected`, `shrn::unexpected`, `shrn::result<T>` — `std::expected` when the standard library has it (C++23), otherwise an in-header polyfill with the same surface (`value`, `error`, `value_or`, `and_then`, `transform`, `or_else`, `transform_error`, `expected<void, E>`). Force the polyfill with `-DSHRN_FORCE_POLYFILL=1`. |
| Files | `file_readable`, `file_non_empty`, `file_is_gzipped` (magic bytes), `read_file`, `file_first_byte` (gzip-transparent), `line_count` (gzip-transparent, gzip detected by magic not extension), `ensure_directory`, `remove_if_exists`, `force_symlink`, `concat_files(inputs, out, concat_mode::raw|decompress)` |
| Temp | `make_temp_file[_in]` (`mkstemps`), `make_temp_dir[_in]` (`mkdtemp`), RAII `TempFile`, `TempDir` |
| Process | `run(args, RunOptions)` → `result<RunResult>`; `which`, `format_command` |
| Outcome | `stage(name)`, `proc` (free and member), `expect_file`, `expect_which`, `expect_success(int)`, `expect(bool, detail)`, `or_execute`, `or_die_if`, `with_error`, `with_stderr` |

### `run`

`fork`/`execvp` with PATH search and the environment inherited unchanged.
`RunOptions`: `workdir`, `capture_stderr` (default on), `inherit_stdout`,
`stdout_file`, `timeout` (child gets `SIGKILL` on expiry; `RunResult::timed_out`),
`on_spawn(stage, cmd)` observer; when unset, `shrn::default_spawn_hook()` is used
(assign it once for a process-wide "Running: ..." log line).

The error side of `result<RunResult>` is reserved for *launch* failures
(`ENOENT` for a missing binary, bad `workdir`, pipe/fork errors), detected via
a `CLOEXEC` status pipe rather than a fake exit code 127. A child that ran and
failed is a value with `!success()`; `summary()` describes exit code / signal /
timeout.

### `Outcome`

A stage is a timeline: checks and `proc` calls in the order they happen.
The first failure short-circuits the rest — later commands are not spawned —
so `detail()` names the earliest problem. Checks placed before a `proc`
are preconditions; the same methods after it verify outputs. Once a command
has run, details are prefixed with its argv[0] (`"minimap2: exited with code 1"`,
`"minimap2: missing: out.paf"`). `or_die_if(cond)` reports under the stage
name (or the last command when the stage is anonymous); `or_die_if(cond, name)`
overrides it.

### zlib

Detected with `__has_include(<zlib.h>)`; `SHRN_HAS_ZLIB` is `1` or `0`. Disable
with `-DSHRN_NO_ZLIB=1` (the CMake target does this automatically when zlib is
not found or `SHRN_USE_ZLIB=OFF`). Without zlib, `file_is_gzipped` still works;
gzip-transparent readers return `shrn::errc::zlib_unavailable` for gzip input.

## Integration

CMake, FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(shrn
    GIT_REPOSITORY https://github.com/f0t1h/shrn.git
    GIT_TAG        v0.1.0)
FetchContent_MakeAvailable(shrn)
target_link_libraries(your_target PRIVATE shrn::shrn)
```

Installed package: `find_package(shrn CONFIG REQUIRED)` then link `shrn::shrn`.
Or copy `include/shrn.hpp` and link zlib yourself (or define `SHRN_NO_ZLIB`).

## Tests

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

Two binaries: `shrn_tests_native` (C++23 when the compiler supports it, so
`std::expected` is exercised) and `shrn_tests_polyfill` (`SHRN_FORCE_POLYFILL`).

## License

MIT.
