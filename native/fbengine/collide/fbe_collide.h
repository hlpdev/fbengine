#ifndef FBE_COLLIDE_H
#define FBE_COLLIDE_H

// Collisions in 2D, on the CPU: masks, an image's solid pixels by an alpha threshold, and whether
// they meet each other, rectangles, and points, each placed as the canvas draws a sprite. A screen
// pixel is a placed mask's where its middle maps back into one of the mask's solid pixels, as a
// sprite sampled NEAREST draws it; touching isn't meeting.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Where a mask is drawn, as a sprite is: its handle, the point of it, in its pixels, that lands at
// (x, y), which it's turned about, `rotation` radians clockwise on the screen, and scaled from,
// across and down, a negative scale mirroring it.
// C has no `using`, so it's a typedef.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct fbe_collide_place_t {
    float x;
    float y;
    float handle_x;
    float handle_y;
    float rotation;
    float scale_x;
    float scale_y;
} fbe_collide_place_t;

// A rectangle, from its top left.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct fbe_collide_rect_t {
    float x;
    float y;
    float width;
    float height;
} fbe_collide_rect_t;

// A mask of the `width` by `height` RGBA8 pixels at `pixels`, `stride` bytes from one row's start
// to the next's, each solid whose alpha is `threshold` (1 to 255) or more; 0 with fbe_error's why
// for a size that's negative or past 65,536, a stride shorter than a row, or a threshold outside
// 1 to 255. fbe_collide_free frees it.
FBE_API uint64_t fbe_collide_mask(const uint8_t* pixels, int32_t width, int32_t height,
                                  int64_t stride, int32_t threshold);
// Frees a mask; false if the handle isn't a mask's.
FBE_API bool fbe_collide_free(uint64_t mask);
// How many masks there are, unfreed.
FBE_API int32_t fbe_collide_count(void);

// A mask's size, -1 if the handle isn't a mask's; how many of its pixels are solid; and whether
// the one at `x`, `y` is, false outside it.
FBE_API int32_t fbe_collide_width(uint64_t mask);
FBE_API int32_t fbe_collide_height(uint64_t mask);
FBE_API int64_t fbe_collide_solid_count(uint64_t mask);
FBE_API bool fbe_collide_solid(uint64_t mask, int32_t x, int32_t y);

// The rectangle a mask placed as `place` says covers, its corners' bounds, into `bounds`; false if
// the handle isn't a mask's, or the place isn't finite.
FBE_API bool fbe_collide_bounds(uint64_t mask, const fbe_collide_place_t* place,
                                fbe_collide_rect_t* bounds);
// Whether two placed masks have a screen pixel solid in both: 1 if they do, 0 if not, -1 with
// fbe_error's why if a handle isn't a mask's or a place isn't finite.
FBE_API int32_t fbe_collide_masks(uint64_t first, const fbe_collide_place_t* first_place,
                                  uint64_t second, const fbe_collide_place_t* second_place);
// Whether a placed mask has a solid screen pixel inside `rect`, one whose area its inside reaches
// into: 1, 0, or -1 as fbe_collide_masks.
FBE_API int32_t fbe_collide_rect(uint64_t mask, const fbe_collide_place_t* place,
                                 const fbe_collide_rect_t* rect);
// Whether the screen pixel `x`, `y` is in is one of a placed mask's solid ones: 1, 0, or -1.
FBE_API int32_t fbe_collide_point(uint64_t mask, const fbe_collide_place_t* place, float x,
                                  float y);

#ifdef __cplusplus
}
#endif

#endif
