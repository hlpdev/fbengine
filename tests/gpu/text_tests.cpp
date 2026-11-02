// 2D's text: the atlas filled by a filler of this file's own, glyphs of known pixels laid out a few
// pixels apart, and drawn as text/'s core's layouts are, so the pages' uploads, the glyphs'
// quads, and trimming are tested with pixels known exactly; and fbe_2d_text in real fonts,
// HarfBuzz's test fonts and the tests' own, against the atlas's pixels, its underline, and a
// golden image. Each test with the GPU ends by stopping it, which has to leave nothing behind.

#include "golden/golden.hpp"
#include "gpu_testing.hpp"

#include <2d/canvas.hpp>
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fbengine.h>
#include <gpu/vulkan.hpp>
#include <map>
#include <string>
#include <string_view>
#include <text/text.hpp>
#include <vector>

using fbe::tests::require;
using fbe::tests::stop_gpu;

namespace {

using rgba_t = std::array<std::uint8_t, 4>;

/// The filler's coverage glyphs, 3 by 5 pixels: '#' covered, '+' half, '.' not.
const std::map<char, std::array<std::string_view, 5>>& bitmaps() {
    static const std::map<char, std::array<std::string_view, 5>> made{
        { 'I', { "###", ".#.", ".#.", ".#.", "###" } },
        { 'L', { "#..", "#..", "#..", "#..", "###" } },
        { 'T', { "###", ".#.", ".#.", ".#.", ".#." } },
        { 'o', { "...", ".+.", "+.+", ".+.", "..." } },
    };
    return made;
}

/// The filler's color glyph, '@', 2 by 2.
constexpr std::array<rgba_t, 4> colored{
    {
        { 200, 100, 50, 255 },
        { 10, 20, 30, 255 },
        { 0, 255, 0, 255 },
        { 255, 255, 255, 255 },
    },
};

/// A glyph wider than a page, 'W', 1100 by 1, covered.
constexpr std::int32_t wide = 1100;

/// Where the filler's put each character's glyph, in the atlas's generation it was put there in,
/// and how many it's put there.
struct placed_t {
    std::uint32_t page = 0;
    fbe::text::rect_t place;
};
std::map<char, placed_t>& placed() {
    static std::map<char, placed_t> made;
    return made;
}
std::uint64_t placed_in = UINT64_MAX;
int places = 0;

/// A character's glyph's size: the wide one's, the color one's, or a 3 by 5 one's.
std::array<std::int32_t, 2> size_of(char character) {
    if (character == 'W') {
        return { wide, 1 };
    }
    if (character == '@') {
        return { 2, 2 };
    }
    return { 3, 5 };
}

/// A bitmap's pixel's coverage.
int coverage_of(char pixel) {
    if (pixel == '#') {
        return 255;
    }
    return pixel == '+' ? 128 : 0;
}

/// Puts `character`'s glyph on the atlas, if it isn't there yet.
bool place(char character, placed_t& into) {
    fbe::text::atlas_t& atlas = fbe::text::atlas();
    if (atlas.generation() != placed_in) {
        placed().clear();
        placed_in = atlas.generation();
    }
    if (const auto found = placed().find(character); found != placed().end()) {
        into = found->second;
        return true;
    }
    const bool color = character == '@';
    const auto [width, height] = size_of(character);
    placed_t made;
    if (!atlas.place(color ? fbe::text::page_format_t::COLOR : fbe::text::page_format_t::COVERAGE,
                     width, height, made.page, made.place)) {
        return false;
    }
    fbe::text::page_t& page = atlas.page(made.page);
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(made.place.y + y) *
                                    static_cast<std::size_t>(page.width)) +
                                   static_cast<std::size_t>(made.place.x + x);
            if (color) {
                const rgba_t& texel =
                    colored.at((static_cast<std::size_t>(y) * 2) + static_cast<std::size_t>(x));
                std::ranges::copy(texel, page.pixels.begin() + static_cast<std::ptrdiff_t>(at * 4));
            } else if (character == 'W') {
                page.pixels[at] = 255;
            } else {
                const char pixel = bitmaps().at(character).at(
                    static_cast<std::size_t>(y))[static_cast<std::size_t>(x)];
                page.pixels[at] = static_cast<std::uint8_t>(coverage_of(pixel));
            }
        }
    }
    atlas.written(made.page, made.place);
    placed()[character] = made;
    ++places;
    into = made;
    return true;
}

