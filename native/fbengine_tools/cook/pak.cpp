// A pak written a file at a time: each file's local header, then its bytes, a chunk at a time,
// and its CRC written into the header after them; then the central directory, and Zip64's end
// records where its count, size, or offset needs them.

#include "cook/pak.hpp"

#include "vfs/disk.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <new>
#include <utility>

namespace fbe::tools {

namespace {

constexpr std::uint32_t local_signature = 0x0403'4B50;
constexpr std::uint32_t central_signature = 0x0201'4B50;
constexpr std::uint32_t end_signature = 0x0605'4B50;
constexpr std::uint32_t zip64_end_signature = 0x0606'4B50;
constexpr std::uint32_t zip64_locator_signature = 0x0706'4B50;
constexpr std::uint64_t wide = 0xFFFF'FFFF;
constexpr std::uint64_t most_count = 0xFFFF;
constexpr std::uint16_t zip64_extra = 0x0001;
// Zip 4.5's, the version with Zip64, and 2.0's, which a stored file needs.
constexpr std::uint16_t zip64_version = 45;
constexpr std::uint16_t stored_version = 20;
constexpr std::uint16_t utf8_names = 0x0800;
// 1980-01-01, at midnight, as std.compress's ZipWriter dates a file it isn't given a date of.
constexpr std::uint16_t dos_date = 0x0021;
// Where a local header's CRC is, and the size of a Zip64 end record past its first 12 bytes.
constexpr std::size_t crc_at = 14;
constexpr std::uint64_t zip64_end_rest = 44;
// No OS has a path longer than this, or of more names, as the engine's reader has them.
constexpr std::size_t most_path = 4096;
constexpr std::size_t most_names = 255;
// How much of a file is read and written at once.
constexpr std::size_t chunk = std::size_t{ 1 } << 20U;

constexpr std::array<std::uint32_t, 256> crc_table = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0 ? 0xEDB8'8320U ^ (value >> 1U) : value >> 1U;
        }
        table[i] = value;
    }
    return table;
}();

// Zip's CRC-32 of what `crc` was of, then of `bytes`.
std::uint32_t crc32(std::uint32_t crc, std::span<const std::uint8_t> bytes) {
    crc = ~crc;
    for (const std::uint8_t byte : bytes) {
        crc = crc_table[(crc ^ byte) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

// `value`'s `size` bytes, little-endian, after what `bytes` has.
void put(bytes_t& bytes, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
    }
}

} // namespace

std::expected<std::unique_ptr<pak_writer_t>, error_t> pak_writer_t::open(std::string_view to,
                                                                         bool zip64) {
    std::unique_ptr<pak_writer_t> made(new pak_writer_t(std::string(to), zip64));
    if (std::expected<void, std::string> opened = made->m_file.open(to); !opened) {
        return std::unexpected(other(std::move(opened.error())));
    }
    return made;
}

pak_writer_t::pak_writer_t(std::string to, bool zip64) : m_to(std::move(to)), m_zip64(zip64) {}

std::expected<std::string, error_t> pak_writer_t::checked(std::string_view name) {
    const auto refused = [&](std::string_view why) {
        return std::unexpected(
            invalid(std::format("a pak's file can't be named '{}': {}", name, why)));
    };
    if (name.size() > most_path) {
        return refused("it's longer than a path can be");
    }
    if (vfs::well_formed(name) != name) {
        return refused("it isn't UTF-8");
    }
    const vfs::result_t<vfs::path_t> path = vfs::parse(std::format("res://{}", name));
    if (!path || path->keys.empty() || path->text() != std::format("res://{}", name)) {
        return refused("res:// has no such path");
    }
    if (path->keys.size() > most_names) {
        return refused("it's deeper than a path can be");
    }
    std::string key;
    for (std::size_t i = 0; i + 1 < path->keys.size(); ++i) {
        key += path->keys[i];
        if (m_files.contains(key)) {
            return refused("a file has a name of its folders, in some case");
        }
        key += '/';
    }
    key += path->keys.back();
    if (m_files.contains(key)) {
        return refused("a file has its name, in some case");
    }
    if (m_folders.contains(key)) {
        return refused("a folder has its name, in some case");
    }
    return key;
}

void pak_writer_t::reserve(std::string_view key) {
    for (std::size_t slash = key.find('/'); slash != std::string_view::npos;
         slash = key.find('/', slash + 1)) {
        m_folders.emplace(key.substr(0, slash));
    }
    m_files.emplace(key);
}

std::expected<void, error_t> pak_writer_t::add(std::string_view name, std::string_view from) {
    try {
        if (vfs::faulted(vfs::fault_t::OUT_OF_MEMORY)) {
            throw std::bad_alloc();
        }
        return copied(name, from);
    } catch (...) {
        m_failed = true;
        throw;
    }
}

std::expected<void, error_t> pak_writer_t::copied(std::string_view name, std::string_view from) {
    if (m_finished || m_failed) {
        return std::unexpected(other(std::format("{} is finished, or failed", m_to)));
    }
    const std::expected<std::string, error_t> key = checked(name);
    if (!key) {
        return std::unexpected(key.error());
    }
    bool missing = false;
    const std::unique_ptr<vfs::read_file_t> source =
        vfs::read_file_t::open(vfs::disk_path(from), missing);
    if (source == nullptr) {
        return std::unexpected(error_t{
            .failure = missing ? failure_t::NOT_FOUND : failure_t::OTHER,
            .message = std::format("couldn't read {}{}", from, missing ? ": it isn't there" : ""),
        });
    }
    reserve(*key);
    entry_t entry{ .name = std::string(name), .header = m_offset, .size = source->size() };
    const bool big = m_zip64 || entry.size >= wide;
    bytes_t header;
    put(header, local_signature, 4);
    put(header, big ? zip64_version : stored_version, 2);
    put(header, utf8_names, 2);
    // Stored, at midnight, and its CRC, which is written once its bytes are.
    put(header, 0, 2);
    put(header, 0, 2);
    put(header, dos_date, 2);
    put(header, 0, 4);
    put(header, big ? wide : entry.size, 4);
    put(header, big ? wide : entry.size, 4);
    put(header, name.size(), 2);
    put(header, big ? 20 : 0, 2);
    header.insert(header.end(), name.begin(), name.end());
    if (big) {
        put(header, zip64_extra, 2);
        put(header, 16, 2);
        put(header, entry.size, 8);
        put(header, entry.size, 8);
    }
    if (!write(header)) {
        return std::unexpected(unwritable());
    }
    bytes_t bytes(static_cast<std::size_t>(std::min<std::uint64_t>(entry.size, chunk)));
    for (std::uint64_t at = 0; at < entry.size;) {
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(entry.size - at, chunk));
        const std::span<std::uint8_t> part(bytes.data(), count);
        if (!source->read_at(at, part)) {
            m_failed = true;
            return std::unexpected(other(std::format("couldn't read {}", from)));
        }
        entry.crc = crc32(entry.crc, part);
        if (!write(part)) {
            return std::unexpected(unwritable());
        }
        at += count;
    }
    bytes_t crc;
    put(crc, entry.crc, 4);
    if (!m_file.write_at(entry.header + crc_at, crc)) {
        m_failed = true;
        return std::unexpected(unwritable());
    }
    m_entries.push_back(std::move(entry));
    return {};
}

