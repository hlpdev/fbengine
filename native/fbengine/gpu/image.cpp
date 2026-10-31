// Images on the GPU, work run now on a pool of its own, and reading an image's pixels back.

#include "gpu/image.hpp"

#include "core/engine.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace {

using fbe::gpu::flag;

VkCommandPool pool = VK_NULL_HANDLE;
VkCommandBuffer commands = VK_NULL_HANDLE;
VkFence done = VK_NULL_HANDLE;

/// The pool, command buffer, and fence run_now uses, made the first time.
bool start_immediate(const fbe::gpu::device_t& gpu) {
    if (pool != VK_NULL_HANDLE) {
        return true;
    }
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = flag<VkCommandPoolCreateFlags>(VK_COMMAND_POOL_CREATE_TRANSIENT_BIT),
        .queueFamilyIndex = gpu.family,
    };
    VkResult result = vkCreateCommandPool(gpu.device, &pool_info, nullptr, &pool);
    if (result == VK_SUCCESS) {
        const VkCommandBufferAllocateInfo commands_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        result = vkAllocateCommandBuffers(gpu.device, &commands_info, &commands);
    }
    if (result == VK_SUCCESS) {
        const VkFenceCreateInfo fence_info{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        result = fbe::gpu::faulted(fbe::gpu::fault_t::WORK_NOW)
                     ? VK_ERROR_OUT_OF_HOST_MEMORY
                     : vkCreateFence(gpu.device, &fence_info, nullptr, &done);
    }
    if (result != VK_SUCCESS) {
        // What was made goes, so the next try starts again, leaking nothing.
        vkDestroyFence(gpu.device, done, nullptr);
        vkDestroyCommandPool(gpu.device, pool, nullptr);
        done = VK_NULL_HANDLE;
        pool = VK_NULL_HANDLE;
        commands = VK_NULL_HANDLE;
        return fbe::gpu::fail_vulkan("couldn't make the GPU's commands for work now", result);
    }
    return true;
}

} // namespace

namespace fbe::gpu {

image_t make_color_image(VkFormat format, VkExtent2D extent) {
    const device_t* gpu = device();
    if (gpu == nullptr) {
        fail("the GPU hasn't started");
        return {};
    }
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { .width = extent.width, .height = extent.height, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = flag<VkImageUsageFlags>(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                 flag<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_DST_BIT) |
                 flag<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo allocation{ .usage = VMA_MEMORY_USAGE_AUTO };
    image_t image{ .format = format, .extent = extent };
    VkResult result =
        vmaCreateImage(gpu->allocator, &info, &allocation, &image.image, &image.memory, nullptr);
    if (result != VK_SUCCESS) {
        fail_vulkan("couldn't make an image", result);
        return {};
    }
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    result = vkCreateImageView(gpu->device, &view_info, nullptr, &image.view);
    if (result != VK_SUCCESS) {
        fail_vulkan("couldn't make an image's view", result);
        destroy_image(image);
        return {};
    }
    return image;
}

void destroy_image(image_t& image) {
    const device_t* gpu = device();
    if (gpu != nullptr && image.image != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(gpu->device);
        vkDestroyImageView(gpu->device, image.view, nullptr);
        vmaDestroyImage(gpu->allocator, image.image, image.memory);
    }
    image = {};
}

void stop_immediate() {
    const device_t* gpu = device();
    vkDestroyFence(gpu->device, done, nullptr);
    vkDestroyCommandPool(gpu->device, pool, nullptr);
    done = VK_NULL_HANDLE;
    pool = VK_NULL_HANDLE;
    commands = VK_NULL_HANDLE;
}

bool run_now(const std::function<void(VkCommandBuffer)>& record) {
    const device_t* gpu = device();
    if (gpu == nullptr) {
        return fail("the GPU hasn't started");
    }
    if (lost()) {
        return fail_lost();
    }
    if (!start_immediate(*gpu)) {
        return false;
    }
    vkResetCommandPool(gpu->device, pool, 0);
    const VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = flag<VkCommandBufferUsageFlags>(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT),
    };
    vkBeginCommandBuffer(commands, &begin);
    record(commands);
    vkEndCommandBuffer(commands);
    const VkCommandBufferSubmitInfo submitted{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = commands,
    };
    const VkSubmitInfo2 submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &submitted,
    };
    VkResult result = gpu->queue_submit(gpu->queue, 1, &submit, done);
    if (result == VK_SUCCESS) {
        result = vkWaitForFences(gpu->device, 1, &done, VK_TRUE, UINT64_MAX);
    }
    vkResetFences(gpu->device, 1, &done);
    if (lose(result)) {
        return fail_lost();
    }
    return result == VK_SUCCESS ? true : fail_vulkan("the GPU couldn't run its work", result);
}

