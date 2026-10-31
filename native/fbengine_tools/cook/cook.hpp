#pragma once

// The cooker's C++, in memory: what fbe_cook_* do between reading their files and writing them.

#include "cook/fbe_cook.h"
#include "tools/tools.hpp"
#include "vfs/vfs.hpp"

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
/// 2x2 under it, and of the row or column past it at an odd edge, in linear light when `srgb`, its
/// color weighted by its alpha, so what's clear doesn't tint what isn't, and its alpha as it is.
rgba_t half(const rgba_t& image, bool srgb);

/// A PNG or a JPEG cooked into a KTX2's bytes, as `options` says; INVALID_DATA for one that won't
/// decode, is bigger than the engine loads, or whose quality isn't 0 to 4.
std::expected<bytes_t, error_t> cook_image(std::span<const std::uint8_t> bytes,
                                           const fbe_cook_image_t& options);

/// The glTF at `path`, whose `bytes` are read, with what it names read from `files`, cooked into
/// a binary glTF's bytes, as `options` says; what the engine wouldn't load isn't cooked, and
/// says why.
std::expected<bytes_t, error_t> cook_model(std::span<const std::uint8_t> bytes,
                                           const vfs::path_t& path, const vfs::view_t& files,
                                           const fbe_cook_model_t& options);

} // namespace fbe::tools
