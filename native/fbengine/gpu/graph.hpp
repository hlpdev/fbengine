#pragma once

// The frame graph. Everything the frame's commands do to textures is a pass, added as it's asked
// for and recorded at the Flip, or when a readback needs what's been drawn so far: each pass says
// what it draws into, and what else it reads and writes, and the graph drops the passes nothing
// needs, runs the rest in the order they were added, each in the level of the one before unless it
// depends on a pass there, places the barriers each level needs before it, at once, and gives the
// transient targets passes ask for, which the frame makes and drops, images they share when their
// passes don't overlap, kept from frame to frame while they're asked for. Passes keep their order,
// rather than moving up to the first level they could run in, so a transient target lives as long
// as the passes using it were apart as they were added, not longer.

#include "gpu/vulkan.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace fbe::gpu {

/// A texture, a transient target, or an image of the frame's own, as the graph knows it until the
/// graph's next recorded.
using resource_t = std::uint32_t;
constexpr resource_t no_resource = UINT32_MAX;

/// What a pass does with a texture besides drawing into it.
enum class use_t : std::uint8_t {
    /// Samples it in its shaders.
    SAMPLED,
    /// Reads a multisampled transient target's samples in its shaders, one by one, as they were
    /// drawn, not what they resolve to, by resource_samples_index.
    SAMPLES_READ,
    /// Copies or blits from it.
    COPY_FROM,
    /// Clears, copies, or blits into it, any level.
    COPY_TO,
    /// Copies or blits into its first level, then makes the others from it, as a copy into a
    /// texture with mipmaps does, leaving every level to be sampled.
    MIPMAPPED,
};

/// What came of a pass, as its `settled` is told.
enum class settled_t : std::uint8_t {
    /// A submit took it.
    RAN,
    /// Nothing of it happened, and it can happen yet: a transient target it needs couldn't be made,
    /// or the submit it was recorded for failed.
    MISSED,
    /// It never will: the GPU's lost or stopping, or nothing needed it.
    GIVEN_UP,
};

/// A target a pass draws into: cleared to `rgba`, linear, as the pass begins, or drawn on as it is.
struct color_t {
    resource_t target = no_resource;
    bool clear = false;
    std::array<float, 4> rgba{};
    /// Whether it's drawn through its target's view of its stored bytes, if it has one.
    bool stored = false;
};

/// A pass: the targets it draws into, if it draws, and the one whose depth it tests and writes,
/// cleared to `depth_value` first if `clear_depth` says, all one size and drawing as many samples;
/// what else it does with textures; and `record`, which records its commands. The graph begins
/// drawing into its targets, with the viewport and scissor their whole, before `record`, and ends
/// it after, resolving a multisampled target into its texture; a pass that draws into nothing,
/// as a copy, records outside drawing. Its name, which lasts, as a literal does, is what
/// recorded_t says. A pass that's `carried`, as a write of pixels into a texture is, goes into the
/// graph again if the submit it was recorded for fails, as the frame's uploads are carried; it
/// uses textures only, and what it records stays good until a submit runs it. `settled`, if it has
/// one, is told what came of it, once, outside the graph's recording, so it can add passes: as a
/// transient target's id isn't good in another frame, it's what does a pass that uses one again.
struct pass_t {
    std::string_view name;
    std::vector<color_t> colors;
    resource_t depth = no_resource;
    bool clear_depth = false;
    float depth_value = 0.0F;
    std::vector<std::pair<resource_t, use_t>> uses;
    std::function<void(VkCommandBuffer)> record;
    bool carried = false;
    std::function<void(settled_t)> settled;
};

/// A live texture, by its handle, at rest as it is between frames' commands; the same resource
/// each time it's asked for; no_resource, with fbe_error's why, if it isn't live.
resource_t texture_resource(std::uint64_t handle);

/// A target a pass draws into that the frame makes for itself: `width` by `height` of an
/// FBE_TEXTURE_ format, drawing `samples` a pixel, with depth if it says. What's in it isn't kept
/// past the graph's recording, so what draws into it first clears it, or draws all of it.
struct transient_t {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t format = 0;
    std::uint32_t samples = 1;
    bool depth = false;
    /// Whether a shader reads its samples one by one, as an HDR camera's tonemap does, so it takes
    /// a slot of the set's for them, if there's one, and the device can.
    bool reads_samples = false;
    bool operator==(const transient_t&) const = default;
};

