#pragma once

// A pak written a file at a time, as fbe_cook_pak_open's is: a zip whose files are stored, as the
// engine's res:// reads one (vfs/pak.cpp), with Zip64 where sizes, offsets, or the count of files
// need it, since a game's pak can be bigger than memory, or than a UInt8[] holds.

#include "tools/tools.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fbe::tools {

class pak_writer_t {
public:
    /// A pak to be written at `to`, a UTF-8 path on the disk, making its folder: it's written
    /// beside it, as `to`.cook, then renamed over it by finish. One `zip64` has Zip64's fields
    /// everywhere, as it would were each size and offset too big for the rest, for the tests.
    static std::expected<std::unique_ptr<pak_writer_t>, error_t> open(std::string_view to,
                                                                      bool zip64 = false);

    pak_writer_t(const pak_writer_t&) = delete;
    pak_writer_t& operator=(const pak_writer_t&) = delete;
    pak_writer_t(pak_writer_t&&) = delete;
    pak_writer_t& operator=(pak_writer_t&&) = delete;
    /// Takes out what was written of one that isn't finished.
    ~pak_writer_t();

    /// Adds the file at `from`, a UTF-8 path on the disk, as `name`, '/' between its folders,
    /// read and written a chunk at a time. A name that's empty, holds a '\' or a NUL, ends with
    /// '/', or is longer than 65535 bytes is INVALID_DATA; a file that isn't there NOT_FOUND.
    std::expected<void, error_t> add(std::string_view name, std::string_view from);

    /// Writes its central directory and puts it in place, after which it adds nothing more.
    std::expected<void, error_t> finish();

private:
    struct entry_t {
        std::string name;
        std::uint64_t header = 0;
        std::uint64_t size = 0;
        std::uint32_t crc = 0;
    };

    pak_writer_t(std::string to, std::filesystem::path file, std::filesystem::path written,
                 bool zip64);

    // Writes `bytes` where it is, and moves on past them; false, after which it writes nothing
    // more, if it can't.
    bool write(std::span<const std::uint8_t> bytes);
    [[nodiscard]] error_t unwritable() const;

    std::string m_to;
    std::filesystem::path m_file;
    std::filesystem::path m_written;
    // Held apart, so it's closed by its destructor, which doesn't throw, before what it wrote is
    // taken out.
    std::unique_ptr<std::ofstream> m_stream;
    std::uint64_t m_offset = 0;
    std::vector<entry_t> m_entries;
    bool m_zip64 = false;
    bool m_finished = false;
    bool m_failed = false;
};

} // namespace fbe::tools
