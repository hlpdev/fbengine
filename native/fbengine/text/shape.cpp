// Shaping: text split into lines, each line into runs by direction (SheenBidi), each run into
// pieces of one script and one face, the first of a font's, its fallbacks', and the OS's that has
// a character's glyph, each piece shaped by HarfBuzz, and the glyphs placed in the order they're
// seen, a right-to-left run's turned around.

#include "core/engine.hpp"
#include "fbengine.h"
#include "text/font.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <SheenBidi/SheenBidi.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbe::text {

namespace {

/// The amount a face that isn't bold is made bolder, an em's fraction, as FreeType's own
/// emboldening is, and how far one that doesn't slant is slanted, across for each up.
constexpr float emboldening = 1.0F / 24.0F;

/// The biggest em glyphs are rasterized at, in pixels, past which they're drawn bigger; and how far
/// from the origin a glyph is placed at most, a canvas's limit.
constexpr float largest_em = 512.0F;
constexpr double farthest = 16777216.0;
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

/// Whether `point` is one a font needn't have for a cluster it's in, as HarfBuzz hides it:
/// joiners, variation selectors, tags, and the other invisible formats.
bool ignorable(char32_t point) {
    return point == 0x00AD || point == 0x034F || (point >= 0x180B && point <= 0x180F) ||
           (point >= 0x200B && point <= 0x200F) || (point >= 0x202A && point <= 0x202E) ||
           (point >= 0x2060 && point <= 0x206F) || (point >= 0xFE00 && point <= 0xFE0F) ||
           point == 0xFEFF || (point >= 0xE0000 && point <= 0xE0FFF);
}

/// Whether `point` is a regional indicator, two of which are a flag.
bool regional(char32_t point) {
    return point >= 0x1F1E6 && point <= 0x1F1FF;
}

/// Unicode's Emoji_Presentation (emoji-data.txt, 16.0): what's an emoji, in color, unless FE0E
/// asks for text.
constexpr auto emoji_ranges = std::to_array<std::pair<char32_t, char32_t>>({
    { 0x231A, 0x231B },   { 0x23E9, 0x23EC },   { 0x23F0, 0x23F0 },   { 0x23F3, 0x23F3 },
    { 0x25FD, 0x25FE },   { 0x2614, 0x2615 },   { 0x2648, 0x2653 },   { 0x267F, 0x267F },
    { 0x2693, 0x2693 },   { 0x26A1, 0x26A1 },   { 0x26AA, 0x26AB },   { 0x26BD, 0x26BE },
    { 0x26C4, 0x26C5 },   { 0x26CE, 0x26CE },   { 0x26D4, 0x26D4 },   { 0x26EA, 0x26EA },
    { 0x26F2, 0x26F3 },   { 0x26F5, 0x26F5 },   { 0x26FA, 0x26FA },   { 0x26FD, 0x26FD },
    { 0x2705, 0x2705 },   { 0x270A, 0x270B },   { 0x2728, 0x2728 },   { 0x274C, 0x274C },
    { 0x274E, 0x274E },   { 0x2753, 0x2755 },   { 0x2757, 0x2757 },   { 0x2795, 0x2797 },
    { 0x27B0, 0x27B0 },   { 0x27BF, 0x27BF },   { 0x2B1B, 0x2B1C },   { 0x2B50, 0x2B50 },
    { 0x2B55, 0x2B55 },   { 0x1F004, 0x1F004 }, { 0x1F0CF, 0x1F0CF }, { 0x1F18E, 0x1F18E },
    { 0x1F191, 0x1F19A }, { 0x1F1E6, 0x1F1FF }, { 0x1F201, 0x1F201 }, { 0x1F21A, 0x1F21A },
    { 0x1F22F, 0x1F22F }, { 0x1F232, 0x1F236 }, { 0x1F238, 0x1F23A }, { 0x1F250, 0x1F251 },
    { 0x1F300, 0x1F320 }, { 0x1F32D, 0x1F335 }, { 0x1F337, 0x1F37C }, { 0x1F37E, 0x1F393 },
    { 0x1F3A0, 0x1F3CA }, { 0x1F3CF, 0x1F3D3 }, { 0x1F3E0, 0x1F3F0 }, { 0x1F3F4, 0x1F3F4 },
    { 0x1F3F8, 0x1F43E }, { 0x1F440, 0x1F440 }, { 0x1F442, 0x1F4FC }, { 0x1F4FF, 0x1F53D },
    { 0x1F54B, 0x1F54E }, { 0x1F550, 0x1F567 }, { 0x1F57A, 0x1F57A }, { 0x1F595, 0x1F596 },
    { 0x1F5A4, 0x1F5A4 }, { 0x1F5FB, 0x1F64F }, { 0x1F680, 0x1F6C5 }, { 0x1F6CC, 0x1F6CC },
    { 0x1F6D0, 0x1F6D2 }, { 0x1F6D5, 0x1F6D7 }, { 0x1F6DC, 0x1F6DF }, { 0x1F6EB, 0x1F6EC },
    { 0x1F6F4, 0x1F6FC }, { 0x1F7E0, 0x1F7EB }, { 0x1F7F0, 0x1F7F0 }, { 0x1F90C, 0x1F93A },
    { 0x1F93C, 0x1F945 }, { 0x1F947, 0x1F9FF }, { 0x1FA70, 0x1FA7C }, { 0x1FA80, 0x1FA89 },
    { 0x1FA8F, 0x1FAC6 }, { 0x1FACE, 0x1FADC }, { 0x1FADF, 0x1FAE9 }, { 0x1FAF0, 0x1FAF8 },
});

bool emoji_presented(char32_t point) {
    const auto at = std::ranges::lower_bound(emoji_ranges, point, {},
                                             [](const auto& range) { return range.second; });
    return at != emoji_ranges.end() && at->first <= point;
}

/// How a cluster asks to be shown: as an emoji, in color, as text, or as its font has it.
enum class presentation_t : std::uint8_t { ANY, EMOJI, TEXT };

/// One of the faces a font's glyphs come from, and what of the font's style it's made.
struct source_t {
    face_t* face = nullptr;
    bool bolder = false;
    bool slanted = false;
};

/// A font's faces, in the order its glyphs come from them: its own, its fallbacks', then the OS's,
/// each of the OS's read whole only once it gives a glyph, its coverage telling before that.
class sources_t {
public:
    explicit sources_t(const font_t& font) : m_font(font) {
        m_own.push_back(
            { .face = font.face.get(), .bolder = font.bolder, .slanted = font.slanted });
        for (const std::uint64_t handle : font.fallbacks) {
            if (const font_t* fallback = font_of(handle, false)) {
                if (std::ranges::none_of(m_own, [&](const source_t& each) {
                        return each.face == fallback->face.get();
                    })) {
                    m_kept.push_back(fallback->face);
                    m_own.push_back(styled_source(*fallback->face));
                }
            }
        }
    }

