// The window's images and Flip's present. A shown window has a surface and a swapchain, made
// again whenever the window's size in pixels or vsync changes, or the surface says it's out of
// date; each frame in flight acquires one of its images, draws the screen into it, scaled, the
// frame graph's last pass, and presents it. A headless window has one image of its own, its size in
// pixels, which each frame draws into the same way and leaves, as nothing shows it: Vulkan's
// headless surfaces aren't on every driver (Windows' have none, and lavapipe's need udmabuf), so
// headless frames never depend on them. The window's images are sRGB, so what's drawn is linear
// light, and the screen scales into them in linear light too.

#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/gpu.hpp"
#include "gpu/graph.hpp"
#include "gpu/image.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/vulkan.hpp"
#include "texture/texture.hpp"

#include <algorithm>
#include <cstdint>
#include <engine_shaders.hpp>
#include <format>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using fbe::gpu::flag;

/// What a headless window's image is, as a swapchain's mostly are.
constexpr VkFormat headless_format = VK_FORMAT_B8G8R8A8_SRGB;

/// The window the GPU draws into, from when it's opened until it starts closing.
SDL_Window* drawn_window = nullptr;
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
/// The window's images' format: sRGB, or, where a surface has none, 8-bit stored as it's written,
/// which the present pass encodes as sRGB itself.
VkFormat format = VK_FORMAT_UNDEFINED;
/// Whether the surface said the swapchain no longer matches it.
bool stale = false;
/// Whether the surface said it's lost, as a window's can be when its display goes: it's made again
/// with its swapchain, which can't be made of it.
bool surface_lost = false;
/// For a test, what makes a surface in SDL's place.
VkSurfaceKHR (*test_surface)() = nullptr;
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
    SDL_GetWindowSizeInPixels(drawn_window, &width, &height);
    return {
        .width = static_cast<std::uint32_t>(std::max(width, 0)),
        .height = static_cast<std::uint32_t>(std::max(height, 0)),
    };
}

