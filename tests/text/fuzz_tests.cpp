// text's fonts and layouts against what a game could be given: fonts whose bytes are damaged,
// mutated from the tests' own by a seeded generator, so a failure's run again; UTF-8 that's
// invalid, overlong, or a surrogate, combining marks by the thousand, bidi controls unbalanced,
// huge strings, and scales past reason. Each is refused with an error or laid out with its glyphs
// in reach, never a crash, a hang, or a read out of bounds; the sanitizers' builds show the last.
// tests/text/corpus/ keeps the inputs found to have broken it once, each laid out again here.

#include "text/fbe_text.h"
#include "text/font.hpp"
#include "text/text.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// A seeded generator, splitmix64, the same on every target.
class random_t {
public:
    explicit random_t(std::uint64_t seed) : m_state(seed) {}

    std::uint64_t next() {
        m_state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t mixed = m_state;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
        return mixed ^ (mixed >> 31U);
    }

    /// From 0 to `below`, less 1.
    std::size_t below(std::size_t below) {
        return static_cast<std::size_t>(next() % below);
    }

private:
    std::uint64_t m_state;
};

std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// Each glyph of `laid` on a page, in the place on it it says, and its quad in the canvas's reach.
void in_reach(const fbe::text::layout_t& laid) {
    const auto pages = fbe::text::atlas().pages();
    for (const fbe::text::placed_glyph_t& glyph : laid.glyphs) {
        REQUIRE(glyph.page < pages.size());
        const fbe::text::page_t& page = pages[glyph.page];
        REQUIRE(glyph.source.x >= 0);
        REQUIRE(glyph.source.y >= 0);
        REQUIRE(glyph.source.width >= 0);
        REQUIRE(glyph.source.height >= 0);
        REQUIRE(glyph.source.x + glyph.source.width <= page.width);
        REQUIRE(glyph.source.y + glyph.source.height <= page.height);
        REQUIRE(std::abs(static_cast<std::int64_t>(glyph.x)) <= 16777216);
        REQUIRE(std::abs(static_cast<std::int64_t>(glyph.y)) <= 16777216);
        REQUIRE(glyph.width >= 0);
        REQUIRE(glyph.height >= 0);
    }
    REQUIRE(std::isfinite(laid.width));
    REQUIRE(std::isfinite(laid.height));
}

/// Lays `text` out in `font` at `scale`, and measures it: refused, or each glyph in reach.
void tried(std::uint64_t font, std::string_view text, float scale) {
    fbe::text::layout_t laid;
    if (fbe::text::lay_out(font, text, scale, laid)) {
        in_reach(laid);
    }
    float width = 0.0F;
    float height = 0.0F;
    if (fbe::text::measure(font, text, width, height)) {
        REQUIRE(std::isfinite(width));
        REQUIRE(std::isfinite(height));
    }
    const std::string terminated(text.substr(0, text.find('\0')));
    fbe_text_has_glyphs(font, terminated.c_str());
}

