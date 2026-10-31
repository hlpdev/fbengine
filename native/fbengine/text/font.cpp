// Fonts: FreeType, faces read once from files or bytes and shared by the fonts of them, and the
// pool of fonts their handles are for.

#include "text/font.hpp"

#include "core/engine.hpp"
#include "fbengine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include FT_PARAMETER_TAGS_H
#include FT_TRUETYPE_TABLES_H

#ifdef _WIN32
#include <windows.h>
#endif

namespace fbe::text {

namespace {

FT_Library freetype = nullptr;

/// How many faces have been read, the last one's id, and how many are open.
std::uint32_t faces_read = 0;
std::size_t open_faces = 0;

/// A face read from a file, kept while a font has it, so the file's read once.
struct file_face_t {
    std::filesystem::path path;
    std::int32_t index = 0;
    std::weak_ptr<face_t> face;
};

std::vector<file_face_t> file_faces;

/// Fonts by slot, null where a slot's free, each slot's generation, which goes up as its font is
/// freed, so its old handles are caught, and the slots free to take again.
std::vector<std::unique_ptr<font_t>> fonts;
std::vector<std::uint32_t> generations;
std::vector<std::uint32_t> free_slots;

/// A slot whose generation reaches this is retired, never taken again.
constexpr std::uint32_t last_generation = (1U << 24U) - 1U;

std::uint64_t handle_of(std::uint32_t slot) {
    return (std::uint64_t{ kind } << 56U) | (std::uint64_t{ generations[slot] } << 32U) | slot;
}

/// `path` as UTF-8, for what's said of it.
std::string utf8_of(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring& wide = path.native();
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(length > 0 ? length : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), length,
                        nullptr, nullptr);
    return utf8;
#else
    return path.native();
#endif
}

/// Whether `size` is a font's: over 0, and finite.
bool sized(float size) {
    if (size > 0.0F && std::isfinite(size)) {
        return true;
    }
    return fbe::fail(std::format("a font's size is over 0, not {}", size));
}

/// The font the C API's call is for, or null with fbe_error's why.
font_t* found(std::uint64_t handle) {
    return font_of(handle, true);
}

/// fbe_text_font_family's and fbe_text_font_style_name's, which the next call replaces.
std::string given;

} // namespace

face_t::face_t() {
    ++open_faces;
}

std::size_t faces_open() {
    return open_faces;
}

face_t::~face_t() {
    --open_faces;
    hb_font_destroy(hb);
    hb_face_destroy(hb_face);
    if (ft != nullptr) {
        FT_Done_Face(ft);
    }
}

FT_Library library() {
    if (freetype == nullptr && FT_Init_FreeType(&freetype) != 0) {
        freetype = nullptr;
        fbe::fail("FreeType couldn't start");
    }
    return freetype;
}

