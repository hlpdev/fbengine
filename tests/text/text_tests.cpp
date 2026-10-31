// text's atlas and layouts, apart from the DLL, as the canvas reads them, on HarfBuzz's own test
// fonts: glyphs placed on pages at whole pixels, rasterized once, at a scale's pixels, color glyphs
// on color pages, straight, right-to-left runs turned around, and the atlas trimmed past its
// budget.

#include "text/fbe_text.h"
#include "text/font.hpp"
#include "text/text.hpp"

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
    fbe::text::atlas().set_budget(1);
    fbe::text::atlas().trim();
    CHECK(fbe::text::atlas().generation() == generation + 1);
    CHECK(fbe::text::atlas().pages().empty());
    fbe::text::atlas().set_budget(std::size_t{ 16 } << 20U);
    // Laid out again, it's rasterized again.
    REQUIRE(fbe::text::lay_out(font, "W", 1.0F, huge));
    CHECK(huge.generation == generation + 1);
    CHECK(fbe::text::atlas().pages().size() == 1);
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
