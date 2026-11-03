#ifndef FBE_ASSETS_H
#define FBE_ASSETS_H

// Loads: a file at a res:// path read and decoded, its bytes as they are, an image (PNG, JPEG,
// KTX2), or a glTF's model, now or on the engine's workers. A load is a handle, held until it's
// freed, whose handle is also the ticket the hand-over gives once a load on the workers is done.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// What a load decodes its file to.
#define FBE_ASSETS_BYTES 0
#define FBE_ASSETS_IMAGE 1
#define FBE_ASSETS_MODEL 2

// How a load failed, as std.io's IOErrorKind has it.
#define FBE_ASSETS_NOT_FOUND    0
#define FBE_ASSETS_IS_A_FOLDER  1
#define FBE_ASSETS_NOT_A_FOLDER 2
#define FBE_ASSETS_INVALID_PATH 3
#define FBE_ASSETS_INVALID_DATA 4
#define FBE_ASSETS_OTHER        5

// An image load's image: its Vulkan format, its size, and how many layers, faces (6 for a cube
// map's), and mip levels it has.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct fbe_image_info_t {
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t layers;
    uint32_t faces;
    uint32_t levels;
} fbe_image_info_t;

// How much a model load's model has of each thing.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct fbe_model_info_t {
    int32_t meshes;
    int32_t primitives;
    int32_t materials;
    int32_t images;
    int32_t nodes;
    int32_t scenes;
    int32_t skins;
    int32_t clips;
    int32_t cameras;
    int32_t lights;
} fbe_model_info_t;

// Loads the file at `path` as `kind` now, on this thread and the workers, an image's color linear
// if `linear`, and its sidecar too if `settings`, and gives the load, done, which can have
// failed; 0 if `path` isn't a res:// path, or `kind` isn't a kind. It reads the mounts there are
// as it's asked for, whatever's mounted while it runs.
FBE_API uint64_t fbe_assets_load(int32_t kind, const char* path, bool linear, bool settings);
// Starts loading the file at `path` on the workers, and gives the load, which the hand-over
// after it's done gives the FB side as its ticket; 0 as fbe_assets_load gives it.
FBE_API uint64_t fbe_assets_load_async(int32_t kind, const char* path, bool linear, bool settings);
// A bytes load of the file at `path` on the workers, as fbe_assets_load_async's, of 2147483647
// bytes at most, as many as an FB array holds, which more fails before they're allocated.
FBE_API uint64_t fbe_assets_read_async(const char* path);
// How the last fbe_assets_load or fbe_assets_load_async that gave 0 failed, FBE_ASSETS_INVALID_PATH
// for a path that isn't a res:// path and FBE_ASSETS_OTHER for the rest, as for want of memory.
FBE_API int32_t fbe_assets_load_failure(void);
// Whether the load is done: at once for fbe_assets_load's, and at its hand-over for the rest.
FBE_API bool fbe_assets_done(uint64_t load);
// How much of the load is done, from 0 to 1; -1 for a load that's freed.
FBE_API float fbe_assets_progress(uint64_t load);
// Whether the load is done and failed, which fbe_assets_failure and fbe_assets_error say how.
FBE_API bool fbe_assets_failed(uint64_t load);
// How the load failed, FBE_ASSETS_NOT_FOUND and the rest; -1 if it didn't.
FBE_API int32_t fbe_assets_failure(uint64_t load);
// Why the load failed; "" if it didn't.
FBE_API const char* fbe_assets_error(uint64_t load);
// The res:// path the load read, normalized.
FBE_API const char* fbe_assets_path(uint64_t load);
// Whether the load's file has import settings: a sidecar beside it, its path with ".toml" after
// it, which the load read with it.
FBE_API bool fbe_assets_has_settings(uint64_t load);
// The text of the load's sidecar, TOML, as UTF-8 with no NULs; "" for a load without one, or
// whose sidecar can't be read or isn't TOML's text.
FBE_API const char* fbe_assets_settings(uint64_t load);
// How the load's sidecar couldn't be read, or wasn't TOML's text, FBE_ASSETS_NOT_FOUND and the
// rest, and why; -1 and "" for one that was, and a load without one.
FBE_API int32_t fbe_assets_settings_failure(uint64_t load);
FBE_API const char* fbe_assets_settings_error(uint64_t load);
// How many files the load read besides its own, done, and the res:// path, normalized, of the
// one at `index`: its sidecar, if it reads one, and a model's buffers and images; -1 and "" for
// a load that isn't done, or freed.
FBE_API int32_t fbe_assets_reads(uint64_t load);
FBE_API const char* fbe_assets_read(uint64_t load, int32_t index);
// Waits for a load on the workers to be done, and runs its hand-over, as fbe_frame_flip does, so
// it's done when this returns; false for a load that's freed.
FBE_API bool fbe_assets_wait(uint64_t load);
// Lets go of the load and what it made; false if it's freed already, or never was.
FBE_API bool fbe_assets_free(uint64_t load);
// How many loads there are, not freed.
FBE_API int64_t fbe_assets_count(void);
// A bytes load's bytes, and how many there are, which stay until it's freed; -1 and null for a
// load that isn't a bytes load done and loaded.
FBE_API int64_t fbe_assets_size(uint64_t load);
FBE_API const uint8_t* fbe_assets_bytes(uint64_t load);
// An image load's image; false for a load that isn't an image load done and loaded.
FBE_API bool fbe_assets_image(uint64_t load, fbe_image_info_t* info);
// A model load's model; false for a load that isn't a model load done and loaded.
FBE_API bool fbe_assets_model(uint64_t load, fbe_model_info_t* info);

#ifdef __cplusplus
}
#endif

#endif
