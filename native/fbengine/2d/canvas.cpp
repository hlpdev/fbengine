// 2D's batch: the quads drawn into one target since they last went to the frame graph, in draws
// of one blend and one viewport, which become one pass, their quads in the frame's ring. They go
// whenever another target's drawn into, or anything else is about to enter the graph, so 2D keeps
// its turn with what the frame does. Shapes are spans of whole pixels, worked out on the CPU, and
// polygons triangles, their points to 1/16 of a pixel, which every GPU draws exactly; a triangle's
// a quad whose last two corners are one. A line's one quad over the pixels around it, of which the
// shader keeps the ones shapes.hpp's line has, so a long line costs no more than a short one; a
// line too long for the shader's arithmetic is spans. A texture's drawn as a quad of its own
// texels, through the bindless set, the pass saying it samples it, so what drew into it before is
// done first; tiles are one quad over the viewport, whose texels repeat.

#include "2d/canvas.hpp"

#include "2d/fbe_2d.h"
#include "2d/shapes.hpp"
#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/buffer.hpp"
#include "gpu/graph.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/vulkan.hpp"
#include "text/text.hpp"
#include "texture/texture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <engine_shaders.hpp>
#include <format>
#include <memory>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using fbe::gpu::flag;

/// What the canvas shader draws: a quad's corners, clockwise from its top left, in pixels; its
/// texture's uv at its top left and bottom right, or, for a line, its start along its longer axis
/// and across it, its length, and how far across it goes; the part of its texture a sprite's
/// texels are kept in, left, top, right, and bottom, in uv; its color, sRGB bytes, red first; its
/// texture's index in the bindless set and sampler's, the sampler's in the top 8 bits; and its
/// flags.
struct quad_t {
    std::array<float, 8> corners;
    std::array<float, 4> uv;
    std::array<float, 4> part;
    std::uint32_t color;
    std::uint32_t texture;
    std::uint32_t flags;
    std::uint32_t spare;
};
static_assert(sizeof(quad_t) == 80, "the canvas shader reads quads 80 bytes apart");

/// A quad's flags: it's a line; steeper than it's long, its longer axis down; its texels are kept
/// in its part, half a texel in from its edges, so filtering doesn't reach a neighbor's; it's
/// tiles, its uv where the tiles start and how big they are, from which the shader works out each
/// pixel's, wrapped into its part if it has one; its texture's coverage, its red the alpha of its
/// color; and a texel of no alpha leaves what's there, whatever the blend.
constexpr std::uint32_t line_flag = 1;
constexpr std::uint32_t steep_flag = 2;
constexpr std::uint32_t part_flag = 4;
constexpr std::uint32_t tile_flag = 8;
constexpr std::uint32_t coverage_flag = 16;
constexpr std::uint32_t key_flag = 32;
/// No blend but the pen's, for begin.
constexpr std::int32_t pens_blend = -1;
/// The longest line the shader's 32-bit arithmetic draws, and the furthest from 0 its ends can be.
constexpr std::int64_t longest_line = 32767;
constexpr std::int64_t furthest_line = 16777216;
/// How far past the clip a polygon's triangle reaches and is drawn as it is, its points on the
/// 1/16 pixel grid every GPU draws exactly; one reaching further is cut to the clip first, in
/// doubles, as a GPU's own clipping far out isn't exact.
constexpr double furthest_triangle = 16384.0;
/// The widest and tallest a tile's quad is, so how many tiles in a pixel is stays small enough for
/// floats to keep its texels.
constexpr std::int64_t widest_tiles = 2048;
/// How far from 0 an outline's points are whole pixels in doubles, 2^52; a side reaching further
/// is cut there first.
constexpr double furthest_point = 4503599627370496.0;

/// What the canvas shader's push constants are: the quads' address, what turns pixels into clip
/// space, and whether the target's pixels are data, which a pen's color is stored into as it's
/// given, not as sRGB.
struct push_t {
    VkDeviceAddress quads;
    std::array<float, 2> scale;
    std::uint32_t data;
};

/// A run of the batch's quads drawn with one blend and one scissor.
struct draw_t {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    std::int32_t blend = FBE_2D_SOLID;
    VkRect2D scissor{};
};

/// The batch: its target, its quads, and its draws.
std::uint64_t batch_target = 0;
std::vector<quad_t> quads;
/// The textures the batch's quads sample, and each slot's mark, the batch's number when its
/// texture was added, so each is added once.
std::vector<std::uint64_t> sampled;
std::vector<std::uint64_t> sampled_marks;
std::uint64_t batches = 1;
std::vector<draw_t> draws;
/// The pixels the draw being added to may cover, its scissor's.
fbe::canvas::clip_t clip;
std::uint32_t color = 0;
/// The target and pen the batch was last readied for, and the blend in place of the pen's, which a
/// draw with them again needn't check, until the batch goes to the graph.
bool ready = false;
std::uint64_t ready_target = 0;
fbe_2d_pen_t ready_pen{};
std::int32_t ready_blend = pens_blend;

