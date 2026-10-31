// The C API over the engine's res://, whose results stay in the calling thread's until its next
// call.

#include "core/engine.hpp"
#include "fbengine.h"
#include "vfs/disk.hpp"
#include "vfs/vfs.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace vfs = fbe::vfs;

thread_local int32_t last_failure = FBE_VFS_OTHER;
thread_local std::string normalized;
thread_local vfs::bytes_t read_bytes;
thread_local std::vector<vfs::entry_t> listed;

int32_t failure_of(vfs::failure_t failure) {
    switch (failure) {
    case vfs::failure_t::NOT_FOUND:
        return FBE_VFS_NOT_FOUND;
    case vfs::failure_t::IS_A_FOLDER:
        return FBE_VFS_IS_A_FOLDER;
    case vfs::failure_t::NOT_A_FOLDER:
        return FBE_VFS_NOT_A_FOLDER;
    case vfs::failure_t::INVALID_PATH:
        return FBE_VFS_INVALID_PATH;
    case vfs::failure_t::INVALID_DATA:
        return FBE_VFS_INVALID_DATA;
    case vfs::failure_t::OTHER:
        break;
    }
    return FBE_VFS_OTHER;
}

bool failed(const vfs::error_t& error) {
    last_failure = failure_of(error.failure);
    return fbe::fail(error.message);
}

// A path on the disk, made absolute now, so the program's folder changing later doesn't move it.
std::filesystem::path on_disk(const char* text) {
    std::filesystem::path const path = vfs::disk_path(text);
    std::error_code error;
    std::filesystem::path const absolute = std::filesystem::absolute(path, error);
    return error ? path : absolute;
}

// Mounts what `open` gives at `at`, once `at` is known to be a res:// path.
template <typename Open>
bool mount(const char* at, Open open) {
    const vfs::result_t<vfs::path_t> place = vfs::parse(at);
    if (!place) {
        return failed(place.error());
    }
    vfs::result_t<std::shared_ptr<vfs::source_t>> source = open();
    if (!source) {
        return failed(source.error());
    }
    vfs::files().mount(*place, std::move(*source));
    return true;
}

// What `work` gives, or `failed`, with fbe_error saying so, when there isn't the memory for it:
// the sizes a pak or a folder gives can be more than there is.
template <typename T, typename Work>
T guarded(T failed, Work work) {
    try {
        return work();
    } catch (const std::bad_alloc&) {
        last_failure = FBE_VFS_OTHER;
        fbe::fail("there isn't the memory for it");
        return failed;
    }
}

} // namespace

namespace fbe::vfs {

vfs_t& files() {
    // Never destroyed, since workers can be reading as the program ends.
    static auto* const engine = new vfs_t();
    return *engine;
}

} // namespace fbe::vfs

namespace {

bool unmount(const char* at) {
    const vfs::result_t<vfs::path_t> place = vfs::parse(at);
    if (!place) {
        return failed(place.error());
    }
    if (!vfs::files().unmount(*place)) {
        last_failure = FBE_VFS_NOT_FOUND;
        return fbe::fail(std::format("nothing is mounted at {}", place->text()));
    }
    return true;
}

bool normalize(const char* path) {
    normalized.clear();
    const vfs::result_t<vfs::path_t> parsed = vfs::parse(path);
    if (!parsed) {
        return failed(parsed.error());
    }
    normalized = parsed->key();
    return true;
}

int32_t kind(const char* path) {
    const vfs::result_t<vfs::path_t> parsed = vfs::parse(path);
    if (!parsed) {
        failed(parsed.error());
        return -1;
    }
    const vfs::result_t<vfs::kind_t> kind = vfs::files().kind(*parsed);
    if (!kind) {
        failed(kind.error());
        return -1;
    }
    switch (*kind) {
    case vfs::kind_t::FILE:
        return FBE_VFS_FILE;
    case vfs::kind_t::FOLDER:
        return FBE_VFS_FOLDER;
    case vfs::kind_t::NONE:
        break;
    }
    return FBE_VFS_NONE;
}

int64_t read(const char* path, bool text) {
    read_bytes = {};
    const vfs::result_t<vfs::path_t> parsed = vfs::parse(path);
    if (!parsed) {
        failed(parsed.error());
        return -1;
    }
    vfs::result_t<vfs::bytes_t> bytes = vfs::files().read(*parsed);
    if (!bytes) {
        failed(bytes.error());
        return -1;
    }
    if (text) {
        const std::string well = vfs::well_formed(
            std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        read_bytes.assign(well.begin(), well.end());
    } else {
        read_bytes = std::move(*bytes);
    }
    return static_cast<int64_t>(read_bytes.size());
}

int32_t list(const char* path) {
    listed.clear();
    const vfs::result_t<vfs::path_t> parsed = vfs::parse(path);
    if (!parsed) {
        failed(parsed.error());
        return -1;
    }
    vfs::result_t<std::vector<vfs::entry_t>> entries = vfs::files().list(*parsed);
    if (!entries) {
        failed(entries.error());
        return -1;
    }
    listed = std::move(*entries);
    return static_cast<int32_t>(listed.size());
}

} // namespace

extern "C" {

FBE_API bool fbe_vfs_mount_folder(const char* at, const char* folder) {
    return guarded(false, [&] {
        return mount(at, [folder] {
            return vfs::result_t<std::shared_ptr<vfs::source_t>>(vfs::folder(on_disk(folder)));
        });
    });
}

FBE_API bool fbe_vfs_mount_pak(const char* at, const char* file) {
    return guarded(false, [&] { return mount(at, [file] { return vfs::pak(on_disk(file)); }); });
}

FBE_API bool fbe_vfs_mount_pak_bytes(const char* at, const uint8_t* bytes, int64_t size) {
    return guarded(false, [&] {
        return mount(at, [bytes, size] {
            return vfs::pak(size > 0 ? vfs::bytes_t(bytes, bytes + size) : vfs::bytes_t());
        });
    });
}

FBE_API bool fbe_vfs_unmount(const char* at) {
    return guarded(false, [&] { return unmount(at); });
}

FBE_API int32_t fbe_vfs_failure(void) {
    return last_failure;
}

FBE_API bool fbe_vfs_normalize(const char* path) {
    return guarded(false, [&] { return normalize(path); });
}

FBE_API const char* fbe_vfs_normalized(void) {
    return normalized.c_str();
}

FBE_API int32_t fbe_vfs_kind(const char* path) {
    return guarded(-1, [&] { return kind(path); });
}

FBE_API int64_t fbe_vfs_read(const char* path) {
    return guarded<int64_t>(-1, [&] { return read(path, false); });
}

FBE_API int64_t fbe_vfs_read_text(const char* path) {
    return guarded<int64_t>(-1, [&] { return read(path, true); });
}

FBE_API void fbe_vfs_read_copy(uint8_t* into) {
    std::ranges::copy(read_bytes, into);
    read_bytes = {};
}

FBE_API int32_t fbe_vfs_list(const char* path) {
    return guarded(-1, [&] { return list(path); });
}

FBE_API const char* fbe_vfs_list_name(int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= listed.size()) {
        return "";
    }
    return listed[static_cast<std::size_t>(index)].name.c_str();
}

FBE_API int32_t fbe_vfs_list_kind(int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= listed.size()) {
        return FBE_VFS_NONE;
    }
    return listed[static_cast<std::size_t>(index)].kind == vfs::kind_t::FOLDER ? FBE_VFS_FOLDER
                                                                               : FBE_VFS_FILE;
}

} // extern "C"
