// 2D: shapes drawn exact to the pixel, the viewport and origin, blends, the batch going to the
// frame graph in its turn, what a canvas refuses, and the golden image of them all. Each test
// with the GPU ends by stopping it, which has to leave nothing behind.

#include "golden/golden.hpp"
#include "gpu_testing.hpp"

#include <2d/canvas.hpp>
#include <2d/shapes.hpp>
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fbengine.h>
#include <gpu/graph.hpp>
#include <gpu/vulkan.hpp>
#include <numbers>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using fbe::tests::require;
using fbe::tests::stop_gpu;

namespace {

void start() {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_gpu_start());
}

std::uint64_t target(std::int32_t width, std::int32_t height,
                     std::int32_t format = FBE_TEXTURE_RGBA8) {
    const std::uint64_t made = fbe_texture_create_target(width, height, format, 1, false);
    INFO(fbe_error());
    REQUIRE(made != 0);
    return made;
}

using rgba_t = std::array<std::uint8_t, 4>;

fbe_2d_pen_t pen(rgba_t color, std::int32_t blend = FBE_2D_SOLID) {
    return {
        .red = color[0],
        .green = color[1],
        .blue = color[2],
        .alpha = color[3],
        .blend = blend,
    };
}

constexpr rgba_t black{ 0, 0, 0, 255 };
constexpr rgba_t red{ 255, 0, 0, 255 };
constexpr rgba_t green{ 0, 255, 0, 255 };
constexpr rgba_t blue{ 0, 0, 255, 255 };
constexpr rgba_t white{ 255, 255, 255, 255 };

fbe::golden::frame_t read(std::uint64_t texture) {
    const auto width = static_cast<std::uint32_t>(fbe_texture_width(texture));
    const auto height = static_cast<std::uint32_t>(fbe_texture_height(texture));
    fbe::golden::frame_t frame{ .width = width, .height = height };
    frame.rgba.resize(std::size_t{ width } * height * 4);
    require(fbe_texture_read(texture, 0, frame.rgba.data(),
                             static_cast<std::int64_t>(frame.rgba.size())));
    return frame;
}

rgba_t pixel(const fbe::golden::frame_t& frame, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{ y } * frame.width) + x) * 4;
    return { frame.rgba[at], frame.rgba[at + 1], frame.rgba[at + 2], frame.rgba[at + 3] };
}

/// The pixels a frame has in `color`.
std::set<std::pair<std::uint32_t, std::uint32_t>> pixels_of(const fbe::golden::frame_t& frame,
                                                            rgba_t color) {
    std::set<std::pair<std::uint32_t, std::uint32_t>> found;
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            if (pixel(frame, x, y) == color) {
                found.emplace(x, y);
            }
        }
    }
    return found;
}

/// The pixels spans cover, each as often as it's covered.
std::multiset<std::pair<std::int64_t, std::int64_t>>
covered(const std::vector<fbe::canvas::span_t>& spans) {
    std::multiset<std::pair<std::int64_t, std::int64_t>> found;
    for (const fbe::canvas::span_t& span : spans) {
        for (std::int64_t y = span.y; y < span.y + span.height; ++y) {
            for (std::int64_t x = span.x; x < span.x + span.width; ++x) {
                found.emplace(x, y);
            }
        }
    }
    return found;
}

constexpr fbe::canvas::clip_t everywhere{
    .left = -1000,
    .top = -1000,
    .right = 1000,
    .bottom = 1000,
};

/// sRGB's byte for a linear value, and the linear value of one.
double to_linear(std::uint8_t byte) {
    const double value = byte / 255.0;
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}
int to_srgb(double linear) {
    const double value =
        linear <= 0.0031308 ? linear * 12.92 : (1.055 * std::pow(linear, 1.0 / 2.4)) - 0.055;
    return static_cast<int>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

/// The pixels of a frame, or worked out, as x and y.
using pixels_t = std::set<std::pair<std::uint32_t, std::uint32_t>>;
using point_t = std::array<double, 2>;

/// Whether `x`, `y` is in the polygon through `points`, x and y in turn, even-odd, and whether
/// it's within `near` of one of its sides, where a GPU's own rule says which side it's on.
bool inside(std::span<const double> points, double x, double y, bool& tie, double near = 1e-7) {
    const std::size_t count = points.size() / 2;
    bool in = false;
    tie = false;
    for (std::size_t at = 0; at < count; ++at) {
        const std::size_t before = (at + count - 1) % count;
        const double x1 = points[at * 2];
        const double y1 = points[(at * 2) + 1];
        const double x2 = points[before * 2];
        const double y2 = points[(before * 2) + 1];
        const double across = x2 - x1;
        const double down = y2 - y1;
        const double length = std::hypot(across, down);
        if (length > 0.0) {
            const double off = std::abs((across * (y - y1)) - (down * (x - x1))) / length;
            const double along = (((x - x1) * across) + ((y - y1) * down)) / (length * length);
            tie = tie || (off < near && along >= -1e-12 && along <= 1.0 + 1e-12);
        }
        if ((y1 > y) != (y2 > y) && x < x1 + ((y - y1) * across / down)) {
            in = !in;
        }
    }
    return in;
}

/// The pixels of a target `width` by `height` whose middles are in the polygon through `points`,
/// and those whose middles are on its sides.
std::pair<pixels_t, pixels_t> middles_in(std::span<const double> points, std::uint32_t width,
                                         std::uint32_t height, double near = 1e-7) {
    pixels_t in;
    pixels_t ties;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            bool tie = false;
            if (inside(points, x + 0.5, y + 0.5, tie, near)) {
                in.emplace(x, y);
            }
            if (tie) {
                ties.emplace(x, y);
            }
        }
    }
    return { in, ties };
}

/// How many of a target's pixels `drawn` has wrong, against those whose middles are in the
/// polygon through `points`, those on its sides left out.
int wrong_pixels(const pixels_t& drawn, std::span<const double> points, std::uint32_t width,
                 std::uint32_t height, double near = 1e-7) {
    const auto [in, ties] = middles_in(points, width, height, near);
    int wrong = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            if (!ties.contains({ x, y }) && drawn.contains({ x, y }) != in.contains({ x, y })) {
                ++wrong;
            }
        }
    }
    return wrong;
}

/// Twice the area of the polygon through `points`, x and y in turn.
double twice_area(std::span<const double> points) {
    const std::size_t count = points.size() / 2;
    double area = 0.0;
    for (std::size_t at = 0; at < count; ++at) {
        const std::size_t next = (at + 1) % count;
        area +=
            (points[at * 2] * points[(next * 2) + 1]) - (points[next * 2] * points[(at * 2) + 1]);
    }
    return area;
}

/// Which side of the line from `a` to `b` `c` is: 1, -1, or 0 on it.
int side(const point_t& a, const point_t& b, const point_t& c) {
    const double cross = ((b[0] - a[0]) * (c[1] - a[1])) - ((b[1] - a[1]) * (c[0] - a[0]));
    return static_cast<int>(cross > 0.0) - static_cast<int>(cross < 0.0);
}

/// Whether `c`, on the line through `a` and `b`, is between them.
bool between(const point_t& a, const point_t& b, const point_t& c) {
    return std::min(a[0], b[0]) <= c[0] && c[0] <= std::max(a[0], b[0]) &&
           std::min(a[1], b[1]) <= c[1] && c[1] <= std::max(a[1], b[1]);
}

/// Whether the sides from `a` to `b` and from `c` to `d` meet, touching too.
bool meet(const point_t& a, const point_t& b, const point_t& c, const point_t& d) {
    const int ab_c = side(a, b, c);
    const int ab_d = side(a, b, d);
    const int cd_a = side(c, d, a);
    const int cd_b = side(c, d, b);
    return (ab_c * ab_d < 0 && cd_a * cd_b < 0) || (ab_c == 0 && between(a, b, c)) ||
           (ab_d == 0 && between(a, b, d)) || (cd_a == 0 && between(c, d, a)) ||
           (cd_b == 0 && between(c, d, b));
}

/// Whether the polygon through `points` is simple: no point twice, no two sides meeting but
/// neighbors at their point, and no neighbors folding back along each other.
bool simple(std::span<const double> points) {
    const std::size_t count = points.size() / 2;
    const auto point = [&](std::size_t at) {
        return point_t{ points[(at % count) * 2], points[((at % count) * 2) + 1] };
    };
    for (std::size_t at = 0; at < count; ++at) {
        for (std::size_t other = at + 1; other < count; ++other) {
            if (point(at) == point(other)) {
                return false;
            }
            const bool neighbors = other == at + 1 || (at == 0 && other == count - 1);
            if (neighbors) {
                // The far end of one on the other.
                const std::size_t shared = other == at + 1 ? other : at;
                const point_t from = point(shared + count - 1);
                const point_t to = point(shared + 1);
                if (side(from, point(shared), to) == 0 &&
                    (between(from, point(shared), to) || between(to, point(shared), from))) {
                    return false;
                }
            } else if (meet(point(at), point(at + 1), point(other), point(other + 1))) {
                return false;
            }
        }
    }
    return true;
}

/// `count` simple polygons of 4 to 14 points on a grid `grid` pixels wide, each moved by up to
/// `nudge` of a pixel, from `seed`: their points sorted by angle about one near the grid's middle,
/// none at the same angle, with an area of a pixel or more.
std::vector<std::vector<double>> random_polygons(std::size_t count, std::uint32_t grid,
                                                 double nudge, std::uint32_t seed) {
    std::mt19937 random(seed);
    const double middle_x = (grid / 2.0) + 0.37;
    const double middle_y = (grid / 2.0) + 0.21;
    std::vector<std::vector<double>> made;
    while (made.size() < count) {
        std::vector<std::pair<double, point_t>> around;
        const auto size = static_cast<std::uint32_t>(4 + (random() % 11));
        for (std::uint32_t at = 0; at < size; ++at) {
            const auto x = static_cast<double>(random() % (grid + 1));
            const auto y = static_cast<double>(random() % (grid + 1));
            const double nudge_x = nudge * static_cast<double>(random() % 1024) / 1024.0;
            const double nudge_y = nudge * static_cast<double>(random() % 1024) / 1024.0;
            const point_t point{ x + nudge_x, y + nudge_y };
            around.emplace_back(std::atan2(point[1] - middle_y, point[0] - middle_x), point);
        }
        std::ranges::sort(around);
        const auto same = std::ranges::adjacent_find(
            around, [](const auto& a, const auto& b) { return a.first == b.first; });
        if (same != around.end()) {
            continue;
        }
        std::vector<double> points;
        for (const auto& [angle, point] : around) {
            points.insert(points.end(), { point[0], point[1] });
        }
        if (std::abs(twice_area(points)) >= 2.0 && simple(points)) {
            made.push_back(std::move(points));
        }
    }
    return made;
}