std::shared_ptr<face_t> open_face(std::vector<std::uint8_t> bytes, std::int32_t index,
                                  std::string_view named) {
    FT_Library started = library();
    if (started == nullptr) {
        return nullptr;
    }
    if (index < 0 || index > 0xFFFF) {
        fbe::fail(std::format("a font's face is 0 to 65,535, not {}", index));
        return nullptr;
    }
    auto face = std::make_shared<face_t>();
    face->bytes = std::move(bytes);
    // An sbix font's pictures are PNGs, which FreeType isn't built to read here, so it's taken as
    // its outlines, as it isn't by default.
    FT_Parameter ignore_sbix{ .tag = FT_PARAM_TAG_IGNORE_SBIX, .data = nullptr };
    FT_Open_Args arguments{};
    arguments.flags = FT_OPEN_MEMORY | FT_OPEN_PARAMS;
    arguments.memory_base = face->bytes.data();
    arguments.memory_size = static_cast<FT_Long>(face->bytes.size());
    arguments.num_params = 1;
    arguments.params = &ignore_sbix;
    const FT_Error error = FT_Open_Face(started, &arguments, index, &face->ft);
    if (error != 0) {
        face->ft = nullptr;
        // Too short for a font, FreeType's stream ends early.
        if (error == FT_Err_Unknown_File_Format || error == FT_Err_Invalid_Stream_Operation ||
            error == FT_Err_Invalid_Stream_Read) {
            fbe::fail(std::format("{} isn't a font", named));
        } else if (error == FT_Err_Invalid_Argument && index != 0) {
            fbe::fail(std::format("{} has no face {}", named, index));
        } else {
            fbe::fail(std::format("{} is a broken font (FreeType's error {})", named, error));
        }
        return nullptr;
    }
    // What HarfBuzz shapes: TrueType's and OpenType's, not bitmap fonts' others.
    if (!FT_IS_SFNT(face->ft)) {
        fbe::fail(std::format("{} isn't a TrueType or OpenType font", named));
        return nullptr;
    }
    // A font with no Unicode charmap keeps the one it has, as a symbol font's.
    FT_Select_Charmap(face->ft, FT_ENCODING_UNICODE);
    hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(face->bytes.data()),
                                     static_cast<unsigned int>(face->bytes.size()),
                                     HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    face->hb_face = hb_face_create(blob, static_cast<unsigned int>(index));
    hb_blob_destroy(blob);
    face->units_per_em = static_cast<std::int32_t>(hb_face_get_upem(face->hb_face));
    face->hb = hb_font_create(face->hb_face);
    hb_font_set_scale(face->hb, face->units_per_em, face->units_per_em);
    face->family = face->ft->family_name != nullptr ? face->ft->family_name : "";
    face->style = face->ft->style_name != nullptr ? face->ft->style_name : "";
    face->weight = weight_of(face->ft);
    face->italic = italic_of(face->ft);
    face->id = ++faces_read;
    return face;
}

std::shared_ptr<face_t> file_face(const std::filesystem::path& path, std::int32_t index) {
    for (const file_face_t& each : file_faces) {
        if (each.index == index && each.path == path) {
            if (std::shared_ptr<face_t> kept = each.face.lock()) {
                return kept;
            }
        }
    }
    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes)) {
        return nullptr;
    }
    std::shared_ptr<face_t> face = open_face(std::move(bytes), index, utf8_of(path));
    if (face == nullptr) {
        return nullptr;
    }
    std::erase_if(file_faces, [](const file_face_t& each) { return each.face.expired(); });
    file_faces.push_back({ .path = path, .index = index, .face = face });
    return face;
}

font_t styled(std::shared_ptr<face_t> face, float size, std::uint32_t style) {
    font_t font;
    font.bolder = (style & bold_bit) != 0U && face->weight < 600;
    font.slanted = (style & italic_bit) != 0U && !face->italic;
    font.face = std::move(face);
    font.size = size;
    font.style = style & (bold_bit | italic_bit | underline_bit);
    return font;
}

std::uint64_t add_font(font_t font) {
    std::uint32_t slot = 0;
    if (!free_slots.empty()) {
        slot = free_slots.back();
        free_slots.pop_back();
    } else if (fonts.size() < 0xFFFFFFFFU) {
        slot = static_cast<std::uint32_t>(fonts.size());
        fonts.emplace_back();
        generations.push_back(1);
    } else {
        fbe::fail("there are as many fonts as there can be");
        return 0;
    }
    fonts[slot] = std::make_unique<font_t>(std::move(font));
    return handle_of(slot);
}

font_t* font_of(std::uint64_t handle, bool failing) {
    const auto slot = static_cast<std::uint32_t>(handle);
    const auto generation = static_cast<std::uint32_t>(handle >> 32U) & 0xFFFFFFU;
    const auto tag = static_cast<std::uint8_t>(handle >> 56U);
    if (tag != kind || slot >= fonts.size() || fonts[slot] == nullptr ||
        generations[slot] != generation) {
        if (failing) {
            fbe::fail(handle == 0 ? "no font was given" : "the font was freed");
        }
        return nullptr;
    }
    return fonts[slot].get();
}

