#ifndef FBE_TEXT_H
#define FBE_TEXT_H

// Text: fonts from files and the OS's, read by FreeType and shaped by HarfBuzz, and the size text
// takes in them. A font is a face at a size, in units of the game's own, as CSS's px are, with a
// style; a glyph it hasn't is its fallbacks', then the OS's. Drawing it is the canvas's, from the
// atlas text/text.hpp keeps.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// A font's style, its bits or'ed: bold and italic, which a face that isn't is made, and
// underlined, which the canvas draws.
#define FBE_TEXT_BOLD      1
#define FBE_TEXT_ITALIC    2
#define FBE_TEXT_UNDERLINE 4

// A font's measures, in its units, at its size: from its baseline up to its top and down to its
// bottom (a positive number), the gap it asks between lines, a line's height (the three summed),
// and its underline's place, below the baseline, and thickness.
// C has no `using`, so it's a typedef.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct fbe_text_metrics_t {
    float size;
    float ascent;
    float descent;
    float line_gap;
    float line_height;
    float underline_position;
    float underline_thickness;
} fbe_text_metrics_t;

// The size text takes, in a font's units.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct fbe_text_size_t {
    float width;
    float height;
} fbe_text_size_t;

// Loads the face at `index` (0 for a file of one) of the font file at `path`, UTF-8 (TrueType,
// OpenType, or a collection of them), at `size` with a style of FBE_TEXT_ bits; 0, with
// fbe_error's why, for a file it can't read, or one that isn't a font, or a size that isn't over 0.
// A file is read once, however many fonts are loaded from it. fbe_text_free_font frees the font.
FBE_API uint64_t fbe_text_load_font(const char* path, int32_t index, float size, int32_t style);
// The same from the font file's bytes, `count` of them at `bytes`, which it copies, its errors
// naming them `named`, as their file's path, or "the font's bytes" for "".
FBE_API uint64_t fbe_text_load_font_bytes(const uint8_t* bytes, int64_t count, const char* named,
                                          int32_t index, float size, int32_t style);
// The OS's font of `family`, as "Arial", or a family and a style as its fonts name them, as
// "Segoe UI Semibold", nearest `style`; 0, with fbe_error's why, when the OS has none of it.
FBE_API uint64_t fbe_text_system_font(const char* family, float size, int32_t style);
// The OS's own font for its UI, as Segoe UI is Windows'; 0, with fbe_error's why, when it has no
// font at all.
FBE_API uint64_t fbe_text_default_font(float size, int32_t style);
// Another font of `font`'s face, at `size` and `style`, with its fallbacks; 0 with fbe_error's why.
FBE_API uint64_t fbe_text_resize_font(uint64_t font, float size, int32_t style);
// Frees a font; false if the handle isn't a font's.
FBE_API bool fbe_text_free_font(uint64_t font);
// How many fonts there are, unfreed.
FBE_API int32_t fbe_text_font_count(void);

// A font's measures, into `metrics`; false if the handle isn't a font's.
FBE_API bool fbe_text_font_metrics(uint64_t font, fbe_text_metrics_t* metrics);
// Its style, its FBE_TEXT_ bits, -1 if the handle isn't a font's.
FBE_API int32_t fbe_text_font_style(uint64_t font);
// Its face's family and style, as the font names them, "" if the handle isn't a font's.
FBE_API const char* fbe_text_font_family(uint64_t font);
FBE_API const char* fbe_text_font_style_name(uint64_t font);

// Adds `fallback` after the fallbacks it has: its face gives the glyphs the font's hasn't, at the
// font's size and style; `fallback`'s own fallbacks aren't the font's. False if either isn't a
// font's, or they're one.
FBE_API bool fbe_text_add_fallback(uint64_t font, uint64_t fallback);
// Takes its fallbacks away; false if the handle isn't a font's.
FBE_API bool fbe_text_clear_fallbacks(uint64_t font);
// Whether a glyph it and its fallbacks haven't is the OS's fonts', as it is at first; false if the
// handle isn't a font's.
FBE_API bool fbe_text_set_system_fallback(uint64_t font, bool on);
FBE_API bool fbe_text_system_fallback(uint64_t font);

// The size `text`, UTF-8, takes in `font`, its lines by '\n', into `size`: its widest line's
// advance, and its lines times its line height; false if the handle isn't a font's.
FBE_API bool fbe_text_measure(uint64_t font, const char* text, fbe_text_size_t* size);
// Whether `font`, with its fallbacks and the OS's, has a glyph for each character of `text`.
FBE_API bool fbe_text_has_glyphs(uint64_t font, const char* text);

// Adds a folder of font files, and those in the folders in it, as a game's own fonts, whose
// families fbe_text_system_font finds before the OS's; false if it isn't a folder.
FBE_API bool fbe_text_add_font_folder(const char* folder);
// The OS's font families, and those of the folders added, each once, in order: the count lists
// them again, and the index is into what it listed; "" for no such one.
FBE_API int32_t fbe_text_system_family_count(void);
FBE_API const char* fbe_text_system_family_at(int32_t index);

#ifdef __cplusplus
}
#endif

#endif
