#pragma once

// What the canvas reads of text: the atlas's pages, which glyphs are rasterized into, and a text
// laid out into glyphs placed on them. Fonts are FreeType's and HarfBuzz's, a handle each, as
// fbe_text.h makes them; text is shaped in runs SheenBidi splits by direction. All of it is the
// main thread's.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace fbe::text {

/// Handles' kind for fonts (architecture.md's handles).
constexpr std::uint8_t kind = 6;

/// A page's pixels: coverage, one byte a pixel, linear, which an R8 texture holds and the canvas
/// draws as the text's color times it; or color glyphs (emoji), straight RGBA8 in sRGB, as images
/// are, which an RGBA8 texture decodes to linear light, and the canvas draws as it does an image,
/// times the color's alpha.
enum class page_format_t : std::uint8_t { COVERAGE, COLOR };

/// A rectangle of a page's pixels, empty when its width or height is 0.
struct rect_t {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;

    [[nodiscard]] bool empty() const {
        return width <= 0 || height <= 0;
    }
};

/// One page of the atlas, 1024 by 1024 pixels, or a glyph's own size for one bigger. Glyphs are
/// only added to a page, never moved, each with a transparent pixel around it, so what changes is
/// `dirty`, what's been written since the canvas last took it, which the canvas uploads and
/// empties, and `version`, which goes up with each write.
struct page_t {
    page_format_t format = page_format_t::COVERAGE;
    std::int32_t width = 0;
    std::int32_t height = 0;
    /// Rows top down, `width` times 1 or 4 bytes each; none once it's dropped.
    std::vector<std::uint8_t> pixels;
    rect_t dirty;
    std::uint64_t version = 0;

    /// Whether trim's let go of it: it has no pixels, nothing's on it, and the canvas lets go of
    /// its texture, until a glyph's placed on it, when it's made again, its number kept, perhaps
    /// another format and size, all of it `dirty`.
    [[nodiscard]] bool dropped() const {
        return pixels.empty();
    }
};

/// A glyph placed for drawing: its page, where it is on it in pixels, and the quad it goes in, in
/// whole pixels from the text's origin, the first line's baseline at its left, y down: (x, y),
/// `width` by `height`. Up to 512 pixels an em, the quad is the source's size, its quarter of a
/// pixel across rasterized into it already (a color glyph's, or a strike's bitmap's, at the
/// nearest pixel instead), so drawn on pixel edges, sampled NEAREST, at a whole pixel plus (x, y),
/// it's crisp. Past 512, glyphs are rasterized at 512, and past 2048 pixels across or down,
/// smaller, and the quad is bigger than the source, which the canvas draws scaled, sampled
/// LINEAR.
struct placed_glyph_t {
    std::uint32_t page = 0;
    rect_t source;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

/// A text laid out: its glyphs in drawing order, a right-to-left run's turned around already, and
/// its size, in pixels at the scale it was laid out at.
struct layout_t {
    std::vector<placed_glyph_t> glyphs;
    /// The widest line's advance.
    float width = 0.0F;
    /// Its lines times the font's line height.
    float height = 0.0F;
    /// The first line's ascent, above its baseline, where the text's top is.
    float ascent = 0.0F;
    /// The atlas's generation it was laid out in, past which its glyphs aren't on its pages.
    std::uint64_t generation = 0;
};

/// The atlas: every font's glyphs, rasterized at the sizes they're drawn at, on pages the canvas
/// uploads.
class atlas_t {
public:
    /// Its pages, which only grow until it's cleared.
    [[nodiscard]] std::span<const page_t> pages() const {
        return m_pages;
    }

    /// A page, to empty its `dirty` once the canvas has uploaded it.
    page_t& page(std::size_t index) {
        return m_pages[index];
    }

    /// How many times pages of it have been emptied; a layout of an older generation is laid out
    /// again.
    [[nodiscard]] std::uint64_t generation() const {
        return m_generation;
    }

    /// What the canvas calls as a frame starts, never during one: past `budget` bytes of pages, 16
    /// MB at first, the pages no layout used since the last trim are dropped, their pixels let go
    /// of, where they are, and its generation goes up; the last frame's pages stay, however many,
    /// so a frame that needs more than the budget isn't rasterized again each frame. Within a frame
    /// it only grows, so the frame's layouts stay drawable.
    void trim();

    /// Empties it at once, its pages gone, and its generation goes up.
    void clear();

    /// Marks `page` used by a layout, which trim keeps.
    void used(std::uint32_t page) {
        if (page < m_used.size()) {
            m_used[page] = true;
        }
    }

    /// Sets how many bytes of pages trim lets it keep.
    void set_budget(std::size_t bytes) {
        m_budget = bytes;
    }

    /// Where a glyph `width` by `height` pixels goes, on a page of `format`, which it's written
    /// into by the caller; its page's index in `page`, or false if it can't be made.
    bool place(page_format_t format, std::int32_t width, std::int32_t height, std::uint32_t& page,
               rect_t& place);

    /// Marks `place`, on `page`, written.
    void written(std::uint32_t page, const rect_t& place);

private:
    /// A row of glyphs on a page, as tall as its tallest, filled from the left.
    struct shelf_t {
        std::int32_t y = 0;
        std::int32_t height = 0;
        std::int32_t used = 0;
    };

    std::vector<page_t> m_pages;
    /// Each page's shelves, and how far down they reach.
    std::vector<std::vector<shelf_t>> m_shelves;
    std::vector<std::int32_t> m_bottoms;
    /// Whether a layout used each page since the last trim.
    std::vector<bool> m_used;
    std::uint64_t m_generation = 0;
    std::size_t m_budget = std::size_t{ 16 } << 20U;
};

/// The atlas.
atlas_t& atlas();

/// Lays `text`, UTF-8, out in `font` at `scale`, the window's pixels a unit of the font's size, its
/// glyphs rasterized into the atlas as they're needed: '\n' breaks its lines, a glyph its font
/// hasn't is its fallbacks', and each run of a script and a direction is shaped on its own. False,
/// with fbe_error's why, for a handle that isn't a font's, or a scale that isn't over 0.
bool lay_out(std::uint64_t font, std::string_view text, float scale, layout_t& into);

/// The size `text` takes in `font`, in its own units, as lay_out lays it out at 1 without
/// rasterizing it; false as lay_out is.
bool measure(std::uint64_t font, std::string_view text, float& width, float& height);

} // namespace fbe::text
