// The stand-in for the input lane's text core, as text_standin.hpp says: its atlas's pages of
// shelves, each glyph with a transparent pixel around it, trimmed as its trim is, and its lay_out
// handed on.

#include "2d/text_standin.hpp"

#include "core/engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

/// A page's size, which a glyph bigger than gets a page its size.
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
    bool emptied = false;
    if (bytes > m_budget) {
        // The pages no layout used since the last trim, dropped where they are.
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
    ++m_generation;
}

bool atlas_t::place(page_format_t format, std::int32_t width, std::int32_t height,
                    std::uint32_t& page, rect_t& place) {
    const std::int32_t wide = width + 2;
    const std::int32_t tall = height + 2;
    for (std::size_t index = 0; index < m_pages.size(); ++index) {
        const page_t& each = m_pages[index];
        if (each.dropped() || each.format != format || wide > each.width) {
            continue;
        }
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
