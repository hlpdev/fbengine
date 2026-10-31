// Shaping: text split into lines, each line into runs by direction (SheenBidi), each run into
// pieces of one script and one face, the first of a font's, its fallbacks', and the OS's that has
// a character's glyph, each piece shaped by HarfBuzz, and the glyphs placed in the order they're
// seen, a right-to-left run's turned around.

#include "core/engine.hpp"
#include "fbengine.h"
#include "text/font.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <SheenBidi/SheenBidi.h>
#include <string>
#include <string_view>
#include <vector>

namespace fbe::text {

namespace {

/// The amount a face that isn't bold is made bolder, an em's fraction, as FreeType's own
/// emboldening is, and how far one that doesn't slant is slanted, across for each up.
constexpr float emboldening = 1.0F / 24.0F;
constexpr float slant = 0.2F;
/// Emboldening that widens the advances too, not only the glyphs.
constexpr hb_bool_t widening = 0;

/// `text` as code points, U+FFFD for each sequence that isn't well-formed UTF-8.
std::u32string decoded(std::string_view text) {
    std::u32string points;
    points.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        const auto lead = static_cast<unsigned char>(text[at]);
        if (lead < 0x80U) {
            points.push_back(lead);
            ++at;
            continue;
        }
        char32_t point = 0;
        std::size_t needed = 0;
        char32_t least = 0;
        if (lead >= 0xC2U && lead <= 0xDFU) {
            point = lead & 0x1FU;
            needed = 1;
            least = 0x80;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            point = lead & 0x0FU;
            needed = 2;
            least = 0x800;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            point = lead & 0x07U;
            needed = 3;
            least = 0x10000;
        } else {
            points.push_back(U'�');
            ++at;
            continue;
        }
        std::size_t got = 0;
        while (got < needed && at + 1 + got < text.size() &&
               (static_cast<unsigned char>(text[at + 1 + got]) & 0xC0U) == 0x80U) {
            point = (point << 6U) | (static_cast<unsigned char>(text[at + 1 + got]) & 0x3FU);
            ++got;
        }
        if (got < needed || point < least || point > 0x10FFFF ||
            (point >= 0xD800 && point <= 0xDFFF)) {
            points.push_back(U'�');
            at += 1 + got;
            continue;
        }
        points.push_back(point);
        at += 1 + needed;
    }
    return points;
}

/// Whether `point` takes the face of what's before it: a mark, a joiner, or a variation selector,
/// which go with the character they're on.
bool clings(char32_t point) {
    const hb_unicode_general_category_t category =
        hb_unicode_general_category(hb_unicode_funcs_get_default(), point);
    return category == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK ||
           category == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK ||
           category == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK ||
           category == HB_UNICODE_GENERAL_CATEGORY_FORMAT || (point >= 0xFE00 && point <= 0xFE0F) ||
           (point >= 0xE0100 && point <= 0xE01EF);
}

/// One of the faces a font's glyphs come from, and what of the font's style it's made.
struct source_t {
    face_t* face = nullptr;
    bool bolder = false;
    bool slanted = false;
};

/// A font's faces, in the order its glyphs come from them: its own, its fallbacks', then the OS's,
/// each read only once a character before it is missing from all those before.
class sources_t {
public:
    explicit sources_t(const font_t& font) : m_font(font) {
        m_sources.push_back(
            { .face = font.face.get(), .bolder = font.bolder, .slanted = font.slanted });
        for (const std::uint64_t handle : font.fallbacks) {
            if (const font_t* fallback = font_of(handle, false)) {
                add(fallback->face);
            }
        }
    }

    /// The first source with a glyph for `point`, or null.
    const source_t* first_with(char32_t point) {
        for (std::size_t at = 0;; ++at) {
            while (at >= m_sources.size()) {
                if (!m_font.system_fallback || m_system >= system_fallback_count()) {
                    return nullptr;
                }
                add(system_fallback(m_system++));
            }
            if (has(*m_sources[at].face, point)) {
                return &m_sources[at];
            }
        }
    }

