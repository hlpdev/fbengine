#pragma once

// A pak written a file at a time, as fbe_cook_pak_open's is: a zip whose files are stored, as the
// engine's res:// reads one (vfs/pak.cpp), with Zip64 where sizes, offsets, or the count of files
// need it, since a game's pak can be bigger than memory, or than a UInt8[] holds.

#include "tools/atomic.hpp"
#include "tools/tools.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fbe::tools {

class pak_writer_t {
public:
    /// A pak to be written at `to`, a UTF-8 path on the disk, as an atomic_file_t is, making its
    /// folder. One `zip64` has Zip64's fields everywhere, as it would were each size and offset
    /// too big for the rest, for the tests.
    static std::expected<std::unique_ptr<pak_writer_t>, error_t> open(std::string_view to,
                                                                      bool zip64 = false);

    /// Adds the file at `from`, a UTF-8 path on the disk, as `name`, '/' between its folders, read
    /// and written a chunk at a time. So a finished pak always mounts, `name` is INVALID_DATA
    /// unless it's the path res:// has for it as it's written, of 4096 bytes and 255 names at
    /// most, well-formed UTF-8, and no other file's or folder's name, folded; a file that isn't
    /// there is NOT_FOUND. Those leave the pak as it was; one that fails as it's read or written,
    /// or runs out of memory, leaves it only to be let go of.
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

    pak_writer_t(std::string to, bool zip64);

    // The folded path of `name`, if it can be a file of it.
    std::expected<std::string, error_t> checked(std::string_view name);
    // Takes a file's folded path, and its folders', for one that's being added.
    void reserve(std::string_view key);
    std::expected<void, error_t> copied(std::string_view name, std::string_view from);
    // Writes `bytes` where it is, and moves on past them; false, after which it writes nothing
    // more, if it can't.
    bool write(std::span<const std::uint8_t> bytes);
    [[nodiscard]] error_t unwritable() const;

    std::string m_to;
    atomic_file_t m_file;
    std::uint64_t m_offset = 0;
    std::vector<entry_t> m_entries;
    // Its files' and their folders' folded paths.
    std::set<std::string> m_files;
    std::set<std::string> m_folders;
    bool m_zip64 = false;
    bool m_finished = false;
    bool m_failed = false;
};

} // namespace fbe::tools
