// 2D's batch: the quads drawn into one target since they last went to the frame graph, in draws
// of one blend and one viewport, which become one pass, their quads in the frame's ring. They go
// whenever another target's drawn into, or anything else is about to enter the graph, so 2D keeps
// its turn with what the frame does. Shapes are spans of whole pixels, worked out on the CPU, and
// polygons triangles; a triangle's a quad whose last two corners are one. A line's one quad over
// the pixels around it, of which the shader keeps the ones shapes.hpp's line has, so a long line
// costs no more than a short one; a line too long for the shader's arithmetic is spans.

#include "2d/fbe_2d.h"
#include "2d/shapes.hpp"
#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/buffer.hpp"
#include "gpu/graph.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/vulkan.hpp"
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
#include <vector>

namespace {

using fbe::gpu::flag;

/// What the canvas shader draws: a quad's corners, clockwise from its top left, in pixels; the
/// part of its texture it shows, or, for a line, its start along its longer axis and across it,
/// its length, and how far across it goes; its color, sRGB bytes, red first; its texture's index in
/// the bindless set and sampler's, the sampler's in the top 8 bits; and its flags.
struct quad_t {
    std::array<float, 8> corners;
    std::array<float, 4> uv;
    std::uint32_t color;
    std::uint32_t texture;
    std::uint32_t mask;
    std::uint32_t flags;
};
static_assert(sizeof(quad_t) == 64, "the canvas shader reads quads 64 bytes apart");

/// A quad's flags: it's a line; steeper than it's long, its longer axis down; and it leaves out
/// its pixel at the end of that axis, or at the start.
constexpr std::uint32_t line_flag = 1;
constexpr std::uint32_t steep_flag = 2;
constexpr std::uint32_t no_end_flag = 4;
constexpr std::uint32_t no_start_flag = 8;
/// The longest line the shader's 32-bit arithmetic draws, and the furthest from 0 its ends can be.
constexpr std::int64_t longest_line = 32767;
constexpr std::int64_t furthest_line = 16777216;

/// What the canvas shader's push constants are: the quads' address, and what turns pixels into
/// clip space.
struct push_t {
    VkDeviceAddress quads;
    std::array<float, 2> scale;
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
std::vector<draw_t> draws;
/// The pixels the draw being added to may cover, its scissor's.
fbe::canvas::clip_t clip;
std::uint32_t color = 0;
/// The target and pen the batch was last readied for, and whether unblended, which a draw with
/// them again needn't check, until the batch goes to the graph.
bool ready = false;
std::uint64_t ready_target = 0;
fbe_2d_pen_t ready_pen{};
bool ready_solid = false;

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
/// frame's ring; a target freed since is drawn into no more.
void flush() {
    ready = false;
    if (quads.empty()) {
        draws.clear();
        return;
    }
    // Taken from the batch first, so adding the pass, which sends what's held back, finds it
    // empty.
    const std::vector<quad_t> sent = std::move(quads);
    const auto drawn = std::make_shared<const std::vector<draw_t>>(std::move(draws));
    quads.clear();
    draws.clear();
    const fbe::texture::texture_t* found = fbe::texture::find(batch_target);
    const fbe::gpu::resource_t target =
        found != nullptr ? fbe::gpu::texture_resource(batch_target) : fbe::gpu::no_resource;
    const VkDeviceSize bytes = sent.size() * sizeof(quad_t);
    const fbe::gpu::slice_t slice =
        target != fbe::gpu::no_resource ? fbe::gpu::frame_memory(bytes, 16) : fbe::gpu::slice_t{};
    if (slice.data != nullptr) {
        std::memcpy(slice.data, sent.data(), bytes);
        const push_t push{
            .quads = slice.address,
            .scale = {
                2.0F / static_cast<float>(found->width),
                2.0F / static_cast<float>(found->height),
            },
        };
        fbe::gpu::add_pass({
            .name = "2d",
            .colors = { { .target = target } },
            .record =
                [push, drawn, format = found->vk_format, samples = found->samples](
                    VkCommandBuffer commands) { record(commands, push, *drawn, format, samples); },
        });
    }
}

/// Forgets what the batch has, as a lost GPU or a stop does.
void drop() {
    ready = false;
    quads.clear();
    draws.clear();
    batch_target = 0;
}

/// Readies the batch for what `pen` draws into `target`, the screen for 0: sends what it has to
/// the graph first if that's another target's, and starts a draw if the blend or the viewport
/// isn't the last's; `solid` draws unblended whatever the pen says. False, with fbe_error's why,
/// if the target or the pen isn't one.
bool begin(std::uint64_t target, const fbe_2d_pen_t* pen, bool solid = false) {
    if (pen == nullptr) {
        return fbe::fail("no pen was given");
    }
    // Drawing again with what the batch's ready for, as most draws do; the screen's checked
    // still, as a new render size makes it another texture.
    if (ready && target == ready_target && solid == ready_solid &&
        std::memcmp(pen, &ready_pen, sizeof(fbe_2d_pen_t)) == 0 &&
        (target != 0 || fbe::gpu::screen_handle() == batch_target)) {
        return true;
    }
    if (pen->blend < FBE_2D_SOLID || pen->blend > FBE_2D_MULTIPLY) {
        return fbe::fail(std::format("{} isn't a Blend", pen->blend));
    }
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
    if (handle != batch_target) {
        flush();
        batch_target = handle;
    }
    const bool whole = pen->viewport_width == 0 && pen->viewport_height == 0;
    const std::int64_t width = found->width;
    const std::int64_t height = found->height;
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
    const std::int32_t blend = solid ? FBE_2D_SOLID : pen->blend;
    if (draws.empty() || draws.back().blend != blend ||
        draws.back().scissor.offset.x != scissor.offset.x ||
        draws.back().scissor.offset.y != scissor.offset.y ||
        draws.back().scissor.extent.width != scissor.extent.width ||
        draws.back().scissor.extent.height != scissor.extent.height) {
        draws.push_back({
            .first = static_cast<std::uint32_t>(quads.size()),
            .blend = blend,
            .scissor = scissor,
        });
    }
    color = std::uint32_t{ pen->red } | (std::uint32_t{ pen->green } << 8U) |
            (std::uint32_t{ pen->blue } << 16U) | (std::uint32_t{ pen->alpha } << 24U);
    fbe::gpu::hold_back(flush, drop);
    ready = true;
    ready_target = target;
    ready_pen = *pen;
    ready_solid = solid;
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

/// Adds a line from `x1`, `y1` to `x2`, `y2`, all of it, or with `x2`, `y2` left out unless
/// `last`, as one quad the shader keeps shapes.hpp's pixels of; false where it's too long for the
/// shader, for spans to draw.
bool add_line(std::int64_t x1, std::int64_t y1, std::int64_t x2, std::int64_t y2, bool last) {
    const bool steep = std::abs(y2 - y1) > std::abs(x2 - x1);
    std::int64_t m1 = steep ? y1 : x1;
    std::int64_t n1 = steep ? x1 : y1;
    std::int64_t m2 = steep ? y2 : x2;
    std::int64_t n2 = steep ? x2 : y2;
    std::uint32_t flags = line_flag | (steep ? steep_flag : 0U) | (last ? 0U : no_end_flag);
    if (m2 < m1) {
        std::swap(m1, m2);
        std::swap(n1, n2);
        flags = last ? flags : ((flags & ~no_end_flag) | no_start_flag);
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

extern "C" {

FBE_API bool fbe_2d_clear(uint64_t target, const fbe_2d_pen_t* pen) {
    if (!begin(target, pen, true)) {
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
    if (!add_line(left, top, right, bottom, true)) {
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
    std::vector<float> moved(given.begin(), given.end());
    for (std::size_t at = 0; at < moved.size(); at += 2) {
        moved[at] += static_cast<float>(pen->origin_x);
        moved[at + 1] += static_cast<float>(pen->origin_y);
    }
    if (filled) {
        for (const std::array<float, 6>& triangle : fbe::canvas::triangulate(moved)) {
            add({
                triangle[0],
                triangle[1],
                triangle[2],
                triangle[3],
                triangle[4],
                triangle[5],
                triangle[4],
                triangle[5],
            });
        }
        return true;
    }
    // Each side from the pixel its first point's in to the next's, that one left for the next side.
    std::vector<fbe::canvas::span_t> spans;
    const auto pixel = [](float value) { return static_cast<std::int64_t>(std::floor(value)); };
    for (std::size_t at = 0; at < moved.size(); at += 2) {
        const std::size_t next = (at + 2) % moved.size();
        const std::int64_t x1 = pixel(moved[at]);
        const std::int64_t y1 = pixel(moved[at + 1]);
        const std::int64_t x2 = pixel(moved[next]);
        const std::int64_t y2 = pixel(moved[next + 1]);
        if (!add_line(x1, y1, x2, y2, false)) {
            fbe::canvas::line(x1, y1, x2, y2, false, clip, spans);
        }
    }
    add(spans);
    return true;
}

} // extern "C"
