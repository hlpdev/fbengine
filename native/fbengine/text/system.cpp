// The OS's fonts: its font folders' files, and those of folders a game adds, listed the first time
// a font's looked up by family, each face by its family, style, weight, and slant; the OS's own
// font for its UI; and the families its fonts fall back to for glyphs a font hasn't.

#include "core/engine.hpp"
#include "fbengine.h"
#include "text/font.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include FT_TRUETYPE_TABLES_H

#ifdef _WIN32
#include <windows.h>
#endif

namespace fbe::text {

namespace {

/// A face in a font folder, as it's listed.
struct listed_t {
    std::filesystem::path path;
    std::int32_t index = 0;
    std::string family;
    std::string style;
    std::int32_t weight = 400;
    bool italic = false;
};

std::vector<listed_t> listed;
/// Whether the OS's folders are listed, and the folders a game added, which come first.
bool listed_os = false;
std::vector<std::filesystem::path> added;

/// fbe_text_system_family_at's families, as the count listed them.
std::vector<std::string> families;

/// The fallback faces read so far, by their place in the OS's list, and whether each was tried.
std::vector<std::shared_ptr<face_t>> fallbacks;
std::vector<bool> tried;

/// The families this OS's fonts fall back to, in order: its UI's, then those of other scripts.
#ifdef _WIN32
constexpr std::array fallback_families{
    "Segoe UI",
    "Segoe UI Symbol",
    "Segoe UI Emoji",
    "Segoe UI Historic",
    "Nirmala UI",
    "Ebrima",
    "Gadugi",
    "Leelawadee UI",
    "Microsoft YaHei",
    "Yu Gothic",
    "Malgun Gothic",
    "Microsoft JhengHei",
    "Javanese Text",
    "Myanmar Text",
    "Mongolian Baiti",
    "Microsoft Himalaya",
    "Microsoft Yi Baiti",
    "Microsoft Tai Le",
    "Microsoft New Tai Lue",
    "Sylfaen",
    "Arial",
    "Arial Unicode MS",
    "SimSun-ExtB",
};
constexpr std::array default_families{ "Segoe UI", "Tahoma", "Arial" };
#elifdef __APPLE__
constexpr std::array fallback_families{
    "Helvetica Neue",      "Lucida Grande", "Apple Color Emoji",
    "Apple Symbols",       "PingFang SC",   "Hiragino Sans",
    "Apple SD Gothic Neo", "Geeza Pro",     "Arial Hebrew",
    "Kohinoor Devanagari", "Thonburi",      "Arial Unicode MS",
    "STIXGeneral",
};
constexpr std::array default_families{ "SF Pro", "Helvetica Neue", "Helvetica", "Arial" };
#else
constexpr std::array fallback_families{
    "DejaVu Sans",
    "Noto Sans",
    "Noto Sans CJK SC",
    "Noto Sans Arabic",
    "Noto Sans Hebrew",
    "Noto Sans Devanagari",
    "Noto Sans Thai",
    "Noto Color Emoji",
    "Noto Sans Symbols",
    "Noto Sans Symbols2",
    "Droid Sans Fallback",
    "WenQuanYi Micro Hei",
    "Unifont",
};
constexpr std::array default_families{ "Noto Sans", "DejaVu Sans", "Liberation Sans",
                                       "Ubuntu",    "Cantarell",   "FreeSans" };
#endif

/// Whether `a` and `b` are the same, ASCII's letters in any case, as families are.
bool same_name(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

/// An environment variable's value, "" for none.
std::filesystem::path from_environment(const char* name) {
#ifdef _WIN32
    const std::wstring wide(name, name + std::char_traits<char>::length(name));
    const DWORD length = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (length == 0) {
        return {};
    }
    std::wstring value(length, L'\0');
    value.resize(GetEnvironmentVariableW(wide.c_str(), value.data(), length));
    return { value };
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::filesystem::path(value) : std::filesystem::path();
#endif
}

/// The OS's font folders.
std::vector<std::filesystem::path> os_folders() {
    std::vector<std::filesystem::path> folders;
#ifdef _WIN32
    std::array<wchar_t, MAX_PATH> windows{};
    const UINT length = GetWindowsDirectoryW(windows.data(), MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        folders.push_back(std::filesystem::path(std::wstring(windows.data(), length)) / L"Fonts");
    }
    const std::filesystem::path local = from_environment("LOCALAPPDATA");
    if (!local.empty()) {
        folders.push_back(local / L"Microsoft" / L"Windows" / L"Fonts");
    }
#elifdef __APPLE__
    folders.emplace_back("/System/Library/Fonts");
    folders.emplace_back("/Library/Fonts");
    const std::filesystem::path home = from_environment("HOME");
    if (!home.empty()) {
        folders.push_back(home / "Library" / "Fonts");
    }
#else
    folders.emplace_back("/usr/share/fonts");
    folders.emplace_back("/usr/local/share/fonts");
    const std::filesystem::path data = from_environment("XDG_DATA_HOME");
    const std::filesystem::path home = from_environment("HOME");
    if (!data.empty()) {
        folders.push_back(data / "fonts");
    } else if (!home.empty()) {
        folders.push_back(home / ".local" / "share" / "fonts");
    }
    if (!home.empty()) {
        folders.push_back(home / ".fonts");
    }
#endif
    return folders;
}

/// Whether `path` is a font file FreeType reads, by its extension.
bool font_file(const std::filesystem::path& path) {
    std::string extension;
    for (const auto each : path.extension().native()) {
        const auto low = static_cast<unsigned char>(static_cast<unsigned long>(each) & 0x7FU);
        extension += static_cast<char>(std::tolower(low));
    }
    return extension == ".ttf" || extension == ".otf" || extension == ".ttc" || extension == ".otc";
}

/// FreeType's reads of a listed file, which it seeks in, as `count` 0 does.
unsigned long read_stream(FT_Stream stream, unsigned long offset, unsigned char* buffer,
                          unsigned long count) {
    auto* file = static_cast<std::FILE*>(stream->descriptor.pointer);
    if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
        return count == 0 ? 1 : 0;
    }
    return count == 0 ? 0 : static_cast<unsigned long>(std::fread(buffer, 1, count, file));
}

/// Lists the faces of the font file at `path`, reading only what FreeType needs of it.
void list_file(FT_Library started, const std::filesystem::path& path) {
    std::FILE* file = open_read(path);
    if (file == nullptr) {
        return;
    }
    FT_StreamRec stream{};
    stream.descriptor.pointer = file;
    stream.read = read_stream;
    if (std::fseek(file, 0, SEEK_END) == 0) {
        const long size = std::ftell(file);
        stream.size = size > 0 ? static_cast<unsigned long>(size) : 0;
    }
    FT_Open_Args arguments{};
    arguments.flags = FT_OPEN_STREAM;
    arguments.stream = &stream;
    FT_Long faces = 1;
    for (FT_Long index = 0; index < faces; ++index) {
        FT_Face face = nullptr;
        if (FT_Open_Face(started, &arguments, index, &face) != 0) {
            break;
        }
        faces = face->num_faces;
        if (face->family_name != nullptr && FT_IS_SCALABLE(face)) {
            listed.push_back({
                .path = path,
                .index = static_cast<std::int32_t>(index),
                .family = face->family_name,
                .style = face->style_name != nullptr ? face->style_name : "",
                .weight = weight_of(face),
                .italic = italic_of(face),
            });
        }
        FT_Done_Face(face);
    }
    std::fclose(file);
}

/// Lists the font files in `folder` and the folders in it.
void list_folder(FT_Library started, const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator walk(
        folder, std::filesystem::directory_options::skip_permission_denied, error);
    for (; !error && walk != std::filesystem::recursive_directory_iterator();
         walk.increment(error)) {
        if (walk->is_regular_file(error) && font_file(walk->path())) {
            list_file(started, walk->path());
        }
    }
}

/// Lists the OS's folders, once, after the ones added.
bool list() {
    if (listed_os) {
        return true;
    }
    FT_Library started = library();
    if (started == nullptr) {
        return false;
    }
    for (const std::filesystem::path& folder : os_folders()) {
        list_folder(started, folder);
    }
    listed_os = true;
    return true;
}

/// The listed face of `family`, or a family and its style, as "Segoe UI Semibold", nearest
/// `style`, or null.
const listed_t* nearest(std::string_view family, std::uint32_t style) {
    const std::int32_t weight = (style & bold_bit) != 0 ? 700 : 400;
    const bool italic = (style & italic_bit) != 0;
    const listed_t* best = nullptr;
    std::int32_t distance = 0;
    for (const bool whole : { false, true }) {
        for (const listed_t& each : listed) {
            const bool named =
                whole ? same_name(std::format("{} {}", each.family, each.style), family)
                      : same_name(each.family, family);
            if (!named) {
                continue;
            }
            const std::int32_t off =
                std::abs(each.weight - weight) + (each.italic != italic ? 1000 : 0);
            if (best == nullptr || off < distance) {
                best = &each;
                distance = off;
            }
        }
        if (best != nullptr) {
            return best;
        }
    }
    return nullptr;
}

} // namespace

std::shared_ptr<face_t> system_face(std::string_view family, std::uint32_t style) {
    if (!list()) {
        return nullptr;
    }
    const listed_t* found = nearest(family, style);
    if (found == nullptr) {
        fbe::fail(std::format("there's no font of the family {}", family));
        return nullptr;
    }
    return file_face(found->path, found->index);
}

std::shared_ptr<face_t> system_default(std::uint32_t style) {
    if (!list()) {
        return nullptr;
    }
    for (const char* family : default_families) {
        if (const listed_t* found = nearest(family, style)) {
            return file_face(found->path, found->index);
        }
    }
    if (listed.empty()) {
        fbe::fail("there are no fonts");
        return nullptr;
    }
    return file_face(listed.front().path, listed.front().index);
}

std::vector<std::string> system_families() {
    std::vector<std::string> found;
    if (!list()) {
        return found;
    }
    for (const listed_t& each : listed) {
        if (std::ranges::none_of(
                found, [&](const std::string& had) { return same_name(had, each.family); })) {
            found.push_back(each.family);
        }
    }
    std::ranges::sort(found);
    return found;
}

bool add_folder(const std::filesystem::path& folder) {
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) {
        return fbe::fail("there's no such folder of fonts");
    }
    FT_Library started = library();
    if (started == nullptr) {
        return false;
    }
    if (std::ranges::find(added, folder) != added.end()) {
        return true;
    }
    added.push_back(folder);
    // Its faces come first, so of two as near, its are taken.
    std::vector<listed_t> had = std::move(listed);
    listed.clear();
    list_folder(started, folder);
    listed.insert(listed.end(), had.begin(), had.end());
    return true;
}

