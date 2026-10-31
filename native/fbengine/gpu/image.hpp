#pragma once

// Images on the GPU, and work run on it now rather than in a frame: what uploads, readbacks, and
// the tests use.

#include "gpu/vulkan.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace fbe::gpu {

/// An image on the GPU, its memory, and a view of the whole of it.
struct image_t {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
};

/// A 2D color image, one level, drawn into and copied into and out of; an empty one, with
/// fbe_error's why, if it can't be made.
image_t make_color_image(VkFormat format, VkExtent2D extent);

/// Frees an image, once the GPU is done with it, and empties it.
void destroy_image(image_t& image);

/// Records commands with `record` and runs them now, waiting for the GPU to finish them; false,
/// with fbe_error's why, if they couldn't run.
bool run_now(const std::function<void(VkCommandBuffer)>& record);

/// The pixels of a color image of 4 bytes a pixel, in `layout`, which it's left in: its rows, top
/// first, packed; empty, with fbe_error's why, if they can't be read.
std::vector<std::uint8_t> read_pixels(const image_t& image, VkImageLayout layout);

/// The barrier that moves the whole of a color image from `from` to `to`.
VkImageMemoryBarrier2 color_transition(VkImage image, VkImageLayout from, VkImageLayout to,
                                       VkPipelineStageFlags2 before_stages, VkAccessFlags2 before,
                                       VkPipelineStageFlags2 after_stages, VkAccessFlags2 after);

/// Records one image barrier.
void barrier(VkCommandBuffer commands, const VkImageMemoryBarrier2& image);

} // namespace fbe::gpu