    /// The face for a cluster of `points`: the first that draws all of them but the ignorable, an
    /// emoji's color faces and the OS's emoji font first, a text one's those without color; else
    /// the first that draws its first; else null.
    const source_t* chosen(std::span<const char32_t> points, presentation_t presentation) {
        std::vector<char32_t> needed;
        for (const char32_t point : points) {
            if (!ignorable(point)) {
                needed.push_back(point);
            }
        }
        if (needed.empty()) {
            needed.push_back(points.front());
        }
        if (presentation == presentation_t::EMOJI) {
            for (const source_t& each : m_own) {
                if (FT_HAS_COLOR(each.face->ft) && draws_all(*each.face, needed)) {
                    return &each;
                }
            }
            if (const source_t* emoji = from_system(system_emoji_fallback(), needed)) {
                return emoji;
            }
        }
        for (const bool colored : { false, true }) {
            for (const source_t& each : m_own) {
                const bool skip =
                    presentation == presentation_t::TEXT && FT_HAS_COLOR(each.face->ft) != colored;
                if (!skip && draws_all(*each.face, needed)) {
                    return &each;
                }
            }
            if (presentation != presentation_t::TEXT) {
                break;
            }
        }
        for (std::size_t index = 0; index < system_fallback_count(); ++index) {
            if (const source_t* found = from_system(index, needed)) {
                return found;
            }
        }
        // None has the whole cluster: the first with its first.
        const std::vector<char32_t> first{ needed.front() };
        for (const source_t& each : m_own) {
            if (draws_all(*each.face, first)) {
                return &each;
            }
        }
        for (std::size_t index = 0; index < system_fallback_count(); ++index) {
            if (const source_t* found = from_system(index, first)) {
                return found;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const source_t& primary() const {
        return m_own.front();
    }

    static bool draws_all(face_t& face, std::span<const char32_t> points) {
        return std::ranges::all_of(points, [&](char32_t point) { return draws(face, point); });
    }

private:
    /// The font's style, made where `face` isn't so.
    [[nodiscard]] source_t styled_source(face_t& face) const {
        return {
            .face = &face,
            .bolder = (m_font.style & bold_bit) != 0U && face.weight < 600,
            .slanted = (m_font.style & italic_bit) != 0U && !face.italic,
        };
    }

    /// The OS's `index`th fallback face, if its coverage has all of `points` and it draws them,
    /// read whole only then.
    const source_t* from_system(std::size_t index, std::span<const char32_t> points) {
        if (!m_font.system_fallback || index >= system_fallback_count() ||
            !std::ranges::all_of(
                points, [&](char32_t point) { return system_fallback_has(index, point); })) {
            return nullptr;
        }
        for (const auto& [at, source] : m_system) {
            if (at == index) {
                return draws_all(*source.face, points) ? &source : nullptr;
            }
        }
        const std::shared_ptr<face_t> face = system_fallback(index);
        if (face == nullptr) {
            return nullptr;
        }
        m_kept.push_back(face);
        m_system.emplace_back(index, styled_source(*face));
        return draws_all(*face, points) ? &m_system.back().second : nullptr;
    }

    const font_t& m_font;
    std::vector<source_t> m_own;
    /// The OS's faces taken so far, by their place in its list; a deque's, so they stay put.
    std::deque<std::pair<std::size_t, source_t>> m_system;
    std::vector<std::shared_ptr<face_t>> m_kept;
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

/// Where the cluster starting at `at` of `line`'s points up to `end` ends: its marks, joiners,
/// and variation selectors, what a zero-width joiner joins to it, and a flag's second half.
std::size_t cluster_end(const std::u32string& line, std::size_t at, std::size_t end) {
    std::size_t next = at + 1;
    if (regional(line[at]) && next < end && regional(line[next])) {
        ++next;
    }
    while (next < end && (clings(line[next]) || line[next - 1] == 0x200D)) {
        ++next;
    }
    return next;
}

/// The pieces of `line`'s points from `start`, `length` of them, each of one script and face, the
/// face chosen for each cluster.
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
    const std::size_t end = start + length;
    for (std::size_t at = start; at < end;) {
        const std::size_t after = cluster_end(line, at, end);
        const std::span<const char32_t> cluster(line.data() + at, after - at);
        presentation_t presentation =
            emoji_presented(line[at]) ? presentation_t::EMOJI : presentation_t::ANY;
        for (const char32_t point : cluster) {
            if (point == 0xFE0F) {
                presentation = presentation_t::EMOJI;
            } else if (point == 0xFE0E) {
                presentation = presentation_t::TEXT;
            }
        }
        // What's common to scripts, as a space, stays in the face before it, when that draws it.
        const bool common = hb_unicode_script(unicode, line[at]) == HB_SCRIPT_COMMON &&
                            presentation != presentation_t::EMOJI;
        const source_t* source = nullptr;
        if (!found.empty() && common && sources_t::draws_all(*found.back().source.face, cluster)) {
            source = &found.back().source;
        } else {
            source = sources.chosen(cluster, presentation);
        }
        const source_t chosen = source != nullptr ? *source : sources.primary();
        for (std::size_t point = at; point < after; ++point) {
            const hb_script_t script = scripts[point - start];
            if (!found.empty() && found.back().source.face == chosen.face &&
                found.back().script == script) {
                ++found.back().length;
            } else {
                found.push_back(
                    { .start = point, .length = 1, .source = chosen, .script = script });
            }
        }
        at = after;
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
    // A paragraph ends at a separator, which lines_of has taken out, so it's the whole line;
    // what it isn't of the line is laid out left to right after it.
    const std::size_t covered = paragraph != nullptr ? SBParagraphGetLength(paragraph) : 0;
    SBLineRef runs = covered > 0 ? SBParagraphCreateLine(paragraph, 0, covered) : nullptr;
    const std::size_t rest = runs != nullptr ? covered : 0;
    if (runs != nullptr) {
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
        SBLineRelease(runs);
    }
    if (rest < line.size()) {
        for (const piece_t& piece : pieces(line, rest, line.size() - rest, sources)) {
            shape(line, piece, false, pixels, index, pen, into);
        }
    }
    if (paragraph != nullptr) {
        SBParagraphRelease(paragraph);
    }
    if (algorithm != nullptr) {
        SBAlgorithmRelease(algorithm);
    }
    return pen;
}

/// Whether `point` ends a line: '\n', a vertical tab, NEL, and Unicode's line and paragraph
/// separators.
bool breaks(char32_t point) {
    return point == U'\n' || point == 0x0B || point == 0x85 || point == 0x2028 || point == 0x2029;
}

/// `text`'s lines, by what breaks one, a "\r\n"'s '\r' and the other control characters, C0's
/// and C1's, dropped.
std::vector<std::u32string> lines_of(std::string_view text) {
    std::vector<std::u32string> lines(1);
    for (const char32_t point : decoded(text)) {
        if (breaks(point)) {
            lines.emplace_back();
        } else if (point >= 0x20 && (point < 0x7F || point > 0x9F)) {
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
    if (!std::isfinite(pixels)) {
        return fbe::fail("a font's size times its scale is past a float's");
    }
    std::vector<shaped_t> glyphs;
    std::size_t lines = 0;
    into.glyphs.clear();
    into.width = shaped(*found, text, pixels, &glyphs, lines);
    float line_height = 0.0F;
    line_metrics(*found, pixels, line_height, into.ascent);
    into.height = line_height * static_cast<float>(lines);
    // Rasterized at 512 pixels an em at most, and drawn bigger past it.
    const float rastered = std::min(pixels, largest_em);
    const double enlarged = static_cast<double>(pixels) / rastered;
    const auto em = static_cast<std::int32_t>(std::lround(rastered * 64.0F));
    for (const shaped_t& each : glyphs) {
        // At a whole pixel and a quarter of one across, and a whole pixel down, kept to what a
        // canvas can draw at.
        const double x = std::clamp(static_cast<double>(each.x), -farthest, farthest);
        const double floored = std::floor(x);
        auto whole = static_cast<std::int32_t>(floored);
        auto quarter = static_cast<std::int32_t>(std::lround((x - floored) * 4.0));
        if (quarter == 4 || enlarged > 1.0) {
            whole += quarter == 4 ? 1 : 0;
            quarter = 0;
        }
        const double down = std::clamp(
            std::round(static_cast<double>(line_height) * static_cast<double>(each.line)) -
                std::round(static_cast<double>(each.up)),
            -farthest, farthest);
        const auto baseline = static_cast<std::int32_t>(down);
        const glyph_t* glyph = rasterized(*each.source.face, each.glyph, em, each.source.bolder,
                                          each.source.slanted, quarter);
        if (glyph == nullptr || glyph->place.empty()) {
            continue;
        }
        atlas().used(glyph->page);
        const auto sized = [&](std::int32_t length) {
            return static_cast<std::int32_t>(std::lround(static_cast<double>(length) * enlarged));
        };
        into.glyphs.push_back({
            .page = glyph->page,
            .source = glyph->place,
            .x = whole + sized(glyph->left),
            .y = baseline - sized(glyph->top),
            .width = sized(glyph->place.width),
            .height = sized(glyph->place.height),
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
            const std::span<const char32_t> alone(&point, 1);
            if (!fbe::text::ignorable(point) &&
                sources.chosen(alone, fbe::text::presentation_t::ANY) == nullptr) {
                return false;
            }
        }
    }
    return true;
}

} // extern "C"
