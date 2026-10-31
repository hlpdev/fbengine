// The GPU's instance and device: Vulkan loaded through SDL's loader, the devices it has and what
// each lacks of what fbengine needs, the one picked, its queue, VMA, and the frames in flight.

#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/pipeline.hpp"
#include "gpu/vulkan.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fbe::gpu::flag;

/// A device Vulkan has, and what it lacks of what fbengine needs, "" for nothing.
struct candidate_t {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    std::string lacks;
};

bool tried = false;
VkInstance vulkan = VK_NULL_HANDLE;
/// Why Vulkan couldn't start, if it couldn't, which every call that needs it says again.
std::string why_not;
/// The highest Vulkan the instance was made for: 1.3 when the loader has it, else 1.2.
std::uint32_t instance_version = VK_API_VERSION_1_2;
std::vector<candidate_t> candidates;
std::int32_t wanted = -1;
std::int32_t picked = -1;
fbe::gpu::device_t current;
bool started = false;

bool validating = false;
std::int32_t validation_errors = 0;
VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

bool has_extension(const std::vector<VkExtensionProperties>& extensions, std::string_view name) {
    return std::ranges::any_of(extensions, [name](const VkExtensionProperties& extension) {
        return name == extension.extensionName;
    });
}

/// Whether the validation layers are wanted: in a debug build, unless FBE_GPU_VALIDATION is 0,
/// and in any when it's 1.
bool wants_validation() {
    const char* setting = SDL_getenv("FBE_GPU_VALIDATION");
    if (setting != nullptr && std::string_view(setting) == "1") {
        return true;
    }
    if (setting != nullptr && std::string_view(setting) == "0") {
        return false;
    }
#ifdef NDEBUG
    return false;
#else
    return true;
#endif
}

constexpr const char* validation_layer = "VK_LAYER_KHRONOS_validation";

bool has_validation_layer() {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::ranges::any_of(layers, [](const VkLayerProperties& layer) {
        return std::strcmp(layer.layerName, validation_layer) == 0;
    });
}

VKAPI_ATTR VkBool32 VKAPI_CALL on_validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* /*user*/) {
    const bool error = severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    if (error) {
        ++validation_errors;
    }
    // std::println needs MinGW's libstdc++exp, which the DLL doesn't link.
    const std::string line =
        std::format("Vulkan validation {}: {}\n", error ? "error" : "warning", data->pMessage);
    std::fputs(line.c_str(), stderr);
    return VK_FALSE;
}

/// Loads Vulkan's loader through SDL, which finds it as the window's surfaces need it, and makes
/// the instance, with SDL's surfaces' extensions; false, with fbe_error's why, if it can't.
bool make_instance() {
    if (!fbe::start_video()) {
        return false;
    }
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        return fbe::fail_sdl("couldn't load Vulkan");
    }
    volkInitializeCustom(
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr()));
    const std::uint32_t loader = volkGetInstanceVersion();
    if (loader < VK_API_VERSION_1_2) {
        return fbe::fail(std::format("fbengine needs Vulkan 1.2, and the installed Vulkan is {}.{}",
                                     VK_API_VERSION_MAJOR(loader), VK_API_VERSION_MINOR(loader)));
    }
    instance_version = loader >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;

    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());

    Uint32 sdl_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
    std::vector<const char*> enabled;
    for (Uint32 i = 0; sdl_extensions != nullptr && i < sdl_count; ++i) {
        if (has_extension(extensions, sdl_extensions[i])) {
            enabled.push_back(sdl_extensions[i]);
        }
    }
    // Portability drivers, as MoltenVK is, are only listed when asked for.
    VkInstanceCreateFlags flags = 0;
    if (has_extension(extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= flag<VkInstanceCreateFlags>(VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR);
    }
    std::vector<const char*> layers;
    validating = wants_validation() && has_validation_layer() &&
                 has_extension(extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (validating) {
        layers.push_back(validation_layer);
        enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    const VkApplicationInfo application{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pEngineName = "fbengine",
        .apiVersion = instance_version,
    };
    const VkInstanceCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags = flags,
        .pApplicationInfo = &application,
        .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = static_cast<std::uint32_t>(enabled.size()),
        .ppEnabledExtensionNames = enabled.data(),
    };
    const VkResult made = vkCreateInstance(&info, nullptr, &vulkan);
    if (made != VK_SUCCESS) {
        vulkan = VK_NULL_HANDLE;
        return fbe::gpu::fail_vulkan("couldn't start Vulkan", made);
    }
    volkLoadInstanceOnly(vulkan);

    if (validating) {
        const VkDebugUtilsMessengerCreateInfoEXT messenger_info{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = flag<VkDebugUtilsMessageSeverityFlagsEXT>(
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) |
                               flag<VkDebugUtilsMessageSeverityFlagsEXT>(
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT),
            .messageType = flag<VkDebugUtilsMessageTypeFlagsEXT>(
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT),
            .pfnUserCallback = on_validation,
        };
        vkCreateDebugUtilsMessengerEXT(vulkan, &messenger_info, nullptr, &messenger);
    }
    return true;
}

/// The highest Vulkan fbengine uses on `properties`' device: the device's own, up to the
/// instance's.
std::uint32_t version_of(const VkPhysicalDeviceProperties& properties) {
    return std::min(properties.apiVersion, instance_version);
}

/// The first queue family that draws and, if there's a surface, presents to it; UINT32_MAX if none
/// does.
std::uint32_t queue_family(VkPhysicalDevice physical, VkSurfaceKHR surface) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    for (std::uint32_t family = 0; family < count; ++family) {
        if ((families[family].queueFlags & flag<VkQueueFlags>(VK_QUEUE_GRAPHICS_BIT)) == 0) {
            continue;
        }
        VkBool32 presents = VK_TRUE;
        if (surface != VK_NULL_HANDLE) {
            vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &presents);
        }
        if (presents == VK_TRUE) {
            return family;
        }
    }
    return UINT32_MAX;
}

