#pragma once

// What fbengine_tools' own files share: its errors, and its files on the disk.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fbe::tools {

using bytes_t = std::vector<std::uint8_t>;

/// Keeps `message` for fbe_tools_error(), and returns false, for a call that failed.
bool fail(std::string_view message);

/// The path on the disk that `utf8` names.
std::filesystem::path disk_path(std::string_view utf8);

/// The bytes of the file at `path`, a UTF-8 path on the disk, or why it can't be read.
std::expected<bytes_t, std::string> read_file(std::string_view path);

/// Writes `bytes` to the file at `path`, a UTF-8 path on the disk, making its folder, and gives
/// why it can't, if it can't.
std::expected<void, std::string> write_file(std::string_view path,
                                            std::span<const std::uint8_t> bytes);

} // namespace fbe::tools
