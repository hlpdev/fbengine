// A shown window's swapchain, on SDL's offscreen video, which shows none and has no surface: the
// tests make its surface, and stand in for the driver's surface and swapchain calls with images
// of their own, so what the engine does with what a surface says, out of date, suboptimal, lost,
// or the device lost, is what it does on a screen.

#include "gpu_testing.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <deque>
#include <fbengine.h>
#include <gpu/vulkan.hpp>
#include <map>
#include <set>
#include <string>
#include <vector>

using fbe::gpu::flag;
using fbe::tests::require;
using fbe::tests::stop_gpu;

namespace {

/// A swapchain the tests made: its surface, its images, and the next it gives.
struct chain_t {
    std::uint64_t surface = 0;
    std::vector<VkImage> images;
    std::vector<VmaAllocation> memory;
    VkExtent2D extent{};
    std::uint32_t next = 0;
};

/// What the tests' surface and swapchains say, and what's been asked of them.
struct fake_t {
    /// The surface's size, which a minimized window's is 0 by 0 on some systems.
    VkExtent2D extent{ .width = 320, .height = 200 };
    /// What the next acquires and presents say, in turn, before they're as they should be; one that
    /// says the surface is lost loses it.
    std::deque<VkResult> acquires;
    std::deque<VkResult> presents;
    std::uint64_t surfaces = 0;
    std::set<std::uint64_t> lost;
    std::uint64_t destroyed_surfaces = 0;
    std::map<std::uint64_t, chain_t> chains;
    std::uint64_t made_chains = 0;
    std::int64_t presented = 0;
    /// How many surfaces to come are lost as they're made.
    int lost_as_made = 0;
    /// What was asked of them that a driver would take as the caller's mistake: a swapchain of no
    /// size, or one that isn't there.
    int misused = 0;
};

// A test's, made as the program starts, where a failure is the test program's own.
fake_t fake; // NOLINT(bugprone-throwing-static-initialization)

/// The driver's own calls, as the GPU loaded them, put back after each test.
struct real_t {
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR support = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capabilities = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR formats = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR modes = nullptr;
    PFN_vkCreateSwapchainKHR create = nullptr;
    PFN_vkDestroySwapchainKHR destroy = nullptr;
    PFN_vkGetSwapchainImagesKHR images = nullptr;
    PFN_vkAcquireNextImageKHR acquire = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    PFN_vkDestroySurfaceKHR destroy_surface = nullptr;
};

real_t real;

template <typename Handle>
Handle handle_of(std::uint64_t value) {
    return reinterpret_cast<Handle>(value); // NOLINT(performance-no-int-to-ptr): a test's handle.
}

template <typename Handle>
std::uint64_t value_of(Handle handle) {
    return reinterpret_cast<std::uint64_t>(handle);
}

VkSurfaceKHR make_surface() {
    ++fake.surfaces;
    if (fake.lost_as_made > 0) {
        --fake.lost_as_made;
        fake.lost.insert(fake.surfaces);
    }
    return handle_of<VkSurfaceKHR>(fake.surfaces);
}

VKAPI_ATTR VkResult VKAPI_CALL support(VkPhysicalDevice /*physical*/, std::uint32_t /*family*/,
                                       VkSurfaceKHR /*surface*/, VkBool32* supported) {
    *supported = VK_TRUE;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL capabilities(VkPhysicalDevice /*physical*/, VkSurfaceKHR surface,
                                            VkSurfaceCapabilitiesKHR* said) {
    if (fake.lost.contains(value_of(surface))) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    *said = {
        .minImageCount = 2,
        .maxImageCount = 3,
        .currentExtent = fake.extent,
        .minImageExtent = { .width = 1, .height = 1 },
        .maxImageExtent = { .width = 4096, .height = 4096 },
        .maxImageArrayLayers = 1,
        .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .supportedUsageFlags = flag<VkImageUsageFlags>(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                               flag<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_DST_BIT) |
                               flag<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
    };
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL formats(VkPhysicalDevice /*physical*/, VkSurfaceKHR /*surface*/,
                                       std::uint32_t* count, VkSurfaceFormatKHR* listed) {
    if (listed != nullptr && *count >= 1) {
        listed[0] = {
            .format = VK_FORMAT_B8G8R8A8_SRGB,
            .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        };
    }
    *count = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL modes(VkPhysicalDevice /*physical*/, VkSurfaceKHR /*surface*/,
                                     std::uint32_t* count, VkPresentModeKHR* listed) {
    if (listed != nullptr && *count >= 1) {
        listed[0] = VK_PRESENT_MODE_FIFO_KHR;
    }
    *count = 1;
    return VK_SUCCESS;
}

void destroy_chain(chain_t& chain) {
    const fbe::gpu::device_t* gpu = fbe::gpu::device();
    for (std::size_t at = 0; at < chain.images.size(); ++at) {
        vmaDestroyImage(gpu->allocator, chain.images[at], chain.memory[at]);
    }
    chain = {};
}

VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice /*device*/, const VkSwapchainCreateInfoKHR* info,
                                      const VkAllocationCallbacks* /*callbacks*/,
                                      VkSwapchainKHR* made) {
    if (fake.lost.contains(value_of(info->surface))) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    if (info->imageExtent.width == 0 || info->imageExtent.height == 0) {
        ++fake.misused;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    chain_t chain{ .surface = value_of(info->surface), .extent = info->imageExtent };
    const fbe::gpu::device_t* gpu = fbe::gpu::device();
    for (std::uint32_t at = 0; at < info->minImageCount; ++at) {
        const VkImageCreateInfo image_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = info->imageFormat,
            .extent = {
                .width = info->imageExtent.width,
                .height = info->imageExtent.height,
                .depth = 1,
            },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = info->imageUsage,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        const VmaAllocationCreateInfo allocation{ .usage = VMA_MEMORY_USAGE_AUTO };
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = VK_NULL_HANDLE;
        if (vmaCreateImage(gpu->allocator, &image_info, &allocation, &image, &memory, nullptr) !=
            VK_SUCCESS) {
            destroy_chain(chain);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        chain.images.push_back(image);
        chain.memory.push_back(memory);
    }
    ++fake.made_chains;
    fake.chains[fake.made_chains] = std::move(chain);
    *made = handle_of<VkSwapchainKHR>(fake.made_chains);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy(VkDevice /*device*/, VkSwapchainKHR swapchain,
                                   const VkAllocationCallbacks* /*callbacks*/) {
    const auto found = fake.chains.find(value_of(swapchain));
    if (found != fake.chains.end()) {
        destroy_chain(found->second);
        fake.chains.erase(found);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL images(VkDevice /*device*/, VkSwapchainKHR swapchain,
                                      std::uint32_t* count, VkImage* listed) {
    const chain_t& chain = fake.chains.at(value_of(swapchain));
    if (listed != nullptr) {
        std::copy_n(chain.images.begin(), std::min<std::size_t>(*count, chain.images.size()),
                    listed);
    }
    *count = static_cast<std::uint32_t>(chain.images.size());
    return VK_SUCCESS;
}

/// A submit of nothing that waits for `wait`, or signals `signal`, so a semaphore's used as the
/// driver's acquire and present use it.
void submit_nothing(VkSemaphore wait, VkSemaphore signal) {
    const fbe::gpu::device_t* gpu = fbe::gpu::device();
    const VkSemaphoreSubmitInfo waited{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = wait,
        .stageMask = flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT),
    };
    const VkSemaphoreSubmitInfo signaled{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = signal,
        .stageMask = flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT),
    };
    const VkSubmitInfo2 submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = wait != VK_NULL_HANDLE ? 1U : 0U,
        .pWaitSemaphoreInfos = &waited,
        .signalSemaphoreInfoCount = signal != VK_NULL_HANDLE ? 1U : 0U,
        .pSignalSemaphoreInfos = &signaled,
    };
    require(gpu->queue_submit(gpu->queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
}

VKAPI_ATTR VkResult VKAPI_CALL acquire(VkDevice /*device*/, VkSwapchainKHR swapchain,
                                       std::uint64_t /*timeout*/, VkSemaphore semaphore,
                                       VkFence /*fence*/, std::uint32_t* index) {
    const auto found = fake.chains.find(value_of(swapchain));
    if (found == fake.chains.end()) {
        ++fake.misused;
        return VK_ERROR_UNKNOWN;
    }
    chain_t& chain = found->second;
    // As a driver's does, each time, once its surface is lost.
    if (fake.lost.contains(chain.surface)) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    if (!fake.acquires.empty()) {
        const VkResult said = fake.acquires.front();
        fake.acquires.pop_front();
        if (said == VK_ERROR_SURFACE_LOST_KHR) {
            fake.lost.insert(chain.surface);
        }
        return said;
    }
    *index = chain.next;
    chain.next = (chain.next + 1) % static_cast<std::uint32_t>(chain.images.size());
    submit_nothing(VK_NULL_HANDLE, semaphore);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue /*queue*/, const VkPresentInfoKHR* info) {
    for (std::uint32_t at = 0; at < info->waitSemaphoreCount; ++at) {
        submit_nothing(info->pWaitSemaphores[at], VK_NULL_HANDLE);
    }
    const auto found = fake.chains.find(value_of(info->pSwapchains[0]));
    if (found == fake.chains.end()) {
        ++fake.misused;
        return VK_ERROR_UNKNOWN;
    }
    const std::uint64_t surface = found->second.surface;
    VkResult said = fake.lost.contains(surface) ? VK_ERROR_SURFACE_LOST_KHR : VK_SUCCESS;
    if (said == VK_SUCCESS && !fake.presents.empty()) {
        said = fake.presents.front();
        fake.presents.pop_front();
        if (said == VK_ERROR_SURFACE_LOST_KHR) {
            fake.lost.insert(surface);
        }
    }
    if (said == VK_SUCCESS || said == VK_SUBOPTIMAL_KHR) {
        ++fake.presented;
    }
    return said;
}

VKAPI_ATTR void VKAPI_CALL destroy_surface(VkInstance /*instance*/, VkSurfaceKHR surface,
                                           const VkAllocationCallbacks* /*callbacks*/) {
    if (surface != VK_NULL_HANDLE) {
        ++fake.destroyed_surfaces;
    }
}

/// The GPU started with no window, the driver's surface and swapchain calls the tests', and a shown
/// window of 320 by 200 open on a surface of theirs; and, as it goes, the window closed, the
/// GPU stopped, and the driver's own calls back.
struct shown_t {
    shown_t() {
        fake = {};
        REQUIRE(fbe_window_set_headless(false));
        fbe_gpu_set_pipeline_cache("");
        require(fbe_gpu_start());
        real = {
            .support = vkGetPhysicalDeviceSurfaceSupportKHR,
            .capabilities = vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
            .formats = vkGetPhysicalDeviceSurfaceFormatsKHR,
            .modes = vkGetPhysicalDeviceSurfacePresentModesKHR,
            .create = vkCreateSwapchainKHR,
            .destroy = vkDestroySwapchainKHR,
            .images = vkGetSwapchainImagesKHR,
            .acquire = vkAcquireNextImageKHR,
            .present = vkQueuePresentKHR,
            .destroy_surface = vkDestroySurfaceKHR,
        };
        vkGetPhysicalDeviceSurfaceSupportKHR = support;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR = capabilities;
        vkGetPhysicalDeviceSurfaceFormatsKHR = formats;
        vkGetPhysicalDeviceSurfacePresentModesKHR = modes;
        vkCreateSwapchainKHR = create;
        vkDestroySwapchainKHR = destroy;
        vkGetSwapchainImagesKHR = images;
        vkAcquireNextImageKHR = acquire;
        vkQueuePresentKHR = present;
        vkDestroySurfaceKHR = destroy_surface;
        fbe::gpu::make_surfaces_by(make_surface);
        require(fbe_window_open(320, 200, FBE_WINDOW_WINDOWED));
    }
    shown_t(const shown_t&) = delete;
    shown_t& operator=(const shown_t&) = delete;
    shown_t(shown_t&&) = delete;
    shown_t& operator=(shown_t&&) = delete;
    ~shown_t() {
        fbe_window_close();
        stop_gpu();
        fbe::gpu::make_surfaces_by(nullptr);
        vkGetPhysicalDeviceSurfaceSupportKHR = real.support;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR = real.capabilities;
        vkGetPhysicalDeviceSurfaceFormatsKHR = real.formats;
        vkGetPhysicalDeviceSurfacePresentModesKHR = real.modes;
        vkCreateSwapchainKHR = real.create;
        vkDestroySwapchainKHR = real.destroy;
        vkGetSwapchainImagesKHR = real.images;
        vkAcquireNextImageKHR = real.acquire;
        vkQueuePresentKHR = real.present;
        vkDestroySurfaceKHR = real.destroy_surface;
        CHECK(fake.chains.empty());
        CHECK(fake.misused == 0);
    }
};

} // namespace

TEST_CASE("a shown window presents through a swapchain of its surface's size", "[gpu]") {
    const shown_t shown;
    CHECK(!fbe_gpu_swapchain_headless());
    CHECK(fake.surfaces == 1);
    CHECK(fake.made_chains == 1);
    CHECK(fbe_gpu_swapchain_width() == 320);
    CHECK(fbe_gpu_swapchain_height() == 200);
    for (int frame = 0; frame < 5; ++frame) {
        fbe_frame_flip(true);
    }
    CHECK(fake.presented == 5);
    CHECK(fbe_gpu_presented() == 5);
    CHECK(fake.made_chains == 1);
    CHECK(fbe_gpu_validation_errors() == 0);
}

// Out of date as it's acquired, or as it's presented: that frame isn't shown, or is, and the next
// makes the swapchain again and presents.
TEST_CASE("a swapchain out of date is made again, and the next frame's presented", "[gpu]") {
    const shown_t shown;
    fbe_frame_flip(true);
    REQUIRE(fake.presented == 1);
    SECTION("as it's acquired") {
        fake.acquires.push_back(VK_ERROR_OUT_OF_DATE_KHR);
        fbe_frame_flip(true);
        CHECK(fake.presented == 1);
    }
    SECTION("as it's presented") {
        fake.presents.push_back(VK_ERROR_OUT_OF_DATE_KHR);
        fbe_frame_flip(true);
        CHECK(fake.presented == 1);
    }
    SECTION("suboptimal, as it's presented, which shows it") {
        fake.presents.push_back(VK_SUBOPTIMAL_KHR);
        fbe_frame_flip(true);
        CHECK(fake.presented == 2);
    }
    fbe_frame_flip(true);
    CHECK(fake.made_chains == 2);
    const std::int64_t before = fake.presented;
    fbe_frame_flip(true);
    CHECK(fake.presented == before + 1);
    CHECK(fake.made_chains == 2);
    CHECK(!fbe_gpu_lost());
    CHECK(fbe_gpu_validation_errors() == 0);
}

// A surface lost, as a display that goes loses it: its swapchain can't be made of it again, so a
// new surface is made, and a swapchain of that, and frames are presented again, from the frame
// after it said so.
TEST_CASE("a lost surface is made again with its swapchain, and presents again", "[gpu]") {
    const shown_t shown;
    fbe_frame_flip(true);
    REQUIRE(fake.presented == 1);
    const auto flips = [](int count) {
        for (int frame = 0; frame < count; ++frame) {
            fbe_frame_flip(true);
        }
    };
    // Lost from then on, as the fake has it once it says so.
    SECTION("as it's acquired") {
        fake.acquires.push_back(VK_ERROR_SURFACE_LOST_KHR);
        flips(2);
        CHECK(fake.surfaces == 2);
    }
    SECTION("as it's presented") {
        fake.presents.push_back(VK_ERROR_SURFACE_LOST_KHR);
        flips(2);
        CHECK(fake.surfaces == 2);
    }
    SECTION("as its swapchain's made again, and its old one's acquired") {
        fake.lost.insert(1);
        fake.acquires.push_back(VK_ERROR_OUT_OF_DATE_KHR);
        flips(3);
        CHECK(fake.surfaces == 2);
    }
    // With no swapchain of its own to say so as it's acquired.
    SECTION("again, as the one made again is read") {
        fake.lost_as_made = 1;
        fake.acquires.push_back(VK_ERROR_SURFACE_LOST_KHR);
        flips(3);
        CHECK(fake.surfaces == 3);
    }
    CHECK(fake.presented == 2);
    CHECK(fake.destroyed_surfaces == fake.surfaces - 1);
    flips(2);
    CHECK(fake.presented == 4);
    CHECK(fake.destroyed_surfaces == fake.surfaces - 1);
    CHECK(fbe_gpu_swapchain_width() == 320);
    CHECK(!fbe_gpu_lost());
    CHECK(fbe_gpu_validation_errors() == 0);
}

// The device lost as an image is acquired or presented: the GPU's lost, and says so.
TEST_CASE("a device lost as a swapchain's image is acquired or presented is lost", "[gpu]") {
    const shown_t shown;
    fbe_frame_flip(true);
    SECTION("acquired") {
        fake.acquires.push_back(VK_ERROR_DEVICE_LOST);
    }
    SECTION("presented") {
        fake.presents.push_back(VK_ERROR_DEVICE_LOST);
    }
    fbe_frame_flip(true);
    CHECK(fbe_gpu_lost());
    const std::int64_t before = fake.presented;
    fbe_frame_flip(true);
    CHECK(fake.presented == before);
    CHECK(fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, false) == 0);
    CHECK(std::string(fbe_error()).starts_with("the GPU was lost"));
}

// A surface 0 by 0, as a minimized window's is on some systems: no swapchain, nothing presented,
// and none of it an error; then, its size back, a swapchain again.
TEST_CASE("a surface of no size has no swapchain until it has one", "[gpu]") {
    const shown_t shown;
    fbe_frame_flip(true);
    REQUIRE(fake.presented == 1);
    fake.extent = { .width = 0, .height = 0 };
    fake.acquires.push_back(VK_ERROR_OUT_OF_DATE_KHR);
    for (int frame = 0; frame < 3; ++frame) {
        fbe_frame_flip(true);
    }
    CHECK(fake.presented == 1);
    CHECK(fake.chains.empty());
    fake.extent = { .width = 320, .height = 200 };
    REQUIRE(fbe_window_set_size(320, 201));
    fbe_frame_flip(true);
    fbe_frame_flip(true);
    CHECK(fake.chains.size() == 1);
    CHECK(fake.presented >= 2);
    CHECK(fbe_gpu_validation_errors() == 0);
}

// Minimized, as a window's said to be: nothing's acquired or presented, and on restoring, frames
// are presented again through the swapchain it had.
TEST_CASE("a minimized window on a surface isn't presented, and restored is", "[gpu]") {
    const shown_t shown;
    fbe_frame_flip(true);
    REQUIRE(fbe_window_simulate(FBE_WINDOW_SIMULATE_MINIMIZE));
    fbe_frame_flip(true);
    const std::int64_t before = fake.presented;
    fbe_frame_flip(true);
    fbe_frame_flip(true);
    CHECK(fake.presented == before);
    REQUIRE(fbe_window_simulate(FBE_WINDOW_SIMULATE_RESTORE));
    fbe_frame_flip(true);
    fbe_frame_flip(true);
    CHECK(fake.presented == before + 1);
    CHECK(fbe_gpu_validation_errors() == 0);
}