/// Lays `text` out as the core does, into `into`: each character its glyph, 4 pixels on from the
/// last, the top of a 3 by 5 glyph 5 above the baseline and of the color one 2; a space nothing,
/// but its 4 pixels.
bool fill(std::string_view text, fbe::text::layout_t& into) {
    into = {};
    std::int32_t x = 0;
    for (const char character : text) {
        if (character == ' ') {
            into.glyphs.push_back({ .x = x });
            x += 4;
            continue;
        }
        placed_t glyph;
        if (!place(character, glyph)) {
            return false;
        }
        into.glyphs.push_back({
            .page = glyph.page,
            .source = glyph.place,
            .x = x,
            .y = character == '@' ? -2 : -5,
            .width = glyph.place.width,
            .height = glyph.place.height,
        });
        fbe::text::atlas().used(glyph.page);
        x += character == 'W' ? wide + 1 : 4;
    }
    into.width = static_cast<float>(x);
    into.height = 6.0F;
    into.ascent = 5.0F;
    into.generation = fbe::text::atlas().generation();
    return true;
}

/// Starts the GPU headless with an empty atlas.
void start() {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_gpu_start());
    fbe::text::atlas().clear();
    fbe::text::atlas().set_budget(std::size_t{ 16 } << 20U);
    places = 0;
}

void stop() {
    stop_gpu();
}

/// A font from HarfBuzz's test fonts, or the tests' own, at `size` with `style`, with no fallback
/// from the OS's, so what it has is the font's own.
std::uint64_t font_of(const std::string& path, float size, std::int32_t style = 0) {
    const std::uint64_t font = fbe_text_load_font(path.c_str(), 0, size, style);
    INFO(fbe_error());
    REQUIRE(font != 0);
    fbe_text_set_system_fallback(font, false);
    return font;
}
std::uint64_t opened(const char* name, float size, std::int32_t style = 0) {
    return font_of(std::string(FBE_TEST_FONTS) + "/" + name, size, style);
}
std::uint64_t own(const char* name, float size, std::int32_t style = 0) {
    return font_of(std::string(FBE_OWN_TEST_FONTS) + "/" + name, size, style);
}

std::uint64_t target(std::int32_t width, std::int32_t height,
                     std::int32_t format = FBE_TEXTURE_RGBA8_LINEAR) {
    const std::uint64_t made = fbe_texture_create_target(width, height, format, 1, false);
    INFO(fbe_error());
    REQUIRE(made != 0);
    return made;
}

fbe_2d_pen_t pen(rgba_t color) {
    return { .red = color[0], .green = color[1], .blue = color[2], .alpha = color[3] };
}

constexpr rgba_t black{ 0, 0, 0, 255 };
constexpr rgba_t white{ 255, 255, 255, 255 };

/// Draws `text` as the filler lays it out at `x`, `y`, as fbe_2d_text does a layout, the atlas
/// trimmed first as the frame's first canvas draw does; false, with fbe_error's why, if it can't.
bool drawn(std::uint64_t into, std::string_view text, std::int32_t x, std::int32_t y,
           rgba_t color = white) {
    const fbe_2d_pen_t ink = pen(color);
    fbe::canvas::trim_atlas();
    fbe::text::layout_t layout;
    return fill(text, layout) && fbe::canvas::draw_text(into, &ink, layout, x, y);
}

void draw(std::uint64_t into, std::string_view text, std::int32_t x, std::int32_t y,
          rgba_t color = white) {
    INFO(text);
    require(drawn(into, text, x, y, color));
}

void clear(std::uint64_t into) {
    const fbe_2d_pen_t ink = pen(black);
    require(fbe_2d_clear(into, &ink));
}

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

