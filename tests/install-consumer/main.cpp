#include <shrn.hpp>

#include <cstdio>
#include <fstream>

int main() {
    auto dir = shrn::TempDir::create();
    if (!dir) return 1;
    auto input = dir->path() / "input.txt";
    auto output = dir->path() / "output.txt";
    {
        std::ofstream file(input);
        file << "installed package\n";
        file.close();
        if (!file) return 1;
    }

    auto child = shrn::run({"sh", "-c", "printf captured >&2"});
    if (!child || !child->success() || child->stderr_output != "captured") {
        std::fprintf(stderr, "installed package could not run and capture a child\n");
        return 1;
    }

    // Exercise the zlib-linked path, or its plain-file fallback.
    auto copied = shrn::concat_files({input}, output, shrn::concat_mode::decompress);
    if (!copied) return 1;
    auto content = shrn::read_file(output);
    if (!content || *content != "installed package\n") {
        std::fprintf(stderr, "installed package could not copy file contents\n");
        return 1;
    }
    return 0;
}
