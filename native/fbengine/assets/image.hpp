#pragma once

// Images decoded to their texels on the CPU, as a texture takes them: PNG and JPEG by stb_image,
// KTX2 by libktx, its Basis Universal ones transcoded to what the GPU reads. Any thread decodes.

#include "vfs/vfs.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace fbe::assets {

using vfs::bytes_t;
using vfs::error_t;
using vfs::failure_t;
using vfs::result_t;

/// What a file's bytes say it is, whatever its name says.
enum class file_kind_t : std::uint8_t { UNKNOWN, PNG, JPEG, KTX2, GLB, GLTF };

/// What `bytes` start as: a PNG's, a JPEG's, or a KTX2's signature, a binary glTF's, or the JSON
/// object a glTF is.
file_kind_t kind_of(std::span<const std::uint8_t> bytes);

/// Vulkan's numbers for the formats PNGs and JPEGs decode to, and those Basis Universal's KTX2
/// files transcode to; a KTX2 that isn't Basis Universal's keeps its own.
namespace formats {
constexpr std::uint32_t rgba8 = 37;
constexpr std::uint32_t rgba8_srgb = 43;
constexpr std::uint32_t rgba16 = 91;
constexpr std::uint32_t bc7 = 145;
constexpr std::uint32_t bc7_srgb = 146;
constexpr std::uint32_t astc_4x4 = 157;
constexpr std::uint32_t astc_4x4_srgb = 158;
} // namespace formats

/// What a Basis Universal KTX2 is transcoded to: BC7 for desktop GPUs, ASTC 4x4 for Apple's and
/// phones', and RGBA8 for what has neither.
enum class transcode_t : std::uint8_t { RGBA8, BC7, ASTC_4X4 };

struct image_options_t {
    /// Whether a PNG's or JPEG's 8-bit color is sRGB, as a color texture's is, or linear, as a
    /// normal map's is. A KTX2 says so itself.
    bool srgb = true;
    transcode_t transcode = transcode_t::RGBA8;
};

/// A mip level: its size in texels, and where its images are in the image's texels, one a layer
/// and face, layer by layer, the faces of each in order, `size` bytes each.
struct level_t {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    std::size_t offset = 0;
    std::size_t size = 0;
};

/// An image's texels and how they're laid out.
struct image_t {
    /// Its Vulkan format.
    std::uint32_t format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    std::uint32_t layers = 1;
    /// 6 for a cube map's, +X, -X, +Y, -Y, +Z, -Z, and 1 for the rest.
    std::uint32_t faces = 1;
    /// The largest first.
    std::vector<level_t> levels;
    bytes_t texels;
};

/// A PNG, a JPEG, or a KTX2 decoded, by what its bytes say it is; INVALID_DATA for another kind of
/// file, or one that's broken. `name` is what errors call it.
result_t<image_t> decode_image(std::span<const std::uint8_t> bytes, const image_options_t& options,
                               std::string_view name);

} // namespace fbe::assets
