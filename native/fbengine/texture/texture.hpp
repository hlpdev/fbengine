#pragma once

// What draws with textures reads of them: the texture a handle is for, its index in the bindless
// set, and its sampler's.

#include "gpu/vulkan.hpp"

#include <cstdint>
#include <vector>

namespace fbe::texture {

/// Handles' kind for textures, in fbengine's range (architecture.md's handles).
constexpr std::uint8_t kind = 5;

/// A texture: its image on the GPU, a view of every level, and how it's sampled. Its slot is its
/// index in the bindless set.
struct texture_t {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::int32_t format = 0;
    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 1;
    std::int32_t filter = 0;
    std::int32_t wrap_u = 0;
    std::int32_t wrap_v = 0;
    std::uint32_t references = 0;
};

/// The live texture a handle is for, or nullptr, with fbe_error's why.
texture_t* find(std::uint64_t handle);

/// A live texture's index in the bindless set, which is its slot, and its sampler's; the white
/// texture, index 0, is what a draw with no texture reads.
std::uint32_t index_of(std::uint64_t handle);
std::uint32_t sampler_of(const texture_t& texture);
constexpr std::uint32_t white = 0;

/// The bytes of a level of a texture, read back from the GPU after what the frame recorded so far,
/// its rows packed; empty, with fbe_error's why, if they can't be.
std::vector<std::uint8_t> read(std::uint64_t handle, std::uint32_t level);

/// How many bytes a pixel of an FBE_TEXTURE_ format has; 0 for none.
std::uint32_t pixel_size(std::int32_t format);

} // namespace fbe::texture
