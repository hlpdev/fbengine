// fbengine_tools' errors, and its files, read and written through fbengine's own UTF-8 paths
// (vfs/disk.cpp, built into this DLL too), since std::filesystem's UTF-8 conversions don't link
// in a MinGW UCRT DLL.

#include "tools/tools.hpp"

#include "fbengine_tools.h"
#include "tools/atomic.hpp"
#include "vfs/disk.hpp"

#include <format>
#include <memory>
#include <span>

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
    atomic_file_t file;
    if (std::expected<void, std::string> opened = file.open(path); !opened) {
        return opened;
    }
    if (!file.write_at(0, bytes)) {
        return std::unexpected(std::format("couldn't write {}", path));
    }
    return file.commit();
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
