// The bindless set every pipeline reads textures through: a few samplers, one for each filter and
// wrap, made with the global LOD bias and anisotropy, and every texture's image, by its index,
// written as textures are made, while frames that don't read it are in flight.

#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/vulkan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>

namespace {

/// The most textures there can be, unless the device holds fewer.
constexpr std::uint32_t most_textures = 16384;
/// The fewest textures fbengine needs a device to hold.
constexpr std::uint32_t fewest_textures = 1024;
/// The filters and wraps, as fbe_texture.h numbers them.
constexpr std::int32_t filters = 2;
constexpr std::int32_t wraps = 3;
/// The furthest the LOD bias goes, either way, before the device's own limit.
constexpr float furthest_bias = 16.0F;

VkDescriptorSetLayout layout = VK_NULL_HANDLE;
VkDescriptorPool pool = VK_NULL_HANDLE;
VkDescriptorSet set = VK_NULL_HANDLE;
std::uint32_t capacity = 0;
std::array<VkSampler, fbe::gpu::sampler_slots> samplers{};

float lod_bias = 0.0F;
float anisotropy = 1.0F;

void destroy_samplers(const fbe::gpu::device_t& gpu,
                      std::array<VkSampler, fbe::gpu::sampler_slots>& made) {
    for (VkSampler& sampler : made) {
        vkDestroySampler(gpu.device, sampler, nullptr);
        sampler = VK_NULL_HANDLE;
    }
}

VkSamplerAddressMode address_mode(std::int32_t wrap) {
    switch (wrap) {
    case FBE_TEXTURE_CLAMP:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case FBE_TEXTURE_MIRROR:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

/// Makes a sampler for each filter and wrap with the LOD bias and anisotropy, and 2D's, for each
/// filter clamped and repeating, with neither, as a sprite's texels are its own however 3D's are
/// set; and only once they're all made points the set at them and destroys the old ones. False,
/// with fbe_error's why, if one can't be made, leaving the old ones as they were.
bool make_samplers(const fbe::gpu::device_t& gpu) {
    std::array<VkSampler, fbe::gpu::sampler_slots> made{};
    const float bias = std::clamp(lod_bias, -gpu.max_lod_bias, gpu.max_lod_bias);
    const float most = std::min(anisotropy, gpu.max_anisotropy);
    std::array<VkDescriptorImageInfo, fbe::gpu::sampler_slots> infos{};
    const auto make = [&](std::int32_t filter, std::int32_t u, std::int32_t v, float biased,
                          float steepest, std::uint32_t index) {
        const bool linear = filter == FBE_TEXTURE_LINEAR;
        const VkSamplerCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .mipmapMode = linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = address_mode(u),
            .addressModeV = address_mode(v),
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .mipLodBias = biased,
            .anisotropyEnable = linear && steepest > 1.0F ? VK_TRUE : VK_FALSE,
            .maxAnisotropy = linear && steepest > 1.0F ? steepest : 1.0F,
            .maxLod = VK_LOD_CLAMP_NONE,
        };
        const VkResult result = fbe::gpu::faulted(fbe::gpu::fault_t::SAMPLER)
                                    ? VK_ERROR_OUT_OF_HOST_MEMORY
                                    : vkCreateSampler(gpu.device, &info, nullptr, &made.at(index));
        if (result != VK_SUCCESS) {
            destroy_samplers(gpu, made);
            return fbe::gpu::fail_vulkan("couldn't make the GPU's samplers", result);
        }
        return true;
    };
    for (std::int32_t filter = 0; filter < filters; ++filter) {
        for (std::int32_t u = 0; u < wraps; ++u) {
            for (std::int32_t v = 0; v < wraps; ++v) {
                if (!make(filter, u, v, bias, most, fbe::gpu::sampler_index(filter, u, v))) {
                    return false;
                }
            }
        }
        for (const std::int32_t wrap : { FBE_TEXTURE_CLAMP, FBE_TEXTURE_REPEAT }) {
            if (!make(filter, wrap, wrap, 0.0F, 1.0F,
                      fbe::gpu::sampler_2d_index(filter, wrap == FBE_TEXTURE_REPEAT))) {
                return false;
            }
        }
    }
    // The slots past the last are the first's, so every one the set has is written.
    for (std::uint32_t index = 0; index < fbe::gpu::sampler_slots; ++index) {
        infos.at(index).sampler = made.at(index) != VK_NULL_HANDLE ? made.at(index) : made.front();
    }
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = fbe::gpu::sampler_binding,
        .descriptorCount = fbe::gpu::sampler_slots,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
        .pImageInfo = infos.data(),
    };
    vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
    destroy_samplers(gpu, samplers);
    samplers = made;
    return true;
}

/// The samplers made again for a new LOD bias or anisotropy, once the GPU's done with the old.
bool remake_samplers() {
    const fbe::gpu::device_t* gpu = fbe::gpu::device();
    if (gpu == nullptr) {
        return true;
    }
    vkDeviceWaitIdle(gpu->device);
    return make_samplers(*gpu);
}

} // namespace

