// The pixels of 2D's shapes, on the CPU in whole numbers, so every GPU draws the same ones.

#include "2d/shapes.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>

namespace {

using fbe::canvas::clip_t;
using fbe::canvas::span_t;

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
    // The pixel nearest n1 + (n2 - n1) * t / length at t along, a tie rounding up: n1 and the
    // quotient of (2 (n2 - n1) t + length) / (2 length), stepped along with its remainder, so
    // nothing overflows. Its first is guessed in doubles, a few out at most, and its remainder
    // worked out in 64 bits wrapping, which is exact as the remainder's small, then put right.
    const std::int64_t rise = n2 - n1;
    const std::int64_t twice = 2 * std::max<std::int64_t>(length, 1);
    const std::int64_t start = std::clamp<std::int64_t>(first - m1, 0, length);
    auto quotient = static_cast<std::int64_t>(
        std::floor(((2.0 * static_cast<double>(rise) * static_cast<double>(start)) +
                    static_cast<double>(length)) /
                   static_cast<double>(twice)));
    const auto wrapped = [](std::int64_t value) { return static_cast<std::uint64_t>(value); };
    auto remainder =
        static_cast<std::int64_t>((2 * wrapped(rise) * wrapped(start)) + wrapped(length) -
                                  (wrapped(twice) * wrapped(quotient)));
    while (remainder < 0) {
        --quotient;
        remainder += twice;
    }
    while (remainder >= twice) {
        ++quotient;
        remainder -= twice;
    }
    const auto step = [&] {
        remainder += 2 * rise;
        if (remainder >= twice) {
            ++quotient;
            remainder -= twice;
        } else if (remainder < 0) {
            --quotient;
            remainder += twice;
        }
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
        if (m != first) {
            step();
        }
        const std::int64_t n = n1 + quotient;
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

std::vector<span_t> merged(std::span<const span_t> spans) {
    // Each span's rows, merged across where they overlap or touch.
    std::vector<span_t> rows;
    for (const span_t& span : spans) {
        for (std::int64_t y = span.y; y < span.y + span.height; ++y) {
            rows.push_back({ .x = span.x, .y = y, .width = span.width, .height = 1 });
        }
    }
    std::ranges::sort(rows, {}, [](const span_t& row) { return std::pair{ row.y, row.x }; });
    std::vector<span_t> across;
    for (const span_t& row : rows) {
        span_t* last = across.empty() ? nullptr : &across.back();
        if (last != nullptr && last->y == row.y && row.x <= last->x + last->width) {
            last->width = std::max(last->x + last->width, row.x + row.width) - last->x;
        } else {
            across.push_back(row);
        }
    }
    // Then runs the same across, in rows one after another, merged down.
    std::ranges::sort(across, {},
                      [](const span_t& run) { return std::tuple{ run.x, run.width, run.y }; });
    std::vector<span_t> down;
    for (const span_t& run : across) {
        span_t* last = down.empty() ? nullptr : &down.back();
        if (last != nullptr && last->x == run.x && last->width == run.width &&
            last->y + last->height == run.y) {
            ++last->height;
        } else {
            down.push_back(run);
        }
    }
    return down;
}

std::vector<std::array<double, 6>> triangulate(std::span<const double> points) {
    std::vector<std::array<double, 2>> ring;
    for (std::size_t at = 0; at + 1 < points.size(); at += 2) {
        const std::array<double, 2> point{ points[at], points[at + 1] };
        if (ring.empty() || ring.back() != point) {
            ring.push_back(point);
        }
    }
    while (ring.size() > 1 && ring.back() == ring.front()) {
        ring.pop_back();
    }
    std::vector<std::array<double, 6>> triangles;
    const std::size_t count = ring.size();
    if (count < 3) {
        return triangles;
    }
    // Twice the polygon's area, positive when its points turn clockwise on a y-down screen.
    double area = 0.0;
    for (std::size_t at = 0; at < count; ++at) {
        const std::array<double, 2>& to = ring[(at + 1) % count];
        area += (ring[at][0] * to[1]) - (to[0] * ring[at][1]);
    }
    const double turn = area >= 0.0 ? 1.0 : -1.0;
    const auto cross = [&](std::size_t a, std::size_t b, std::size_t c) {
        return (((ring[b][0] - ring[a][0]) * (ring[c][1] - ring[a][1])) -
                ((ring[b][1] - ring[a][1]) * (ring[c][0] - ring[a][0]))) *
               turn;
    };
    // The points left, a ring of each one's next and previous.
    std::vector<std::size_t> next(count);
    std::vector<std::size_t> previous(count);
    for (std::size_t at = 0; at < count; ++at) {
        next[at] = (at + 1) % count;
        previous[at] = (at + count - 1) % count;
    }
    // Whether each point's corner isn't convex, and those points, the only ones an ear can have
    // in it as long as the sides don't cross; and whether it's been cut off.
    std::vector<std::uint8_t> reflex(count, 0);
    std::vector<std::uint8_t> cut(count, 0);
    std::vector<std::size_t> reflexes;
    const auto bend = [&](std::size_t at) {
        const bool was = reflex[at] != 0;
        reflex[at] = cross(previous[at], at, next[at]) <= 0.0 ? 1 : 0;
        if (reflex[at] != 0 && !was) {
            reflexes.push_back(at);
        }
    };
    for (std::size_t at = 0; at < count; ++at) {
        bend(at);
    }
    // An ear: a convex corner with no other point in it or on its sides, but at its own corners.
    const auto ear = [&](std::size_t b) {
        const std::size_t a = previous[b];
        const std::size_t c = next[b];
        return reflex[b] == 0 && std::ranges::none_of(reflexes, [&](std::size_t p) {
                   return cut[p] == 0 && reflex[p] != 0 && ring[p] != ring[a] &&
                          ring[p] != ring[b] && ring[p] != ring[c] && cross(a, b, p) >= 0.0 &&
                          cross(b, c, p) >= 0.0 && cross(c, a, p) >= 0.0;
               });
    };
    const auto triangle = [&](std::size_t a, std::size_t b, std::size_t c) {
        triangles.push_back(
            { ring[a][0], ring[a][1], ring[b][0], ring[b][1], ring[c][0], ring[c][1] });
    };
    // Around the ring, cutting off each ear found, until a whole way round finds none.
    std::size_t left = count;
    std::size_t at = 0;
    std::size_t looked = 0;
    while (left > 3 && looked < left) {
        if (!ear(at)) {
            at = next[at];
            ++looked;
            continue;
        }
        const std::size_t a = previous[at];
        const std::size_t c = next[at];
        triangle(a, at, c);
        cut[at] = 1;
        next[a] = c;
        previous[c] = a;
        --left;
        bend(a);
        bend(c);
        at = c;
        looked = 0;
    }
    for (std::size_t from = next[at]; next[from] != at; from = next[from]) {
        triangle(at, from, next[from]);
    }
    return triangles;
}

} // namespace fbe::canvas
