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

// Makes a target, a texture drawn into, `width` by `height` pixels of an FBE_TEXTURE_ format,
// transparent black, with no mipmaps, drawing `samples` samples a pixel (1, 2, 4, or 8), which
// each pass resolves into it, and with depth if `depth` (32-bit floats, reversed: 1 nearest, 0
// farthest, as it's cleared, compared GREATER_OR_EQUAL); 0 if it can't. What's drawn into it is in
// Vulkan's clip space, x right and y down from -1 to 1, depth 1 at the near plane, as the engine's
// cameras project (std.math's Perspective, near 0 and y up, isn't for it). It's held and freed as
// fbe_texture_create's are.
FBE_API uint64_t fbe_texture_create_target(int32_t width, int32_t height, int32_t format,
                                           int32_t samples, bool depth);
// Whether it's a target, how many samples a pass draws a pixel, -1 if the handle isn't a
// texture's, and whether it has depth.
FBE_API bool fbe_texture_is_target(uint64_t texture);
FBE_API int32_t fbe_texture_samples(uint64_t texture);
FBE_API bool fbe_texture_has_depth(uint64_t texture);

// Its size in pixels, format, and how many mipmap levels it has, the first its own size; -1 if
// the handle isn't a texture's.
FBE_API int32_t fbe_texture_width(uint64_t texture);
FBE_API int32_t fbe_texture_height(uint64_t texture);
FBE_API int32_t fbe_texture_format(uint64_t texture);
FBE_API int32_t fbe_texture_levels(uint64_t texture);

// Replaces the pixels `width` by `height` at `x`, `y` from its top left with `pixels`, rows
// `pitch` bytes apart, in its format's bytes (4 a pixel for RGBA8, 1 for R8, 8 for
// RGBA16F), and
// makes its mipmaps again, in turn with what else the frame does with it: drawing with it or into
// it, clearing, and copying before the write see it as it was, and after, the write. False if the
// region isn't in it, or it's a multisampled target, which is only drawn into.
FBE_API bool fbe_texture_write(uint64_t texture, int32_t x, int32_t y, int32_t width,
                               int32_t height, const uint8_t* pixels, int32_t pitch);

// Clears every level of it to a color, linear (an RGBA8 texture's is stored as sRGB), and a
// target's depth to the farthest, in the frame's commands, after what the frame does with it
// before.
FBE_API bool fbe_texture_clear(uint64_t texture, float red, float green, float blue, float alpha);

// Copies the pixels `width` by `height` at `from_x`, `from_y` of `from`'s first level to `x`, `y`
// in `to`'s, and makes `to`'s mipmaps again, in the frame's commands, so a frame drawn into a
// target can be copied into a texture as it's drawn; within one texture, regions that overlap
// copy as they were. False unless their formats are the same size (RGBA8 and RGBA8_LINEAR are),
// the regions are in them, and `to` isn't a multisampled target, which is only drawn into.
FBE_API bool fbe_texture_copy(uint64_t to, int32_t x, int32_t y, uint64_t from, int32_t from_x,
                              int32_t from_y, int32_t width, int32_t height);

// A CPU copy of a texture's first level's pixels, rows top first, packed, in its format's bytes,
// which reads and writes go to a pixel at a time; `size` is how many bytes there are. It's read
// back from the GPU, into where it is, only when the GPU's changed them since, which waits for the
// frame so far, and stays where it is until the texture's freed or the GPU stops. What's written
// into it reaches the GPU once fbe_texture_pixels_written says where, before the GPU next uses the
// texture, so drawing and writing keep their order. nullptr if the texture isn't live or is
// multisampled, whose pixels are only drawn.
FBE_API uint8_t* fbe_texture_pixels(uint64_t texture, int64_t* size);
// Where the stamp of a texture's CPU copy is, a number that changes whenever the copy's freed, as
// the texture is or the GPU stops: it stays there as long as the program runs, so what holds the
// copy's pointer can tell with a read that the copy's still there. nullptr if the texture isn't
// live.
FBE_API const uint64_t* fbe_texture_pixels_stamp(uint64_t texture);
// Says the rectangle `width` by `height` at `x`, `y` of a texture's CPU copy was written; false
// if it isn't in it, the texture has no CPU copy, or the GPU's changed the texture since the copy
// was read back, as drawing into it does, whose pixels the rectangle would go over.
FBE_API bool fbe_texture_pixels_written(uint64_t texture, int32_t x, int32_t y, int32_t width,
                                        int32_t height);

// Reads a level's pixels back into `pixels`, `size` bytes, after everything the frame's recorded
// so far, waiting for the GPU to finish it: rows top first, packed, in its format's bytes. False
// if there's no such level, or `size` isn't its size.
FBE_API bool fbe_texture_read(uint64_t texture, int32_t level, uint8_t* pixels, int64_t size);

// Its FBE_TEXTURE_ filter, and wraps in U, across, and V, down; false if one isn't an
// FBE_TEXTURE_ one.
// Whether its alpha's for blending, which a material blending FBE_BLEND_AUTO goes by, false at
// first; false for a freed texture.
FBE_API bool fbe_texture_set_alpha(uint64_t texture, bool alpha);
FBE_API bool fbe_texture_alpha(uint64_t texture);
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