/// What `physical` lacks of what fbengine needs, "" for nothing: Vulkan 1.2, swapchains, dynamic
/// rendering, synchronization2, descriptor indexing for bindless textures, timeline semaphores,
/// and a queue that draws (and presents to `surface`, if there is one).
std::string lacks_of(VkPhysicalDevice physical, const VkPhysicalDeviceProperties& properties,
                     VkSurfaceKHR surface) {
    if (properties.apiVersion < VK_API_VERSION_1_2) {
        return "Vulkan 1.2";
    }
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
    const bool core13 = version_of(properties) >= VK_API_VERSION_1_3;

    VkPhysicalDeviceSynchronization2Features synchronization2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
    };
    VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
        .pNext = &synchronization2,
    };
    VkPhysicalDeviceVulkan12Features vulkan12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &dynamic_rendering,
    };
    VkPhysicalDeviceFeatures2 features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vulkan12,
    };
    vkGetPhysicalDeviceFeatures2(physical, &features);

    std::vector<std::string_view> missing;
    if (!has_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        missing.emplace_back("swapchains");
    }
    if ((!core13 && !has_extension(extensions, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) ||
        dynamic_rendering.dynamicRendering != VK_TRUE) {
        missing.emplace_back("dynamic rendering");
    }
    if ((!core13 && !has_extension(extensions, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) ||
        synchronization2.synchronization2 != VK_TRUE) {
        missing.emplace_back("synchronization2");
    }
    if (vulkan12.descriptorIndexing != VK_TRUE || vulkan12.runtimeDescriptorArray != VK_TRUE ||
        vulkan12.descriptorBindingPartiallyBound != VK_TRUE ||
        vulkan12.descriptorBindingVariableDescriptorCount != VK_TRUE ||
        vulkan12.descriptorBindingSampledImageUpdateAfterBind != VK_TRUE ||
        vulkan12.shaderSampledImageArrayNonUniformIndexing != VK_TRUE) {
        missing.emplace_back("descriptor indexing");
    }
    if (vulkan12.timelineSemaphore != VK_TRUE) {
        missing.emplace_back("timeline semaphores");
    }
    if (queue_family(physical, VK_NULL_HANDLE) == UINT32_MAX) {
        missing.emplace_back("a queue that draws");
    } else if (surface != VK_NULL_HANDLE && queue_family(physical, surface) == UINT32_MAX) {
        missing.emplace_back("presenting to the window");
    }

    std::string lacks;
    for (const std::string_view name : missing) {
        lacks += lacks.empty() ? "" : ", ";
        lacks += name;
    }
    return lacks;
}

/// Lists the devices, and what each lacks; false, with fbe_error's why, if Vulkan can't start.
bool list_devices(VkSurfaceKHR surface) {
    if (fbe::gpu::instance() == VK_NULL_HANDLE) {
        return false;
    }
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(vulkan, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(vulkan, &count, physicals.data());
    candidates.clear();
    for (VkPhysicalDevice physical : physicals) {
        candidate_t candidate{ .physical = physical };
        vkGetPhysicalDeviceProperties(physical, &candidate.properties);
        candidate.lacks = lacks_of(physical, candidate.properties, surface);
        candidates.push_back(std::move(candidate));
    }
    return true;
}

/// How much fbengine prefers a kind of device, the lowest first.
int preference(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 0;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 1;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return 3;
    default:
        return 4;
    }
}

/// The device to start on: the one asked for, or the most preferred with all fbengine needs, the
/// first of a kind; -1, with fbe_error's why, if there's none.
std::int32_t pick() {
    if (wanted >= 0) {
        if (static_cast<std::size_t>(wanted) >= candidates.size()) {
            fbe::fail(std::format("there's no GPU {}", wanted));
            return -1;
        }
        const candidate_t& candidate = candidates[static_cast<std::size_t>(wanted)];
        if (!candidate.lacks.empty()) {
            fbe::fail(std::format("the GPU {} lacks what fbengine needs: {}",
                                  candidate.properties.deviceName, candidate.lacks));
            return -1;
        }
        return wanted;
    }
    std::int32_t best = -1;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const candidate_t& candidate = candidates[i];
        if (candidate.lacks.empty() &&
            (best < 0 ||
             preference(candidate.properties.deviceType) <
                 preference(candidates[static_cast<std::size_t>(best)].properties.deviceType))) {
            best = static_cast<std::int32_t>(i);
        }
    }
    if (best >= 0) {
        return best;
    }
    if (candidates.empty()) {
        fbe::fail("there's no GPU with Vulkan");
        return -1;
    }
    std::string why;
    for (const candidate_t& candidate : candidates) {
        why += std::format("{}{} lacks {}", why.empty() ? "" : "; ",
                           candidate.properties.deviceName, candidate.lacks);
    }
    fbe::fail(std::format("no GPU has what fbengine needs: {}", why));
    return -1;
}

