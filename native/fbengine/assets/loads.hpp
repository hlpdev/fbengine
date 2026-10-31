#pragma once

// Loads: a file at a res:// path read and decoded, now on the calling thread and the workers, or
// on a worker while the main thread goes on, handed over as a ticket, its handle, once it's done.
// The handles are the main thread's; what a load makes is its until it's freed.

#include "assets/image.hpp"
#include "assets/model.hpp"
#include "vfs/vfs.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fbe::assets {

/// The kinds of fbengine's handles that assets' pools give, in its range of architecture.md's,
/// laid out as native/common/handles.hpp will have them: a 32-bit slot, a 24-bit generation, and
/// the 8-bit kind.
enum handle_kind_t : std::uint8_t { LOAD_KIND = 7 };

/// What a load decodes its file to: its bytes as they are, an image, or a glTF's model.
enum class load_kind_t : std::uint8_t { BYTES, IMAGE, MODEL };

struct load_options_t {
    /// For images: their color is linear, as a normal map's is, not sRGB.
    bool linear = false;
    transcode_t transcode = transcode_t::RGBA8;
};

/// A load, and what it made once it's done: its kind's result and its file's import settings, or
/// why it failed.
struct load_t {
    load_kind_t kind = load_kind_t::BYTES;
    vfs::path_t path;
    load_options_t options;
    /// The job a load on the workers runs as; 0 for one that ran where it was asked for.
    job_t job = 0;
    /// From 0 to 1, as the worker goes.
    std::atomic<float> progress = 0;
    /// Set on the main thread, as the load's handed over.
    bool done = false;
    std::optional<error_t> error;
    /// The text of its file's sidecar, its path with ".toml" after it, when there's one: UTF-8
    /// with no NULs, as TOML is.
    std::optional<std::string> settings;
    bytes_t bytes;
    image_t image;
    model_t model;
};

/// Reads and decodes `load`'s file, and its sidecar, on this thread, with `jobs` to decode a
/// model's images at once.
void run_load(load_t& load, jobs_t* jobs);

/// The sidecar of the file at `path`, its import settings: its path with ".toml" after it.
std::optional<vfs::path_t> sidecar_of(const vfs::path_t& path);

/// The loads by handle; the main thread's.
class loads_t {
public:
    /// Keeps a new load, and gives its handle, never 0.
    std::uint64_t add(std::shared_ptr<load_t> load);
    /// The load a handle is, or null for one that's freed, never was, or is of another kind.
    [[nodiscard]] load_t* get(std::uint64_t handle) const;
    [[nodiscard]] std::shared_ptr<load_t> share(std::uint64_t handle) const;
    /// Lets go of a load; false if there's none.
    bool remove(std::uint64_t handle);
    [[nodiscard]] std::size_t count() const;

private:
    struct slot_t {
        std::shared_ptr<load_t> load;
        // 0 once retired, its last generation freed.
        std::uint32_t generation = 1;
    };

    [[nodiscard]] std::optional<std::uint32_t> slot_of(std::uint64_t handle) const;

    std::vector<slot_t> m_slots;
    std::vector<std::uint32_t> m_free;
    std::size_t m_count = 0;
};

/// The engine's loads, for the C API.
loads_t& loads();

} // namespace fbe::assets