/// Checks `character`'s glyph is at `x`, `y` in a frame of white text on black in a target of data,
/// whose pixels are its coverage, and nothing's around it.
void check_glyph(const fbe::golden::frame_t& frame, char character, std::uint32_t x,
                 std::uint32_t y) {
    for (std::uint32_t row = 0; row < 5; ++row) {
        for (std::uint32_t column = 0; column < 3; ++column) {
            INFO(character << "'s " << column << ", " << row);
            const char drawn = bitmaps().at(character).at(row)[column];
            const int expected = coverage_of(drawn);
            CHECK(std::abs(pixel(frame, x + column, y + row)[0] - expected) <= 1);
        }
    }
    CHECK(pixel(frame, x + 3, y)[0] == 0);
    if (x > 0) {
        CHECK(pixel(frame, x - 1, y + 2)[0] == 0);
    }
}

} // namespace

TEST_CASE("text is its glyphs' pixels, texel for pixel at whole pixels", "[gpu]") {
    start();
    const std::uint64_t made = target(24, 10);
    clear(made);
    // Its top left at 3, 2, so its baseline at 7, each glyph's top 5 above it.
    draw(made, "IL To", 3, 2);
    const fbe::golden::frame_t frame = read(made);
    check_glyph(frame, 'I', 3, 2);
    check_glyph(frame, 'L', 7, 2);
    check_glyph(frame, 'T', 15, 2);
    check_glyph(frame, 'o', 19, 2);
    // The space between drew nothing.
    for (std::uint32_t y = 0; y < 10; ++y) {
        CHECK(pixel(frame, 12, y)[0] == 0);
    }
    stop();
}

// A page's texture gets what's been written into it since it was last uploaded, and the pixel
// around it, and nothing when nothing has: a frame drawing a glyph already there uploads nothing,
// one adding a glyph its 3 by 5 bytes and their transparent border, 5 by 7.
TEST_CASE("a page's new glyphs are what's uploaded of it", "[gpu]") {
    start();
    const std::uint64_t made = target(16, 8);
    const auto uploaded = [] {
        fbe_frame_flip(false);
        fbe_gpu_counters_t counters{};
        require(fbe_gpu_counters(&counters));
        return counters.uploaded;
    };
    draw(made, "I", 0, 0);
    uploaded();
    draw(made, "I", 0, 0);
    CHECK(uploaded() == 0);
    draw(made, "IL", 0, 0);
    CHECK(uploaded() == 35);
    check_glyph(read(made), 'L', 4, 0);
    stop();
}

// Cleared, the atlas's pages are made again, and their versions start again: the glyph put first in
// the new page is uploaded, though its page's version is the old page's, as the atlas's generation
// isn't.
TEST_CASE("a cleared atlas's pages are uploaded again, though their versions start again",
          "[gpu]") {
    start();
    const std::uint64_t made = target(8, 8);
    clear(made);
    draw(made, "I", 0, 0);
    const std::uint64_t version = fbe::text::atlas().pages()[0].version;
    fbe_frame_flip(false);
    fbe::text::atlas().clear();
    clear(made);
    draw(made, "L", 0, 0);
    CHECK(fbe::text::atlas().pages()[0].version == version);
    check_glyph(read(made), 'L', 0, 0);
    stop();
}

// Past its budget, the atlas is trimmed by a frame's first canvas draw, whatever it draws, and not
// later in the frame, so the frame's text stays drawable: a page the last frame's text used is
// kept, and one nothing's used since is dropped, its texture let go of, and made again, its number
// kept, when a glyph's placed on it, its new texture uploaded whole, as the page's `dirty` is only
// that glyph, so T where I was is T; text drawn after is laid out again.
TEST_CASE("the atlas is trimmed by a frame's first canvas draw, not during it", "[gpu]") {
    start();
    const std::uint64_t made = target(16, 8);
    fbe::text::atlas().set_budget(1);
    clear(made);
    const std::uint64_t first = fbe::text::atlas().generation();
    draw(made, "I", 0, 0);
    draw(made, "IL", 0, 0);
    CHECK(fbe::text::atlas().generation() == first);
    CHECK(places == 2);
    // Used by the frame before, its page's kept.
    fbe_frame_flip(false);
    clear(made);
    CHECK(fbe::text::atlas().generation() == first);
    fbe_frame_flip(false);
    // Used by none since, it's dropped, and its texture's let go of by the next text drawn, which
    // has nothing on it; then made again.
    clear(made);
    CHECK(fbe::text::atlas().generation() == first + 1);
    REQUIRE(fbe::text::atlas().pages().size() == 1);
    CHECK(fbe::text::atlas().pages()[0].dropped());
    CHECK(fbe::canvas::page_textures_made() == 1);
    draw(made, " ", 0, 0);
    CHECK(fbe::canvas::page_textures_made() == 0);
    draw(made, "TI", 0, 0);
    CHECK(fbe::canvas::page_textures_made() == 1);
    CHECK(fbe::text::atlas().generation() == first + 1);
    CHECK(places == 4);
    const fbe::golden::frame_t frame = read(made);
    check_glyph(frame, 'T', 0, 0);
    check_glyph(frame, 'I', 4, 0);
    stop();
}