/// Polygons ear clipping finds hard: concave ones, collinear points, a point twice, a hole through
/// a bridge, a comb, a spiral, a point on a diagonal, and some the other way round.
std::vector<std::vector<double>> awkward_polygons() {
    std::vector<std::vector<double>> made{
        { 1, 1, 3, 1, 3, 5, 7, 5, 7, 7, 1, 7 },
        { 2, 2, 8, 2, 14, 2, 20, 2, 20, 9, 20, 20, 11, 20, 2, 20, 2, 11 },
        { 2, 2, 20, 2, 20, 2, 20, 20, 20, 20, 20, 20, 2, 20, 2, 2 },
        // A square with a square hole, a bridge from one's first point to the other's.
        { 2, 2, 40, 2, 40, 40, 2, 40, 2, 2, 12, 12, 12, 30, 30, 30, 30, 12, 12, 12 },
        { 2, 10, 20, 10, 20, 2, 36, 16, 20, 30, 20, 22, 2, 22 },
        { 20, 20, 2, 2, 38, 2, 38, 38, 2, 38 },
        // 28, 36 is on the line from 36, 24 to 20, 48, the first ear's diagonal.
        { 36, 0, 36, 24, 40, 36, 28, 36, 20, 48 },
    };
    std::vector<double> comb;
    for (int tooth = 0; tooth < 8; ++tooth) {
        const double x = 2.0 + (tooth * 5.0);
        comb.insert(comb.end(), { x, 2, x + 2, 2, x + 2, 30, x + 5, 30 });
    }
    comb.insert(comb.end(), { 42, 30, 42, 40, 2, 40 });
    made.push_back(comb);
    std::vector<double> spiral;
    std::vector<double> inner;
    for (int at = 0; at <= 60; ++at) {
        const double angle = at * 0.25;
        const double radius = 4.0 + (at * 0.45);
        spiral.insert(spiral.end(), {
                                        32 + ((radius + 3) * std::cos(angle)),
                                        32 + ((radius + 3) * std::sin(angle)),
                                    });
        inner.insert(inner.end(),
                     { 32 + (radius * std::cos(angle)), 32 + (radius * std::sin(angle)) });
    }
    for (std::size_t at = inner.size(); at >= 2; at -= 2) {
        spiral.insert(spiral.end(), { inner[at - 2], inner[at - 1] });
    }
    made.push_back(spiral);
    for (const std::size_t turned : { std::size_t{ 3 }, std::size_t{ 4 }, std::size_t{ 7 } }) {
        std::vector<double> back;
        for (std::size_t at = made[turned].size(); at >= 2; at -= 2) {
            back.insert(back.end(), { made[turned][at - 2], made[turned][at - 1] });
        }
        made.push_back(back);
    }
    return made;
}

/// `points` as the canvas takes them, floats, and as it draws them, each to the nearest 1/16 of a
/// pixel.
std::pair<std::vector<float>, std::vector<double>> given_and_drawn(std::span<const double> points) {
    std::vector<float> given;
    std::vector<double> drawn;
    for (const double value : points) {
        given.push_back(static_cast<float>(value));
        drawn.push_back(std::round(static_cast<double>(given.back()) * 16.0) / 16.0);
    }
    return { given, drawn };
}

} // namespace

TEST_CASE("a line's pixels are one a column or row, the same from either end", "[gpu]") {
    const std::vector<std::array<std::int64_t, 4>> lines{
        { 0, 0, 9, 3 },  { 0, 0, 3, 9 }, { 5, 5, -4, 2 }, { 2, 7, 8, -1 },  { 0, 0, 6, 6 },
        { 0, 0, 6, -6 }, { 3, 3, 3, 3 }, { 0, 0, 10, 5 }, { -3, 4, 4, -3 }, { 1, 1, 1, 9 },
    };
    for (const auto& [x1, y1, x2, y2] : lines) {
        INFO(x1 << ", " << y1 << " to " << x2 << ", " << y2);
        std::vector<fbe::canvas::span_t> forward;
        std::vector<fbe::canvas::span_t> backward;
        fbe::canvas::line(x1, y1, x2, y2, true, everywhere, forward);
        fbe::canvas::line(x2, y2, x1, y1, true, everywhere, backward);
        const auto pixels = covered(forward);
        CHECK(pixels == covered(backward));
        // Both ends, and one pixel for each step along the longer axis.
        CHECK(pixels.contains({ x1, y1 }));
        CHECK(pixels.contains({ x2, y2 }));
        const std::int64_t length = std::max(std::abs(x2 - x1), std::abs(y2 - y1)) + 1;
        CHECK(std::cmp_equal(pixels.size(), length));
        // Without its last pixel, it's the rest.
        std::vector<fbe::canvas::span_t> shorter;
        fbe::canvas::line(x1, y1, x2, y2, false, everywhere, shorter);
        CHECK(std::cmp_equal(covered(shorter).size(), length - 1));
        CHECK(!covered(shorter).contains({ x2, y2 }));
    }
    // A tie goes down: from 0, 0 to 4, 1 the middle pixel, x 2, is at y 1.
    std::vector<fbe::canvas::span_t> tie;
    fbe::canvas::line(0, 0, 4, 1, true, everywhere, tie);
    CHECK(covered(tie).contains({ 2, 1 }));
    // A clip keeps only what's in it, and a huge line costs only what's in it.
    std::vector<fbe::canvas::span_t> clipped;
    fbe::canvas::line(-2000000000, 0, 2000000000, 0, true, { .right = 10, .bottom = 10 }, clipped);
    CHECK(covered(clipped).size() == 10);
}

// A line's pixels are worked out exactly from its ends however far apart they are, stepped from
// the first in the clip, even where the arithmetic of one pixel's would overflow 64 bits.
TEST_CASE("a line's pixels are exact however long it is", "[gpu]") {
    const std::int64_t far = 4294967296;
    const std::int64_t low = -2147483648;
    // One pixel up from -2^31 over 2^33: below it left of 0, the tie at 0 rounding up.
    std::vector<fbe::canvas::span_t> rising;
    fbe::canvas::line(-far, low, far, low + 1, true,
                      { .left = -10, .top = low - 5, .right = 10, .bottom = low + 5 }, rising);
    std::multiset<std::pair<std::int64_t, std::int64_t>> expected;
    for (std::int64_t x = -10; x < 10; ++x) {
        expected.emplace(x, x < 0 ? low : low + 1);
    }
    CHECK(covered(rising) == expected);
    // Ends 2^41 apart, y = x + 10.
    std::vector<fbe::canvas::span_t> diagonal;
    const std::int64_t further = 1099511627776;
    fbe::canvas::line(-further, 10 - further, further, 10 + further, true,
                      { .right = 64, .bottom = 64 }, diagonal);
    expected.clear();
    for (std::int64_t x = 0; x < 54; ++x) {
        expected.emplace(x, x + 10);
    }
    CHECK(covered(diagonal) == expected);
    // Outside the clip, nothing, however far its ends are.
    for (const auto& [x1, y1, x2, y2] : std::vector<std::array<std::int64_t, 4>>{
             { INT32_MIN, INT32_MIN, INT32_MAX, INT32_MIN },
             { INT32_MIN, INT32_MAX, INT32_MAX, INT32_MAX },
             { INT32_MIN, INT32_MIN, INT32_MIN, INT32_MAX },
             { -2000000000, -1500000000, 2000000000, -1500000000 },
         }) {
        std::vector<fbe::canvas::span_t> spans;
        fbe::canvas::line(x1, y1, x2, y2, true, { .right = 64, .bottom = 64 }, spans);
        CHECK(spans.empty());
    }
    // Against each pixel's own arithmetic, for lines short enough for it, clipped anywhere along
    // them.
    // Seeded, so a failure's the same each run.
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937 random(3);
    const auto coordinate = [&random] {
        return static_cast<std::int64_t>(random() % 2097152) - 1048576;
    };
    for (int at = 0; at < 300; ++at) {
        const std::int64_t x1 = coordinate();
        const std::int64_t y1 = coordinate();
        const std::int64_t x2 = at % 3 == 0 ? x1 + (coordinate() % 64) : coordinate();
        const std::int64_t y2 = coordinate();
        INFO(x1 << ", " << y1 << " to " << x2 << ", " << y2);
        // A clip 64 by 64 about a point along it.
        const std::int64_t middle_x = x1 + ((x2 - x1) * (at % 7) / 7);
        const std::int64_t middle_y = y1 + ((y2 - y1) * (at % 7) / 7);
        const fbe::canvas::clip_t clip{
            .left = middle_x - 32,
            .top = middle_y - 32,
            .right = middle_x + 32,
            .bottom = middle_y + 32,
        };
        std::vector<fbe::canvas::span_t> spans;
        fbe::canvas::line(x1, y1, x2, y2, true, clip, spans);
        // Along its longer axis, from the end where that's smaller, and across.
        const bool across = std::abs(x2 - x1) >= std::abs(y2 - y1);
        std::array<std::int64_t, 2> start = across ? std::array{ x1, y1 } : std::array{ y1, x1 };
        std::array<std::int64_t, 2> finish = across ? std::array{ x2, y2 } : std::array{ y2, x2 };
        if (finish[0] < start[0]) {
            std::swap(start, finish);
        }
        const auto [m1, n1] = start;
        const auto [m2, n2] = finish;
        const std::int64_t length = m2 - m1;
        std::multiset<std::pair<std::int64_t, std::int64_t>> each;
        const std::int64_t from = std::max(m1, across ? clip.left : clip.top);
        const std::int64_t to = std::min(m2, (across ? clip.right : clip.bottom) - 1);
        for (std::int64_t m = from; m <= to; ++m) {
            std::int64_t n = n1;
            if (length > 0) {
                const std::int64_t top = (2 * (n2 - n1) * (m - m1)) + length;
                const std::int64_t bottom = 2 * length;
                n += (top / bottom) - (top % bottom != 0 && top < 0 ? 1 : 0);
            }
            const std::int64_t x = across ? m : n;
            const std::int64_t y = across ? n : m;
            if (x >= clip.left && x < clip.right && y >= clip.top && y < clip.bottom) {
                each.emplace(x, y);
            }
        }
        CHECK(covered(spans) == each);
    }
}

