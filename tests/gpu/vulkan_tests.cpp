// Vulkan where the tests run, with no window: an instance through volk, a device, preferring a
// CPU one (lavapipe in CI), and an image cleared on it and read back, which is how the renderer's
// golden image tests will read their frames.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <volk.h>

namespace {

constexpr std::uint32_t size = 4;
/// No queue family or memory type.
constexpr std::uint32_t none = UINT32_MAX;

/// What the test made, destroyed in the order Vulkan needs, whatever it got as far as.
struct gpu_t {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    gpu_t() = default;
    gpu_t(const gpu_t&) = delete;
    gpu_t& operator=(const gpu_t&) = delete;
    gpu_t(gpu_t&&) = delete;
    gpu_t& operator=(gpu_t&&) = delete;

    ~gpu_t() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyBuffer(device, buffer, nullptr);
            vkFreeMemory(device, buffer_memory, nullptr);
            vkDestroyImage(device, image, nullptr);
            vkFreeMemory(device, image_memory, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
    }
};

/// Vulkan's flag bits are enum constants, which are signed; its flags are unsigned.
template <typename Flags, typename Bit>
constexpr Flags flag(Bit bit) {
    return static_cast<Flags>(bit);
}

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

/// An instance, enumerating portability drivers too, as MoltenVK is on macOS.
VkInstance make_instance() {
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());

    std::vector<const char*> enabled;
    VkInstanceCreateFlags flags = 0;
    if (has_extension(extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= flag<VkInstanceCreateFlags>(VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR);
    }

    const VkApplicationInfo application{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "fbengine_gpu_tests",
        .apiVersion = VK_API_VERSION_1_2,
    };
    const VkInstanceCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags = flags,
        .pApplicationInfo = &application,
        .enabledExtensionCount = static_cast<std::uint32_t>(enabled.size()),
        .ppEnabledExtensionNames = enabled.data(),
    };
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return instance;
}

/// The device the tests use: a CPU one if there is one, so frames are the same everywhere,
/// else the first.
VkPhysicalDevice pick_device(VkInstance instance) {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    VkPhysicalDevice picked = VK_NULL_HANDLE;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            return device;
        }
        if (picked == VK_NULL_HANDLE) {
            picked = device;
        }
    }
    return picked;
}

/// The first queue family that draws, or `none`.
std::uint32_t graphics_family(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (std::uint32_t family = 0; family < count; ++family) {
        if ((families[family].queueFlags & flag<VkQueueFlags>(VK_QUEUE_GRAPHICS_BIT)) != 0) {
            return family;
        }
    }
    return none;
}

/// A memory type for `requirements` with `wanted`'s properties, or `none`.
std::uint32_t memory_type(VkPhysicalDevice device, const VkMemoryRequirements& requirements,
                          VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(device, &memory);
    for (std::uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
        if ((requirements.memoryTypeBits & (1U << type)) != 0 &&
            (memory.memoryTypes[type].propertyFlags & wanted) == wanted) {
            return type;
        }
    }
    return none;
}

VkDevice make_device(VkPhysicalDevice physical, std::uint32_t family) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());

    // A portability driver's subset has to be asked for when it's there.
    std::vector<const char*> enabled;
    if (has_extension(extensions, "VK_KHR_portability_subset")) {
        enabled.push_back("VK_KHR_portability_subset");
    }

    const float priority = 1.0F;
    const VkDeviceQueueCreateInfo queue{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
        .enabledExtensionCount = static_cast<std::uint32_t>(enabled.size()),
        .ppEnabledExtensionNames = enabled.data(),
    };
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(physical, &info, nullptr, &device) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return device;
}

/// The whole of a one-layer, one-level color image.
constexpr VkImageSubresourceRange whole_image{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .levelCount = 1,
    .layerCount = 1,
};

VkImageMemoryBarrier transition(VkImage image, VkImageLayout from, VkImageLayout to,
                                VkAccessFlags before, VkAccessFlags after) {
    return {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = before,
        .dstAccessMask = after,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = whole_image,
    };
}

} // namespace

