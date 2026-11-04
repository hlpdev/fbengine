// The frame graph: passes kept as they're added, each expanded into the images it uses and how;
// at the recording, the passes nothing needs dropped, last first, the rest put in levels in their
// order, transient targets given images, and the levels recorded in order, each one's barriers
// first, in one command, then textures left at rest and the frame's own images as they're asked
// to be.

#include "gpu/graph.hpp"

#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/image.hpp"
#include "texture/texture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using fbe::gpu::flag;
using fbe::gpu::resource_t;
using fbe::gpu::use_t;

/// How an image is used: its layout, the stages that use it and how, and whether that writes it.
struct state_t {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stages = 0;
    VkAccessFlags2 access = 0;
    bool writes = false;
};

constexpr auto output =
    flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
constexpr auto tests = flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT) |
                       flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
constexpr auto copies = flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COPY_BIT) |
                        flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_BLIT_BIT);
constexpr auto sampled_read = flag<VkAccessFlags2>(VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

constexpr state_t drawn{
    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    .stages = output,
    .access = flag<VkAccessFlags2>(VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT) |
              flag<VkAccessFlags2>(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
    .writes = true,
};
constexpr state_t depth_drawn{
    .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    .stages = tests,
    .access = flag<VkAccessFlags2>(VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT) |
              flag<VkAccessFlags2>(VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
    .writes = true,
};
constexpr state_t sampled{
    .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    .stages = fbe::texture::shader_stages,
    .access = sampled_read,
};
constexpr state_t copied_from{
    .layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    .stages = copies,
    .access = flag<VkAccessFlags2>(VK_ACCESS_2_TRANSFER_READ_BIT),
};
constexpr state_t copied_to{
    .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    .stages = copies | flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_CLEAR_BIT),
    .access = flag<VkAccessFlags2>(VK_ACCESS_2_TRANSFER_WRITE_BIT),
    .writes = true,
};
/// A texture's own image at rest, as the frame's commands find it and leave it, and as making its
/// mipmaps leaves it.
constexpr state_t resting{
    .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    .stages = fbe::texture::resting_stages,
    .access = sampled_read,
};

state_t state_of(use_t use) {
    switch (use) {
    case use_t::SAMPLED:
        return sampled;
    case use_t::COPY_FROM:
        return copied_from;
    case use_t::COPY_TO:
    case use_t::MIPMAPPED:
        return copied_to;
    }
    return sampled;
}

/// What a resource is: a texture, a transient target, or an image of the frame's own.
enum class kind_t : std::uint8_t { TEXTURE, TRANSIENT, EXTERNAL };

/// The parts of a resource each have an image: its own, a multisampled target's samples, which
/// resolve into it, and a target's depth.
enum class part_t : std::uint8_t { IMAGE, SAMPLES, DEPTH };
constexpr std::uint32_t parts = 3;
constexpr std::uint32_t none = UINT32_MAX;

/// An image the recording uses, and how it was used last.
struct image_t {
    VkImage image = VK_NULL_HANDLE;
    VkImageAspectFlags aspect = 0;
    std::uint32_t levels = 1;
    state_t state;
};

/// A resource in the graph: what it is; its texture, as it was when it was asked for, or a
/// transient's image's, once it's given; its images in the recording, by part; and the levels it's
/// used from and to.
struct resource_record_t {
    kind_t kind = kind_t::TEXTURE;
    std::uint64_t handle = 0;
    fbe::texture::texture_t texture;
    std::uint32_t slot = fbe::texture::white;
    fbe::gpu::transient_t transient;
    fbe::gpu::external_t external;
    std::array<std::uint32_t, parts> images{ none, none, none };
    std::int32_t first = -1;
    std::int32_t last = -1;
    bool failed = false;
};

/// What a pass does with one of its resources' images: how it uses it, how that leaves it, and
/// whether it's all written, so what was in it before isn't needed.
struct access_t {
    resource_t resource = 0;
    part_t part = part_t::IMAGE;
    state_t state;
    bool mipmaps = false;
    bool overwrites = false;
};

/// A pass as it's added: its accesses, at `first` in the list of them, and, as it's recorded,
/// whether it's kept and its level.
struct added_t {
    fbe::gpu::pass_t pass;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    bool kept = true;
    /// Whether anything needs it, whether or not it can be recorded.
    bool needed = true;
    std::int32_t level = 0;
};

/// A transient target's image, kept from frame to frame: the target it's for, its texture, how
/// each part of it was left, the last level of this recording it's given to a target until, the
/// frame it was last asked for, and its images' place in the recording's, if it's in it.
struct pooled_t {
    fbe::gpu::transient_t target;
    std::uint64_t handle = 0;
    std::array<state_t, parts> states;
    std::int32_t busy = -1;
    std::uint64_t asked = 0;
    std::uint32_t images = none;
};

std::vector<resource_record_t> resources;
std::vector<added_t> added;
std::vector<access_t> accesses;
std::vector<image_t> images;
std::vector<pooled_t> pool;
fbe::gpu::recorded_t recorded;

/// A carried pass the graph last recorded, its textures by handle, kept for a failed submit.
struct carried_t {
    std::string_view name;
    std::vector<std::pair<std::uint64_t, use_t>> uses;
    std::function<void(VkCommandBuffer)> record;
};
std::vector<carried_t> carriable;
/// What to tell of the passes the last recording recorded, once their submit's known.
std::vector<std::function<void(fbe::gpu::settled_t)>> awaiting;
/// Whether their submit failed.
bool awaiting_missed = false;
bool recording = false;
/// What sends a batch held back to the graph, and drops it, and whether it's sending it.
void (*held_back)() = nullptr;
void (*drop_held)() = nullptr;
bool flushing = false;
/// Frames submitted, which say how long ago a transient's image was last asked for.
std::uint64_t frames = 0;
/// How many frames a transient's image is kept for without being asked for.
constexpr std::uint64_t kept_frames = 3;

/// The resources textures are in the graph as, by handle.
std::unordered_map<std::uint64_t, resource_t>& by_handle() {
    static std::unordered_map<std::uint64_t, resource_t> resources_of;
    return resources_of;
}

/// What a pass does with each of its images.
std::span<const access_t> uses_of(const added_t& pass) {
    return { accesses.data() + pass.first, pass.count };
}

std::uint32_t key_of(resource_t resource, part_t part) {
    return (resource * parts) + static_cast<std::uint32_t>(part);
}

/// The images a texture's parts start the recording as: its own at rest, a multisampled one's
/// samples drawn into, and its depth as depth, as textures leave them.
std::array<state_t, parts> texture_states() {
    return { resting, drawn, depth_drawn };
}

/// Adds the images of a texture's parts to the recording, as `states` say they were left, the
/// first's index.
std::uint32_t add_images(const fbe::texture::texture_t& texture,
                         const std::array<state_t, parts>& states) {
    const auto first = static_cast<std::uint32_t>(images.size());
    images.push_back({
        .image = texture.image,
        .aspect = flag<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
        .levels = texture.levels,
        .state = states[0],
    });
    images.push_back({
        .image = texture.msaa,
        .aspect = flag<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
        .state = states[1],
    });
    images.push_back({
        .image = texture.depth,
        .aspect = flag<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT),
        .state = states[2],
    });
    return first;
}

/// Points a texture resource's parts at its images from `first`, those it has.
void point_at(resource_record_t& resource, std::uint32_t first) {
    resource.images = {
        first,
        resource.texture.msaa != VK_NULL_HANDLE ? first + 1 : none,
        resource.texture.depth != VK_NULL_HANDLE ? first + 2 : none,
    };
}

bool is_target(const resource_record_t& resource) {
    return resource.kind != kind_t::TEXTURE || resource.texture.target;
}

VkExtent2D extent_of(const resource_record_t& resource) {
    switch (resource.kind) {
    case kind_t::TEXTURE:
        return { .width = resource.texture.width, .height = resource.texture.height };
    case kind_t::TRANSIENT:
        return { .width = resource.transient.width, .height = resource.transient.height };
    case kind_t::EXTERNAL:
        return resource.external.extent;
    }
    return {};
}

std::uint32_t samples_of(const resource_record_t& resource) {
    switch (resource.kind) {
    case kind_t::TEXTURE:
        return static_cast<std::uint32_t>(resource.texture.samples);
    case kind_t::TRANSIENT:
        return resource.transient.samples;
    case kind_t::EXTERNAL:
        return 1;
    }
    return 1;
}

bool has_depth(const resource_record_t& resource) {
    switch (resource.kind) {
    case kind_t::TEXTURE:
        return resource.texture.depth != VK_NULL_HANDLE;
    case kind_t::TRANSIENT:
        return resource.transient.depth;
    case kind_t::EXTERNAL:
        return false;
    }
    return false;
}

/// Expands a pass into what it does with each image, after the accesses already there; false,
/// with fbe_error's why, if it can't be.
bool expand(const fbe::gpu::pass_t& pass) {
    const auto count = static_cast<resource_t>(resources.size());
    const auto known = [count](resource_t resource) { return resource < count; };
    VkExtent2D extent{};
    std::uint32_t samples = 0;
    const auto matches = [&extent, &samples](const resource_record_t& resource) {
        const VkExtent2D size = extent_of(resource);
        if (samples == 0) {
            extent = size;
            samples = samples_of(resource);
        }
        return size.width == extent.width && size.height == extent.height &&
               samples_of(resource) == samples;
    };
    for (const fbe::gpu::color_t& color : pass.colors) {
        if (!known(color.target)) {
            return fbe::fail("a pass's target isn't in the frame's graph");
        }
        const resource_record_t& target = resources[color.target];
        if (!is_target(target)) {
            return fbe::fail("the texture isn't a target, which draws");
        }
        if (!matches(target)) {
            return fbe::fail("a pass's targets are one size and draw as many samples");
        }
        const bool multisampled = samples_of(target) > 1;
        // A multisampled target's resolve writes all of its own image.
        accesses.push_back({
            .resource = color.target,
            .state = drawn,
            .overwrites = color.clear || multisampled,
        });
        if (multisampled) {
            accesses.push_back({
                .resource = color.target,
                .part = part_t::SAMPLES,
                .state = drawn,
                .overwrites = color.clear,
            });
        }
    }
    if (pass.depth != fbe::gpu::no_resource) {
        if (!known(pass.depth)) {
            return fbe::fail("a pass's depth isn't in the frame's graph");
        }
        const resource_record_t& target = resources[pass.depth];
        if (!has_depth(target)) {
            return fbe::fail("a pass's depth is a target's with depth");
        }
        if (!matches(target)) {
            return fbe::fail("a pass's targets are one size and draw as many samples");
        }
        accesses.push_back({
            .resource = pass.depth,
            .part = part_t::DEPTH,
            .state = depth_drawn,
            .overwrites = pass.clear_depth,
        });
    }
    for (const auto& [resource, use] : pass.uses) {
        if (!known(resource)) {
            return fbe::fail("a texture a pass uses isn't in the frame's graph");
        }
        if (resources[resource].kind == kind_t::EXTERNAL) {
            return fbe::fail("an image of the frame's own is only drawn into");
        }
        accesses.push_back({
            .resource = resource,
            .state = state_of(use),
            .mipmaps = use == use_t::MIPMAPPED,
        });
    }
    return true;
}

/// Each image part's mark, the number of the last pass checked that uses it, so a pass's checked
/// in one go through its accesses.
std::vector<std::uint64_t> marks;
std::uint64_t checked = 0;

/// Whether a pass's accesses from `first` use each image once.
bool once_each(std::size_t first) {
    ++checked;
    marks.resize(std::max(marks.size(), resources.size() * parts), 0);
    for (std::size_t at = first; at < accesses.size(); ++at) {
        std::uint64_t& mark = marks[key_of(accesses[at].resource, accesses[at].part)];
        if (mark == checked) {
            return false;
        }
        mark = checked;
    }
    return true;
}

/// Drops the passes nothing needs, last first: a pass is kept if it writes a texture or an image
/// of the frame's own, or a transient target that a kept pass after it reads, or what it draws
/// on, or if it writes nothing the graph knows.
void drop_unneeded() {
    std::vector<bool> needed(resources.size() * parts, false);
    for (added_t& pass : std::views::reverse(added)) {
        const std::span<const access_t> uses = uses_of(pass);
        bool writes = false;
        bool kept = false;
        for (const access_t& use : uses) {
            if (use.state.writes) {
                writes = true;
                kept = kept || resources[use.resource].kind != kind_t::TRANSIENT ||
                       needed[key_of(use.resource, use.part)];
            }
        }
        pass.kept = kept || !writes;
        if (!pass.kept) {
            continue;
        }
        for (const access_t& use : uses) {
            if (use.state.writes && use.overwrites) {
                needed[key_of(use.resource, use.part)] = false;
            }
        }
        for (const access_t& use : uses) {
            if (!use.state.writes || !use.overwrites) {
                needed[key_of(use.resource, use.part)] = true;
            }
        }
    }
}

/// How an image's been used so far in the levels: the last level that wrote it, and whether it's
/// been read since, in which layout, from which level, and up to which.
struct track_t {
    std::int32_t written = -1;
    bool reading = false;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::int32_t since = 0;
    std::int32_t reads = -1;
};

/// Puts each kept pass in the level of the pass before it, unless it depends on a pass there, and
/// then the next: on one that wrote an image it uses, one that read an image it writes, or one
/// that read an image in another layout; reads in one layout share a level. The levels there are.
std::int32_t level_passes() {
    std::vector<track_t> tracks(resources.size() * parts);
    std::int32_t levels = 0;
    for (added_t& pass : added) {
        if (!pass.kept) {
            continue;
        }
        const std::span<const access_t> uses = uses_of(pass);
        std::int32_t level = std::max(levels - 1, 0);
        for (const access_t& use : uses) {
            const track_t& track = tracks[key_of(use.resource, use.part)];
            if (!use.state.writes && track.reading && track.layout == use.state.layout) {
                level = std::max(level, track.since);
            } else {
                level = std::max({ level, track.written + 1, track.reads + 1 });
            }
        }
        pass.level = level;
        levels = std::max(levels, level + 1);
        for (const access_t& use : uses) {
            track_t& track = tracks[key_of(use.resource, use.part)];
            if (use.state.writes) {
                track = { .written = level };
            } else if (track.reading && track.layout == use.state.layout) {
                track.reads = std::max(track.reads, level);
            } else {
                track.reading = true;
                track.layout = use.state.layout;
                track.since = level;
                track.reads = level;
            }
            resource_record_t& resource = resources[use.resource];
            resource.first = resource.first < 0 ? level : std::min(resource.first, level);
            resource.last = std::max(resource.last, level);
        }
    }
    return levels;
}

/// Drops each kept pass that reads a transient target's image no kept pass before it drew, or
/// draws on one without drawing all of it, as when what drew it was dropped for a transient that
/// couldn't be made; what it would have drawn into transient targets is then missing too, for the
/// passes after.
void drop_unwritten() {
    std::vector<bool> written(resources.size() * parts, false);
    for (added_t& pass : added) {
        if (!pass.kept) {
            continue;
        }
        const std::span<const access_t> uses = uses_of(pass);
        pass.kept = std::ranges::none_of(uses, [&written](const access_t& use) {
            return resources[use.resource].kind == kind_t::TRANSIENT &&
                   (!use.state.writes || !use.overwrites) &&
                   !written[key_of(use.resource, use.part)];
        });
        if (!pass.kept) {
            continue;
        }
        for (const access_t& use : uses) {
            if (use.state.writes) {
                written[key_of(use.resource, use.part)] = true;
            }
        }
    }
}

/// Gives each transient target the kept passes use an image: one kept for a target like it that
/// no target has at the levels it's used, or a new one; false, with fbe_error's why, if one
/// couldn't be made, leaving that target failed.
bool give_images() {
    std::vector<resource_t> transients;
    for (resource_t resource = 0; resource < resources.size(); ++resource) {
        if (resources[resource].kind == kind_t::TRANSIENT && resources[resource].first >= 0) {
            transients.push_back(resource);
        }
    }
    std::ranges::stable_sort(transients, {},
                             [](resource_t resource) { return resources[resource].first; });
    for (pooled_t& pooled : pool) {
        pooled.busy = -1;
        pooled.images = none;
    }
    bool made_all = true;
    for (const resource_t transient : transients) {
        resource_record_t& resource = resources[transient];
        auto found = std::ranges::find_if(pool, [&resource](const pooled_t& pooled) {
            return pooled.target == resource.transient && pooled.busy < resource.first;
        });
        if (found == pool.end()) {
            const std::uint64_t handle = fbe::texture::create_engine_target(
                resource.transient.width, resource.transient.height, resource.transient.format,
                static_cast<VkSampleCountFlagBits>(resource.transient.samples),
                resource.transient.depth);
            if (handle == 0) {
                resource.failed = true;
                made_all = false;
                continue;
            }
            pool.push_back({
                .target = resource.transient,
                .handle = handle,
                .states = texture_states(),
            });
            found = pool.end() - 1;
            ++recorded.made;
        }
        found->busy = resource.last;
        found->asked = frames;
        resource.handle = found->handle;
        resource.texture = *fbe::texture::find(found->handle);
        resource.slot = fbe::texture::index_of(found->handle);
        if (found->images == none) {
            found->images = add_images(resource.texture, found->states);
        }
        point_at(resource, found->images);
    }
    return made_all;
}

/// The barrier that moves an image from how it was last used to `to`, if one's needed, and keeps
/// `to` as how it's used; `discard` says what's in it isn't kept. Reads in the layout an image's
/// already read in need none.
void transition(image_t& image, const state_t& to, bool discard,
                std::vector<VkImageMemoryBarrier2>& barriers) {
    const state_t& from = image.state;
    if (!discard && !from.writes && !to.writes && from.layout == to.layout) {
        return;
    }
    VkImageMemoryBarrier2 barrier = fbe::gpu::color_transition(
        image.image, discard ? VK_IMAGE_LAYOUT_UNDEFINED : from.layout, to.layout, from.stages,
        from.writes ? from.access : 0, to.stages, to.access);
    barrier.subresourceRange.aspectMask = image.aspect;
    barrier.subresourceRange.levelCount = image.levels;
    barriers.push_back(barrier);
    image.state = to;
}

/// Records `barriers`, if there are any, in one command, and empties them.
void place(VkCommandBuffer commands, std::vector<VkImageMemoryBarrier2>& barriers) {
    if (barriers.empty()) {
        return;
    }
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    };
    fbe::gpu::device()->pipeline_barrier(commands, &dependency);
    recorded.barriers += static_cast<std::uint32_t>(barriers.size());
    ++recorded.batches;
    barriers.clear();
}

