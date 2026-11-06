#pragma once

// What draws with textures reads of them: the texture a handle is for, its index in the bindless
// set, and its sampler's; how a texture rests between what the frame's commands do with it, which
// the frame graph's passes draw into, copy, and clear; and targets the engine makes for itself.

#include "../../common/handles.hpp"
#include "assets/image.hpp"
#include "gpu/vulkan.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fbe::texture {

/// Handles' kind for textures, handles.hpp's.
constexpr std::uint8_t kind = fbe::TEXTURE_KIND;

/// The stages that sample textures.
constexpr auto shader_stages =
    gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT) |
    gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) |
    gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
/// A texture's image rests, between what the frame's commands do with it, as the uploads, run
/// before them, need: every level SHADER_READ_ONLY, what wrote it last seen by these stages, which
/// whatever uses it next waits for: a shader sampling it, a copy or blit out of it, or a pass's
/// drawing. A multisampled target's samples rest drawn into, COLOR_ATTACHMENT, and a target's
/// depth as depth, DEPTH_STENCIL_ATTACHMENT, the next pass waiting for the last's writes.
constexpr auto resting_stages =
    shader_stages | gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COPY_BIT) |
    gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_BLIT_BIT) |
    gpu::flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);

/// A texture: its image on the GPU, a view of every level, and how it's sampled. Its slot is its
/// index in the bindless set.
struct texture_t {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::int32_t format = 0;
    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 1;
    std::int32_t filter = 0;
    std::int32_t wrap_u = 0;
    std::int32_t wrap_v = 0;
    std::uint32_t references = 0;
    /// Whether its alpha's for blending, which a material blending AUTO goes by.
    bool alpha = false;
    /// The file it was loaded from, "" for none.
    std::string path;
    /// How many times its pixels have changed on the GPU, which a CPU copy of them was read at.
    std::uint64_t version = 0;
    /// Whether it's the engine's own, as the frame graph's transient targets' images are, which
    /// Texture.Count doesn't count.
    bool engine = false;
    /// Whether it's drawn into, a target, and the samples a pass draws, which a multisampled
    /// target's resolve into its image at the pass's end.
    bool target = false;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImage msaa = VK_NULL_HANDLE;
    VmaAllocation msaa_memory = VK_NULL_HANDLE;
    VkImageView msaa_view = VK_NULL_HANDLE;
    /// A target of sRGB bytes' views that write those bytes as they are, for a camera that draws
    /// stored values; nothing for any other texture.
    VkImageView stored_view = VK_NULL_HANDLE;
    VkImageView stored_msaa_view = VK_NULL_HANDLE;
    /// Where a shader reads those samples, one by one, if it can: the engine's own only.
    std::uint32_t samples_slot = fbe::gpu::no_samples_slot;
    /// A target's depth, if it has one: 32-bit floats, reversed, 1 nearest and 0 farthest, as
    /// many samples as it draws.
    VkImage depth = VK_NULL_HANDLE;
    VmaAllocation depth_memory = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
};

/// A target's depth's format.
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
/// A float of red alone, as a shadow's depth is kept: a format only the engine's own textures have.
constexpr std::int32_t shadow_format = 4;

/// The live texture a handle is for, or nullptr, with fbe_error's why. The pool it's in grows as
/// textures are made, which recording the frame graph can do, so the pointer's good only until
/// then.
texture_t* find(std::uint64_t handle);

/// A live texture's index in the bindless set, which is its slot, and its sampler's; the white
/// texture, index 0, is what a draw with no texture reads.
std::uint32_t index_of(std::uint64_t handle);
std::uint32_t sampler_of(const texture_t& texture);
constexpr std::uint32_t white = 0;

/// The bytes of a level of a texture, read back from the GPU after what the frame recorded so far,
/// its rows packed; empty, with fbe_error's why, if they can't be.
std::vector<std::uint8_t> read(std::uint64_t handle, std::uint32_t level);

/// Sends what was written into a texture's CPU copy of its pixels (fbe_texture_pixels) to the GPU,
/// if anything was, as a write in its turn: what uses a texture on the GPU calls it first.
void send_pixels(std::uint64_t handle);

/// Counts a change the GPU makes to a texture's pixels, which a CPU copy of them is then older
/// than: what writes a texture on the GPU calls it.
void changed_on_gpu(std::uint64_t handle);

/// How many times a CPU copy of a texture's pixels has been read back from the GPU.
std::uint64_t pixel_readbacks();

/// How many bytes a pixel of an FBE_TEXTURE_ format has; 0 for none.
std::uint32_t pixel_size(std::int32_t format);

/// A target the engine makes for itself, `width` by `height` of an FBE_TEXTURE_ format, drawing
/// `samples` a pixel, with depth if it says, which Texture.Count doesn't count and
/// fbe_texture_release lets go of, with a slot for its samples if a shader `reads_samples`; its
/// handle, or 0, with fbe_error's why, if it can't be made.
std::uint64_t create_engine_target(std::uint32_t width, std::uint32_t height, std::int32_t format,
                                   VkSampleCountFlagBits samples, bool depth,
                                   bool reads_samples = false);

/// A texture the engine makes for itself, `width` by `height` of an FBE_TEXTURE_ format, with room
/// for mipmaps if it says, as create_engine_target's are; 0, with fbe_error's why, without a
/// started GPU.
std::uint64_t create_engine_texture(std::uint32_t width, std::uint32_t height, std::int32_t format,
                                    bool mipmaps = false);

/// Writes `width` by `height` pixels at `x`, `y` into level 0 of a texture without mipmaps, rows
/// `pitch` bytes apart, in the frame's uploads, before its commands, even where a pass of the
/// frame's graph uses it, so a 2D batch drawing it isn't sent first: only where nothing the frame
/// draws samples, as an atlas page's new glyphs are. False, with fbe_error's why, if it can't.
bool write_ahead(std::uint64_t handle, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                 std::uint32_t height, const std::uint8_t* pixels, std::uint32_t pitch);

/// What a Basis Universal KTX2 is transcoded to for the started device: BC7 if it samples that,
/// else ASTC 4x4, else RGBA8, as it is without a started device.
assets::transcode_t device_transcode();

} // namespace fbe::texture
