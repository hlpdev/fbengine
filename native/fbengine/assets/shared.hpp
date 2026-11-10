#pragma once

// Shared textures: which texture's been made of an image that's a file of its own, a glTF's URI
// image, so the next model naming that file reads the same one, not another of the same texels.
// One's known by its file's res:// path and what's in it, and by how it's made: the color space
// it's decoded as and made in, its mipmaps, and its sampling. Nothing here holds a texture: one
// goes with the last of what does, which the texture area says as it frees it.

#include <cstdint>
#include <string>
#include <string_view>

namespace fbe::assets {

/// What a shared texture's known by, but for how it's made: its file's path, normalized, and its
/// bytes' hash.
std::string shared_file(std::string_view path, std::uint64_t hash);

/// A hash of a file's bytes, FNV-1a's, with how many there are.
std::uint64_t shared_hash(const std::uint8_t* bytes, std::size_t size);

/// Whether a texture of `file`, shared_file's, decoded as a color or not, is there, however it's
/// sampled; on any thread.
bool shared_known(std::string_view file, bool decoded_srgb);

/// The texture of `file` made as `made` says, after shared_file's and whether it's decoded as a
/// color; 0 for none. On any thread, though a texture's the main thread's to use.
std::uint64_t shared_find(std::string_view file, bool decoded_srgb, std::string_view made);

/// Says `texture` is the one of `file` made as `made` says, in place of any that was.
void shared_add(std::string_view file, bool decoded_srgb, std::string_view made,
                std::uint64_t texture);

/// Forgets `texture`, which is freed, if it's one of these; on any thread.
void shared_forget(std::uint64_t texture);

/// How many there are.
std::size_t shared_count();

} // namespace fbe::assets
