// 2D's text: the atlas's pages uploaded as textures, coverage as R8 and color as sRGB RGBA8, each
// what's been written since, and a whole page when it's new or the atlas has been cleared, as a
// page's version starts again then; and text laid out into its glyphs, drawn texel for pixel on
// whole pixels with the canvas, a run of glyphs of one page at a time.

#include "2d/canvas.hpp"
#include "2d/fbe_2d.h"
#include "2d/text_standin.hpp"
#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/vulkan.hpp"
#include "texture/texture.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace {

/// A page's texture, and the page's version it has.
struct page_texture_t {
    std::uint64_t texture = 0;
    std::uint64_t version = 0;
};

/// The pages' textures, and the atlas's generation they're of.
std::vector<page_texture_t> page_textures;
std::uint64_t pages_generation = 0;

/// Lets go of the pages' textures.
void release_pages() {
    for (const page_texture_t& page : page_textures) {
        if (page.texture != 0 && fbe::texture::find(page.texture) != nullptr) {
            fbe_texture_release(page.texture);
        }
    }
    page_textures.clear();
}

/// Uploads what's been written into the atlas's pages since they were last uploaded, making a
/// texture for each new page, or each again once the atlas is cleared or the GPU's started again;
/// false, with fbe_error's why, if one can't be made.
bool upload_pages() {
    fbe::text::atlas_t& atlas = fbe::text::atlas();
    if (atlas.generation() != pages_generation) {
        release_pages();
        pages_generation = atlas.generation();
    }
    page_textures.resize(std::max(page_textures.size(), atlas.pages().size()));
    for (std::size_t at = 0; at < atlas.pages().size(); ++at) {
        fbe::text::page_t& page = atlas.page(at);
        page_texture_t& kept = page_textures[at];
        fbe::text::rect_t written = page.dirty;
        if (kept.texture == 0 || fbe::texture::find(kept.texture) == nullptr) {
            const bool coverage = page.format == fbe::text::page_format_t::COVERAGE;
            kept.texture = fbe::texture::create_engine_texture(
                static_cast<std::uint32_t>(page.width), static_cast<std::uint32_t>(page.height),
                coverage ? FBE_TEXTURE_R8 : FBE_TEXTURE_RGBA8);
            if (kept.texture == 0) {
                return false;
            }
            written = { .x = 0, .y = 0, .width = page.width, .height = page.height };
        } else if (kept.version == page.version) {
            continue;
        }
        if (!written.empty()) {
            const std::size_t channels = page.format == fbe::text::page_format_t::COLOR ? 4 : 1;
            const std::size_t pitch = static_cast<std::size_t>(page.width) * channels;
            const std::size_t first = (static_cast<std::size_t>(written.y) * pitch) +
                                      (static_cast<std::size_t>(written.x) * channels);
            if (!fbe_texture_write(kept.texture, written.x, written.y, written.width,
                                   written.height, page.pixels.data() + first,
                                   static_cast<std::int32_t>(pitch))) {
                return false;
            }
        }
        page.dirty = {};
        kept.version = page.version;
    }
    return true;
}

/// How many of a target's pixels a unit of a font's size is: the screen's, its render size's
/// pixels for each of the window's units; a texture's, 1.
float scale_of(std::uint64_t target) {
    if (target != 0 || fbe_window_width() <= 0) {
        return 1.0F;
    }
    const fbe::texture::texture_t* screen = fbe::texture::find(fbe::gpu::screen_handle());
    return screen != nullptr
               ? static_cast<float>(screen->width) / static_cast<float>(fbe_window_width())
               : 1.0F;
}

} // namespace

extern "C" {

FBE_API bool fbe_2d_text(uint64_t target, const fbe_2d_pen_t* pen, uint64_t font, const char* text,
                         int32_t length, int32_t x, int32_t y) {
    if (text == nullptr || length < 0) {
        return fbe::fail("no text was given");
    }
    if (pen == nullptr) {
        return fbe::fail("no pen was given");
    }
    // What the frame's first canvas draw does, before the atlas takes this text's glyphs.
    fbe::canvas::trim_atlas();
    fbe::text::layout_t layout;
    if (!fbe::text::lay_out(font, std::string_view(text, static_cast<std::size_t>(length)),
                            scale_of(target), layout) ||
        !upload_pages()) {
        return false;
    }
    // Its top left at x, y, so its first line's baseline below it by its ascent, in whole pixels.
    const std::int64_t left = x;
    const std::int64_t baseline = std::int64_t{ y } + std::lround(layout.ascent);
    // A run of glyphs of one page at a time, those with nothing to draw, as a space, left out.
    std::vector<fbe::canvas::glyph_t> run;
    std::uint32_t run_page = 0;
    const std::span<const fbe::text::page_t> pages = fbe::text::atlas().pages();
    const auto draw_run = [&] {
        if (run.empty()) {
            return true;
        }
        if (run_page >= pages.size() || run_page >= page_textures.size()) {
            return fbe::fail("a glyph's page isn't the atlas's");
        }
        const bool coverage = pages[run_page].format == fbe::text::page_format_t::COVERAGE;
        const bool drawn =
            fbe::canvas::draw_glyphs(target, pen, page_textures[run_page].texture, coverage, run);
        run.clear();
        return drawn;
    };
    for (const fbe::text::placed_glyph_t& glyph : layout.glyphs) {
        if (glyph.source.empty()) {
            continue;
        }
        if (!run.empty() && glyph.page != run_page && !draw_run()) {
            return false;
        }
        run_page = glyph.page;
        run.push_back({
            .source_x = glyph.source.x,
            .source_y = glyph.source.y,
            .width = glyph.source.width,
            .height = glyph.source.height,
            .x = left + glyph.x,
            .y = baseline + glyph.y,
        });
    }
    return draw_run();
}

} // extern "C"
