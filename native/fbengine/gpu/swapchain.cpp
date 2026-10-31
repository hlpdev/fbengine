// The window's images and Flip's present. A shown window has a surface and a swapchain, made
// again whenever the window's size in pixels or vsync changes, or the surface says it's out of
// date; each frame in flight acquires one of its images, draws into it, and presents it. A headless
// window has one image of its own, its size in pixels, which each frame draws into the same way
// and leaves, as nothing shows it: Vulkan's headless surfaces aren't on every driver (Windows'
// have none, and lavapipe's need udmabuf), so headless frames never depend on them.

#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/gpu.hpp"
#include "gpu/vulkan.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vector>

namespace {

using fbe::gpu::flag;

/// What a headless window's image is, as a swapchain's mostly are.
constexpr VkFormat headless_format = VK_FORMAT_B8G8R8A8_UNORM;

SDL_Window* window = nullptr;
/// Whether the window is shown, not headless.
bool shown = false;
VkSurfaceKHR surface = VK_NULL_HANDLE;
VkSwapchainKHR swapchain = VK_NULL_HANDLE;
/// A headless window's image, and its memory.
VkImage headless = VK_NULL_HANDLE;
VmaAllocation headless_memory = VK_NULL_HANDLE;
VkExtent2D extent{};
/// The window's size in pixels when its images were made, which a surface's size can lag.
VkExtent2D made_for{};
bool vsync = true;
/// Whether the surface said the swapchain no longer matches it.
bool stale = false;
std::vector<VkImage> images;
std::vector<VkImageView> views;
/// Signaled when an image's frame has been drawn, which its present waits for: one an image, as
/// an image's semaphore is free again only once it's acquired again.
std::vector<VkSemaphore> rendered;
std::int64_t presented = 0;

/// The window's size in pixels; 0 by 0 when it's minimized, as some systems say.
VkExtent2D window_pixels() {
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    return {
        .width = static_cast<std::uint32_t>(std::max(width, 0)),
        .height = static_cast<std::uint32_t>(std::max(height, 0)),
    };
}

/// Destroys the window's images, waiting for the GPU to be done with them, but not the swapchain,
/// which its next one is made from.
void destroy_images(const fbe::gpu::device_t& gpu) {
    vkDeviceWaitIdle(gpu.device);
    for (VkImageView view : views) {
        vkDestroyImageView(gpu.device, view, nullptr);
    }
    for (VkSemaphore semaphore : rendered) {
        vkDestroySemaphore(gpu.device, semaphore, nullptr);
    }
    if (headless != VK_NULL_HANDLE) {
        vmaDestroyImage(gpu.allocator, headless, headless_memory);
        headless = VK_NULL_HANDLE;
        headless_memory = VK_NULL_HANDLE;
    }
    views.clear();
    rendered.clear();
    images.clear();
    extent = {};
}

/// A view of each of the images, of `format`; false, with fbe_error's why, if one can't be made.
bool make_views(const fbe::gpu::device_t& gpu, VkFormat format) {
    for (VkImage image : images) {
        const VkImageViewCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        VkImageView view = VK_NULL_HANDLE;
        const VkResult result = vkCreateImageView(gpu.device, &info, nullptr, &view);
        if (result != VK_SUCCESS) {
            return fbe::gpu::fail_vulkan("couldn't make the window's views", result);
        }
        views.push_back(view);
    }
    return true;
}

/// The present mode for `wait`: FIFO, which every device has, waits for the display; without it,
/// mailbox, which never tears, or else immediate.
VkPresentModeKHR present_mode(const fbe::gpu::device_t& gpu, bool wait) {
    if (wait) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu.physical, surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu.physical, surface, &count, modes.data());
    for (const VkPresentModeKHR wanted : {
             VK_PRESENT_MODE_MAILBOX_KHR,
             VK_PRESENT_MODE_IMMEDIATE_KHR,
         }) {
        if (std::ranges::find(modes, wanted) != modes.end()) {
            return wanted;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

/// The surface's format: 8-bit BGRA or RGBA, written as they're stored, for what fbengine draws
/// is already in the display's sRGB; or else its first.
VkSurfaceFormatKHR surface_format(const fbe::gpu::device_t& gpu) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physical, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physical, surface, &count, formats.data());
    for (const VkFormat wanted : { headless_format, VK_FORMAT_R8G8B8A8_UNORM }) {
        for (const VkSurfaceFormatKHR& found : formats) {
            if (found.format == wanted && found.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return found;
            }
        }
    }
    return formats.empty() ? VkSurfaceFormatKHR{} : formats.front();
}

/// The surface's composite alpha: opaque, or else the first it has.
VkCompositeAlphaFlagBitsKHR composite_alpha(const VkSurfaceCapabilitiesKHR& capabilities) {
    for (const VkCompositeAlphaFlagBitsKHR alpha : {
             VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
             VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
             VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
             VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
         }) {
        if ((capabilities.supportedCompositeAlpha & flag<VkCompositeAlphaFlagsKHR>(alpha)) != 0) {
            return alpha;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

/// Makes the swapchain again for the window's size and `wait`, or none while the window has no
/// pixels; false, with fbe_error's why, if it can't.
bool make_swapchain(const fbe::gpu::device_t& gpu, bool wait) {
    VkSurfaceCapabilitiesKHR capabilities{
        .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
    };
    VkResult result =
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu.physical, surface, &capabilities);
    if (result != VK_SUCCESS) {
        return fbe::gpu::fail_vulkan("couldn't read the window's surface", result);
    }
    // A surface that doesn't say its size, as Wayland's don't, takes the window's.
    VkExtent2D size = capabilities.currentExtent;
    if (size.width == UINT32_MAX) {
        const VkExtent2D pixels = window_pixels();
        size.width = std::clamp(pixels.width, capabilities.minImageExtent.width,
                                capabilities.maxImageExtent.width);
        size.height = std::clamp(pixels.height, capabilities.minImageExtent.height,
                                 capabilities.maxImageExtent.height);
    }

    destroy_images(gpu);
    made_for = window_pixels();
    VkSwapchainKHR old = swapchain;
    swapchain = VK_NULL_HANDLE;
    stale = false;
    vsync = wait;
    if (size.width == 0 || size.height == 0) {
        vkDestroySwapchainKHR(gpu.device, old, nullptr);
        return true;
    }

    const VkSurfaceFormatKHR chosen = surface_format(gpu);
    std::uint32_t count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount != 0) {
        count = std::min(count, capabilities.maxImageCount);
    }
    // Drawn into, and copied into and out of, where the surface lets it.
    auto usage = flag<VkImageUsageFlags>(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    for (const VkImageUsageFlagBits extra :
         { VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT }) {
        usage |= capabilities.supportedUsageFlags & flag<VkImageUsageFlags>(extra);
    }
    const VkSwapchainCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = count,
        .imageFormat = chosen.format,
        .imageColorSpace = chosen.colorSpace,
        .imageExtent = size,
        .imageArrayLayers = 1,
        .imageUsage = usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = composite_alpha(capabilities),
        .presentMode = present_mode(gpu, wait),
        .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    result = vkCreateSwapchainKHR(gpu.device, &info, nullptr, &swapchain);
    vkDestroySwapchainKHR(gpu.device, old, nullptr);
    if (result != VK_SUCCESS) {
        swapchain = VK_NULL_HANDLE;
        return fbe::gpu::fail_vulkan("couldn't make the window's swapchain", result);
    }

    vkGetSwapchainImagesKHR(gpu.device, swapchain, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(gpu.device, swapchain, &count, images.data());
    if (!make_views(gpu, chosen.format)) {
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const VkSemaphoreCreateInfo semaphore_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        VkSemaphore semaphore = VK_NULL_HANDLE;
        result = vkCreateSemaphore(gpu.device, &semaphore_info, nullptr, &semaphore);
        if (result != VK_SUCCESS) {
            return fbe::gpu::fail_vulkan("couldn't make the swapchain's semaphores", result);
        }
        rendered.push_back(semaphore);
    }
    extent = size;
    return true;
}

/// Makes a headless window's image again for its size, or none while it has no pixels, and
/// keeps `wait`, which nothing waits for; false, with fbe_error's why, if it can't.
bool make_headless(const fbe::gpu::device_t& gpu, bool wait) {
    destroy_images(gpu);
    made_for = window_pixels();
    vsync = wait;
    if (made_for.width == 0 || made_for.height == 0) {
        return true;
    }
    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = headless_format,
        .extent = { .width = made_for.width, .height = made_for.height, .depth = 1 },
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
    const VkResult result = vmaCreateImage(gpu.allocator, &image_info, &allocation, &headless,
                                           &headless_memory, nullptr);
    if (result != VK_SUCCESS) {
        headless = VK_NULL_HANDLE;
        return fbe::gpu::fail_vulkan("couldn't make the headless window's image", result);
    }
    images.push_back(headless);
    if (!make_views(gpu, headless_format)) {
        return false;
    }
    extent = made_for;
    return true;
}

/// Makes the window's images again: its swapchain's, or a headless one's.
bool make_images(const fbe::gpu::device_t& gpu, bool wait) {
    return surface != VK_NULL_HANDLE ? make_swapchain(gpu, wait) : make_headless(gpu, wait);
}

/// A shown window's surface, if it hasn't one yet.
bool create_surface(VkInstance vulkan) {
    if (window == nullptr || !shown || surface != VK_NULL_HANDLE) {
        return true;
    }
    if (!SDL_Vulkan_CreateSurface(window, vulkan, nullptr, &surface)) {
        surface = VK_NULL_HANDLE;
        return fbe::fail_sdl("couldn't make the window's Vulkan surface");
    }
    return true;
}

/// The window's surface, if it's shown, and its images, on the started GPU.
bool make_window(const fbe::gpu::device_t& gpu) {
    if (!create_surface(gpu.instance)) {
        return false;
    }
    if (surface != VK_NULL_HANDLE) {
        VkBool32 presents = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu.physical, gpu.family, surface, &presents);
        if (presents != VK_TRUE) {
            return fbe::fail(
                std::format("the GPU {} can't present to the window", gpu.properties.deviceName));
        }
    }
    return make_images(gpu, vsync);
}

void destroy_window() {
    const fbe::gpu::device_t* gpu = fbe::gpu::device();
    if (gpu != nullptr) {
        destroy_images(*gpu);
        vkDestroySwapchainKHR(gpu->device, swapchain, nullptr);
    }
    swapchain = VK_NULL_HANDLE;
    if (surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(fbe::gpu::instance(), surface, nullptr);
        surface = VK_NULL_HANDLE;
    }
}

/// The barrier that moves one of the window's images from `from` to `to`.
VkImageMemoryBarrier2 transition(VkImage image, VkImageLayout from, VkImageLayout to,
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

void barrier(const fbe::gpu::device_t& gpu, VkCommandBuffer commands,
             const VkImageMemoryBarrier2& image) {
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &image,
    };
    gpu.pipeline_barrier(commands, &dependency);
}

/// Records the frame's commands into the window's image `index`: cleared, then left to be
/// presented, or, a headless window's, to be copied from. A headless window's one image is drawn
/// by each frame in flight, which the first barrier orders after the frame before's copies.
void record(const fbe::gpu::device_t& gpu, VkCommandBuffer commands, std::uint32_t index) {
    const VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = flag<VkCommandBufferUsageFlags>(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT),
    };
    vkBeginCommandBuffer(commands, &begin);
    const auto output =
        flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    const auto write = flag<VkAccessFlags2>(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    const auto copy = flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COPY_BIT);
    const auto copy_read = flag<VkAccessFlags2>(VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(gpu, commands,
            transition(images[index], VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, output | copy, 0, output, write));
    const VkRenderingAttachmentInfo color{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = views[index],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { .float32 = { 0.0F, 0.0F, 0.0F, 1.0F } } },
    };
    const VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { .extent = extent },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    gpu.begin_rendering(commands, &rendering);
    gpu.end_rendering(commands);
    if (swapchain != VK_NULL_HANDLE) {
        barrier(gpu, commands,
                transition(images[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, output, write,
                           flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_NONE), 0));
    } else {
        barrier(gpu, commands,
                transition(images[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output, write, copy, copy_read));
    }
    vkEndCommandBuffer(commands);
}

/// Submits the frame's commands, waiting for the swapchain's image and signaling its present, if
/// there's a swapchain.
VkResult submit(const fbe::gpu::device_t& gpu, const fbe::gpu::frame_t& frame,
                std::uint32_t index) {
    const VkSemaphoreSubmitInfo wait_for{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = frame.acquired,
        .stageMask = flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT),
    };
    const bool presents = swapchain != VK_NULL_HANDLE;
    const VkSemaphoreSubmitInfo signal{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = presents ? rendered[index] : VK_NULL_HANDLE,
        .stageMask = flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT),
    };
    const VkCommandBufferSubmitInfo commands{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = frame.commands,
    };
    const VkSubmitInfo2 info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = presents ? 1U : 0U,
        .pWaitSemaphoreInfos = &wait_for,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &commands,
        .signalSemaphoreInfoCount = presents ? 1U : 0U,
        .pSignalSemaphoreInfos = &signal,
    };
    return gpu.queue_submit(gpu.queue, 1, &info, frame.done);
}

} // namespace