/// Texts a game could be given: what a player types, a file's lines, a translator's mistakes.
std::vector<std::string> nasty() {
    std::vector<std::string> texts = {
        "", "\n\n\n", "\r\n\r\n", "\t\v\f\x1C\x1D\x1E\x85", std::string("a\0b\0c", 5), "\x80",
        "\xBF\xBF\xBF",
        "\xC0\x80",                 // an overlong NUL
        "\xC1\xBF",                 // an overlong '\x7F'
        "\xE0\x80\xAF",             // an overlong '/'
        "\xF0\x80\x80\xAF",         // and in four
        "\xED\xA0\x80",             // a high surrogate
        "\xED\xBF\xBF",             // a low one
        "\xED\xA0\xBD\xED\xB8\x80", // a pair, as CESU-8 writes it
        "\xF4\x90\x80\x80",         // past U+10FFFF
        "\xF8\x88\x80\x80\x80",     // five bytes
        "\xFC\x84\x80\x80\x80\x80", // six
        "\xFE\xFF", "\xEF\xBB\xBF\x61 BOM",
        "abc\xE2\x82",                     // cut short at its end
        "\xE2\x82\x61\x62\x63",            // and at its start
        "\xEF\xBF\xBE\xEF\xBF\xBF",        // U+FFFE, U+FFFF
        "\xE2\x80\xAE\x65vil\xE2\x80\xAC", // RLO, PDF
        // NOLINTNEXTLINE(misc-misleading-bidirectional): left open, as the test means
        "\xE2\x80\xAE\xE2\x80\xAE\xE2\x80\xAEunclosed", // overrides never closed
        // NOLINTNEXTLINE(misc-misleading-bidirectional): left open, as the test means
        "\xE2\x81\xA6\xE2\x81\xA7\xE2\x81\xA8isolates",     // LRI RLI FSI, never closed
        "\xE2\x81\xA9\xE2\x81\xA9\xE2\x80\xAC\xE2\x80\xAC", // closings with nothing open
        "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D abc \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 123",
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7", // a ZWJ family
        "\xE2\x80\x8D\xE2\x80\x8D\xE2\x80\x8C\xE2\x80\x8C",                         // joiners alone
        "\xEF\xB8\x8F\xEF\xB8\x8E\xEF\xB8\x8F", // selectors alone
        "\xF3\xA0\x80\x81\xF3\xA0\x81\xA1",     // tags
    };
    // Combining marks by the thousand, on one base, and alone.
    std::string marks = "a";
    for (int i = 0; i < 5000; ++i) {
        marks += "\xCC\x81";
    }
    texts.push_back(marks);
    texts.push_back(marks.substr(1));
    // Thousands of bidi embeddings, deeper than the algorithm's 125 levels.
    std::string deep;
    for (int i = 0; i < 2000; ++i) {
        // NOLINTNEXTLINE(misc-misleading-bidirectional): left open, as the test means
        deep += (i % 2 == 0) ? "\xE2\x80\xAB" : "\xE2\x81\xA7";
    }
    texts.push_back(deep + "x");
    // A megabyte on one line, and one of line breaks.
    texts.emplace_back(std::size_t{ 1 } << 20U, 'W');
    texts.emplace_back(std::size_t{ 1 } << 20U, '\n');
    // Every byte, in order, and every two-byte sequence's start with every continuation.
    std::string every;
    for (int byte = 1; byte < 256; ++byte) {
        every += static_cast<char>(byte);
    }
    texts.push_back(every);
    return texts;
}

/// Random bytes, mostly UTF-8's own, so they make sequences, broken and whole.
std::string random_text(random_t& random) {
    static constexpr std::array<std::string_view, 12> pieces = {
        "a",
        " ",
        "\n",
        "\xCC\x81",
        "\xD9\x85",
        // NOLINTNEXTLINE(misc-misleading-bidirectional): left open, as the test means
        "\xE2\x80\xAE",
        "\xE2\x81\xA9",
        "\xF0\x9F\x98\x80",
        "\xE2\x80\x8D",
        "\xEF\xB8\x8F",
        "\xED\xA0\x80",
        "\xF4\x8F\xBF\xBF",
    };
    std::string text;
    const std::size_t length = random.below(64);
    for (std::size_t i = 0; i < length; ++i) {
        if (random.below(3) == 0) {
            text += static_cast<char>(random.below(256));
        } else {
            text += pieces[random.below(pieces.size())];
        }
    }
    return text;
}

/// `font`'s bytes damaged: bytes flipped, a number in its table directory or a table set to an
/// edge, or cut short.
std::vector<std::uint8_t> damaged(const std::vector<std::uint8_t>& font, random_t& random) {
    std::vector<std::uint8_t> bytes = font;
    const std::size_t kind = random.below(4);
    if (kind == 0) {
        bytes.resize(1 + random.below(bytes.size()));
        return bytes;
    }
    constexpr std::array<std::uint32_t, 6> edges = { 0, 1, 0x7FFF, 0xFFFF, 0x7FFFFFFF, 0xFFFFFFFF };
    const std::size_t changes = 1 + random.below(kind == 1 ? 16 : 4);
    for (std::size_t change = 0; change < changes; ++change) {
        // The table directory's first 1 KB most of the time, where the offsets and lengths are.
        const std::size_t reach =
            random.below(2) == 0 ? std::min<std::size_t>(1024, bytes.size()) : bytes.size();
        const std::size_t at = random.below(reach);
        if (kind == 1) {
            bytes[at] = static_cast<std::uint8_t>(random.next());
        } else {
            const std::uint32_t edge = edges[random.below(edges.size())];
            const std::size_t width = kind == 2 ? 2 : 4;
            for (std::size_t i = 0; i < width && at + i < bytes.size(); ++i) {
                bytes[at + i] = static_cast<std::uint8_t>(edge >> (8U * (width - 1 - i)));
            }
        }
    }
    return bytes;
}

