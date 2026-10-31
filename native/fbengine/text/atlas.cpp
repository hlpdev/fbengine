// The atlas: glyphs rasterized by FreeType at the sizes they're drawn at, each kept by its face,
// index, size, style, and quarter of a pixel, on pages of rows, as tall as their tallest glyph,
// that the canvas uploads; a page trimmed let go of until it's used again.

#include "core/engine.hpp"
#include "text/font.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <unordered_map>
#include <vector>

#include FT_OUTLINE_H

namespace fbe::text {

namespace {

/// A page's size, which a glyph bigger than gets a page of its own size.
constexpr std::int32_t page_size = 1024;

/// The most pixels a glyph's bitmap is across or down, past which it's rasterized smaller and
/// drawn bigger.
constexpr double largest_side = 2048.0;

/// What a glyph's kept by.
struct glyph_key_t {
    std::uint32_t face = 0;
    std::uint32_t glyph = 0;
    std::int32_t pixels = 0;
    /// Bolder, slanted, and the quarter, a bit each, then two.
    std::uint32_t how = 0;

    bool operator==(const glyph_key_t&) const = default;
};

struct glyph_hash_t {
    std::size_t operator()(const glyph_key_t& key) const {
        std::uint64_t mixed = (std::uint64_t{ key.face } << 32U) ^ key.glyph;
        mixed ^=
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.pixels)) << 8U) ^ key.how;
        mixed *= 0x9E3779B97F4A7C15ULL;
        return static_cast<std::size_t>(mixed ^ (mixed >> 29U));
    }
};

/// The glyphs rasterized, made the first time one is.
std::unique_ptr<std::unordered_map<glyph_key_t, glyph_t, glyph_hash_t>> glyphs;

/// A glyph slot's bitmap copied into a place on the atlas's pages, of the format its pixels are:
/// coverage for gray and mono, color for BGRA. Its place is empty when there's nothing to draw.
bool stored(const FT_Bitmap& bitmap, glyph_t& glyph) {
    const auto width = static_cast<std::int32_t>(bitmap.width);
    const auto height = static_cast<std::int32_t>(bitmap.rows);
    if (width == 0 || height == 0) {
        return true;
    }
    page_format_t format = page_format_t::COVERAGE;
    if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
        format = page_format_t::COLOR;
    } else if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && bitmap.pixel_mode != FT_PIXEL_MODE_MONO) {
        return true;
    }
    atlas_t& kept = atlas();
    if (!kept.place(format, width, height, glyph.page, glyph.place)) {
        return false;
    }
    page_t& page = kept.page(glyph.page);
    const std::size_t channels = format == page_format_t::COLOR ? 4 : 1;
    for (std::int32_t row = 0; row < height; ++row) {
        const unsigned char* from =
            bitmap.buffer + (static_cast<std::ptrdiff_t>(row) * bitmap.pitch);
        const std::size_t first =
            (static_cast<std::size_t>(glyph.place.y + row) * static_cast<std::size_t>(page.width)) +
            static_cast<std::size_t>(glyph.place.x);
        std::uint8_t* to = page.pixels.data() + (first * channels);
        for (std::int32_t column = 0; column < width; ++column) {
            const auto at = static_cast<std::size_t>(column);
            if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                to[at] = from[at];
            } else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                to[at] = (from[at / 8] & (0x80U >> (at % 8))) != 0 ? 0xFF : 0x00;
            } else {
                // FreeType's BGRA is premultiplied, its colors sRGB's; the page's are straight.
                const std::size_t pixel = at * 4;
                const std::uint8_t alpha = from[pixel + 3];
                to[pixel] = straight(from[pixel + 2], alpha);
                to[pixel + 1] = straight(from[pixel + 1], alpha);
                to[pixel + 2] = straight(from[pixel], alpha);
                to[pixel + 3] = alpha;
            }
        }
    }
    kept.written(glyph.page, glyph.place);
    return true;
}

/// Whether `point` draws nothing, its glyph empty: a space, or a format character.
bool blank(char32_t point) {
    const hb_unicode_general_category_t category =
        hb_unicode_general_category(hb_unicode_funcs_get_default(), point);
    return category == HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR ||
           category == HB_UNICODE_GENERAL_CATEGORY_FORMAT ||
           category == HB_UNICODE_GENERAL_CATEGORY_CONTROL ||
           category == HB_UNICODE_GENERAL_CATEGORY_LINE_SEPARATOR ||
           category == HB_UNICODE_GENERAL_CATEGORY_PARAGRAPH_SEPARATOR;
}

