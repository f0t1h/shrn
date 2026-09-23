# shrn

Run subcommands and verify their outputs. Single header, C++20, POSIX, no dependencies.

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

A stage is a sequence of checks and commands. Checks before a command validate
its inputs and stop it from running; checks after it verify its outputs. The
first failure wins and `or_die_if(true)` reports it under the stage name. To
handle failures yourself, keep the `Outcome` and read `ok()`, `detail()`, and
`stderr_output()`.

## Features

- Commands start asynchronously; the next check or command waits for them.
  Join independent stages with `after(a, b)`.
- `call(fn, args...)` runs an in-process step in the same sequence as commands.
- `temp_file{"x"}` names a scratch file in a per-stage private directory,
  removed with the stage.
- `StageTemplate` records a stage with `slot` placeholders and `launch()`es it
  many times.
- `in(path)` / `out(path)` tags skip a command whose outputs are newer than its
  inputs, like `make`.
- Argument lists accept `{flag, value}` pairs and `std::optional`, so a
  conditional flag needs no `push_back`.
- No exceptions, no output except from `or_die_if`. Failures are strings on the
  value. Builds with `-fno-exceptions`.

Every function and option is documented in [`include/shrn.hpp`](include/shrn.hpp).

## Temp files and templates

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

A template is the same stage with holes. `slot{"name"}` is required,
`slot{"name", "default"}` is not, `many{"name"}` binds a list, and
`optional{...}` drops a group whole when a slot inside it is unbound. Each
`launch()` gets its own temp directory.

```cpp
#include <shrn.hpp>

int main() {
    const auto align = shrn::StageTemplate("align")
        .expect_which("minimap2")
        .expect_file(shrn::slot{"reads"}, shrn::file_non_empty, "non-empty")
        .proc({"minimap2", "-a", "-t", shrn::slot{"threads", "4"},
               shrn::optional{"-x", shrn::slot{"preset"}},
               shrn::slot{"ref"}, shrn::slot{"reads"}, "-o", shrn::slot{"out"}})
        .expect_file(shrn::slot{"out"}, shrn::file_non_empty, "non-empty");

    align.launch({{"ref", "ref.fa"}, {"reads", "a.fq"}, {"out", "a.sam"}})
        .or_die_if(true);
    align.launch({{"ref", "ref.fa"}, {"reads", "b.fq"}, {"out", "b.sam"},
                       {"preset", "map-ont"}, {"threads", "16"}})
        .or_die_if(true);
}
```

## Argument lists

Each element of a braced list yields zero, one, or several argv entries: text,
paths, and integers yield one; `std::optional` yields its value or nothing;
`each(vector)` splices; `{flag, value}` puts the flag before each entry the
value yields and vanishes with it.

```cpp
#include <shrn.hpp>
#include <optional>

int main(int argc, char** argv) {
    std::optional<std::filesystem::path> mate = argc > 2 ? std::optional(argv[2]) : std::nullopt;
    std::vector<std::filesystem::path> refs = {"a.fa", "b.fa"};
    shrn::stage("align")
        .proc({"printf", "%s\\n",
               {"-1", shrn::in(argv[1])},
               {"-2", shrn::in(mate)},     // absent -> no -2 at all
               {"-t", 8},
               {"-r", shrn::each(refs)}})  // -r a.fa -r b.fa
        .or_die_if(true);
}
```

In a template, `{"-2", slot{"r2"}}` is an optional argument and `{"-l", many{"x"}}`
repeats the flag per item.

## Freshness

A command with any `out` is skipped when every output exists and none is older
than any `in`. Only that command is skipped. `out` alone means skip-if-exists;
`RunOptions::inputs`/`outputs` cover files a tool does not name in argv;
`StageOptions{.force = true}` runs everything.

```cpp
#include <shrn.hpp>

int main() {
    shrn::stage("sort and index")
        .expect_file("input.txt", shrn::file_non_empty, "non-empty")
        .proc({"sort", shrn::in("input.txt"), "-o", shrn::out("sorted.txt")})  // skipped while sorted.txt is newer
        .proc({"gzip", "-kf", shrn::in("sorted.txt")}, {.outputs = {"sorted.txt.gz"}})  // output not named in argv
        .expect_file("sorted.txt.gz", shrn::file_non_empty, "non-empty")
        .or_die_if(true);
}
```

`shrn::fresh(outputs, inputs)` is the predicate on its own; it lets a script
rebuild itself when its source changes:

```cpp
#include <shrn.hpp>
#include <unistd.h>

int main(int, char** argv) {
    const std::filesystem::path exe = std::filesystem::canonical("/proc/self/exe");
    if (!shrn::fresh({exe}, {"script.cpp", "include/shrn.hpp"})) {
        shrn::stage("rebuild")
            .proc({"c++", "-std=c++20", "-pthread", "-Iinclude", "script.cpp", "-o", shrn::temp_file{"fresh"}})
            .call([&](const std::filesystem::path& fresh_binary) {
                std::filesystem::rename(fresh_binary, exe);
                return 0;
            }, shrn::temp_file{"fresh"})
            .or_die_if(true);
        execv(exe.c_str(), argv);  // start over in the rebuilt program
        return 1;
    }
    // ... the program itself ...
    return 0;
}
```

## Notes

- `run(args)` and `spawn(args)` are the plain process layer: `fork`/`execvp`,
  PATH search, no shell. `RunResult` and `Process` carry launch errors as text.
- Stage destructors wait for their child. `or_die_if(true)` kills its own child
  and exits without running other destructors; a kept temp directory's path is
  printed with the error.
- Stderr is captured by a reader thread; stdout is inherited or redirected to
  a file. Descendants are not killed or drained.
- `-pthread` is the only requirement; the CMake target carries it.

## Installation

```cmake
include(FetchContent)
FetchContent_Declare(shrn
    GIT_REPOSITORY https://github.com/f0t1h/shrn.git
    GIT_TAG        v0.11.0)
FetchContent_MakeAvailable(shrn)
target_link_libraries(your_target PRIVATE shrn::shrn)
```

Or `find_package(shrn CONFIG REQUIRED)` after `cmake --install`, or copy
`include/shrn.hpp`.

Tests: `cmake -S . -B build && cmake --build build && ctest --test-dir build`.

## License

MIT.