// Ear clipping covers every pixel middle in a polygon once, and none outside it, its triangles'
// area its own: an ear's blocked by a point on its sides too, so a point on a diagonal isn't cut
// past. On the CPU, over awkward polygons and seeded random ones, on a small grid so their points
// are often collinear, and nudged off it.
TEST_CASE("a polygon's triangles cover each pixel middle in it once", "[gpu]") {
    const auto check = [](std::span<const double> points, std::uint32_t size) {
        const std::vector<std::array<double, 6>> triangles = fbe::canvas::triangulate(points);
        double area = 0.0;
        for (const std::array<double, 6>& triangle : triangles) {
            area += std::abs(twice_area(triangle));
        }
        CHECK(std::abs(area - std::abs(twice_area(points))) < 1e-6);
        int twice = 0;
        int wrong = 0;
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                bool tie = false;
                const bool in = inside(points, x + 0.5, y + 0.5, tie);
                int covers = 0;
                for (const std::array<double, 6>& triangle : triangles) {
                    bool on_side = false;
                    covers += inside(triangle, x + 0.5, y + 0.5, on_side) ? 1 : 0;
                    tie = tie || on_side;
                }
                if (!tie) {
                    twice += covers > 1 ? 1 : 0;
                    wrong += (covers > 0) != in ? 1 : 0;
                }
            }
        }
        CHECK(twice == 0);
        CHECK(wrong == 0);
    };
    for (const std::vector<double>& polygon : awkward_polygons()) {
        INFO("an awkward polygon of " << polygon.size() / 2 << " points, from " << polygon[0]
                                      << ", " << polygon[1]);
        check(polygon, 64);
    }
    for (const double nudge : { 0.0, 1.0 }) {
        std::size_t index = 0;
        for (const std::vector<double>& polygon : random_polygons(500, 12, nudge, 7)) {
            INFO("random polygon " << index++ << ", nudged " << nudge);
            check(polygon, 13);
        }
    }
    // Sides that cross promise nothing, but are triangles still.
    const std::vector<double> bowtie{ 0, 0, 10, 10, 10, 0, 0, 10 };
    CHECK(!fbe::canvas::triangulate(bowtie).empty());
}

// Only a corner that isn't convex can be in an ear, so a convex polygon's ears are found going
// round it once: 100,000 points take a moment, where trying each against every other would take
// many seconds.
TEST_CASE("a convex polygon's triangles are found going round it once", "[gpu]") {
    constexpr std::size_t count = 100000;
    std::vector<double> circle;
    for (std::size_t at = 0; at < count; ++at) {
        const double angle = static_cast<double>(at) * 2.0 * std::numbers::pi / count;
        circle.insert(circle.end(),
                      { 500 + (400 * std::cos(angle)), 500 + (400 * std::sin(angle)) });
    }
    const auto started = std::chrono::steady_clock::now();
    const std::vector<std::array<double, 6>> triangles = fbe::canvas::triangulate(circle);
    const std::chrono::duration<double, std::milli> spent =
        std::chrono::steady_clock::now() - started;
    CHECK(triangles.size() == count - 2);
    CHECK(spent.count() < 1000.0);
}

TEST_CASE("an oval's pixels are symmetric, and its outline is its edge's", "[gpu]") {
    for (const auto& [width, height] : std::vector<std::pair<std::int64_t, std::int64_t>>{
             { 1, 1 },
             { 2, 2 },
             { 3, 3 },
             { 8, 5 },
             { 5, 8 },
             { 15, 15 },
             { 16, 9 },
             { 1, 7 },
         }) {
        INFO(width << "x" << height);
        std::vector<fbe::canvas::span_t> filled;
        std::vector<fbe::canvas::span_t> outline;
        fbe::canvas::oval(10, 20, width, height, true, everywhere, filled);
        fbe::canvas::oval(10, 20, width, height, false, everywhere, outline);
        const auto inside = covered(filled);
        const auto edge = covered(outline);
        // Each pixel once, inside the box, the same mirrored across both middles.
        CHECK(std::set(inside.begin(), inside.end()).size() == inside.size());
        CHECK(std::set(edge.begin(), edge.end()).size() == edge.size());
        for (const auto& [x, y] : inside) {
            CHECK((x >= 10 && x < 10 + width && y >= 20 && y < 20 + height));
            CHECK(inside.contains({ std::int64_t{ 20 } + width - 1 - x, y }));
            CHECK(inside.contains({ x, std::int64_t{ 40 } + height - 1 - y }));
        }
        // The outline's in the fill, and has each row's ends.
        for (const auto& point : edge) {
            CHECK(inside.contains(point));
        }
        for (const fbe::canvas::span_t& span : filled) {
            CHECK(edge.contains({ span.x, span.y }));
            CHECK(edge.contains({ span.x + span.width - 1, span.y }));
        }
    }
    // A 1 by 1 oval is its pixel; a 4 by 4 one, the pixels whose middles are inside: rows of 2, 4,
    // 4, and 2.
    std::vector<fbe::canvas::span_t> one;
    fbe::canvas::oval(0, 0, 1, 1, true, everywhere, one);
    CHECK(covered(one) == std::multiset<std::pair<std::int64_t, std::int64_t>>{ { 0, 0 } });
    std::vector<fbe::canvas::span_t> four;
    fbe::canvas::oval(0, 0, 4, 4, true, everywhere, four);
    CHECK(covered(four) == std::multiset<std::pair<std::int64_t, std::int64_t>>{ { 1, 0 },
                                                                                 { 2, 0 },
                                                                                 { 0, 1 },
                                                                                 { 1, 1 },
                                                                                 { 2, 1 },
                                                                                 { 3, 1 },
                                                                                 { 0, 2 },
                                                                                 { 1, 2 },
                                                                                 { 2, 2 },
                                                                                 { 3, 2 },
                                                                                 { 1, 3 },
                                                                                 { 2, 3 } });
}