std::vector<std::filesystem::path> fonts() {
    std::vector<std::filesystem::path> found;
    for (const auto& entry : std::filesystem::directory_iterator(FBE_OWN_TEST_FONTS)) {
        if (entry.path().extension() == ".ttf") {
            found.push_back(entry.path());
        }
    }
    found.emplace_back(std::filesystem::path(FBE_TEST_FONTS) / "OpenSans-Regular.ttf");
    return found;
}

/// A font that was loaded, laid out with, and freed.
void used(std::uint64_t font, random_t& random) {
    fbe_text_set_system_fallback(font, false);
    static constexpr std::array<std::string_view, 4> texts = {
        "Hello, world",
        "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7",
        "a\xCC\x81\xCC\x81 \xF0\x9F\x98\x80",
        "\xE2\x80\xAE\x61\x62\x63\n123",
    };
    tried(font, texts[random.below(texts.size())], 1.0F);
    tried(font, random_text(random), 3.0F);
    fbe_text_metrics_t metrics{};
    fbe_text_font_metrics(font, &metrics);
    fbe_text_font_family(font);
    fbe_text_font_style_name(font);
    CHECK(fbe_text_free_font(font));
}

} // namespace

TEST_CASE("damaged fonts are refused or laid out with, never read past") {
    random_t random(0x5EEDF0A7ULL);
    for (const std::filesystem::path& path : fonts()) {
        const std::vector<std::uint8_t> font = read(path);
        INFO(path.filename().string());
        std::size_t loads = 0;
        for (int round = 0; round < 1000; ++round) {
            INFO("round " << round);
            const std::vector<std::uint8_t> bytes = damaged(font, random);
            const auto style = static_cast<std::int32_t>(random.below(4));
            const std::uint64_t loaded =
                fbe_text_load_font_bytes(bytes.data(), static_cast<std::int64_t>(bytes.size()),
                                         "damaged.ttf", 0, 16.0F, style);
            if (loaded != 0) {
                used(loaded, random);
                ++loads;
            } else {
                CHECK_FALSE(std::string(fbe_error()).empty());
            }
        }
        // Enough load to reach shaping and rasterizing, or the damage is all at the door.
        CHECK(loads > 100);
        fbe::text::atlas().clear();
    }
}

TEST_CASE("bytes that are no font, and faces past a collection's, are refused") {
    random_t random(0xB17E5ULL);
    for (int round = 0; round < 200; ++round) {
        std::vector<std::uint8_t> bytes(1 + random.below(4096));
        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(random.next());
        }
        // Sometimes with a font's magic first, so FreeType reads further.
        static constexpr std::array<std::uint32_t, 4> magic = {
            0x00010000,
            0x4F54544F,
            0x74746366,
            0x774F4646,
        };
        if (bytes.size() >= 4 && random.below(2) == 0) {
            const std::uint32_t chosen = magic[random.below(magic.size())];
            for (std::size_t i = 0; i < 4; ++i) {
                bytes[i] = static_cast<std::uint8_t>(chosen >> (8U * (3 - i)));
            }
        }
        const std::uint64_t loaded = fbe_text_load_font_bytes(
            bytes.data(), static_cast<std::int64_t>(bytes.size()), "noise.ttf",
            static_cast<std::int32_t>(random.below(3)), 12.0F, 0);
        if (loaded != 0) {
            used(loaded, random);
        }
    }
    fbe::text::atlas().clear();
}

