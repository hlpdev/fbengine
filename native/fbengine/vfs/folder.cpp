// A folder on the disk as a source, which finds a path as a pak does, whatever the disk does with
// case: at each name, every name in the folders so far that folds to it, by their real names on
// the disk, first by bytes, so "Data" and "data" are one folder, and neither spelling in a path,
// nor Windows' own names for a file (a short 8.3 name, a dot at the end, a device), finds what a
// pak of the same files wouldn't, nor do hidden names, starting with '.', which the cooker leaves
// out of a pak. Each folder's listing is kept, and listed again only once the
// folder has changed, so a read takes a stat of each folder on its path.

#include "vfs/disk.hpp"
#include "vfs/vfs.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace fbe::vfs {

namespace {

namespace fs = std::filesystem;

class folder_source_t final : public source_t {
public:
    explicit folder_source_t(fs::path folder) : m_folder(std::move(folder)) {}

    result_t<std::optional<bytes_t>> read(std::span<const std::string> /*names*/,
                                          std::span<const std::string> keys,
                                          std::uint64_t most) override {
        if (keys.empty()) {
            return std::optional<bytes_t>{};
        }
        const std::vector<found_t> found = find(keys);
        if (found.empty() || found.front().kind != kind_t::FILE) {
            return std::optional<bytes_t>{};
        }
        const fs::path& file = found.front().path;
        bool missing = false;
        const std::unique_ptr<read_file_t> opened = read_file_t::open(file, missing);
        if (opened == nullptr) {
            // One that's gone since it was found isn't there.
            if (missing) {
                return std::optional<bytes_t>{};
            }
            return std::unexpected(error_t{
                .failure = failure_t::OTHER,
                .message = std::format("couldn't read {}", utf8(file)),
            });
        }
        if (opened->size() > most) {
            return std::unexpected(too_big(utf8(file), opened->size()));
        }
        try {
            if (faulted(fault_t::OUT_OF_MEMORY)) {
                throw std::bad_alloc();
            }
            bytes_t bytes(static_cast<std::size_t>(opened->size()));
            if (!opened->read_at(0, bytes)) {
                return std::unexpected(error_t{
                    .failure = failure_t::OTHER,
                    .message = std::format("couldn't read {}", utf8(file)),
                });
            }
            return std::optional<bytes_t>(std::move(bytes));
        } catch (const std::bad_alloc&) {
            return std::unexpected(error_t{
                .failure = failure_t::OTHER,
                .message = std::format("there isn't the memory to read {}", utf8(file)),
            });
        }
    }

    result_t<kind_t> kind(std::span<const std::string> /*names*/,
                          std::span<const std::string> keys) override {
        if (keys.empty()) {
            return kind_t::FOLDER;
        }
        const std::vector<found_t> found = find(keys);
        return found.empty() ? kind_t::NONE : found.front().kind;
    }

    std::optional<stamp_t> stamp(std::span<const std::string> /*names*/,
                                 std::span<const std::string> keys) override {
        if (keys.empty()) {
            return std::nullopt;
        }
        const std::vector<found_t> found = find(keys);
        if (found.empty() || found.front().kind != kind_t::FILE) {
            return std::nullopt;
        }
        stamp_t stamp;
        if (!disk_stamp(found.front().path, stamp.size, stamp.written)) {
            return std::nullopt;
        }
        return stamp;
    }

