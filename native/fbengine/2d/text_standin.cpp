// The stand-in for the input lane's text core, as text_standin.hpp says: its atlas's pages of
// shelves, each glyph with a transparent pixel around it, and its lay_out handed on.

#include "2d/text_standin.hpp"

#include "core/engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

/// A page's size, which a glyph bigger than gets a page of its own size.
constexpr std::int32_t page_size = 1024;

fbe::text::lay_out_t laid_out = nullptr;

} // namespace

namespace fbe::text {

void set_lay_out(lay_out_t laying) {
    laid_out = laying;
}

atlas_t& atlas() {
    static atlas_t kept;
    return kept;
}

void atlas_t::trim() {
    std::size_t bytes = 0;
    for (const page_t& each : m_pages) {
        bytes += each.pixels.size();
    }
    if (bytes > m_budget) {
        clear();
    }
}

void atlas_t::clear() {
    m_pages.clear();
    m_shelves.clear();
    m_bottoms.clear();
    ++m_generation;
}

bool atlas_t::place(page_format_t format, std::int32_t width, std::int32_t height,
                    std::uint32_t& page, rect_t& place) {
    if (width <= 0 || height <= 0) {
        return fbe::fail("a glyph's place is wider and taller than 0");
    }
    // With its transparent pixel around it.
    const std::int32_t across = width + 2;
    const std::int32_t down = height + 2;
    const auto made = [&](std::int32_t page_width, std::int32_t page_height) {
        m_pages.push_back({
            .format = format,
            .width = page_width,
            .height = page_height,
            .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(page_width) *
                                                    static_cast<std::size_t>(page_height) *
                                                    (format == page_format_t::COLOR ? 4U : 1U),
                                                0),
        });
        m_shelves.emplace_back();
        m_bottoms.push_back(0);
        return static_cast<std::uint32_t>(m_pages.size() - 1);
    };
    if (across > page_size || down > page_size) {
        page = made(across, down);
        m_bottoms[page] = down;
        place = { .x = 1, .y = 1, .width = width, .height = height };
        return true;
    }
    for (std::uint32_t at = 0; at < m_pages.size(); ++at) {
        if (m_pages[at].format != format || m_pages[at].width != page_size) {
            continue;
        }
        for (shelf_t& shelf : m_shelves[at]) {
            if (shelf.height >= down && shelf.used + across <= page_size) {
                page = at;
                place = { .x = shelf.used + 1, .y = shelf.y + 1, .width = width, .height = height };
                shelf.used += across;
                return true;
            }
        }
        if (m_bottoms[at] + down <= page_size) {
            page = at;
            m_shelves[at].push_back({ .y = m_bottoms[at], .height = down, .used = across });
            place = { .x = 1, .y = m_bottoms[at] + 1, .width = width, .height = height };
            m_bottoms[at] += down;
            return true;
        }
    }
    page = made(page_size, page_size);
    m_shelves[page].push_back({ .y = 0, .height = down, .used = across });
    m_bottoms[page] = down;
    place = { .x = 1, .y = 1, .width = width, .height = height };
    return true;
}

void atlas_t::written(std::uint32_t page, const rect_t& place) {
    page_t& written = m_pages[page];
    if (written.dirty.empty()) {
        written.dirty = place;
    } else {
        const std::int32_t left = std::min(written.dirty.x, place.x);
        const std::int32_t top = std::min(written.dirty.y, place.y);
        const std::int32_t right =
            std::max(written.dirty.x + written.dirty.width, place.x + place.width);
        const std::int32_t bottom =
            std::max(written.dirty.y + written.dirty.height, place.y + place.height);
        written.dirty = { .x = left, .y = top, .width = right - left, .height = bottom - top };
    }
    ++written.version;
}

bool lay_out(std::uint64_t font, std::string_view text, float scale, layout_t& into) {
    if (laid_out == nullptr) {
        return fbe::fail("there's no text core yet to lay text out");
    }
    return laid_out(font, text, scale, into);
}

} // namespace fbe::text
