// The pixels of 2D's shapes, on the CPU in whole numbers, so every GPU draws the same ones.

#include "2d/shapes.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>

namespace {

using fbe::canvas::clip_t;
using fbe::canvas::span_t;

/// `top` / `bottom`, rounded down, `bottom` positive.
std::int64_t floor_divide(std::int64_t top, std::int64_t bottom) {
    const std::int64_t quotient = top / bottom;
    return (top % bottom != 0 && top < 0) ? quotient - 1 : quotient;
}

/// Adds the part of `span` in `clip`, if any of it is.
void add(span_t span, const clip_t& clip, std::vector<span_t>& spans) {
    const std::int64_t left = std::max(span.x, clip.left);
    const std::int64_t top = std::max(span.y, clip.top);
    const std::int64_t right = std::min(span.x + span.width, clip.right);
    const std::int64_t bottom = std::min(span.y + span.height, clip.bottom);
    if (left < right && top < bottom) {
        spans.push_back({ .x = left, .y = top, .width = right - left, .height = bottom - top });
    }
}

/// An oval's row `row` of `height`: where it starts across, from the oval's left, and how wide it
/// is, 0 where no pixel's middle is in it.
std::pair<std::int64_t, std::int64_t> row_of(std::int64_t width, std::int64_t height,
                                             std::int64_t row) {
    // How far the row's middle is from the oval's, as a share of its half height.
    const double across = static_cast<double>((2 * row) + 1 - height) / static_cast<double>(height);
    const double half = (static_cast<double>(width) / 2.0) * std::sqrt(1.0 - (across * across));
    // The first pixel whose middle is further right than the left edge, and its mirror.
    const auto left =
        static_cast<std::int64_t>(std::floor((static_cast<double>(width) / 2.0) - half + 0.5));
    return { left, std::max<std::int64_t>(width - (2 * left), 0) };
}

} // namespace