/// Records a pass that draws: drawing begun into its targets, its commands, and drawing ended.
void draw(VkCommandBuffer commands, const fbe::gpu::pass_t& pass) {
    const fbe::gpu::device_t& gpu = *fbe::gpu::device();
    std::vector<VkRenderingAttachmentInfo> colors;
    colors.reserve(pass.colors.size());
    VkExtent2D extent{};
    for (const fbe::gpu::color_t& color : pass.colors) {
        const resource_record_t& target = resources[color.target];
        extent = extent_of(target);
        const bool multisampled = target.texture.msaa != VK_NULL_HANDLE;
        VkImageView view = multisampled ? target.texture.msaa_view : target.texture.view;
        if (target.kind == kind_t::EXTERNAL) {
            view = target.external.view;
        }
        colors.push_back({
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .resolveMode = multisampled ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE,
            .resolveImageView = multisampled ? target.texture.view : VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = color.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {
                .color = {
                    .float32 = { color.rgba[0], color.rgba[1], color.rgba[2], color.rgba[3] },
                },
            },
        });
    }
    VkRenderingAttachmentInfo depth{};
    if (pass.depth != fbe::gpu::no_resource) {
        const resource_record_t& target = resources[pass.depth];
        extent = extent_of(target);
        depth = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = target.texture.depth_view,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .loadOp = pass.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = { .depthStencil = { .depth = pass.depth_value } },
        };
    }
    const VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { .extent = extent },
        .layerCount = 1,
        .colorAttachmentCount = static_cast<std::uint32_t>(colors.size()),
        .pColorAttachments = colors.data(),
        .pDepthAttachment = pass.depth != fbe::gpu::no_resource ? &depth : nullptr,
    };
    gpu.begin_rendering(commands, &rendering);
    const VkViewport viewport{
        .width = static_cast<float>(extent.width),
        .height = static_cast<float>(extent.height),
        .maxDepth = 1.0F,
    };
    vkCmdSetViewport(commands, 0, 1, &viewport);
    const VkRect2D scissor{ .extent = extent };
    vkCmdSetScissor(commands, 0, 1, &scissor);
    if (pass.record) {
        pass.record(commands);
    }
    gpu.end_rendering(commands);
}

