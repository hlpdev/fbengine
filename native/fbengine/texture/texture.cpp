// Textures: a pool of them by slot, each slot its index in the bindless set, with the white
// texture at 0; pixels uploaded through staging buffers in the frame's uploads, which free them
// once the GPU's done; mipmaps made by blitting each level from the one before.

#include "texture/texture.hpp"

#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/image.hpp"
#include "gpu/vulkan.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
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

/// The stages that read textures, which an upload's writes are made visible to.
constexpr auto readers =
    fbe::gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT) |
    fbe::gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) |
    fbe::gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
constexpr auto copy = fbe::gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COPY_BIT);
constexpr auto blit = fbe::gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_BLIT_BIT);
constexpr auto transfer =
    copy | blit | fbe::gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_CLEAR_BIT);
constexpr auto transfer_read = fbe::gpu::flag<VkAccessFlags2>(VK_ACCESS_2_TRANSFER_READ_BIT);
constexpr auto transfer_write = fbe::gpu::flag<VkAccessFlags2>(VK_ACCESS_2_TRANSFER_WRITE_BIT);
constexpr auto shader_read = fbe::gpu::flag<VkAccessFlags2>(VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

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

/// Destroys a texture's image now, which the GPU is done with.
void destroy(texture_t& texture) {
    const fbe::gpu::device_t& gpu = *fbe::gpu::device();
    vkDestroyImageView(gpu.device, texture.view, nullptr);
    vmaDestroyImage(gpu.allocator, texture.image, texture.memory);
    texture = {};
}

/// Makes a texture in `slot`: its image, transparent black, every level SHADER_READ_ONLY, and
/// its view in the bindless set; false, with fbe_error's why, if it can't.
bool make(std::uint32_t slot, std::uint32_t width, std::uint32_t height, std::int32_t format,
          bool mipmaps) {
    const fbe::gpu::device_t& gpu = *fbe::gpu::device();
    texture_t& texture = textures[slot];
    texture = {
        .format = format,
        .vk_format = vk_format_of(format),
        .width = width,
        .height = height,
        .levels =
            mipmaps ? static_cast<std::uint32_t>(std::bit_width(std::max(width, height))) : 1U,
        .filter = FBE_TEXTURE_LINEAR,
        .wrap_u = FBE_TEXTURE_REPEAT,
        .wrap_v = FBE_TEXTURE_REPEAT,
        .references = 1,
    };
    VkFormatProperties support{};
    vkGetPhysicalDeviceFormatProperties(gpu.physical, texture.vk_format, &support);
    const auto mipmappable =
        flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_BLIT_SRC_BIT) |
        flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_BLIT_DST_BIT) |
        flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    if (mipmaps && (support.optimalTilingFeatures & mipmappable) != mipmappable) {
        return fbe::fail("this GPU can't make that format's mipmaps");
    }
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = texture.vk_format,
        .extent = { .width = width, .height = height, .depth = 1 },
        .mipLevels = texture.levels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = flag<VkImageUsageFlags>(VK_IMAGE_USAGE_SAMPLED_BIT) |
                 flag<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_DST_BIT) |
                 flag<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
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

    VkCommandBuffer commands = fbe::gpu::upload_commands();
    const VkImageMemoryBarrier2 to_clear =
        levels_barrier(texture.image, 0, texture.levels, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, transfer, 0, transfer, transfer_write);
    fbe::gpu::barrier(commands, to_clear);
    const VkClearColorValue black{};
    const VkImageSubresourceRange everything{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .levelCount = texture.levels,
        .layerCount = 1,
    };
    vkCmdClearColorImage(commands, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                         &everything);
    fbe::gpu::barrier(commands, levels_barrier(texture.image, 0, texture.levels,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, transfer,
                                               transfer_write, readers, shader_read));
    fbe::gpu::write_texture(slot, texture.view);
    live[slot] = true;
    ++textures_made;
    return true;
}