    result_t<std::optional<std::vector<entry_t>>> list(std::span<const std::string> /*names*/,
                                                       std::span<const std::string> keys) override {
        std::vector<found_t> found;
        if (keys.empty()) {
            // The root is a folder, empty while the folder on the disk isn't there.
            found.push_back({ .path = m_folder, .kind = kind_t::FOLDER });
        } else {
            found = find(keys);
            if (found.empty() || found.front().kind != kind_t::FOLDER) {
                return std::optional<std::vector<entry_t>>{};
            }
        }
        // What every folder of these names has, first by bytes for names that fold the same.
        std::map<std::string, entry_t> listed;
        for (const found_t& folder : found) {
            if (folder.kind != kind_t::FOLDER) {
                continue;
            }
            const std::shared_ptr<const listing_t> entries = listing(folder.path);
            if (entries == nullptr) {
                continue;
            }
            for (const auto& [key, named] : entries->names) {
                const found_t& entry = named.front();
                const auto [at, added] =
                    listed.try_emplace(key, entry_t{ .name = entry.name, .kind = entry.kind });
                if (!added && entry.name < at->second.name) {
                    at->second = { .name = entry.name, .kind = entry.kind };
                }
            }
        }
        std::vector<entry_t> items;
        items.reserve(listed.size());
        for (auto& [key, entry] : listed) {
            items.push_back(std::move(entry));
        }
        return std::optional<std::vector<entry_t>>(std::move(items));
    }

private:
    using found_t = listed_t;

    // A folder on the disk as it was listed: when it last changed then, and its entries by their
    // folded names, first by bytes, of which a lookup takes one stat.
    struct listing_t {
        folder_time_t time;
        std::map<std::string, std::vector<found_t>, std::less<>> names;
    };

    // `folder`'s listing, listed again when it has changed since, or was listed so soon after its
    // last change that another might not have changed its time; null when it isn't a folder.
    [[nodiscard]] std::shared_ptr<const listing_t> listing(const fs::path& folder) const {
        const std::optional<folder_time_t> time = folder_time(folder);
        if (!time) {
            const std::scoped_lock lock(m_mutex);
            m_listings.erase(folder);
            return nullptr;
        }
        {
            const std::scoped_lock lock(m_mutex);
            const auto it = m_listings.find(folder);
            if (it != m_listings.end() && it->second->time.settled &&
                it->second->time.written == time->written) {
                return it->second;
            }
        }
        std::optional<std::vector<found_t>> entries = list_folder(folder);
        if (!entries) {
            return nullptr;
        }
        auto listed = std::make_shared<listing_t>();
        listed->time = *time;
        for (found_t& entry : *entries) {
            if (nameable(entry.name) && !entry.name.starts_with('.')) {
                listed->names[fold(entry.name)].push_back(std::move(entry));
            }
        }
        for (auto& [key, found] : listed->names) {
            std::ranges::sort(found, {}, &found_t::name);
        }
        const std::scoped_lock lock(m_mutex);
        m_listings.insert_or_assign(folder, listed);
        return listed;
    }

    // Everything at `keys`, first by bytes at each name: the folders it can be in, and what's in
    // them by that name.
    [[nodiscard]] std::vector<found_t> find(std::span<const std::string> keys) const {
        std::vector<found_t> at = { { .path = m_folder, .kind = kind_t::FOLDER } };
        for (const std::string& key : keys) {
            std::vector<found_t> next;
            for (const found_t& folder : at) {
                if (folder.kind != kind_t::FOLDER) {
                    continue;
                }
                const std::shared_ptr<const listing_t> listed = listing(folder.path);
                if (listed == nullptr) {
                    continue;
                }
                const auto named = listed->names.find(key);
                if (named != listed->names.end()) {
                    next.insert(next.end(), named->second.begin(), named->second.end());
                }
            }
            if (next.empty()) {
                return {};
            }
            std::ranges::stable_sort(next, {}, &found_t::name);
            at = std::move(next);
        }
        return at;
    }

    fs::path m_folder;
    mutable std::mutex m_mutex;
    // The folders in it listed so far, by their paths on the disk.
    mutable std::map<fs::path, std::shared_ptr<const listing_t>> m_listings;
};

} // namespace

std::shared_ptr<source_t> folder(std::filesystem::path folder) {
    return std::make_shared<folder_source_t>(std::move(folder));
}

} // namespace fbe::vfs
