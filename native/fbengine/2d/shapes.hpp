#pragma once

// The pixels 2D's shapes cover, worked out on the CPU so they're exact on every GPU: lines, ovals,
// and outlines as spans of whole pixels, and polygons as triangles the GPU fills.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace fbe::canvas {

/// A run of pixels, `width` by `height` from `x`, `y`, one of them a pixel thick.
struct span_t {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;
};

/// The pixels from `left`, `top` up to `right`, `bottom`, which a shape's spans are clipped to.
struct clip_t {
    std::int64_t left = 0;
    std::int64_t top = 0;
    std::int64_t right = 0;
    std::int64_t bottom = 0;
};

/// The pixels of a line from `x1`, `y1` to `x2`, `y2`, both ends drawn unless `last` is false,
/// which leaves out `x2`, `y2`: along its longer axis, each pixel's the one nearest the line
/// between the ends' middles, a tie going to the one further down or right, so it's the same
/// whichever end it starts from. Only those in `clip` are added to `spans`, as runs along that
/// axis. Its ends are no further than 2^53 from 0.
void line(std::int64_t x1, std::int64_t y1, std::int64_t x2, std::int64_t y2, bool last,
          const clip_t& clip, std::vector<span_t>& spans);

/// The pixels of a rectangle's outline, a pixel wide, each once; none if a side isn't positive.
void rect_outline(std::int64_t x, std::int64_t y, std::int64_t width, std::int64_t height,
                  std::vector<span_t>& spans);

/// The pixels of the ellipse filling the rectangle `width` by `height` at `x`, `y`: each row's,
/// whose middle's in the ellipse, symmetric across both its middles; filled, or its outline, each
/// row's ends reaching to the next row's toward the ellipse's top or bottom. Only rows in `clip`
/// are added.
void oval(std::int64_t x, std::int64_t y, std::int64_t width, std::int64_t height, bool filled,
          const clip_t& clip, std::vector<span_t>& spans);

/// `spans`, which can overlap, as rectangles that cover each of their pixels once.
std::vector<span_t> merged(std::span<const span_t> spans);

/// Triangles covering the polygon through `points`, x and y in turn, as long as its sides don't
/// cross, each point but the one before it: its ears cut off one at a time, or, where that finds
/// none, a fan of what's left.
std::vector<std::array<double, 6>> triangulate(std::span<const double> points);

} // namespace fbe::canvas
