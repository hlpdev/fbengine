// Textures: a pool of them by slot, each slot its index in the bindless set, with the white
// texture at 0; pixels uploaded through staging buffers in the frame's uploads, which free them
// once the GPU's done; mipmaps made by blitting each level from the one before; targets, drawn
// into by the frame graph's passes, multisampled and with depth if they ask; and clears and
// copies, passes too, and reading pixels back.

#include "texture/texture.hpp"

#include "assets/loads.hpp"
#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/graph.hpp"
#include "gpu/image.hpp"
#include "gpu/vulkan.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using fbe::gpu::flag;
using fbe::texture::texture_t;

/// Textures by slot, and each slot's generation, which goes up as its texture is freed, so its
/// old handles are caught. A slot freed while a frame in flight may draw with it is reused only
/// once the GPU's done with that frame.
std::vector<texture_t> textures;
std::vector<std::uint32_t> generations;
std::vector<bool> live;
std::vector<std::uint32_t> free_slots;
std::int32_t textures_made = 0;

/// A CPU copy of a texture's first level's pixels: their bytes, the texture's version they were
/// read at, and the rectangle written since they were last sent, empty when its right isn't past
/// its left.
struct cpu_copy_t {
    std::vector<std::uint8_t> pixels;
    std::uint64_t version = UINT64_MAX;
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

/// The CPU copies there are, by slot, how many have something written to send, and how many have
/// been read back.
std::unordered_map<std::uint32_t, cpu_copy_t>& copies() {
    static std::unordered_map<std::uint32_t, cpu_copy_t> made;
    return made;
}
std::uint32_t written_copies = 0;
std::uint64_t readbacks = 0;

/// Each slot's CPU copy's stamp, which changes whenever its copy's freed: a deque, so each stays
/// where it is as the program runs, and what holds a copy's pointer can tell with a read that it's
/// still there.
std::deque<std::uint64_t>& stamps() {
    static std::deque<std::uint64_t> made;
    return made;
}

std::uint64_t& stamp_of(std::uint32_t slot) {
    while (stamps().size() <= slot) {
        stamps().push_back(0);
    }
    return stamps()[slot];
}

/// Frees a slot's CPU copy, if it has one, and what was written into it and not sent.
void drop_copy(std::uint32_t slot) {
    if (const auto kept = copies().find(slot); kept != copies().end()) {
        written_copies -= kept->second.right > kept->second.left ? 1U : 0U;
        copies().erase(kept);
        ++stamp_of(slot);
    }
}

using fbe::texture::resting_stages;
/// The stages that read textures, which an upload's writes are made visible to.
constexpr auto readers = fbe::texture::shader_stages;
constexpr auto copy = fbe::gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COPY_BIT);
constexpr auto blit = fbe::gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_BLIT_BIT);
constexpr auto transfer =
    copy | blit | fbe::gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_CLEAR_BIT);
