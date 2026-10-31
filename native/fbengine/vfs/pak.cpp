// A pak file as a source: a zip whose files are stored, with Zip64 where sizes need it, as
// std.compress's ZipWriter makes one. Its central directory is read once, into an index by folded
// paths; a read finds the file's local header, past which its bytes are. Reads of a pak on the
// disk take turns at its file, since copying a stored file is quick beside decoding it.

#include "vfs/disk.hpp"
#include "vfs/vfs.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <ios>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

namespace fbe::vfs {

namespace {

constexpr std::uint32_t end_signature = 0x0605'4B50;
constexpr std::uint32_t zip64_end_signature = 0x0606'4B50;
constexpr std::uint32_t zip64_locator_signature = 0x0706'4B50;
constexpr std::uint32_t central_signature = 0x0201'4B50;
constexpr std::uint32_t local_signature = 0x0403'4B50;
constexpr std::size_t end_size = 22;
constexpr std::size_t zip64_end_size = 56;
constexpr std::size_t locator_size = 20;
constexpr std::size_t central_size = 46;
constexpr std::size_t local_size = 30;
// The end record's comment is at most this long.
constexpr std::size_t most_comment = 0xFFFF;
constexpr std::uint32_t wide = 0xFFFF'FFFF;
constexpr std::uint16_t zip64_extra = 0x0001;
constexpr std::uint16_t encrypted = 0x0001;
// Unix's, the host whose external attributes have its file modes in their top half.
constexpr unsigned unix_host = 3;
constexpr std::uint32_t mode_type = 0xF000;
constexpr std::uint32_t mode_link = 0xA000;

// Little-endian numbers in bytes, which the caller has checked are there.
std::uint64_t number(std::span<const std::uint8_t> bytes, std::size_t at, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t i = size; i-- > 0;) {
        value = (value << 8U) | bytes[at + i];
    }
    return value;
}

std::uint16_t u16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(number(bytes, at, 2));
}

std::uint32_t u32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(number(bytes, at, 4));
}

std::uint64_t u64(std::span<const std::uint8_t> bytes, std::size_t at) {
    return number(bytes, at, 8);
}

// Where a pak's bytes are: a file on the disk or memory.
class reader_t {
public:
    reader_t() = default;
    virtual ~reader_t() = default;
    reader_t(const reader_t&) = delete;
    reader_t& operator=(const reader_t&) = delete;
    reader_t(reader_t&&) = delete;
    reader_t& operator=(reader_t&&) = delete;

    /// Reads `into.size()` bytes from `offset`, which the caller has checked are in it.
    virtual bool read(std::uint64_t offset, std::span<std::uint8_t> into) = 0;
    [[nodiscard]] virtual std::uint64_t size() const = 0;
    /// What it's called in errors.
    [[nodiscard]] virtual const std::string& name() const = 0;
};

class file_reader_t final : public reader_t {
public:
    file_reader_t(std::ifstream stream, std::uint64_t size, std::string name)
        : m_stream(std::move(stream)), m_size(size), m_name(std::move(name)) {}

    bool read(std::uint64_t offset, std::span<std::uint8_t> into) override {
        const std::scoped_lock lock(m_mutex);
        m_stream.clear();
        m_stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        m_stream.read(reinterpret_cast<char*>(into.data()),
                      static_cast<std::streamsize>(into.size()));
        return std::cmp_equal(m_stream.gcount(), into.size());
    }

    [[nodiscard]] std::uint64_t size() const override {
        return m_size;
    }

    [[nodiscard]] const std::string& name() const override {
        return m_name;
    }

private:
    std::mutex m_mutex;
    std::ifstream m_stream;
    std::uint64_t m_size;
    std::string m_name;
};

class memory_reader_t final : public reader_t {
public:
    explicit memory_reader_t(bytes_t bytes) : m_bytes(std::move(bytes)) {}

    bool read(std::uint64_t offset, std::span<std::uint8_t> into) override {
        std::ranges::copy(std::span(m_bytes).subspan(static_cast<std::size_t>(offset), into.size()),
                          into.begin());
        return true;
    }

    [[nodiscard]] std::uint64_t size() const override {
        return m_bytes.size();
    }

    [[nodiscard]] const std::string& name() const override {
        return m_name;
    }

private:
    bytes_t m_bytes;
    std::string m_name = "the pak in memory";
};

class pak_source_t final : public source_t {
public:
    explicit pak_source_t(std::unique_ptr<reader_t> reader) : m_reader(std::move(reader)) {
        m_folders.try_emplace("");
    }