fbe::gpu::blend_t blend_of(std::int32_t blend) {
    switch (blend) {
    case FBE_2D_ALPHA:
        return fbe::gpu::blend_t::ALPHA;
    case FBE_2D_ADD:
        return fbe::gpu::blend_t::ADD;
    case FBE_2D_MULTIPLY:
        return fbe::gpu::blend_t::MULTIPLY;
    default:
        return fbe::gpu::blend_t::NONE;
    }
}

/// Records a pass's draws, binding each blend's pipeline as it changes.
void record(VkCommandBuffer commands, const push_t& push, const std::vector<draw_t>& drawn,
            VkFormat format, VkSampleCountFlagBits samples) {
    VkDescriptorSet set = fbe::gpu::bindless_set();
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, fbe::gpu::pipeline_layout(),
                            0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commands, fbe::gpu::pipeline_layout(),
                       flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL), 0, sizeof(push), &push);
    std::int32_t bound = -1;
    for (const draw_t& draw : drawn) {
        if (draw.count == 0) {
            continue;
        }
        if (draw.blend != bound) {
            VkPipeline pipeline = fbe::gpu::pipeline({
                .shader = &fbe::shaders::canvas,
                .color = format,
                .samples = samples,
                .blend = blend_of(draw.blend),
            });
            if (pipeline == VK_NULL_HANDLE) {
                continue;
            }
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound = draw.blend;
        }
        vkCmdSetScissor(commands, 0, 1, &draw.scissor);
        vkCmdDraw(commands, draw.count * 6, 1, draw.first * 6, 0);
        fbe::gpu::count_draws(1, std::uint64_t{ draw.count } * 2);
    }
}

/// Sends the batch to the frame graph as a pass into its target, its quads copied into the
/// frame's ring; a target freed since is drawn into no more. With no room for them in the frame's
/// memory they stay held, for the next time there is, and nothing's said of it, as nothing's lost;
/// but in a frame's submit, which is the last there is for them, they're dropped, and the submit
/// fails saying so.
void flush() {
    ready = false;
    if (quads.empty()) {
        draws.clear();
        return;
    }
    // The frame's memory first, before anything's taken from the batch.
    fbe::gpu::slice_t slice{};
    if (fbe::texture::find(batch_target) != nullptr) {
        const bool said = fbe::gpu::said_no_room();
        slice = fbe::gpu::frame_memory(quads.size() * sizeof(quad_t), 16);
        if (slice.data == nullptr && !fbe::gpu::in_submit()) {
            // Passed over, so it's not what a call that goes on to fail is run again for.
            if (!said) {
                fbe::gpu::forget_no_room();
            }
            return;
        }
        if (slice.data == nullptr) {
            fbe::gpu::spoil_frame(std::format("what was drawn in 2D wasn't: {}", fbe_error()));
        }
    }
    // Taken from the batch first, so adding the pass, which sends what's held back, finds it
    // empty.
    const std::vector<quad_t> sent = std::move(quads);
    const auto drawn = std::make_shared<const std::vector<draw_t>>(std::move(draws));
    const std::vector<std::uint64_t> textures = std::move(sampled);
    quads.clear();
    draws.clear();
    sampled.clear();
    ++batches;
    const fbe::texture::texture_t* found = fbe::texture::find(batch_target);
    if (found == nullptr) {
        return;
    }
    const VkExtent2D extent{ .width = found->width, .height = found->height };
    const VkFormat format = found->vk_format;
    const VkSampleCountFlagBits samples = found->samples;
    const bool data = found->format == FBE_TEXTURE_RGBA8_LINEAR || found->format == FBE_TEXTURE_R8;
    const fbe::gpu::resource_t target = fbe::gpu::texture_resource(batch_target);
    const VkDeviceSize bytes = sent.size() * sizeof(quad_t);
    if (target != fbe::gpu::no_resource && slice.data != nullptr) {
        std::memcpy(slice.data, sent.data(), bytes);
        const push_t push{
            .quads = slice.address,
            .scale = {
                2.0F / static_cast<float>(extent.width),
                2.0F / static_cast<float>(extent.height),
            },
            .data = data ? 1U : 0U,
        };
        // A texture freed since it was drawn still has its image this frame, at rest.
        std::vector<std::pair<fbe::gpu::resource_t, fbe::gpu::use_t>> uses;
        for (const std::uint64_t texture : textures) {
            const fbe::gpu::resource_t resource = fbe::gpu::texture_resource(texture);
            if (resource != fbe::gpu::no_resource) {
                uses.emplace_back(resource, fbe::gpu::use_t::SAMPLED);
            }
        }
        fbe::gpu::add_pass({
            .name = "2d",
            .colors = { { .target = target } },
            .uses = std::move(uses),
            .record =
                [push, drawn, format, samples](VkCommandBuffer commands) {
                    record(commands, push, *drawn, format, samples);
                },
        });
    }
}

/// The frame the text atlas was last trimmed in, by its first canvas draw.
std::uint64_t trimmed = UINT64_MAX;

