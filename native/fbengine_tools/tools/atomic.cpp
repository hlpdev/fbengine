// A file written whole or not at all, by what the OS itself has, as std's atomic_file_t in
// FutureBlitz's runtime: an exclusive create, a write at an offset, a flush, and a rename that
// replaces a file another handle holds open.

#include "tools/atomic.hpp"

#include "vfs/disk.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fbe::tools {

namespace {

namespace fs = std::filesystem;

// The name of a temporary file: hidden, so a cook of its folder leaves it out, mixed from the
// process, a count, and the clock.
fs::path::string_type temporary_name() {
    static std::atomic<std::uint64_t> made = 0;
#ifdef _WIN32
    const std::uint64_t process = GetCurrentProcessId();
#else
    const auto process = static_cast<std::uint64_t>(getpid());
#endif
    // SplitMix64's mix, which spreads every bit of each part over the whole.
    auto mixed =
        (process << 32U) ^
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
        (made.fetch_add(1, std::memory_order_relaxed) * 0x9E37'79B9'7F4A'7C15U);
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9U;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EBU;
    mixed ^= mixed >> 31U;
    constexpr std::string_view digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    fs::path::string_type name;
    for (const char c : std::string_view(".fbe-")) {
        name.push_back(static_cast<fs::path::value_type>(c));
    }
    for (int i = 0; i < 12; ++i) {
        name.push_back(static_cast<fs::path::value_type>(digits[mixed % digits.size()]));
        mixed /= digits.size();
    }
    for (const char c : std::string_view(".tmp")) {
        name.push_back(static_cast<fs::path::value_type>(c));
    }
    return name;
}

#ifdef _WIN32
// What REPARSE_DATA_BUFFER's symbolic link part is, which MinGW's headers don't have outside the
// driver kit's: the names go on past its first character, in the buffer the struct starts.
struct symlink_data_t {
    ULONG tag;
    USHORT length;
    USHORT reserved;
    USHORT substitute_offset;
    USHORT substitute_length;
    USHORT print_offset;
    USHORT print_length;
    ULONG flags;
    std::array<WCHAR, 1> names;
};

constexpr ULONG symlink_is_relative = 0x1;

// Where the symbolic link at `path` leads, though nothing's there; nothing for what isn't one.
std::optional<fs::path> link_target(const fs::path& path) {
    HANDLE file = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> buffer{};
    DWORD returned = 0;
    const BOOL read = DeviceIoControl(file, FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.data(),
                                      static_cast<DWORD>(buffer.size()), &returned, nullptr);
    CloseHandle(file);
    const auto* data = reinterpret_cast<const symlink_data_t*>(buffer.data());
    if (read == 0 || returned < sizeof(symlink_data_t) || data->tag != IO_REPARSE_TAG_SYMLINK) {
        return std::nullopt;
    }
    const std::wstring target(reinterpret_cast<const wchar_t*>(buffer.data() +
                                                               offsetof(symlink_data_t, names) +
                                                               data->print_offset),
                              data->print_length / sizeof(wchar_t));
    const fs::path found(target);
    return (data->flags & symlink_is_relative) != 0 ? path.parent_path() / found : found;
}

// The file a link at `path` leads to, through each link after it, which needn't be there yet, or
// `path` when it isn't a link.
fs::path linked_file(fs::path path) {
    // As many links as Linux follows before it says there's a loop.
    for (int links = 0; links < 40; ++links) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
            break;
        }
        std::optional<fs::path> target = link_target(path);
        if (!target) {
            break;
        }
        path = std::move(*target);
    }
    return path;
}

// FILE_RENAME_INFO as Windows 10 1709's SDK has it, with Flags in place of ReplaceIfExists, and
// FileRenameInfoEx's flags, which older MinGW headers lack. The name goes on past its first
// character, in the buffer the struct starts.
struct rename_info_t {
    DWORD flags;
    HANDLE root;
    DWORD length;
    std::array<WCHAR, 1> name;
};

constexpr DWORD rename_replace_if_exists = 0x1;
constexpr DWORD rename_posix_semantics = 0x2;
constexpr auto file_rename_info_ex = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);

