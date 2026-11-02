// text's atlas and layouts, apart from the DLL, as the canvas reads them, on HarfBuzz's own test
// fonts: glyphs placed on pages at whole pixels, rasterized once, at a scale's pixels, color glyphs
// on color pages, straight, right-to-left runs turned around, and the atlas trimmed past its
// budget.

#include "text/fbe_text.h"
#include "text/font.hpp"
#include "text/text.hpp"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

/// A test font, at `size`, with no fallback from the OS's, so what it has is the font's own.
std::uint64_t opened(const char* name, float size) {
    const std::string path = std::string(FBE_TEST_FONTS) + "/" + name;
    const std::uint64_t font = fbe_text_load_font(path.c_str(), 0, size, 0);
    INFO(fbe_error());
    REQUIRE(font != 0);
    fbe_text_set_system_fallback(font, false);
    return font;
}

/// One of the tests' own fonts, at `size`, with no fallback from the OS's.
std::uint64_t own(const char* name, float size) {
    const std::string path = std::string(FBE_OWN_TEST_FONTS) + "/" + name;
    const std::uint64_t font = fbe_text_load_font(path.c_str(), 0, size, 0);
    INFO(fbe_error());
    REQUIRE(font != 0);
    fbe_text_set_system_fallback(font, false);
    return font;
}

/// The format of the page `text`'s one glyph in `font` is on.
fbe::text::page_format_t format_of(std::uint64_t font, const char* text) {
    fbe::text::layout_t laid;
    REQUIRE(fbe::text::lay_out(font, text, 1.0F, laid));
    REQUIRE(laid.glyphs.size() == 1);
    return fbe::text::atlas().pages()[laid.glyphs[0].page].format;
}

/// Whether `glyph`'s place on its page has coverage, and a clear pixel all around it.
bool drawn(const fbe::text::placed_glyph_t& glyph) {
    const fbe::text::page_t& page = fbe::text::atlas().pages()[glyph.page];
    const fbe::text::rect_t& place = glyph.source;
    const std::size_t channels = page.format == fbe::text::page_format_t::COLOR ? 4 : 1;
    const auto at = [&](std::int32_t x, std::int32_t y) {
        const std::size_t first =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(page.width)) +
            static_cast<std::size_t>(x);
        const std::size_t pixel = first * channels;
        return page.pixels[pixel + channels - 1];
    };
    bool covered = false;
    for (std::int32_t y = place.y; y < place.y + place.height; ++y) {
        for (std::int32_t x = place.x; x < place.x + place.width; ++x) {
            covered = covered || at(x, y) != 0;
        }
    }
    for (std::int32_t x = place.x - 1; x <= place.x + place.width; ++x) {
        if (at(x, place.y - 1) != 0 || at(x, place.y + place.height) != 0) {
            return false;
        }
    }
    for (std::int32_t y = place.y; y < place.y + place.height; ++y) {
        if (at(place.x - 1, y) != 0 || at(place.x + place.width, y) != 0) {
            return false;
        }
    }
    return covered;
}

} // namespace

TEST_CASE("a text laid out is its glyphs placed on the atlas's pages") {
    fbe::text::atlas().clear();
    const std::uint64_t font = opened("OpenSans-Regular.ttf", 20.0F);
    fbe::text::layout_t layout;
    REQUIRE(fbe::text::lay_out(font, "Hello", 1.0F, layout));
    REQUIRE(layout.glyphs.size() == 5);
    CHECK(layout.generation == fbe::text::atlas().generation());
    std::int32_t before = -1;
    for (const fbe::text::placed_glyph_t& glyph : layout.glyphs) {
        const fbe::text::page_t& page = fbe::text::atlas().pages()[glyph.page];
        CHECK(page.format == fbe::text::page_format_t::COVERAGE);
        CHECK(drawn(glyph));
        CHECK(glyph.x > before);
        before = glyph.x;
        // Above the baseline, which is at 0.
        CHECK(glyph.y < 0);
        CHECK(glyph.y + glyph.source.height <= 1);
    }
    // The pages say what's new on them.
    const fbe::text::page_t& page = fbe::text::atlas().pages()[0];
    CHECK(page.version > 0);
    CHECK_FALSE(page.dirty.empty());
    // Its width is the measured width, and its height a line.
    fbe_text_size_t size{};
    REQUIRE(fbe_text_measure(font, "Hello", &size));
    CHECK(layout.width == Catch::Approx(size.width));
    CHECK(layout.height == Catch::Approx(size.height));
    CHECK(layout.ascent > 0.0F);
    fbe_text_free_font(font);
}