std::int32_t weight_of(FT_Face face) {
    const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
    if (os2 != nullptr && os2->version != 0xFFFFU && os2->usWeightClass != 0) {
        return os2->usWeightClass;
    }
    const auto flags = static_cast<unsigned long>(face->style_flags);
    return (flags & static_cast<unsigned long>(FT_STYLE_FLAG_BOLD)) != 0 ? 700 : 400;
}

bool italic_of(FT_Face face) {
    const auto flags = static_cast<unsigned long>(face->style_flags);
    return (flags & static_cast<unsigned long>(FT_STYLE_FLAG_ITALIC)) != 0;
}

std::FILE* open_read(const std::filesystem::path& path) {
    std::FILE* file = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0) {
        return nullptr;
    }
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    return file;
}

std::filesystem::path native_path(std::string_view utf8) {
#ifdef _WIN32
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                        length);
    return { wide };
#else
    return { std::string(utf8) };
#endif
}

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes) {
    std::FILE* file = open_read(path);
    if (file == nullptr) {
        return fbe::fail(std::format("couldn't open {}", utf8_of(path)));
    }
    bytes.clear();
    constexpr std::size_t chunk = std::size_t{ 1 } << 16U;
    std::size_t got = 0;
    do {
        bytes.resize(bytes.size() + chunk);
        got = std::fread(bytes.data() + bytes.size() - chunk, 1, chunk, file);
        bytes.resize(bytes.size() - chunk + got);
    } while (got == chunk);
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    return failed ? fbe::fail(std::format("couldn't read {}", utf8_of(path))) : true;
}

} // namespace fbe::text

using fbe::text::font_t;