/// Records the kept passes a level at a time, each level's barriers first, then leaves textures at
/// rest, the frame's own images as they asked, and transient targets' images as they are, for
/// their next frame.
void record_levels(std::int32_t levels) {
    std::vector<const added_t*> order;
    for (const added_t& pass : added) {
        if (pass.kept) {
            order.push_back(&pass);
        }
    }
    VkCommandBuffer commands = fbe::gpu::frame_commands();
    std::vector<bool> begun(resources.size() * parts, false);
    std::vector<VkImageMemoryBarrier2> barriers;
    std::size_t at = 0;
    for (std::int32_t level = 0; level < levels; ++level) {
        std::size_t end = at;
        while (end < order.size() && order[end]->level == level) {
            ++end;
        }
        for (std::size_t which = at; which < end; ++which) {
            for (const access_t& use : uses_of(*order[which])) {
                const resource_record_t& resource = resources[use.resource];
                const std::uint32_t key = key_of(use.resource, use.part);
                const bool discard = !begun[key] && resource.kind != kind_t::TEXTURE;
                begun[key] = true;
                transition(images[resource.images[static_cast<std::size_t>(use.part)]], use.state,
                           discard, barriers);
            }
        }
        place(commands, barriers);
        for (std::size_t which = at; which < end; ++which) {
            const fbe::gpu::pass_t& pass = order[which]->pass;
            if (!pass.colors.empty() || pass.depth != fbe::gpu::no_resource) {
                draw(commands, pass);
            } else if (pass.record) {
                pass.record(commands);
            }
            for (const access_t& use : uses_of(*order[which])) {
                if (use.mipmaps) {
                    images[resources[use.resource].images[0]].state = resting;
                }
            }
            recorded.passes.push_back(pass.name);
            recorded.levels.push_back(static_cast<std::uint32_t>(level));
        }
        at = end;
    }
    // An image of the frame's own that no kept pass drew into is left as it was.
    for (resource_t which = 0; which < resources.size(); ++which) {
        const resource_record_t& resource = resources[which];
        if (resource.images[0] == none) {
            continue;
        }
        image_t& image = images[resource.images[0]];
        if (resource.kind == kind_t::TEXTURE) {
            transition(image, resting, false, barriers);
        } else if (resource.kind == kind_t::EXTERNAL && begun[key_of(which, part_t::IMAGE)]) {
            transition(image,
                       {
                           .layout = resource.external.layout,
                           .stages = resource.external.stages,
                           .access = resource.external.access,
                           .writes = true,
                       },
                       false, barriers);
        }
    }
    place(commands, barriers);
    for (pooled_t& pooled : pool) {
        if (pooled.images != none) {
            for (std::uint32_t part = 0; part < parts; ++part) {
                pooled.states[part] = images[pooled.images + part].state;
            }
        }
    }
}

