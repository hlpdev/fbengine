// res:// paths and the mount table, which follow std.vfs's Vfs: a file is the last mount's that
// has it, a folder is what every mount has there, and a mount under a place makes it a folder.

#include "vfs/vfs.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <map>
#include <ranges>
#include <span>
#include <utility>

namespace fbe::vfs {

namespace {

constexpr std::string_view scheme = "res://";

std::atomic<fault_t> injected = fault_t::NONE;

// Whether `name` is one of Windows' devices, which a name is with any extension, or spaces before
// it: CON, PRN, AUX, NUL, CONIN$, CONOUT$, and COM and LPT with a digit, 0 to 9, or a
// superscript one, two, or three.
bool device(std::string_view name) {
    std::string base = fold(name.substr(0, name.find('.')));
    while (!base.empty() && base.back() == ' ') {
        base.pop_back();
    }
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "conin$" ||
        base == "conout$") {
        return true;
    }
    if (!base.starts_with("com") && !base.starts_with("lpt")) {
        return false;
    }
    const std::string_view digit = std::string_view(base).substr(3);
    // U+00B9, U+00B2, and U+00B3 in UTF-8.
    return (digit.size() == 1 && digit[0] >= '0' && digit[0] <= '9') || digit == "\xC2\xB9" ||
           digit == "\xC2\xB2" || digit == "\xC2\xB3";
}

} // namespace

// Whether `name`, a name in a path, can be a file's on every OS: none of Windows' characters
// that can't be in one, no control characters, no dot or space at its end, which Windows takes
// off, and none of Windows' devices.
bool nameable(std::string_view name) {
    const bool characters = std::ranges::none_of(name, [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < 0x20 || byte == 0x7F || std::string_view(R"(:*?"<>|)").contains(c);
    });
    return characters && !name.empty() && name.back() != '.' && name.back() != ' ' && !device(name);
}

namespace {

error_t invalid(std::string_view text, std::string_view why) {
    return { .failure = failure_t::INVALID_PATH, .message = std::format("'{}' {}", text, why) };
}

// Adds the names in `rest`, split at '/' and '\', to `path`.
result_t<path_t> add_names(path_t path, std::string_view rest, std::string_view text) {
    std::size_t start = 0;
    while (start <= rest.size()) {
        const std::size_t end = std::min(rest.find_first_of("/\\", start), rest.size());
        const std::string_view name = rest.substr(start, end - start);
        start = end + 1;
        if (name.empty() || name == ".") {
            continue;
        }
        if (name == "..") {
            if (path.names.empty()) {
                return std::unexpected(invalid(text, "goes above res://"));
            }
            path.names.pop_back();
            path.keys.pop_back();
            continue;
        }
        if (!nameable(name)) {
            return std::unexpected(
                invalid(text, std::format("has '{}', a name no file can have on every OS", name)));
        }
        path.names.emplace_back(name);
        path.keys.push_back(fold(name));
    }
    return path;
}

std::string joined(std::span<const std::string> names) {
    std::string text(scheme);
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            text += '/';
        }
        text += names[i];
    }
    return text;
}

bool starts(std::span<const std::string> keys, std::span<const std::string> prefix) {
    return prefix.size() <= keys.size() && std::ranges::equal(prefix, keys.first(prefix.size()));
}

} // namespace

std::string path_t::text() const {
    return joined(names);
}

std::string path_t::key() const {
    return joined(keys);
}

std::string fold(std::string_view name) {
    std::string folded(name);
    for (char& c : folded) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return folded;
}

result_t<path_t> parse(std::string_view text) {
    if (text.size() < scheme.size() || fold(text.substr(0, scheme.size())) != scheme) {
        return std::unexpected(invalid(text, "isn't a res:// path"));
    }
    return add_names(path_t(), text.substr(scheme.size()), text);
}

result_t<path_t> resolve(const path_t& base, std::string_view relative) {
    if (relative.size() >= scheme.size() && fold(relative.substr(0, scheme.size())) == scheme) {
        return parse(relative);
    }
    path_t folder = base;
    if (!folder.names.empty()) {
        folder.names.pop_back();
        folder.keys.pop_back();
    }
    return add_names(std::move(folder), relative, relative);
}

std::uint64_t vfs_t::mount(const path_t& at, std::shared_ptr<source_t> source) {
    const std::scoped_lock lock(m_mutex);
    const std::uint64_t id = m_next_id++;
    auto changed = std::make_shared<view_t::mounts_t>(*m_mounts);
    changed->push_back({ .at = at, .source = std::move(source), .id = id });
    m_mounts = std::move(changed);
    return id;
}

bool vfs_t::unmount(std::uint64_t id) {
    const std::scoped_lock lock(m_mutex);
    const view_t::mounts_t& mounts = *m_mounts;
    for (std::size_t i = mounts.size(); i-- > 0;) {
        if (mounts[i].id == id) {
            auto changed = std::make_shared<view_t::mounts_t>(*m_mounts);
            changed->erase(changed->begin() + static_cast<std::ptrdiff_t>(i));
            m_mounts = std::move(changed);
            return true;
        }
    }
    return false;
}

bool vfs_t::unmount(const path_t& at) {
    const std::scoped_lock lock(m_mutex);
    const view_t::mounts_t& mounts = *m_mounts;
    for (std::size_t i = mounts.size(); i-- > 0;) {
        if (mounts[i].at.keys == at.keys) {
            auto changed = std::make_shared<view_t::mounts_t>(*m_mounts);
            changed->erase(changed->begin() + static_cast<std::ptrdiff_t>(i));
            m_mounts = std::move(changed);
            return true;
        }
    }
    return false;
}

void inject(fault_t fault) {
    injected = fault;
}