/// Forgets what the batch has, as a lost GPU or a stop does.
void drop() {
    ready = false;
    sampled.clear();
    sampled_marks.clear();
    ++batches;
    quads.clear();
    draws.clear();
    batch_target = 0;
}

/// Readies the batch for what `pen` draws into `target`, the screen for 0: sends what it has to
/// the graph first if that's another target's, and starts a draw if the blend or the viewport
/// isn't the last's; `blend`, unless it's pens_blend, is drawn with whatever the pen says. False,
/// with fbe_error's why, if the target or the pen isn't one.
bool begin(std::uint64_t target, const fbe_2d_pen_t* pen, std::int32_t blend = pens_blend) {
    if (pen == nullptr) {
        fbe::fail("no pen was given");
        return false;
    }
    // Drawing again with what the batch's ready for, as most draws do; the target's checked
    // still, as a texture freed since isn't drawn into, and a new render size makes the screen
    // another texture.
    if (ready && target == ready_target && blend == ready_blend &&
        std::memcmp(pen, &ready_pen, sizeof(fbe_2d_pen_t)) == 0) {
        if (target != 0) {
            return fbe::texture::find(target) != nullptr;
        }
        if (fbe::gpu::screen_handle() == batch_target) {
            return true;
        }
    }
    if (pen->blend < FBE_2D_SOLID || pen->blend > FBE_2D_MULTIPLY) {
        return fbe::fail(std::format("{} isn't a Blend", pen->blend));
    }
    fbe::canvas::trim_atlas();
    if (pen->viewport_width < 0 || pen->viewport_height < 0) {
        return fbe::fail(std::format("a viewport is 0 by 0 or bigger, not {}x{}",
                                     pen->viewport_width, pen->viewport_height));
    }
    std::uint64_t handle = target != 0 ? target : fbe_gpu_screen();
    if (handle == 0) {
        return fbe::fail("there's no screen, as there's no window");
    }
    const fbe::texture::texture_t* found = fbe::texture::find(handle);
    if (found == nullptr) {
        return false;
    }
    if (!found->target) {
        return fbe::fail("the texture isn't a target, which draws");
    }
    // What the CPU wrote into it goes before what's drawn into it now, and so does the batch so
    // far, if the write's a pass.
    fbe::texture::send_pixels(handle);
    found = fbe::texture::find(handle);
    const std::int64_t width = found->width;
    const std::int64_t height = found->height;
    if (handle != batch_target) {
        flush();
        // Still held, for want of the frame's memory: another target's can't join them.
        if (!quads.empty()) {
            return false;
        }
        batch_target = handle;
    }
    const bool whole = pen->viewport_width == 0 && pen->viewport_height == 0;
    clip = {
        .left = whole ? 0 : std::clamp<std::int64_t>(pen->viewport_x, 0, width),
        .top = whole ? 0 : std::clamp<std::int64_t>(pen->viewport_y, 0, height),
        .right = whole ? width
                       : std::clamp<std::int64_t>(
                             std::int64_t{ pen->viewport_x } + pen->viewport_width, 0, width),
        .bottom = whole ? height
                        : std::clamp<std::int64_t>(
                              std::int64_t{ pen->viewport_y } + pen->viewport_height, 0, height),
    };
    clip.right = std::max(clip.right, clip.left);
    clip.bottom = std::max(clip.bottom, clip.top);
    const VkRect2D scissor{
        .offset = {
            .x = static_cast<std::int32_t>(clip.left),
            .y = static_cast<std::int32_t>(clip.top),
        },
        .extent = {
            .width = static_cast<std::uint32_t>(clip.right - clip.left),
            .height = static_cast<std::uint32_t>(clip.bottom - clip.top),
        },
    };
    const std::int32_t drawn = blend != pens_blend ? blend : pen->blend;
    if (draws.empty() || draws.back().blend != drawn ||
        draws.back().scissor.offset.x != scissor.offset.x ||
        draws.back().scissor.offset.y != scissor.offset.y ||
        draws.back().scissor.extent.width != scissor.extent.width ||
        draws.back().scissor.extent.height != scissor.extent.height) {
        draws.push_back({
            .first = static_cast<std::uint32_t>(quads.size()),
            .blend = drawn,
            .scissor = scissor,
        });
    }
    color = std::uint32_t{ pen->red } | (std::uint32_t{ pen->green } << 8U) |
            (std::uint32_t{ pen->blue } << 16U) | (std::uint32_t{ pen->alpha } << 24U);
    fbe::gpu::hold_back(flush, drop);
    ready = true;
    ready_target = target;
    ready_pen = *pen;
    ready_blend = blend;
    return true;
}

/// Adds a quad of the pen's color to the draw.
void add(const std::array<float, 8>& corners) {
    quads.push_back({
        .corners = corners,
        .uv = { 0.0F, 0.0F, 1.0F, 1.0F },
        .color = color,
        .texture = fbe::texture::white,
    });
    ++draws.back().count;
}