constexpr auto transfer_read = fbe::gpu::flag<VkAccessFlags2>(VK_ACCESS_2_TRANSFER_READ_BIT);
constexpr auto transfer_write = fbe::gpu::flag<VkAccessFlags2>(VK_ACCESS_2_TRANSFER_WRITE_BIT);
constexpr auto shader_read = fbe::gpu::flag<VkAccessFlags2>(VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
constexpr auto output =
    fbe::gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
constexpr auto output_write =
    fbe::gpu::flag<VkAccessFlags2>(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
constexpr auto output_access =
    output_write | fbe::gpu::flag<VkAccessFlags2>(VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
constexpr auto tests =
    fbe::gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT) |
    fbe::gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
constexpr auto depth_access =
    fbe::gpu::flag<VkAccessFlags2>(VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT) |
    fbe::gpu::flag<VkAccessFlags2>(VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

std::uint64_t handle_of(std::uint32_t slot) {
    return (std::uint64_t{ fbe::texture::kind } << 56U) |
           (std::uint64_t{ generations[slot] } << 32U) | slot;
}

/// The live slot a handle is for, or UINT32_MAX, with fbe_error's why.
std::uint32_t slot_of(std::uint64_t handle) {
    const auto slot = static_cast<std::uint32_t>(handle);
    const auto generation = static_cast<std::uint32_t>(handle >> 32U) & 0xFFFFFFU;
    const auto tag = static_cast<std::uint8_t>(handle >> 56U);
    // The white texture, slot 0, is the engine's, never a game's.
    if (tag != fbe::texture::kind || slot == fbe::texture::white || slot >= generations.size() ||
        !live[slot] || generations[slot] != generation) {
        fbe::fail(handle == 0 ? "no texture was given" : "the texture was freed");
        return UINT32_MAX;
    }
    return slot;
}

VkFormat vk_format_of(std::int32_t format) {
    switch (format) {
    case FBE_TEXTURE_RGBA8:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case FBE_TEXTURE_RGBA8_LINEAR:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case FBE_TEXTURE_R8:
        return VK_FORMAT_R8_UNORM;
    case FBE_TEXTURE_RGBA16F:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case fbe::texture::shadow_format:
        return VK_FORMAT_R32_SFLOAT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

/// The barrier that moves `count` levels of a texture from `first` on.
VkImageMemoryBarrier2 levels_barrier(VkImage image, std::uint32_t first, std::uint32_t count,
                                     VkImageLayout from, VkImageLayout to,
                                     VkPipelineStageFlags2 before_stages, VkAccessFlags2 before,
                                     VkPipelineStageFlags2 after_stages, VkAccessFlags2 after) {
    VkImageMemoryBarrier2 barrier =
        fbe::gpu::color_transition(image, from, to, before_stages, before, after_stages, after);
    barrier.subresourceRange.baseMipLevel = first;
    barrier.subresourceRange.levelCount = count;
    return barrier;
}

/// Records making every level from the first one's pixels, level 0 in TRANSFER_DST and the rest
/// in TRANSFER_DST too, leaving every level SHADER_READ_ONLY.
void record_mipmaps(VkCommandBuffer commands, const texture_t& texture) {
    auto width = static_cast<std::int32_t>(texture.width);
    auto height = static_cast<std::int32_t>(texture.height);
    for (std::uint32_t level = 1; level < texture.levels; ++level) {
        fbe::gpu::barrier(commands, levels_barrier(texture.image, level - 1, 1,
                                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transfer,
                                                   transfer_write, blit, transfer_read));
        const std::int32_t next_width = std::max(width / 2, 1);
        const std::int32_t next_height = std::max(height / 2, 1);
        const VkImageBlit region{
            .srcSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = level - 1,
                .layerCount = 1,
            },
            .srcOffsets = { {}, { .x = width, .y = height, .z = 1 } },
            .dstSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = level,
                .layerCount = 1,
            },
            .dstOffsets = { {}, { .x = next_width, .y = next_height, .z = 1 } },
        };
        vkCmdBlitImage(commands, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
        width = next_width;
        height = next_height;
    }
    const std::uint32_t last = texture.levels - 1;
    if (last > 0) {
        fbe::gpu::barrier(commands, levels_barrier(texture.image, 0, last,
                                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, blit,
                                                   0, readers, shader_read));
    }
    fbe::gpu::barrier(commands,
                      levels_barrier(texture.image, last, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, transfer,
                                     transfer_write, readers, shader_read));
}

/// A slot for a new texture, or UINT32_MAX, with fbe_error's why, when the set holds no more.
std::uint32_t take_slot() {
    if (!free_slots.empty()) {
        const std::uint32_t slot = free_slots.back();
        free_slots.pop_back();
        return slot;
    }
    if (textures.size() >= fbe::gpu::texture_capacity()) {
        fbe::fail(std::format("the GPU holds {} textures at once", fbe::gpu::texture_capacity()));
        return UINT32_MAX;
    }
    textures.emplace_back();
    generations.push_back(1);
    live.push_back(false);
    return static_cast<std::uint32_t>(textures.size() - 1);
}

/// Destroys a texture's images now, which the GPU is done with.
void destroy(texture_t& texture) {
    const fbe::gpu::device_t& gpu = *fbe::gpu::device();
    vkDestroyImageView(gpu.device, texture.view, nullptr);
    vmaDestroyImage(gpu.allocator, texture.image, texture.memory);
    vkDestroyImageView(gpu.device, texture.stored_view, nullptr);
    vkDestroyImageView(gpu.device, texture.stored_msaa_view, nullptr);
    vkDestroyImageView(gpu.device, texture.msaa_view, nullptr);
    vmaDestroyImage(gpu.allocator, texture.msaa, texture.msaa_memory);
    vkDestroyImageView(gpu.device, texture.depth_view, nullptr);
    vmaDestroyImage(gpu.allocator, texture.depth, texture.depth_memory);
    texture = {};
}

/// The two formats a target of sRGB bytes is viewed as, which, being listed, leaves a GPU that
/// compresses what it draws doing so.
constexpr std::array<VkFormat, 2> stored_or_decoded{
    VK_FORMAT_R8G8B8A8_SRGB,
    VK_FORMAT_R8G8B8A8_UNORM,
};
const VkImageFormatListCreateInfo stored_or_decoded_list{
    .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
    .viewFormatCount = stored_or_decoded.size(),
    .pViewFormats = stored_or_decoded.data(),
};

/// A view of a target's image, of a format it has that writes its bytes as they are; false, with
/// fbe_error's why, if it can't be made.
bool make_stored_view(VkImage image, VkImageView& view) {
    const VkImageViewCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    const VkResult result = vkCreateImageView(fbe::gpu::device()->device, &info, nullptr, &view);
    if (result != VK_SUCCESS) {
        view = VK_NULL_HANDLE;
        return fbe::gpu::fail_vulkan("couldn't make a target's view of its stored bytes", result);
    }
    return true;
}

/// A target's own attachment, the size of its image: its multisampled color or its depth, with a
/// view, and, given where to put it, one of its stored bytes; false, with fbe_error's why, if it
/// can't be made.
bool make_attachment(const texture_t& texture, VkFormat format, VkImageUsageFlags usage,
                     VkImageAspectFlags aspect, VkImage& image, VmaAllocation& memory,
                     VkImageView& view, VkImageView* stored_view = nullptr) {
    const fbe::gpu::device_t& gpu = *fbe::gpu::device();
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = stored_view != nullptr ? &stored_or_decoded_list : nullptr,
        .flags = stored_view != nullptr
                     ? flag<VkImageCreateFlags>(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)
                     : 0U,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { .width = texture.width, .height = texture.height, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = texture.samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo allocation{ .usage = VMA_MEMORY_USAGE_AUTO };
    VkResult result = vmaCreateImage(gpu.allocator, &info, &allocation, &image, &memory, nullptr);
    if (result != VK_SUCCESS) {
        image = VK_NULL_HANDLE;
        return fbe::gpu::fail_vulkan("couldn't make a target's attachment", result);
    }
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = { .aspectMask = aspect, .levelCount = 1, .layerCount = 1 },
    };
    result = vkCreateImageView(gpu.device, &view_info, nullptr, &view);
    if (result != VK_SUCCESS) {
        view = VK_NULL_HANDLE;
        return fbe::gpu::fail_vulkan("couldn't make a target's attachment's view", result);
    }
    return stored_view == nullptr || make_stored_view(image, *stored_view);
}

/// The barrier that moves a target's depth from `from` to `to`.
VkImageMemoryBarrier2 depth_barrier(VkImage image, VkImageLayout from, VkImageLayout to,
                                    VkPipelineStageFlags2 before_stages, VkAccessFlags2 before) {
    VkImageMemoryBarrier2 barrier =
        fbe::gpu::color_transition(image, from, to, before_stages, before, tests, depth_access);
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    return barrier;
}

/// What a texture is made: its size and format, and whether it has mipmaps, or is a target, with
/// its samples and depth, and whether it's the engine's own.
struct spec_t {
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::int32_t format = FBE_TEXTURE_RGBA8;
    bool mipmaps = false;
    bool target = false;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool depth = false;
    bool engine = false;
    /// A file's image, which it's made of, of this format; its levels, and more made from the
    /// first if `mipmaps`, in place of being cleared.
    const fbe::assets::image_t* image = nullptr;
    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    /// Whether its alpha's for blending.
    bool alpha = false;
    /// Whether its view goes in the bindless set, as a slot past the set's end, a reload's,
    /// doesn't.
    bool bound = true;
};

/// The half float nearest `value`, from 0 to the largest finite one; NaN's 0.
std::uint16_t half_of(float value) {
    constexpr float largest_half = 65504.0F;
    // NaN compares false, and is left at 0.
    const float light = value > 0.0F ? std::min(value, largest_half) : 0.0F;
    const auto bits = std::bit_cast<std::uint32_t>(light);
    const auto exponent = static_cast<std::int32_t>(bits >> 23U) - 127;
    if (exponent < -24) {
        return 0;
    }
    if (exponent < -14) {
        // Subnormal in a half, in units of 2^-24.
        return static_cast<std::uint16_t>(std::lround(std::ldexp(light, 24)));
    }
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(exponent + 15) << 10U) +
                                      (((bits & 0x7FFFFFU) + 0x1000U) >> 13U));
}

VkFormat vk_format_of(const spec_t& spec) {
    return spec.image != nullptr ? spec.vk_format : vk_format_of(spec.format);
}

/// Whether the device can make a texture of `spec`; false, with fbe_error's why, if not.
bool supports(const spec_t& spec) {
    const fbe::gpu::device_t& gpu = *fbe::gpu::device();
    VkFormatProperties support{};
    vkGetPhysicalDeviceFormatProperties(gpu.physical, vk_format_of(spec), &support);
    const auto mipmappable =
        flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_BLIT_SRC_BIT) |
        flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_BLIT_DST_BIT) |
        flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    if (spec.mipmaps && (support.optimalTilingFeatures & mipmappable) != mipmappable) {
        return fbe::fail("this GPU can't make that format's mipmaps");
    }
    const auto drawable = flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) |
                          flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
    if (spec.target && (support.optimalTilingFeatures & drawable) != drawable) {
        return fbe::fail("this GPU can't draw into that format");
    }
    const VkSampleCountFlags counts =
        gpu.properties.limits.framebufferColorSampleCounts &
        (spec.depth ? gpu.properties.limits.framebufferDepthSampleCounts : ~0U);
    if ((counts & flag<VkSampleCountFlags>(spec.samples)) == 0) {
        return fbe::fail(std::format("this GPU doesn't draw {} samples a pixel",
                                     static_cast<int>(spec.samples)));
    }
    return true;
}

/// A buffer to upload through, mapped, which is freed after the frame.
struct staging_t {
    VkBuffer buffer = VK_NULL_HANDLE;
    std::uint8_t* to = nullptr;
    VmaAllocation memory = VK_NULL_HANDLE;
};

/// A new buffer of `size` bytes to upload through; none, with fbe_error's why, if it can't be made.
staging_t make_staging(VkDeviceSize size) {
    const fbe::gpu::device_t& gpu = *fbe::gpu::device();
    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_SRC_BIT),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo allocation_info{
        .flags =
            flag<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) |
            flag<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_MAPPED_BIT),
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    staging_t staging;
    VmaAllocationInfo mapped{};
    const VkResult made = fbe::gpu::faulted(fbe::gpu::fault_t::UPLOAD)
                              ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                              : vmaCreateBuffer(gpu.allocator, &buffer_info, &allocation_info,
                                                &staging.buffer, &staging.memory, &mapped);
    if (made != VK_SUCCESS) {
        fbe::gpu::fail_vulkan("couldn't make a buffer to upload a texture through", made);
        return {};
    }
    staging.to = static_cast<std::uint8_t*>(mapped.pMappedData);
    fbe::gpu::after_frame([buffer = staging.buffer, memory = staging.memory] {
        vmaDestroyBuffer(fbe::gpu::device()->allocator, buffer, memory);
    });
    fbe::gpu::count_upload(size);
    return staging;
}

/// What's written of a file's image: its levels, packed into a buffer, and where each goes.
struct image_copy_t {
    staging_t staging;
    std::vector<VkBufferImageCopy> regions;
};