/// A new transient target in the graph, given an image as the graph's recorded; no_resource, with
/// fbe_error's why, if it can't be one.
resource_t transient_resource(const transient_t& target);

/// An image of the frame's own, drawn into, as the window's: what's in it as the graph begins
/// isn't kept, its first use waits for `waits`' stages of what came before, and the graph leaves
/// it in `layout`, for `stages` to use as `access` says.
struct external_t {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkPipelineStageFlags2 waits = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stages = 0;
    VkAccessFlags2 access = 0;
};

resource_t external_resource(const external_t& image);

/// Has `flush` run whenever the graph's about to take a pass, say whether a texture's in it, or be
/// recorded, so what a batch holds back, as 2D's does, goes in first, in its turn; and `drop` run
/// when the graph's passes are dropped, as a lost GPU's are, or the GPU stops.
void hold_back(void (*flush)(), void (*drop)());

/// Whether a texture's in the graph, as a pass added since it was last recorded uses it, so what's
/// done with it next, as a write of its pixels, is a pass after those.
bool in_graph(std::uint64_t handle);

/// Adds a pass to the graph, after those added before, which it runs after; false, with
/// fbe_error's why, if what it uses isn't in the graph, isn't a target where it draws, is used
/// twice, or its targets don't match.
bool add_pass(pass_t pass);

/// How many samples a pixel of a resource draws, as soon as it's in the graph.
std::uint32_t resource_samples(resource_t resource);

/// Makes sure there are `count` images for transient targets of `target`, which passes that ask for
/// them in the frame's graph then have, as many as are needed at once: false, with fbe_error's why,
/// if one can't be made now, where what asks can say so, rather than the frame dropping its passes
/// when it's recorded.
bool reserve_transient(const transient_t& target, std::uint32_t count = 1);

/// A texture's or transient target's index in the bindless set, which a pass's `record` gives its
/// shaders to sample it by, its format, and its texture's handle; while the graph's recorded only.
std::uint32_t resource_index(resource_t resource);
/// A multisampled transient target's slot for its samples, no_samples_slot if it has none, where
/// what reads them reads what they resolve to, and the graph leaves them as they are; while the
/// graph's recorded only.
std::uint32_t resource_samples_index(resource_t resource);
VkFormat resource_format(resource_t resource);
std::uint64_t resource_texture(resource_t resource);
/// Its image, for a pass's commands: the frame's, however its texture's been let go of since it was
/// added; a transient's once the graph's recording.
VkImage resource_image(resource_t resource);

/// Records the passes added since the graph was last recorded into the frame's commands, and
/// empties it; false, with fbe_error's why, if a transient target couldn't be made, whose passes
/// are dropped. The frame's submits record it first.
bool record_graph();

/// Drops the passes added since the graph was last recorded, unrecorded, as a lost GPU runs none.
void drop_passes();

/// Says the submit of what the graph last recorded failed, which their `settled` are told as it's
/// next recorded, once the frame that failed is put right: what they do can begin a frame.
void miss_passes();

/// Adds the carried passes the graph last recorded to it again, first, after the submit they were
/// recorded for failed.
void carry_passes();

/// What the graph's last recording did: the passes it recorded, by name, in order, and the level
/// each was in; how many it dropped; the image barriers it placed, and the commands that placed
/// them; the transient targets' images it made; and the size of each transient target it gave one.
struct recorded_t {
    std::vector<std::string_view> passes;
    std::vector<std::uint32_t> levels;
    std::uint32_t dropped = 0;
    std::uint32_t barriers = 0;
    std::uint32_t batches = 0;
    std::uint32_t made = 0;
    std::vector<std::array<std::uint32_t, 2>> transient_sizes;
};

const recorded_t& last_recorded();

/// How many images transient targets have, kept for the frames that ask for them again.
std::uint32_t transient_images();

/// Counts a frame submitted, letting go of the transient targets' images no frame asked for in
/// the last few.
void age_transients();

/// Drops what the graph has, as the GPU stops.
void stop_graph();

} // namespace fbe::gpu