/// Adds a span, already clipped, to the draw.
void add(const fbe::canvas::span_t& span) {
    const auto left = static_cast<float>(span.x);
    const auto top = static_cast<float>(span.y);
    const auto right = static_cast<float>(span.x + span.width);
    const auto bottom = static_cast<float>(span.y + span.height);
    add({ left, top, right, top, right, bottom, left, bottom });
}

void add(const std::vector<fbe::canvas::span_t>& spans) {
    for (const fbe::canvas::span_t& span : spans) {
        add(span);
    }
}

/// Adds a line from `x1`, `y1` to `x2`, `y2` as one quad the shader keeps shapes.hpp's pixels of;
/// false where it's too long for the shader, for spans to draw.
bool add_line(std::int64_t x1, std::int64_t y1, std::int64_t x2, std::int64_t y2) {
    const bool steep = std::abs(y2 - y1) > std::abs(x2 - x1);
    std::int64_t m1 = steep ? y1 : x1;
    std::int64_t n1 = steep ? x1 : y1;
    std::int64_t m2 = steep ? y2 : x2;
    std::int64_t n2 = steep ? x2 : y2;
    const std::uint32_t flags = line_flag | (steep ? steep_flag : 0U);
    if (m2 < m1) {
        std::swap(m1, m2);
        std::swap(n1, n2);
    }
    const std::int64_t length = m2 - m1;
    const std::int64_t across = n2 - n1;
    if (length > longest_line ||
        std::max({ std::abs(m1), std::abs(m2), std::abs(n1), std::abs(n2) }) > furthest_line) {
        return false;
    }
    // Where the line through the ends' middles is, across, at the start's edge and past the end's;
    // the quad's 1.5 pixels either side of it, which has every pixel the shader keeps.
    const double slope =
        length > 0 ? static_cast<double>(across) / static_cast<double>(length) : 0.0;
    const double start = static_cast<double>(n1) + 0.5 - (0.5 * slope);
    const double end = static_cast<double>(n2) + 0.5 + (0.5 * slope);
    const auto along = [](std::int64_t value) { return static_cast<float>(value); };
    const auto off = [](double value) { return static_cast<float>(value); };
    const std::array<float, 8> corners =
        steep ? std::array<float, 8>{
                    off(start - 1.5), along(m1), off(start + 1.5), along(m1),
                    off(end + 1.5), along(m2 + 1), off(end - 1.5), along(m2 + 1),
                }
              : std::array<float, 8>{
                    along(m1), off(start - 1.5), along(m2 + 1), off(end - 1.5),
                    along(m2 + 1), off(end + 1.5), along(m1), off(start + 1.5),
                };
    const auto bits = [](std::int64_t value) {
        return std::bit_cast<float>(static_cast<std::int32_t>(value));
    };
    quads.push_back({
        .corners = corners,
        .uv = { bits(m1), bits(n1), bits(length), bits(across) },
        .color = color,
        .texture = fbe::texture::white,
        .flags = flags,
    });
    ++draws.back().count;
    return true;
}

/// A texture a sprite draws: its handle, size, index in the bindless set, and filter, and the part
/// of it drawn, in its pixels. Sprites are sampled clamped at the texture's edges, but all of it
/// tiled, which repeats.
struct source_t {
    std::uint64_t handle = 0;
    float width = 0.0F;
    float height = 0.0F;
    std::uint32_t slot = 0;
    std::int32_t filter = 0;
    std::array<float, 4> region{};
};

/// The texture `texture` and the part of it `sprite` draws at `x`, `y`, into the batch's target;
/// false, with fbe_error's why, if it isn't live, is the target, or the part isn't in it, or the
/// position or the sprite's handle, rotation, or scale isn't finite.
bool source_of(std::uint64_t texture, const fbe_2d_sprite_t* sprite, float x, float y,
               source_t& source) {
    if (sprite == nullptr) {
        return fbe::fail("no sprite was given");
    }
    const fbe::texture::texture_t* found = fbe::texture::find(texture);
    if (found == nullptr) {
        return false;
    }
    if (texture == batch_target) {
        return fbe::fail("a texture isn't drawn into itself");
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(sprite->rotation) ||
        !std::isfinite(sprite->scale_x) || !std::isfinite(sprite->scale_y) ||
        !std::isfinite(sprite->handle_x) || !std::isfinite(sprite->handle_y)) {
        return fbe::fail("a sprite's position, handle, rotation, and scale are finite");
    }
    const auto width = static_cast<float>(found->width);
    const auto height = static_cast<float>(found->height);
    const bool whole = sprite->region_width == 0.0F && sprite->region_height == 0.0F;
    const std::array<float, 4> region = whole ? std::array<float, 4>{ 0.0F, 0.0F, width, height }
                                              : std::array<float, 4>{
                                                    sprite->region_x,
                                                    sprite->region_y,
                                                    sprite->region_x + sprite->region_width,
                                                    sprite->region_y + sprite->region_height,
                                                };
    if (!std::ranges::all_of(region, [](float value) { return std::isfinite(value); }) ||
        region[0] < 0.0F || region[1] < 0.0F || region[2] > width || region[3] > height ||
        region[2] <= region[0] || region[3] <= region[1]) {
        return fbe::fail(std::format("a sprite's part, {}x{} at {}, {}, isn't in a {}x{} texture",
                                     sprite->region_width, sprite->region_height, sprite->region_x,
                                     sprite->region_y, found->width, found->height));
    }
    source = {
        .handle = texture,
        .width = width,
        .height = height,
        .slot = fbe::texture::index_of(texture),
        .filter = found->filter,
        .region = region,
    };
    return true;
}