// A line drawn on the GPU, one quad whose shader keeps the pixels nearest it, has the pixels the
// CPU works out for it, whatever its slope and direction; one too long for the shader is those
// spans.
TEST_CASE("a line on the GPU has the pixels the CPU works out for it", "[gpu]") {
    start();
    const std::uint64_t made = target(64, 64);
    const fbe_2d_pen_t clear = pen(black);
    const fbe_2d_pen_t ink = pen(white);
    for (std::int32_t at = 0; at < 64; ++at) {
        INFO("line " << at);
        require(fbe_2d_clear(made, &clear));
        const std::int32_t x1 = (at * 7) % 64;
        const std::int32_t y1 = (at * 13) % 64;
        const std::int32_t x2 = ((at * 29) + 5) % 64;
        const std::int32_t y2 = ((at * 41) + 3) % 64;
        require(fbe_2d_line(made, &ink, x1, y1, x2, y2));
        std::vector<fbe::canvas::span_t> spans;
        fbe::canvas::line(x1, y1, x2, y2, true, { .right = 64, .bottom = 64 }, spans);
        std::set<std::pair<std::uint32_t, std::uint32_t>> expected;
        for (const auto& [x, y] : covered(spans)) {
            expected.emplace(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        }
        CHECK(pixels_of(read(made), white) == expected);
    }
    // Longer than the shader's arithmetic, it's spans, the pixels the same.
    require(fbe_2d_clear(made, &clear));
    require(fbe_2d_line(made, &ink, -100000, -50000, 100063, 50031));
    std::vector<fbe::canvas::span_t> spans;
    fbe::canvas::line(-100000, -50000, 100063, 50031, true, { .right = 64, .bottom = 64 }, spans);
    CHECK(pixels_of(read(made), white).size() == covered(spans).size());
    CHECK(!covered(spans).empty());
    stop_gpu();
}

// Lines going back across their longer axis, where the shader's arithmetic is negative, and their
// ties, have the CPU's pixels: each line short enough to have a box of its own on one target, and
// long ones each alone. Signed division of negative numbers isn't defined in Vulkan, and lavapipe
// and MoltenVK, which CI runs, do it as C++ does, so only another GPU, as an RTX 4080 in a lane's
// own runs, shows it wrong.
TEST_CASE("a line going back across its longer axis has the CPU's pixels, ties too", "[gpu]") {
    start();
    const fbe_2d_pen_t clear = pen(black);
    const fbe_2d_pen_t ink = pen(white);
    const auto expected_of = [](const std::vector<std::array<std::int32_t, 4>>& lines,
                                std::int64_t width, std::int64_t height) {
        std::vector<fbe::canvas::span_t> spans;
        for (const auto& [x1, y1, x2, y2] : lines) {
            fbe::canvas::line(x1, y1, x2, y2, true, { .right = width, .bottom = height }, spans);
        }
        std::set<std::pair<std::uint32_t, std::uint32_t>> expected;
        for (const auto& [x, y] : covered(spans)) {
            expected.emplace(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        }
        return expected;
    };
    // Every slope back across for lengths 2 to 16, shallow and steep, each in an 18 pixel box.
    std::vector<std::array<std::int32_t, 4>> short_lines;
    for (std::int32_t length = 2; length <= 16; ++length) {
        for (std::int32_t across = -length; across < 0; ++across) {
            for (const bool steep : { false, true }) {
                const auto box = static_cast<std::int32_t>(short_lines.size());
                const std::int32_t x = ((box % 28) * 18) + 1;
                const std::int32_t y = ((box / 28) * 18) + 1;
                short_lines.push_back(steep ? std::array{ x + 16, y, x + 16 + across, y + length }
                                            : std::array{ x, y + 16, x + length, y + 16 + across });
            }
        }
    }
    const std::uint64_t made = target(512, 256);
    require(fbe_2d_clear(made, &clear));
    for (const auto& [x1, y1, x2, y2] : short_lines) {
        require(fbe_2d_line(made, &ink, x1, y1, x2, y2));
    }
    CHECK(pixels_of(read(made), white) == expected_of(short_lines, 512, 256));
    // Long ones, those an RTX 4080 drew wrong among them.
    std::vector<std::array<std::int32_t, 4>> long_lines{
        { 203, 254, 217, 30 },    { 354, 5, 142, 119 },    { 55, -197, -46, 453 },
        { 523, -221, -138, 555 }, { 219, -147, -34, 193 }, { 157, 339, 533, -11 },
        { -3, 438, 434, -258 },   { -298, 421, 294, 185 }, { 46, 153, 433, -241 },
    };
    for (const std::int32_t across : { -1, -3, -99, -100, -101, -150, -199, -200 }) {
        long_lines.push_back({ 10, 240, 210, 240 + across });
        long_lines.push_back({ 250, 10, 250 + across, 210 });
    }
    for (const auto& line : long_lines) {
        INFO(line[0] << ", " << line[1] << " to " << line[2] << ", " << line[3]);
        require(fbe_2d_clear(made, &clear));
        require(fbe_2d_line(made, &ink, line[0], line[1], line[2], line[3]));
        CHECK(pixels_of(read(made), white) == expected_of({ line }, 512, 256));
    }
    stop_gpu();
}

TEST_CASE("a rectangle covers exactly its pixels, and its outline each once", "[gpu]") {
    start();
    const std::uint64_t made = target(16, 12);
    const fbe_2d_pen_t clear = pen(black);
    require(fbe_2d_clear(made, &clear));
    const fbe_2d_pen_t solid = pen(red);
    require(fbe_2d_rect(made, &solid, 2, 3, 5, 4, true));
    std::set<std::pair<std::uint32_t, std::uint32_t>> expected;
    for (std::uint32_t y = 3; y < 7; ++y) {
        for (std::uint32_t x = 2; x < 7; ++x) {
            expected.emplace(x, y);
        }
    }
    CHECK(pixels_of(read(made), red) == expected);
    // Drawn over half transparent, a pixel drawn twice would be brighter than one drawn once.
    require(fbe_2d_clear(made, &clear));
    const fbe_2d_pen_t half = pen({ 255, 255, 255, 128 }, FBE_2D_ALPHA);
    require(fbe_2d_rect(made, &half, 1, 1, 6, 5, false));
    const fbe::golden::frame_t frame = read(made);
    const int once = to_srgb(128.0 / 255.0);
    for (std::uint32_t y = 0; y < 12; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            INFO(x << ", " << y);
            const bool edge =
                x >= 1 && x <= 6 && y >= 1 && y <= 5 && (x == 1 || x == 6 || y == 1 || y == 5);
            CHECK(std::abs(pixel(frame, x, y)[0] - (edge ? once : 0)) <= 1);
        }
    }
    // Nothing for a side that isn't positive.
    require(fbe_2d_rect(made, &solid, 0, 0, 0, 5, true));
    require(fbe_2d_rect(made, &solid, 0, 0, 5, -1, false));
    CHECK(pixels_of(read(made), red).empty());
    stop_gpu();
}

TEST_CASE("the viewport clips, the origin moves, and a clear fills only the viewport", "[gpu]") {
    start();
    const std::uint64_t made = target(12, 12);
    const fbe_2d_pen_t clear = pen(black);
    require(fbe_2d_clear(made, &clear));
    fbe_2d_pen_t moved = pen(green);
    moved.origin_x = 2;
    moved.origin_y = 2;
    moved.viewport_x = 3;
    moved.viewport_y = 3;
    moved.viewport_width = 4;
    moved.viewport_height = 4;
    // At 2, 2 to 5, 5, clipped to 3, 3 to 6, 6: 3 by 3 pixels.
    require(fbe_2d_rect(made, &moved, 0, 0, 4, 4, true));
    std::set<std::pair<std::uint32_t, std::uint32_t>> expected;
    for (std::uint32_t y = 3; y < 6; ++y) {
        for (std::uint32_t x = 3; x < 6; ++x) {
            expected.emplace(x, y);
        }
    }
    CHECK(pixels_of(read(made), green) == expected);
    // A clear fills the viewport, the origin not added.
    fbe_2d_pen_t filling = pen(blue);
    filling.origin_x = 5;
    filling.viewport_x = 8;
    filling.viewport_y = 9;
    filling.viewport_width = 10;
    filling.viewport_height = 10;
    require(fbe_2d_clear(made, &filling));
    CHECK(pixels_of(read(made), blue).size() == std::size_t{ 4 } * 3);
    // A line and an oval are clipped too.
    require(fbe_2d_clear(made, &clear));
    require(fbe_2d_line(made, &moved, -10, 1, 20, 1));
    require(fbe_2d_oval(made, &moved, -6, -6, 20, 20, true));
    for (const auto& [x, y] : pixels_of(read(made), green)) {
        CHECK((x >= 3 && x < 7 && y >= 3 && y < 7));
    }
    stop_gpu();
}

// The blends on an sRGB target: drawn in linear light and stored as sRGB, so each is what the
// colors' linear values make, encoded.
TEST_CASE("a canvas's blends are replacing, over, added, and multiplied", "[gpu]") {
    start();
    const std::uint64_t made = target(4, 1);
    const rgba_t gray{ 128, 128, 128, 255 };
    const fbe_2d_pen_t clear = pen(gray);
    require(fbe_2d_clear(made, &clear));
    const fbe_2d_pen_t solid = pen({ 0, 0, 0, 0 });
    require(fbe_2d_plot(made, &solid, 0, 0));
    const fbe_2d_pen_t over = pen({ 255, 0, 0, 128 }, FBE_2D_ALPHA);
    require(fbe_2d_plot(made, &over, 1, 0));
    const fbe_2d_pen_t added = pen({ 64, 64, 64, 255 }, FBE_2D_ADD);
    require(fbe_2d_plot(made, &added, 2, 0));
    const fbe_2d_pen_t multiplied = pen(gray, FBE_2D_MULTIPLY);
    require(fbe_2d_plot(made, &multiplied, 3, 0));
    const fbe::golden::frame_t frame = read(made);
    const double under = to_linear(128);
    const double alpha = 128.0 / 255.0;
    CHECK(pixel(frame, 0, 0) == rgba_t{ 0, 0, 0, 0 });
    CHECK(std::abs(pixel(frame, 1, 0)[0] - to_srgb((1.0 * alpha) + (under * (1.0 - alpha)))) <= 1);
    CHECK(std::abs(pixel(frame, 1, 0)[1] - to_srgb(under * (1.0 - alpha))) <= 1);
    CHECK(std::abs(pixel(frame, 2, 0)[0] - to_srgb(under + to_linear(64))) <= 1);
    CHECK(std::abs(pixel(frame, 3, 0)[0] - to_srgb(under * under)) <= 1);
    stop_gpu();
}

TEST_CASE("a polygon fills the pixels whose middles are in it, concave too", "[gpu]") {
    start();
    const std::uint64_t made = target(10, 10);
    const fbe_2d_pen_t clear = pen(black);
    require(fbe_2d_clear(made, &clear));
    // An L: 6 high and 2 wide, with a foot 6 wide and 2 high.
    const std::array<float, 12> shape{ 1, 1, 3, 1, 3, 5, 7, 5, 7, 7, 1, 7 };
    const fbe_2d_pen_t solid = pen(white);
    require(fbe_2d_polygon(made, &solid, shape.data(), 6, true));
    std::set<std::pair<std::uint32_t, std::uint32_t>> expected;
    for (std::uint32_t y = 1; y < 7; ++y) {
        for (std::uint32_t x = 1; x < (y < 5 ? 3U : 7U); ++x) {
            expected.emplace(x, y);
        }
    }
    CHECK(pixels_of(read(made), white) == expected);
    // Its outline draws each pixel once.
    require(fbe_2d_clear(made, &clear));
    const fbe_2d_pen_t half = pen({ 255, 255, 255, 128 }, FBE_2D_ALPHA);
    require(fbe_2d_polygon(made, &half, shape.data(), 6, false));
    const fbe::golden::frame_t frame = read(made);
    const int once = to_srgb(128.0 / 255.0);
    std::size_t drawn = 0;
    for (std::uint32_t y = 0; y < 10; ++y) {
        for (std::uint32_t x = 0; x < 10; ++x) {
            const int value = pixel(frame, x, y)[0];
            CHECK((value == 0 || std::abs(value - once) <= 1));
            drawn += value != 0 ? 1 : 0;
        }
    }
    // Its sides, from the pixels its points are in: 2 + 4 + 4 + 2 + 6 + 6, less its 6 corners.
    CHECK(drawn == 2 + 4 + 4 + 2 + 6 + 6);
    stop_gpu();
}

// Filled, a polygon covers the pixels whose middles are in it, its points to 1/16 of a pixel, the
// precision every GPU's rasterizing is exact to; the pixels whose middles are on a side are the
// GPU's to decide. Awkward polygons, seeded random ones off the grid, and a triangle reaching
// further than a GPU's own clipping is exact, which is cut to the viewport first.
TEST_CASE("a polygon fills the pixels whose middles are in it, on every GPU", "[gpu]") {
    start();
    const std::uint64_t made = target(64, 64);
    const fbe_2d_pen_t clear = pen(black);
    const fbe_2d_pen_t ink = pen(white);
    const auto drawn_of = [&](const std::vector<float>& given) {
        require(fbe_2d_clear(made, &clear));
        require(fbe_2d_polygon(made, &ink, given.data(),
                               static_cast<std::int32_t>(given.size() / 2), true));
        return pixels_of(read(made), white);
    };
    std::vector<std::vector<double>> polygons = awkward_polygons();
    for (std::vector<double>& polygon : random_polygons(60, 60, 1.0, 11)) {
        polygons.push_back(std::move(polygon));
    }
    std::size_t index = 0;
    for (const std::vector<double>& polygon : polygons) {
        INFO("polygon " << index++);
        const auto [given, drawn] = given_and_drawn(polygon);
        CHECK(wrong_pixels(drawn_of(given), drawn, 64, 64) == 0);
    }
    // The pentagon with a point on its first ear's diagonal fills its 270 pixels, not 340.
    const auto [in, ties] = middles_in(awkward_polygons()[6], 64, 64);
    CHECK(in.size() == 270);
    // Out to a billion pixels, a triangle whose long side is y = x / 2 + 10.25, as floats hold it.
    for (const double far : { 1e4, 1e5, 1e6, 1e7, 1e8, 1e9 }) {
        INFO("out to " << far);
        const std::vector<double> points{
            -far, (-far / 2) + 10.25, far, (far / 2) + 10.25, -far, far,
        };
        const auto [given, drawn] = given_and_drawn(points);
        CHECK(wrong_pixels(drawn_of(given), drawn, 64, 64, 1e-6) == 0);
    }
    stop_gpu();
}

// An outline draws each of its pixels once, blended: a line from each point's pixel to the
// next's, both drawn, where an acute corner's sides share pixels, a sliver's go back over each
// other, and an outline in one pixel is that pixel; and a side from a point past where doubles
// are whole numbers, where it reaches the target.
TEST_CASE("a polygon's outline draws each of its pixels once", "[gpu]") {
    start();
    const std::uint64_t made = target(48, 24);
    const fbe_2d_pen_t clear = pen(black);
    const fbe_2d_pen_t half = pen({ 255, 255, 255, 128 }, FBE_2D_ALPHA);
    const int once = to_srgb(128.0 / 255.0);
    const auto outline = [&](const std::vector<float>& points) {
        require(fbe_2d_clear(made, &clear));
        require(fbe_2d_polygon(made, &half, points.data(),
                               static_cast<std::int32_t>(points.size() / 2), false));
        const fbe::golden::frame_t frame = read(made);
        std::set<std::pair<std::int64_t, std::int64_t>> drawn;
        for (std::uint32_t y = 0; y < frame.height; ++y) {
            for (std::uint32_t x = 0; x < frame.width; ++x) {
                const int value = pixel(frame, x, y)[0];
                if (value != 0) {
                    INFO(x << ", " << y);
                    CHECK(std::abs(value - once) <= 1);
                    drawn.emplace(x, y);
                }
            }
        }
        return drawn;
    };
    // The pixels of lines through `ends`, x and y in turn, each to the next and the last to the
    // first, together.
    const auto lines = [](const std::vector<std::int64_t>& ends) {
        std::vector<fbe::canvas::span_t> spans;
        for (std::size_t at = 0; at < ends.size(); at += 2) {
            const std::size_t next = (at + 2) % ends.size();
            fbe::canvas::line(ends[at], ends[at + 1], ends[next], ends[next + 1], true,
                              { .right = 48, .bottom = 24 }, spans);
        }
        const auto pixels = covered(spans);
        return std::set(pixels.begin(), pixels.end());
    };
    CHECK(outline({ 0.5F, 0.5F, 40.5F, 4.5F, 0.5F, 8.5F }) == lines({ 0, 0, 40, 4, 0, 8 }));
    CHECK(outline({ 2.5F, 2.5F, 9.5F, 2.5F, 9.6F, 2.6F }).size() == 8);
    CHECK(outline({ 5.1F, 5.2F, 5.8F, 5.3F, 5.5F, 5.9F }) ==
          std::set<std::pair<std::int64_t, std::int64_t>>{ { 5, 5 } });
    // From 1e20 left of the target: its sides along rows 10 and 20 and down column 40, which
    // they reach from 0; from 1e20 right of it, nothing.
    const auto far = outline({ -1e20F, 10.5F, 40.5F, 10.5F, 40.5F, 20.5F });
    CHECK(far.size() == 41 + 41 + 9);
    CHECK(far.contains({ 0, 10 }));
    CHECK(far.contains({ 0, 20 }));
    CHECK(far.contains({ 40, 15 }));
    CHECK(outline({ 1e20F, 10.5F, 50.5F, 10.5F, 50.5F, 20.5F }).empty());
    stop_gpu();
}

// Into a target whose pixels are data, RGBA8_LINEAR's and R8's, a pen's color is stored as it's
// given, not taken as sRGB, and blended as it's stored.
TEST_CASE("a pen's color goes into a target of data as it's given", "[gpu]") {
    start();
    for (const std::int32_t format : { FBE_TEXTURE_RGBA8_LINEAR, FBE_TEXTURE_R8 }) {
        INFO("format " << format);
        const std::uint64_t made = target(2, 1, format);
        const fbe_2d_pen_t ink = pen({ 128, 64, 200, 255 });
        require(fbe_2d_clear(made, &ink));
        const fbe_2d_pen_t half = pen({ 255, 255, 255, 128 }, FBE_2D_ALPHA);
        require(fbe_2d_plot(made, &half, 1, 0));
        const std::size_t channels = format == FBE_TEXTURE_R8 ? 1 : 4;
        std::vector<std::uint8_t> pixels(channels * 2);
        require(fbe_texture_read(made, 0, pixels.data(), static_cast<std::int64_t>(pixels.size())));
        CHECK(pixels[0] == 128);
        // Half of 255 over 128: 191.75.
        CHECK(std::abs(pixels[channels] - 192) <= 1);
        if (channels == 4) {
            CHECK(pixels[1] == 64);
            CHECK(pixels[2] == 200);
            CHECK(std::abs(pixels[5] - 160) <= 1);
        }
    }
    stop_gpu();
}

// An atlas page's glyphs, as text draws them, each texel a pixel: a page of coverage's as the
// pen's color times it, drawn over what's there though the pen's SOLID; a page of color's as its
// texels, faded by the pen's alpha but not tinted; and a glyph whose pixels aren't on its page
// refused.
TEST_CASE("an atlas page's glyphs are drawn as their coverage or their color", "[gpu]") {
    start();
    const fbe_2d_pen_t clear = pen(black);
    const std::uint64_t coverage = fbe_texture_create(4, 1, FBE_TEXTURE_R8, false);
    REQUIRE(coverage != 0);
    const std::array<std::uint8_t, 4> covered{ 0, 64, 128, 255 };
    require(fbe_texture_write(coverage, 0, 0, 4, 1, covered.data(), 4));
    const fbe::canvas::glyph_t row{
        .source_width = 4,
        .source_height = 1,
        .x = 2,
        .y = 1,
        .width = 4,
        .height = 1,
    };
    // Into a target of data, white's stored as given, so each pixel's its coverage.
    const std::uint64_t data = target(8, 2, FBE_TEXTURE_RGBA8_LINEAR);
    require(fbe_2d_clear(data, &clear));
    const fbe_2d_pen_t ink = pen(white);
    REQUIRE(fbe::canvas::draw_glyphs(data, &ink, coverage, true, { &row, 1 }));
    fbe::golden::frame_t frame = read(data);
    for (std::uint32_t x = 0; x < 4; ++x) {
        INFO(x);
        CHECK(std::abs(pixel(frame, 2 + x, 1)[0] - covered.at(x)) <= 1);
    }
    CHECK(pixel(frame, 1, 1)[0] == 0);
    CHECK(pixel(frame, 6, 1)[0] == 0);
    CHECK(pixel(frame, 3, 0)[0] == 0);
    // Into an sRGB target, red at half alpha, over black in linear light.
    const std::uint64_t shown = target(4, 1);
    require(fbe_2d_clear(shown, &clear));
    const fbe_2d_pen_t faded = pen({ 255, 0, 0, 128 });
    const fbe::canvas::glyph_t at_left{
        .source_width = 4,
        .source_height = 1,
        .width = 4,
        .height = 1,
    };
    REQUIRE(fbe::canvas::draw_glyphs(shown, &faded, coverage, true, { &at_left, 1 }));
    frame = read(shown);
    for (std::uint32_t x = 0; x < 4; ++x) {
        INFO(x);
        CHECK(std::abs(pixel(frame, x, 0)[0] - to_srgb(128.0 / 255.0 * covered.at(x) / 255.0)) <=
              1);
        CHECK(pixel(frame, x, 0)[1] == 0);
    }
    // A page of color: its texels as they are, the pen's green not tinting them.
    const std::uint64_t colors = fbe_texture_create(2, 1, FBE_TEXTURE_RGBA8, false);
    REQUIRE(colors != 0);
    const std::array<std::uint8_t, 8> texels{ 200, 100, 50, 255, 10, 20, 30, 255 };
    require(fbe_texture_write(colors, 0, 0, 2, 1, texels.data(), 8));
    const std::uint64_t emoji = target(2, 1);
    require(fbe_2d_clear(emoji, &clear));
    const fbe_2d_pen_t green_ink = pen(green);
    const fbe::canvas::glyph_t both{
        .source_width = 2,
        .source_height = 1,
        .width = 2,
        .height = 1,
    };
    REQUIRE(fbe::canvas::draw_glyphs(emoji, &green_ink, colors, false, { &both, 1 }));
    frame = read(emoji);
    CHECK(pixel(frame, 0, 0) == rgba_t{ 200, 100, 50, 255 });
    CHECK(pixel(frame, 1, 0) == rgba_t{ 10, 20, 30, 255 });
    // A glyph drawn bigger than its pixels, as one past 512 pixels an em is, is scaled LINEAR:
    // 0 and 255 across 4 pixels are 0, a quarter, three quarters, and 255.
    const std::array<std::uint8_t, 2> edge{ 0, 255 };
    const std::uint64_t ramp = fbe_texture_create(2, 1, FBE_TEXTURE_R8, false);
    REQUIRE(ramp != 0);
    require(fbe_texture_write(ramp, 0, 0, 2, 1, edge.data(), 2));
    const std::uint64_t scaled = target(4, 1, FBE_TEXTURE_RGBA8_LINEAR);
    require(fbe_2d_clear(scaled, &clear));
    const fbe::canvas::glyph_t doubled{
        .source_width = 2,
        .source_height = 1,
        .width = 4,
        .height = 1,
    };
    REQUIRE(fbe::canvas::draw_glyphs(scaled, &ink, ramp, true, { &doubled, 1 }));
    frame = read(scaled);
    const std::array<int, 4> smooth{ 0, 64, 191, 255 };
    for (std::uint32_t x = 0; x < 4; ++x) {
        INFO(x);
        CHECK(std::abs(pixel(frame, x, 0)[0] - smooth.at(x)) <= 2);
    }
    // Off its page.
    const fbe::canvas::glyph_t off{
        .source_x = 3,
        .source_width = 2,
        .source_height = 1,
        .width = 2,
        .height = 1,
    };
    CHECK(!fbe::canvas::draw_glyphs(shown, &ink, coverage, true, { &off, 1 }));
    CHECK(std::string(fbe_error()) == "a glyph's pixels aren't on its page");
    stop_gpu();
}

// Everything drawn into one target goes to the GPU as one pass, and it keeps its turn with
// clears, writes, and other targets' drawing.
TEST_CASE("what's drawn into a target is one pass, in its turn", "[gpu]") {
    start();
    const std::uint64_t first = target(32, 32);
    const std::uint64_t second = target(32, 32);
    const fbe_2d_pen_t solid = pen(red);
    for (std::int32_t at = 0; at < 1000; ++at) {
        require(fbe_2d_rect(first, &solid, at % 32, (at / 32) % 32, 1, 1, true));
    }
    const fbe_2d_pen_t over = pen(green, FBE_2D_ALPHA);
    require(fbe_2d_line(first, &over, 0, 0, 31, 31));
    require(fbe::gpu::finish_frame());
    CHECK(fbe::gpu::last_recorded().passes == std::vector<std::string_view>{ "2d" });
    require(fbe_2d_rect(first, &solid, 0, 0, 4, 4, true));
    require(fbe_texture_clear(first, 0.0F, 0.0F, 1.0F, 1.0F));
    require(fbe_2d_rect(second, &solid, 0, 0, 4, 4, true));
    require(fbe_2d_rect(first, &solid, 8, 8, 4, 4, true));
    const std::vector<std::uint8_t> written(std::size_t{ 4 } * 4 * 4, 255);
    require(fbe_texture_write(first, 8, 8, 4, 4, written.data(), 16));
    require(fbe::gpu::finish_frame());
    CHECK(fbe::gpu::last_recorded().passes ==
          std::vector<std::string_view>{ "2d", "clear", "2d", "2d", "write" });
    const fbe::golden::frame_t frame = read(first);
    CHECK(pixel(frame, 1, 1) == blue);
    CHECK(pixel(frame, 9, 9) == white);
    CHECK(pixel(read(second), 1, 1) == red);
    stop_gpu();
}

// Drawing again with the same pen skips checking it, but not which texture the screen is: one made
// again at a new render size, mid-frame, is drawn into from then on.
TEST_CASE("the screen's drawn into as it is, made again mid-frame", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_window_open(32, 16, FBE_WINDOW_WINDOWED));
    require(fbe_gpu_start());
    const fbe_2d_pen_t ink = pen(green);
    const std::uint64_t first = fbe_gpu_screen();
    require(fbe_2d_rect(0, &ink, 0, 0, 4, 4, true));
    require(fbe_gpu_set_render_size(16, 8));
    const std::uint64_t second = fbe_gpu_screen();
    REQUIRE(second != first);
    require(fbe_2d_rect(0, &ink, 0, 0, 4, 4, true));
    CHECK(pixel(read(second), 1, 1) == green);
    require(fbe_gpu_set_render_size(0, 0));
    fbe_window_close();
    stop_gpu();
}