std::size_t system_fallback_count() {
    return fallback_families.size();
}

std::shared_ptr<face_t> system_fallback(std::size_t index) {
    if (index >= fallback_families.size()) {
        return nullptr;
    }
    if (fallbacks.size() < fallback_families.size()) {
        fallbacks.resize(fallback_families.size());
        tried.resize(fallback_families.size());
    }
    if (!tried[index]) {
        tried[index] = true;
        if (list()) {
            if (const listed_t* found = nearest(fallback_families[index], 0U)) {
                fallbacks[index] = file_face(found->path, found->index);
            }
        }
    }
    return fallbacks[index];
}

} // namespace fbe::text

extern "C" {

FBE_API int32_t fbe_text_system_family_count(void) {
    fbe::text::families = fbe::text::system_families();
    return static_cast<std::int32_t>(fbe::text::families.size());
}

FBE_API const char* fbe_text_system_family_at(int32_t index) {
    const bool there = index >= 0 && static_cast<std::size_t>(index) < fbe::text::families.size();
    return there ? fbe::text::families[static_cast<std::size_t>(index)].c_str() : "";
}

FBE_API bool fbe_text_add_font_folder(const char* folder) {
    if (folder == nullptr || *folder == '\0') {
        return fbe::fail("there's no folder of fonts to add");
    }
    return fbe::text::add_folder(fbe::text::native_path(folder));
}

} // extern "C"
