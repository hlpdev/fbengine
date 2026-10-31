#pragma once

// Paths on the disk from UTF-8 and back. std::filesystem's own conversions need libstdc++'s
// codecvt, whose names MinGW's UCRT build of the DLL can't link, so Windows' UTF-16 is made here.

#include <filesystem>
#include <string>
#include <string_view>

namespace fbe::vfs {

/// The path on the disk that `utf8` names.
std::filesystem::path disk_path(std::string_view utf8);

/// `path` in UTF-8, with U+FFFD for what Windows' UTF-16 has that isn't Unicode.
std::string utf8(const std::filesystem::path& path);

} // namespace fbe::vfs