/// A buffer of `image`'s levels, each where a copy can read it; no buffer, with fbe_error's why, if
/// it can't be made.
image_copy_t staged_image(const fbe::assets::image_t& image) {
    // Every format's block is 16 bytes or fewer, and divides this.
    constexpr VkDeviceSize alignment = 16;
    image_copy_t staged;
    VkDeviceSize size = 0;
    for (const fbe::assets::level_t& level : image.levels) {
        const auto at = static_cast<std::uint32_t>(staged.regions.size());
        staged.regions.push_back({
            .bufferOffset = size,
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = at,
                .layerCount = 1,
            },
            .imageExtent = { .width = level.width, .height = level.height, .depth = 1 },
        });
        size = (size + level.size + alignment - 1) / alignment * alignment;
    }
    staged.staging = make_staging(size);
    if (staged.staging.buffer == VK_NULL_HANDLE) {
        return staged;
    }
    for (std::size_t at = 0; at < image.levels.size(); ++at) {
        std::memcpy(staged.staging.to + staged.regions[at].bufferOffset,
                    image.texels.data() + image.levels[at].offset, image.levels[at].size);
    }
    vmaFlushAllocation(fbe::gpu::device()->allocator, staged.staging.memory, 0, VK_WHOLE_SIZE);
    return staged;
}

/// Makes a texture in `slot`: its image, transparent black or a file's, every level
/// SHADER_READ_ONLY, a target's attachments, cleared, and its view in the bindless set; false, with
/// fbe_error's why, if it can't.
bool make(std::uint32_t slot, const spec_t& spec) {
    const fbe::gpu::device_t& gpu = *fbe::gpu::device();
    if (!supports(spec)) {
        return false;
    }
    texture_t& texture = textures[slot];
    const auto file_levels =
        spec.image != nullptr ? static_cast<std::uint32_t>(spec.image->levels.size()) : 1U;
    texture = {
        .format = spec.format,
        .vk_format = vk_format_of(spec),
        .width = spec.width,
        .height = spec.height,
        .levels = spec.mipmaps ? static_cast<std::uint32_t>(
                                     std::bit_width(std::max(spec.width, spec.height)))
                               : file_levels,
        .filter = FBE_TEXTURE_LINEAR,
        .wrap_u = FBE_TEXTURE_REPEAT,
        .wrap_v = FBE_TEXTURE_REPEAT,
        .references = 1,
        .alpha = spec.alpha,
        .engine = spec.engine,
        .target = spec.target,
        .samples = spec.samples,
    };
    // A game's target of sRGB bytes, the screen too, as a camera can draw it in stored values.
    const bool stores = spec.target && !spec.engine && texture.vk_format == VK_FORMAT_R8G8B8A8_SRGB;
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = stores ? &stored_or_decoded_list : nullptr,
        .flags = stores ? flag<VkImageCreateFlags>(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) : 0U,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = texture.vk_format,
        .extent = { .width = spec.width, .height = spec.height, .depth = 1 },
        .mipLevels = texture.levels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = flag<VkImageUsageFlags>(VK_IMAGE_USAGE_SAMPLED_BIT) |
                 flag<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_DST_BIT) |
                 flag<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT) |
                 (spec.target ? flag<VkImageUsageFlags>(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) : 0U),
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo allocation{ .usage = VMA_MEMORY_USAGE_AUTO };
    VkResult result =
        vmaCreateImage(gpu.allocator, &info, &allocation, &texture.image, &texture.memory, nullptr);
    if (result != VK_SUCCESS) {
        texture = {};
        return fbe::gpu::fail_vulkan("couldn't make a texture", result);
    }
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = texture.vk_format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = texture.levels,
            .layerCount = 1,
        },
    };
    result = fbe::gpu::faulted(fbe::gpu::fault_t::VIEW)
                 ? VK_ERROR_OUT_OF_HOST_MEMORY
                 : vkCreateImageView(gpu.device, &view_info, nullptr, &texture.view);
    if (result != VK_SUCCESS) {
        destroy(texture);
        return fbe::gpu::fail_vulkan("couldn't make a texture's view", result);
    }
    if (stores && !make_stored_view(texture.image, texture.stored_view)) {
        destroy(texture);
        return false;
    }
    const auto attached = flag<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const bool multisampled = spec.samples != VK_SAMPLE_COUNT_1_BIT;
    if ((multisampled &&
         !make_attachment(texture, texture.vk_format,
                          attached | flag<VkImageUsageFlags>(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
                          flag<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT), texture.msaa,
                          texture.msaa_memory, texture.msaa_view,
                          stores ? &texture.stored_msaa_view : nullptr)) ||
        (spec.depth && !make_attachment(texture, fbe::texture::depth_format,
                                        attached | flag<VkImageUsageFlags>(
                                                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT),
                                        flag<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT),
                                        texture.depth, texture.depth_memory, texture.depth_view))) {
        destroy(texture);
        return false;
    }

    // Before any commands use the image, so it can still go.
    image_copy_t file;
    if (spec.image != nullptr) {
        file = staged_image(*spec.image);
        if (file.staging.buffer == VK_NULL_HANDLE) {
            destroy(texture);
            return false;
        }
    }
    VkCommandBuffer commands = fbe::gpu::upload_commands();
    const VkImageMemoryBarrier2 to_clear =
        levels_barrier(texture.image, 0, texture.levels, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, transfer, 0, transfer, transfer_write);
    fbe::gpu::barrier(commands, to_clear);
    const VkClearColorValue black{};
    if (spec.image != nullptr) {
        vkCmdCopyBufferToImage(
            commands, file.staging.buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<std::uint32_t>(file.regions.size()), file.regions.data());
    } else {
        const VkImageSubresourceRange everything{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = texture.levels,
            .layerCount = 1,
        };
        vkCmdClearColorImage(commands, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black,
                             1, &everything);
    }
    if (spec.image != nullptr && spec.mipmaps) {
        record_mipmaps(commands, texture);
    } else {
        fbe::gpu::barrier(commands,
                          levels_barrier(texture.image, 0, texture.levels,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, transfer,
                                         transfer_write, resting_stages, shader_read));
    }
    // A multisampled color rests drawn into, and depth rests as depth, cleared to the farthest.
    if (texture.msaa != VK_NULL_HANDLE) {
        fbe::gpu::barrier(commands,
                          fbe::gpu::color_transition(texture.msaa, VK_IMAGE_LAYOUT_UNDEFINED,
                                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, transfer,
                                                     0, transfer, transfer_write));
        const VkImageSubresourceRange whole{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        };
        vkCmdClearColorImage(commands, texture.msaa, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black,
                             1, &whole);
        fbe::gpu::barrier(
            commands, fbe::gpu::color_transition(texture.msaa, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, transfer,
                                                 transfer_write, output, output_access));
    }
    if (texture.depth != VK_NULL_HANDLE) {
        VkImageMemoryBarrier2 to_depth_clear =
            depth_barrier(texture.depth, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, transfer, 0);
        to_depth_clear.dstStageMask = transfer;
        to_depth_clear.dstAccessMask = transfer_write;
        fbe::gpu::barrier(commands, to_depth_clear);
        const VkClearDepthStencilValue farthest{ .depth = 0.0F };
        const VkImageSubresourceRange whole{
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .levelCount = 1,
            .layerCount = 1,
        };
        vkCmdClearDepthStencilImage(commands, texture.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &farthest, 1, &whole);
        fbe::gpu::barrier(commands,
                          depth_barrier(texture.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, transfer,
                                        transfer_write));
    }
    if (spec.bound) {
        fbe::gpu::write_texture(slot, texture.view);
    }
    live[slot] = true;
    textures_made += spec.engine ? 0 : 1;
    return true;
}