/// Adds a quad of `source`'s texels, `uv` at its top left and bottom right, sampled with 2D's
/// sampler for its filter, repeating or not, tinted the pen's color, with `flags`, its part
/// `source`'s; and says the batch samples the texture.
void add_texels(const std::array<float, 8>& corners, const std::array<float, 4>& uv,
                const source_t& source, bool repeat, std::uint32_t flags) {
    if (sampled_marks.size() <= source.slot) {
        sampled_marks.resize(std::size_t{ source.slot } + 1, 0);
    }
    if (sampled_marks[source.slot] != batches) {
        sampled_marks[source.slot] = batches;
        sampled.push_back(source.handle);
    }
    const std::uint32_t sampler = fbe::gpu::sampler_2d_index(source.filter, repeat);
    quads.push_back({
        .corners = corners,
        .uv = uv,
        .part = {
            source.region[0] / source.width,
            source.region[1] / source.height,
            source.region[2] / source.width,
            source.region[3] / source.height,
        },
        .color = color,
        .texture = source.slot | (sampler << 24U),
        .flags = flags,
    });
    ++draws.back().count;
}

/// Whether a polygon's triangle, x and y in turn, reaches too far past the clip for every GPU to
/// draw it exactly.
bool reaches_far(const std::array<double, 6>& triangle) {
    const auto near = [](double value, std::int64_t low, std::int64_t high) {
        return value >= static_cast<double>(low) - furthest_triangle &&
               value <= static_cast<double>(high) + furthest_triangle;
    };
    for (std::size_t at = 0; at < triangle.size(); at += 2) {
        if (!near(triangle[at], clip.left, clip.right) ||
            !near(triangle[at + 1], clip.top, clip.bottom)) {
            return true;
        }
    }
    return false;
}

/// Adds a polygon's triangle, x and y in turn: as it is, or, `cut`, the part of it in the clip,
/// cut in doubles, as all of a polygon's are once one reaches far, so a side a whole triangle and
/// a cut one share has the same ends in both. Where a side two triangles share is cut, it's cut at
/// one point, worked out from whichever end's first.
void add_triangle(const std::array<double, 6>& triangle, bool cut) {
    const auto narrow = [](double value) { return static_cast<float>(value); };
    if (!cut) {
        add({
            narrow(triangle[0]),
            narrow(triangle[1]),
            narrow(triangle[2]),
            narrow(triangle[3]),
            narrow(triangle[4]),
            narrow(triangle[5]),
            narrow(triangle[4]),
            narrow(triangle[5]),
        });
        return;
    }
    using point_t = std::array<double, 2>;
    std::vector<point_t> shape{
        { triangle[0], triangle[1] },
        { triangle[2], triangle[3] },
        { triangle[4], triangle[5] },
    };
    // Each side of the clip in turn: an axis, where it is, and whether what's kept is past it.
    const std::array<std::tuple<std::size_t, double, bool>, 4> sides{
        {
            { 0, static_cast<double>(clip.left), true },
            { 0, static_cast<double>(clip.right), false },
            { 1, static_cast<double>(clip.top), true },
            { 1, static_cast<double>(clip.bottom), false },
        },
    };
    for (const auto& [axis, edge, past] : sides) {
        const auto kept = [&](const point_t& point) {
            return past ? point[axis] >= edge : point[axis] <= edge;
        };
        std::vector<point_t> inside;
        for (std::size_t at = 0; at < shape.size(); ++at) {
            point_t from = shape[at];
            point_t to = shape[(at + 1) % shape.size()];
            if (kept(from)) {
                inside.push_back(from);
            }
            if (kept(from) != kept(to)) {
                if (to < from) {
                    std::swap(from, to);
                }
                const double share = (edge - from[axis]) / (to[axis] - from[axis]);
                point_t crossing{};
                crossing[axis] = edge;
                crossing[1 - axis] = from[1 - axis] + ((to[1 - axis] - from[1 - axis]) * share);
                inside.push_back(crossing);
            }
        }
        shape = std::move(inside);
        if (shape.size() < 3) {
            return;
        }
    }
    for (std::size_t at = 1; at + 1 < shape.size(); ++at) {
        add({
            narrow(shape[0][0]),
            narrow(shape[0][1]),
            narrow(shape[at][0]),
            narrow(shape[at][1]),
            narrow(shape[at + 1][0]),
            narrow(shape[at + 1][1]),
            narrow(shape[at + 1][0]),
            narrow(shape[at + 1][1]),
        });
    }
}

