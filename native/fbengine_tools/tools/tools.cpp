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

} // namespace

namespace fbe::tools {

bool fail(std::string_view message) {
    last_error = message;
    return false;
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
    // An ofstream truncates what it opens.
    std::ofstream stream(file, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) {
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

} // extern "C"