TEST_CASE("a glyph's rasterized once, and laid out again it's where it was") {
    fbe::text::atlas().clear();
    const std::uint64_t font = opened("OpenSans-Regular.ttf", 20.0F);
    fbe::text::layout_t first;
    REQUIRE(fbe::text::lay_out(font, "Hello", 1.0F, first));
    fbe::text::atlas().page(0).dirty = {};
    const std::uint64_t version = fbe::text::atlas().pages()[0].version;
    fbe::text::layout_t again;
    REQUIRE(fbe::text::lay_out(font, "Hello", 1.0F, again));
    CHECK(fbe::text::atlas().pages()[0].version == version);
    CHECK(fbe::text::atlas().pages()[0].dirty.empty());
    REQUIRE(again.glyphs.size() == first.glyphs.size());
    for (std::size_t at = 0; at < first.glyphs.size(); ++at) {
        CHECK(again.glyphs[at].source.x == first.glyphs[at].source.x);
        CHECK(again.glyphs[at].source.y == first.glyphs[at].source.y);
        CHECK(again.glyphs[at].x == first.glyphs[at].x);
    }
    // Both l's are one glyph, at the same quarter of a pixel or not.
    CHECK(fbe::text::atlas().pages().size() == 1);
    fbe_text_free_font(font);
}

TEST_CASE("a scale's glyphs are rasterized at its pixels") {
    const std::uint64_t font = opened("OpenSans-Regular.ttf", 20.0F);
    fbe::text::layout_t one;
    fbe::text::layout_t two;
    REQUIRE(fbe::text::lay_out(font, "H", 1.0F, one));
    REQUIRE(fbe::text::lay_out(font, "H", 2.0F, two));
    REQUIRE(one.glyphs.size() == 1);
    REQUIRE(two.glyphs.size() == 1);
    CHECK(two.width == Catch::Approx(one.width * 2.0F));
    CHECK(two.glyphs[0].source.height >= (one.glyphs[0].source.height * 2) - 2);
    CHECK(two.ascent == Catch::Approx(one.ascent * 2.0F));
    // Bad scales and freed fonts are refused.
    fbe::text::layout_t none;
    CHECK_FALSE(fbe::text::lay_out(font, "H", 0.0F, none));
    fbe_text_free_font(font);
    CHECK_FALSE(fbe::text::lay_out(font, "H", 1.0F, none));
    CHECK(std::string(fbe_error()) == "the font was freed");
}

TEST_CASE("a space takes room but no place, and lines go down by the line height") {
    const std::uint64_t font = opened("OpenSans-Regular.ttf", 20.0F);
    fbe::text::layout_t spaced;
    REQUIRE(fbe::text::lay_out(font, "a b", 1.0F, spaced));
    REQUIRE(spaced.glyphs.size() == 2);
    fbe_text_size_t a_space{};
    REQUIRE(fbe_text_measure(font, "a ", &a_space));
    CHECK(static_cast<float>(spaced.glyphs[1].x) >= a_space.width - 2.0F);
    fbe::text::layout_t lines;
    REQUIRE(fbe::text::lay_out(font, "a\na", 1.0F, lines));
    REQUIRE(lines.glyphs.size() == 2);
    fbe_text_metrics_t metrics{};
    REQUIRE(fbe_text_font_metrics(font, &metrics));
    CHECK(static_cast<float>(lines.glyphs[1].y - lines.glyphs[0].y) ==
          Catch::Approx(metrics.line_height).margin(1.0));
    CHECK(lines.glyphs[1].x == lines.glyphs[0].x);
    CHECK(lines.height == Catch::Approx(metrics.line_height * 2.0F));
    fbe_text_free_font(font);
}

