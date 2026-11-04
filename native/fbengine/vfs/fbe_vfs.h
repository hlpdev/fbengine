#ifndef FBE_VFS_H
#define FBE_VFS_H

// The game's res:// files: folders on the disk and pak files, mounted at places in one tree, a
// later mount over an earlier one. Paths are "res://" and names with '/' or '\' between them, in
// any case. What fails says why in fbe_error, and how in fbe_vfs_failure, as OTHER when there
// isn't the memory for it.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// What's at a path.
#define FBE_VFS_NONE   0
#define FBE_VFS_FILE   1
#define FBE_VFS_FOLDER 2

// How the last call that failed on this thread failed, as std.io's IOErrorKind has it.
#define FBE_VFS_NOT_FOUND    0
#define FBE_VFS_IS_A_FOLDER  1
#define FBE_VFS_NOT_A_FOLDER 2
#define FBE_VFS_INVALID_PATH 3
#define FBE_VFS_INVALID_DATA 4
#define FBE_VFS_OTHER        5

// Mounts the folder on the disk at `folder` at `at`, a res:// path, over what's mounted; false if
// `at` isn't a res:// path. A folder that isn't there is empty until it is.
FBE_API bool fbe_vfs_mount_folder(const char* at, const char* folder);
// Mounts the pak file at `file`, a zip whose files are stored, reading its index now; false if it
// can't be read or isn't a pak.
FBE_API bool fbe_vfs_mount_pak(const char* at, const char* file);
// Mounts the `size` bytes at `bytes`, a pak, copying them.
FBE_API bool fbe_vfs_mount_pak_bytes(const char* at, const uint8_t* bytes, int64_t size);
// Takes out the last mount at `at`; false if there's none there.
FBE_API bool fbe_vfs_unmount(const char* at);
// The number of the last mount this thread made, for fbe_vfs_unmount_id.
FBE_API uint64_t fbe_vfs_last_mount(void);
// Takes out the mount of that number, whatever's over it; false if it's gone.
FBE_API bool fbe_vfs_unmount_id(uint64_t mount);
// How the last call that failed on this thread failed: FBE_VFS_NOT_FOUND and the rest.
FBE_API int32_t fbe_vfs_failure(void);

// Keeps `path` as it's compared, for fbe_vfs_normalized: "res://" and its names, folded, with '/'
// between them; false if it isn't a res:// path.
FBE_API bool fbe_vfs_normalize(const char* path);
// What the last fbe_vfs_normalize on this thread kept.
FBE_API const char* fbe_vfs_normalized(void);
// What's at `path`, FBE_VFS_NONE, FBE_VFS_FILE, or FBE_VFS_FOLDER; -1 if it can't tell.
FBE_API int32_t fbe_vfs_kind(const char* path);
// The folder or file at `path` on the disk, UTF-8, through every link to it, or `path` when the OS
// can't say, kept for fbe_vfs_normalized to give; false without the memory for it.
FBE_API bool fbe_vfs_real_path(const char* path);
// Reads the file at `path`, keeping its bytes for fbe_vfs_read_copy, of 2147483647 at most, as many
// as an FB array holds, which more fails before they're allocated, and gives how many there are;
// -1 if it can't.
FBE_API int64_t fbe_vfs_read(const char* path);
// Reads the file at `path` as fbe_vfs_read does, keeping it as well-formed UTF-8, with U+FFFD for
// each byte that doesn't start a well-formed sequence, and gives how many bytes that is.
FBE_API int64_t fbe_vfs_read_text(const char* path);
// Copies the bytes the last fbe_vfs_read or fbe_vfs_read_text on this thread kept into `into`, and
// lets them go.
FBE_API void fbe_vfs_read_copy(uint8_t* into);
// Lists the folder at `path`, sorted by folded names, keeping the list for fbe_vfs_list_name and
// fbe_vfs_list_kind, and gives how many are in it; -1 if it can't.
FBE_API int32_t fbe_vfs_list(const char* path);
// The name at `index` of the last list on this thread, as its mount has it; "" past its end.
FBE_API const char* fbe_vfs_list_name(int32_t index);
// What's at `index` of the last list on this thread; FBE_VFS_NONE past its end.
FBE_API int32_t fbe_vfs_list_kind(int32_t index);

#ifdef __cplusplus
}
#endif

#endif