bool faulted(fault_t fault) {
    fault_t expected = fault;
    return fault != fault_t::NONE && injected.compare_exchange_strong(expected, fault_t::NONE);
}

view_t vfs_t::view() const {
    const std::scoped_lock lock(m_mutex);
    return view_t(m_mounts);
}

error_t too_big(std::string_view shown, std::uint64_t size) {
    return {
        .failure = failure_t::OTHER,
        .message = std::format("{} is {} bytes, more than can be read at once", shown, size),
    };
}

result_t<bytes_t> view_t::read(const path_t& path, std::uint64_t most) const {
    const error_t folder{
        .failure = failure_t::IS_A_FOLDER,
        .message = std::format("{} is a folder", path.text()),
    };
    // res:// is a folder with nothing mounted too, as kind has it.
    if (path.keys.empty()) {
        return std::unexpected(folder);
    }
    // As kind finds it: the newest mount that has something there, or a mount at or under it,
    // which makes it a folder.
    for (const mount_t& mount : std::views::reverse(*m_mounts)) {
        if (starts(mount.at.keys, path.keys)) {
            return std::unexpected(folder);
        }
        if (!starts(path.keys, mount.at.keys)) {
            continue;
        }
        const std::size_t depth = mount.at.keys.size();
        const std::span<const std::string> names = std::span(path.names).subspan(depth);
        const std::span<const std::string> keys = std::span(path.keys).subspan(depth);
        result_t<std::optional<bytes_t>> bytes = mount.source->read(names, keys, most);
        if (!bytes) {
            return std::unexpected(std::move(bytes.error()));
        }
        if (*bytes) {
            return std::move(**bytes);
        }
        const result_t<kind_t> there = mount.source->kind(names, keys);
        if (!there) {
            return std::unexpected(there.error());
        }
        if (*there == kind_t::FOLDER) {
            return std::unexpected(folder);
        }
    }
    return std::unexpected(error_t{
        .failure = failure_t::NOT_FOUND,
        .message = std::format("no mount has {}", path.text()),
    });
}

result_t<kind_t> view_t::kind(const path_t& path) const {
    if (path.keys.empty()) {
        return kind_t::FOLDER;
    }
    for (const mount_t& mount : std::views::reverse(*m_mounts)) {
        // A mount in it makes it a folder, and so does one at it.
        if (starts(mount.at.keys, path.keys)) {
            return kind_t::FOLDER;
        }
        if (!starts(path.keys, mount.at.keys)) {
            continue;
        }
        const std::size_t depth = mount.at.keys.size();
        result_t<kind_t> kind = mount.source->kind(std::span(path.names).subspan(depth),
                                                   std::span(path.keys).subspan(depth));
        if (!kind || *kind != kind_t::NONE) {
            return kind;
        }
    }
    return kind_t::NONE;
}

std::optional<stamp_t> view_t::stamp(const path_t& path) const {
    if (path.keys.empty()) {
        return std::nullopt;
    }
    // As read finds it: the newest mount that has a file there.
    for (const mount_t& mount : std::views::reverse(*m_mounts)) {
        if (starts(mount.at.keys, path.keys)) {
            return std::nullopt;
        }
        if (!starts(path.keys, mount.at.keys)) {
            continue;
        }
        const std::size_t depth = mount.at.keys.size();
        const std::span<const std::string> names = std::span(path.names).subspan(depth);
        const std::span<const std::string> keys = std::span(path.keys).subspan(depth);
        const result_t<kind_t> kind = mount.source->kind(names, keys);
        if (!kind) {
            return std::nullopt;
        }
        if (*kind == kind_t::FILE) {
            return mount.source->stamp(names, keys);
        }
        if (*kind != kind_t::NONE) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

result_t<std::vector<entry_t>> view_t::list(const path_t& path) const {
    // A later mount's file where an earlier one has a folder is the file, as kind says.
    const result_t<kind_t> there = kind(path);
    if (!there) {
        return std::unexpected(there.error());
    }
    if (*there == kind_t::FILE) {
        return std::unexpected(error_t{
            .failure = failure_t::NOT_A_FOLDER,
            .message = std::format("{} is a file", path.text()),
        });
    }

    bool found = path.keys.empty();
    std::map<std::string, entry_t> listed;
    for (const mount_t& mount : std::views::reverse(*m_mounts)) {
        // A mount below the folder is a folder in it.
        const std::size_t depth = mount.at.keys.size();
        if (depth > path.keys.size() && starts(mount.at.keys, path.keys)) {
            found = true;
            listed.try_emplace(
                mount.at.keys[path.keys.size()],
                entry_t{ .name = mount.at.names[path.keys.size()], .kind = kind_t::FOLDER });
            continue;
        }
        if (!starts(path.keys, mount.at.keys)) {
            continue;
        }
        if (depth == path.keys.size()) {
            found = true;
        }
        result_t<std::optional<std::vector<entry_t>>> items = mount.source->list(
            std::span(path.names).subspan(depth), std::span(path.keys).subspan(depth));
        if (!items) {
            return std::unexpected(std::move(items.error()));
        }
        if (!*items) {
            continue;
        }
        found = true;
        // Of a mount's names that fold the same, the first by its bytes, as a folder finds it.
        std::ranges::sort(**items, {}, &entry_t::name);
        for (const entry_t& item : **items) {
            listed.try_emplace(fold(item.name), item);
        }
    }
    if (!found) {
        return std::unexpected(error_t{
            .failure = failure_t::NOT_FOUND,
            .message = std::format("no mount has {}", path.text()),
        });
    }
    std::vector<entry_t> entries;
    entries.reserve(listed.size());
    for (auto& [key, entry] : listed) {
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace fbe::vfs