TEST_CASE("a right-to-left run is turned around, and a fallback gives what a font hasn't") {
    const std::uint64_t latin = opened("OpenSans-Regular.ttf", 20.0F);
    const std::uint64_t arabic = opened("Mada-VF.ttf", 20.0F);
    // "marhaban", in Arabic, then "ab".
    const char* mixed = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 ab";
    CHECK_FALSE(fbe_text_has_glyphs(latin, mixed));
    REQUIRE(fbe_text_add_fallback(latin, arabic));
    CHECK(fbe_text_has_glyphs(latin, mixed));
    fbe::text::layout_t ab;
    REQUIRE(fbe::text::lay_out(latin, "ab", 1.0F, ab));
    fbe::text::layout_t laid;
    REQUIRE(fbe::text::lay_out(latin, mixed, 1.0F, laid));
    // The paragraph's right to left, by its first letter, so "ab" is at its left, first.
    REQUIRE(laid.glyphs.size() > 2);
    CHECK(laid.glyphs[0].source.x == ab.glyphs[0].source.x);
    CHECK(laid.glyphs[0].source.y == ab.glyphs[0].source.y);
    CHECK(laid.glyphs[0].x == ab.glyphs[0].x);
    CHECK(laid.glyphs[1].x == ab.glyphs[1].x);
    for (std::size_t at = 2; at < laid.glyphs.size(); ++at) {
        CHECK(laid.glyphs[at].x > laid.glyphs[1].x);
    }
    CHECK_FALSE(fbe_text_add_fallback(latin, latin));
    fbe_text_free_font(latin);
    fbe_text_free_font(arabic);
}

TEST_CASE("a color font's glyphs are on a color page") {
    const std::uint64_t font = opened("chromacheck-colr.ttf", 32.0F);
    fbe::text::font_t* found = fbe::text::font_of(font);
    REQUIRE(found != nullptr);
    FT_UInt index = 0;
    const FT_ULong first = FT_Get_First_Char(found->face->ft, &index);
    REQUIRE(index != 0);
    std::string text;
    // The first character it maps, as UTF-8.
    if (first < 0x80) {
        text += static_cast<char>(first);
    } else if (first < 0x800) {
        text += static_cast<char>(0xC0U | (first >> 6U));
        text += static_cast<char>(0x80U | (first & 0x3FU));
    } else {
        text += static_cast<char>(0xE0U | (first >> 12U));
        text += static_cast<char>(0x80U | ((first >> 6U) & 0x3FU));
        text += static_cast<char>(0x80U | (first & 0x3FU));
    }
    fbe::text::layout_t laid;
    REQUIRE(fbe::text::lay_out(font, text, 1.0F, laid));
    REQUIRE(laid.glyphs.size() == 1);
    const fbe::text::page_t& page = fbe::text::atlas().pages()[laid.glyphs[0].page];
    CHECK(page.format == fbe::text::page_format_t::COLOR);
    CHECK(page.pixels.size() ==
          static_cast<std::size_t>(page.width) * static_cast<std::size_t>(page.height) * 4);
    CHECK(drawn(laid.glyphs[0]));
    // Straight, not premultiplied, rounded, and none for nothing covered.
    CHECK(fbe::text::straight(64, 128) == 128);
    CHECK(fbe::text::straight(100, 200) == 128);
    CHECK(fbe::text::straight(255, 255) == 255);
    CHECK(fbe::text::straight(0, 0) == 0);
    CHECK(fbe::text::straight(200, 100) == 255);
    fbe_text_free_font(font);
}