/// Uploads `width` by `height` pixels at `x`, `y` into level 0 of `handle`'s texture, rows `pitch`
/// bytes apart, through a staging buffer freed after the frame, and makes the mipmaps again: in the
/// uploads, before the frame's commands, unless a pass of the frame's graph uses the texture, and
/// then in a pass after it, so writes and what else the frame does with a texture keep their order;
/// or, `ahead`, in the uploads whatever the graph has.
bool upload(std::uint64_t handle, const texture_t& texture, std::uint32_t x, std::uint32_t y,
            std::uint32_t width, std::uint32_t height, const std::uint8_t* pixels,
            std::uint32_t pitch, bool ahead = false) {
    fbe::texture::changed_on_gpu(handle);
    const std::uint32_t row = width * fbe::texture::pixel_size(texture.format);
    const staging_t staging = make_staging(VkDeviceSize{ row } * height);
    if (staging.buffer == VK_NULL_HANDLE) {
        return false;
    }
    VkBuffer buffer = staging.buffer;
    for (std::uint32_t line = 0; line < height; ++line) {
        std::memcpy(staging.to + (std::size_t{ line } * row),
                    pixels + (std::size_t{ line } * pitch), row);
    }
    vmaFlushAllocation(fbe::gpu::device()->allocator, staging.memory, 0, VK_WHOLE_SIZE);

    const VkBufferImageCopy region{
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageOffset = { .x = static_cast<std::int32_t>(x), .y = static_cast<std::int32_t>(y) },
        .imageExtent = { .width = width, .height = height, .depth = 1 },
    };
    if (!ahead && fbe::gpu::in_graph(handle)) {
        // Carried, as the uploads are, if the frame's submit fails, its staging buffer with it.
        return fbe::gpu::add_pass({
            .name = "write",
            .uses = { { fbe::gpu::texture_resource(handle), fbe::gpu::use_t::MIPMAPPED } },
            .record =
                [buffer, written = texture, region](VkCommandBuffer commands) {
                    vkCmdCopyBufferToImage(commands, buffer, written.image,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                    record_mipmaps(commands, written);
                },
            .carried = true,
        });
    }
    VkCommandBuffer commands = fbe::gpu::upload_commands();
    fbe::gpu::barrier(commands, levels_barrier(texture.image, 0, texture.levels,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, resting_stages,
                                               0, transfer, transfer_write));
    vkCmdCopyBufferToImage(commands, buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);
    record_mipmaps(commands, texture);
    return true;
}

/// Uploads the whole of `level` of `handle`'s texture, `width` by `height`, from `pixels`, rows
/// `pitch` bytes apart, as `upload` does but making nothing of the other levels from it.
bool upload_level(std::uint64_t handle, const texture_t& texture, std::uint32_t level,
                  std::uint32_t width, std::uint32_t height, const std::uint8_t* pixels,
                  std::uint32_t pitch) {
    if (level == 0) {
        fbe::texture::changed_on_gpu(handle);
    }
    const std::uint32_t row = width * fbe::texture::pixel_size(texture.format);
    const staging_t staging = make_staging(VkDeviceSize{ row } * height);
    if (staging.buffer == VK_NULL_HANDLE) {
        return false;
    }
    VkBuffer buffer = staging.buffer;
    for (std::uint32_t line = 0; line < height; ++line) {
        std::memcpy(staging.to + (std::size_t{ line } * row),
                    pixels + (std::size_t{ line } * pitch), row);
    }
    vmaFlushAllocation(fbe::gpu::device()->allocator, staging.memory, 0, VK_WHOLE_SIZE);
    const VkBufferImageCopy region{
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = level,
            .layerCount = 1,
        },
        .imageExtent = { .width = width, .height = height, .depth = 1 },
    };
    if (fbe::gpu::in_graph(handle)) {
        return fbe::gpu::add_pass({
            .name = "write a level",
            .uses = { { fbe::gpu::texture_resource(handle), fbe::gpu::use_t::COPY_TO } },
            .record =
                [buffer, image = texture.image, region](VkCommandBuffer commands) {
                    vkCmdCopyBufferToImage(commands, buffer, image,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                },
            .carried = true,
        });
    }
    VkCommandBuffer commands = fbe::gpu::upload_commands();
    fbe::gpu::barrier(commands, levels_barrier(texture.image, level, 1,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, resting_stages,
                                               0, transfer, transfer_write));
    vkCmdCopyBufferToImage(commands, buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);
    fbe::gpu::barrier(commands,
                      levels_barrier(texture.image, level, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, transfer,
                                     transfer_write, readers, shader_read));
    return true;
}

bool valid_filter(std::int32_t filter) {
    return filter == FBE_TEXTURE_NEAREST || filter == FBE_TEXTURE_LINEAR;
}

bool valid_wrap(std::int32_t wrap) {
    return wrap == FBE_TEXTURE_REPEAT || wrap == FBE_TEXTURE_CLAMP || wrap == FBE_TEXTURE_MIRROR;
}

/// Whether a new texture can be `width` by `height` of `format`, starting the GPU if it hasn't;
/// false, with fbe_error's why, if not.
bool checked(std::int32_t width, std::int32_t height, std::int32_t format) {
    if (fbe::texture::pixel_size(format) == 0 || format == fbe::texture::shadow_format) {
        return fbe::fail(std::format("{} isn't a TextureFormat", format));
    }
    if (!fbe::gpu::start_with_window()) {
        return false;
    }
    if (fbe::gpu::lost()) {
        return fbe::gpu::fail_lost();
    }
    const auto largest =
        static_cast<std::int32_t>(fbe::gpu::device()->properties.limits.maxImageDimension2D);
    if (width < 1 || height < 1 || width > largest || height > largest) {
        return fbe::fail(std::format("a texture is 1 to {} pixels on a side, not {}x{}", largest,
                                     width, height));
    }
    return true;
}

/// A new texture of `spec`, its handle, or 0, with fbe_error's why.
std::uint64_t create(const spec_t& spec) {
    const std::uint32_t slot = take_slot();
    if (slot == UINT32_MAX) {
        return 0;
    }
    if (!make(slot, spec)) {
        free_slots.push_back(slot);
        return 0;
    }
    return handle_of(slot);
}

/// Copies `region` of a texture within it, through a transient target the region's size, so
/// regions that overlap copy as they were; false, with fbe_error's why, if it can't.
bool copy_within(const texture_t& texture, fbe::gpu::resource_t resource,
                 const VkImageCopy& region) {
    const fbe::gpu::resource_t through = fbe::gpu::transient_resource({
        .width = region.extent.width,
        .height = region.extent.height,
        .format = texture.format,
    });
    if (through == fbe::gpu::no_resource) {
        return false;
    }
    VkImageCopy out = region;
    out.dstOffset = {};
    VkImageCopy back = region;
    back.srcOffset = {};
    return fbe::gpu::add_pass({
               .name = "copy",
               .uses = {
                   { resource, fbe::gpu::use_t::COPY_FROM },
                   { through, fbe::gpu::use_t::COPY_TO },
               },
               .record =
                   [image = texture.image, through, out](VkCommandBuffer commands) {
                       const texture_t* held =
                           fbe::texture::find(fbe::gpu::resource_texture(through));
                       vkCmdCopyImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      held->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &out);
                   },
           }) &&
           fbe::gpu::add_pass({
               .name = "copy",
               .uses = {
                   { through, fbe::gpu::use_t::COPY_FROM },
                   { resource, fbe::gpu::use_t::MIPMAPPED },
               },
               .record =
                   [copied = texture, through, back](VkCommandBuffer commands) {
                       const texture_t* held =
                           fbe::texture::find(fbe::gpu::resource_texture(through));
                       vkCmdCopyImage(commands, held->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      copied.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &back);
                       record_mipmaps(commands, copied);
                   },
           });
}

/// Whether a region is inside a texture's first level; false, with fbe_error's why, if not.
bool inside(const texture_t& texture, std::int32_t x, std::int32_t y, std::int32_t width,
            std::int32_t height) {
    const auto across = static_cast<std::int64_t>(texture.width);
    const auto down = static_cast<std::int64_t>(texture.height);
    if (x < 0 || y < 0 || width < 1 || height < 1 || x + std::int64_t{ width } > across ||
        y + std::int64_t{ height } > down) {
        return fbe::fail(std::format("a {}x{} region at {}, {} isn't in a {}x{} texture", width,
                                     height, x, y, across, down));
    }
    return true;
}

/// False, with fbe_error's why, for a texture of a format a file has, which is only sampled.
bool has_pixels(const texture_t& texture) {
    return texture.format != FBE_TEXTURE_FROM_FILE ||
           fbe::fail("a texture of a file's own format is only sampled: `compression = \"none\"` "
                     "in its sidecar keeps its pixels");
}

/// A format a file's image can be: its FBE_TEXTURE_ format, and how many texels a side its blocks
/// are, and bytes each is.
struct file_format_t {
    std::int32_t format = FBE_TEXTURE_FROM_FILE;
    std::uint32_t block = 1;
    std::uint32_t bytes = 0;
};

/// What the Vulkan number a file gives is, if it's a format fbengine samples, which it knows the
/// size of, so what the file says of its levels' sizes can be checked before a copy reads them.
std::optional<file_format_t> file_format(std::uint32_t format) {
    switch (static_cast<VkFormat>(format)) {
    case VK_FORMAT_R8G8B8A8_SRGB:
        return file_format_t{ .format = FBE_TEXTURE_RGBA8, .bytes = 4 };
    case VK_FORMAT_R8G8B8A8_UNORM:
        return file_format_t{ .format = FBE_TEXTURE_RGBA8_LINEAR, .bytes = 4 };
    case VK_FORMAT_R8_UNORM:
        return file_format_t{ .format = FBE_TEXTURE_R8, .bytes = 1 };
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return file_format_t{ .format = FBE_TEXTURE_RGBA16F, .bytes = 8 };
    case VK_FORMAT_R16G16B16A16_UNORM:
        return file_format_t{ .bytes = 8 };
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
        return file_format_t{ .block = 4, .bytes = 8 };
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
    case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        return file_format_t{ .block = 4, .bytes = 16 };
    default:
        return std::nullopt;
    }
}

