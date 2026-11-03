// A pak file as a source: a zip whose files are stored, with Zip64 where sizes need it, as
// std.compress's ZipWriter makes one. Its central directory is read once, a chunk at a time, into
// a tree of its folders by folded names; a read finds the file's local header, past which its
// bytes are. Nothing the archive says is allocated before it's checked against the pak's size,
// and a pak on the disk is read at offsets, by every thread at once.

#include "vfs/disk.hpp"
#include "vfs/vfs.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
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
// The end record's comment is at most this long, and a central entry's name, extra field, and
// comment each.
constexpr std::size_t most_field = 0xFFFF;
constexpr std::size_t most_entry = central_size + (3 * most_field);
constexpr std::uint32_t wide = 0xFFFF'FFFF;
constexpr std::uint16_t zip64_extra = 0x0001;
constexpr std::uint16_t encrypted = 0x0001;
// Unix's, the host whose external attributes have its file modes in their top half.
constexpr unsigned unix_host = 3;
constexpr std::uint32_t mode_type = 0xF000;
constexpr std::uint32_t mode_link = 0xA000;
// No OS has a path longer than this, or of more names.
constexpr std::size_t most_path = 4096;
constexpr std::size_t most_names = 255;
// How much of the central directory is read at once.
constexpr std::size_t chunk = std::size_t{ 1 } << 20U;

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

// A name as errors show it: its start, for one that's long.
std::string shown(std::string_view name) {
    constexpr std::size_t most = 80;
    return name.size() <= most ? std::string(name) : std::format("{}...", name.substr(0, most));
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

    /// Reads `into.size()` bytes from `offset`, which the caller has checked are in it, from any
    /// thread.
    [[nodiscard]] virtual bool read(std::uint64_t offset, std::span<std::uint8_t> into) const = 0;
    [[nodiscard]] virtual std::uint64_t size() const = 0;
    /// What it's called in errors.
    [[nodiscard]] virtual const std::string& name() const = 0;
};

class file_reader_t final : public reader_t {
public:
    file_reader_t(std::unique_ptr<read_file_t> file, std::string name)
        : m_file(std::move(file)), m_name(std::move(name)) {}

    [[nodiscard]] bool read(std::uint64_t offset, std::span<std::uint8_t> into) const override {
        return m_file->read_at(offset, into);
    }

    [[nodiscard]] std::uint64_t size() const override {
        return m_file->size();
    }

    [[nodiscard]] const std::string& name() const override {
        return m_name;
    }

private:
    std::unique_ptr<read_file_t> m_file;
    std::string m_name;
};

class memory_reader_t final : public reader_t {
public:
    explicit memory_reader_t(bytes_t bytes) : m_bytes(std::move(bytes)) {}