std::vector<std::uint8_t> read_pixels(const image_t& image, VkImageLayout layout) {
    return read_pixels(image.image, image.extent, 4, layout);
}

std::vector<std::uint8_t> read_pixels(VkImage image, VkExtent2D extent, std::uint32_t pixel_size,
                                      VkImageLayout layout, std::uint32_t level) {
    const device_t* gpu = device();
    if (gpu == nullptr) {
        fail("the GPU hasn't started");
        return {};
    }
    const VkDeviceSize size = VkDeviceSize{ extent.width } * extent.height * pixel_size;
    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_DST_BIT),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo allocation_info{
        .flags = flag<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT) |
                 flag<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_MAPPED_BIT),
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation memory = VK_NULL_HANDLE;
    VmaAllocationInfo mapped{};
    const VkResult made =
        vmaCreateBuffer(gpu->allocator, &buffer_info, &allocation_info, &buffer, &memory, &mapped);
    if (made != VK_SUCCESS) {
        fail_vulkan("couldn't make a buffer to read an image back through", made);
        return {};
    }

    const auto all = flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    const auto any = flag<VkAccessFlags2>(VK_ACCESS_2_MEMORY_WRITE_BIT) |
                     flag<VkAccessFlags2>(VK_ACCESS_2_MEMORY_READ_BIT);
    const auto copy = flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COPY_BIT);
    const auto copy_read = flag<VkAccessFlags2>(VK_ACCESS_2_TRANSFER_READ_BIT);
    const bool ran = run_now([&](VkCommandBuffer recording) {
        VkImageMemoryBarrier2 to_copy = color_transition(
            image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, all, any, copy, copy_read);
        to_copy.subresourceRange.baseMipLevel = level;
        barrier(recording, to_copy);
        const VkBufferImageCopy region{
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = level,
                .layerCount = 1,
            },
            .imageExtent = { .width = extent.width, .height = extent.height, .depth = 1 },
        };
        vkCmdCopyImageToBuffer(recording, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                               &region);
        VkImageMemoryBarrier2 back = color_transition(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                      layout, copy, 0, all, any);
        back.subresourceRange.baseMipLevel = level;
        barrier(recording, back);
        // The host reads the buffer once the copy's done.
        const VkMemoryBarrier2 to_host{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = copy,
            .srcAccessMask = flag<VkAccessFlags2>(VK_ACCESS_2_TRANSFER_WRITE_BIT),
            .dstStageMask = flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_HOST_BIT),
            .dstAccessMask = flag<VkAccessFlags2>(VK_ACCESS_2_HOST_READ_BIT),
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &to_host,
        };
        gpu->pipeline_barrier(recording, &dependency);
    });
    std::vector<std::uint8_t> pixels;
    if (ran) {
        vmaInvalidateAllocation(gpu->allocator, memory, 0, VK_WHOLE_SIZE);
        pixels.resize(static_cast<std::size_t>(size));
        std::memcpy(pixels.data(), mapped.pMappedData, pixels.size());
    }
    vmaDestroyBuffer(gpu->allocator, buffer, memory);
    return pixels;
}

VkImageMemoryBarrier2 color_transition(VkImage image, VkImageLayout from, VkImageLayout to,
                                       VkPipelineStageFlags2 before_stages, VkAccessFlags2 before,
                                       VkPipelineStageFlags2 after_stages, VkAccessFlags2 after) {
    return {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = before_stages,
        .srcAccessMask = before,
        .dstStageMask = after_stages,
        .dstAccessMask = after,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
}

void barrier(VkCommandBuffer commands, const VkImageMemoryBarrier2& image) {
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &image,
    };
    device()->pipeline_barrier(commands, &dependency);
}

} // namespace fbe::gpu