std::expected<void, error_t> pak_writer_t::finish() {
    if (m_finished || m_failed) {
        return std::unexpected(other(std::format("{} is finished, or failed", m_to)));
    }
    const std::uint64_t directory = m_offset;
    bytes_t bytes;
    for (const entry_t& entry : m_entries) {
        const bool big = m_zip64 || entry.size >= wide;
        const bool far = m_zip64 || entry.header >= wide;
        const std::size_t extra = (big ? std::size_t{ 16 } : 0) + (far ? std::size_t{ 8 } : 0);
        put(bytes, central_signature, 4);
        // Made by Zip 4.5 on MS-DOS's file system, whose attributes it has none of.
        put(bytes, zip64_version, 2);
        put(bytes, big || far ? zip64_version : stored_version, 2);
        put(bytes, utf8_names, 2);
        put(bytes, 0, 2);
        put(bytes, 0, 2);
        put(bytes, dos_date, 2);
        put(bytes, entry.crc, 4);
        put(bytes, big ? wide : entry.size, 4);
        put(bytes, big ? wide : entry.size, 4);
        put(bytes, entry.name.size(), 2);
        put(bytes, extra == 0 ? 0 : extra + 4, 2);
        // No comment, the one disk, and no attributes.
        put(bytes, 0, 2);
        put(bytes, 0, 2);
        put(bytes, 0, 2);
        put(bytes, 0, 4);
        put(bytes, far ? wide : entry.header, 4);
        bytes.insert(bytes.end(), entry.name.begin(), entry.name.end());
        if (extra != 0) {
            put(bytes, zip64_extra, 2);
            put(bytes, extra, 2);
            if (big) {
                put(bytes, entry.size, 8);
                put(bytes, entry.size, 8);
            }
            if (far) {
                put(bytes, entry.header, 8);
            }
        }
        if (bytes.size() >= chunk) {
            if (!write(bytes)) {
                return std::unexpected(unwritable());
            }
            bytes.clear();
        }
    }
    const std::uint64_t count = m_entries.size();
    const std::uint64_t size = m_offset + bytes.size() - directory;
    if (m_zip64 || count >= most_count || size >= wide || directory >= wide) {
        const std::uint64_t record = directory + size;
        put(bytes, zip64_end_signature, 4);
        put(bytes, zip64_end_rest, 8);
        put(bytes, zip64_version, 2);
        put(bytes, zip64_version, 2);
        put(bytes, 0, 4);
        put(bytes, 0, 4);
        put(bytes, count, 8);
        put(bytes, count, 8);
        put(bytes, size, 8);
        put(bytes, directory, 8);
        put(bytes, zip64_locator_signature, 4);
        put(bytes, 0, 4);
        put(bytes, record, 8);
        put(bytes, 1, 4);
    }
    // Each of the end record's that's too big for it is Zip64's sign.
    put(bytes, end_signature, 4);
    put(bytes, 0, 2);
    put(bytes, 0, 2);
    put(bytes, m_zip64 ? most_count : std::min(count, most_count), 2);
    put(bytes, m_zip64 ? most_count : std::min(count, most_count), 2);
    put(bytes, m_zip64 ? wide : std::min(size, wide), 4);
    put(bytes, m_zip64 ? wide : std::min(directory, wide), 4);
    put(bytes, 0, 2);
    if (!write(bytes)) {
        return std::unexpected(unwritable());
    }
    if (std::expected<void, std::string> committed = m_file.commit(); !committed) {
        m_failed = true;
        return std::unexpected(other(std::move(committed.error())));
    }
    m_finished = true;
    return {};
}

bool pak_writer_t::write(std::span<const std::uint8_t> bytes) {
    if (!m_file.write_at(m_offset, bytes)) {
        m_failed = true;
        return false;
    }
    m_offset += bytes.size();
    return true;
}

error_t pak_writer_t::unwritable() const {
    return other(std::format("couldn't write {}", m_to));
}

} // namespace fbe::tools