TEST_CASE("a glyph bigger than a page has one of its own, and trim clears past the budget") {
    fbe::text::atlas().clear();
    const std::uint64_t font = opened("OpenSans-Regular.ttf", 1500.0F);
    fbe::text::layout_t huge;
    REQUIRE(fbe::text::lay_out(font, "W", 1.0F, huge));
    REQUIRE(huge.glyphs.size() == 1);
    const fbe::text::page_t& page = fbe::text::atlas().pages()[huge.glyphs[0].page];
    CHECK(page.width >= huge.glyphs[0].source.width + 2);
    CHECK(drawn(huge.glyphs[0]));
    const std::uint64_t generation = fbe::text::atlas().generation();
    fbe::text::atlas().trim();
    CHECK(fbe::text::atlas().generation() == generation);
    // Past its budget, a page the last frame used stays.
    fbe::text::atlas().set_budget(1);
    REQUIRE(fbe::text::lay_out(font, "W", 1.0F, huge));
    fbe::text::atlas().trim();
    CHECK(fbe::text::atlas().generation() == generation);
    // A frame that doesn't use it: dropped, its pixels let go of, its number kept, and what was
    // written on it, which the canvas hadn't taken, not to upload, as nothing samples it.
    CHECK_FALSE(fbe::text::atlas().pages()[0].dirty.empty());
    fbe::text::atlas().trim();
    CHECK(fbe::text::atlas().generation() == generation + 1);
    REQUIRE(fbe::text::atlas().pages().size() == 1);
    CHECK(fbe::text::atlas().pages()[0].dropped());
    CHECK(fbe::text::atlas().pages()[0].pixels.capacity() == 0);
    CHECK(fbe::text::atlas().pages()[0].dirty.empty());
    // Dropped, it's never trimmed again.
    fbe::text::atlas().trim();
    CHECK(fbe::text::atlas().generation() == generation + 1);
    fbe::text::atlas().set_budget(std::size_t{ 16 } << 20U);
    // Laid out again, it's rasterized again, on that page made again, only the glyph to upload.
    const std::uint64_t version = fbe::text::atlas().pages()[0].version;
    REQUIRE(fbe::text::lay_out(font, "W", 1.0F, huge));
    CHECK(huge.generation == generation + 1);
    REQUIRE(fbe::text::atlas().pages().size() == 1);
    const fbe::text::page_t& again = fbe::text::atlas().pages()[0];
    CHECK_FALSE(again.dropped());
    CHECK(again.dirty.x == huge.glyphs[0].source.x);
    CHECK(again.dirty.y == huge.glyphs[0].source.y);
    CHECK(again.dirty.width == huge.glyphs[0].source.width);
    CHECK(again.dirty.height == huge.glyphs[0].source.height);
    CHECK(again.version > version);
    CHECK(drawn(huge.glyphs[0]));
    fbe_text_free_font(font);
}

TEST_CASE("a font's faces are read once, its sizes share them, and its handles are checked") {
    const std::uint64_t font = opened("OpenSans-Regular.ttf", 12.0F);
    const std::uint64_t again = opened("OpenSans-Regular.ttf", 30.0F);
    const std::uint64_t sized = fbe_text_resize_font(font, 18.0F, FBE_TEXT_BOLD);
    REQUIRE(sized != 0);
    CHECK(fbe::text::font_of(font)->face == fbe::text::font_of(again)->face);
    CHECK(fbe::text::font_of(sized)->face == fbe::text::font_of(font)->face);
    CHECK(fbe::text::font_of(sized)->bolder);
    CHECK(fbe_text_font_style(sized) == FBE_TEXT_BOLD);
    const std::int32_t count = fbe_text_font_count();
    CHECK(fbe_text_free_font(sized));
    CHECK_FALSE(fbe_text_free_font(sized));
    CHECK(fbe_text_font_count() == count - 1);
    // A handle of another kind, or none, isn't a font's.
    CHECK(fbe::text::font_of((std::uint64_t{ 5 } << 56U) | (std::uint64_t{ 1 } << 32U)) == nullptr);
    CHECK(fbe::text::font_of(0) == nullptr);
    CHECK(std::string(fbe_error()) == "no font was given");
    CHECK(fbe_text_load_font("no such font.ttf", 0, 12.0F, 0) == 0);
    CHECK(std::string(fbe_error()) == "couldn't open no such font.ttf");
    CHECK(fbe_text_load_font("x.ttf", 0, -1.0F, 0) == 0);
    CHECK(std::string(fbe_error()) == "a font's size is over 0, not -1");
    fbe_text_free_font(font);
    fbe_text_free_font(again);
}