/// Cuts the side from `from` to `to` to where doubles hold whole pixels, `furthest_point` either
/// way of 0; false if none of it's there.
bool cut_side(std::array<double, 2>& from, std::array<double, 2>& to) {
    double first = 0.0;
    double last = 1.0;
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const double step = to[axis] - from[axis];
        if (step == 0.0) {
            if (std::abs(from[axis]) > furthest_point) {
                return false;
            }
            continue;
        }
        const double low = (-furthest_point - from[axis]) / step;
        const double high = (furthest_point - from[axis]) / step;
        first = std::max(first, std::min(low, high));
        last = std::min(last, std::max(low, high));
    }
    if (first > last) {
        return false;
    }
    const std::array<double, 2> start = from;
    const std::array<double, 2> end = to;
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const double step = end[axis] - start[axis];
        if (first > 0.0) {
            from[axis] = start[axis] + (step * first);
        }
        if (last < 1.0) {
            to[axis] = start[axis] + (step * last);
        }
        from[axis] = std::clamp(from[axis], -furthest_point, furthest_point);
        to[axis] = std::clamp(to[axis], -furthest_point, furthest_point);
    }
    return true;
}

/// Adds the part of a rectangle in the clip, if any is.
void add_rect(std::int64_t x, std::int64_t y, std::int64_t width, std::int64_t height) {
    const std::int64_t left = std::max(x, clip.left);
    const std::int64_t top = std::max(y, clip.top);
    const std::int64_t right = std::min(x + width, clip.right);
    const std::int64_t bottom = std::min(y + height, clip.bottom);
    if (left < right && top < bottom) {
        add({ .x = left, .y = top, .width = right - left, .height = bottom - top });
    }
}

} // namespace

namespace fbe::canvas {

bool draw_over(std::uint64_t target, const fbe_2d_pen_t* pen, std::int64_t x, std::int64_t y,
               std::int64_t width, std::int64_t height) {
    const bool adds = pen != nullptr && pen->blend == FBE_2D_ADD;
    if (!begin(target, pen, adds ? pens_blend : FBE_2D_ALPHA)) {
        return false;
    }
    add_rect(x + pen->origin_x, y + pen->origin_y, width, height);
    return true;
}

void trim_atlas() {
    const std::uint64_t frame = fbe::gpu::submitted_frames();
    if (frame != trimmed) {
        trimmed = frame;
        fbe::text::atlas().trim();
    }
}

bool draw_glyphs(std::uint64_t target, const fbe_2d_pen_t* pen, std::uint64_t page, bool coverage,
                 std::span<const glyph_t> glyphs) {
    fbe::texture::send_pixels(page);
    const bool adds = pen != nullptr && pen->blend == FBE_2D_ADD;
    if (!begin(target, pen, adds ? pens_blend : FBE_2D_ALPHA)) {
        return false;
    }
    const fbe::texture::texture_t* found = fbe::texture::find(page);
    if (found == nullptr) {
        return false;
    }
    if (page == batch_target) {
        return fbe::fail("a texture isn't drawn into itself");
    }
    const auto width = static_cast<float>(found->width);
    const auto height = static_cast<float>(found->height);
    source_t source{
        .handle = page,
        .width = width,
        .height = height,
        .slot = fbe::texture::index_of(page),
        .filter = FBE_TEXTURE_NEAREST,
        .region = { 0.0F, 0.0F, width, height },
    };
    // A color glyph's texels are its own, only faded by the pen.
    const std::uint32_t pens = color;
    if (!coverage) {
        color = 0x00FFFFFFU | (std::uint32_t{ pen->alpha } << 24U);
    }
    bool placed = true;
    for (const glyph_t& glyph : glyphs) {
        if (glyph.source_x < 0 || glyph.source_y < 0 || glyph.source_width < 0 ||
            glyph.source_height < 0 || glyph.width < 0 || glyph.height < 0 ||
            std::int64_t{ glyph.source_x } + glyph.source_width > found->width ||
            std::int64_t{ glyph.source_y } + glyph.source_height > found->height) {
            placed = false;
            continue;
        }
        const std::int64_t left = glyph.x + pen->origin_x;
        const std::int64_t top = glyph.y + pen->origin_y;
        if (glyph.width == 0 || glyph.height == 0 || left >= clip.right || top >= clip.bottom ||
            left + glyph.width <= clip.left || top + glyph.height <= clip.top) {
            continue;
        }
        const auto l = static_cast<float>(left);
        const auto t = static_cast<float>(top);
        const auto r = static_cast<float>(left + glyph.width);
        const auto b = static_cast<float>(top + glyph.height);
        const bool scaled =
            glyph.width != glyph.source_width || glyph.height != glyph.source_height;
        source.filter = scaled ? FBE_TEXTURE_LINEAR : FBE_TEXTURE_NEAREST;
        add_texels({ l, t, r, t, r, b, l, b },
                   {
                       static_cast<float>(glyph.source_x) / width,
                       static_cast<float>(glyph.source_y) / height,
                       static_cast<float>(glyph.source_x + glyph.source_width) / width,
                       static_cast<float>(glyph.source_y + glyph.source_height) / height,
                   },
                   source, false, coverage ? coverage_flag : 0U);
    }
    color = pens;
    return placed || fbe::fail("a glyph's pixels aren't on its page");
}

} // namespace fbe::canvas

