// A folder on the disk as a source, which finds a path as a pak does, whatever the disk does with
// case: at each name, every name in the folders so far that folds to it, by their real names on
// the disk, first by bytes, so "Data" and "data" are one folder, and neither spelling in a path,
// nor Windows' own names for a file (a short 8.3 name, a dot at the end, a device), finds what a
// pak of the same files wouldn't.

#include "vfs/disk.hpp"
#include "vfs/vfs.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <new>
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
                                          std::span<const std::string> keys) override {
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
        try {
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
            for (found_t& entry : entries(folder.path)) {
                const auto [at, added] = listed.try_emplace(
                    fold(entry.name), entry_t{ .name = entry.name, .kind = entry.kind });
                if (!added && entry.name < at->second.name) {
                    at->second = { .name = std::move(entry.name), .kind = entry.kind };
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
    struct found_t {
        fs::path path;
        kind_t kind = kind_t::NONE;
        // Its name on the disk.
        std::string name;
    };

    static kind_t kind_of(const fs::path& path) {
        std::error_code error;
        const fs::file_status status = fs::status(path, error);
        if (error) {
            return kind_t::NONE;
        }
        if (fs::is_directory(status)) {
            return kind_t::FOLDER;
        }
        return fs::is_regular_file(status) ? kind_t::FILE : kind_t::NONE;
    }

    // What's in `folder` that a res:// path can name, by its real names, in no order.
    static std::vector<found_t> entries(const fs::path& folder) {
        std::vector<found_t> found;
        std::error_code error;
        for (fs::directory_iterator it(folder, error); !error && it != fs::directory_iterator();
             it.increment(error)) {
            std::string name = utf8(it->path().filename());
            const kind_t kind = kind_of(it->path());
            if (kind != kind_t::NONE && nameable(name)) {
                found.push_back({ .path = it->path(), .kind = kind, .name = std::move(name) });
            }
        }
        return found;
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
                for (found_t& entry : entries(folder.path)) {
                    if (fold(entry.name) == key) {
                        next.push_back(std::move(entry));
                    }
                }
            }
            std::ranges::stable_sort(next, {}, &found_t::name);
            if (next.empty()) {
                return {};
            }
            at = std::move(next);
        }
        return at;
    }

    fs::path m_folder;
};

} // namespace

std::shared_ptr<source_t> folder(std::filesystem::path folder) {
    return std::make_shared<folder_source_t>(std::move(folder));
}

} // namespace fbe::vfs