TEST_CASE("an image cleared on the GPU reads back as its color, with no window", "[gpu]") {
    REQUIRE(volkInitialize() == VK_SUCCESS);
    gpu_t gpu;
    gpu.instance = make_instance();
    REQUIRE(gpu.instance != VK_NULL_HANDLE);
    volkLoadInstance(gpu.instance);

    gpu.physical = pick_device(gpu.instance);
    REQUIRE(gpu.physical != VK_NULL_HANDLE);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu.physical, &properties);
    // Shown on every run, so CI's log says which device the frames came from.
    WARN("Vulkan device: " << properties.deviceName);

    gpu.family = graphics_family(gpu.physical);
    REQUIRE(gpu.family != none);
    gpu.device = make_device(gpu.physical, gpu.family);
    REQUIRE(gpu.device != VK_NULL_HANDLE);
    volkLoadDevice(gpu.device);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(gpu.device, gpu.family, 0, &queue);

    // The image, on the device, and a buffer the CPU reads it back through.
    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { .width = size, .height = size, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = flag<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT) |
                 flag<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_DST_BIT),
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    REQUIRE(vkCreateImage(gpu.device, &image_info, nullptr, &gpu.image) == VK_SUCCESS);
    VkMemoryRequirements image_needs{};
    vkGetImageMemoryRequirements(gpu.device, gpu.image, &image_needs);
    const std::uint32_t image_type =
        memory_type(gpu.physical, image_needs,
                    flag<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    REQUIRE(image_type != none);
    const VkMemoryAllocateInfo image_allocation{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_needs.size,
        .memoryTypeIndex = image_type,
    };
    REQUIRE(vkAllocateMemory(gpu.device, &image_allocation, nullptr, &gpu.image_memory) ==
            VK_SUCCESS);
    REQUIRE(vkBindImageMemory(gpu.device, gpu.image, gpu.image_memory, 0) == VK_SUCCESS);

    const VkDeviceSize bytes = VkDeviceSize{ size } * size * 4;
    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_DST_BIT),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    REQUIRE(vkCreateBuffer(gpu.device, &buffer_info, nullptr, &gpu.buffer) == VK_SUCCESS);
    VkMemoryRequirements buffer_needs{};
    vkGetBufferMemoryRequirements(gpu.device, gpu.buffer, &buffer_needs);
    const VkMemoryPropertyFlags host =
        flag<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) |
        flag<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const std::uint32_t buffer_type = memory_type(gpu.physical, buffer_needs, host);
    REQUIRE(buffer_type != none);
    const VkMemoryAllocateInfo buffer_allocation{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buffer_needs.size,
        .memoryTypeIndex = buffer_type,
    };
    REQUIRE(vkAllocateMemory(gpu.device, &buffer_allocation, nullptr, &gpu.buffer_memory) ==
            VK_SUCCESS);
    REQUIRE(vkBindBufferMemory(gpu.device, gpu.buffer, gpu.buffer_memory, 0) == VK_SUCCESS);

    // Clear it, then copy it into the buffer.
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = gpu.family,
    };
    REQUIRE(vkCreateCommandPool(gpu.device, &pool_info, nullptr, &gpu.pool) == VK_SUCCESS);
    const VkCommandBufferAllocateInfo commands_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = gpu.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer commands = VK_NULL_HANDLE;
    REQUIRE(vkAllocateCommandBuffers(gpu.device, &commands_info, &commands) == VK_SUCCESS);

    const VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = flag<VkCommandBufferUsageFlags>(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT),
    };
    REQUIRE(vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS);
    const auto write = flag<VkAccessFlags>(VK_ACCESS_TRANSFER_WRITE_BIT);
    const auto read = flag<VkAccessFlags>(VK_ACCESS_TRANSFER_READ_BIT);
    const VkImageMemoryBarrier to_clear = transition(
        gpu.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, write);
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_clear);
    const VkClearColorValue color{ .float32 = { 1.0F, 0.0F, 1.0F, 1.0F } };
    vkCmdClearColorImage(commands, gpu.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                         &whole_image);
    const VkImageMemoryBarrier to_copy =
        transition(gpu.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, write, read);
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_copy);
    const VkBufferImageCopy region{
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { .width = size, .height = size, .depth = 1 },
    };
    vkCmdCopyImageToBuffer(commands, gpu.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gpu.buffer, 1,
                           &region);
    // The host reads the buffer once the copy's done.
    const VkBufferMemoryBarrier to_host{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = write,
        .dstAccessMask = flag<VkAccessFlags>(VK_ACCESS_HOST_READ_BIT),
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = gpu.buffer,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &to_host, 0, nullptr);
    REQUIRE(vkEndCommandBuffer(commands) == VK_SUCCESS);

    const VkFenceCreateInfo fence_info{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    REQUIRE(vkCreateFence(gpu.device, &fence_info, nullptr, &gpu.fence) == VK_SUCCESS);
    const VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commands,
    };
    REQUIRE(vkQueueSubmit(queue, 1, &submit, gpu.fence) == VK_SUCCESS);
    REQUIRE(vkWaitForFences(gpu.device, 1, &gpu.fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);

    void* mapped = nullptr;
    REQUIRE(vkMapMemory(gpu.device, gpu.buffer_memory, 0, bytes, 0, &mapped) == VK_SUCCESS);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bytes));
    std::memcpy(pixels.data(), mapped, pixels.size());
    vkUnmapMemory(gpu.device, gpu.buffer_memory);

    constexpr std::array<std::uint8_t, 4> magenta{ 255, 0, 255, 255 };
    for (std::size_t pixel = 0; pixel < std::size_t{ size } * size; ++pixel) {
        INFO("pixel " << pixel);
        CHECK(pixels[pixel * 4] == magenta[0]);
        CHECK(pixels[(pixel * 4) + 1] == magenta[1]);
        CHECK(pixels[(pixel * 4) + 2] == magenta[2]);
        CHECK(pixels[(pixel * 4) + 3] == magenta[3]);
    }
}
