#pragma once

// What text's own files share: faces, a font file's read once by FreeType and HarfBuzz both;
// fonts, the handles' objects, a face at a size and a style; the OS's fonts by family; and the
// files they're read from.

#include "text/text.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <ft2build.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include FT_FREETYPE_H
#include <hb-ot.h>
#include <hb.h>

namespace fbe::text {

/// fbe_text.h's style bits, a font's asked-for boldness and slant.
constexpr std::uint32_t bold_bit = 1U;
constexpr std::uint32_t italic_bit = 2U;
constexpr std::uint32_t underline_bit = 4U;

/// A face of a font file, read once and shared by the fonts of it: FreeType's, for its glyphs, and
/// HarfBuzz's, for shaping, at its units per em, over the same bytes.
struct face_t {
    face_t();
    face_t(const face_t&) = delete;
    face_t& operator=(const face_t&) = delete;
    face_t(face_t&&) = delete;
    face_t& operator=(face_t&&) = delete;
    ~face_t();

    /// Its own number, which the atlas's glyphs are kept by.
    std::uint32_t id = 0;
    std::vector<std::uint8_t> bytes;
    FT_Face ft = nullptr;
    hb_face_t* hb_face = nullptr;
    hb_font_t* hb = nullptr;
    std::string family;
    std::string style;
    /// OS/2's weight, 400 regular and 700 bold, and whether it slants.
    std::int32_t weight = 400;
    bool italic = false;
    std::int32_t units_per_em = 1000;
    /// Whether each glyph asked of it can be drawn, at each size it's asked at, by glyph << 32 |
    /// size: an outline, COLR's layers, or a bitmap FreeType reads, not a PNG or SVG picture alone.
    std::unordered_map<std::uint64_t, bool> drawable;
};

/// A font, as its handle is: a face at a size, in its own units, the style asked of it, what of
/// that its face isn't and is made so, and the fonts its missing glyphs are from.
struct font_t {
    std::shared_ptr<face_t> face;
    float size = 12.0F;
    std::uint32_t style = 0;
    bool bolder = false;
    bool slanted = false;
    std::vector<std::uint64_t> fallbacks;
    bool system_fallback = true;
};

/// How many faces are read and kept, by a font or a fallback.
std::size_t faces_open();

/// FreeType, started the first time anything reads a font; null with fbe_error's why if it can't.
FT_Library library();

/// The face at `index` of the font in `bytes`, read by both, or null with fbe_error's why.
std::shared_ptr<face_t> open_face(std::vector<std::uint8_t> bytes, std::int32_t index,
                                  std::string_view named);

/// The face at `index` of the font file at `path`, read once, then shared; null with fbe_error's
/// why.
std::shared_ptr<face_t> file_face(const std::filesystem::path& path, std::int32_t index);

/// `font` made with `face`, the style asked of it made where the face isn't so.
font_t styled(std::shared_ptr<face_t> face, float size, std::uint32_t style);

/// A new handle for `font`, or 0 when no more fit.
std::uint64_t add_font(font_t font);

/// The font a handle is for, or null, with fbe_error's why when `failing`.
font_t* font_of(std::uint64_t handle, bool failing = true);

/// The OS's face of `family` nearest `style`, its weight and slant, or null with fbe_error's why.
std::shared_ptr<face_t> system_face(std::string_view family, std::uint32_t style);

/// The OS's families, and those of the folders added, each once, in order.
std::vector<std::string> system_families();

/// Adds a folder of fonts, and those in it, whose families come before the OS's own; false with
/// fbe_error's why if it isn't a folder.
bool add_folder(const std::filesystem::path& folder);

/// How many faces the OS's fonts fall back to for a glyph a font and its fallbacks haven't, from
/// this OS's own list of families covering the scripts its fonts have between them, and which of
/// them is its color emoji font (the count where it hasn't one).
std::size_t system_fallback_count();
std::size_t system_emoji_fallback();

/// Whether the `index`th has `point`, by its coverage, read from its file's character map the
/// first time, without reading the file whole: none for a face it can't draw.
bool system_fallback_has(std::size_t index, char32_t point);

/// The `index`th, read whole the first time it gives a glyph, then kept; null where the OS hasn't
/// it.
std::shared_ptr<face_t> system_fallback(std::size_t index);

/// Lists the OS's fonts now, as the first lookup by family or a fallback to them would.
void scan_system();

/// The OS's own font for its UI, as Segoe UI is Windows', or its first, or null with fbe_error's
/// why when it has none.
std::shared_ptr<face_t> system_default(std::uint32_t style);

/// A glyph rasterized into the atlas: its page, its place there, and where that place's top left
/// is from the pen on the baseline, y down; its place is empty for one with nothing to draw, as a
/// space. `scale` is how much bigger it's drawn than it's rasterized, past 1 for one more than 2048
/// pixels across or down; `shifted`, whether a quarter's move right, and a made bolder and slant,
/// are rasterized into it: an outline's are, a color glyph's or a strike's bitmap's aren't.
struct glyph_t {
    std::uint32_t page = 0;
    rect_t place;
    std::int32_t left = 0;
    std::int32_t top = 0;
    double scale = 1.0;
    bool shifted = true;
};

/// Whether `face` can draw its glyph for `point` at `pixels` an em, 26.6: it has one, and FreeType
/// can load it, in color or not; kept for each glyph and size.
bool draws(face_t& face, char32_t point, std::int32_t pixels);

/// The glyph `glyph` of `face` in the atlas, rasterized the first time it's asked for at `pixels`
/// an em, in 26.6 fixed point, made bolder and slanted as they say, and moved right by `quarter`
/// quarters of a pixel, where it's an outline; past 2048 pixels across or down, rasterized smaller;
/// null with fbe_error's why if FreeType can't.
const glyph_t* rasterized(face_t& face, std::uint32_t glyph, std::int32_t pixels, bool bolder,
                          bool slanted, std::int32_t quarter);

/// A premultiplied color's channel, straight, by its alpha; 0 where nothing's covered.
std::uint8_t straight(std::uint8_t premultiplied, std::uint8_t alpha);

/// A path given as UTF-8, as the OS takes it.
std::filesystem::path native_path(std::string_view utf8);

/// The file at `path` opened to read, or null.
std::FILE* open_read(const std::filesystem::path& path);

/// A face's weight, OS/2's, or by its style's flags without one, 400 regular and 700 bold; and
/// whether it slants.
std::int32_t weight_of(FT_Face face);
bool italic_of(FT_Face face);

/// Reads the file at `path` whole, or false with fbe_error's why.
bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes);

} // namespace fbe::text
