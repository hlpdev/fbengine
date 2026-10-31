#pragma once

// What draws with textures reads of them: the texture a handle is for, its index in the bindless
// set, and its sampler's; and what draws into them: a target's passes, clears, and copies.

#include "gpu/vulkan.hpp"

#include <array>
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
    /// Whether it's drawn into, a target, and the samples a pass draws, which a multisampled
    /// target's resolve into its image at the pass's end.
    bool target = false;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImage msaa = VK_NULL_HANDLE;
    VmaAllocation msaa_memory = VK_NULL_HANDLE;
    VkImageView msaa_view = VK_NULL_HANDLE;
    /// A target's depth, if it has one: 32-bit floats, reversed, 1 nearest and 0 farthest, as
    /// many samples as it draws.
    VkImage depth = VK_NULL_HANDLE;
    VmaAllocation depth_memory = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
};

/// A target's depth's format.
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;

/// What a pass clears its target to as it begins: its color, linear, and its depth, 0, the
/// farthest, unless it keeps them.
struct clear_t {
    bool color = false;
    std::array<float, 4> rgba{};
    bool depth = false;
    float depth_value = 0.0F;
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

/// Begins drawing into a target in the frame's commands, clearing what `clear` says, with the
/// viewport and scissor its whole; the command buffer, or VK_NULL_HANDLE, with fbe_error's why,
/// if the handle isn't a live target's. end_pass ends it, resolving a multisampled one, and leaves
/// it to be sampled.
VkCommandBuffer begin_pass(std::uint64_t target, const clear_t& clear);
void end_pass(std::uint64_t target);

} // namespace fbe::texture