// Renames `from` over `to` with POSIX semantics, which replaces a file another handle holds open,
// sharing delete, and which goes on reading the old one; where Windows or the disk doesn't have
// them (before Windows 10 1709, or FAT), MoveFileExW's, which can't.
bool replace_file(const fs::path& from, const fs::path& to) {
    const auto moved = [&] {
        return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    };
    const DWORD needed = GetFullPathNameW(to.c_str(), 0, nullptr, nullptr);
    std::wstring full(needed, L'\0');
    const DWORD written = GetFullPathNameW(to.c_str(), needed, full.data(), nullptr);
    if (needed == 0 || written == 0 || written >= needed) {
        return moved();
    }
    full.resize(written);
    HANDLE file = CreateFileW(from.c_str(), DELETE | SYNCHRONIZE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return moved();
    }
    const std::size_t size = offsetof(rename_info_t, name) + ((full.size() + 1) * sizeof(WCHAR));
    std::vector<std::byte> buffer(size);
    auto* info = reinterpret_cast<rename_info_t*>(buffer.data());
    info->flags = rename_replace_if_exists | rename_posix_semantics;
    info->root = nullptr;
    info->length = static_cast<DWORD>(full.size() * sizeof(WCHAR));
    std::memcpy(static_cast<void*>(info->name.data()), full.c_str(),
                (full.size() + 1) * sizeof(WCHAR));
    const BOOL renamed =
        SetFileInformationByHandle(file, file_rename_info_ex, info, static_cast<DWORD>(size));
    const DWORD error = renamed != 0 ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (renamed != 0) {
        return true;
    }
    // A Windows or a disk without them says it doesn't know the class or its flags.
    return (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
            error == ERROR_INVALID_FUNCTION) &&
           moved();
}
#else
// The file a link at `path` leads to, through each link after it, which needn't be there yet, or
// `path` when it isn't a link.
fs::path linked_file(fs::path path) {
    // As many links as Linux follows before it says there's a loop.
    for (int links = 0; links < 40; ++links) {
        std::error_code error;
        if (!fs::is_symlink(fs::symlink_status(path, error))) {
            break;
        }
        const fs::path target = fs::read_symlink(path, error);
        if (error) {
            break;
        }
        path = target.is_absolute() ? target : path.parent_path() / target;
    }
    return path;
}
#endif

} // namespace

atomic_file_t::~atomic_file_t() {
    discard();
}

std::expected<void, std::string> atomic_file_t::open(std::string_view path) {
    discard();
    m_shown = std::string(path);
    const auto unwritable = [&] { return std::format("couldn't write {}", m_shown); };
    m_file = linked_file(vfs::disk_path(path));
    std::error_code error;
    // A rename over a folder fails differently on each OS, and only after the whole file.
    if (fs::is_directory(m_file, error)) {
        return std::unexpected(std::format("couldn't write {}: it's a folder", m_shown));
    }
    if (m_file.has_parent_path()) {
        fs::create_directories(m_file.parent_path(), error);
    }
    for (int tries = 0; tries < 100; ++tries) {
        m_temporary = m_file.parent_path() / fs::path(temporary_name());
#ifdef _WIN32
        HANDLE file = CreateFileW(m_temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            m_handle = file;
            return {};
        }
        const bool taken = GetLastError() == ERROR_FILE_EXISTS;
#else
        const int file = ::open(m_temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (file >= 0) {
            m_handle = file;
            return {};
        }
        const bool taken = errno == EEXIST;
#endif
        if (!taken) {
            break;
        }
    }
    m_temporary.clear();
    return std::unexpected(unwritable());
}

bool atomic_file_t::write_at(std::uint64_t offset, std::span<const std::uint8_t> bytes) const {
    std::size_t done = 0;
    while (done < bytes.size()) {
#ifdef _WIN32
        constexpr std::size_t most = 0x4000'0000;
        const std::uint64_t at = offset + done;
        OVERLAPPED where{};
        where.Offset = static_cast<DWORD>(at);
        where.OffsetHigh = static_cast<DWORD>(at >> 32U);
        DWORD wrote = 0;
        if (WriteFile(m_handle, bytes.data() + done,
                      static_cast<DWORD>(std::min(bytes.size() - done, most)), &wrote,
                      &where) == 0 ||
            wrote == 0) {
            return false;
        }
#else
        const ssize_t wrote = pwrite(m_handle, bytes.data() + done, bytes.size() - done,
                                     static_cast<off_t>(offset + done));
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            return false;
        }
#endif
        done += static_cast<std::size_t>(wrote);
    }
    return true;
}

std::expected<void, std::string> atomic_file_t::commit() {
    bool done = false;
#ifdef _WIN32
    done = FlushFileBuffers(m_handle) != 0;
    close();
    done = done && replace_file(m_temporary, m_file);
#else
    // The mode of what it replaces, which a new file's umask wouldn't give.
    if (struct stat replaced{}; ::stat(m_file.c_str(), &replaced) == 0) {
        fchmod(m_handle, replaced.st_mode & 07777U);
    }
#ifdef __APPLE__
    // macOS's fsync doesn't reach the disk's platters, and not every disk takes the full one.
    done = fcntl(m_handle, F_FULLFSYNC) == 0 || fcntl(m_handle, F_BARRIERFSYNC) == 0 ||
           fsync(m_handle) == 0;
#else
    done = fsync(m_handle) == 0;
#endif
    close();
    done = done && std::rename(m_temporary.c_str(), m_file.c_str()) == 0;
#endif
    if (!done) {
        discard();
        return std::unexpected(std::format("couldn't write {}", m_shown));
    }
    m_temporary.clear();
    return {};
}

void atomic_file_t::close() {
#ifdef _WIN32
    if (m_handle != nullptr) {
        CloseHandle(m_handle);
        m_handle = nullptr;
    }
#else
    if (m_handle >= 0) {
        ::close(m_handle);
        m_handle = -1;
    }
#endif
}

void atomic_file_t::discard() {
    close();
    if (!m_temporary.empty()) {
        std::error_code error;
        fs::remove(m_temporary, error);
        m_temporary.clear();
    }
}

} // namespace fbe::tools