    /// Reads the central directory into the index.
    result_t<void> open() {
        const std::uint64_t size = m_reader->size();
        const std::size_t tail_size =
            static_cast<std::size_t>(std::min<std::uint64_t>(size, end_size + most_comment));
        bytes_t tail(tail_size);
        if (!m_reader->read(size - tail_size, tail)) {
            return unreadable();
        }
        std::optional<std::size_t> end;
        for (std::size_t at = tail_size >= end_size ? tail_size - end_size + 1 : 0; at-- > 0;) {
            if (u32(tail, at) == end_signature && at + end_size + u16(tail, at + 20) <= tail_size) {
                end = at;
                break;
            }
        }
        if (!end) {
            return invalid("it has no zip's end record");
        }
        const std::uint64_t end_offset = size - tail_size + *end;
        if (u16(tail, *end + 4) != 0 || u16(tail, *end + 6) != 0) {
            return invalid("it's a zip of several parts");
        }
        std::uint64_t count = u16(tail, *end + 10);
        std::uint64_t directory_size = u32(tail, *end + 12);
        std::uint64_t directory = u32(tail, *end + 16);

        if (count == 0xFFFF || directory_size == wide || directory == wide) {
            if (end_offset < locator_size) {
                return invalid("its Zip64 end record is missing");
            }
            bytes_t locator(locator_size);
            if (!m_reader->read(end_offset - locator_size, locator)) {
                return unreadable();
            }
            const std::uint64_t record = u64(locator, 8);
            if (u32(locator, 0) != zip64_locator_signature || record > size - zip64_end_size) {
                return invalid("its Zip64 end record is missing");
            }
            bytes_t zip64(zip64_end_size);
            if (!m_reader->read(record, zip64)) {
                return unreadable();
            }
            if (u32(zip64, 0) != zip64_end_signature) {
                return invalid("its Zip64 end record is missing");
            }
            count = u64(zip64, 32);
            directory_size = u64(zip64, 40);
            directory = u64(zip64, 48);
        }
        if (directory > size || directory_size > size - directory) {
            return invalid("its central directory is past its end");
        }

        bytes_t entries(static_cast<std::size_t>(directory_size));
        if (!m_reader->read(directory, entries)) {
            return unreadable();
        }
        std::size_t at = 0;
        for (std::uint64_t i = 0; i < count; ++i) {
            result_t<std::size_t> next = add(entries, at);
            if (!next) {
                return std::unexpected(std::move(next.error()));
            }
            at = *next;
        }
        return {};
    }

    result_t<std::optional<bytes_t>> read(std::span<const std::string> /*names*/,
                                          std::span<const std::string> keys) override {
        const auto found = m_files.find(joined(keys));
        if (found == m_files.end()) {
            return std::optional<bytes_t>{};
        }
        const file_t& file = found->second;
        const std::uint64_t size = m_reader->size();
        bytes_t header(local_size);
        if (file.header > size || size - file.header < local_size) {
            return invalid(std::format("{}'s header is past its end", file.name));
        }
        if (!m_reader->read(file.header, header)) {
            return unreadable();
        }
        if (u32(header, 0) != local_signature) {
            return invalid(std::format("{}'s header is missing", file.name));
        }
        const std::uint64_t data = file.header + local_size + u16(header, 26) + u16(header, 28);
        if (data > size || file.size > size - data) {
            return invalid(std::format("{} is past its end", file.name));
        }
        bytes_t bytes(static_cast<std::size_t>(file.size));
        if (!m_reader->read(data, bytes)) {
            return unreadable();
        }
        return std::optional<bytes_t>(std::move(bytes));
    }

    result_t<kind_t> kind(std::span<const std::string> /*names*/,
                          std::span<const std::string> keys) override {
        const std::string key = joined(keys);
        if (m_files.contains(key)) {
            return kind_t::FILE;
        }
        return m_folders.contains(key) ? kind_t::FOLDER : kind_t::NONE;
    }

    result_t<std::optional<std::vector<entry_t>>> list(std::span<const std::string> /*names*/,
                                                       std::span<const std::string> keys) override {
        const auto found = m_folders.find(joined(keys));
        if (found == m_folders.end()) {
            return std::optional<std::vector<entry_t>>{};
        }
        std::vector<entry_t> entries;
        entries.reserve(found->second.size());
        for (const auto& [key, entry] : found->second) {
            entries.push_back(entry);
        }
        return std::optional<std::vector<entry_t>>(std::move(entries));
    }

private:
    struct file_t {
        // Its path as the zip has it, for errors.
        std::string name;
        std::uint64_t header = 0;
        std::uint64_t size = 0;
    };