    [[nodiscard]] bool read(std::uint64_t offset, std::span<std::uint8_t> into) const override {
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

// A reader's bytes from one place to another, read in order a chunk at a time.
class cursor_t {
public:
    cursor_t(const reader_t& reader, std::uint64_t from, std::uint64_t to)
        : m_reader(reader), m_at(from), m_end(to), m_window_at(from) {}

    // The next `count` bytes, good until the next take; none if they aren't all there.
    std::optional<std::span<const std::uint8_t>> take(std::size_t count) {
        if (count > m_end - m_at) {
            return std::nullopt;
        }
        if (m_at + count > m_window_at + m_window.size()) {
            m_window.resize(static_cast<std::size_t>(
                std::min<std::uint64_t>(std::max(count, chunk), m_end - m_at)));
            if (!m_reader.read(m_at, m_window)) {
                return std::nullopt;
            }
            m_window_at = m_at;
        }
        const auto taken = std::span<const std::uint8_t>(m_window).subspan(
            static_cast<std::size_t>(m_at - m_window_at), count);
        m_at += count;
        return taken;
    }

private:
    const reader_t& m_reader;
    std::uint64_t m_at;
    std::uint64_t m_end;
    std::uint64_t m_window_at;
    bytes_t m_window;
};

// Where the central directory is, and how many entries it has.
struct directory_t {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t count = 0;
};

class pak_source_t final : public source_t {
public:
    explicit pak_source_t(std::unique_ptr<reader_t> reader) : m_reader(std::move(reader)) {
        m_nodes.emplace_back();
    }

    // Reads the central directory into the index.
    result_t<void> open() {
        const std::uint64_t size = m_reader->size();
        const auto tail_size =
            static_cast<std::size_t>(std::min<std::uint64_t>(size, end_size + most_field));
        bytes_t tail(tail_size);
        if (!m_reader->read(size - tail_size, tail)) {
            return unreadable();
        }
        // An end record's signature can be in a comment too, so the one taken is the first, from
        // the end, whose comment ends the file and whose central directory ends where it starts,
        // or failing that, the first whose directory does.
        std::optional<directory_t> directory;
        for (const bool exact : { true, false }) {
            for (std::size_t at = tail_size >= end_size ? tail_size - end_size + 1 : 0;
                 !directory && at-- > 0;) {
                if (u32(tail, at) != end_signature) {
                    continue;
                }
                const std::size_t ends = at + end_size + u16(tail, at + 20);
                if (ends > tail_size || (exact && ends != tail_size)) {
                    continue;
                }
                directory = located(tail, at, size - tail_size + at);
            }
        }
        if (!directory) {
            return invalid("it has no zip's end record, with a central directory before it");
        }

        cursor_t entries(*m_reader, directory->offset, directory->offset + directory->size);
        for (std::uint64_t i = 0; i < directory->count; ++i) {
            if (result_t<void> added = add(entries); !added) {
                return std::unexpected(std::move(added.error()));
            }
        }
        return {};
    }

    result_t<std::optional<bytes_t>> read(std::span<const std::string> /*names*/,
                                          std::span<const std::string> keys,
                                          std::uint64_t most) override {
        const std::optional<std::uint32_t> found = find(keys);
        if (!found || m_nodes[*found].folder) {
            return std::optional<bytes_t>{};
        }
        const node_t& file = m_nodes[*found];
        const std::uint64_t size = m_reader->size();
        if (file.header > size || size - file.header < local_size) {
            return invalid(std::format("{}'s header is past its end", shown(file.path)));
        }
        bytes_t header(local_size);
        if (!m_reader->read(file.header, header)) {
            return unreadable();
        }
        if (u32(header, 0) != local_signature) {
            return invalid(std::format("{}'s header is missing", shown(file.path)));
        }
        const std::uint64_t data = file.header + local_size + u16(header, 26) + u16(header, 28);
        if (data > size || file.size > size - data) {
            return invalid(std::format("{} is past its end", shown(file.path)));
        }
        if (file.size > most) {
            return std::unexpected(too_big(shown(file.path), file.size));
        }
        try {
            if (faulted(fault_t::OUT_OF_MEMORY)) {
                throw std::bad_alloc();
            }
            bytes_t bytes(static_cast<std::size_t>(file.size));
            if (!m_reader->read(data, bytes)) {
                return unreadable();
            }
            return std::optional<bytes_t>(std::move(bytes));
        } catch (const std::bad_alloc&) {
            return std::unexpected(error_t{
                .failure = failure_t::OTHER,
                .message = std::format("there isn't the memory to read {} from {}",
                                       shown(file.path), m_reader->name()),
            });
        }
    }

    result_t<kind_t> kind(std::span<const std::string> /*names*/,
                          std::span<const std::string> keys) override {
        const std::optional<std::uint32_t> found = find(keys);
        if (!found) {
            return kind_t::NONE;
        }
        return m_nodes[*found].folder ? kind_t::FOLDER : kind_t::FILE;
    }

    result_t<std::optional<std::vector<entry_t>>> list(std::span<const std::string> /*names*/,
                                                       std::span<const std::string> keys) override {
        const std::optional<std::uint32_t> found = find(keys);
        if (!found || !m_nodes[*found].folder) {
            return std::optional<std::vector<entry_t>>{};
        }
        std::vector<entry_t> entries;
        entries.reserve(m_nodes[*found].children.size());
        for (const auto& [key, child] : m_nodes[*found].children) {
            const node_t& node = m_nodes[child];
            entries.push_back(
                { .name = node.name, .kind = node.folder ? kind_t::FOLDER : kind_t::FILE });
        }
        return std::optional<std::vector<entry_t>>(std::move(entries));
    }

private:
    // A file or a folder of the index: a file's place in the pak, or a folder's names. Its moves,
    // as the index grows, are its string's and map's, which don't throw.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    struct node_t {
        // As the last entry with it has it.
        std::string name;
        bool folder = true;
        std::uint64_t header = 0;
        std::uint64_t size = 0;
        // A file's entry's name, for errors.
        std::string path;
        // The nodes in a folder, by folded name.
        std::map<std::string, std::uint32_t> children;
    };

    [[nodiscard]] std::unexpected<error_t> invalid(std::string_view why) const {
        return std::unexpected(error_t{
            .failure = failure_t::INVALID_DATA,
            .message = std::format("{} isn't a pak: {}", m_reader->name(), why),
        });
    }

    [[nodiscard]] std::unexpected<error_t> unreadable() const {
        return std::unexpected(error_t{
            .failure = failure_t::OTHER,
            .message = std::format("couldn't read {}", m_reader->name()),
        });
    }

    [[nodiscard]] std::unexpected<error_t> both(std::string_view name) const {
        return invalid(std::format("{} makes a place a file and a folder too", shown(name)));
    }

    // The central directory an end record at `at` in `tail`, and `offset` in the pak, says there
    // is, if it ends where the end records start, and has room for its entries.
    [[nodiscard]] std::optional<directory_t> located(std::span<const std::uint8_t> tail,
                                                     std::size_t at, std::uint64_t offset) const {
        if (u16(tail, at + 4) != 0 || u16(tail, at + 6) != 0) {
            return std::nullopt;
        }
        directory_t directory{
            .offset = u32(tail, at + 16),
            .size = u32(tail, at + 12),
            .count = u16(tail, at + 10),
        };
        std::uint64_t ends = offset;
        if (directory.count == 0xFFFF || directory.size == wide || directory.offset == wide) {
            // The Zip64 locator just before it, and the Zip64 end record before that.
            if (offset < locator_size) {
                return std::nullopt;
            }
            bytes_t locator(locator_size);
            if (!m_reader->read(offset - locator_size, locator) ||
                u32(locator, 0) != zip64_locator_signature) {
                return std::nullopt;
            }
            const std::uint64_t record = u64(locator, 8);
            if (record > offset - locator_size || offset - locator_size - record < zip64_end_size) {
                return std::nullopt;
            }
            bytes_t zip64(zip64_end_size);
            if (!m_reader->read(record, zip64) || u32(zip64, 0) != zip64_end_signature) {
                return std::nullopt;
            }
            directory = {
                .offset = u64(zip64, 48),
                .size = u64(zip64, 40),
                .count = u64(zip64, 32),
            };
            ends = record;
        }
        // Each entry has its fixed part, and three fields of 65535 bytes at most.
        if (directory.offset > ends || directory.size != ends - directory.offset ||
            directory.count > directory.size / central_size ||
            directory.size / most_entry > directory.count) {
            return std::nullopt;
        }
        return directory;
    }

    // Adds the central directory's next entry to the index.
    result_t<void> add(cursor_t& entries) {
        const std::optional<std::span<const std::uint8_t>> fixed = entries.take(central_size);
        if (!fixed || u32(*fixed, 0) != central_signature) {
            return invalid("its central directory is cut short");
        }
        const unsigned host = u16(*fixed, 4) >> 8U;
        const std::uint16_t flags = u16(*fixed, 8);
        const std::uint16_t method = u16(*fixed, 10);
        std::uint64_t packed = u32(*fixed, 20);
        std::uint64_t size = u32(*fixed, 24);
        const std::size_t name_size = u16(*fixed, 28);
        const std::size_t extra_size = u16(*fixed, 30);
        const std::size_t comment_size = u16(*fixed, 32);
        const std::uint32_t attributes = u32(*fixed, 38);
        std::uint64_t header = u32(*fixed, 42);
        const std::optional<std::span<const std::uint8_t>> rest =
            entries.take(name_size + extra_size + comment_size);
        if (!rest) {
            return invalid("its central directory is cut short");
        }
        const std::string name(reinterpret_cast<const char*>(rest->data()), name_size);

        // Zip64's sizes and offset, for those too big for the entry's own.
        const std::span<const std::uint8_t> extra = rest->subspan(name_size, extra_size);
        for (std::size_t field = 0; field + 4 <= extra.size();) {
            const std::uint16_t id = u16(extra, field);
            const std::size_t length = u16(extra, field + 2);
            if (field + 4 + length > extra.size()) {
                break;
            }
            if (id == zip64_extra) {
                std::size_t value = field + 4;
                for (std::uint64_t* big : { &size, &packed, &header }) {
                    if (*big == wide) {
                        if (value + 8 > field + 4 + length) {
                            return invalid(
                                std::format("{}'s Zip64 sizes are cut short", shown(name)));
                        }
                        *big = u64(extra, value);
                        value += 8;
                    }
                }
            }
            field += 4 + length;
        }

        if (host == unix_host && ((attributes >> 16U) & mode_type) == mode_link) {
            return {};
        }
        if (name.size() > most_path) {
            return invalid(std::format("{} is longer than a path can be", shown(name)));
        }
        const bool folder = !name.empty() && (name.back() == '/' || name.back() == '\\');
        // An entry above its root, or with a name no res:// path can have, isn't in it.
        const result_t<path_t> path = parse(std::format("res://{}", name));
        if (!path || path->keys.empty()) {
            return {};
        }
        if (path->keys.size() > most_names) {
            return invalid(std::format("{} is deeper than a path can be", shown(name)));
        }
        if ((flags & encrypted) != 0) {
            return invalid(std::format("{} is encrypted", shown(name)));
        }
        if (!folder) {
            if (method != 0 || packed != size) {
                return invalid(
                    std::format("{} is compressed, and a pak's files are stored", shown(name)));
            }
            if (header >= m_reader->size() || size > m_reader->size() - header) {
                return invalid(std::format("{} is past its end", shown(name)));
            }
        }

        // Its folders, each in the one it's in, by folded name.
        std::uint32_t at = 0;
        const std::size_t folders = folder ? path->keys.size() : path->keys.size() - 1;
        for (std::size_t i = 0; i < folders; ++i) {
            const auto found = m_nodes[at].children.find(path->keys[i]);
            if (found == m_nodes[at].children.end()) {
                const auto made = static_cast<std::uint32_t>(m_nodes.size());
                m_nodes.push_back({ .name = path->names[i] });
                m_nodes[at].children.emplace(path->keys[i], made);
                at = made;
                continue;
            }
            const std::uint32_t next = found->second;
            if (!m_nodes[next].folder) {
                return both(name);
            }
            m_nodes[next].name = path->names[i];
            at = next;
        }
        if (folder) {
            return {};
        }
        const auto found = m_nodes[at].children.find(path->keys.back());
        node_t file{
            .name = path->names.back(),
            .folder = false,
            .header = header,
            .size = size,
            .path = name,
        };
        if (found == m_nodes[at].children.end()) {
            const auto made = static_cast<std::uint32_t>(m_nodes.size());
            m_nodes.push_back(std::move(file));
            m_nodes[at].children.emplace(path->keys.back(), made);
        } else if (m_nodes[found->second].folder) {
            return both(name);
        } else {
            // Of two entries of one path, the last is the file.
            m_nodes[found->second] = std::move(file);
        }
        return {};
    }

    // The node at `keys`, if there is one.
    [[nodiscard]] std::optional<std::uint32_t> find(std::span<const std::string> keys) const {
        std::uint32_t at = 0;
        for (const std::string& key : keys) {
            if (!m_nodes[at].folder) {
                return std::nullopt;
            }
            const auto found = m_nodes[at].children.find(key);
            if (found == m_nodes[at].children.end()) {
                return std::nullopt;
            }
            at = found->second;
        }
        return at;
    }

    std::unique_ptr<reader_t> m_reader;
    // The root first.
    std::vector<node_t> m_nodes;
};

result_t<std::shared_ptr<source_t>> opened(std::unique_ptr<reader_t> reader) {
    const std::string name = reader->name();
    try {
        auto source = std::make_shared<pak_source_t>(std::move(reader));
        result_t<void> index = source->open();
        if (!index) {
            return std::unexpected(std::move(index.error()));
        }
        return source;
    } catch (const std::bad_alloc&) {
        return std::unexpected(error_t{
            .failure = failure_t::OTHER,
            .message = std::format("there isn't the memory to read the index of {}", name),
        });
    }
}

} // namespace

result_t<std::shared_ptr<source_t>> pak(const std::filesystem::path& file) {
    std::string name = utf8(file);
    bool missing = false;
    std::unique_ptr<read_file_t> opened_file = read_file_t::open(file, missing);
    if (opened_file == nullptr) {
        return std::unexpected(error_t{
            .failure = missing ? failure_t::NOT_FOUND : failure_t::OTHER,
            .message =
                std::format("couldn't open the pak {}{}", name, missing ? ": it isn't there" : ""),
        });
    }
    return opened(std::make_unique<file_reader_t>(std::move(opened_file), std::move(name)));
}

result_t<std::shared_ptr<source_t>> pak(bytes_t bytes) {
    return opened(std::make_unique<memory_reader_t>(std::move(bytes)));
}

} // namespace fbe::vfs