TEST_CASE("a paragraph or line separator breaks a line, and NEL and a vertical tab do") {
    const std::uint64_t font = opened("OpenSans-Regular.ttf", 20.0F);
    fbe_text_size_t one{};
    REQUIRE(fbe_text_measure(font, "a", &one));
    // Each separator's literal ends before the b, which would be another of its hex digits.
    const std::array<const char*, 4> separated{
        "a\xE2\x80\xA9"
        "b",
        "a\xC2\x85"
        "b",
        "a\xE2\x80\xA8"
        "b",
        "a\x0B"
        "b",
    };
    for (const char* text : separated) {
        fbe_text_size_t size{};
        REQUIRE(fbe_text_measure(font, text, &size));
        CHECK(size.height == Catch::Approx(one.height * 2.0F));
        fbe::text::layout_t laid;
        REQUIRE(fbe::text::lay_out(font, text, 1.0F, laid));
        REQUIRE(laid.glyphs.size() == 2);
        CHECK(laid.glyphs[1].y > laid.glyphs[0].y);
    }
    // C1's other controls take no room.
    fbe_text_size_t controlled{};
    REQUIRE(fbe_text_measure(font, "a\xC2\x80\xC2\x9F", &controlled));
    CHECK(controlled.width == Catch::Approx(one.width));
    fbe_text_free_font(font);
}

TEST_CASE("past 512 pixels an em, glyphs are rasterized at 512 and drawn bigger") {
    fbe::text::atlas().clear();
    const std::uint64_t font = opened("OpenSans-Regular.ttf", 20000.0F);
    fbe::text::layout_t huge;
    REQUIRE(fbe::text::lay_out(font, "W", 1.0F, huge));
    REQUIRE(huge.glyphs.size() == 1);
    const fbe::text::placed_glyph_t& glyph = huge.glyphs[0];
    // Rasterized at 512, about a 40th the size, and placed at its full size.
    CHECK(glyph.source.width < 600);
    CHECK(glyph.width > glyph.source.width * 30);
    CHECK(glyph.height > glyph.source.height * 30);
    std::size_t bytes = 0;
    for (const fbe::text::page_t& page : fbe::text::atlas().pages()) {
        bytes += page.pixels.size();
    }
    CHECK(bytes <= std::size_t{ 1024 } * 1024);
    // At a size it rasterizes, the quad is the source's.
    const std::uint64_t small = opened("OpenSans-Regular.ttf", 20.0F);
    fbe::text::layout_t laid;
    REQUIRE(fbe::text::lay_out(small, "W", 1.0F, laid));
    CHECK(laid.glyphs[0].width == laid.glyphs[0].source.width);
    CHECK(laid.glyphs[0].height == laid.glyphs[0].source.height);
    // A size past a float's is refused, and a far line is kept to the canvas's reach.
    CHECK_FALSE(fbe::text::lay_out(font, "W", 3e38F, huge));
    std::string lines(100, '\n');
    lines += 'W';
    const std::uint64_t vast = opened("OpenSans-Regular.ttf", 1e6F);
    REQUIRE(fbe::text::lay_out(vast, lines, 1.0F, laid));
    CHECK(laid.glyphs[0].y >= -16777216 - laid.glyphs[0].height);
    fbe_text_free_font(font);
    fbe_text_free_font(small);
    fbe_text_free_font(vast);
}

TEST_CASE("a face that isn't one, and a font that isn't TrueType or OpenType, are refused") {
    const std::string path = std::string(FBE_TEST_FONTS) + "/OpenSans-Regular.ttf";
    CHECK(fbe_text_load_font(path.c_str(), -1, 12.0F, 0) == 0);
    CHECK(std::string(fbe_error()) == "a font's face is 0 to 65,535, not -1");
    CHECK(fbe_text_load_font(path.c_str(), 65536, 12.0F, 0) == 0);
    // A BDF bitmap font: FreeType's own module for it isn't built in.
    const char* bdf = "STARTFONT 2.1\nFONT x\nSIZE 8 75 75\nFONTBOUNDINGBOX 8 8 0 0\n"
                      "CHARS 1\nSTARTCHAR a\nENCODING 97\nSWIDTH 500 0\nDWIDTH 8 0\n"
                      "BBX 8 8 0 0\nBITMAP\nFF\nFF\nFF\nFF\nFF\nFF\nFF\nFF\nENDCHAR\nENDFONT\n";
    const std::string bytes(bdf);
    CHECK(fbe_text_load_font_bytes(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                   static_cast<std::int64_t>(bytes.size()), "a.bdf", 0, 12.0F,
                                   0) == 0);
    CHECK(std::string(fbe_error()) == "a.bdf isn't a font");
}