    static std::string joined(std::span<const std::string> keys) {
        std::string key;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (i > 0) {
                key += '/';
            }
            key += keys[i];
        }
        return key;
    }

    std::unexpected<error_t> invalid(std::string_view why) const {
        return std::unexpected(error_t{
            .failure = failure_t::INVALID_DATA,
            .message = std::format("{} isn't a pak: {}", m_reader->name(), why),
        });
    }

    std::unexpected<error_t> unreadable() const {
        return std::unexpected(error_t{
            .failure = failure_t::OTHER,
            .message = std::format("couldn't read {}", m_reader->name()),
        });
    }

    // Adds the central directory's entry at `at` to the index, and gives where the next is.
    result_t<std::size_t> add(std::span<const std::uint8_t> entries, std::size_t at) {
        if (entries.size() - at < central_size || u32(entries, at) != central_signature) {
            return invalid("its central directory is cut short");
        }
        const unsigned host = u16(entries, at + 4) >> 8U;
        const std::uint16_t flags = u16(entries, at + 8);
        const std::uint16_t method = u16(entries, at + 10);
        std::uint64_t packed = u32(entries, at + 20);
        std::uint64_t size = u32(entries, at + 24);
        const std::size_t name_size = u16(entries, at + 28);
        const std::size_t extra_size = u16(entries, at + 30);
        const std::size_t comment_size = u16(entries, at + 32);
        const std::uint32_t attributes = u32(entries, at + 38);
        std::uint64_t header = u32(entries, at + 42);
        const std::size_t next = at + central_size + name_size + extra_size + comment_size;
        if (next > entries.size()) {
            return invalid("its central directory is cut short");
        }
        const std::string name(reinterpret_cast<const char*>(&entries[at + central_size]),
                               name_size);

        // Zip64's sizes and offset, for those too big for the entry's own.
        const std::span<const std::uint8_t> extra =
            entries.subspan(at + central_size + name_size, extra_size);
        for (std::size_t field = 0; field + 4 <= extra.size();) {
            const std::uint16_t id = u16(extra, field);
            const std::size_t length = u16(extra, field + 2);
            if (field + 4 + length > extra.size()) {
                break;
            }
            if (id == zip64_extra) {
                std::size_t value = field + 4;
                for (std::uint64_t* wide_value : { &size, &packed, &header }) {
                    if (*wide_value == wide) {
                        if (value + 8 > field + 4 + length) {
                            return invalid(std::format("{}'s Zip64 sizes are cut short", name));
                        }
                        *wide_value = u64(extra, value);
                        value += 8;
                    }
                }
            }
            field += 4 + length;
        }

        if (host == unix_host && ((attributes >> 16U) & mode_type) == mode_link) {
            return next;
        }
        const bool folder = !name.empty() && (name.back() == '/' || name.back() == '\\');
        // An entry above its root, or with a name no res:// path can have, isn't in it.
        const result_t<path_t> path = parse(std::format("res://{}", name));
        if (!path || path->keys.empty()) {
            return next;
        }
        if ((flags & encrypted) != 0) {
            return invalid(std::format("{} is encrypted", name));
        }
        if (!folder && (method != 0 || packed != size)) {
            return invalid(std::format("{} is compressed, and a pak's files are stored", name));
        }

        // Its folders, each in the one it's in.
        std::string key;
        const std::size_t folders = folder ? path->keys.size() : path->keys.size() - 1;
        for (std::size_t i = 0; i < folders; ++i) {
            std::string inside = key.empty() ? path->keys[i] : key + "/" + path->keys[i];
            if (m_files.contains(inside)) {
                return both(name);
            }
            m_folders[key].insert_or_assign(
                path->keys[i], entry_t{ .name = path->names[i], .kind = kind_t::FOLDER });
            m_folders.try_emplace(inside);
            key = std::move(inside);
        }
        if (folder) {
            return next;
        }
        std::string file = key.empty() ? path->keys.back() : key + "/" + path->keys.back();
        if (m_folders.contains(file)) {
            return both(name);
        }
        m_folders[key].insert_or_assign(
            path->keys.back(), entry_t{ .name = path->names.back(), .kind = kind_t::FILE });
        m_files.insert_or_assign(std::move(file),
                                 file_t{ .name = name, .header = header, .size = size });
        return next;
    }

    std::unexpected<error_t> both(std::string_view name) const {
        return invalid(std::format("{} makes a place a file and a folder too", name));
    }

    std::unique_ptr<reader_t> m_reader;
    // The files by their folded paths, and each folder's names, by folded name; "" is the root.
    std::unordered_map<std::string, file_t> m_files;
    std::unordered_map<std::string, std::map<std::string, entry_t>> m_folders;
};

result_t<std::shared_ptr<source_t>> opened(std::unique_ptr<reader_t> reader) {
    auto source = std::make_shared<pak_source_t>(std::move(reader));
    result_t<void> index = source->open();
    if (!index) {
        return std::unexpected(std::move(index.error()));
    }
    return source;
}

} // namespace

result_t<std::shared_ptr<source_t>> pak(const std::filesystem::path& file) {
    std::string name = utf8(file);
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(file, error);
    std::ifstream stream(file, std::ios::binary);
    if (error || !stream) {
        const bool missing = !std::filesystem::exists(file, error);
        return std::unexpected(error_t{
            .failure = missing ? failure_t::NOT_FOUND : failure_t::OTHER,
            .message =
                std::format("couldn't open the pak {}{}", name, missing ? ": it isn't there" : ""),
        });
    }
    return opened(std::make_unique<file_reader_t>(std::move(stream), size, std::move(name)));
}

result_t<std::shared_ptr<source_t>> pak(bytes_t bytes) {
    return opened(std::make_unique<memory_reader_t>(std::move(bytes)));
}

} // namespace fbe::vfs
