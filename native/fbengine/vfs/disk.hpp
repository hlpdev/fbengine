#pragma once

// The disk: paths from UTF-8 and back, text made well-formed UTF-8, and files read at any offset.
// std::filesystem's own conversions need libstdc++'s codecvt, whose names MinGW's UCRT build of
// the DLL can't link, so Windows' UTF-16 is made here.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace fbe::vfs {

/// The path on the disk that `utf8` names.
std::filesystem::path disk_path(std::string_view utf8);

/// `path` in UTF-8, with U+FFFD for what Windows' UTF-16 has that isn't Unicode.
std::string utf8(const std::filesystem::path& path);

/// `bytes` as well-formed UTF-8: U+FFFD for each byte that doesn't start a well-formed sequence,
/// as std.io's ReadText has it.
std::string well_formed(std::string_view bytes);

/// A file open for reading at any offset, from several threads at once, which can be replaced,
/// renamed, or removed while it's open, as an updater does to a mounted pak, while what's open
/// stays readable.
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