/// The device, its queue, VMA, and the frames in flight, on `candidate`.
bool make_device(const candidate_t& candidate, VkSurfaceKHR surface) {
    current.instance = vulkan;
    current.physical = candidate.physical;
    current.properties = candidate.properties;
    current.family = queue_family(candidate.physical, surface);
    const bool core13 = version_of(candidate.properties) >= VK_API_VERSION_1_3;

    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(candidate.physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(candidate.physical, nullptr, &count, extensions.data());
    std::vector<const char*> enabled{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    if (!core13) {
        enabled.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        enabled.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    }
    // A portability driver's subset has to be asked for when it's there.
    if (has_extension(extensions, "VK_KHR_portability_subset")) {
        enabled.push_back("VK_KHR_portability_subset");
    }

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(candidate.physical, &supported);
    VkPhysicalDeviceSynchronization2Features synchronization2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
        .synchronization2 = VK_TRUE,
    };
    VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
        .pNext = &synchronization2,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features vulkan12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &dynamic_rendering,
        .descriptorIndexing = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingPartiallyBound = VK_TRUE,
        .descriptorBindingVariableDescriptorCount = VK_TRUE,
        .runtimeDescriptorArray = VK_TRUE,
        .timelineSemaphore = VK_TRUE,
    };
    // Anisotropic filtering and wireframes, where the device has them.
    const VkPhysicalDeviceFeatures2 features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vulkan12,
        .features = {
            .fillModeNonSolid = supported.fillModeNonSolid,
            .samplerAnisotropy = supported.samplerAnisotropy,
        },
    };
    const float priority = 1.0F;
    const VkDeviceQueueCreateInfo queue{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = current.family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
        .enabledExtensionCount = static_cast<std::uint32_t>(enabled.size()),
        .ppEnabledExtensionNames = enabled.data(),
    };
    const VkResult made = vkCreateDevice(candidate.physical, &info, nullptr, &current.device);
    if (made != VK_SUCCESS) {
        current.device = VK_NULL_HANDLE;
        return fbe::gpu::fail_vulkan(
            std::format("couldn't start the GPU {}", candidate.properties.deviceName), made);
    }
    volkLoadDevice(current.device);
    vkGetDeviceQueue(current.device, current.family, 0, &current.queue);
    current.begin_rendering = core13 ? vkCmdBeginRendering : vkCmdBeginRenderingKHR;
    current.end_rendering = core13 ? vkCmdEndRendering : vkCmdEndRenderingKHR;
    current.pipeline_barrier = core13 ? vkCmdPipelineBarrier2 : vkCmdPipelineBarrier2KHR;
    current.queue_submit = core13 ? vkQueueSubmit2 : vkQueueSubmit2KHR;

    VmaAllocatorCreateInfo allocator_info{
        .physicalDevice = current.physical,
        .device = current.device,
        .instance = vulkan,
        .vulkanApiVersion = version_of(candidate.properties),
    };
    VmaVulkanFunctions functions{};
    vmaImportVulkanFunctionsFromVolk(&allocator_info, &functions);
    allocator_info.pVulkanFunctions = &functions;
    const VkResult allocator = vmaCreateAllocator(&allocator_info, &current.allocator);
    if (allocator != VK_SUCCESS) {
        return fbe::gpu::fail_vulkan("couldn't start the GPU's memory allocator", allocator);
    }

    for (fbe::gpu::frame_t& frame : current.frames) {
        const VkCommandPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = flag<VkCommandPoolCreateFlags>(VK_COMMAND_POOL_CREATE_TRANSIENT_BIT),
            .queueFamilyIndex = current.family,
        };
        const VkFenceCreateInfo fence_info{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = flag<VkFenceCreateFlags>(VK_FENCE_CREATE_SIGNALED_BIT),
        };
        const VkSemaphoreCreateInfo semaphore_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        VkResult result = vkCreateCommandPool(current.device, &pool_info, nullptr, &frame.pool);
        if (result == VK_SUCCESS) {
            const VkCommandBufferAllocateInfo commands_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = frame.pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            result = vkAllocateCommandBuffers(current.device, &commands_info, &frame.commands);
        }
        if (result == VK_SUCCESS) {
            result = vkCreateFence(current.device, &fence_info, nullptr, &frame.done);
        }
        if (result == VK_SUCCESS) {
            result = vkCreateSemaphore(current.device, &semaphore_info, nullptr, &frame.acquired);
        }
        if (result != VK_SUCCESS) {
            return fbe::gpu::fail_vulkan("couldn't make the GPU's frames", result);
        }
    }
    return true;
}

