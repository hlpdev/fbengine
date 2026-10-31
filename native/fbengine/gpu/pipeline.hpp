#pragma once

// Graphics pipelines: one layout for them all, each made once for what it's described as, through
// Vulkan's pipeline cache, which is kept on disk between runs.

#include "gpu/shader.hpp"
#include "gpu/vulkan.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fbe::gpu {

/// How a pipeline blends what it draws with what's there.
enum class blend_t : std::uint8_t {
    /// Replaces it.
    NONE,
    /// By the source's alpha.
    ALPHA,
    /// By the source's alpha, already multiplied into its color.
    PREMULTIPLIED,
    /// Adds to it.
    ADD,
    /// Multiplies it.
    MULTIPLY,
};

/// What a graphics pipeline is: its shader's entry points, what it draws into, and how. Two
/// pipelines described the same are the same one.
struct pipeline_desc_t {
    const shader_t* shader = nullptr;
    /// The entry points, which outlive the pipeline, as literals do.
    std::string_view vertex = "vertexMain";
    std::string_view fragment = "fragmentMain";
    /// The color target's format, or none; and the depth target's.
    VkFormat color = VK_FORMAT_UNDEFINED;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPolygonMode polygon = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    blend_t blend = blend_t::NONE;
    bool depth_test = false;
    bool depth_write = false;
    /// Depth is reversed, 1 near and 0 far, so nearer passes by default.
    VkCompareOp depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL;

    bool operator==(const pipeline_desc_t&) const = default;
};

/// How many bytes of push constants every pipeline takes, all that Vulkan guarantees.
constexpr std::uint32_t push_constant_size = 128;

/// The layout every pipeline has: push constants, to every stage; VK_NULL_HANDLE, with fbe_error's
/// why, if it can't be made.
VkPipelineLayout pipeline_layout();

/// The pipeline `desc` describes, made the first time it's asked for; VK_NULL_HANDLE, with
/// fbe_error's why, if it can't be.
VkPipeline pipeline(const pipeline_desc_t& desc);

/// Loads the pipeline cache as the device starts: the file fbe_gpu_set_pipeline_cache names, if
/// it's this device's and whole, or an empty cache.
void start_pipeline_cache();

/// Writes the pipeline cache to its file if pipelines were made since it was last written.
void save_pipeline_cache();

/// A pipeline cache's data, with a header saying which device and driver made it and a checksum,
/// as fbengine keeps it on disk.
std::vector<std::byte> pack_pipeline_cache(std::span<const std::byte> data,
                                           const VkPhysicalDeviceProperties& properties);

/// The data in what pack_pipeline_cache packed, if it's whole and `properties`' device and
/// driver made it; empty if not.
std::vector<std::byte> unpack_pipeline_cache(std::span<const std::byte> file,
                                             const VkPhysicalDeviceProperties& properties);

/// Where the pipeline cache goes unless a program says: the user's cache folder's
/// fbengine/<program>/pipelines.bin, the program being the executable's name.
std::string default_pipeline_cache();

} // namespace fbe::gpu
