#pragma once

// The engine's res:// files: folders on the disk and pak files, mounted at places in one tree,
// later mounts first, as std.vfs's Vfs mounts its sources. It's C++ so loaders on worker threads
// read it; any thread can read while the main thread mounts. Paths are case-insensitive and take
// '/' or '\' between names, so a game's paths work on every OS whatever the disk does.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbe::vfs {

/// What's at a path.
enum class kind_t : std::uint8_t { NONE, FILE, FOLDER };

/// Why a call failed, as std.io's IOErrorKind says it.
enum class failure_t : std::uint8_t {
    NOT_FOUND,
    IS_A_FOLDER,
    NOT_A_FOLDER,
    INVALID_PATH,
    INVALID_DATA,
    OTHER,
};

struct error_t {
    failure_t failure = failure_t::OTHER;
    std::string message;
};

template <typename T>
using result_t = std::expected<T, error_t>;

using bytes_t = std::vector<std::uint8_t>;

/// A res:// path, its names as they were written and folded, which is how they're compared:
/// ASCII letters in lower case, other bytes as they are.
struct path_t {
    std::vector<std::string> names;
    std::vector<std::string> keys;

    /// "res://" and the names as written, with '/' between them.
    [[nodiscard]] std::string text() const;
    /// "res://" and the folded names, with '/' between them: the same for every way of writing
    /// one path.
    [[nodiscard]] std::string key() const;
};

/// `text` as a path: "res://", in any case, then names with '/' or '\' between them, where "" and
/// "." are nothing and ".." takes out the name before it. A path that goes above the root, or has
/// a name no file can have on every OS (with one of `: * ? " < > |` or a control character, a dot
/// or a space at its end, or a Windows device's, as CON or nul.txt), is an INVALID_PATH.
result_t<path_t> parse(std::string_view text);

/// `relative`, a path from the folder `base` is in, as glTF's and materials' paths are, or a
/// res:// path, which it is as it is.
result_t<path_t> resolve(const path_t& base, std::string_view relative);

/// A name folded, as paths are compared.
std::string fold(std::string_view name);

/// Whether `name` can be a name in a res:// path, as parse has it.
bool nameable(std::string_view name);

/// A name in a folder, as its source has it, and what it is.
struct entry_t {
    std::string name;
    kind_t kind = kind_t::NONE;
};

/// Where a mount's files come from. Its paths are the folded names inside it, and its root, no
/// names, is a folder. What isn't there is nullopt; what can't be read is an error. Any thread
/// calls it, several at once.
class source_t {
public:
    source_t() = default;
    virtual ~source_t() = default;
    source_t(const source_t&) = delete;
    source_t& operator=(const source_t&) = delete;
    source_t(source_t&&) = delete;
    source_t& operator=(source_t&&) = delete;

    /// The bytes of the file at `path`, whose names are `names` as written and `keys` folded.
    virtual result_t<std::optional<bytes_t>> read(std::span<const std::string> names,
                                                  std::span<const std::string> keys) = 0;
    virtual result_t<kind_t> kind(std::span<const std::string> names,
                                  std::span<const std::string> keys) = 0;
    /// What's in the folder there, in any order, or nullopt when there's no folder there.
    virtual result_t<std::optional<std::vector<entry_t>>>
    list(std::span<const std::string> names, std::span<const std::string> keys) = 0;
};

/// A folder on the disk, read as it's asked, so it sees what changes in it; it's empty while the
/// folder isn't there. A name is found in any case: of the names in a folder that fold to it, the
/// first by its bytes.
std::shared_ptr<source_t> folder(std::filesystem::path folder);

/// A pak file: a zip whose files are stored, not compressed, read through its index, which this
/// reads now. A zip with a compressed or encrypted file, or that makes one place a file and a
/// folder too, is INVALID_DATA. Entries that go above its root, and links, aren't in it, and of
/// two entries of one path, the last is.
result_t<std::shared_ptr<source_t>> pak(const std::filesystem::path& file);

/// A pak in memory, which it keeps.
result_t<std::shared_ptr<source_t>> pak(bytes_t bytes);

/// The mounts there were at a moment, which what's read through it keeps, whatever's mounted
/// since, as a load reads its file, its sidecar, and what its file names. A file is the last
/// mount's that has it; a folder lists what every mount has in it, and the mounts under it, by
/// their folded names, the last mount's name for one two have.
class view_t {
public:
    /// No mounts.
    view_t() = default;

    /// The file's bytes. One that no mount has is NOT_FOUND, and a folder, res:// too,
    /// IS_A_FOLDER.
    [[nodiscard]] result_t<bytes_t> read(const path_t& path) const;
    [[nodiscard]] result_t<kind_t> kind(const path_t& path) const;
    /// What's in the folder, sorted by folded names. A folder no mount has is NOT_FOUND, and a
    /// file NOT_A_FOLDER.
    [[nodiscard]] result_t<std::vector<entry_t>> list(const path_t& path) const;

private:
    friend class vfs_t;

    struct mount_t {
        path_t at;
        std::shared_ptr<source_t> source;
    };
    using mounts_t = std::vector<mount_t>;

    explicit view_t(std::shared_ptr<const mounts_t> mounts) : m_mounts(std::move(mounts)) {}

    std::shared_ptr<const mounts_t> m_mounts = std::make_shared<const mounts_t>();
};

/// Sources mounted at places, a later mount over an earlier one, read as view_t reads them. A read
/// reads the mounts there were when it began, whatever's mounted meanwhile.
class vfs_t {
public:
    void mount(const path_t& at, std::shared_ptr<source_t> source);
    /// Takes out the last mount at `at`, and gives whether there was one.
    bool unmount(const path_t& at);

    /// The mounts there are now.
    [[nodiscard]] view_t view() const;

    [[nodiscard]] result_t<bytes_t> read(const path_t& path) const {
        return view().read(path);
    }
    [[nodiscard]] result_t<kind_t> kind(const path_t& path) const {
        return view().kind(path);
    }
    [[nodiscard]] result_t<std::vector<entry_t>> list(const path_t& path) const {
        return view().list(path);
    }

private:
    mutable std::mutex m_mutex;
    // The mounts, replaced whole by each mount and unmount, so a view keeps the ones it was of.
    std::shared_ptr<const view_t::mounts_t> m_mounts = std::make_shared<const view_t::mounts_t>();
};

/// The engine's res://, which the C API mounts and the loaders read.
vfs_t& files();

/// A fault a test makes happen, as the disk and the system can: OUT_OF_MEMORY is the next read of
/// a file's bytes, from a folder or a pak, finding no memory for them.
enum class fault_t : std::uint8_t { NONE, OUT_OF_MEMORY };

/// Makes `fault` happen once, at the next place it can.
void inject(fault_t fault);

/// Whether `fault` happens here: whether it was injected, which it no longer is.
bool faulted(fault_t fault);

} // namespace fbe::vfs