/// The kind of a device, as fbe_gpu.h numbers them.
std::int32_t kind_of(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return FBE_GPU_INTEGRATED;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return FBE_GPU_DISCRETE;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return FBE_GPU_VIRTUAL;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return FBE_GPU_CPU;
    default:
        return FBE_GPU_OTHER;
    }
}

/// The device at `index`, if there is one, listing them the first time.
const candidate_t* candidate_at(std::int32_t index) {
    if (candidates.empty() && !list_devices(VK_NULL_HANDLE)) {
        return nullptr;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= candidates.size()) {
        return nullptr;
    }
    return &candidates[static_cast<std::size_t>(index)];
}

} // namespace

namespace fbe::gpu {

bool fail_vulkan(std::string_view doing, VkResult result) {
    return fail(std::format("{} (VkResult {})", doing, static_cast<int>(result)));
}

VkInstance instance() {
    if (!tried) {
        tried = true;
        if (!make_instance()) {
            why_not = fbe_error();
        }
    } else if (vulkan == VK_NULL_HANDLE) {
        fail(why_not);
    }
    return vulkan;
}

device_t* device() {
    return started ? &current : nullptr;
}

bool start(VkSurfaceKHR surface) {
    if (started) {
        return true;
    }
    if (!list_devices(surface)) {
        return false;
    }
    const std::int32_t index = pick();
    if (index < 0) {
        return false;
    }
    if (!make_device(candidates[static_cast<std::size_t>(index)], surface)) {
        // What was made is left for the process to free; a GPU that failed once isn't retried.
        return false;
    }
    picked = index;
    started = true;
    start_pipeline_cache();
    return true;
}

} // namespace fbe::gpu

extern "C" {

FBE_API int32_t fbe_gpu_device_count(void) {
    if (candidates.empty() && !list_devices(VK_NULL_HANDLE)) {
        return -1;
    }
    return static_cast<std::int32_t>(candidates.size());
}

FBE_API const char* fbe_gpu_device_name(int32_t index) {
    const candidate_t* candidate = candidate_at(index);
    return candidate != nullptr ? candidate->properties.deviceName : "";
}

FBE_API int32_t fbe_gpu_device_kind(int32_t index) {
    const candidate_t* candidate = candidate_at(index);
    return candidate != nullptr ? kind_of(candidate->properties.deviceType) : FBE_GPU_OTHER;
}

FBE_API const char* fbe_gpu_device_lacks(int32_t index) {
    const candidate_t* candidate = candidate_at(index);
    return candidate != nullptr ? candidate->lacks.c_str() : "";
}

FBE_API bool fbe_gpu_set_device(int32_t index) {
    if (started) {
        return fbe::fail("the GPU's device is set before it starts");
    }
    if (index < -1 || (index >= 0 && candidate_at(index) == nullptr)) {
        return fbe::fail(std::format("there's no GPU {}", index));
    }
    wanted = index;
    return true;
}

FBE_API int32_t fbe_gpu_device(void) {
    return started ? picked : wanted;
}

FBE_API bool fbe_gpu_start(void) {
    return fbe::gpu::start_with_window();
}

FBE_API bool fbe_gpu_started(void) {
    return started;
}

FBE_API int32_t fbe_gpu_validation_errors(void) {
    return validation_errors;
}

FBE_API bool fbe_gpu_validating(void) {
    return validating;
}

} // extern "C"
