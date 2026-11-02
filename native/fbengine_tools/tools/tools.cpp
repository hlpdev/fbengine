// fbengine_tools' errors, and its files, read and written through fbengine's own UTF-8 paths
// (vfs/disk.cpp, built into this DLL too), since std::filesystem's UTF-8 conversions don't link
// in a MinGW UCRT DLL.

#include "tools/tools.hpp"

#include "fbengine_tools.h"
#include "vfs/disk.hpp"

#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <system_error>

namespace {

thread_local std::string last_error;
thread_local std::int32_t last_failure = FBE_TOOLS_OTHER;

} // namespace

namespace fbe::tools {

bool fail(const error_t& error) {
    last_error = error.message;
    switch (error.failure) {
    case failure_t::INVALID_DATA:
        last_failure = FBE_TOOLS_INVALID_DATA;
        break;
    case failure_t::NOT_FOUND:
        last_failure = FBE_TOOLS_NOT_FOUND;
        break;
    default:
        last_failure = FBE_TOOLS_OTHER;
        break;
    }
    return false;
}

bool fail(std::string_view message) {
    return fail(other(std::string(message)));
}

std::filesystem::path disk_path(std::string_view utf8) {
    return vfs::disk_path(utf8);
}

std::expected<bytes_t, std::string> read_file(std::string_view path) {
    bool missing = false;
    const std::unique_ptr<vfs::read_file_t> file = vfs::read_file_t::open(disk_path(path), missing);
    if (file == nullptr) {
        return std::unexpected(
            std::format("couldn't read {}{}", path, missing ? ": it isn't there" : ""));
    }
    bytes_t bytes(static_cast<std::size_t>(file->size()));
    if (!file->read_at(0, bytes)) {
        return std::unexpected(std::format("couldn't read {}", path));
    }
    return bytes;
}

std::expected<void, std::string> write_file(std::string_view path,
                                            std::span<const std::uint8_t> bytes) {
    const std::filesystem::path file = disk_path(path);
    std::error_code error;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), error);
    }
    // Beside it, by a name of the OS's own characters, as a std::filesystem::path's conversions
    // from a char's would need codecvt; an ofstream that couldn't open stays good on MinGW, so
    // that's asked too.
    const std::filesystem::path written(
        file.native() + std::filesystem::path::string_type{ '.', 'c', 'o', 'o', 'k' });
    {
        std::ofstream stream(written, std::ios::binary);
        if (!stream.is_open()) {
            return std::unexpected(std::format("couldn't write {}", path));
        }
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (stream.fail()) {
            std::filesystem::remove(written, error);
            return std::unexpected(std::format("couldn't write {}", path));
        }
    }
    std::filesystem::rename(written, file, error);
    if (error) {
        std::filesystem::remove(written, error);
        return std::unexpected(std::format("couldn't write {}", path));
    }
    return {};
}

} // namespace fbe::tools

extern "C" {

FBE_TOOLS_API const char* fbe_tools_version(void) {
    return FBE_VERSION;
}

FBE_TOOLS_API const char* fbe_tools_error(void) {
    return last_error.c_str();
}

FBE_TOOLS_API int32_t fbe_tools_failure(void) {
    return last_failure;
}

} // extern "C"