// A color glyph is its own colors, the pen's not tinting them, into an sRGB target as it is; and a
// glyph too wide for a page has a page of its own.
TEST_CASE("a color glyph is its own colors, and a wide one has a page of its own", "[gpu]") {
    start();
    const std::uint64_t made = target(8, 6, FBE_TEXTURE_RGBA8);
    clear(made);
    // Its top 2 above the baseline, 5 below its top at 1, 0: at 1, 3.
    draw(made, "@", 1, 0, { 0, 0, 255, 255 });
    const fbe::golden::frame_t frame = read(made);
    CHECK(pixel(frame, 1, 3) == colored[0]);
    CHECK(pixel(frame, 2, 3) == colored[1]);
    CHECK(pixel(frame, 1, 4) == colored[2]);
    CHECK(pixel(frame, 2, 4) == colored[3]);
    CHECK(pixel(frame, 0, 3) == black);
    const std::uint64_t long_one = target(1200, 6);
    clear(long_one);
    draw(long_one, "IW", 0, 0);
    CHECK(fbe::text::atlas().pages().size() == 3);
    const fbe::golden::frame_t row = read(long_one);
    check_glyph(row, 'I', 0, 0);
    CHECK(pixel(row, 4, 0)[0] == 255);
    CHECK(pixel(row, 4 + wide - 1, 0)[0] == 255);
    CHECK(pixel(row, 4 + wide, 0)[0] == 0);
    stop();
}

// The atlas outlives the GPU: started again, its pages are made and uploaded again whole.
TEST_CASE("the atlas's pages are made again when the GPU starts again", "[gpu]") {
    start();
    std::uint64_t made = target(8, 8);
    draw(made, "I", 0, 0);
    fbe_frame_flip(false);
    stop_gpu();
    require(fbe_gpu_start());
    made = target(8, 8);
    clear(made);
    draw(made, "I", 0, 0);
    check_glyph(read(made), 'I', 0, 0);
    CHECK(places == 1);
    stop();
}

TEST_CASE("text that can't be drawn is refused", "[gpu]") {
    start();
    const std::uint64_t made = target(8, 8);
    const fbe_2d_pen_t ink = pen(white);
    const std::uint64_t font = opened("OpenSans-Regular.ttf", 12.0F);
    CHECK(!fbe_2d_text(made, &ink, font, nullptr, 0, 0));
    CHECK(std::string(fbe_error()) == "no text was given");
    CHECK(!fbe_2d_text(made, nullptr, font, "a", 0, 0));
    CHECK(std::string(fbe_error()) == "no pen was given");
    CHECK(fbe_text_free_font(font));
    CHECK(!fbe_2d_text(made, &ink, font, "a", 0, 0));
    CHECK(!fbe_2d_text(made, &ink, 0, "a", 0, 0));
    stop();
}

