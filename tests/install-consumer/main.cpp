#include <shrn.hpp>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

int main() {
    auto dir = shrn::TempDir::create();
    if (!dir) return 1;
    auto input = dir.path() / "input.txt";
    auto output = dir.path() / "output.txt";
    {
        std::ofstream file(input);
        file << "installed package\n";
        file.close();
        if (!file) return 1;
    }

    auto child = shrn::run({"sh", "-c", "printf captured >&2"});
    if (!child.launched() || !child.success() || child.stderr_output != "captured") {
        std::fprintf(stderr, "installed package could not run and capture a child\n");
        return 1;
    }

    shrn::RunOptions options;
    options.stdout_file = output;
    auto copied = shrn::run({"cat", input.string()}, options);
    if (!copied.success()) return 1;
    std::ifstream result(output);
    std::string content((std::istreambuf_iterator<char>(result)), std::istreambuf_iterator<char>());
    if (!result || content != "installed package\n") {
        std::fprintf(stderr, "installed package could not copy file contents\n");
        return 1;
    }
    return shrn::stage("verify output")
        .expect_file(output, shrn::file_non_empty, "non-empty")
        .or_die_if(true)
        .code();
}