TEST_CASE("a canvas refuses what it can't draw into or with", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    const fbe_2d_pen_t solid = pen(red);
    CHECK(!fbe_2d_rect(0, &solid, 0, 0, 1, 1, true));
    CHECK(std::string(fbe_error()) == "there's no screen, as there's no window");
    require(fbe_gpu_start());
    const std::uint64_t plain = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, false);
    REQUIRE(plain != 0);
    CHECK(!fbe_2d_plot(plain, &solid, 0, 0));
    CHECK(std::string(fbe_error()) == "the texture isn't a target, which draws");
    const std::uint64_t made = target(4, 4);
    const fbe_2d_pen_t wrong = pen(red, 7);
    CHECK(!fbe_2d_plot(made, &wrong, 0, 0));
    CHECK(std::string(fbe_error()) == "7 isn't a Blend");
    fbe_2d_pen_t outside = pen(red);
    outside.viewport_width = -1;
    CHECK(!fbe_2d_plot(made, &outside, 0, 0));
    CHECK(!fbe_2d_plot(made, nullptr, 0, 0));
    const std::array<float, 4> two{ 0, 0, 1, 1 };
    CHECK(!fbe_2d_polygon(made, &solid, two.data(), 2, true));
    CHECK(std::string(fbe_error()) == "a polygon has 3 points or more, not 2");
    const std::array<float, 6> broken{ 0, 0, 1, NAN, 2, 2 };
    CHECK(!fbe_2d_polygon(made, &solid, broken.data(), 3, true));
    CHECK(std::string(fbe_error()) == "a polygon's points are finite");
    // A target freed before its batch goes to the graph is drawn into no more, and drawing into
    // it again, with the pen the batch is ready for, is refused.
    require(fbe_2d_rect(made, &solid, 0, 0, 2, 2, true));
    REQUIRE(fbe_texture_release(made));
    CHECK(!fbe_2d_rect(made, &solid, 0, 0, 2, 2, true));
    CHECK(std::string(fbe_error()) == "the texture was freed");
    require(fbe::gpu::finish_frame());
    stop_gpu();
}