// Text checks the GPU as the canvas's other draws do: after a stop, its target's freed and it's
// refused, whatever pages it would make; once the GPU's lost, it's refused; and as a headless
// window's first draw, it starts the GPU for the screen, as a rectangle does.
TEST_CASE("text is refused without a GPU to draw it, and starts one for the screen", "[gpu]") {
    start();
    const std::uint64_t made = target(8, 8);
    draw(made, "I", 0, 0);
    fbe_frame_flip(false);
    fbe_gpu_stop();
    CHECK(!drawn(made, "L", 0, 0));
    CHECK(std::string(fbe_error()) == "the texture was freed");
    stop();
    // As a headless window's first draw, in a real font, which starts the GPU.
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_window_open(32, 24, FBE_WINDOW_WINDOWED));
    const std::uint64_t font = opened("OpenSans-Regular.ttf", 16.0F);
    REQUIRE(!fbe_gpu_started());
    const fbe_2d_pen_t ink = pen(white);
    require(fbe_2d_text(0, &ink, font, "ab", 1, 1));
    CHECK(fbe_gpu_started());
    const fbe::golden::frame_t screen = read(fbe_gpu_screen());
    int lit = 0;
    for (std::uint32_t y = 0; y < screen.height; ++y) {
        for (std::uint32_t x = 0; x < screen.width; ++x) {
            lit += pixel(screen, x, y)[0] > 0 ? 1 : 0;
        }
    }
    CHECK(lit > 20);
    // Lost.
    fbe::gpu::inject(fbe::gpu::fault_t::LOST);
    fbe_frame_flip(false);
    REQUIRE(fbe_gpu_lost());
    CHECK(!fbe_2d_text(0, &ink, font, "ab", 0, 0));
    CHECK(std::string(fbe_error()).starts_with("the GPU was lost"));
    CHECK(fbe_text_free_font(font));
    stop_gpu();
    fbe_window_close();
}

// New glyphs are written into their page in the frame's uploads, so they don't send the 2D batch
// drawing the page to the GPU early: a frame of rectangles and text between them takes the passes
// a frame of the same text, its glyphs already there, does.
TEST_CASE("new glyphs leave the 2D batch whole", "[gpu]") {
    start();
    const std::uint64_t made = target(64, 8);
    const fbe_2d_pen_t ink = pen(white);
    const auto frame = [&] {
        require(fbe_2d_rect(made, &ink, 0, 0, 1, 1, true));
        draw(made, "I", 2, 0);
        require(fbe_2d_rect(made, &ink, 8, 0, 1, 1, true));
        draw(made, "L", 10, 0);
        require(fbe_2d_rect(made, &ink, 16, 0, 1, 1, true));
        draw(made, "T", 18, 0);
        fbe_frame_flip(false);
        fbe_gpu_counters_t counters{};
        require(fbe_gpu_counters(&counters));
        return counters.passes;
    };
    fbe_frame_flip(false);
    const std::int32_t fresh = frame();
    CHECK(places == 3);
    const std::int32_t cached = frame();
    CHECK(places == 3);
    CHECK(fresh == cached);
    const fbe::golden::frame_t drawn = read(made);
    check_glyph(drawn, 'L', 10, 0);
    stop();
}

// A page's texture made again, as the GPU's started again, keeps nothing of what the page's keys
// say until it's uploaded, so an upload that fails is tried again by the next text drawn.
TEST_CASE("a page's texture made again is uploaded once an upload of it fails", "[gpu]") {
    start();
    std::uint64_t made = target(8, 8);
    draw(made, "I", 0, 0);
    fbe_frame_flip(false);
    stop_gpu();
    require(fbe_gpu_start());
    made = target(8, 8);
    clear(made);
    fbe::gpu::inject(fbe::gpu::fault_t::UPLOAD);
    CHECK(!drawn(made, "I", 0, 0));
    CHECK(std::string(fbe_error()).starts_with("couldn't make a buffer to upload a texture"));
    draw(made, "I", 0, 0);
    check_glyph(read(made), 'I', 0, 0);
    stop();
}

