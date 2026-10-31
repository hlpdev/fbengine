#pragma once

// What 2D's text draws through the canvas: the glyphs of an atlas page, its texture's pixels at
// whole pixels of the target, in the batch with whatever else is drawn into it.

#include "2d/fbe_2d.h"

#include <cstdint>
#include <span>

namespace fbe::canvas {

/// A glyph drawn: its pixels on its page, `source_width` by `source_height` from `source_x`,
/// `source_y`, and the quad they're drawn in, `width` by `height` at `x`, `y`, in the target's
/// pixels before the pen's origin's added.
struct glyph_t {
    std::int32_t source_x = 0;
    std::int32_t source_y = 0;
    std::int32_t source_width = 0;
    std::int32_t source_height = 0;
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

/// Draws `glyphs` of `page`, a texture, into `target`, a target or 0 for the screen, with `pen`:
/// a glyph whose quad is its source's size each texel a pixel, sampled NEAREST, and a bigger one
/// scaled, sampled LINEAR; a page of coverage, R8, as the pen's color times it, and one of color,
/// as its texels times the pen's alpha. They're drawn over what's there, as their edges are their
/// coverage, unless the pen adds. False, with fbe_error's why, as the canvas's draws are, or for a
/// glyph whose pixels aren't on its page.
bool draw_glyphs(std::uint64_t target, const fbe_2d_pen_t* pen, std::uint64_t page, bool coverage,
                 std::span<const glyph_t> glyphs);

/// Trims the text atlas, as a frame's first canvas draw does, if this frame's hasn't yet.
void trim_atlas();

} // namespace fbe::canvas