/// Destroys the window's images, waiting for the GPU to be done with them unless it's lost, but
/// not the swapchain, which its next one is made from.
void destroy_images(const fbe::gpu::device_t& gpu) {
    // Nothing's in flight on a lost device to wait for.
    if (!fbe::gpu::lost()) {
        fbe::gpu::wait_idle(gpu.device);
    }
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

/// A view of each of the images, of `view_format`; false, with fbe_error's why, if one can't be
/// made.
bool make_views(const fbe::gpu::device_t& gpu, VkFormat view_format) {
    for (VkImage image : images) {
        const VkImageViewCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = view_format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        VkImageView view = VK_NULL_HANDLE;
        ++fbe::gpu::reached().views;
        const VkResult result = fbe::gpu::faulted(fbe::gpu::fault_t::VIEW)
                                    ? VK_ERROR_OUT_OF_HOST_MEMORY
                                    : vkCreateImageView(gpu.device, &info, nullptr, &view);
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
    for (const VkFormat wanted : {
             headless_format,
             VK_FORMAT_R8G8B8A8_SRGB,
             VK_FORMAT_B8G8R8A8_UNORM,
             VK_FORMAT_R8G8B8A8_UNORM,
         }) {
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
/// pixels; false, with fbe_error's why, if it can't, leaving no images and the swapchain stale, so
/// the next present tries again.
bool make_swapchain(const fbe::gpu::device_t& gpu, bool wait) {
    stale = true;
    VkSurfaceCapabilitiesKHR capabilities{
        .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
    };
    VkResult result =
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu.physical, surface, &capabilities);
    if (result != VK_SUCCESS) {
        surface_lost = surface_lost || result == VK_ERROR_SURFACE_LOST_KHR;
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
    vsync = wait;
    if (size.width == 0 || size.height == 0) {
        vkDestroySwapchainKHR(gpu.device, old, nullptr);
        stale = false;
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
    ++fbe::gpu::reached().allocations;
    result = vkCreateSwapchainKHR(gpu.device, &info, nullptr, &swapchain);
    vkDestroySwapchainKHR(gpu.device, old, nullptr);
    if (result != VK_SUCCESS) {
        swapchain = VK_NULL_HANDLE;
        return fbe::gpu::fail_vulkan("couldn't make the window's swapchain", result);
    }

    vkGetSwapchainImagesKHR(gpu.device, swapchain, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(gpu.device, swapchain, &count, images.data());
    format = chosen.format;
    if (!make_views(gpu, chosen.format)) {
        destroy_images(gpu);
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const VkSemaphoreCreateInfo semaphore_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        VkSemaphore semaphore = VK_NULL_HANDLE;
        result = vkCreateSemaphore(gpu.device, &semaphore_info, nullptr, &semaphore);
        if (result != VK_SUCCESS) {
            destroy_images(gpu);
            return fbe::gpu::fail_vulkan("couldn't make the swapchain's semaphores", result);
        }
        rendered.push_back(semaphore);
    }
    extent = size;
    stale = false;
    return true;
}

/// Makes a headless window's image again for its size, or none while it has no pixels, and
/// keeps `wait`, which nothing waits for; false, with fbe_error's why, if it can't, leaving no
/// image and the next present to try again.
bool make_headless(const fbe::gpu::device_t& gpu, bool wait) {
    stale = true;
    destroy_images(gpu);
    made_for = window_pixels();
    vsync = wait;
    if (made_for.width == 0 || made_for.height == 0) {
        stale = false;
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
    ++fbe::gpu::reached().allocations;
    const VkResult result = vmaCreateImage(gpu.allocator, &image_info, &allocation, &headless,
                                           &headless_memory, nullptr);
    if (result != VK_SUCCESS) {
        headless = VK_NULL_HANDLE;
        return fbe::gpu::fail_vulkan("couldn't make the headless window's image", result);
    }
    images.push_back(headless);
    format = headless_format;
    if (!make_views(gpu, headless_format)) {
        destroy_images(gpu);
        return false;
    }
    extent = made_for;
    stale = false;
    return true;
}

/// Makes the window's images again: its swapchain's, or a headless one's.
bool make_images(const fbe::gpu::device_t& gpu, bool wait) {
    return surface != VK_NULL_HANDLE ? make_swapchain(gpu, wait) : make_headless(gpu, wait);
}

/// Whether the window is shown, as SDL's offscreen video's never are, though they're not headless,
/// unless a test makes their surfaces.
bool on_screen() {
    const char* driver = SDL_GetCurrentVideoDriver();
    return shown && (test_surface != nullptr || driver == nullptr ||
                     std::string_view(driver) != "offscreen");
}

/// A shown window's surface, if it hasn't one yet.
bool create_surface(VkInstance vulkan) {
    if (drawn_window == nullptr || !on_screen() || surface != VK_NULL_HANDLE) {
        return true;
    }
    if (test_surface != nullptr) {
        surface = test_surface();
        return surface != VK_NULL_HANDLE || fbe::fail("couldn't make the window's Vulkan surface");
    }
    if (!SDL_Vulkan_CreateSurface(drawn_window, vulkan, nullptr, &surface)) {
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
            return fbe::fail(std::format("the GPU {} can't present to the window",
                                         fbe::gpu::name_of(gpu.properties)));
        }
    }
    // A lost GPU makes it nothing: its screen and images are made as it starts again.
    if (fbe::gpu::lost()) {
        return true;
    }
    fbe::gpu::update_screen(window_pixels());
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
    surface_lost = false;
}

/// What the present shader's push constants are: the screen's index and sampler, and whether the
/// window's image is stored as written, so the shader encodes sRGB itself.
struct present_t {
    std::uint32_t texture;
    std::uint32_t sampler;
    std::uint32_t encode;
};

/// Records drawing the screen, the graph's `screen`, into `area` of the window's image, a
/// triangle covering it.
void draw_screen(VkCommandBuffer commands, fbe::gpu::resource_t screen, const VkRect2D& area) {
    if (screen == fbe::gpu::no_resource) {
        return;
    }
    VkPipeline pipeline = fbe::gpu::pipeline({ .shader = &fbe::shaders::present, .color = format });
    if (pipeline == VK_NULL_HANDLE) {
        return;
    }
    const VkViewport viewport{
        .x = static_cast<float>(area.offset.x),
        .y = static_cast<float>(area.offset.y),
        .width = static_cast<float>(area.extent.width),
        .height = static_cast<float>(area.extent.height),
        .maxDepth = 1.0F,
    };
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &area);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorSet set = fbe::gpu::bindless_set();
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, fbe::gpu::pipeline_layout(),
                            0, 1, &set, 0, nullptr);
    const bool srgb = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
    const present_t constants{
        .texture = fbe::gpu::resource_index(screen),
        .sampler = fbe::gpu::sampler_index(fbe::gpu::screen_smooth() ? FBE_TEXTURE_LINEAR
                                                                     : FBE_TEXTURE_NEAREST,
                                           FBE_TEXTURE_CLAMP, FBE_TEXTURE_CLAMP),
        .encode = srgb ? 0U : 1U,
    };
    vkCmdPushConstants(commands, fbe::gpu::pipeline_layout(),
                       flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL), 0, sizeof(constants),
                       &constants);
    vkCmdDraw(commands, 3, 1, 0, 0);
    fbe::gpu::count_draws(1, 1);
}

/// Adds the frame's last pass to the graph, into the window's image `index`: cleared, the screen
/// the frame drew into, `drawn`, drawn into it, scaled to the window's size now, then left to be
/// presented, or, a headless window's, to be copied from. A headless window's one image is drawn
/// by each frame in flight, so its first use waits for the frame before's copies.
void add_present(std::uint32_t index, std::uint64_t drawn) {
    const auto output =
        flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    const auto copy = flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COPY_BIT);
    const bool presents = swapchain != VK_NULL_HANDLE;
    const fbe::gpu::resource_t window_image = fbe::gpu::external_resource({
        .image = images[index],
        .view = views[index],
        .extent = extent,
        .waits = output | copy,
        .layout = presents ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .stages = presents ? output : copy,
        .access = presents ? 0 : flag<VkAccessFlags2>(VK_ACCESS_2_TRANSFER_READ_BIT),
    });
    const fbe::gpu::resource_t screen =
        drawn != 0 ? fbe::gpu::texture_resource(drawn) : fbe::gpu::no_resource;
    const VkRect2D area = fbe::gpu::screen_area(extent);
    // Black around the screen, where it doesn't fill the window.
    fbe::gpu::pass_t present{
        .name = "present",
        .colors = { { .target = window_image, .clear = true, .rgba = { 0.0F, 0.0F, 0.0F, 1.0F } } },
        .record = [screen, area](VkCommandBuffer commands) { draw_screen(commands, screen, area); },
    };
    if (screen != fbe::gpu::no_resource) {
        present.uses.emplace_back(screen, fbe::gpu::use_t::SAMPLED);
    }
    fbe::gpu::add_pass(std::move(present));
}

/// Whether the window has images to present this frame in, made again first if it's changed.
bool presentable(const fbe::gpu::device_t& gpu, bool wait) {
    if (drawn_window == nullptr || fbe_window_minimized()) {
        return false;
    }
    if (surface_lost) {
        // Its swapchain goes with it, and both are made again; until a surface is, each frame
        // tries again.
        destroy_window();
        if (!make_window(gpu) && surface == VK_NULL_HANDLE) {
            surface_lost = true;
            return false;
        }
        return !images.empty();
    }
    const VkExtent2D pixels = window_pixels();
    if (stale || wait != vsync || pixels.width != made_for.width ||
        pixels.height != made_for.height) {
        make_images(gpu, wait);
    }
    return !images.empty();
}

/// Brings the screen up to date for the window's size now, for the next frame, once the frame
/// drawn into the one there was is presented.
void update_screen() {
    if (drawn_window != nullptr) {
        fbe::gpu::update_screen(window_pixels());
    }
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
    return drawn_window == nullptr || make_window(*device());
}

std::vector<std::uint8_t> read_window() {
    const device_t* gpu = device();
    if (gpu == nullptr || headless == VK_NULL_HANDLE) {
        return {};
    }
    fbe::gpu::wait_idle(gpu->device);
    std::vector<std::uint8_t> pixels =
        read_pixels(headless, extent, 4, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // Blue first, as the image keeps them, to red first.
    for (std::size_t at = 0; at + 3 < pixels.size(); at += 4) {
        std::swap(pixels[at], pixels[at + 2]);
    }
    return pixels;
}

bool window_open() {
    return drawn_window != nullptr;
}

void stop_window() {
    stop_screen();
    destroy_window();
    made_for = {};
    stale = false;
}

void make_surfaces_by(VkSurfaceKHR (*make)()) {
    test_surface = make;
}

} // namespace fbe::gpu

namespace fbe {

bool gpu_window_opened(SDL_Window* opened, bool unless_headless) {
    drawn_window = opened;
    shown = unless_headless;
    if (gpu::device() != nullptr) {
        return make_window(*gpu::device());
    }
    return !on_screen() || gpu::start_with_window();
}

void gpu_window_closing() {
    gpu::close_screen();
    destroy_window();
    drawn_window = nullptr;
}

void gpu_present(bool wait) {
    const gpu::device_t* found = gpu::device();
    if (found == nullptr) {
        return;
    }
    gpu::save_pipeline_cache();
    // Nothing within a present frees what waits by submitting, as the screen made again would.
    const gpu::edge_t edge;
    const gpu::device_t& gpu = *found;
    // A frame with nowhere to present still runs what it recorded; a lost GPU's runs nothing.
    if (gpu::lost() || !presentable(gpu, wait)) {
        update_screen();
        gpu::submit_frame(VK_NULL_HANDLE, VK_NULL_HANDLE);
        return;
    }

    // Begun, so the acquire's semaphore is the frame's once the GPU's done with it.
    const gpu::frame_t& frame = gpu::recording_frame();
    std::uint32_t index = 0;
    if (swapchain != VK_NULL_HANDLE) {
        ++gpu::reached().submits;
        const VkResult acquired = vkAcquireNextImageKHR(gpu.device, swapchain, UINT64_MAX,
                                                        frame.acquired, VK_NULL_HANDLE, &index);
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            gpu::lose(acquired);
            stale = stale || acquired == VK_ERROR_OUT_OF_DATE_KHR;
            surface_lost = surface_lost || acquired == VK_ERROR_SURFACE_LOST_KHR;
            update_screen();
            gpu::submit_frame(VK_NULL_HANDLE, VK_NULL_HANDLE);
            return;
        }
        stale = stale || acquired == VK_SUBOPTIMAL_KHR;
    }
    add_present(index, gpu::screen_handle());
    update_screen();
    if (swapchain == VK_NULL_HANDLE) {
        if (gpu::submit_frame(VK_NULL_HANDLE, VK_NULL_HANDLE)) {
            ++presented;
        }
        return;
    }
    if (!gpu::submit_frame(frame.acquired, rendered[index])) {
        // The image acquired and not presented goes with the swapchain.
        stale = true;
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
    ++gpu::reached().submits;
    const VkResult result = vkQueuePresentKHR(gpu.queue, &present);
    gpu::lose(result);
    stale = stale || result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR;
    surface_lost = surface_lost || result == VK_ERROR_SURFACE_LOST_KHR;
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

FBE_API bool fbe_gpu_screen_area(fbe_gpu_rect_t* area) {
    if (drawn_window == nullptr || area == nullptr) {
        return fbe::fail("there's no window; Graphics opens one");
    }
    const VkRect2D part = fbe::gpu::screen_area(window_pixels());
    *area = {
        .x = part.offset.x,
        .y = part.offset.y,
        .width = static_cast<std::int32_t>(part.extent.width),
        .height = static_cast<std::int32_t>(part.extent.height),
    };
    return true;
}

FBE_API bool fbe_gpu_swapchain_headless(void) {
    return !images.empty() && swapchain == VK_NULL_HANDLE;
}

} // extern "C"