// In a real font, fbe_2d_text draws the glyphs its layout places, each texel of the atlas a pixel
// at whole pixels: into a data target over black in white, each pixel is its glyphs' coverage
// composited over each other in order, as lay_out laid them out.
TEST_CASE("text in a font is its glyphs' coverage, texel for pixel", "[gpu]") {
    start();
    const std::uint64_t font = opened("OpenSans-Regular.ttf", 20.0F);
    const std::uint64_t made = target(256, 40);
    clear(made);
    const fbe_2d_pen_t ink = pen(white);
    const char* text = "Hello, fbengine";
    require(fbe_2d_text(made, &ink, font, text, 3, 5));
    const fbe::golden::frame_t frame = read(made);
    fbe::text::layout_t layout;
    REQUIRE(fbe::text::lay_out(font, text, 1.0F, layout));
    std::vector<double> expected(std::size_t{ 256 } * 40, 0.0);
    const std::int64_t baseline = 5 + std::lround(layout.ascent);
    for (const fbe::text::placed_glyph_t& glyph : layout.glyphs) {
        const fbe::text::page_t& page = fbe::text::atlas().pages()[glyph.page];
        REQUIRE(page.format == fbe::text::page_format_t::COVERAGE);
        REQUIRE(glyph.width == glyph.source.width);
        for (std::int32_t row = 0; row < glyph.source.height; ++row) {
            for (std::int32_t column = 0; column < glyph.source.width; ++column) {
                const std::int64_t x = 3 + glyph.x + column;
                const std::int64_t y = baseline + glyph.y + row;
                if (x < 0 || x >= 256 || y < 0 || y >= 40) {
                    continue;
                }
                const double covered =
                    page.pixels[(static_cast<std::size_t>(glyph.source.y + row) *
                                 static_cast<std::size_t>(page.width)) +
                                static_cast<std::size_t>(glyph.source.x + column)] /
                    255.0;
                double& under =
                    expected[(static_cast<std::size_t>(y) * 256) + static_cast<std::size_t>(x)];
                under = covered + (under * (1.0 - covered));
            }
        }
    }
    int lit = 0;
    for (std::uint32_t y = 0; y < 40; ++y) {
        for (std::uint32_t x = 0; x < 256; ++x) {
            INFO(x << ", " << y);
            const double want = expected[(std::size_t{ y } * 256) + x] * 255.0;
            CHECK(std::abs(pixel(frame, x, y)[0] - want) <= 1.5);
            lit += want > 0 ? 1 : 0;
        }
    }
    CHECK(lit > 200);
    CHECK(fbe_text_free_font(font));
    stop();
}

// An underlined font's text has a line under each of its lines, at the font's place below the
// baseline, as thick as it says, as long as the line's advance; the same text in the font not
// underlined has nothing there.
TEST_CASE("an underlined font's text has a line under each line", "[gpu]") {
    start();
    const std::uint64_t plain = opened("OpenSans-Regular.ttf", 20.0F);
    const std::uint64_t lined = opened("OpenSans-Regular.ttf", 20.0F, FBE_TEXT_UNDERLINE);
    fbe_text_metrics_t metrics{};
    require(fbe_text_font_metrics(lined, &metrics));
    fbe_text_size_t size{};
    require(fbe_text_measure(lined, "ab", &size));
    fbe::text::layout_t layout;
    REQUIRE(fbe::text::lay_out(lined, "ab", 1.0F, layout));
    const fbe_2d_pen_t ink = pen(white);
    for (const std::uint64_t font : { plain, lined }) {
        INFO((font == lined ? "underlined" : "plain"));
        const std::uint64_t made = target(128, 80);
        clear(made);
        require(fbe_2d_text(made, &ink, font, "ab\nab", 4, 2));
        const fbe::golden::frame_t frame = read(made);
        for (int line = 0; line < 2; ++line) {
            const std::int64_t baseline =
                2 + std::lround(layout.ascent) +
                std::llround(static_cast<float>(line) * metrics.line_height);
            const auto y =
                static_cast<std::uint32_t>(baseline + std::lround(metrics.underline_position));
            const auto middle = static_cast<std::uint32_t>(4 + std::lround(size.width / 2.0F));
            CHECK(pixel(frame, middle, y)[0] == (font == lined ? 255 : 0));
            CHECK(pixel(frame, static_cast<std::uint32_t>(4 + std::lround(size.width) + 2), y)[0] ==
                  0);
        }
    }
    CHECK(fbe_text_free_font(plain));
    CHECK(fbe_text_free_font(lined));
    stop();
}