/// Sends what a batch held back to the graph, before anything else enters it.
void flush_held_back() {
    if (held_back == nullptr || flushing || recording) {
        return;
    }
    flushing = true;
    held_back();
    flushing = false;
}

/// Empties the graph for the next passes, keeping its lists' room.
/// Tells the passes the last recording recorded what came of them.
void settle_awaiting(fbe::gpu::settled_t how) {
    const std::vector<std::function<void(fbe::gpu::settled_t)>> telling = std::move(awaiting);
    awaiting.clear();
    awaiting_missed = false;
    for (const std::function<void(fbe::gpu::settled_t)>& settled : telling) {
        settled(how);
    }
}

/// Tells the passes the last recording recorded, and those added since, that they'll never run.
void give_up_passes() {
    settle_awaiting(fbe::gpu::settled_t::GIVEN_UP);
    std::vector<std::function<void(fbe::gpu::settled_t)>> telling;
    for (added_t& pass : added) {
        if (pass.pass.settled) {
            telling.push_back(std::move(pass.pass.settled));
        }
    }
    for (const std::function<void(fbe::gpu::settled_t)>& settled : telling) {
        settled(fbe::gpu::settled_t::GIVEN_UP);
    }
}

void empty() {
    resources.clear();
    by_handle().clear();
    added.clear();
    accesses.clear();
    images.clear();
}

} // namespace