/// Whether the last load's texture failed for what's in its file.
bool load_invalid = false;

/// False, with fbe_error's why, said to be its file's: what's wrong with an image.
bool invalid_image(const std::string& why) {
    load_invalid = true;
    return fbe::fail(why);
}

/// Whether an image's layout is one texture's: one 2D image, whose levels are the sizes its format
/// says, within its texels; false, with fbe_error's why, if not.
bool well_formed(const fbe::assets::image_t& image, const file_format_t& format) {
    if (image.depth != 1 || image.layers != 1 || image.faces != 1) {
        return invalid_image("a texture is one 2D image, not an array, a cube, or a volume");
    }
    if (image.levels.empty() || image.levels.size() > static_cast<std::size_t>(std::bit_width(
                                                          std::max(image.width, image.height)))) {
        return invalid_image(std::format("a {}x{} image doesn't have {} levels", image.width,
                                         image.height, image.levels.size()));
    }
    for (const fbe::assets::level_t& level : image.levels) {
        const std::uint64_t across =
            (std::uint64_t{ level.width } + format.block - 1) / format.block;
        const std::uint64_t down =
            (std::uint64_t{ level.height } + format.block - 1) / format.block;
        if (across * down * format.bytes != level.size || level.offset > image.texels.size() ||
            level.size > image.texels.size() - level.offset) {
            return invalid_image("the image's levels aren't the sizes its format says");
        }
    }
    return true;
}

/// The image load done and loaded that a handle is; nullptr, with fbe_error's why, if it isn't.
const fbe::assets::image_t* image_of(std::uint64_t handle) {
    load_invalid = false;
    const fbe::assets::load_t* load = fbe::assets::loads().get(handle);
    if (load == nullptr || !load->done || load->error ||
        load->kind != fbe::assets::load_kind_t::IMAGE) {
        fbe::fail("the load isn't an image load done and loaded");
        return nullptr;
    }
    return &load->image;
}

/// Whether any pixel of an 8-bit RGBA image's first level isn't whole. What's compressed is taken
/// to be whole, as its blocks would have to be decoded to say, unless its settings say otherwise.
bool has_alpha(const fbe::assets::image_t& image, std::int32_t format) {
    if (format != FBE_TEXTURE_RGBA8 && format != FBE_TEXTURE_RGBA8_LINEAR) {
        return false;
    }
    const fbe::assets::level_t& level = image.levels.front();
    for (std::size_t at = 3; at < level.size; at += 4) {
        if (image.texels[level.offset + at] != 255) {
            return true;
        }
    }
    return false;
}

/// What a texture of a file's image is, starting the GPU if it hasn't; nothing, with fbe_error's
/// why, if it can't be one.
/// The format of the same bytes, not decoded from sRGB as they're read.
VkFormat undecoded(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case VK_FORMAT_BC2_SRGB_BLOCK:
        return VK_FORMAT_BC2_UNORM_BLOCK;
    case VK_FORMAT_BC3_SRGB_BLOCK:
        return VK_FORMAT_BC3_UNORM_BLOCK;
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return VK_FORMAT_BC7_UNORM_BLOCK;
    case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    default:
        return format;
    }
}

std::optional<spec_t> spec_of(const fbe::assets::image_t& image, bool srgb, bool mipmaps) {
    if (!fbe::gpu::start_with_window()) {
        return std::nullopt;
    }
    if (fbe::gpu::lost()) {
        fbe::gpu::fail_lost();
        return std::nullopt;
    }
    std::optional<file_format_t> format = file_format(image.format);
    if (!format) {
        invalid_image(std::format("a texture isn't of the image's Vulkan format {}", image.format));
        return std::nullopt;
    }
    if (!well_formed(image, *format)) {
        return std::nullopt;
    }
    auto vk_format = static_cast<VkFormat>(image.format);
    if (!srgb) {
        vk_format = undecoded(vk_format);
        if (vk_format == VK_FORMAT_R8G8B8A8_UNORM) {
            format->format = FBE_TEXTURE_RGBA8_LINEAR;
        }
    }
    const fbe::gpu::device_t& gpu = *fbe::gpu::device();
    VkFormatProperties support{};
    vkGetPhysicalDeviceFormatProperties(gpu.physical, vk_format, &support);
    const auto sampled =
        flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) |
        flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    if (fbe::gpu::faulted(fbe::gpu::fault_t::NO_FILE_FORMAT) ||
        (support.optimalTilingFeatures & sampled) != sampled) {
        fbe::fail(std::format("this GPU doesn't sample the image's Vulkan format {}: its sidecar's "
                              "`compression = \"uastc\"` is transcoded to what any GPU samples",
                              image.format));
        return std::nullopt;
    }
    if (image.width > gpu.properties.limits.maxImageDimension2D ||
        image.height > gpu.properties.limits.maxImageDimension2D) {
        fbe::fail(std::format("this GPU's textures are at most {} pixels on a side, not {}x{}",
                              gpu.properties.limits.maxImageDimension2D, image.width,
                              image.height));
        return std::nullopt;
    }
    const auto blitted = flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_BLIT_SRC_BIT) |
                         flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_BLIT_DST_BIT);
    return spec_t{
        .width = image.width,
        .height = image.height,
        .format = format->format,
        .mipmaps = mipmaps && image.levels.size() == 1 && format->block == 1 &&
                   (support.optimalTilingFeatures & blitted) == blitted,
        .image = &image,
        .vk_format = vk_format,
        .alpha = has_alpha(image, format->format),
    };
}

} // namespace

