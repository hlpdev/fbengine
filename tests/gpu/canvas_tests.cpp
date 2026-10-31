// 2D: shapes drawn exact to the pixel, the viewport and origin, blends, the batch going to the
// frame graph in its turn, what a canvas refuses, and the golden image of them all. Each test
// with the GPU ends by stopping it, which has to leave nothing behind.

#include "golden/golden.hpp"
#include "gpu_testing.hpp"

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
#include <set>
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
    // A target freed before its batch goes to the graph is drawn into no more.
    require(fbe_2d_rect(made, &solid, 0, 0, 2, 2, true));
    REQUIRE(fbe_texture_release(made));
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