extern "C" {

FBE_API bool fbe_2d_clear(uint64_t target, const fbe_2d_pen_t* pen) {
    if (!begin(target, pen, FBE_2D_SOLID)) {
        return false;
    }
    add_rect(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top);
    return true;
}

FBE_API bool fbe_2d_plot(uint64_t target, const fbe_2d_pen_t* pen, int32_t x, int32_t y) {
    if (!begin(target, pen)) {
        return false;
    }
    add_rect(std::int64_t{ x } + pen->origin_x, std::int64_t{ y } + pen->origin_y, 1, 1);
    return true;
}

FBE_API bool fbe_2d_line(uint64_t target, const fbe_2d_pen_t* pen, int32_t x1, int32_t y1,
                         int32_t x2, int32_t y2) {
    if (!begin(target, pen)) {
        return false;
    }
    const std::int64_t left = std::int64_t{ x1 } + pen->origin_x;
    const std::int64_t top = std::int64_t{ y1 } + pen->origin_y;
    const std::int64_t right = std::int64_t{ x2 } + pen->origin_x;
    const std::int64_t bottom = std::int64_t{ y2 } + pen->origin_y;
    if (!add_line(left, top, right, bottom)) {
        std::vector<fbe::canvas::span_t> spans;
        fbe::canvas::line(left, top, right, bottom, true, clip, spans);
        add(spans);
    }
    return true;
}

FBE_API bool fbe_2d_rect(uint64_t target, const fbe_2d_pen_t* pen, int32_t x, int32_t y,
                         int32_t width, int32_t height, bool filled) {
    if (!begin(target, pen)) {
        return false;
    }
    const std::int64_t left = std::int64_t{ x } + pen->origin_x;
    const std::int64_t top = std::int64_t{ y } + pen->origin_y;
    if (filled) {
        if (width > 0 && height > 0) {
            add_rect(left, top, width, height);
        }
        return true;
    }
    std::vector<fbe::canvas::span_t> spans;
    fbe::canvas::rect_outline(left, top, width, height, spans);
    for (const fbe::canvas::span_t& span : spans) {
        add_rect(span.x, span.y, span.width, span.height);
    }
    return true;
}

FBE_API bool fbe_2d_oval(uint64_t target, const fbe_2d_pen_t* pen, int32_t x, int32_t y,
                         int32_t width, int32_t height, bool filled) {
    if (!begin(target, pen)) {
        return false;
    }
    std::vector<fbe::canvas::span_t> spans;
    fbe::canvas::oval(std::int64_t{ x } + pen->origin_x, std::int64_t{ y } + pen->origin_y, width,
                      height, filled, clip, spans);
    add(spans);
    return true;
}

FBE_API bool fbe_2d_texture(uint64_t target, const fbe_2d_pen_t* pen, uint64_t texture,
                            const fbe_2d_sprite_t* sprite, float x, float y) {
    // What the CPU wrote into the texture goes before it's drawn, and before the draw's begun, as
    // sending it can send the batch.
    fbe::texture::send_pixels(texture);
    source_t source;
    if (!begin(target, pen) || !source_of(texture, sprite, x, y, source)) {
        return false;
    }
    // Each corner of the part, from its handle, scaled, turned clockwise on the screen, and moved
    // to the position.
    const auto turn = static_cast<double>(sprite->rotation);
    const double cosine = std::cos(turn);
    const double sine = std::sin(turn);
    const double left = static_cast<double>(x) + pen->origin_x;
    const double top = static_cast<double>(y) + pen->origin_y;
    const float across = source.region[2] - source.region[0];
    const float down = source.region[3] - source.region[1];
    std::array<float, 8> corners{};
    const std::array<std::array<float, 2>, 4> part{
        {
            { 0.0F, 0.0F },
            { across, 0.0F },
            { across, down },
            { 0.0F, down },
        },
    };
    for (std::size_t at = 0; at < part.size(); ++at) {
        const double from_x =
            (part[at][0] - sprite->handle_x) * static_cast<double>(sprite->scale_x);
        const double from_y =
            (part[at][1] - sprite->handle_y) * static_cast<double>(sprite->scale_y);
        corners[at * 2] = static_cast<float>(left + (from_x * cosine) - (from_y * sine));
        corners[(at * 2) + 1] = static_cast<float>(top + (from_x * sine) + (from_y * cosine));
    }
    add_texels(corners,
               {
                   source.region[0] / source.width,
                   source.region[1] / source.height,
                   source.region[2] / source.width,
                   source.region[3] / source.height,
               },
               source, false, part_flag | (sprite->keyed != 0 ? key_flag : 0U));
    return true;
}

FBE_API bool fbe_2d_tile(uint64_t target, const fbe_2d_pen_t* pen, uint64_t texture,
                         const fbe_2d_sprite_t* sprite, float x, float y) {
    fbe::texture::send_pixels(texture);
    source_t source;
    if (!begin(target, pen) || !source_of(texture, sprite, x, y, source)) {
        return false;
    }
    if (sprite->scale_x == 0.0F || sprite->scale_y == 0.0F) {
        return fbe::fail("a sprite's tiled at a scale that isn't 0");
    }
    if (clip.right <= clip.left || clip.bottom <= clip.top) {
        return true;
    }
    // A tile's size on the target, mirrored where its scale is negative, and where the one whose
    // handle is at the position starts.
    const double across =
        static_cast<double>(source.region[2] - source.region[0]) * sprite->scale_x;
    const double down = static_cast<double>(source.region[3] - source.region[1]) * sprite->scale_y;
    const double left = static_cast<double>(x) + pen->origin_x -
                        (static_cast<double>(sprite->handle_x) * sprite->scale_x);
    const double top = static_cast<double>(y) + pen->origin_y -
                       (static_cast<double>(sprite->handle_y) * sprite->scale_y);
    // Quads over the viewport, 2048 pixels at most each way, the shader working out each pixel's
    // uv in tiles from where it is, as a GPU's interpolating isn't exact far along a target: all
    // of a texture repeats as the sampler wraps, and a part as the shader wraps it into the part.
    // Each one's tiles start where one does by its top left, moved by whole tiles from the
    // position, so floats keep its texels however far away the position is.
    const bool whole = source.region[0] == 0.0F && source.region[1] == 0.0F &&
                       source.region[2] == source.width && source.region[3] == source.height;
    const std::uint32_t flags =
        (whole ? tile_flag : part_flag | tile_flag) | (sprite->keyed != 0 ? key_flag : 0U);
    for (std::int64_t strip_top = clip.top; strip_top < clip.bottom; strip_top += widest_tiles) {
        const std::int64_t strip_bottom = std::min(strip_top + widest_tiles, clip.bottom);
        const auto t = static_cast<double>(strip_top);
        const double first_top = top - (std::floor((top - t) / down) * down);
        for (std::int64_t strip_left = clip.left; strip_left < clip.right;
             strip_left += widest_tiles) {
            const std::int64_t strip_right = std::min(strip_left + widest_tiles, clip.right);
            const auto l = static_cast<double>(strip_left);
            const double first_left = left - (std::floor((left - l) / across) * across);
            const auto fl = static_cast<float>(strip_left);
            const auto ft = static_cast<float>(strip_top);
            const auto fr = static_cast<float>(strip_right);
            const auto fb = static_cast<float>(strip_bottom);
            add_texels({ fl, ft, fr, ft, fr, fb, fl, fb },
                       {
                           static_cast<float>(first_left),
                           static_cast<float>(first_top),
                           static_cast<float>(across),
                           static_cast<float>(down),
                       },
                       source, whole, flags);
        }
    }
    return true;
}

FBE_API bool fbe_2d_polygon(uint64_t target, const fbe_2d_pen_t* pen, const float* points,
                            int32_t count, bool filled) {
    if (points == nullptr || count < 3) {
        return fbe::fail(std::format("a polygon has 3 points or more, not {}", count));
    }
    const std::span<const float> given(points, static_cast<std::size_t>(count) * 2);
    if (!std::ranges::all_of(given, [](float value) { return std::isfinite(value); })) {
        return fbe::fail("a polygon's points are finite");
    }
    if (!begin(target, pen)) {
        return false;
    }
    // Its points moved by the origin, to the nearest 1/16 of a pixel.
    std::vector<double> moved(given.size());
    for (std::size_t at = 0; at < moved.size(); ++at) {
        const std::int32_t origin = at % 2 == 0 ? pen->origin_x : pen->origin_y;
        moved[at] = std::round((static_cast<double>(given[at]) + origin) * 16.0) / 16.0;
    }
    if (filled) {
        const std::vector<std::array<double, 6>> triangles = fbe::canvas::triangulate(moved);
        const bool cut = std::ranges::any_of(triangles, reaches_far);
        for (const std::array<double, 6>& triangle : triangles) {
            add_triangle(triangle, cut);
        }
        return true;
    }
    // Each side, from the pixel its first point's in to the next's, both drawn, every pixel once.
    std::vector<fbe::canvas::span_t> spans;
    const auto pixel = [](double value) { return static_cast<std::int64_t>(std::floor(value)); };
    for (std::size_t at = 0; at < moved.size(); at += 2) {
        const std::size_t next = (at + 2) % moved.size();
        std::array<double, 2> from{ moved[at], moved[at + 1] };
        std::array<double, 2> to{ moved[next], moved[next + 1] };
        if (cut_side(from, to)) {
            fbe::canvas::line(pixel(from[0]), pixel(from[1]), pixel(to[0]), pixel(to[1]), true,
                              clip, spans);
        }
    }
    add(fbe::canvas::merged(spans));
    return true;
}

} // extern "C"