namespace fbe::texture {

texture_t* find(std::uint64_t handle) {
    const std::uint32_t slot = slot_of(handle);
    return slot != UINT32_MAX ? &textures[slot] : nullptr;
}

std::uint32_t index_of(std::uint64_t handle) {
    const std::uint32_t slot = slot_of(handle);
    return slot != UINT32_MAX ? slot : white;
}

std::uint32_t sampler_of(const texture_t& texture) {
    return gpu::sampler_index(texture.filter, texture.wrap_u, texture.wrap_v);
}

std::vector<std::uint8_t> read(std::uint64_t handle, std::uint32_t level) {
    send_pixels(handle);
    const texture_t* texture = find(handle);
    if (texture == nullptr || !has_pixels(*texture)) {
        return {};
    }
    if (level >= texture->levels) {
        fail(std::format("the texture has {} levels, not {}", texture->levels, level + 1));
        return {};
    }
    // Recording the frame can make textures, transient targets' images, which can move the pool,
    // so the texture's found again after.
    if (!gpu::finish_frame()) {
        return {};
    }
    texture = find(handle);
    if (texture == nullptr) {
        return {};
    }
    const VkExtent2D extent{
        .width = std::max(texture->width >> level, 1U),
        .height = std::max(texture->height >> level, 1U),
    };
    return gpu::read_pixels(texture->image, extent, pixel_size(texture->format),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, level);
}

void send_pixels(std::uint64_t handle) {
    if (written_copies == 0) {
        return;
    }
    const auto slot = static_cast<std::uint32_t>(handle);
    const auto found = copies().find(slot);
    if (found == copies().end() || found->second.right <= found->second.left) {
        return;
    }
    cpu_copy_t& kept = found->second;
    const texture_t* texture = find(handle);
    if (texture == nullptr) {
        return;
    }
    // Emptied first, so the write, which this is called before, doesn't send it again.
    const auto left = static_cast<std::uint32_t>(kept.left);
    const auto top = static_cast<std::uint32_t>(kept.top);
    const auto width = static_cast<std::uint32_t>(kept.right - kept.left);
    const auto height = static_cast<std::uint32_t>(kept.bottom - kept.top);
    kept.right = kept.left;
    --written_copies;
    const std::uint32_t pitch = texture->width * pixel_size(texture->format);
    const std::size_t at =
        (std::size_t{ top } * pitch) + (std::size_t{ left } * pixel_size(texture->format));
    upload(handle, *texture, left, top, width, height, kept.pixels.data() + at, pitch);
    // What the GPU has now is what the kept has.
    const texture_t* sent = find(handle);
    kept.version = sent != nullptr ? sent->version : UINT64_MAX;
}

void changed_on_gpu(std::uint64_t handle) {
    const auto slot = static_cast<std::uint32_t>(handle);
    if (slot < textures.size() && live[slot]) {
        ++textures[slot].version;
    }
}

std::uint64_t pixel_readbacks() {
    return readbacks;
}

std::uint64_t create_engine_target(std::uint32_t width, std::uint32_t height, std::int32_t format,
                                   VkSampleCountFlagBits samples, bool depth) {
    return create({
        .width = width,
        .height = height,
        .format = format,
        .target = true,
        .samples = samples,
        .depth = depth,
        .engine = true,
    });
}

std::uint64_t create_engine_texture(std::uint32_t width, std::uint32_t height, std::int32_t format,
                                    bool mipmaps) {
    if (gpu::device() == nullptr) {
        fbe::fail("the GPU isn't started");
        return 0;
    }
    return create({
        .width = width,
        .height = height,
        .format = format,
        .mipmaps = mipmaps,
        .engine = true,
    });
}

bool write_ahead(std::uint64_t handle, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                 std::uint32_t height, const std::uint8_t* pixels, std::uint32_t pitch) {
    const texture_t* found = find(handle);
    if (found == nullptr) {
        return false;
    }
    if (found->levels != 1 || x + width > found->width || y + height > found->height) {
        return fbe::fail("a write ahead is to a part of a texture without mipmaps");
    }
    return upload(handle, *found, x, y, width, height, pixels, pitch, true);
}

assets::transcode_t device_transcode() {
    const gpu::device_t* gpu = gpu::device();
    if (gpu == nullptr || gpu::faulted(gpu::fault_t::NO_COMPRESSED)) {
        return assets::transcode_t::RGBA8;
    }
    const auto samples = [gpu](VkFormat format) {
        const auto sampled =
            flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) |
            flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        VkFormatProperties support{};
        vkGetPhysicalDeviceFormatProperties(gpu->physical, format, &support);
        return (support.optimalTilingFeatures & sampled) == sampled;
    };
    if (samples(VK_FORMAT_BC7_SRGB_BLOCK) && samples(VK_FORMAT_BC7_UNORM_BLOCK)) {
        return assets::transcode_t::BC7;
    }
    if (samples(VK_FORMAT_ASTC_4x4_SRGB_BLOCK) && samples(VK_FORMAT_ASTC_4x4_UNORM_BLOCK)) {
        return assets::transcode_t::ASTC_4X4;
    }
    return assets::transcode_t::RGBA8;
}

std::uint32_t pixel_size(std::int32_t format) {
    switch (format) {
    case FBE_TEXTURE_RGBA8:
    case FBE_TEXTURE_RGBA8_LINEAR:
    case shadow_format:
        return 4;
    case FBE_TEXTURE_R8:
        return 1;
    case FBE_TEXTURE_RGBA16F:
        return 8;
    default:
        return 0;
    }
}

} // namespace fbe::texture

namespace fbe::gpu {

bool start_textures() {
    // The white texture, which a draw with no texture reads, in slot 0.
    const std::uint32_t slot = take_slot();
    if (slot == UINT32_MAX || !make(slot, { .format = FBE_TEXTURE_RGBA8_LINEAR, .engine = true })) {
        return false;
    }
    const std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
    return upload(handle_of(slot), textures[slot], 0, 0, 1, 1, white.data(), 4);
}

void stop_textures() {
    for (std::uint32_t slot = 0; slot < textures.size(); ++slot) {
        if (textures[slot].image != VK_NULL_HANDLE) {
            destroy(textures[slot]);
        }
        // Every handle there was is caught, and the slots are free for the next start.
        if (live[slot]) {
            ++generations[slot];
        }
        live[slot] = false;
    }
    textures.clear();
    live.clear();
    for (const auto& [slot, kept] : copies()) {
        ++stamp_of(slot);
    }
    copies().clear();
    written_copies = 0;
    free_slots.clear();
    // Slots and their generations are kept, so handles from before never come back.
    for (auto slot = static_cast<std::uint32_t>(generations.size()); slot-- > 0;) {
        if (generations[slot] < (1U << 24U)) {
            free_slots.push_back(slot);
        }
    }
    textures.resize(generations.size());
    live.resize(generations.size(), false);
    textures_made = 0;
}

} // namespace fbe::gpu

extern "C" {

FBE_API uint64_t fbe_texture_create(int32_t width, int32_t height, int32_t format, bool mipmaps) {
    if (!checked(width, height, format)) {
        return 0;
    }
    return create({
        .width = static_cast<std::uint32_t>(width),
        .height = static_cast<std::uint32_t>(height),
        .format = format,
        .mipmaps = mipmaps,
    });
}

FBE_API uint64_t fbe_texture_create_target(int32_t width, int32_t height, int32_t format,
                                           int32_t samples, bool depth) {
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) {
        fbe::fail(std::format("a target draws 1, 2, 4, or 8 samples a pixel, not {}", samples));
        return 0;
    }
    if (!checked(width, height, format)) {
        return 0;
    }
    return create({
        .width = static_cast<std::uint32_t>(width),
        .height = static_cast<std::uint32_t>(height),
        .format = format,
        .target = true,
        .samples = static_cast<VkSampleCountFlagBits>(samples),
        .depth = depth,
    });
}

FBE_API bool fbe_texture_is_target(uint64_t texture) {
    const texture_t* found = fbe::texture::find(texture);
    return found != nullptr && found->target;
}

FBE_API int32_t fbe_texture_samples(uint64_t texture) {
    const texture_t* found = fbe::texture::find(texture);
    return found != nullptr ? static_cast<std::int32_t>(found->samples) : -1;
}

FBE_API bool fbe_texture_has_depth(uint64_t texture) {
    const texture_t* found = fbe::texture::find(texture);
    return found != nullptr && found->depth != VK_NULL_HANDLE;
}

FBE_API bool fbe_texture_clear(uint64_t texture, float r, float g, float b, float a) {
    const texture_t* found = fbe::texture::find(texture);
    if (found == nullptr || !has_pixels(*found)) {
        return false;
    }
    const fbe::gpu::resource_t cleared = fbe::gpu::texture_resource(texture);
    if (cleared == fbe::gpu::no_resource) {
        return false;
    }
    // A target's is a pass that draws nothing, its depth cleared to the farthest.
    if (found->target) {
        return fbe::gpu::add_pass({
            .name = "clear",
            .colors = { { .target = cleared, .clear = true, .rgba = { r, g, b, a } } },
            .depth = found->depth != VK_NULL_HANDLE ? cleared : fbe::gpu::no_resource,
            .clear_depth = true,
        });
    }
    return fbe::gpu::add_pass({
        .name = "clear",
        .uses = { { cleared, fbe::gpu::use_t::COPY_TO } },
        .record =
            [image = found->image, levels = found->levels,
             color = VkClearColorValue{ .float32 = { r, g, b, a } }](VkCommandBuffer commands) {
                const VkImageSubresourceRange everything{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = levels,
                    .layerCount = 1,
                };
                vkCmdClearColorImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color,
                                     1, &everything);
            },
    });
}