namespace fbe::gpu {

resource_t texture_resource(std::uint64_t handle) {
    if (recording) {
        fail("a texture isn't added as the frame's graph is recorded");
        return no_resource;
    }
    if (const auto found = by_handle().find(handle); found != by_handle().end()) {
        return found->second;
    }
    const fbe::texture::texture_t* texture = fbe::texture::find(handle);
    if (texture == nullptr) {
        return no_resource;
    }
    const auto resource = static_cast<resource_t>(resources.size());
    resource_record_t& made = resources.emplace_back(resource_record_t{
        .handle = handle,
        .texture = *texture,
        .slot = fbe::texture::index_of(handle),
    });
    point_at(made, add_images(*texture, texture_states()));
    by_handle().emplace(handle, resource);
    return resource;
}

resource_t transient_resource(const transient_t& target) {
    if (recording) {
        fail("a target isn't added as the frame's graph is recorded");
        return no_resource;
    }
    if (device() == nullptr) {
        fail("the GPU hasn't started");
        return no_resource;
    }
    if (fbe::texture::pixel_size(target.format) == 0) {
        fail(std::format("{} isn't a TextureFormat", target.format));
        return no_resource;
    }
    const std::uint32_t largest = device()->properties.limits.maxImageDimension2D;
    if (target.width < 1 || target.height < 1 || target.width > largest ||
        target.height > largest) {
        fail(std::format("a target is 1 to {} pixels on a side, not {}x{}", largest, target.width,
                         target.height));
        return no_resource;
    }
    if (target.samples != 1 && target.samples != 2 && target.samples != 4 && target.samples != 8) {
        fail(std::format("a target draws 1, 2, 4, or 8 samples a pixel, not {}", target.samples));
        return no_resource;
    }
    resources.push_back({ .kind = kind_t::TRANSIENT, .transient = target });
    return static_cast<resource_t>(resources.size() - 1);
}

resource_t external_resource(const external_t& image) {
    if (recording) {
        fail("an image isn't added as the frame's graph is recorded");
        return no_resource;
    }
    const auto first = static_cast<std::uint32_t>(images.size());
    images.push_back({
        .image = image.image,
        .aspect = flag<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
        .state = { .layout = VK_IMAGE_LAYOUT_UNDEFINED, .stages = image.waits },
    });
    resources.push_back({
        .kind = kind_t::EXTERNAL,
        .external = image,
        .images = { first, none, none },
    });
    return static_cast<resource_t>(resources.size() - 1);
}

void hold_back(void (*flush)(), void (*drop)()) {
    held_back = flush;
    drop_held = drop;
}

bool in_graph(std::uint64_t handle) {
    flush_held_back();
    return by_handle().contains(handle);
}

bool reserve_transient(const transient_t& target) {
    if (std::ranges::any_of(
            pool, [&target](const pooled_t& pooled) { return pooled.target == target; })) {
        return true;
    }
    const std::uint64_t handle = fbe::texture::create_engine_target(
        target.width, target.height, target.format,
        static_cast<VkSampleCountFlagBits>(target.samples), target.depth);
    if (handle == 0) {
        return false;
    }
    pool.push_back({
        .target = target,
        .handle = handle,
        .states = texture_states(),
        .asked = frames,
    });
    return true;
}

bool add_pass(pass_t pass) {
    if (recording) {
        return fail("a pass isn't added as the frame's graph is recorded");
    }
    flush_held_back();
    // What the CPU wrote into a texture this uses goes before it.
    const auto send = [](resource_t resource) {
        if (resource < resources.size() && resources[resource].kind == kind_t::TEXTURE) {
            fbe::texture::send_pixels(resources[resource].handle);
        }
    };
    for (const color_t& color : pass.colors) {
        send(color.target);
    }
    send(pass.depth);
    for (const auto& use : pass.uses) {
        send(use.first);
    }
    const std::size_t first = accesses.size();
    if (!expand(pass)) {
        accesses.resize(first);
        return false;
    }
    if (!once_each(first)) {
        accesses.resize(first);
        return fail("a pass uses an image once, never drawing into a texture it reads");
    }
    // A CPU copy of what this writes is older than it now.
    for (std::size_t at = first; at < accesses.size(); ++at) {
        const resource_record_t& resource = resources[accesses[at].resource];
        if (accesses[at].state.writes && resource.kind == kind_t::TEXTURE) {
            fbe::texture::changed_on_gpu(resource.handle);
        }
    }
    added.push_back({
        .pass = std::move(pass),
        .first = static_cast<std::uint32_t>(first),
        .count = static_cast<std::uint32_t>(accesses.size() - first),
    });
    return true;
}

std::uint32_t resource_index(resource_t resource) {
    return resource < resources.size() ? resources[resource].slot : fbe::texture::white;
}

VkFormat resource_format(resource_t resource) {
    return resource < resources.size() ? resources[resource].texture.vk_format
                                       : VK_FORMAT_UNDEFINED;
}

std::uint64_t resource_texture(resource_t resource) {
    return resource < resources.size() ? resources[resource].handle : 0;
}

VkImage resource_image(resource_t resource) {
    return resource < resources.size() ? resources[resource].texture.image : VK_NULL_HANDLE;
}

bool record_graph() {
    if (recording) {
        return true;
    }
    settle_awaiting(awaiting_missed ? settled_t::MISSED : settled_t::RAN);
    flush_held_back();
    if (added.empty()) {
        empty();
        return true;
    }
    recording = true;
    recorded = {};
    drop_unneeded();
    for (added_t& pass : added) {
        pass.needed = pass.kept;
    }
    const std::int32_t levels = level_passes();
    const bool made = give_images();
    // A pass that uses a transient target that has no image is dropped, and so is one that needs
    // what such a pass would have drawn.
    for (added_t& pass : added) {
        if (pass.kept && std::ranges::any_of(uses_of(pass), [](const access_t& use) {
                return resources[use.resource].failed;
            })) {
            pass.kept = false;
        }
    }
    if (!made) {
        drop_unwritten();
    }
    for (const added_t& pass : added) {
        recorded.dropped += pass.kept ? 0U : 1U;
    }
    if (recorded.dropped < added.size()) {
        record_levels(levels);
    }
    carriable.clear();
    for (added_t& pass : added) {
        if (!pass.kept || !pass.pass.carried) {
            continue;
        }
        carried_t carry{ .name = pass.pass.name, .record = std::move(pass.pass.record) };
        for (const auto& [resource, use] : pass.pass.uses) {
            carry.uses.emplace_back(resources[resource].handle, use);
        }
        carriable.push_back(std::move(carry));
    }
    std::vector<std::pair<std::function<void(settled_t)>, settled_t>> telling;
    for (added_t& pass : added) {
        if (!pass.pass.settled) {
            continue;
        }
        if (pass.kept) {
            awaiting.push_back(std::move(pass.pass.settled));
        } else {
            telling.emplace_back(std::move(pass.pass.settled),
                                 pass.needed ? settled_t::MISSED : settled_t::GIVEN_UP);
        }
    }
    count_passes(static_cast<std::uint32_t>(recorded.passes.size()), recorded.barriers);
    empty();
    recording = false;
    for (const auto& [settled, how] : telling) {
        settled(how);
    }
    return made;
}

void miss_passes() {
    awaiting_missed = true;
}

void carry_passes() {
    std::vector<carried_t> carrying = std::move(carriable);
    carriable.clear();
    for (carried_t& carry : carrying) {
        pass_t pass{ .name = carry.name, .record = std::move(carry.record), .carried = true };
        // A texture freed since is written no more; what was staged for it goes with the frame.
        const bool live = std::ranges::all_of(carry.uses, [&pass](const auto& use) {
            const resource_t resource = texture_resource(use.first);
            pass.uses.emplace_back(resource, use.second);
            return resource != no_resource;
        });
        if (live) {
            add_pass(std::move(pass));
        }
    }
}

void drop_passes() {
    carriable.clear();
    if (drop_held != nullptr) {
        drop_held();
    }
    if (!recording) {
        give_up_passes();
        empty();
    }
}

const recorded_t& last_recorded() {
    return recorded;
}

std::uint32_t transient_images() {
    return static_cast<std::uint32_t>(pool.size());
}

void age_transients() {
    ++frames;
    std::erase_if(pool, [](const pooled_t& pooled) {
        if (frames - pooled.asked <= kept_frames) {
            return false;
        }
        fbe_texture_release(pooled.handle);
        return true;
    });
}

void stop_graph() {
    carriable.clear();
    if (drop_held != nullptr) {
        drop_held();
    }
    give_up_passes();
    empty();
    // Stopping frees every texture, transient targets' too.
    pool.clear();
    marks.clear();
    recorded = {};
    recording = false;
    frames = 0;
}

} // namespace fbe::gpu