// E3's canvas: lines every way, rectangles and their outlines, ovals odd and even, a concave
// polygon and an outline one, a blended square over them, and shapes clipped and moved, drawn into
// the screen, which Flip shows in the window.
TEST_CASE("a canvas's shapes match their golden image", "[gpu][golden]") {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_window_open(96, 64, FBE_WINDOW_WINDOWED));
    require(fbe_gpu_start());
    fbe_2d_pen_t ink = pen({ 20, 24, 40, 255 });
    require(fbe_2d_clear(0, &ink));
    ink = pen({ 255, 200, 40, 255 });
    for (std::int32_t at = 0; at < 8; ++at) {
        const double angle = at * std::numbers::pi / 4.0;
        require(fbe_2d_line(0, &ink, 16, 16,
                            16 + static_cast<std::int32_t>(std::lround(13 * std::cos(angle))),
                            16 + static_cast<std::int32_t>(std::lround(13 * std::sin(angle)))));
    }
    ink = pen({ 80, 200, 120, 255 });
    require(fbe_2d_rect(0, &ink, 34, 4, 12, 8, true));
    require(fbe_2d_rect(0, &ink, 34, 16, 12, 9, false));
    ink = pen({ 120, 160, 255, 255 });
    require(fbe_2d_oval(0, &ink, 50, 4, 15, 9, true));
    require(fbe_2d_oval(0, &ink, 50, 16, 16, 10, false));
    require(fbe_2d_oval(0, &ink, 70, 4, 21, 21, false));
    ink = pen({ 240, 90, 90, 255 });
    const std::array<float, 14> star{ 10, 34, 18, 44, 30, 36, 24, 50, 30, 60, 12, 54, 4, 58 };
    require(fbe_2d_polygon(0, &ink, star.data(), 7, true));
    ink = pen({ 250, 250, 250, 255 });
    require(fbe_2d_polygon(0, &ink, star.data(), 7, false));
    const fbe_2d_pen_t glass = pen({ 255, 255, 255, 96 }, FBE_2D_ALPHA);
    require(fbe_2d_rect(0, &glass, 40, 30, 20, 20, true));
    fbe_2d_pen_t clipped = pen({ 255, 120, 220, 255 });
    clipped.origin_x = 64;
    clipped.origin_y = 30;
    clipped.viewport_x = 66;
    clipped.viewport_y = 32;
    clipped.viewport_width = 26;
    clipped.viewport_height = 26;
    require(fbe_2d_oval(0, &clipped, 0, 0, 30, 30, true));
    clipped = pen({ 30, 30, 30, 255 });
    clipped.viewport_x = 66;
    clipped.viewport_y = 32;
    clipped.viewport_width = 26;
    clipped.viewport_height = 26;
    require(fbe_2d_line(0, &clipped, 60, 60, 95, 28));
    fbe_frame_flip(false);
    const fbe::golden::frame_t frame{
        .width = static_cast<std::uint32_t>(fbe_window_pixel_width()),
        .height = static_cast<std::uint32_t>(fbe_window_pixel_height()),
        .rgba = fbe::gpu::read_window(),
    };
    fbe::golden::check("canvas", frame);
    fbe_window_close();
    stop_gpu();
}

namespace {

/// An 8x8 test card, every pixel different, opaque.
std::uint64_t card(std::int32_t filter = FBE_TEXTURE_NEAREST) {
    const std::uint64_t texture = fbe_texture_create(8, 8, FBE_TEXTURE_RGBA8, false);
    REQUIRE(texture != 0);
    std::vector<std::uint8_t> pixels;
    for (std::uint8_t y = 0; y < 8; ++y) {
        for (std::uint8_t x = 0; x < 8; ++x) {
            pixels.push_back(static_cast<std::uint8_t>((x * 32) + 8));
            pixels.push_back(static_cast<std::uint8_t>((y * 32) + 8));
            pixels.push_back(static_cast<std::uint8_t>(255 - ((x + y) * 16)));
            pixels.push_back(255);
        }
    }
    require(fbe_texture_write(texture, 0, 0, 8, 8, pixels.data(), 32));
    require(fbe_texture_set_filter(texture, filter));
    return texture;
}

/// The card's pixel at `x`, `y`.
rgba_t card_pixel(std::int64_t x, std::int64_t y) {
    return {
        static_cast<std::uint8_t>((x * 32) + 8),
        static_cast<std::uint8_t>((y * 32) + 8),
        static_cast<std::uint8_t>(255 - ((x + y) * 16)),
        255,
    };
}

constexpr fbe_2d_sprite_t all{ .scale_x = 1.0F, .scale_y = 1.0F };

} // namespace

TEST_CASE("a texture drawn at its size at whole pixels is its texels exactly", "[gpu]") {
    start();
    for (const std::int32_t filter : { FBE_TEXTURE_NEAREST, FBE_TEXTURE_LINEAR }) {
        INFO("filter " << filter);
        const std::uint64_t made = target(16, 16);
        const std::uint64_t texture = card(filter);
        const fbe_2d_pen_t ink = pen(white);
        require(fbe_2d_texture(made, &ink, texture, &all, 3.0F, 5.0F));
        const fbe::golden::frame_t frame = read(made);
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                INFO(x << ", " << y);
                CHECK(pixel(frame, x + 3, y + 5) == card_pixel(x, y));
            }
        }
        CHECK(pixel(frame, 2, 5) == rgba_t{ 0, 0, 0, 0 });
    }
    stop_gpu();
}

TEST_CASE("a sprite's region, mirror, and quarter turn are its texels moved", "[gpu]") {
    start();
    const std::uint64_t made = target(16, 8);
    const std::uint64_t texture = card();
    const fbe_2d_pen_t ink = pen(white);
    // The 4x4 at 2, 2, as it is at 0, 0; mirrored across with its handle at 4, 0 drawn at 4, 0,
    // so it covers 4 to 8; turned a quarter clockwise about its top left, drawn at 16, 0, so it
    // covers 12 to 16.
    const fbe_2d_sprite_t region{
        .region_x = 2,
        .region_y = 2,
        .region_width = 4,
        .region_height = 4,
        .scale_x = 1,
        .scale_y = 1,
    };
    fbe_2d_sprite_t mirrored = region;
    mirrored.handle_x = 4;
    mirrored.scale_x = -1;
    fbe_2d_sprite_t turned = region;
    turned.rotation = std::numbers::pi_v<float> / 2;
    require(fbe_2d_texture(made, &ink, texture, &region, 0, 0));
    require(fbe_2d_texture(made, &ink, texture, &mirrored, 4, 0));
    require(fbe_2d_texture(made, &ink, texture, &turned, 16, 0));
    const fbe::golden::frame_t frame = read(made);
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            INFO(x << ", " << y);
            CHECK(pixel(frame, x, y) == card_pixel(2 + x, 2 + y));
            CHECK(pixel(frame, 4 + x, y) == card_pixel(2 + 3 - x, 2 + y));
            // A quarter turn clockwise puts the region's left column at the top, right to left.
            CHECK(pixel(frame, 12 + x, y) == card_pixel(2 + y, 2 + 3 - x));
        }
    }
    stop_gpu();
}

TEST_CASE("a sprite's tinted and faded by the pen's color", "[gpu]") {
    start();
    const std::uint64_t made = target(4, 1);
    const std::uint64_t texture = fbe_texture_create(1, 1, FBE_TEXTURE_RGBA8, false);
    REQUIRE(texture != 0);
    const std::array<std::uint8_t, 4> whitened{ 255, 255, 255, 255 };
    require(fbe_texture_write(texture, 0, 0, 1, 1, whitened.data(), 4));
    const fbe_2d_pen_t clear = pen(black);
    require(fbe_2d_clear(made, &clear));
    const fbe_2d_pen_t tinted = pen(red);
    require(fbe_2d_texture(made, &tinted, texture, &all, 0, 0));
    const fbe_2d_pen_t faded = pen({ 255, 255, 255, 128 }, FBE_2D_ALPHA);
    require(fbe_2d_texture(made, &faded, texture, &all, 1, 0));
    const fbe::golden::frame_t frame = read(made);
    CHECK(pixel(frame, 0, 0) == red);
    CHECK(std::abs(pixel(frame, 1, 0)[0] - to_srgb(128.0 / 255.0)) <= 1);
    stop_gpu();
}