namespace fbe::canvas {

void line(std::int64_t x1, std::int64_t y1, std::int64_t x2, std::int64_t y2, bool last,
          const clip_t& clip, std::vector<span_t>& spans) {
    // Along the longer axis, from the end where it's smaller: m is that axis, n the other.
    const bool across = std::abs(x2 - x1) >= std::abs(y2 - y1);
    std::int64_t m1 = across ? x1 : y1;
    std::int64_t n1 = across ? y1 : x1;
    std::int64_t m2 = across ? x2 : y2;
    std::int64_t n2 = across ? y2 : x2;
    // The end left out, if one is, wherever it ends up.
    const std::int64_t skipped = last ? INT64_MIN : m2;
    if (m2 < m1) {
        std::swap(m1, m2);
        std::swap(n1, n2);
    }
    const std::int64_t length = m2 - m1;
    const clip_t along = across ? clip
                                : clip_t{
                                      .left = clip.top,
                                      .top = clip.left,
                                      .right = clip.bottom,
                                      .bottom = clip.right,
                                  };
    const std::int64_t first = std::max(m1, along.left);
    const std::int64_t end = std::min(m2 + 1, along.right);
    // The pixel nearest n1 + (n2 - n1) * t / length, a tie rounding up.
    const auto at = [&](std::int64_t m) {
        if (length == 0) {
            return n1;
        }
        return floor_divide((2 * n1 * length) + (2 * (n2 - n1) * (m - m1)) + length, 2 * length);
    };
    const auto emit = [&](std::int64_t from, std::int64_t to, std::int64_t n) {
        if (n < along.top || n >= along.bottom || from >= to) {
            return;
        }
        spans.push_back(across ? span_t{ .x = from, .y = n, .width = to - from, .height = 1 }
                               : span_t{ .x = n, .y = from, .width = 1, .height = to - from });
    };
    std::int64_t run = first;
    std::int64_t run_n = 0;
    for (std::int64_t m = first; m < end; ++m) {
        const std::int64_t n = at(m);
        if (m == first) {
            run_n = n;
        } else if (n != run_n) {
            emit(run, m, run_n);
            run = m;
            run_n = n;
        }
        if (m == skipped) {
            emit(run, m, run_n);
            run = m + 1;
        }
    }
    if (first < end) {
        emit(run, end, run_n);
    }
}

void rect_outline(std::int64_t x, std::int64_t y, std::int64_t width, std::int64_t height,
                  std::vector<span_t>& spans) {
    if (width <= 0 || height <= 0) {
        return;
    }
    spans.push_back({ .x = x, .y = y, .width = width, .height = 1 });
    if (height > 1) {
        spans.push_back({ .x = x, .y = y + height - 1, .width = width, .height = 1 });
    }
    if (height > 2) {
        spans.push_back({ .x = x, .y = y + 1, .width = 1, .height = height - 2 });
        if (width > 1) {
            spans.push_back({ .x = x + width - 1, .y = y + 1, .width = 1, .height = height - 2 });
        }
    }
}

void oval(std::int64_t x, std::int64_t y, std::int64_t width, std::int64_t height, bool filled,
          const clip_t& clip, std::vector<span_t>& spans) {
    if (width <= 0 || height <= 0) {
        return;
    }
    const std::int64_t first = std::max<std::int64_t>(clip.top - y, 0);
    const std::int64_t end = std::min(clip.bottom - y, height);
    for (std::int64_t row = first; row < end; ++row) {
        const auto [left, across] = row_of(width, height, row);
        if (across == 0) {
            continue;
        }
        if (filled) {
            add({ .x = x + left, .y = y + row, .width = across, .height = 1 }, clip, spans);
            continue;
        }
        // The row toward the nearer of the top and bottom, whose ends this row's reach to; the
        // first and last rows, and those whose neighbor has no pixels, are all outline.
        const std::int64_t toward = (2 * row) + 1 <= height ? row - 1 : row + 1;
        const std::int64_t next =
            toward >= 0 && toward < height ? row_of(width, height, toward).first : left;
        const bool edge =
            toward < 0 || toward >= height || row_of(width, height, toward).second == 0;
        const std::int64_t end_reach = std::max(next, left + 1) - left;
        if (edge || 2 * end_reach >= across) {
            add({ .x = x + left, .y = y + row, .width = across, .height = 1 }, clip, spans);
            continue;
        }
        add({ .x = x + left, .y = y + row, .width = end_reach, .height = 1 }, clip, spans);
        add({ .x = x + left + across - end_reach, .y = y + row, .width = end_reach, .height = 1 },
            clip, spans);
    }
}

std::vector<std::array<float, 6>> triangulate(std::span<const float> points) {
    const std::size_t count = points.size() / 2;
    std::vector<std::array<float, 6>> triangles;
    if (count < 3) {
        return triangles;
    }
    const auto x = [points](std::size_t at) { return points[at * 2]; };
    const auto y = [points](std::size_t at) { return points[(at * 2) + 1]; };
    // Twice the polygon's area, positive when its points turn clockwise on a y-down screen.
    double area = 0.0;
    for (std::size_t at = 0; at < count; ++at) {
        const std::size_t next = (at + 1) % count;
        area += (static_cast<double>(x(at)) * y(next)) - (static_cast<double>(x(next)) * y(at));
    }
    const double turn = area >= 0.0 ? 1.0 : -1.0;
    const auto cross = [&](std::size_t a, std::size_t b, std::size_t c) {
        return (((static_cast<double>(x(b)) - x(a)) * (static_cast<double>(y(c)) - y(a))) -
                ((static_cast<double>(y(b)) - y(a)) * (static_cast<double>(x(c)) - x(a)))) *
               turn;
    };
    const auto triangle = [&](std::size_t a, std::size_t b, std::size_t c) {
        triangles.push_back({ x(a), y(a), x(b), y(b), x(c), y(c) });
    };
    std::vector<std::size_t> left(count);
    std::ranges::iota(left, std::size_t{ 0 });
    while (left.size() > 3) {
        bool cut = false;
        for (std::size_t at = 0; at < left.size() && !cut; ++at) {
            const std::size_t a = left[(at + left.size() - 1) % left.size()];
            const std::size_t b = left[at];
            const std::size_t c = left[(at + 1) % left.size()];
            if (cross(a, b, c) <= 0.0) {
                continue;
            }
            // An ear has no other point inside it.
            const bool empty = std::ranges::none_of(left, [&](std::size_t p) {
                return p != a && p != b && p != c && cross(a, b, p) > 0.0 && cross(b, c, p) > 0.0 &&
                       cross(c, a, p) > 0.0;
            });
            if (empty) {
                triangle(a, b, c);
                left.erase(left.begin() + static_cast<std::ptrdiff_t>(at));
                cut = true;
            }
        }
        if (!cut) {
            break;
        }
    }
    for (std::size_t at = 1; at + 1 < left.size(); ++at) {
        triangle(left[0], left[at], left[at + 1]);
    }
    return triangles;
}

} // namespace fbe::canvas
