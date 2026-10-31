#pragma once

// The cooker's C++, in memory: what fbe_cook_* do between reading their files and writing them.

#include "cook/fbe_cook.h"
#include "tools/tools.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace fbe::tools {

/// An image's RGBA8 texels, `width` by `height`.
struct rgba_t {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bytes_t texels;
};

/// The next mip of `image`: half its size, rounded down, at least 1, each texel the mean of the
/// 2x2 under it, and of the row or column past it at an odd edge, in linear light when `srgb`,
/// its alpha as it is.
rgba_t half(const rgba_t& image, bool srgb);

/// A PNG or a JPEG cooked into a KTX2's bytes, as `options` says.
std::expected<bytes_t, std::string> cook_image(std::span<const std::uint8_t> bytes,
                                               const fbe_cook_image_t& options);

} // namespace fbe::tools