TEST_CASE("tiles cover the viewport, one's handle at the position", "[gpu]") {
    start();
    const std::uint64_t made = target(24, 20);
    const std::uint64_t texture = card();
    fbe_2d_pen_t clipped = pen(white);
    clipped.viewport_x = 2;
    clipped.viewport_y = 1;
    clipped.viewport_width = 20;
    clipped.viewport_height = 17;
    // All of it, repeating as it wraps, its handle 1, 2 at 5, 7: a tile's top left at 4, 5.
    fbe_2d_sprite_t handled = all;
    handled.handle_x = 1;
    handled.handle_y = 2;
    require(fbe_2d_tile(made, &clipped, texture, &handled, 5, 7));
    fbe::golden::frame_t frame = read(made);
    for (std::uint32_t y = 0; y < 20; ++y) {
        for (std::uint32_t x = 0; x < 24; ++x) {
            INFO(x << ", " << y);
            const bool inside = x >= 2 && x < 22 && y >= 1 && y < 18;
            CHECK(pixel(frame, x, y) == (inside ? card_pixel(((x + 8) - 4) % 8, ((y + 8) - 5) % 8)
                                                : rgba_t{ 0, 0, 0, 0 }));
        }
    }
    // A part of it, a quad a tile.
    const fbe_2d_pen_t ink = pen(white);
    fbe_2d_sprite_t part = all;
    part.region_x = 4;
    part.region_y = 4;
    part.region_width = 3;
    part.region_height = 2;
    require(fbe_2d_tile(made, &ink, texture, &part, 0, 0));
    frame = read(made);
    for (std::uint32_t y = 0; y < 20; ++y) {
        for (std::uint32_t x = 0; x < 24; ++x) {
            INFO(x << ", " << y);
            CHECK(pixel(frame, x, y) == card_pixel(4 + (x % 3), 4 + (y % 2)));
        }
    }
    stop_gpu();
}

// Drawn into, then drawn from, in one frame: the pass sampling it comes after the pass drawing it.
TEST_CASE("a texture drawn into and then drawn shows what was drawn into it", "[gpu]") {
    start();
    const std::uint64_t drawn = target(8, 8);
    const std::uint64_t shown = target(8, 8);
    const fbe_2d_pen_t ink = pen(green);
    require(fbe_2d_rect(drawn, &ink, 2, 2, 4, 4, true));
    const fbe_2d_pen_t plain = pen(white);
    require(fbe_2d_texture(shown, &plain, drawn, &all, 0, 0));
    require(fbe_2d_rect(drawn, &ink, 0, 0, 8, 8, true));
    const fbe::golden::frame_t frame = read(shown);
    CHECK(pixel(frame, 3, 3) == green);
    CHECK(pixel(frame, 0, 0) == rgba_t{ 0, 0, 0, 0 });
    CHECK(fbe::gpu::last_recorded().passes == std::vector<std::string_view>{ "2d", "2d", "2d" });
    stop_gpu();
}

TEST_CASE("a sprite that can't be drawn is refused", "[gpu]") {
    start();
    const std::uint64_t made = target(8, 8);
    const std::uint64_t texture = card();
    const fbe_2d_pen_t ink = pen(white);
    CHECK(!fbe_2d_texture(made, &ink, made, &all, 0, 0));
    CHECK(std::string(fbe_error()) == "a texture isn't drawn into itself");
    fbe_2d_sprite_t outside = all;
    outside.region_x = 6;
    outside.region_width = 4;
    outside.region_height = 2;
    CHECK(!fbe_2d_texture(made, &ink, texture, &outside, 0, 0));
    CHECK(std::string(fbe_error()) == "a sprite's part, 4x2 at 6, 0, isn't in a 8x8 texture");
    fbe_2d_sprite_t spun = all;
    spun.rotation = NAN;
    CHECK(!fbe_2d_texture(made, &ink, texture, &spun, 0, 0));
    CHECK(std::string(fbe_error()) ==
          "a sprite's position, handle, rotation, and scale are finite");
    CHECK(!fbe_2d_texture(made, &ink, texture, &all, NAN, 0));
    CHECK(!fbe_2d_tile(made, &ink, texture, &all, 0, INFINITY));
    CHECK(std::string(fbe_error()) ==
          "a sprite's position, handle, rotation, and scale are finite");
    fbe_2d_sprite_t flat = all;
    flat.scale_y = 0;
    CHECK(!fbe_2d_tile(made, &ink, texture, &flat, 0, 0));
    CHECK(std::string(fbe_error()) == "a sprite's tiled at a scale that isn't 0");
    CHECK(!fbe_2d_texture(made, &ink, texture, nullptr, 0, 0));
    REQUIRE(fbe_texture_release(texture));
    CHECK(!fbe_2d_tile(made, &ink, texture, &all, 0, 0));
    CHECK(std::string(fbe_error()) == "the texture was freed");
    stop_gpu();
}

// A part of a sheet, LINEAR or NEAREST, shows none of the next part's texels: scaled up, turned,
// and drawn into a multisampled target, whose pixels at its edges take uv from inside it.
TEST_CASE("a part of a sheet shows none of its neighbor's texels", "[gpu]") {
    start();
    const fbe_2d_pen_t ink = pen(white);
    const auto bluish = [](const fbe::golden::frame_t& frame) {
        int count = 0;
        for (std::uint32_t y = 0; y < frame.height; ++y) {
            for (std::uint32_t x = 0; x < frame.width; ++x) {
                count += pixel(frame, x, y)[2] > 0 ? 1 : 0;
            }
        }
        return count;
    };
    for (const std::int32_t filter : { FBE_TEXTURE_NEAREST, FBE_TEXTURE_LINEAR }) {
        INFO("filter " << filter);
        // 16 by 8, its left half red and its right blue.
        const std::uint64_t sheet = fbe_texture_create(16, 8, FBE_TEXTURE_RGBA8, false);
        REQUIRE(sheet != 0);
        std::vector<std::uint8_t> halves;
        for (std::int32_t at = 0; at < 16 * 8; ++at) {
            const rgba_t color = at % 16 < 8 ? red : blue;
            halves.insert(halves.end(), color.begin(), color.end());
        }
        require(fbe_texture_write(sheet, 0, 0, 16, 8, halves.data(), 64));
        require(fbe_texture_set_filter(sheet, filter));
        fbe_2d_sprite_t left{
            .region_width = 8,
            .region_height = 8,
            .scale_x = 4,
            .scale_y = 4,
        };
        const std::uint64_t scaled = target(64, 64);
        require(fbe_2d_texture(scaled, &ink, sheet, &left, 0, 0));
        CHECK(bluish(read(scaled)) == 0);
        left.handle_x = 4;
        left.handle_y = 4;
        left.rotation = std::numbers::pi_v<float> / 6;
        const std::uint64_t turned = target(64, 64);
        require(fbe_2d_texture(turned, &ink, sheet, &left, 32, 32));
        CHECK(bluish(read(turned)) == 0);
        left.scale_x = 1;
        left.scale_y = 1;
        const std::uint64_t sampled =
            fbe_texture_create_target(16, 16, FBE_TEXTURE_RGBA8, 4, false);
        REQUIRE(sampled != 0);
        require(fbe_2d_texture(sampled, &ink, sheet, &left, 8, 8));
        CHECK(bluish(read(sampled)) == 0);
        // Tiled, scaled up, the same.
        left.handle_x = 0;
        left.handle_y = 0;
        left.rotation = 0;
        left.scale_x = 3;
        left.scale_y = 3;
        const std::uint64_t tiled = target(64, 64);
        require(fbe_2d_tile(tiled, &ink, sheet, &left, 0, 0));
        CHECK(bluish(read(tiled)) == 0);
    }
    stop_gpu();
}

// Tiles are one quad over the viewport, a part's too: mirrored, a part's repeat as the mirrored
// sprite's pixels do, and at a tiny scale it's still one quad, not a quad a tile.
TEST_CASE("a part's tiles are one quad, mirrored or tiny", "[gpu]") {
    start();
    const std::uint64_t texture = card();
    const fbe_2d_pen_t ink = pen(white);
    fbe_2d_sprite_t part = all;
    part.region_x = 1;
    part.region_y = 2;
    part.region_width = 3;
    part.region_height = 2;
    part.scale_x = -1;
    const std::uint64_t mirrored = target(24, 12);
    require(fbe_2d_tile(mirrored, &ink, texture, &part, 0, 0));
    fbe::golden::frame_t frame = read(mirrored);
    for (std::uint32_t y = 0; y < 12; ++y) {
        for (std::uint32_t x = 0; x < 24; ++x) {
            INFO(x << ", " << y);
            CHECK(pixel(frame, x, y) == card_pixel(1 + 2 - (x % 3), 2 + (y % 2)));
        }
    }
    // All of it mirrored both ways, its handle 0, 0 at 5, 7: texel 0, 0 at pixel 4, 6.
    fbe_2d_sprite_t both = all;
    both.scale_x = -1;
    both.scale_y = -1;
    const std::uint64_t whole = target(24, 20);
    require(fbe_2d_tile(whole, &ink, texture, &both, 5, 7));
    frame = read(whole);
    for (std::uint32_t y = 0; y < 20; ++y) {
        for (std::uint32_t x = 0; x < 24; ++x) {
            INFO(x << ", " << y);
            const std::int64_t u = (((4 - static_cast<std::int64_t>(x)) % 8) + 8) % 8;
            const std::int64_t v = (((6 - static_cast<std::int64_t>(y)) % 8) + 8) % 8;
            CHECK(pixel(frame, x, y) == card_pixel(u, v));
        }
    }
    // A pixel of it at a hundredth of its size over 2048 by 2048: 4 * 10^10 tiles, one quad.
    fbe_2d_sprite_t dot = all;
    dot.region_x = 2;
    dot.region_y = 3;
    dot.region_width = 1;
    dot.region_height = 1;
    dot.scale_x = 0.01F;
    dot.scale_y = 0.01F;
    const std::uint64_t big = target(2048, 2048);
    require(fbe_2d_tile(big, &ink, texture, &dot, 0, 0));
    require(fbe::gpu::finish_frame());
    CHECK(fbe::gpu::last_recorded().passes == std::vector<std::string_view>{ "2d" });
    frame = read(big);
    CHECK(pixel(frame, 0, 0) == card_pixel(2, 3));
    CHECK(pixel(frame, 2047, 1234) == card_pixel(2, 3));
    stop_gpu();
}