FBE_API bool fbe_texture_copy(uint64_t to, int32_t x, int32_t y, uint64_t from, int32_t from_x,
                              int32_t from_y, int32_t width, int32_t height) {
    texture_t* target = fbe::texture::find(to);
    const texture_t* source = target != nullptr ? fbe::texture::find(from) : nullptr;
    if (target == nullptr || source == nullptr || !has_pixels(*target) || !has_pixels(*source)) {
        return false;
    }
    if (target->msaa != VK_NULL_HANDLE) {
        return fbe::fail("a multisampled target is drawn into, not copied into");
    }
    if (fbe::texture::pixel_size(target->format) != fbe::texture::pixel_size(source->format)) {
        return fbe::fail("textures of formats of different sizes aren't copied between");
    }
    if (!inside(*target, x, y, width, height) || !inside(*source, from_x, from_y, width, height)) {
        return false;
    }
    const fbe::gpu::resource_t into = fbe::gpu::texture_resource(to);
    const fbe::gpu::resource_t out_of = fbe::gpu::texture_resource(from);
    if (into == fbe::gpu::no_resource || out_of == fbe::gpu::no_resource) {
        return false;
    }
    const VkExtent3D extent{
        .width = static_cast<std::uint32_t>(width),
        .height = static_cast<std::uint32_t>(height),
        .depth = 1,
    };
    const VkImageCopy region{
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .srcOffset = { .x = from_x, .y = from_y },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstOffset = { .x = x, .y = y },
        .extent = extent,
    };
    if (to == from) {
        return copy_within(*target, into, region);
    }
    return fbe::gpu::add_pass({
        .name = "copy",
        .uses = { { out_of, fbe::gpu::use_t::COPY_FROM }, { into, fbe::gpu::use_t::MIPMAPPED } },
        .record =
            [source = source->image, copied = *target, region](VkCommandBuffer commands) {
                vkCmdCopyImage(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copied.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                record_mipmaps(commands, copied);
            },
    });
}

FBE_API uint8_t* fbe_texture_pixels(uint64_t texture, int64_t* size) {
    const texture_t* found = fbe::texture::find(texture);
    if (found == nullptr || !has_pixels(*found)) {
        return nullptr;
    }
    if (found->msaa != VK_NULL_HANDLE) {
        fbe::fail("a multisampled target's pixels are only drawn");
        return nullptr;
    }
    // A 2D batch drawing into it goes to the graph first, so its version counts it.
    fbe::gpu::in_graph(texture);
    found = fbe::texture::find(texture);
    const auto slot = static_cast<std::uint32_t>(texture);
    cpu_copy_t& kept = copies()[slot];
    if (kept.version != found->version) {
        // Nothing's written into a kept older than the texture: what uses it on the GPU sends the
        // kept's writes first. Read back into where it is, so it stays there.
        std::vector<std::uint8_t> read = fbe::texture::read(texture, 0);
        if (read.empty()) {
            drop_copy(slot);
            return nullptr;
        }
        if (kept.pixels.size() == read.size()) {
            std::memcpy(kept.pixels.data(), read.data(), read.size());
        } else {
            kept.pixels = std::move(read);
        }
        found = fbe::texture::find(texture);
        kept.version = found->version;
        ++readbacks;
    }
    if (size != nullptr) {
        *size = static_cast<std::int64_t>(kept.pixels.size());
    }
    return kept.pixels.data();
}

FBE_API const uint64_t* fbe_texture_pixels_stamp(uint64_t texture) {
    if (fbe::texture::find(texture) == nullptr) {
        return nullptr;
    }
    return &stamp_of(static_cast<std::uint32_t>(texture));
}

FBE_API bool fbe_texture_pixels_written(uint64_t texture, int32_t x, int32_t y, int32_t width,
                                        int32_t height) {
    // A 2D batch drawing into it goes to the graph first, so what it drew before is older, and
    // its version counts it.
    if (fbe::texture::find(texture) == nullptr) {
        return false;
    }
    fbe::gpu::in_graph(texture);
    const texture_t* found = fbe::texture::find(texture);
    if (found == nullptr) {
        return false;
    }
    const auto kept = copies().find(static_cast<std::uint32_t>(texture));
    if (kept == copies().end() || kept->second.pixels.empty()) {
        return fbe::fail(
            "the texture's pixels weren't on the CPU, as fbe_texture_pixels puts them");
    }
    if (!inside(*found, x, y, width, height)) {
        return false;
    }
    // Sent, the rectangle would go over what the GPU's done since with what the copy had before.
    if (kept->second.version != found->version) {
        return fbe::fail("the GPU changed the texture after its pixels came to the CPU, so "
                         "they're put there again to be written");
    }
    cpu_copy_t& written = kept->second;
    if (written.right <= written.left) {
        written.left = x;
        written.top = y;
        written.right = x + width;
        written.bottom = y + height;
        ++written_copies;
    } else {
        written.left = std::min(written.left, x);
        written.top = std::min(written.top, y);
        written.right = std::max(written.right, x + width);
        written.bottom = std::max(written.bottom, y + height);
    }
    return true;
}

FBE_API bool fbe_texture_read(uint64_t texture, int32_t level, uint8_t* pixels, int64_t size) {
    const texture_t* found = fbe::texture::find(texture);
    if (found == nullptr) {
        return false;
    }
    if (level < 0) {
        return fbe::fail(std::format("the texture has {} levels, not {}", found->levels, level));
    }
    const std::vector<std::uint8_t> read =
        fbe::texture::read(texture, static_cast<std::uint32_t>(level));
    if (read.empty()) {
        return false;
    }
    if (pixels == nullptr || std::cmp_not_equal(size, read.size())) {
        return fbe::fail(std::format("the level's {} bytes, not {}", read.size(), size));
    }
    std::memcpy(pixels, read.data(), read.size());
    return true;
}

FBE_API bool fbe_texture_retain(uint64_t texture) {
    texture_t* found = fbe::texture::find(texture);
    if (found == nullptr) {
        return false;
    }
    ++found->references;
    return true;
}

FBE_API bool fbe_texture_release(uint64_t texture) {
    const std::uint32_t slot = slot_of(texture);
    if (slot == UINT32_MAX) {
        return false;
    }
    if (--textures[slot].references > 0) {
        return true;
    }
    // Its handles are caught now; its image and slot wait for the frames that could draw it. What
    // was written into its CPU copy and not sent goes with it.
    drop_copy(slot);
    live[slot] = false;
    ++generations[slot];
    textures_made -= textures[slot].engine ? 0 : 1;
    fbe::gpu::after_frame([slot] {
        fbe::gpu::write_texture(slot, textures[fbe::texture::white].view);
        destroy(textures[slot]);
        if (generations[slot] < (1U << 24U)) {
            free_slots.push_back(slot);
        }
    });
    return true;
}

FBE_API bool fbe_texture_write_floats(uint64_t texture, int32_t x, int32_t y, int32_t width,
                                      int32_t height, const float* pixels) {
    const texture_t* found = fbe::texture::find(texture);
    if (found == nullptr) {
        return false;
    }
    if (found->format != FBE_TEXTURE_RGBA16F) {
        return fbe::fail("floats are written into an RGBA16F texture");
    }
    if (!inside(*found, x, y, width, height)) {
        return false;
    }
    if (pixels == nullptr) {
        return fbe::fail("there are no pixels to write");
    }
    std::vector<std::uint16_t> halves(static_cast<std::size_t>(width) *
                                      static_cast<std::size_t>(height) * 4);
    for (std::size_t at = 0; at < halves.size(); ++at) {
        halves[at] = half_of(pixels[at]);
    }
    std::vector<std::uint8_t> bytes(halves.size() * sizeof(std::uint16_t));
    std::memcpy(bytes.data(), halves.data(), bytes.size());
    return fbe_texture_write(texture, x, y, width, height, bytes.data(), width * 8);
}

FBE_API int32_t fbe_texture_width(uint64_t texture) {
    const texture_t* found = fbe::texture::find(texture);
    return found != nullptr ? static_cast<std::int32_t>(found->width) : -1;
}

FBE_API int32_t fbe_texture_height(uint64_t texture) {
    const texture_t* found = fbe::texture::find(texture);
    return found != nullptr ? static_cast<std::int32_t>(found->height) : -1;
}

FBE_API int32_t fbe_texture_format(uint64_t texture) {
    const texture_t* found = fbe::texture::find(texture);
    return found != nullptr ? found->format : -1;
}

FBE_API int32_t fbe_texture_levels(uint64_t texture) {
    const texture_t* found = fbe::texture::find(texture);
    return found != nullptr ? static_cast<std::int32_t>(found->levels) : -1;
}

FBE_API bool fbe_texture_srgb(uint64_t texture) {
    const texture_t* found = fbe::texture::find(texture);
    if (found == nullptr) {
        return false;
    }
    switch (found->vk_format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
    case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        return true;
    default:
        return false;
    }
}

FBE_API bool fbe_texture_write(uint64_t texture, int32_t x, int32_t y, int32_t width,
                               int32_t height, const uint8_t* pixels, int32_t pitch) {
    texture_t* found = fbe::texture::find(texture);
    if (found == nullptr || !has_pixels(*found)) {
        return false;
    }
    if (found->msaa != VK_NULL_HANDLE) {
        return fbe::fail("a multisampled target is drawn into, not written");
    }
    if (!inside(*found, x, y, width, height)) {
        return false;
    }
    const std::int64_t row = std::int64_t{ width } * fbe::texture::pixel_size(found->format);
    if (pixels == nullptr || pitch < row) {
        return fbe::fail(std::format("a row of {} pixels is {} bytes, more than the pitch {}",
                                     width, row, pitch));
    }
    // What the CPU wrote into its copy goes first, so this, written after, goes over it.
    fbe::texture::send_pixels(texture);
    found = fbe::texture::find(texture);
    if (found == nullptr) {
        return false;
    }
    return upload(texture, *found, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                  static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), pixels,
                  static_cast<std::uint32_t>(pitch));
}