TEST_CASE("an emoji is in color first, and text where FE0E asks") {
    const std::uint64_t text = own("FbeTestText-Regular.ttf", 32.0F);
    const std::uint64_t color = own("FbeTestColor-Regular.ttf", 32.0F);
    REQUIRE(fbe_text_add_fallback(text, color));
    using fbe::text::page_format_t;
    // A watch is an emoji, its color face's before the font's own; with FE0E, the font's.
    CHECK(format_of(text, "\xE2\x8C\x9A") == page_format_t::COLOR);
    CHECK(format_of(text, "\xE2\x8C\x9A\xEF\xB8\x8E") == page_format_t::COVERAGE);
    // A private character is text, unless FE0F asks for an emoji.
    CHECK(format_of(text, "\xEE\xA4\x83") == page_format_t::COVERAGE);
    CHECK(format_of(text, "\xEE\xA4\x83\xEF\xB8\x8F") == page_format_t::COLOR);
    fbe_text_free_font(text);
    fbe_text_free_font(color);
}

TEST_CASE("a mark's cluster is the first face's that has it whole") {
    const std::uint64_t text = own("FbeTestText-Regular.ttf", 100.0F);
    const std::uint64_t marks = own("FbeTestMarks-Regular.ttf", 100.0F);
    REQUIRE(fbe_text_add_fallback(text, marks));
    fbe_text_size_t size{};
    // The text face's a is 50 across, the marks face's 52; with its low line, the marks face's.
    REQUIRE(fbe_text_measure(text, "a", &size));
    CHECK(size.width == Catch::Approx(50.0F));
    REQUIRE(fbe_text_measure(text, "a\xCC\xB2", &size));
    CHECK(size.width == Catch::Approx(52.0F));
    CHECK(fbe_text_has_glyphs(text, "a\xCC\xB2"));
    // b, which the marks face hasn't, keeps its own face, and its mark the first with it.
    REQUIRE(fbe_text_measure(text, "b\xCC\xB2", &size));
    CHECK(size.width == Catch::Approx(60.0F));
    fbe_text_free_font(text);
    fbe_text_free_font(marks);
}

TEST_CASE("a glyph FreeType can't draw in color is drawn as its outline, or not chosen") {
    // An SVG picture, which FreeType isn't built to draw here, over an outline: the outline.
    const std::uint64_t svg = own("FbeTestSvg-Regular.ttf", 32.0F);
    CHECK(format_of(svg, "\xEE\xA4\x83") == fbe::text::page_format_t::COVERAGE);
    // A picture over an empty outline, as sbix's PNGs often are, draws nothing, so the font doesn't
    // have it, and a fallback that draws it is taken.
    CHECK_FALSE(fbe_text_has_glyphs(svg, "\xEE\xA4\x84"));
    const std::string sbix =
        std::string(FBE_SHAPE_TEST_FONTS) + "/fcbaa518d3cce441ed37ae3b1fed6a19e9b54efd.ttf";
    const std::uint64_t pictured = fbe_text_load_font(sbix.c_str(), 0, 32.0F, 0);
    REQUIRE(pictured != 0);
    fbe_text_set_system_fallback(pictured, false);
    CHECK_FALSE(fbe_text_has_glyphs(pictured, "\xF0\x9F\x98\x80"));
    // CBDT's alone, no outlines: its keycap isn't one the font draws.
    const std::uint64_t text = own("FbeTestText-Regular.ttf", 32.0F);
    const std::uint64_t bitmapped = opened("NotoColorEmoji.subset.ttf", 32.0F);
    REQUIRE(fbe_text_add_fallback(text, bitmapped));
    CHECK_FALSE(fbe_text_has_glyphs(text, "\xE2\x83\xA3"));
    // A space draws nothing, and has a glyph all the same.
    CHECK(fbe_text_has_glyphs(text, "a b"));
    fbe_text_free_font(svg);
    fbe_text_free_font(pictured);
    fbe_text_free_font(text);
    fbe_text_free_font(bitmapped);
}

