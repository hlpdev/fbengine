// 2D's text: the atlas's pages uploaded as textures, coverage as R8 and color as sRGB RGBA8, each
// what's been written since, keyed on the atlas's generation as well as the page's version, as a
// page's version starts again once the atlas is cleared; and text laid out into its glyphs, drawn
// with the canvas at whole pixels, a run of glyphs of one page at a time.

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

/// A page's texture, its size and format, and the atlas's generation and the page's version it
/// has.
struct page_texture_t {
    std::uint64_t texture = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    fbe::text::page_format_t format = fbe::text::page_format_t::COVERAGE;
    std::uint64_t generation = 0;
    std::uint64_t version = 0;
};

/// The pages' textures, by page.
std::vector<page_texture_t> page_textures;

/// Lets go of a page's texture.
void release(page_texture_t& page) {
    if (page.texture != 0 && fbe::texture::find(page.texture) != nullptr) {
        fbe_texture_release(page.texture);
    }
    page.texture = 0;
}

/// Uploads what's been written into the atlas's pages since they were last uploaded, its dirty
/// rectangle and the transparent pixel around it, which a page made again has its old texture's
/// pixels in; making a texture for a new page, or a page's again where it's another size or format
/// or the GPU's started again, and uploading all of it. False, with fbe_error's why, if one can't
/// be made.
bool upload_pages() {
    fbe::text::atlas_t& atlas = fbe::text::atlas();
    const std::span<const fbe::text::page_t> pages = atlas.pages();
    for (std::size_t at = pages.size(); at < page_textures.size(); ++at) {
        release(page_textures[at]);
    }
    page_textures.resize(pages.size());
    for (std::size_t at = 0; at < pages.size(); ++at) {
        fbe::text::page_t& page = atlas.page(at);
        page_texture_t& kept = page_textures[at];
        fbe::text::rect_t written = page.dirty;
        if (kept.texture == 0 || fbe::texture::find(kept.texture) == nullptr ||
            kept.width != page.width || kept.height != page.height || kept.format != page.format) {
            release(kept);
            const bool coverage = page.format == fbe::text::page_format_t::COVERAGE;
            kept.texture = fbe::texture::create_engine_texture(
                static_cast<std::uint32_t>(page.width), static_cast<std::uint32_t>(page.height),
                coverage ? FBE_TEXTURE_R8 : FBE_TEXTURE_RGBA8);
            if (kept.texture == 0) {
                return false;
            }
            kept.width = page.width;
            kept.height = page.height;
            kept.format = page.format;
            // Nothing's in it yet, whatever the page's keys are, until it's uploaded.
            kept.generation = UINT64_MAX;
            kept.version = UINT64_MAX;
            written = { .x = 0, .y = 0, .width = page.width, .height = page.height };
        } else if (kept.generation == atlas.generation() && kept.version == page.version) {
            continue;
        } else if (kept.version == UINT64_MAX) {
            // Made, but its first upload failed: all of it, still.
            written = { .x = 0, .y = 0, .width = page.width, .height = page.height };
        } else if (!written.empty()) {
            const std::int32_t left = std::max(written.x - 1, 0);
            const std::int32_t top = std::max(written.y - 1, 0);
            const std::int32_t right = std::min(written.x + written.width + 1, page.width);
            const std::int32_t bottom = std::min(written.y + written.height + 1, page.height);
            written = { .x = left, .y = top, .width = right - left, .height = bottom - top };
        }
        if (!written.empty()) {
            const std::size_t channels = page.format == fbe::text::page_format_t::COLOR ? 4 : 1;
            const std::size_t pitch = static_cast<std::size_t>(page.width) * channels;
            const std::size_t first = (static_cast<std::size_t>(written.y) * pitch) +
                                      (static_cast<std::size_t>(written.x) * channels);
            // In the frame's uploads, so a batch drawing the page isn't sent: what's written is
            // only where no glyph drawn yet is.
            if (!fbe::texture::write_ahead(kept.texture, static_cast<std::uint32_t>(written.x),
                                           static_cast<std::uint32_t>(written.y),
                                           static_cast<std::uint32_t>(written.width),
                                           static_cast<std::uint32_t>(written.height),
                                           page.pixels.data() + first,
                                           static_cast<std::uint32_t>(pitch))) {
                return false;
            }
        }
        page.dirty = {};
        kept.generation = atlas.generation();
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
    // The target, as the canvas's draws find it, the screen starting the GPU for a window, before
    // the pages' textures are made.
    if (fbe::gpu::lost()) {
        return fbe::gpu::fail_lost();
    }
    const std::uint64_t handle = target != 0 ? target : fbe_gpu_screen();
    if (handle == 0) {
        return fbe::fail("there's no screen, as there's no window");
    }
    if (fbe::texture::find(handle) == nullptr) {
        return false;
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
            .source_width = glyph.source.width,
            .source_height = glyph.source.height,
            .x = left + glyph.x,
            .y = baseline + glyph.y,
            .width = glyph.width,
            .height = glyph.height,
        });
    }
    return draw_run();
}

} // extern "C"
