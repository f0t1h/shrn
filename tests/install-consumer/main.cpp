#include <shrn.hpp>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int main() {
    auto child = shrn::run({"sh", "-c", "printf captured >&2"});
    if (!child.launched() || !child.success() || child.stderr_output != "captured") {
        std::fprintf(stderr, "installed package could not run and capture a child\n");
        return 1;
    }

    // Chain commands through stage-scoped temp files and verify the result.
    std::string content;
    auto pipeline = shrn::stage("installed consumer")
        .proc({"sh", "-c", "printf 'installed package' > \"$0\"", shrn::temp_file{"input.txt"}})
        .expect_file(shrn::temp_file{"input.txt"}, shrn::file_non_empty, "non-empty")
        .proc({"cp", shrn::temp_file{"input.txt"}, shrn::temp_file{"output.txt"}})
        .call([&content](const fs::path& p) { content = slurp(p); return 0; }, shrn::temp_file{"output.txt"});
    if (!pipeline.ok() || content != "installed package") {
        std::fprintf(stderr, "stage temp files failed: %s\n", pipeline.detail().c_str());
        return 1;
    }
    return 0;
}