TEST_CASE("a character no font has reads no fallback face whole") {
    const std::uint64_t text = own("FbeTestText-Regular.ttf", 32.0F);
    fbe_text_set_system_fallback(text, true);
    // The OS's fonts listed, and their coverage read, before counting.
    CHECK_FALSE(fbe_text_has_glyphs(text, "\xF3\xB0\x80\x80"));
    const std::size_t before = fbe::text::faces_open();
    CHECK_FALSE(fbe_text_has_glyphs(text, "\xF3\xB0\x80\x81"));
    fbe_text_size_t size{};
    REQUIRE(fbe_text_measure(text, "\xF3\xB0\x80\x82", &size));
    CHECK(fbe::text::faces_open() == before);
    fbe_text_free_font(text);
}

TEST_CASE("a frame that needs more than the budget isn't rasterized again each frame") {
    fbe::text::atlas().clear();
    fbe::text::atlas().set_budget(1);
    const std::uint64_t big = opened("OpenSans-Regular.ttf", 300.0F);
    const std::uint64_t small = opened("OpenSans-Regular.ttf", 12.0F);
    fbe::text::layout_t laid;
    // Each frame lays out the same text, past the budget; another text, once, then not again.
    REQUIRE(fbe::text::lay_out(small, "once", 1.0F, laid));
    const std::uint32_t once_page = laid.glyphs[0].page;
    REQUIRE(fbe::text::lay_out(big, "Text", 1.0F, laid));
    const std::uint32_t text_page = laid.glyphs[0].page;
    fbe::text::atlas().trim();
    std::uint64_t versions = 0;
    for (int frame = 0; frame < 5; ++frame) {
        REQUIRE(fbe::text::lay_out(big, "Text", 1.0F, laid));
        if (frame == 1) {
            versions = fbe::text::atlas().pages()[text_page].version;
        }
        fbe::text::atlas().trim();
    }
    CHECK(fbe::text::atlas().pages()[text_page].version == versions);
    if (once_page != text_page) {
        CHECK(fbe::text::atlas().pages()[once_page].dropped());
    }
    fbe::text::atlas().set_budget(std::size_t{ 16 } << 20U);
    fbe_text_free_font(big);
    fbe_text_free_font(small);
}

TEST_CASE("a size past a float's lines and advances is refused, and one short of it is placed") {
    const std::uint64_t font = opened("OpenSans-Regular.ttf", 20.0F);
    // 100,000,000 pixels an em: each glyph's quad and place kept to the canvas's reach.
    fbe::text::layout_t laid;
    REQUIRE(fbe::text::lay_out(font, "WW\nW", 5e6F, laid));
    REQUIRE(laid.glyphs.size() == 3);
    for (const fbe::text::placed_glyph_t& glyph : laid.glyphs) {
        CHECK(glyph.width > 0);
        CHECK(glyph.width <= 16777216);
        CHECK(glyph.height > 0);
        CHECK(glyph.height <= 16777216);
        CHECK(glyph.x >= -16777216);
        CHECK(glyph.x <= 16777216);
        CHECK(glyph.y >= -16777216);
        CHECK(glyph.y <= 16777216);
    }
    // 3e38 pixels an em, a float's still, but not its line height.
    CHECK_FALSE(fbe::text::lay_out(font, "W\nW", 1.5e37F, laid));
    CHECK(std::string(fbe_error()) ==
          "a font's size times its scale is past what its lines and advances can be");
    const std::uint64_t vast = opened("OpenSans-Regular.ttf", 3e38F);
    fbe_text_size_t size{};
    CHECK_FALSE(fbe_text_measure(vast, "W", &size));
    fbe_text_free_font(font);
    fbe_text_free_font(vast);
}

TEST_CASE("a glyph past 2048 pixels across or down is rasterized smaller and drawn bigger") {
    fbe::text::atlas().clear();
    // H is 15 ems across and down: 6,000 pixels at 400.
    const std::uint64_t font = own("FbeTestHuge-Regular.ttf", 400.0F);
    fbe::text::layout_t laid;
    REQUIRE(fbe::text::lay_out(font, "H", 1.0F, laid));
    REQUIRE(laid.glyphs.size() == 1);
    const fbe::text::placed_glyph_t glyph = laid.glyphs[0];
    CHECK(glyph.source.width <= 2048);
    CHECK(glyph.source.height <= 2048);
    CHECK(glyph.width > 5900);
    CHECK(glyph.height > 5900);
    CHECK(drawn(glyph));
    std::size_t bytes = 0;
    for (const fbe::text::page_t& page : fbe::text::atlas().pages()) {
        bytes += page.pixels.size();
    }
    CHECK(bytes <= std::size_t{ 2050 } * 2050);
    // After a, 200.4 pixels across, at another quarter, it's the same glyph.
    REQUIRE(fbe::text::lay_out(font, "aH", 1.0F, laid));
    REQUIRE(laid.glyphs.size() == 2);
    CHECK(laid.glyphs[1].source.x == glyph.source.x);
    CHECK(laid.glyphs[1].source.y == glyph.source.y);
    fbe_text_free_font(font);
}

