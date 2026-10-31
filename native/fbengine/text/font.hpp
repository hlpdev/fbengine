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
    face_t() = default;
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
/// this OS's own list of families covering the scripts its fonts have between them.
std::size_t system_fallback_count();

/// The `index`th of them, read the first time it's asked for, or null where the OS hasn't it.
std::shared_ptr<face_t> system_fallback(std::size_t index);

/// The OS's own font for its UI, as Segoe UI is Windows', or its first, or null with fbe_error's
/// why when it has none.
std::shared_ptr<face_t> system_default(std::uint32_t style);

/// A glyph rasterized into the atlas: its page, its place there, and where that place's top left
/// is from the pen on the baseline, y down; its place is empty for one with nothing to draw, as a
/// space.
struct glyph_t {
    std::uint32_t page = 0;
    rect_t place;
    std::int32_t left = 0;
    std::int32_t top = 0;
};

/// The glyph `glyph` of `face` in the atlas, rasterized the first time it's asked for at `pixels`
/// an em, in 26.6 fixed point, made bolder and slanted as they say, and moved right by `quarter`
/// quarters of a pixel; null with fbe_error's why if FreeType can't.
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
