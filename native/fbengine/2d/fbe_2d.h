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
// its color, sRGB bytes; and how it blends, an FBE_2D_ blend. C has no `using`, so it's a typedef.
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
} fbe_2d_pen_t;

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
// A polygon through `count` points, `points` their x and y in turn: filled, covering the pixels
// whose middles are in it, concave or not as long as its sides don't cross; or its outline, a line
// between the pixels the points are in, each to the next and the last to the first, each pixel
// drawn once. False for fewer than 3 points, or a point that isn't finite.
FBE_API bool fbe_2d_polygon(uint64_t target, const fbe_2d_pen_t* pen, const float* points,
                            int32_t count, bool filled);

#ifdef __cplusplus
}
#endif

#endif