/// Sizes `face` for `pixels` an em, 26.6, or to its first strike if it's a bitmap font's.
bool sized(const face_t& face, std::int32_t pixels) {
    FT_Face ft = face.ft;
    if (FT_Set_Char_Size(ft, 0, pixels, 72, 72) == 0) {
        return true;
    }
    return FT_HAS_FIXED_SIZES(ft) && FT_Select_Size(ft, 0) == 0;
}

/// Whether FreeType loads `glyph` of `face`, as it's sized, in color or not, as something it
/// draws: an outline, or a bitmap, not a PNG or SVG picture it can't. A scalable face's strike it
/// can't read is passed over for its outline.
bool loads(const face_t& face, std::uint32_t glyph, bool colored) {
    // Hinted up and down only, so it's crisp and keeps its advance.
    auto flags = static_cast<std::uint32_t>(FT_LOAD_TARGET_LIGHT);
    if (colored) {
        flags |= static_cast<std::uint32_t>(FT_LOAD_COLOR);
    }
    if (FT_Load_Glyph(face.ft, glyph, static_cast<FT_Int32>(flags)) != 0) {
        return false;
    }
    const FT_Glyph_Format format = face.ft->glyph->format;
    return format == FT_GLYPH_FORMAT_OUTLINE || format == FT_GLYPH_FORMAT_BITMAP;
}

/// An outline made bolder and slanted as they say, and moved right by `quarter` quarters of a
/// pixel.
bool transformed(FT_GlyphSlot slot, std::int32_t pixels, bool bolder, bool slanted,
                 std::int32_t quarter) {
    if (bolder) {
        // As FreeType's own emboldening is: a 24th of an em.
        const FT_Pos strength = pixels / 24;
        FT_Outline_EmboldenXY(&slot->outline, strength, strength);
    }
    if (slanted) {
        const FT_Matrix shear{ .xx = 0x10000L, .xy = 0x3333L, .yx = 0, .yy = 0x10000L };
        FT_Outline_Transform(&slot->outline, &shear);
    }
    FT_Outline_Translate(&slot->outline, static_cast<FT_Pos>(quarter) * 16, 0);
    return true;
}

/// `glyph` of `face`, `side` pixels across or down at `pixels` an em, made bolder and slanted as
/// they say, rasterized small enough to fit, kept by `key`, and drawn bigger by as much.
const glyph_t* shrunk(face_t& face, std::uint32_t glyph, std::int32_t pixels, bool bolder,
                      bool slanted, const glyph_key_t& key, double side);

} // namespace

atlas_t& atlas() {
    static atlas_t kept;
    return kept;
}

std::uint8_t straight(std::uint8_t premultiplied, std::uint8_t alpha) {
    if (alpha == 0) {
        return 0;
    }
    const unsigned scaled = ((unsigned{ premultiplied } * 255U) + (unsigned{ alpha } / 2U)) / alpha;
    return static_cast<std::uint8_t>(std::min(scaled, 255U));
}

void atlas_t::trim() {
    std::size_t bytes = 0;
    for (const page_t& each : m_pages) {
        bytes += each.pixels.size();
    }
    bool emptied = false;
    if (bytes > m_budget) {
        // The pages no layout used since the last trim, emptied where they are, and their glyphs
        // forgotten.
        for (std::size_t index = 0; index < m_pages.size(); ++index) {
            page_t& page = m_pages[index];
            if (m_used[index] || m_shelves[index].empty()) {
                continue;
            }
            page.pixels.clear();
            page.pixels.shrink_to_fit();
            page.dirty = {};
            ++page.version;
            m_shelves[index].clear();
            m_bottoms[index] = 0;
            emptied = true;
            if (glyphs != nullptr) {
                std::erase_if(*glyphs, [&](const auto& each) { return each.second.page == index; });
            }
        }
    }
    if (emptied) {
        ++m_generation;
    }
    std::ranges::fill(m_used, false);
}

