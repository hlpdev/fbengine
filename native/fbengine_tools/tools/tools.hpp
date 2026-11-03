#pragma once

// What fbengine_tools' own files share: its errors, and its files on the disk.

#include "vfs/vfs.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbe::tools {

using bytes_t = std::vector<std::uint8_t>;
using vfs::error_t;
using vfs::failure_t;

/// Keeps `error` for fbe_tools_error() and fbe_tools_failure(), and returns false, for a call that
/// failed; a message alone is OTHER's.
bool fail(const error_t& error);
bool fail(std::string_view message);

/// An error of what a tool was given that isn't what it takes, INVALID_DATA, and of the rest.
inline error_t invalid(std::string message) {
    return { .failure = failure_t::INVALID_DATA, .message = std::move(message) };
}

inline error_t other(std::string message) {
    return { .failure = failure_t::OTHER, .message = std::move(message) };
}

/// The path on the disk that `utf8` names.
std::filesystem::path disk_path(std::string_view utf8);

/// The bytes of the file at `path`, a UTF-8 path on the disk, or why it can't be read.
std::expected<bytes_t, std::string> read_file(std::string_view path);

/// Writes `bytes` to the file at `path`, a UTF-8 path on the disk, as an atomic_file_t does. It
/// gives why it can't, if it can't.
std::expected<void, std::string> write_file(std::string_view path,
                                            std::span<const std::uint8_t> bytes);

} // namespace fbe::tools
