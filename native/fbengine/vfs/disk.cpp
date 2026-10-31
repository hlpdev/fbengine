// UTF-8 to and from what the OS's paths are: UTF-16 on Windows, and bytes, taken as UTF-8,
// elsewhere; and files read at an offset, by ReadFile on Windows and pread elsewhere, which take
// no file pointer, so threads read one file at once.

#include "vfs/disk.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fbe::vfs {

namespace {

constexpr char32_t replacement = 0xFFFD;

// The code points of UTF-8, with U+FFFD for each byte that doesn't start a well-formed one.
std::u32string decoded(std::string_view text) {
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

std::string encoded(std::u32string_view points) {
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

std::string well_formed(std::string_view bytes) {
    return encoded(decoded(bytes));
}

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

std::unique_ptr<read_file_t> read_file_t::open(const std::filesystem::path& path, bool& missing) {
    // Others may read, write, rename, and remove it meanwhile, as they can on Linux and macOS.
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        return nullptr;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle, &size) == 0) {
        CloseHandle(handle);
        missing = false;
        return nullptr;
    }
    return std::unique_ptr<read_file_t>(
        new read_file_t(handle, static_cast<std::uint64_t>(size.QuadPart)));
}

read_file_t::~read_file_t() {
    CloseHandle(m_handle);
}

bool read_file_t::read_at(std::uint64_t offset, std::span<std::uint8_t> into) const {
    std::size_t done = 0;
    while (done < into.size()) {
        // A read at an OVERLAPPED's offset of a file opened for synchronous reads takes no file
        // pointer, and the system lets one at a time through.
        OVERLAPPED at{};
        const std::uint64_t from = offset + done;
        at.Offset = static_cast<DWORD>(from);
        at.OffsetHigh = static_cast<DWORD>(from >> 32U);
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(into.size() - done, std::numeric_limits<DWORD>::max() / 2));
        DWORD got = 0;
        if (ReadFile(m_handle, into.data() + done, chunk, &got, &at) == 0 || got == 0) {
            return false;
        }
        done += got;
    }
    return true;
}

#else

std::filesystem::path disk_path(std::string_view utf8) {
    return { std::string(utf8) };
}

std::string utf8(const std::filesystem::path& path) {
    return path.native();
}

std::unique_ptr<read_file_t> read_file_t::open(const std::filesystem::path& path, bool& missing) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        missing = errno == ENOENT || errno == ENOTDIR;
        return nullptr;
    }
    struct stat status{};
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        close(descriptor);
        missing = false;
        return nullptr;
    }
    return std::unique_ptr<read_file_t>(
        new read_file_t(descriptor, static_cast<std::uint64_t>(status.st_size)));
}

read_file_t::~read_file_t() {
    close(m_handle);
}

bool read_file_t::read_at(std::uint64_t offset, std::span<std::uint8_t> into) const {
    std::size_t done = 0;
    while (done < into.size()) {
        const ssize_t got = pread(m_handle, into.data() + done, into.size() - done,
                                  static_cast<off_t>(offset + done));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(got);
    }
    return true;
}

#endif

} // namespace fbe::vfs
