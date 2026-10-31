// UTF-8 to and from what the OS's paths are: UTF-16 on Windows, and bytes, taken as UTF-8,
// elsewhere.

#include "vfs/disk.hpp"

#include <cstdint>

namespace fbe::vfs {

namespace {

[[maybe_unused]] constexpr char32_t replacement = 0xFFFD;

// The code points of UTF-8, with U+FFFD for each byte that doesn't start a well-formed one.
[[maybe_unused]] std::u32string decoded(std::string_view text) {
    std::u32string points;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto lead = static_cast<std::uint8_t>(text[at]);
        std::size_t length = 0;
        char32_t point = 0;
        char32_t least = 0;
        if (lead < 0x80) {
            length = 1;
            point = lead;
        } else if ((lead & 0xE0U) == 0xC0) {
            length = 2;
            point = lead & 0x1FU;
            least = 0x80;
        } else if ((lead & 0xF0U) == 0xE0) {
            length = 3;
            point = lead & 0x0FU;
            least = 0x800;
        } else if ((lead & 0xF8U) == 0xF0) {
            length = 4;
            point = lead & 0x07U;
            least = 0x1'0000;
        }
        bool good = length > 0 && at + length <= text.size();
        for (std::size_t i = 1; good && i < length; ++i) {
            const auto next = static_cast<std::uint8_t>(text[at + i]);
            good = (next & 0xC0U) == 0x80;
            point = (point << 6U) | (next & 0x3FU);
        }
        good = good && point >= least && point <= 0x10'FFFF && (point < 0xD800 || point > 0xDFFF);
        points.push_back(good ? point : replacement);
        at += good ? length : 1;
    }
    return points;
}

[[maybe_unused]] std::string encoded(std::u32string_view points) {
    std::string text;
    for (const char32_t point : points) {
        if (point < 0x80) {
            text += static_cast<char>(point);
        } else if (point < 0x800) {
            text += static_cast<char>(0xC0U | (point >> 6U));
            text += static_cast<char>(0x80U | (point & 0x3FU));
        } else if (point < 0x1'0000) {
            text += static_cast<char>(0xE0U | (point >> 12U));
            text += static_cast<char>(0x80U | ((point >> 6U) & 0x3FU));
            text += static_cast<char>(0x80U | (point & 0x3FU));
        } else {
            text += static_cast<char>(0xF0U | (point >> 18U));
            text += static_cast<char>(0x80U | ((point >> 12U) & 0x3FU));
            text += static_cast<char>(0x80U | ((point >> 6U) & 0x3FU));
            text += static_cast<char>(0x80U | (point & 0x3FU));
        }
    }
    return text;
}

} // namespace

#ifdef _WIN32

std::filesystem::path disk_path(std::string_view utf8) {
    std::wstring wide;
    for (const char32_t point : decoded(utf8)) {
        if (point < 0x1'0000) {
            wide += static_cast<wchar_t>(point);
        } else {
            wide += static_cast<wchar_t>(0xD800U + ((point - 0x1'0000U) >> 10U));
            wide += static_cast<wchar_t>(0xDC00U + ((point - 0x1'0000U) & 0x3FFU));
        }
    }
    return { wide };
}

std::string utf8(const std::filesystem::path& path) {
    const std::wstring& wide = path.native();
    std::u32string points;
    for (std::size_t i = 0; i < wide.size(); ++i) {
        const auto unit = static_cast<char32_t>(wide[i]);
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < wide.size() &&
            static_cast<char32_t>(wide[i + 1]) >= 0xDC00 &&
            static_cast<char32_t>(wide[i + 1]) <= 0xDFFF) {
            points.push_back(0x1'0000U + ((unit - 0xD800U) << 10U) +
                             (static_cast<char32_t>(wide[i + 1]) - 0xDC00U));
            ++i;
        } else {
            points.push_back(unit >= 0xD800 && unit <= 0xDFFF ? replacement : unit);
        }
    }
    return encoded(points);
}

#else

std::filesystem::path disk_path(std::string_view utf8) {
    return { std::string(utf8) };
}

std::string utf8(const std::filesystem::path& path) {
    return path.native();
}

#endif

} // namespace fbe::vfs