    [[nodiscard]] const source_t& primary() const {
        return m_sources.front();
    }

    static bool has(const face_t& face, char32_t point) {
        return FT_Get_Char_Index(face.ft, point) != 0;
    }

private:
    void add(const std::shared_ptr<face_t>& face) {
        if (face == nullptr || std::ranges::any_of(m_sources, [&](const source_t& each) {
                return each.face == face.get();
            })) {
            return;
        }
        // The font's style, made where this face isn't so.
        m_kept.push_back(face);
        m_sources.push_back({
            .face = face.get(),
            .bolder = (m_font.style & bold_bit) != 0U && face->weight < 600,
            .slanted = (m_font.style & italic_bit) != 0U && !face->italic,
        });
    }

    const font_t& m_font;
    std::vector<source_t> m_sources;
    std::vector<std::shared_ptr<face_t>> m_kept;
    std::size_t m_system = 0;
};

/// A piece of a line shaped as one: its code points, from `start`, its face, and its script.
struct piece_t {
    std::size_t start = 0;
    std::size_t length = 0;
    source_t source;
    hb_script_t script = HB_SCRIPT_COMMON;
};

/// A glyph shaped: its face, its index there, where it is from the line's pen at its start and up
/// from its baseline, in pixels, and the line it's on.
struct shaped_t {
    source_t source;
    std::uint32_t glyph = 0;
    float x = 0.0F;
    float up = 0.0F;
    std::size_t line = 0;
};

/// The pieces of `line`'s points from `start`, `length` of them, each of one script and face.
std::vector<piece_t> pieces(const std::u32string& line, std::size_t start, std::size_t length,
                            sources_t& sources) {
    hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();
    // Each point's script, a common or inherited one taking the one before it, or after it at
    // the start.
    std::vector<hb_script_t> scripts(length);
    hb_script_t last = HB_SCRIPT_COMMON;
    for (std::size_t at = 0; at < length; ++at) {
        hb_script_t script = hb_unicode_script(unicode, line[start + at]);
        if (script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED) {
            script = last;
        }
        scripts[at] = script;
        last = script;
    }
    hb_script_t next = HB_SCRIPT_COMMON;
    for (std::size_t at = length; at > 0; --at) {
        if (scripts[at - 1] == HB_SCRIPT_COMMON) {
            scripts[at - 1] = next;
        } else {
            next = scripts[at - 1];
        }
    }
    std::vector<piece_t> found;
    for (std::size_t at = 0; at < length; ++at) {
        const char32_t point = line[start + at];
        const source_t* source = nullptr;
        const bool common = hb_unicode_script(unicode, point) == HB_SCRIPT_COMMON || clings(point);
        // A mark, a joiner, and what's common to scripts, as a space, stay in the face before
        // them, when it has them.
        if (!found.empty() &&
            (clings(point) || (common && sources_t::has(*found.back().source.face, point)))) {
            source = &found.back().source;
        } else {
            source = sources.first_with(point);
        }
        const source_t chosen = source != nullptr ? *source : sources.primary();
        if (!found.empty() && found.back().source.face == chosen.face &&
            found.back().script == scripts[at]) {
            ++found.back().length;
        } else {
            found.push_back(
                { .start = start + at, .length = 1, .source = chosen, .script = scripts[at] });
        }
    }
    return found;
}

/// Shapes `piece` of `line`, `rtl` or not, at `pixels` an em, its glyphs after `pen`, which it
/// moves past them.
void shape(const std::u32string& line, const piece_t& piece, bool rtl, float pixels,
           std::size_t index, float& pen, std::vector<shaped_t>* into) {
    const face_t& face = *piece.source.face;
    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_codepoints(buffer, reinterpret_cast<const hb_codepoint_t*>(line.data()),
                             static_cast<int>(line.size()), static_cast<unsigned int>(piece.start),
                             static_cast<int>(piece.length));
    hb_buffer_set_direction(buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    if (piece.script != HB_SCRIPT_COMMON && piece.script != HB_SCRIPT_INHERITED &&
        piece.script != HB_SCRIPT_UNKNOWN) {
        hb_buffer_set_script(buffer, piece.script);
    }
    hb_buffer_guess_segment_properties(buffer);
    const float bold = piece.source.bolder ? emboldening : 0.0F;
    hb_font_set_synthetic_bold(face.hb, bold, bold, widening);
    hb_font_set_synthetic_slant(face.hb, piece.source.slanted ? slant : 0.0F);
    hb_shape(face.hb, buffer, nullptr, 0);
    unsigned int count = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
    const float units = pixels / static_cast<float>(face.units_per_em);
    for (unsigned int at = 0; at < count; ++at) {
        if (into != nullptr) {
            into->push_back({
                .source = piece.source,
                .glyph = infos[at].codepoint,
                .x = pen + (static_cast<float>(positions[at].x_offset) * units),
                .up = static_cast<float>(positions[at].y_offset) * units,
                .line = index,
            });
        }
        pen += static_cast<float>(positions[at].x_advance) * units;
    }
    hb_font_set_synthetic_bold(face.hb, 0.0F, 0.0F, widening);
    hb_font_set_synthetic_slant(face.hb, 0.0F);
    hb_buffer_destroy(buffer);
}

/// `line` laid out at `pixels` an em, its glyphs into `into`; its advance.
float lay_line(const std::u32string& line, std::size_t index, float pixels, sources_t& sources,
               std::vector<shaped_t>* into) {
    float pen = 0.0F;
    if (line.empty()) {
        return pen;
    }
    const SBCodepointSequence sequence{
        .stringEncoding = SBStringEncodingUTF32,
        .stringBuffer = line.data(),
        .stringLength = line.size(),
    };
    SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
    SBParagraphRef paragraph =
        algorithm != nullptr
            ? SBAlgorithmCreateParagraph(algorithm, 0, line.size(), SBLevelDefaultLTR)
            : nullptr;
    SBLineRef runs =
        paragraph != nullptr ? SBParagraphCreateLine(paragraph, 0, line.size()) : nullptr;
    if (runs == nullptr) {
        // Without its runs, the line's one, left to right.
        for (const piece_t& piece : pieces(line, 0, line.size(), sources)) {
            shape(line, piece, false, pixels, index, pen, into);
        }
    } else {
        const SBRun* run = SBLineGetRunsPtr(runs);
        for (SBUInteger at = 0; at < SBLineGetRunCount(runs); ++at, ++run) {
            const bool rtl = (run->level & 1U) != 0;
            std::vector<piece_t> found = pieces(line, run->offset, run->length, sources);
            // A right-to-left run's pieces are seen last first.
            if (rtl) {
                std::ranges::reverse(found);
            }
            for (const piece_t& piece : found) {
                shape(line, piece, rtl, pixels, index, pen, into);
            }
        }
    }
    SBLineRelease(runs);
    SBParagraphRelease(paragraph);
    SBAlgorithmRelease(algorithm);
    return pen;
}

/// `text`'s lines, by '\n', a "\r\n"'s '\r' and other control characters dropped.
std::vector<std::u32string> lines_of(std::string_view text) {
    std::vector<std::u32string> lines(1);
    for (const char32_t point : decoded(text)) {
        if (point == U'\n') {
            lines.emplace_back();
        } else if (point >= 0x20 && point != 0x7F) {
            lines.back().push_back(point);
        }
    }
    return lines;
}

/// A font's line height and ascent, in pixels at `pixels` an em.
void line_metrics(const font_t& font, float pixels, float& height, float& ascent) {
    const face_t& face = *font.face;
    const float units = pixels / static_cast<float>(face.units_per_em);
    hb_position_t above = 0;
    hb_position_t below = 0;
    hb_position_t gap = 0;
    hb_ot_metrics_get_position_with_fallback(face.hb, HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER,
                                             &above);
    hb_ot_metrics_get_position_with_fallback(face.hb, HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER,
                                             &below);
    hb_ot_metrics_get_position_with_fallback(face.hb, HB_OT_METRICS_TAG_HORIZONTAL_LINE_GAP, &gap);
    ascent = static_cast<float>(above) * units;
    height = static_cast<float>(above - below + gap) * units;
}

/// `text` in `font`, shaped at `pixels` an em, its glyphs into `into` if it isn't null: its
/// widest line, and how many lines it has.
float shaped(const font_t& font, std::string_view text, float pixels, std::vector<shaped_t>* into,
             std::size_t& lines) {
    sources_t sources(font);
    const std::vector<std::u32string> split = lines_of(text);
    float widest = 0.0F;
    for (std::size_t index = 0; index < split.size(); ++index) {
        widest = std::max(widest, lay_line(split[index], index, pixels, sources, into));
    }
    lines = split.size();
    return widest;
}

} // namespace

bool lay_out(std::uint64_t font, std::string_view text, float scale, layout_t& into) {
    const font_t* found = font_of(font);
    if (found == nullptr) {
        return false;
    }
    if (scale <= 0.0F || !std::isfinite(scale)) {
        return fbe::fail("a scale is over 0");
    }
    const float pixels = found->size * scale;
    std::vector<shaped_t> glyphs;
    std::size_t lines = 0;
    into.glyphs.clear();
    into.width = shaped(*found, text, pixels, &glyphs, lines);
    float line_height = 0.0F;
    line_metrics(*found, pixels, line_height, into.ascent);
    into.height = line_height * static_cast<float>(lines);
    const auto em = static_cast<std::int32_t>(std::lround(pixels * 64.0F));
    for (const shaped_t& each : glyphs) {
        // At a whole pixel and a quarter of one across, and a whole pixel down.
        const float floored = std::floor(each.x);
        auto whole = static_cast<std::int32_t>(floored);
        auto quarter = static_cast<std::int32_t>(std::lround((each.x - floored) * 4.0F));
        if (quarter == 4) {
            whole += 1;
            quarter = 0;
        }
        const auto baseline =
            static_cast<std::int32_t>(std::lround(line_height * static_cast<float>(each.line))) -
            static_cast<std::int32_t>(std::lround(each.up));
        const glyph_t* glyph = rasterized(*each.source.face, each.glyph, em, each.source.bolder,
                                          each.source.slanted, quarter);
        if (glyph == nullptr || glyph->place.empty()) {
            continue;
        }
        into.glyphs.push_back({
            .page = glyph->page,
            .source = glyph->place,
            .x = whole + glyph->left,
            .y = baseline - glyph->top,
        });
    }
    into.generation = atlas().generation();
    return true;
}

bool measure(std::uint64_t font, std::string_view text, float& width, float& height) {
    const font_t* found = font_of(font);
    if (found == nullptr) {
        return false;
    }
    std::size_t lines = 0;
    width = shaped(*found, text, found->size, nullptr, lines);
    float line_height = 0.0F;
    float ascent = 0.0F;
    line_metrics(*found, found->size, line_height, ascent);
    height = line_height * static_cast<float>(lines);
    return true;
}

} // namespace fbe::text

extern "C" {

FBE_API bool fbe_text_measure(uint64_t font, const char* text, fbe_text_size_t* size) {
    if (size == nullptr) {
        return fbe::fail("there's nowhere to put the text's size");
    }
    return fbe::text::measure(font, text != nullptr ? text : "", size->width, size->height);
}

FBE_API bool fbe_text_has_glyphs(uint64_t font, const char* text) {
    const fbe::text::font_t* found = fbe::text::font_of(font);
    if (found == nullptr) {
        return false;
    }
    fbe::text::sources_t sources(*found);
    for (const std::u32string& line : fbe::text::lines_of(text != nullptr ? text : "")) {
        for (const char32_t point : line) {
            if (!fbe::text::clings(point) && sources.first_with(point) == nullptr) {
                return false;
            }
        }
    }
    return true;
}

} // extern "C"
