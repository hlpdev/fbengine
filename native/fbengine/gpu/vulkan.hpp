#pragma once

// What the GPU's own files share: Vulkan through volk, VMA, the device the GPU started on, and its
// frames in flight.

#include <array>
#include <cstdint>
#include <string_view>
#include <volk.h>

// VMA calls Vulkan through the functions volk loaded.
#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

namespace fbe::gpu {

/// How many frames the CPU records while the GPU draws earlier ones.
constexpr std::uint32_t frames_in_flight = 2;

/// Vulkan's flag bits are enum constants, which are signed; its flags are unsigned.
template <typename Flags, typename Bit>
constexpr Flags flag(Bit bit) {
    return static_cast<Flags>(bit);
}

/// One frame in flight: its commands, and what says the GPU's done with them.
struct frame_t {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    /// Signaled when the GPU has finished the frame's commands, as it starts.
    VkFence done = VK_NULL_HANDLE;
    /// Signaled when the swapchain's image the frame draws into is free.
    VkSemaphore acquired = VK_NULL_HANDLE;
};

/// The device the GPU started on, and what it draws with. Vulkan 1.3's names for dynamic
/// rendering and synchronization2 or 1.2's extensions', whichever the device has.
struct device_t {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    PFN_vkCmdBeginRendering begin_rendering = nullptr;
    PFN_vkCmdEndRendering end_rendering = nullptr;
    PFN_vkCmdPipelineBarrier2 pipeline_barrier = nullptr;
    PFN_vkQueueSubmit2 queue_submit = nullptr;
    std::array<frame_t, frames_in_flight> frames{};
    /// The frame being recorded.
    std::uint32_t frame = 0;
};

/// Vulkan's instance, made the first time something asks for it, with SDL's surfaces' extensions;
/// VK_NULL_HANDLE, with fbe_error's why, if Vulkan can't start.
VkInstance instance();

/// The device, once the GPU has started; nullptr before.
device_t* device();

/// Starts the GPU, making `surface` the swapchain's if there is one, which the device has to be
/// able to present to; false, with fbe_error's why, if it can't.
bool start(VkSurfaceKHR surface);

/// Starts the GPU, if it hasn't started, with a surface and a swapchain for the window if there's
/// one; false, with fbe_error's why, if it can't.
bool start_with_window();

/// Keeps a Vulkan call's failure for fbe_error(), with what was being done, and returns false.
bool fail_vulkan(std::string_view doing, VkResult result);

} // namespace fbe::gpu