TEST_CASE("an sbix PNG FreeType can't read is passed over for the outline under it") {
    // A PNG of a at 32 pixels an em; FreeType takes an sbix font as its bitmaps alone, unless
    // it's told not to.
    for (const float size : { 20.0F, 32.0F, 50.0F }) {
        const std::uint64_t font = own("FbeTestSbix-Regular.ttf", size);
        INFO("at " << size);
        CHECK(fbe_text_has_glyphs(font, "a"));
        fbe::text::layout_t laid;
        REQUIRE(fbe::text::lay_out(font, "a", 1.0F, laid));
        REQUIRE(laid.glyphs.size() == 1);
        CHECK(fbe::text::atlas().pages()[laid.glyphs[0].page].format ==
              fbe::text::page_format_t::COVERAGE);
        CHECK(drawn(laid.glyphs[0]));
        fbe_text_free_font(font);
    }
}

TEST_CASE("a color glyph is rasterized once, at a whole pixel, without a made bolder") {
    fbe::text::atlas().clear();
    // a is half an em: at 33, 16.5 pixels, so what's after it is half a pixel across.
    const std::uint64_t color = own("FbeTestColor-Regular.ttf", 33.0F);
    fbe::text::layout_t alone;
    REQUIRE(fbe::text::lay_out(color, "\xE2\x8C\x9A", 1.0F, alone));
    REQUIRE(alone.glyphs.size() == 1);
    REQUIRE(fbe::text::atlas().pages()[alone.glyphs[0].page].format ==
            fbe::text::page_format_t::COLOR);
    fbe::text::layout_t after;
    REQUIRE(fbe::text::lay_out(color, "a\xE2\x8C\x9A", 1.0F, after));
    REQUIRE(after.glyphs.size() == 2);
    const fbe::text::placed_glyph_t& moved = after.glyphs[1];
    CHECK(moved.source.x == alone.glyphs[0].source.x);
    CHECK(moved.source.y == alone.glyphs[0].source.y);
    CHECK(moved.page == alone.glyphs[0].page);
    // At the nearest pixel, 16.5's 17.
    CHECK(moved.x - alone.glyphs[0].x == 17);
    // Bold, a face that isn't is made bolder, but not its color glyphs.
    const std::string path = std::string(FBE_OWN_TEST_FONTS) + "/FbeTestColor-Regular.ttf";
    const std::uint64_t bold =
        fbe_text_load_font(path.c_str(), 0, 33.0F, static_cast<std::int32_t>(fbe::text::bold_bit));
    REQUIRE(bold != 0);
    fbe_text_set_system_fallback(bold, false);
    fbe::text::layout_t bolder;
    REQUIRE(fbe::text::lay_out(bold, "\xE2\x8C\x9A", 1.0F, bolder));
    REQUIRE(bolder.glyphs.size() == 1);
    CHECK(bolder.glyphs[0].source.x == alone.glyphs[0].source.x);
    CHECK(bolder.glyphs[0].source.y == alone.glyphs[0].source.y);
    // An outline's still moved a quarter, and made bolder.
    fbe::text::layout_t letters;
    REQUIRE(fbe::text::lay_out(color, "aa", 1.0F, letters));
    REQUIRE(letters.glyphs.size() == 2);
    CHECK(letters.glyphs[0].source.x != letters.glyphs[1].source.x);
    REQUIRE(fbe::text::lay_out(bold, "a", 1.0F, bolder));
    CHECK(bolder.glyphs[0].source.width > letters.glyphs[0].source.width);
    fbe_text_free_font(color);
    fbe_text_free_font(bold);
}