void atlas_t::clear() {
    m_pages.clear();
    m_shelves.clear();
    m_bottoms.clear();
    m_used.clear();
    if (glyphs != nullptr) {
        glyphs->clear();
    }
    ++m_generation;
}

bool atlas_t::place(page_format_t format, std::int32_t width, std::int32_t height,
                    std::uint32_t& page, rect_t& place) {
    // A transparent pixel all around it, so it's sampled clean.
    const std::int32_t wide = width + 2;
    const std::int32_t tall = height + 2;
    for (std::size_t index = 0; index < m_pages.size(); ++index) {
        const page_t& each = m_pages[index];
        if (each.dropped() || each.format != format || wide > each.width) {
            continue;
        }
        // A row as tall as it, or not much taller, or a new one under the rows there.
        for (shelf_t& shelf : m_shelves[index]) {
            if (shelf.height >= tall && shelf.height <= tall + (tall / 2) + 2 &&
                shelf.used + wide <= each.width) {
                page = static_cast<std::uint32_t>(index);
                place = { .x = shelf.used + 1, .y = shelf.y + 1, .width = width, .height = height };
                shelf.used += wide;
                return true;
            }
        }
        if (m_bottoms[index] + tall <= each.height) {
            m_shelves[index].push_back({ .y = m_bottoms[index], .height = tall, .used = wide });
            page = static_cast<std::uint32_t>(index);
            place = { .x = 1, .y = m_bottoms[index] + 1, .width = width, .height = height };
            m_bottoms[index] += tall;
            return true;
        }
    }
    // A page trimmed made again, its number kept, all of it to upload; or a new one.
    std::size_t index = 0;
    while (index < m_pages.size() && !m_pages[index].dropped()) {
        ++index;
    }
    if (index == m_pages.size()) {
        if (m_pages.size() >= 0xFFFFFFFFU) {
            return fbe::fail("the atlas has as many pages as it can");
        }
        m_pages.emplace_back();
        m_shelves.emplace_back();
        m_bottoms.push_back(0);
        m_used.push_back(false);
    }
    page_t& made = m_pages[index];
    const bool again = made.version != 0;
    made.format = format;
    made.width = std::max(page_size, wide);
    made.height = std::max(page_size, tall);
    made.pixels.assign(static_cast<std::size_t>(made.width) *
                           static_cast<std::size_t>(made.height) *
                           (format == page_format_t::COLOR ? 4U : 1U),
                       0);
    made.dirty =
        again ? rect_t{ .x = 0, .y = 0, .width = made.width, .height = made.height } : rect_t{};
    made.version += again ? 1 : 0;
    m_shelves[index] = { { .y = 0, .height = tall, .used = wide } };
    m_bottoms[index] = tall;
    page = static_cast<std::uint32_t>(index);
    place = { .x = 1, .y = 1, .width = width, .height = height };
    return true;
}

void atlas_t::written(std::uint32_t page, const rect_t& place) {
    page_t& each = m_pages[page];
    if (each.dirty.empty()) {
        each.dirty = place;
    } else {
        const std::int32_t left = std::min(each.dirty.x, place.x);
        const std::int32_t top = std::min(each.dirty.y, place.y);
        const std::int32_t right = std::max(each.dirty.x + each.dirty.width, place.x + place.width);
        const std::int32_t bottom =
            std::max(each.dirty.y + each.dirty.height, place.y + place.height);
        each.dirty = { .x = left, .y = top, .width = right - left, .height = bottom - top };
    }
    ++each.version;
}