// The underline is under each of the lines the core breaks text into, whatever breaks it: "ab" then
// "abababab", each line's as long as its own advance.
TEST_CASE("an underline is under each line, whatever breaks it", "[gpu]") {
    start();
    const std::uint64_t lined = opened("OpenSans-Regular.ttf", 20.0F, FBE_TEXT_UNDERLINE);
    fbe_text_metrics_t metrics{};
    require(fbe_text_font_metrics(lined, &metrics));
    fbe_text_size_t first{};
    require(fbe_text_measure(lined, "ab", &first));
    fbe_text_size_t second{};
    require(fbe_text_measure(lined, "abababab", &second));
    fbe::text::layout_t layout;
    REQUIRE(fbe::text::lay_out(lined, "ab", 1.0F, layout));
    const fbe_2d_pen_t ink = pen(white);
    // '\n', "\r\n", a vertical tab, NEL, U+2028, and U+2029.
    for (const std::string_view broken :
         { "\n", "\r\n", "\v", "\xC2\x85", "\xE2\x80\xA8", "\xE2\x80\xA9" }) {
        const std::string text = "ab" + std::string(broken) + "abababab";
        INFO("broken by " << broken.size() << " bytes, starting " << int{ broken[0] });
        const std::vector<std::string_view> lines = fbe::text::split_lines(text);
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == (broken == "\r\n" ? "ab\r" : "ab"));
        CHECK(lines[1] == "abababab");
        fbe::text::layout_t laid;
        REQUIRE(fbe::text::lay_out(lined, text, 1.0F, laid));
        CHECK(std::abs(laid.height - (2 * metrics.line_height)) < 0.01F);
        const std::uint64_t made = target(200, 80);
        clear(made);
        require(fbe_2d_text(made, &ink, lined, text.c_str(), 4, 2));
        const fbe::golden::frame_t frame = read(made);
        for (int line = 0; line < 2; ++line) {
            INFO("line " << line);
            const std::int64_t baseline =
                2 + std::lround(layout.ascent) +
                std::llround(static_cast<float>(line) * metrics.line_height);
            const auto y =
                static_cast<std::uint32_t>(baseline + std::lround(metrics.underline_position));
            const float width = line == 0 ? first.width : second.width;
            CHECK(pixel(frame, static_cast<std::uint32_t>(4 + std::lround(width / 2.0F)), y)[0] ==
                  255);
            CHECK(pixel(frame, static_cast<std::uint32_t>(4 + std::lround(width) + 2), y)[0] == 0);
        }
    }
    CHECK(fbe::text::split_lines("") == std::vector<std::string_view>{ "" });
    CHECK(fbe::text::split_lines("ab\n") == std::vector<std::string_view>{ "ab", "" });
    // A break's bytes in a sequence that isn't well-formed don't break it: NEL's second byte alone,
    // and U+2028's first two.
    CHECK(fbe::text::split_lines("a\x85-b\xE2\x80-c").size() == 1);
    CHECK(fbe_text_free_font(lined));
    stop();
}

// E3's text: Open Sans at three sizes, one underlined, two lines, color glyphs of the tests' own,
// blended over a gray, and text in a texture drawn into the screen.
TEST_CASE("text matches its golden image", "[gpu][golden]") {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_window_open(160, 96, FBE_WINDOW_WINDOWED));
    require(fbe_gpu_start());
    fbe::text::atlas().clear();
    const fbe_2d_pen_t gray = pen({ 40, 44, 52, 255 });
    require(fbe_2d_clear(0, &gray));
    const std::uint64_t small = opened("OpenSans-Regular.ttf", 12.0F);
    const std::uint64_t medium = opened("OpenSans-Regular.ttf", 18.0F, FBE_TEXT_UNDERLINE);
    const std::uint64_t large = opened("OpenSans-Regular.ttf", 30.0F);
    const std::uint64_t color = own("FbeTestColor-Regular.ttf", 20.0F);
    const fbe_2d_pen_t ink = pen(white);
    require(fbe_2d_text(0, &ink, small, "The canvas's text,\ntwo lines at 12.", 4, 2));
    const fbe_2d_pen_t yellow = pen({ 250, 220, 90, 255 });
    require(fbe_2d_text(0, &yellow, medium, "Underlined", 4, 34));
    const fbe_2d_pen_t faded = pen({ 120, 200, 255, 160 });
    require(fbe_2d_text(0, &faded, large, "Big", 100, 26));
    require(fbe_2d_text(0, &ink, color, "\xE2\x8C\x9A\xEE\xA4\x83", 110, 2));
    fbe_frame_flip(false);
    const fbe::golden::frame_t frame{
        .width = static_cast<std::uint32_t>(fbe_window_pixel_width()),
        .height = static_cast<std::uint32_t>(fbe_window_pixel_height()),
        .rgba = fbe::gpu::read_window(),
    };
    fbe::golden::check("text", frame);
    for (const std::uint64_t font : { small, medium, large, color }) {
        CHECK(fbe_text_free_font(font));
    }
    fbe_window_close();
    stop_gpu();
}