extern "C" {

FBE_API uint64_t fbe_text_load_font(const char* path, int32_t index, float size, int32_t style) {
    if (!fbe::text::sized(size)) {
        return 0;
    }
    if (path == nullptr || *path == '\0') {
        fbe::fail("there's no font file to load");
        return 0;
    }
    const std::shared_ptr<fbe::text::face_t> face =
        fbe::text::file_face(fbe::text::native_path(path), index);
    return face != nullptr ? fbe::text::add_font(
                                 fbe::text::styled(face, size, static_cast<std::uint32_t>(style)))
                           : 0;
}

FBE_API uint64_t fbe_text_load_font_bytes(const uint8_t* bytes, int64_t count, const char* named,
                                          int32_t index, float size, int32_t style) {
    if (!fbe::text::sized(size)) {
        return 0;
    }
    if (bytes == nullptr || count <= 0) {
        fbe::fail("there are no font bytes to load");
        return 0;
    }
    std::vector<std::uint8_t> copied(bytes, bytes + count);
    const std::string_view name = named != nullptr && *named != '\0' ? named : "the font's bytes";
    const std::shared_ptr<fbe::text::face_t> face =
        fbe::text::open_face(std::move(copied), index, name);
    return face != nullptr ? fbe::text::add_font(
                                 fbe::text::styled(face, size, static_cast<std::uint32_t>(style)))
                           : 0;
}

FBE_API uint64_t fbe_text_system_font(const char* family, float size, int32_t style) {
    if (!fbe::text::sized(size)) {
        return 0;
    }
    const std::shared_ptr<fbe::text::face_t> face =
        fbe::text::system_face(family != nullptr ? family : "", static_cast<std::uint32_t>(style));
    return face != nullptr ? fbe::text::add_font(
                                 fbe::text::styled(face, size, static_cast<std::uint32_t>(style)))
                           : 0;
}

FBE_API uint64_t fbe_text_default_font(float size, int32_t style) {
    if (!fbe::text::sized(size)) {
        return 0;
    }
    const std::shared_ptr<fbe::text::face_t> face =
        fbe::text::system_default(static_cast<std::uint32_t>(style));
    return face != nullptr ? fbe::text::add_font(
                                 fbe::text::styled(face, size, static_cast<std::uint32_t>(style)))
                           : 0;
}

FBE_API uint64_t fbe_text_resize_font(uint64_t font, float size, int32_t style) {
    const font_t* had = fbe::text::found(font);
    if (had == nullptr || !fbe::text::sized(size)) {
        return 0;
    }
    font_t resized = fbe::text::styled(had->face, size, static_cast<std::uint32_t>(style));
    resized.fallbacks = had->fallbacks;
    resized.system_fallback = had->system_fallback;
    return fbe::text::add_font(std::move(resized));
}

FBE_API bool fbe_text_free_font(uint64_t font) {
    if (fbe::text::found(font) == nullptr) {
        return false;
    }
    const auto slot = static_cast<std::uint32_t>(font);
    fbe::text::fonts[slot].reset();
    // A slot whose generation would wrap is retired.
    if (++fbe::text::generations[slot] < fbe::text::last_generation) {
        fbe::text::free_slots.push_back(slot);
    }
    return true;
}

FBE_API int32_t fbe_text_font_count(void) {
    std::int32_t count = 0;
    for (const std::unique_ptr<font_t>& each : fbe::text::fonts) {
        count += each != nullptr ? 1 : 0;
    }
    return count;
}

FBE_API bool fbe_text_font_metrics(uint64_t font, fbe_text_metrics_t* metrics) {
    const font_t* found = fbe::text::found(font);
    if (found == nullptr) {
        return false;
    }
    if (metrics == nullptr) {
        return fbe::fail("there's nowhere to put the font's measures");
    }
    hb_font_t* hb = found->face->hb;
    const float units = found->size / static_cast<float>(found->face->units_per_em);
    const auto measure = [&](hb_ot_metrics_tag_t tag) {
        hb_position_t position = 0;
        hb_ot_metrics_get_position_with_fallback(hb, tag, &position);
        return static_cast<float>(position) * units;
    };
    metrics->size = found->size;
    metrics->ascent = measure(HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER);
    metrics->descent = -measure(HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER);
    metrics->line_gap = measure(HB_OT_METRICS_TAG_HORIZONTAL_LINE_GAP);
    metrics->line_height = metrics->ascent + metrics->descent + metrics->line_gap;
    metrics->underline_position = -measure(HB_OT_METRICS_TAG_UNDERLINE_OFFSET);
    metrics->underline_thickness = measure(HB_OT_METRICS_TAG_UNDERLINE_SIZE);
    return true;
}

FBE_API int32_t fbe_text_font_style(uint64_t font) {
    const font_t* found = fbe::text::found(font);
    return found != nullptr ? static_cast<std::int32_t>(found->style) : -1;
}

FBE_API const char* fbe_text_font_family(uint64_t font) {
    const font_t* found = fbe::text::found(font);
    fbe::text::given = found != nullptr ? found->face->family : "";
    return fbe::text::given.c_str();
}

FBE_API const char* fbe_text_font_style_name(uint64_t font) {
    const font_t* found = fbe::text::found(font);
    fbe::text::given = found != nullptr ? found->face->style : "";
    return fbe::text::given.c_str();
}

FBE_API bool fbe_text_add_fallback(uint64_t font, uint64_t fallback) {
    font_t* found = fbe::text::found(font);
    if (found == nullptr || fbe::text::found(fallback) == nullptr) {
        return false;
    }
    if (font == fallback) {
        return fbe::fail("a font isn't its own fallback");
    }
    if (std::ranges::find(found->fallbacks, fallback) == found->fallbacks.end()) {
        found->fallbacks.push_back(fallback);
    }
    return true;
}

FBE_API bool fbe_text_clear_fallbacks(uint64_t font) {
    font_t* found = fbe::text::found(font);
    if (found == nullptr) {
        return false;
    }
    found->fallbacks.clear();
    return true;
}

FBE_API bool fbe_text_set_system_fallback(uint64_t font, bool on) {
    font_t* found = fbe::text::found(font);
    if (found == nullptr) {
        return false;
    }
    found->system_fallback = on;
    return true;
}

FBE_API bool fbe_text_system_fallback(uint64_t font) {
    const font_t* found = fbe::text::found(font);
    return found != nullptr && found->system_fallback;
}

} // extern "C"