/// Uploads `width` by `height` pixels at `x`, `y` into level 0, rows `pitch` bytes apart, through
/// a staging buffer freed after the frame, and makes the mipmaps again.
bool upload(texture_t& texture, std::uint32_t x, std::uint32_t y, std::uint32_t width,
            std::uint32_t height, const std::uint8_t* pixels, std::uint32_t pitch) {
    const fbe::gpu::device_t& gpu = *fbe::gpu::device();
    const std::uint32_t row = width * fbe::texture::pixel_size(texture.format);
    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize{ row } * height,
        .usage = flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_SRC_BIT),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo allocation_info{
        .flags =
            flag<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) |
            flag<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_MAPPED_BIT),
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation memory = VK_NULL_HANDLE;
    VmaAllocationInfo mapped{};
    const VkResult made =
        vmaCreateBuffer(gpu.allocator, &buffer_info, &allocation_info, &buffer, &memory, &mapped);
    if (made != VK_SUCCESS) {
        return fbe::gpu::fail_vulkan("couldn't make a buffer to upload a texture through", made);
    }
    auto* to = static_cast<std::uint8_t*>(mapped.pMappedData);
    for (std::uint32_t line = 0; line < height; ++line) {
        std::memcpy(to + (std::size_t{ line } * row), pixels + (std::size_t{ line } * pitch), row);
    }
    vmaFlushAllocation(gpu.allocator, memory, 0, VK_WHOLE_SIZE);
    fbe::gpu::after_frame(
        [buffer, memory] { vmaDestroyBuffer(fbe::gpu::device()->allocator, buffer, memory); });

    VkCommandBuffer commands = fbe::gpu::upload_commands();
    fbe::gpu::barrier(commands, levels_barrier(texture.image, 0, texture.levels,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, readers, 0,
                                               transfer, transfer_write));
    const VkBufferImageCopy region{
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageOffset = { .x = static_cast<std::int32_t>(x), .y = static_cast<std::int32_t>(y) },
        .imageExtent = { .width = width, .height = height, .depth = 1 },
    };
    vkCmdCopyBufferToImage(commands, buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);
    record_mipmaps(commands, texture);
    return true;
}

bool valid_filter(std::int32_t filter) {
    return filter == FBE_TEXTURE_NEAREST || filter == FBE_TEXTURE_LINEAR;
}

bool valid_wrap(std::int32_t wrap) {
    return wrap == FBE_TEXTURE_REPEAT || wrap == FBE_TEXTURE_CLAMP || wrap == FBE_TEXTURE_MIRROR;
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
    const texture_t* texture = find(handle);
    if (texture == nullptr) {
        return {};
    }
    if (level >= texture->levels) {
        fail(std::format("the texture has {} levels, not {}", texture->levels, level + 1));
        return {};
    }
    if (!gpu::finish_frame()) {
        return {};
    }
    const VkExtent2D extent{
        .width = std::max(texture->width >> level, 1U),
        .height = std::max(texture->height >> level, 1U),
    };
    return gpu::read_pixels(texture->image, extent, pixel_size(texture->format),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, level);
}

std::uint32_t pixel_size(std::int32_t format) {
    switch (format) {
    case FBE_TEXTURE_RGBA8:
    case FBE_TEXTURE_RGBA8_LINEAR:
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
    if (slot == UINT32_MAX || !make(slot, 1, 1, FBE_TEXTURE_RGBA8_LINEAR, false)) {
        return false;
    }
    --textures_made;
    const std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
    return upload(textures[slot], 0, 0, 1, 1, white.data(), 4);
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
    if (fbe::texture::pixel_size(format) == 0) {
        fbe::fail(std::format("{} isn't a TextureFormat", format));
        return 0;
    }
    if (!fbe::gpu::start_with_window()) {
        return 0;
    }
    const auto largest =
        static_cast<std::int32_t>(fbe::gpu::device()->properties.limits.maxImageDimension2D);
    if (width < 1 || height < 1 || width > largest || height > largest) {
        fbe::fail(std::format("a texture is 1 to {} pixels on a side, not {}x{}", largest, width,
                              height));
        return 0;
    }
    const std::uint32_t slot = take_slot();
    if (slot == UINT32_MAX) {
        return 0;
    }
    if (!make(slot, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), format,
              mipmaps)) {
        free_slots.push_back(slot);
        return 0;
    }
    return handle_of(slot);
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
    // Its handles are caught now; its image and slot wait for the frames that could draw it.
    live[slot] = false;
    ++generations[slot];
    --textures_made;
    fbe::gpu::after_frame([slot] {
        fbe::gpu::write_texture(slot, textures[fbe::texture::white].view);
        destroy(textures[slot]);
        if (generations[slot] < (1U << 24U)) {
            free_slots.push_back(slot);
        }
    });
    return true;
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

FBE_API bool fbe_texture_write(uint64_t texture, int32_t x, int32_t y, int32_t width,
                               int32_t height, const uint8_t* pixels, int32_t pitch) {
    texture_t* found = fbe::texture::find(texture);
    if (found == nullptr) {
        return false;
    }
    const auto across = static_cast<std::int64_t>(found->width);
    const auto down = static_cast<std::int64_t>(found->height);
    if (x < 0 || y < 0 || width < 1 || height < 1 || x + std::int64_t{ width } > across ||
        y + std::int64_t{ height } > down) {
        return fbe::fail(std::format("a {}x{} region at {}, {} isn't in a {}x{} texture", width,
                                     height, x, y, across, down));
    }
    const std::int64_t row = std::int64_t{ width } * fbe::texture::pixel_size(found->format);
    if (pixels == nullptr || pitch < row) {
        return fbe::fail(std::format("a row of {} pixels is {} bytes, more than the pitch {}",
                                     width, row, pitch));
    }
    return upload(*found, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                  static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), pixels,
                  static_cast<std::uint32_t>(pitch));
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

} // extern "C"