namespace fbe::gpu {

bool start_bindless() {
    const device_t& gpu = *device();
    VkPhysicalDeviceVulkan12Properties vulkan12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
        .driverID = VK_DRIVER_ID_MESA_LLVMPIPE,
    };
    VkPhysicalDeviceVulkan11Properties vulkan11{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
        .pNext = &vulkan12,
    };
    VkPhysicalDeviceProperties2 properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &vulkan11,
    };
    vkGetPhysicalDeviceProperties2(gpu.physical, &properties);
    capacity = std::min({
        most_textures,
        vulkan12.maxDescriptorSetUpdateAfterBindSampledImages,
        vulkan12.maxPerStageDescriptorUpdateAfterBindSampledImages,
    });

    const auto every_stage = flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL);
    std::array bindings{
        VkDescriptorSetLayoutBinding{
            .binding = sampler_binding,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = sampler_slots,
            .stageFlags = every_stage,
        },
        VkDescriptorSetLayoutBinding{
            .binding = texture_binding,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = capacity,
            .stageFlags = every_stage,
        },
    };
    const auto after_bind =
        flag<VkDescriptorBindingFlags>(VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) |
        flag<VkDescriptorBindingFlags>(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT) |
        flag<VkDescriptorBindingFlags>(VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT);
    const std::array binding_flags{
        after_bind,
        after_bind |
            flag<VkDescriptorBindingFlags>(VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT),
    };
    const VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(binding_flags.size()),
        .pBindingFlags = binding_flags.data(),
    };
    const VkDescriptorSetLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &flags_info,
        .flags = flag<VkDescriptorSetLayoutCreateFlags>(
            VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT),
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };
    // Past the descriptors a set is promised, the device has to say it holds them, which MoltenVK
    // doesn't for what its other limits say; if it doesn't, the textures are what's promised.
    const bool fewest_fault = faulted(fault_t::FEWEST_DESCRIPTORS);
    const bool promised_fault = faulted(fault_t::PROMISED_DESCRIPTORS);
    std::uint32_t promised = vulkan11.maxPerSetDescriptors;
    if (promised_fault || fewest_fault) {
        promised = fewest_fault ? fewest_textures + sampler_slots - 1 : 1212;
    }
    if (capacity + sampler_slots > promised) {
        VkDescriptorSetLayoutSupport support{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,
        };
        vkGetDescriptorSetLayoutSupport(gpu.device, &layout_info, &support);
        if (support.supported != VK_TRUE || promised_fault || fewest_fault) {
            capacity = promised > sampler_slots ? promised - sampler_slots : 0;
            bindings.at(1).descriptorCount = capacity;
        }
    }
    if (capacity < fewest_textures ||
        vulkan12.maxPerStageDescriptorUpdateAfterBindSamplers < sampler_slots) {
        return fail(std::format("the GPU {} holds {} textures at once, and fbengine needs {}",
                                name_of(gpu.properties), capacity, fewest_textures));
    }
    VkResult result = vkCreateDescriptorSetLayout(gpu.device, &layout_info, nullptr, &layout);
    if (result != VK_SUCCESS) {
        return fail_vulkan("couldn't make the bindless set's layout", result);
    }
    const std::array sizes{
        VkDescriptorPoolSize{
            .type = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = sampler_slots,
        },
        VkDescriptorPoolSize{
            .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = capacity,
        },
    };
    const VkDescriptorPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = flag<VkDescriptorPoolCreateFlags>(VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT),
        .maxSets = 1,
        .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
        .pPoolSizes = sizes.data(),
    };
    result = vkCreateDescriptorPool(gpu.device, &pool_info, nullptr, &pool);
    if (result != VK_SUCCESS) {
        return fail_vulkan("couldn't make the bindless set's pool", result);
    }
    const VkDescriptorSetVariableDescriptorCountAllocateInfo count_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .descriptorSetCount = 1,
        .pDescriptorCounts = &capacity,
    };
    const VkDescriptorSetAllocateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = &count_info,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    };
    result = vkAllocateDescriptorSets(gpu.device, &set_info, &set);
    if (result != VK_SUCCESS) {
        return fail_vulkan("couldn't make the bindless set", result);
    }
    return make_samplers(gpu);
}