FBE_API bool fbe_texture_write_level(uint64_t texture, int32_t level, int32_t width, int32_t height,
                                     const uint8_t* pixels, int32_t pitch) {
    texture_t* found = fbe::texture::find(texture);
    if (found == nullptr || !has_pixels(*found)) {
        return false;
    }
    if (found->msaa != VK_NULL_HANDLE) {
        return fbe::fail("a multisampled target is drawn into, not written");
    }
    if (level < 0 || std::cmp_greater_equal(level, found->levels)) {
        return fbe::fail(
            std::format("the texture has {} levels, and no level {}", found->levels, level));
    }
    const std::uint32_t level_width =
        std::max(found->width >> static_cast<std::uint32_t>(level), 1U);
    const std::uint32_t level_height =
        std::max(found->height >> static_cast<std::uint32_t>(level), 1U);
    if (std::int64_t{ width } != level_width || std::int64_t{ height } != level_height) {
        return fbe::fail(std::format("level {} of a {}x{} texture is {}x{}, not {}x{}", level,
                                     found->width, found->height, level_width, level_height, width,
                                     height));
    }
    const std::int64_t row = std::int64_t{ width } * fbe::texture::pixel_size(found->format);
    if (pixels == nullptr || pitch < row) {
        return fbe::fail(std::format("a row of {} pixels is {} bytes, more than the pitch {}",
                                     width, row, pitch));
    }
    // What the CPU wrote into its copy goes first, making the levels from it, so this goes over.
    fbe::texture::send_pixels(texture);
    found = fbe::texture::find(texture);
    if (found == nullptr) {
        return false;
    }
    return upload_level(texture, *found, static_cast<std::uint32_t>(level), level_width,
                        level_height, pixels, static_cast<std::uint32_t>(pitch));
}

FBE_API bool fbe_texture_set_alpha(uint64_t texture, bool alpha) {
    texture_t* found = fbe::texture::find(texture);
    if (found == nullptr) {
        return false;
    }
    found->alpha = alpha;
    return true;
}

FBE_API bool fbe_texture_set_path(uint64_t texture, const char* path) {
    texture_t* found = fbe::texture::find(texture);
    if (found == nullptr || path == nullptr) {
        return false;
    }
    found->path = path;
    return true;
}

FBE_API const char* fbe_texture_path(uint64_t texture) {
    const texture_t* found = fbe::texture::find(texture);
    return found != nullptr ? found->path.c_str() : "";
}

FBE_API bool fbe_texture_alpha(uint64_t texture) {
    const texture_t* found = fbe::texture::find(texture);
    return found != nullptr && found->alpha;
}

FBE_API bool fbe_texture_set_filter(uint64_t texture, int32_t filter) {
    texture_t* found = fbe::texture::find(texture);
    if (found == nullptr) {
        return false;
    }
    if (!valid_filter(filter)) {
        return fbe::fail(std::format("{} isn't a TextureFilter", filter));
    }
    found->filter = filter;
    return true;
}

FBE_API int32_t fbe_texture_filter(uint64_t texture) {
    const texture_t* found = fbe::texture::find(texture);
    return found != nullptr ? found->filter : -1;
}

FBE_API bool fbe_texture_set_wrap(uint64_t texture, int32_t wrap_u, int32_t wrap_v) {
    texture_t* found = fbe::texture::find(texture);
    if (found == nullptr) {
        return false;
    }
    if (!valid_wrap(wrap_u) || !valid_wrap(wrap_v)) {
        return fbe::fail(std::format("{} and {} aren't both TextureWraps", wrap_u, wrap_v));
    }
    found->wrap_u = wrap_u;
    found->wrap_v = wrap_v;
    return true;
}

FBE_API int32_t fbe_texture_wrap_u(uint64_t texture) {
    const texture_t* found = fbe::texture::find(texture);
    return found != nullptr ? found->wrap_u : -1;
}

FBE_API int32_t fbe_texture_wrap_v(uint64_t texture) {
    const texture_t* found = fbe::texture::find(texture);
    return found != nullptr ? found->wrap_v : -1;
}

FBE_API int32_t fbe_texture_count(void) {
    return textures_made;
}

FBE_API bool fbe_texture_start_loads(void) {
    return fbe::gpu::start_with_window();
}

FBE_API uint64_t fbe_texture_from_load(uint64_t load, bool srgb, bool mipmaps) {
    const fbe::assets::image_t* image = image_of(load);
    if (image == nullptr) {
        return 0;
    }
    const std::optional<spec_t> spec = spec_of(*image, srgb, mipmaps);
    return spec ? create(*spec) : 0;
}

FBE_API uint64_t fbe_texture_from_model(uint64_t load, int32_t index, bool srgb, bool mipmaps) {
    load_invalid = false;
    const fbe::assets::load_t* found = fbe::assets::loads().get(load);
    if (found == nullptr || !found->done || found->error ||
        found->kind != fbe::assets::load_kind_t::MODEL) {
        fbe::fail("the load isn't a model load done and loaded");
        return 0;
    }
    if (index < 0 || std::cmp_greater_equal(index, found->model.images.size())) {
        fbe::fail(std::format("the model has {} images; there's no image {}",
                              found->model.images.size(), index));
        return 0;
    }
    const std::optional<spec_t> spec =
        spec_of(found->model.images[static_cast<std::size_t>(index)], srgb, mipmaps);
    return spec ? create(*spec) : 0;
}

FBE_API bool fbe_texture_reload(uint64_t texture, uint64_t load, bool srgb, bool mipmaps) {
    const texture_t* reloaded = fbe::texture::find(texture);
    if (reloaded == nullptr) {
        return false;
    }
    if (reloaded->target) {
        return fbe::fail("a target is drawn into, not reloaded");
    }
    const fbe::assets::image_t* image = image_of(load);
    if (image == nullptr) {
        return false;
    }
    std::optional<spec_t> spec = spec_of(*image, srgb, mipmaps);
    if (!spec) {
        return false;
    }
    // Frames in flight draw with its slot's view, whose descriptor can't change under them, and a
    // slot goes on being drawn from, so nothing's waited for but all of them.
    if (!fbe::gpu::finish_frame()) {
        return false;
    }
    const VkResult idle = vkDeviceWaitIdle(fbe::gpu::device()->device);
    if (idle != VK_SUCCESS) {
        return fbe::gpu::fail_vulkan("the GPU couldn't finish", idle);
    }
    // Finishing the frame can make transient targets' images.
    if (fbe::texture::find(texture) == nullptr) {
        return false;
    }
    // It's made in a slot of its own, past the others, which may be the bindless set's last, so
    // it doesn't go in the set, and it's gone again below.
    const auto scratch = static_cast<std::uint32_t>(textures.size());
    textures.emplace_back();
    generations.push_back(1);
    live.push_back(false);
    const auto give_back = [] {
        textures.pop_back();
        generations.pop_back();
        live.pop_back();
    };
    spec->engine = true;
    spec->bound = false;
    if (!make(scratch, *spec)) {
        give_back();
        return false;
    }
    const auto slot = static_cast<std::uint32_t>(texture);
    texture_t& held = textures[slot];
    texture_t& made = textures[scratch];
    made.filter = held.filter;
    made.wrap_u = held.wrap_u;
    made.wrap_v = held.wrap_v;
    made.references = held.references;
    made.version = held.version + 1;
    made.engine = held.engine;
    std::swap(held, made);
    fbe::gpu::write_texture(slot, held.view);
    destroy(made);
    give_back();
    drop_copy(slot);
    return true;
}

FBE_API bool fbe_texture_load_invalid(void) {
    return load_invalid;
}

FBE_API uint32_t fbe_texture_vulkan_format(uint64_t texture) {
    const texture_t* found = fbe::texture::find(texture);
    return found != nullptr ? static_cast<std::uint32_t>(found->vk_format) : 0;
}

} // extern "C"
