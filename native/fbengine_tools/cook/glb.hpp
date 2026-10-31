#pragma once

// Writing a binary glTF, by fastgltf's exporter, in a file of its own: the exporter's
// std::filesystem paths need libstdc++'s codecvt, which MinGW's static libstdc++ has only for
// msvcrt's mbstate_t, so that file is built as msvcrt's, as fastgltf's own code is.

#include "tools/tools.hpp"

#include <expected>
#include <string>

namespace fastgltf {
// fastgltf's own name.
// NOLINTNEXTLINE(readability-identifier-naming)
class Asset;
} // namespace fastgltf

namespace fbe::tools {

/// `asset` as a binary glTF's bytes, its first buffer its binary chunk, or why it can't be.
std::expected<bytes_t, std::string> write_glb(const fastgltf::Asset& asset);

} // namespace fbe::tools
