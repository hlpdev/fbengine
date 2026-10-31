#ifndef FBE_TEXTURE_H
#define FBE_TEXTURE_H

// Textures: images on the GPU, which 2D draws and materials sample, with their pixels uploaded
// through staging, their mipmaps made from their first level, and how they're filtered and
// wrapped.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// What a texture's pixels are: 8-bit RGBA, its color sRGB, as images' is, and sampled as linear
// light; 8-bit RGBA as it's stored, for data, as normal maps and masks; one 8-bit channel; and
// 16-bit floating-point RGBA, for light brighter than white.
#define FBE_TEXTURE_RGBA8        0
#define FBE_TEXTURE_RGBA8_LINEAR 1
#define FBE_TEXTURE_R8           2
#define FBE_TEXTURE_RGBA16F      3

// How a texture is filtered: its nearest pixel, or a blend of the four around, and of its two
// nearest mipmaps.
#define FBE_TEXTURE_NEAREST 0
#define FBE_TEXTURE_LINEAR  1

// How a texture wraps past its edges: repeating, clamped to its edge, or repeating mirrored.
#define FBE_TEXTURE_REPEAT 0
#define FBE_TEXTURE_CLAMP  1
#define FBE_TEXTURE_MIRROR 2

// Makes a texture `width` by `height` pixels of an FBE_TEXTURE_ format, transparent black, with a
// full chain of mipmaps if `mipmaps`, filtered LINEAR and wrapped REPEAT, starting the GPU if it
// hasn't; 0 if it can't. fbe_texture_create holds it once, fbe_texture_retain once more, and
// fbe_texture_release lets go; it's freed once nothing holds it, and stopping the GPU frees every
// texture.
FBE_API uint64_t fbe_texture_create(int32_t width, int32_t height, int32_t format, bool mipmaps);
FBE_API bool fbe_texture_retain(uint64_t texture);
FBE_API bool fbe_texture_release(uint64_t texture);

// Its size in pixels, format, and how many mipmap levels it has, the first its own size; -1 if
// the handle isn't a texture's.
FBE_API int32_t fbe_texture_width(uint64_t texture);
FBE_API int32_t fbe_texture_height(uint64_t texture);
FBE_API int32_t fbe_texture_format(uint64_t texture);
FBE_API int32_t fbe_texture_levels(uint64_t texture);

// Replaces the pixels `width` by `height` at `x`, `y` from its top left with `pixels`, rows
// `pitch` bytes apart, in its format's bytes (4 a pixel for RGBA8, 1 for R8, 8 for RGBA16F), and
// makes its mipmaps again; the GPU has them before anything the frame draws after. False if the
// region isn't in it.
FBE_API bool fbe_texture_write(uint64_t texture, int32_t x, int32_t y, int32_t width,
                               int32_t height, const uint8_t* pixels, int32_t pitch);

// Its FBE_TEXTURE_ filter, and wraps in U, across, and V, down; false if one isn't an
// FBE_TEXTURE_ one.
FBE_API bool fbe_texture_set_filter(uint64_t texture, int32_t filter);
FBE_API int32_t fbe_texture_filter(uint64_t texture);
FBE_API bool fbe_texture_set_wrap(uint64_t texture, int32_t wrap_u, int32_t wrap_v);
FBE_API int32_t fbe_texture_wrap_u(uint64_t texture);
FBE_API int32_t fbe_texture_wrap_v(uint64_t texture);

// How many textures there are.
FBE_API int32_t fbe_texture_count(void);

#ifdef __cplusplus
}
#endif

#endif