namespace fbe::gpu {

bool start_with_window() {
    if (device() != nullptr) {
        return true;
    }
    VkInstance vulkan = instance();
    if (vulkan == VK_NULL_HANDLE) {
        return false;
    }
    if (!create_surface(vulkan) || !start(surface)) {
        return false;
    }
    return window == nullptr || make_window(*device());
}

} // namespace fbe::gpu

namespace fbe {

bool gpu_window_opened(SDL_Window* opened, bool on_screen) {
    window = opened;
    shown = on_screen;
    if (gpu::device() != nullptr) {
        return make_window(*gpu::device());
    }
    return !on_screen || gpu::start_with_window();
}

void gpu_window_closing() {
    destroy_window();
    window = nullptr;
}

void gpu_present(bool wait) {
    gpu::device_t* found = gpu::device();
    if (found == nullptr || window == nullptr || fbe_window_minimized()) {
        return;
    }
    gpu::device_t& gpu = *found;
    const VkExtent2D pixels = window_pixels();
    if (stale || wait != vsync || pixels.width != made_for.width ||
        pixels.height != made_for.height) {
        if (!make_images(gpu, wait)) {
            return;
        }
    }
    if (images.empty()) {
        return;
    }

    const gpu::frame_t& frame = gpu.frames[gpu.frame];
    vkWaitForFences(gpu.device, 1, &frame.done, VK_TRUE, UINT64_MAX);
    std::uint32_t index = 0;
    if (swapchain != VK_NULL_HANDLE) {
        const VkResult acquired = vkAcquireNextImageKHR(gpu.device, swapchain, UINT64_MAX,
                                                        frame.acquired, VK_NULL_HANDLE, &index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            stale = true;
            return;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            return;
        }
        stale = acquired == VK_SUBOPTIMAL_KHR;
    }
    vkResetFences(gpu.device, 1, &frame.done);
    vkResetCommandPool(gpu.device, frame.pool, 0);
    record(gpu, frame.commands, index);
    if (submit(gpu, frame, index) != VK_SUCCESS) {
        return;
    }
    gpu.frame = (gpu.frame + 1) % gpu::frames_in_flight;

    if (swapchain == VK_NULL_HANDLE) {
        ++presented;
        return;
    }
    const VkPresentInfoKHR present{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &rendered[index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &index,
    };
    const VkResult result = vkQueuePresentKHR(gpu.queue, &present);
    stale = result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR;
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        ++presented;
    }
}

} // namespace fbe

extern "C" {

FBE_API int64_t fbe_gpu_presented(void) {
    return presented;
}

FBE_API int32_t fbe_gpu_swapchain_width(void) {
    return static_cast<std::int32_t>(extent.width);
}

FBE_API int32_t fbe_gpu_swapchain_height(void) {
    return static_cast<std::int32_t>(extent.height);
}

FBE_API bool fbe_gpu_swapchain_vsync(void) {
    return !images.empty() && vsync;
}

FBE_API bool fbe_gpu_swapchain_headless(void) {
    return !images.empty() && swapchain == VK_NULL_HANDLE;
}

} // extern "C"
