// UTF-8 to and from what the OS's paths are: UTF-16 on Windows, and bytes, taken as UTF-8,
// elsewhere; files read at an offset, by ReadFile on Windows and pread elsewhere, which take no
// file pointer, so threads read one file at once; and folders listed by FindFirstFileEx on
// Windows, whose entries come with their attributes, and readdir's d_type elsewhere.

#include "vfs/disk.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <system_error>

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
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fbe::vfs {

namespace {

constexpr char32_t replacement = 0xFFFD;

// What a link links to, a file or a folder, or NONE.
kind_t followed(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(path, error);
    if (error) {
        return kind_t::NONE;
    }
    if (std::filesystem::is_directory(status)) {
        return kind_t::FOLDER;
    }
    return std::filesystem::is_regular_file(status) ? kind_t::FILE : kind_t::NONE;
}

// Calls `visit` with each code point of UTF-8, with U+FFFD for each byte that doesn't start a
// well-formed one.
template <typename Visit>
void each_point(std::string_view text, Visit visit) {
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
        visit(good ? point : replacement);
        at += good ? length : 1;
    }
}

void encode(std::string& text, char32_t point) {
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

} // namespace

std::string well_formed(std::string_view bytes) {
    std::string text;
    text.reserve(well_formed_size(bytes));
    each_point(bytes, [&](char32_t point) { encode(text, point); });
    return text;
}

std::filesystem::path real_path(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return path;
    }
    const DWORD needed = GetFinalPathNameByHandleW(file, nullptr, 0, VOLUME_NAME_DOS);
    std::wstring found(needed, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(file, found.data(), needed, VOLUME_NAME_DOS);
    CloseHandle(file);
    if (needed == 0 || written == 0 || written >= needed) {
        return path;
    }
    found.resize(written);
    return found;
#else
    std::error_code error;
    std::filesystem::path found = std::filesystem::canonical(path, error);
    return error ? path : found;
#endif
}

std::uint64_t well_formed_size(std::string_view bytes) {
    std::uint64_t size = 0;
    each_point(bytes, [&](char32_t point) {
        size += 1U + (point >= 0x80 ? 1U : 0U) + (point >= 0x800 ? 1U : 0U) +
                (point >= 0x1'0000 ? 1U : 0U);
    });
    return size;
}

#ifdef _WIN32

std::filesystem::path disk_path(std::string_view utf8) {
    std::wstring wide;
    each_point(utf8, [&](char32_t point) {
        if (point < 0x1'0000) {
            wide += static_cast<wchar_t>(point);
        } else {
            wide += static_cast<wchar_t>(0xD800U + ((point - 0x1'0000U) >> 10U));
            wide += static_cast<wchar_t>(0xDC00U + ((point - 0x1'0000U) & 0x3FFU));
        }
    });
    return { wide };
}

std::string utf8(const std::filesystem::path& path) {
    const std::wstring& wide = path.native();
    std::string text;
    for (std::size_t i = 0; i < wide.size(); ++i) {
        const auto unit = static_cast<char32_t>(wide[i]);
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < wide.size() &&
            static_cast<char32_t>(wide[i + 1]) >= 0xDC00 &&
            static_cast<char32_t>(wide[i + 1]) <= 0xDFFF) {
            encode(text, 0x1'0000U + ((unit - 0xD800U) << 10U) +
                             (static_cast<char32_t>(wide[i + 1]) - 0xDC00U));
            ++i;
        } else {
            encode(text, unit >= 0xD800 && unit <= 0xDFFF ? replacement : unit);
        }
    }
    return text;
}

std::optional<folder_time_t> folder_time(const std::filesystem::path& path) {
    // FILETIMEs, in 100 ns ticks, which a disk keeps in steps of up to 2 s (FAT's); a folder's is
    // when an entry was last added to it, taken out, or renamed. A junction's or a link's is its
    // target's, as its listing is, which opening it, rather than reading its attributes, gives.
    constexpr std::uint64_t settling = std::uint64_t{ 2000 } * 10'000;
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION data{};
    const bool read = GetFileInformationByHandle(handle, &data) != 0;
    CloseHandle(handle);
    if (!read || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return std::nullopt;
    }
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
    };
    const std::uint64_t written = ticks(data.ftLastWriteTime);
    return folder_time_t{ .written = written, .settled = ticks(now) > written + settling };
}

std::optional<std::vector<listed_t>> list_folder(const std::filesystem::path& path) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((path / L"*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    std::vector<listed_t> listed;
    do {
        const std::wstring_view name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        std::filesystem::path entry = path / name;
        kind_t kind =
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? kind_t::FOLDER : kind_t::FILE;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            kind = followed(entry);
        }
        if (kind != kind_t::NONE) {
            std::string text = utf8(entry.filename());
            listed.push_back({ .path = std::move(entry), .name = std::move(text), .kind = kind });
        }
    } while (FindNextFileW(find, &data) != 0);
    const bool whole = GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(find);
    if (!whole) {
        return std::nullopt;
    }
    return listed;
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

std::optional<folder_time_t> folder_time(const std::filesystem::path& path) {
    // Nanoseconds, which a disk keeps in steps of up to 2 s (HFS+'s and ext3's are whole seconds,
    // FAT's two); a folder's is when an entry was last added to it, taken out, or renamed. stat
    // follows a link, so a linked folder's is its target's, as its listing is.
    constexpr std::uint64_t settling = 2'000'000'000;
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    struct stat status{};
    if (stat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)) {
        return std::nullopt;
    }
#ifdef __APPLE__
    const timespec& modified = status.st_mtimespec;
#else
    const timespec& modified = status.st_mtim;
#endif
    const auto nanoseconds = [](const timespec& time) {
        return (static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000) +
               static_cast<std::uint64_t>(time.tv_nsec);
    };
    const std::uint64_t written = nanoseconds(modified);
    return folder_time_t{ .written = written, .settled = nanoseconds(now) > written + settling };
}

std::optional<std::vector<listed_t>> list_folder(const std::filesystem::path& path) {
    // libstdc++'s and libc++'s entries keep readdir's d_type, so only a link is stat'd.
    std::vector<listed_t> listed;
    std::error_code error;
    std::filesystem::directory_iterator it(path, error);
    for (; !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
        kind_t kind = kind_t::NONE;
        std::error_code type_error;
        if (it->is_symlink(type_error)) {
            kind = followed(it->path());
        } else if (it->is_directory(type_error)) {
            kind = kind_t::FOLDER;
        } else if (it->is_regular_file(type_error)) {
            kind = kind_t::FILE;
        }
        if (kind != kind_t::NONE) {
            listed.push_back(
                { .path = it->path(), .name = utf8(it->path().filename()), .kind = kind });
        }
    }
    if (error) {
        return std::nullopt;
    }
    return listed;
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
