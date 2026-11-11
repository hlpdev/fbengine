#pragma once

// The disk: paths from UTF-8 and back, text made well-formed UTF-8, files read at any offset, and
// folders listed with their entries' kinds and when they last changed.
// std::filesystem's own conversions need libstdc++'s codecvt, whose names MinGW's UCRT build of
// the DLL can't link, so Windows' UTF-16 is made here.

#include "vfs/vfs.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fbe::vfs {

/// The path on the disk that `utf8` names.
std::filesystem::path disk_path(std::string_view utf8);

/// `path` in UTF-8, with U+FFFD for what Windows' UTF-16 has that isn't Unicode.
std::string utf8(const std::filesystem::path& path);

/// `bytes` as well-formed UTF-8: U+FFFD for each byte that doesn't start a well-formed sequence,
/// as std.io's ReadText has it.
std::string well_formed(std::string_view bytes);
/// How many bytes well_formed gives of `bytes`, found without making them.
std::uint64_t well_formed_size(std::string_view bytes);
/// The folder or file at `path`, through every link to it, as the OS has it, or `path` when it
/// can't say, as when nothing's there: the same for each way of reaching one.
std::filesystem::path real_path(const std::filesystem::path& path);

/// When a folder last changed, on the disk's clock, and whether that was long enough before now
/// that any change since has changed it again, however coarse the disk's clock is.
struct folder_time_t {
    std::uint64_t written = 0;
    bool settled = false;
};

/// When the folder at `path` last changed, or nullopt when there's no folder there.
std::optional<folder_time_t> folder_time(const std::filesystem::path& path);

/// A file or a folder in a folder on the disk: its path, its name in UTF-8, and its kind.
struct listed_t {
    std::filesystem::path path;
    std::string name;
    kind_t kind = kind_t::NONE;
};

/// What's in the folder at `path`, in no order: its files and folders, a link as what it links
/// to, and nothing else, each one's kind from the listing, so only a link is looked at on its own.
/// nullopt when it can't be listed.
std::optional<std::vector<listed_t>> list_folder(const std::filesystem::path& path);

/// A file open for reading at any offset, from several threads at once, which can be replaced,
/// renamed, or removed while it's open, as an updater does to a mounted pak, while what's open
/// stays readable.
/// How many bytes the file at `path` has, and when it was last written, as finely as its file
/// system keeps it: in 100 ns on Windows, whose C++ library's own gives whole seconds, and in
/// nanoseconds elsewhere. False if it can't be told, as for what isn't there.
bool disk_stamp(const std::filesystem::path& path, std::uint64_t& size, std::int64_t& written);

class read_file_t {
public:
    /// The file at `path`, or null, with `missing` saying whether that's because nothing's there.
    static std::unique_ptr<read_file_t> open(const std::filesystem::path& path, bool& missing);

    ~read_file_t();
    read_file_t(const read_file_t&) = delete;
    read_file_t& operator=(const read_file_t&) = delete;
    read_file_t(read_file_t&&) = delete;
    read_file_t& operator=(read_file_t&&) = delete;

    /// Reads `into.size()` bytes from `offset`; false if they aren't all there, or can't be read.
    [[nodiscard]] bool read_at(std::uint64_t offset, std::span<std::uint8_t> into) const;

    /// Its size when it was opened.
    [[nodiscard]] std::uint64_t size() const {
        return m_size;
    }

private:
#ifdef _WIN32
    // A HANDLE, which is a void*.
    using handle_t = void*;
#else
    // A file descriptor.
    using handle_t = int;
#endif

    read_file_t(handle_t handle, std::uint64_t size) : m_handle(handle), m_size(size) {}

    handle_t m_handle;
    std::uint64_t m_size;
};

} // namespace fbe::vfs
