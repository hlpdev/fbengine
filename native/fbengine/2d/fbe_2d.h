#ifndef FBE_2D_H
#define FBE_2D_H

// 2D: shapes drawn into a target, a texture or the screen, exact to the pixel, with a pen that says
// where its origin is, what its viewport clips to, its color, and how it blends. What's drawn into
// one target is batched, going to the GPU together, in its turn with whatever else the frame does.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// How a pen blends what it draws with what's there: replacing it, alpha too; over it, by alpha;
// added to it, as light is; or multiplied with it.
#define FBE_2D_SOLID    0
#define FBE_2D_ALPHA    1
#define FBE_2D_ADD      2
#define FBE_2D_MULTIPLY 3

// A pen: its origin, added to every position it draws at; its viewport, which clips what it draws,
// in the target's pixels from its top left with the origin not added, 0 by 0 for the whole target;
// its color, sRGB bytes, or, into a target whose pixels are data, RGBA8_LINEAR's or R8's, the
// bytes stored; how it blends, an FBE_2D_ blend; and whether it draws in stored values, 1 for yes
// and 0 for no, as a camera does that fbe_camera_set_stored_values is set on: its color goes
// into the target's bytes as it's given, what it blends with are those bytes, so half white over
// black is 128, not 188, and an sRGB texture's texels, decoded as they're sampled and filtered by
// their light, are encoded again before the pen's color multiplies them; a draw with it into an
// RGBA16F target is refused. C has no `using`, so it's a typedef.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct fbe_2d_pen_t {
    int32_t origin_x;
    int32_t origin_y;
    int32_t viewport_x;
    int32_t viewport_y;
    int32_t viewport_width;
    int32_t viewport_height;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
    int32_t blend;
    int32_t stored;
} fbe_2d_pen_t;

// How a texture's drawn: the part of it, in its pixels from its top left, 0 by 0 for all of it;
// its handle, the point of that part, in its pixels, that lands at the position drawn at, which
// it's rotated and scaled about; its rotation, in radians, clockwise on the screen; and its scale
// across and down, a negative one mirroring it; and whether a texel of no alpha leaves what's there
// whatever the pen's blend, as a key color's do, 1 for yes and 0 for no. C has no `using`, so it's
// a typedef. NOLINTNEXTLINE(modernize-use-using)
typedef struct fbe_2d_sprite_t {
    float region_x;
    float region_y;
    float region_width;
    float region_height;
    float handle_x;
    float handle_y;
    float rotation;
    float scale_x;
    float scale_y;
    int32_t keyed;
} fbe_2d_sprite_t;

// Each draws into `target`, a texture that's a target or 0 for the screen, with `pen`; false if the
// target isn't a live target, there's no screen, or the pen's viewport or blend isn't one.
// Positions are in pixels from the target's top left, a pixel covering from its whole coordinates
// to the next.

// Fills the viewport with the pen's color, replacing what's there, whatever its blend.
FBE_API bool fbe_2d_clear(uint64_t target, const fbe_2d_pen_t* pen);
// A pixel.
FBE_API bool fbe_2d_plot(uint64_t target, const fbe_2d_pen_t* pen, int32_t x, int32_t y);
// A line from one pixel to another, both drawn: in each column, or each row where it's steeper,
// the pixel nearest it, the same whichever end it starts from.
FBE_API bool fbe_2d_line(uint64_t target, const fbe_2d_pen_t* pen, int32_t x1, int32_t y1,
                         int32_t x2, int32_t y2);
// A rectangle `width` by `height` pixels with its top left at `x`, `y`, filled, or its outline a
// pixel wide, each pixel drawn once; nothing if a side isn't positive.
FBE_API bool fbe_2d_rect(uint64_t target, const fbe_2d_pen_t* pen, int32_t x, int32_t y,
                         int32_t width, int32_t height, bool filled);
// The ellipse that fills that rectangle, filled, or its outline, each row's ends joined to the
// next's; symmetric across both its middles.
FBE_API bool fbe_2d_oval(uint64_t target, const fbe_2d_pen_t* pen, int32_t x, int32_t y,
                         int32_t width, int32_t height, bool filled);
// A polygon through `count` points, `points` their x and y in turn, each taken to the nearest 1/16
// of a pixel, which every GPU draws exactly: filled, covering the pixels whose middles are in it,
// concave or not as long as its sides don't cross, though they can touch at a point, a middle on
// a side, or within 1/1000 of a pixel of one, as the GPU's rule says (all of it cut to the
// viewport first where any of it reaches more than 16384 pixels past it, the cuts as exact as the
// GPU draws them); or its outline, a line between the pixels the points are in, each to the next
// and the last to the first, each pixel drawn once. False for fewer than 3 points, or a point that
// isn't finite.
FBE_API bool fbe_2d_polygon(uint64_t target, const fbe_2d_pen_t* pen, const float* points,
                            int32_t count, bool filled);
// `texture` as `sprite` says, its handle at `x`, `y` (the pen's origin added), sampled as the
// texture's filter says and clamped at its edges, times the pen's color, which tints it and its
// alpha fades it, a part's texels kept half a texel in from its edges so filtering never reaches
// its neighbors'; unrotated and at its own size at whole pixels, each pixel is one of its texels
// exactly. False also if the texture isn't live or is the target, or the sprite's part isn't in it,
// or the position or its handle, rotation, or scale isn't finite.
FBE_API bool fbe_2d_texture(uint64_t target, const fbe_2d_pen_t* pen, uint64_t texture,
                            const fbe_2d_sprite_t* sprite, float x, float y);
// UTF-8 `text`, which ends at its first NUL, in `font`, an fbe_text.h font, as it's laid out, its
// top left at `x`, `y` (the pen's origin added), its lines as fbe_text_measure breaks them, and an
// underlined font's line under each line: each glyph's pixels at whole pixels, crisp, rasterized at
// the target's pixels a unit of the font's size (the screen's render size over the window's, a
// texture's 1), and past 512 pixels an em drawn scaled from 512, smooth; the pen's color times
// their coverage, or a color glyph's own colors faded by its alpha, over what's there unless the
// pen adds. False also as laying it out is, for a font that isn't one.
FBE_API bool fbe_2d_text(uint64_t target, const fbe_2d_pen_t* pen, uint64_t font, const char* text,
                         int32_t x, int32_t y);
// The viewport covered with `texture` as `sprite` says, unrotated and scaled, tiles placed so one's
// handle is at `x`, `y` (the pen's origin added), in one quad: all of a texture repeats as its
// sampler wraps, and a part of one as the shader wraps it into the part. False also as
// fbe_2d_texture is, or for a scale of 0.
FBE_API bool fbe_2d_tile(uint64_t target, const fbe_2d_pen_t* pen, uint64_t texture,
                         const fbe_2d_sprite_t* sprite, float x, float y);

#ifdef __cplusplus
}
#endif

#endif
