// Slang's compiler as far as a test of what's asked of it needs: it says FBE_SLANG_VERSION to -v;
// and for a compile, adds a line to the file FBENGINE_FAKE_SLANGC_LOG names, writes the SPIR-V the
// file FBENGINE_FAKE_SLANGC_SPV has to its -o, and a depfile of nothing of the game's to its
// -depfile.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>

// NOLINTNEXTLINE(bugprone-exception-escape, misc-const-correctness)
int main(int count, char** arguments) {
    const char* to = nullptr;
    const char* depfile = nullptr;
    for (int at = 1; at < count; ++at) {
        const std::string_view argument = arguments[at];
        if (argument == "-v") {
            std::puts(FBE_SLANG_VERSION);
            return 0;
        }
        if (argument == "-o" && at + 1 < count) {
            to = arguments[at + 1];
        }
        if (argument == "-depfile" && at + 1 < count) {
            depfile = arguments[at + 1];
        }
    }
    // NOLINTBEGIN(clang-diagnostic-deprecated-declarations): only read here, and a test's own
    // files.
    const char* log = std::getenv("FBENGINE_FAKE_SLANGC_LOG");
    const char* spirv = std::getenv("FBENGINE_FAKE_SLANGC_SPV");
    if (to == nullptr || log == nullptr || spirv == nullptr) {
        return 1;
    }
    if (std::FILE* file = std::fopen(log, "a"); file != nullptr) {
        std::fputs("compiled\n", file);
        std::fclose(file);
    }
    std::error_code failed;
    std::filesystem::copy_file(spirv, to, std::filesystem::copy_options::overwrite_existing,
                               failed);
    if (failed) {
        return 1;
    }
    if (depfile != nullptr) {
        if (std::FILE* file = std::fopen(depfile, "w"); file != nullptr) {
            std::fputs("own.spv:\n", file);
            std::fclose(file);
        }
    }
    // NOLINTEND(clang-diagnostic-deprecated-declarations)
    return 0;
}
