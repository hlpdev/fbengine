// A folder on the disk as a source. A path is looked for as it's written first, which is all a
// disk that ignores case needs, and then name by name in any case, the first by its bytes of the
// names that fold the same, so a game's paths find its files on every OS.

#include "vfs/disk.hpp"
#include "vfs/vfs.hpp"

#include <format>
#include <fstream>
#include <ios>
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

    result_t<std::optional<bytes_t>> read(std::span<const std::string> names,
                                          std::span<const std::string> keys) override {
        const std::optional<fs::path> file = find(names, keys);
        std::error_code error;
        if (!file || !fs::is_regular_file(*file, error)) {
            return std::optional<bytes_t>{};
        }
        std::ifstream stream(*file, std::ios::binary);
        const std::uintmax_t size = fs::file_size(*file, error);
        if (!stream || error) {
            return std::unexpected(error_t{
                .failure = failure_t::OTHER,
                .message = std::format("couldn't read {}: {}", utf8(*file),
                                       error ? error.message() : "it won't open"),
            });
        }
        bytes_t bytes(static_cast<std::size_t>(size));
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        // A file that got shorter since its size was read is what was there.
        bytes.resize(static_cast<std::size_t>(stream.gcount()));
        if (stream.bad()) {
            return std::unexpected(error_t{
                .failure = failure_t::OTHER,
                .message = std::format("couldn't read {}", utf8(*file)),
            });
        }
        return std::optional<bytes_t>(std::move(bytes));
    }

    result_t<kind_t> kind(std::span<const std::string> names,
                          std::span<const std::string> keys) override {
        if (names.empty()) {
            return kind_t::FOLDER;
        }
        const std::optional<fs::path> found = find(names, keys);
        return found ? kind_of(*found) : kind_t::NONE;
    }

    result_t<std::optional<std::vector<entry_t>>> list(std::span<const std::string> names,
                                                       std::span<const std::string> keys) override {
        const std::optional<fs::path> found = find(names, keys);
        if (!found || kind_of(*found) != kind_t::FOLDER) {
            // The root is a folder, empty while the folder on the disk isn't there.
            if (names.empty()) {
                return std::optional<std::vector<entry_t>>(std::vector<entry_t>{});
            }
            return std::optional<std::vector<entry_t>>{};
        }
        std::vector<entry_t> entries;
        std::error_code error;
        for (fs::directory_iterator it(*found, error); !error && it != fs::directory_iterator();
             it.increment(error)) {
            std::string name = utf8(it->path().filename());
            const kind_t kind = kind_of(it->path());
            // A name no res:// path can have isn't in it.
            const result_t<path_t> as_path = parse(std::format("res://{}", name));
            if (kind != kind_t::NONE && as_path && as_path->names.size() == 1) {
                entries.push_back({ .name = std::move(name), .kind = kind });
            }
        }
        if (error) {
            return std::unexpected(error_t{
                .failure = failure_t::OTHER,
                .message = std::format("couldn't list {}: {}", utf8(*found), error.message()),
            });
        }
        return std::optional<std::vector<entry_t>>(std::move(entries));
    }

private:
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

    // Where the path is on the disk, or nullopt when nothing is.
    [[nodiscard]] std::optional<fs::path> find(std::span<const std::string> names,
                                               std::span<const std::string> keys) const {
        std::error_code error;
        fs::path exact = m_folder;
        for (const std::string& name : names) {
            exact /= disk_path(name);
        }
        if (fs::exists(exact, error)) {
            return exact;
        }

        fs::path at = m_folder;
        for (std::size_t i = 0; i < names.size(); ++i) {
            fs::path next = at / disk_path(names[i]);
            if (fs::exists(next, error)) {
                at = std::move(next);
                continue;
            }
            std::optional<fs::path> match;
            std::string matched;
            for (fs::directory_iterator it(at, error); !error && it != fs::directory_iterator();
                 it.increment(error)) {
                std::string name = utf8(it->path().filename());
                if (fold(name) == keys[i] && (!match || name < matched)) {
                    match = it->path();
                    matched = std::move(name);
                }
            }
            if (!match) {
                return std::nullopt;
            }
            at = std::move(*match);
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