const glyph_t* rasterized(face_t& face, std::uint32_t glyph, std::int32_t pixels, bool bolder,
                          bool slanted, std::int32_t quarter) {
    if (glyphs == nullptr) {
        glyphs = std::make_unique<std::unordered_map<glyph_key_t, glyph_t, glyph_hash_t>>();
    }
    const glyph_key_t key{
        .face = face.id,
        .glyph = glyph,
        .pixels = pixels,
        .how =
            (bolder ? 1U : 0U) | (slanted ? 2U : 0U) | (static_cast<std::uint32_t>(quarter) << 2U),
    };
    if (const auto found = glyphs->find(key); found != glyphs->end()) {
        return &found->second;
    }
    // A color glyph, or a bitmap font's, takes no made bolder or slant, nor a quarter's move, so
    // it's kept once, as it's plainly asked for.
    const glyph_key_t plain{ .face = face.id, .glyph = glyph, .pixels = pixels, .how = 0 };
    if (const auto found = glyphs->find(plain); found != glyphs->end() && !found->second.shifted) {
        return &found->second;
    }
    FT_Face ft = face.ft;
    if (!sized(face, pixels)) {
        fbe::fail(std::format("FreeType couldn't size {} at {} pixels", face.family, pixels / 64));
        return nullptr;
    }
    // In color where it has it, and as its outline where FreeType can't draw that color, as a PNG
    // or SVG picture.
    FT_GlyphSlot slot = ft->glyph;
    bool loaded = false;
    bool outline = false;
    for (const bool colored : { true, false }) {
        if ((colored && !FT_HAS_COLOR(ft)) || !loads(face, glyph, colored)) {
            continue;
        }
        outline = slot->format == FT_GLYPH_FORMAT_OUTLINE;
        if (outline) {
            transformed(slot, pixels, bolder, slanted, quarter);
            FT_BBox box{};
            FT_Outline_Get_CBox(&slot->outline, &box);
            const auto side =
                static_cast<double>(std::max(box.xMax - box.xMin, box.yMax - box.yMin)) / 64.0;
            if (side > largest_side) {
                return shrunk(face, glyph, pixels, bolder, slanted, key, side);
            }
        }
        loaded = !outline || FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) == 0;
        if (loaded) {
            break;
        }
    }
    if (!loaded) {
        fbe::fail(std::format("FreeType couldn't rasterize {}'s glyph {}", face.family, glyph));
        return nullptr;
    }
    const FT_Bitmap& bitmap = slot->bitmap;
    const auto side = static_cast<double>(std::max(bitmap.width, bitmap.rows));
    if (side > largest_side) {
        return shrunk(face, glyph, pixels, bolder, slanted, key, side);
    }
    glyph_t made;
    made.left = slot->bitmap_left;
    made.top = slot->bitmap_top;
    // COLR's layers are each loaded again as they're rendered, so its glyph's as plain as a
    // bitmap's.
    made.shifted = outline && bitmap.pixel_mode != FT_PIXEL_MODE_BGRA;
    if (!stored(bitmap, made)) {
        return nullptr;
    }
    return &glyphs->emplace(made.shifted ? key : plain, made).first->second;
}

namespace {

const glyph_t* shrunk(face_t& face, std::uint32_t glyph, std::int32_t pixels, bool bolder,
                      bool slanted, const glyph_key_t& key, double side) {
    // A couple of pixels short, for what's hinted and rounded.
    const auto smaller = static_cast<std::int32_t>(
        std::floor(static_cast<double>(pixels) * (largest_side - 2.0) / side));
    if (smaller < 64 || smaller >= pixels) {
        fbe::fail(std::format("{}'s glyph {} is too big to rasterize", face.family, glyph));
        return nullptr;
    }
    const glyph_t* small = rasterized(face, glyph, smaller, bolder, slanted, 0);
    if (small == nullptr) {
        return nullptr;
    }
    glyph_t made = *small;
    made.scale = small->scale * static_cast<double>(pixels) / static_cast<double>(smaller);
    made.shifted = false;
    return &glyphs->emplace(key, made).first->second;
}

} // namespace

bool draws(face_t& face, char32_t point, std::int32_t pixels) {
    const FT_UInt glyph = FT_Get_Char_Index(face.ft, point);
    if (glyph == 0) {
        return false;
    }
    const std::uint64_t at = (std::uint64_t{ glyph } << 32U) | static_cast<std::uint32_t>(pixels);
    if (const auto found = face.drawable.find(at); found != face.drawable.end()) {
        return found->second;
    }
    // At the size it's laid out at, a strike's bitmap or an outline; an empty outline is only a
    // space's, or what doesn't show.
    const auto shows = [&] {
        FT_GlyphSlot slot = face.ft->glyph;
        return slot->format != FT_GLYPH_FORMAT_OUTLINE || slot->outline.n_contours > 0 ||
               blank(point);
    };
    const bool can =
        sized(face, pixels) && ((FT_HAS_COLOR(face.ft) && loads(face, glyph, true) && shows()) ||
                                (loads(face, glyph, false) && shows()));
    face.drawable.emplace(at, can);
    return can;
}

} // namespace fbe::text