TEST_CASE("texts that are broken, huge, or all marks are laid out safely and soon") {
    const std::string path = std::string(FBE_OWN_TEST_FONTS) + "/FbeTestMarks-Regular.ttf";
    const std::uint64_t marks = fbe_text_load_font(path.c_str(), 0, 16.0F, 0);
    const std::string open = std::string(FBE_TEST_FONTS) + "/OpenSans-Regular.ttf";
    const std::uint64_t sans = fbe_text_load_font(open.c_str(), 0, 16.0F, 0);
    REQUIRE(marks != 0);
    REQUIRE(sans != 0);
    fbe_text_set_system_fallback(marks, false);
    fbe_text_set_system_fallback(sans, false);
    REQUIRE(fbe_text_add_fallback(sans, marks));
    for (const std::string& text : nasty()) {
        INFO("a text of " << text.size() << " bytes, starting " << text.substr(0, 16));
        const auto started = std::chrono::steady_clock::now();
        tried(sans, text, 1.0F);
        tried(marks, text, 2.0F);
        const auto took = std::chrono::steady_clock::now() - started;
        // A megabyte in seconds, under half a minute with the sanitizers in a debug build;
        // quadratic would be hours.
        CHECK(took < std::chrono::seconds(120));
    }
    random_t random(0x7E47ULL);
    for (int round = 0; round < 3000; ++round) {
        tried(round % 2 == 0 ? sans : marks, random_text(random),
              1.0F + static_cast<float>(round % 4));
    }
    fbe_text_free_font(sans);
    fbe_text_free_font(marks);
    fbe::text::atlas().clear();
}

TEST_CASE("a scale past reason is refused, and one that's tiny lays out nothing out of reach") {
    const std::string open = std::string(FBE_TEST_FONTS) + "/OpenSans-Regular.ttf";
    const std::uint64_t font = fbe_text_load_font(open.c_str(), 0, 16.0F, 0);
    REQUIRE(font != 0);
    fbe_text_set_system_fallback(font, false);
    fbe::text::layout_t laid;
    for (const float scale : {
             0.0F,
             -1.0F,
             -0.0F,
             std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity(),
         }) {
        INFO("scale " << scale);
        CHECK_FALSE(fbe::text::lay_out(font, "W", scale, laid));
    }
    for (const float scale : { std::numeric_limits<float>::denorm_min(), 1e-30F, 1e-6F, 0.01F }) {
        INFO("scale " << scale);
        tried(font, "Hello\nworld", scale);
    }
    // A font made at sizes past reason is refused as it's made.
    for (const float size : {
             0.0F,
             -3.0F,
             std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
         }) {
        CHECK(fbe_text_resize_font(font, size, 0) == 0);
    }
    const std::uint64_t tiny =
        fbe_text_resize_font(font, std::numeric_limits<float>::denorm_min(), 0);
    if (tiny != 0) {
        tried(tiny, "tiny", 1.0F);
        fbe_text_free_font(tiny);
    }
    fbe_text_free_font(font);
    fbe::text::atlas().clear();
}

TEST_CASE("the corpus of what broke text once is laid out safely") {
    const std::filesystem::path corpus = std::filesystem::path(FBE_TEXT_CORPUS);
    const std::string open = std::string(FBE_TEST_FONTS) + "/OpenSans-Regular.ttf";
    const std::uint64_t sans = fbe_text_load_font(open.c_str(), 0, 16.0F, 0);
    REQUIRE(sans != 0);
    fbe_text_set_system_fallback(sans, false);
    random_t random(0xC0B05ULL);
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(corpus)) {
        const std::vector<std::uint8_t> bytes = read(entry.path());
        INFO(entry.path().filename().string());
        if (entry.path().extension() == ".txt") {
            tried(sans, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                  1.0F);
        } else if (entry.path().extension() == ".font") {
            const std::uint64_t loaded = fbe_text_load_font_bytes(
                bytes.data(), static_cast<std::int64_t>(bytes.size()), "corpus.ttf", 0, 16.0F, 0);
            if (loaded != 0) {
                used(loaded, random);
            }
        }
        ++count;
    }
    CHECK(count > 0);
    fbe_text_free_font(sans);
    fbe::text::atlas().clear();
}