// All of a texture tiled 8 million pixels away is the same as tiled where it lands: its tiles are
// moved by whole tiles to the viewport before their uv's worked out in floats.
TEST_CASE("a texture tiled far from 0 is its texels still", "[gpu]") {
    start();
    const std::uint64_t texture = card();
    const fbe_2d_pen_t ink = pen(white);
    const std::uint64_t near = target(64, 8);
    require(fbe_2d_tile(near, &ink, texture, &all, 3, 0));
    const fbe::golden::frame_t expected = read(near);
    for (const float offset : { 8.0e4F, 8.0e5F, 8.0e6F }) {
        INFO("at " << offset + 3);
        const std::uint64_t far = target(64, 8);
        require(fbe_2d_tile(far, &ink, texture, &all, 3.0F + offset, 0));
        CHECK(read(far).rgba == expected.rgba);
    }
    stop_gpu();
}

// 2D's samplers are its own, without 3D's LOD bias: a mipmapped texture at its size is its first
// level's texels exactly, biased toward smaller levels or not.
TEST_CASE("a sprite isn't biased by the GPU's LOD bias", "[gpu]") {
    start();
    const std::uint64_t texture = fbe_texture_create(8, 8, FBE_TEXTURE_RGBA8, true);
    REQUIRE(texture != 0);
    std::vector<std::uint8_t> pixels;
    for (std::int64_t y = 0; y < 8; ++y) {
        for (std::int64_t x = 0; x < 8; ++x) {
            const rgba_t color = card_pixel(x, y);
            pixels.insert(pixels.end(), color.begin(), color.end());
        }
    }
    require(fbe_texture_write(texture, 0, 0, 8, 8, pixels.data(), 32));
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_LINEAR));
    require(fbe_gpu_set_lod_bias(2.0F));
    const std::uint64_t made = target(8, 8);
    const fbe_2d_pen_t ink = pen(white);
    require(fbe_2d_texture(made, &ink, texture, &all, 0, 0));
    const fbe::golden::frame_t frame = read(made);
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            INFO(x << ", " << y);
            CHECK(pixel(frame, x, y) == card_pixel(x, y));
        }
    }
    require(fbe_gpu_set_lod_bias(0.0F));
    stop_gpu();
}

// E3's sprites: the card at its size, scaled, turned smoothly about its middle, mirrored, tinted
// and faded, frames of a sheet, and tiles of all of it and of a part, into the screen.
TEST_CASE("sprites match their golden image", "[gpu][golden]") {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_window_open(96, 64, FBE_WINDOW_WINDOWED));
    require(fbe_gpu_start());
    const fbe_2d_pen_t clear = pen({ 30, 30, 40, 255 });
    require(fbe_2d_clear(0, &clear));
    const std::uint64_t sharp = card();
    const std::uint64_t smooth = card(FBE_TEXTURE_LINEAR);
    const fbe_2d_pen_t plain = pen(white);
    require(fbe_2d_texture(0, &plain, sharp, &all, 2, 2));
    fbe_2d_sprite_t bigger = all;
    bigger.scale_x = 3;
    bigger.scale_y = 2;
    require(fbe_2d_texture(0, &plain, sharp, &bigger, 12, 2));
    fbe_2d_sprite_t turned = all;
    turned.handle_x = 4;
    turned.handle_y = 4;
    turned.rotation = std::numbers::pi_v<float> / 6;
    turned.scale_x = 2;
    turned.scale_y = 2;
    require(fbe_2d_texture(0, &plain, smooth, &turned, 50, 12));
    fbe_2d_sprite_t mirrored = all;
    mirrored.scale_x = -2;
    mirrored.scale_y = 2;
    require(fbe_2d_texture(0, &plain, sharp, &mirrored, 86, 2));
    const fbe_2d_pen_t tinted = pen({ 255, 160, 60, 160 }, FBE_2D_ALPHA);
    require(fbe_2d_texture(0, &tinted, sharp, &bigger, 62, 20));
    // Frames: the card's four 4x4 quarters as a sheet, side by side.
    for (std::int32_t frame = 0; frame < 4; ++frame) {
        const fbe_2d_sprite_t quarter{
            .region_x = static_cast<float>((frame % 2) * 4),
            .region_y = frame >= 2 ? 4.0F : 0.0F,
            .region_width = 4,
            .region_height = 4,
            .scale_x = 2,
            .scale_y = 2,
        };
        require(fbe_2d_texture(0, &plain, sharp, &quarter,
                               2.0F + (static_cast<float>(frame) * 10.0F), 30));
    }
    fbe_2d_pen_t tiles = pen(white);
    tiles.viewport_x = 2;
    tiles.viewport_y = 42;
    tiles.viewport_width = 40;
    tiles.viewport_height = 20;
    require(fbe_2d_tile(0, &tiles, sharp, &all, 3, 44));
    tiles.viewport_x = 46;
    fbe_2d_sprite_t part = all;
    part.region_x = 2;
    part.region_y = 2;
    part.region_width = 5;
    part.region_height = 3;
    require(fbe_2d_tile(0, &tiles, sharp, &part, 46, 42));
    fbe_frame_flip(false);
    const fbe::golden::frame_t frame{
        .width = static_cast<std::uint32_t>(fbe_window_pixel_width()),
        .height = static_cast<std::uint32_t>(fbe_window_pixel_height()),
        .rgba = fbe::gpu::read_window(),
    };
    fbe::golden::check("sprites", frame);
    fbe_window_close();
    stop_gpu();
}

// Not a check, a measure: a 1280x720 frame of 10,000 rectangles, 1,000 lines, and 100 ovals into
// the screen, what the CPU spends drawing them, and the frame.
TEST_CASE("a frame of 11,100 shapes, timed", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_window_open(1280, 720, FBE_WINDOW_WINDOWED));
    require(fbe_gpu_start());
    fbe_2d_pen_t ink = pen(red, FBE_2D_ALPHA);
    const auto draw = [&ink] {
        for (std::int32_t at = 0; at < 10000; ++at) {
            fbe_2d_rect(0, &ink, (at * 37) % 1270, (at * 53) % 710, 10, 10, true);
        }
        for (std::int32_t at = 0; at < 1000; ++at) {
            fbe_2d_line(0, &ink, (at * 13) % 1280, 0, (at * 29) % 1280, 719);
        }
        for (std::int32_t at = 0; at < 100; ++at) {
            fbe_2d_oval(0, &ink, (at * 97) % 1200, (at * 61) % 640, 60, 40, at % 2 == 0);
        }
    };
    for (int at = 0; at < 3; ++at) {
        draw();
        fbe_frame_flip(false);
    }
    constexpr int frames = 30;
    std::chrono::duration<double, std::milli> drawing{};
    const auto started = std::chrono::steady_clock::now();
    for (int at = 0; at < frames; ++at) {
        fbe::gpu::recording_frame();
        const auto before = std::chrono::steady_clock::now();
        draw();
        drawing += std::chrono::steady_clock::now() - before;
        fbe_frame_flip(false);
    }
    vkDeviceWaitIdle(fbe::gpu::device()->device);
    const std::chrono::duration<double, std::milli> spent =
        std::chrono::steady_clock::now() - started;
    fbe_gpu_counters_t counters{};
    require(fbe_gpu_counters(&counters));
    CHECK(counters.passes == 2);
    WARN("11,100 shapes: drawing " << drawing.count() / frames << " ms, the frame "
                                   << spent.count() / frames << " ms, the GPU's "
                                   << counters.gpu_milliseconds << " ms, " << counters.draws
                                   << " draws, " << counters.triangles << " triangles, on "
                                   << fbe_gpu_device_name(fbe_gpu_device()));
    fbe_window_close();
    stop_gpu();
}

// Not a check, a measure: a 1280x720 frame of 10,000 sprites, turned and scaled, of 8 textures,
// and of 4,000, into the screen, what the CPU spends drawing them, and the frame.
TEST_CASE("a frame of 10,000 sprites, timed", "[gpu]") {
    for (const std::size_t count : { std::size_t{ 8 }, std::size_t{ 4000 } }) {
        REQUIRE(fbe_window_set_headless(true));
        fbe_gpu_set_pipeline_cache("");
        require(fbe_window_open(1280, 720, FBE_WINDOW_WINDOWED));
        require(fbe_gpu_start());
        std::vector<std::uint64_t> textures;
        textures.reserve(count);
        for (std::size_t at = 0; at < count; ++at) {
            textures.push_back(card(FBE_TEXTURE_LINEAR));
        }
        const fbe_2d_pen_t ink = pen(white, FBE_2D_ALPHA);
        const auto draw = [&] {
            for (std::int32_t at = 0; at < 10000; ++at) {
                const fbe_2d_sprite_t sprite{
                    .handle_x = 4,
                    .handle_y = 4,
                    .rotation = static_cast<float>(at % 360) * std::numbers::pi_v<float> / 180,
                    .scale_x = 2,
                    .scale_y = 2,
                };
                fbe_2d_texture(0, &ink, textures[static_cast<std::size_t>(at) % textures.size()],
                               &sprite, static_cast<float>((at * 37) % 1280),
                               static_cast<float>((at * 53) % 720));
            }
        };
        for (int at = 0; at < 3; ++at) {
            draw();
            fbe_frame_flip(false);
        }
        constexpr int frames = 30;
        std::chrono::duration<double, std::milli> drawing{};
        const auto started = std::chrono::steady_clock::now();
        for (int at = 0; at < frames; ++at) {
            fbe::gpu::recording_frame();
            const auto before = std::chrono::steady_clock::now();
            draw();
            drawing += std::chrono::steady_clock::now() - before;
            fbe_frame_flip(false);
        }
        vkDeviceWaitIdle(fbe::gpu::device()->device);
        const std::chrono::duration<double, std::milli> spent =
            std::chrono::steady_clock::now() - started;
        fbe_gpu_counters_t counters{};
        require(fbe_gpu_counters(&counters));
        CHECK(counters.passes == 2);
        WARN("10,000 sprites of " << count << " textures: drawing " << drawing.count() / frames
                                  << " ms, the frame " << spent.count() / frames
                                  << " ms, the GPU's " << counters.gpu_milliseconds << " ms, "
                                  << counters.draws << " draws, on "
                                  << fbe_gpu_device_name(fbe_gpu_device()));
        fbe_window_close();
        stop_gpu();
    }
}
