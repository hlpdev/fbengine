#pragma once

// Graphics pipelines: one layout for them all, each made once for what it's described as, through
// Vulkan's pipeline cache, which is kept on disk between runs.

#include "gpu/shader.hpp"
#include "gpu/vulkan.hpp"

#include <array>
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
    /// Holds depth to the depth range instead of clipping there, where gpu::device()->depth_clamp.
    bool depth_clamp = false;
    /// Depth is reversed, 1 near and 0 far, so nearer passes by default.
    VkCompareOp depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    /// The shader's first `constant_count` specialization constants, by their ids from 0, which
    /// make one shader a family of pipelines, each as fast as if it were written for what it draws.
    std::array<std::uint32_t, 16> constants{};
    std::uint32_t constant_count = 0;

    bool operator==(const pipeline_desc_t&) const = default;
};

/// How many bytes of push constants every pipeline takes, all that Vulkan guarantees.
constexpr std::uint32_t push_constant_size = 128;

/// The layout every pipeline has: the bindless set as set 0, and push constants, to every stage;
/// VK_NULL_HANDLE, with fbe_error's why, if it can't be made.
VkPipelineLayout pipeline_layout();

/// The pipeline `desc` describes, made the first time it's asked for; VK_NULL_HANDLE, with
/// fbe_error's why, if it can't be.
VkPipeline pipeline(const pipeline_desc_t& desc);

/// For a test: pipelines of a shader of `name` aren't made, as a driver might refuse them, "" for
/// all to be; and how many have been asked for since.
void refuse_pipelines_of(std::string_view name);
std::uint32_t pipelines_refused();

/// How many pipelines have been made since the GPU started, for a test.
std::size_t pipelines_made();

/// Loads the pipeline cache as the device starts: the file fbe_gpu_set_pipeline_cache names, if
/// it's this device's and whole, or an empty cache.
void start_pipeline_cache();

/// Writes the pipeline cache to its file if pipelines were made since it was last looked at and
/// its data differs from what was read or last written; while the GPU runs, not within a couple of
/// seconds of the last write, unless `now`, as stopping writes it.
void save_pipeline_cache(bool now = false);

/// The data in the pipeline cache file at `path`, if it's whole and `properties`' device and
/// driver wrote it, which the cache starts with; empty if not, or if `path` is "".
std::vector<std::byte> pipeline_cache_from(const std::string& path,
                                           const VkPhysicalDeviceProperties& properties);

/// The pipeline cache's data now, as the driver gives it.
std::vector<std::byte> pipeline_cache_data();

/// Whether pipelines were made since the cache was last looked at, which a later Flip does; how
/// many bytes the cache started with as the GPU last started; and how many times it's been
/// written.
bool pipeline_cache_pending();
std::size_t pipeline_cache_loaded();
std::int32_t pipeline_cache_writes();

/// How long the cache waits after a write before it's written again while the GPU runs, 2
/// seconds unless a test says.
void set_pipeline_cache_interval(double seconds);

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