void stop_bindless() {
    const device_t& gpu = *device();
    destroy_samplers(gpu, samplers);
    vkDestroyDescriptorPool(gpu.device, pool, nullptr);
    vkDestroyDescriptorSetLayout(gpu.device, layout, nullptr);
    pool = VK_NULL_HANDLE;
    layout = VK_NULL_HANDLE;
    set = VK_NULL_HANDLE;
    capacity = 0;
}

VkDescriptorSetLayout bindless_layout() {
    return layout;
}

VkDescriptorSet bindless_set() {
    return set;
}

std::uint32_t texture_capacity() {
    return capacity;
}

void write_texture(std::uint32_t index, VkImageView view) {
    const VkDescriptorImageInfo image{
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = texture_binding,
        .dstArrayElement = index,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo = &image,
    };
    vkUpdateDescriptorSets(device()->device, 1, &write, 0, nullptr);
}

std::uint32_t sampler_index(std::int32_t filter, std::int32_t wrap_u, std::int32_t wrap_v) {
    return static_cast<std::uint32_t>((filter * wraps * wraps) + (wrap_u * wraps) + wrap_v);
}

std::uint32_t sampler_2d_index(std::int32_t filter, bool repeat) {
    static_assert((filters * wraps * wraps) + (filters * 2) <= sampler_slots,
                  "the set has a slot for each of 3D's samplers and 2D's");
    return static_cast<std::uint32_t>((filters * wraps * wraps) + (filter * 2) + (repeat ? 1 : 0));
}

} // namespace fbe::gpu

extern "C" {

FBE_API bool fbe_gpu_set_lod_bias(float bias) {
    if (!std::isfinite(bias) || std::abs(bias) > furthest_bias) {
        return fbe::fail(std::format("a LOD bias is -{0} to {0}, not {1}", furthest_bias, bias));
    }
    // A bias the samplers can't be made with leaves the one they have.
    const float before = lod_bias;
    lod_bias = bias;
    if (!remake_samplers()) {
        lod_bias = before;
        return false;
    }
    return true;
}

FBE_API float fbe_gpu_lod_bias(void) {
    return lod_bias;
}

FBE_API bool fbe_gpu_set_anisotropy(float most) {
    if (!std::isfinite(most) || most < 1.0F || most > furthest_bias) {
        return fbe::fail(
            std::format("anisotropy is 1, for none, to {}, not {}", furthest_bias, most));
    }
    const float before = anisotropy;
    anisotropy = most;
    if (!remake_samplers()) {
        anisotropy = before;
        return false;
    }
    return true;
}

FBE_API float fbe_gpu_anisotropy(void) {
    return anisotropy;
}

FBE_API float fbe_gpu_max_anisotropy(void) {
    const fbe::gpu::device_t* gpu = fbe::gpu::device();
    return gpu != nullptr ? std::max(gpu->max_anisotropy, 1.0F) : 0.0F;
}

} // extern "C"
