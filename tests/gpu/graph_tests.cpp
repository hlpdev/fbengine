// The frame graph: passes put in levels by what they depend on, each level's barriers placed in
// one command; passes nothing needs dropped; transient targets sharing images when their passes
// don't overlap, and kept from frame to frame while they're asked for; and what it refuses. The
// validation layers check every barrier it places, synchronization too. Each test ends by
// stopping the GPU, which has to leave nothing behind.

#include "golden/golden.hpp"
#include "gpu_testing.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <fbengine.h>
#include <gpu/graph.hpp>
#include <gpu/pipeline.hpp>
#include <gpu/vulkan.hpp>
#include <string>
#include <string_view>
#include <test_shaders.hpp>
#include <texture/texture.hpp>
#include <utility>
#include <vector>

using fbe::gpu::resource_t;
using fbe::gpu::use_t;
using fbe::tests::require;
using fbe::tests::stop_gpu;

namespace {

using fbe::gpu::flag;

void start() {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_gpu_start());
}

std::uint64_t target(std::int32_t width, std::int32_t height) {
    const std::uint64_t made =
        fbe_texture_create_target(width, height, FBE_TEXTURE_RGBA8, 1, false);
    INFO(fbe_error());
    REQUIRE(made != 0);
    return made;
}

/// What the shape shader's push constants are.
struct shape_t {
    std::array<float, 4> color;
    std::array<float, 2> offset;
    float size;
    float depth;
};

/// What the textured shader's push constants are.
struct textured_t {
    std::array<float, 4> rect;
    std::array<float, 4> uv;
    std::uint32_t texture;
    std::uint32_t sampler;
};

constexpr std::array<float, 4> red{ 1.0F, 0.0F, 0.0F, 1.0F };
constexpr std::array<float, 4> green{ 0.0F, 1.0F, 0.0F, 1.0F };
constexpr std::array<float, 4> blue{ 0.0F, 0.0F, 1.0F, 1.0F };
constexpr std::array<float, 4> navy{ 0.0F, 0.0F, 0.2F, 1.0F };
constexpr std::array<float, 4> whole{ -1.0F, -1.0F, 1.0F, 1.0F };
constexpr fbe::gpu::transient_t small{ .width = 8, .height = 8, .format = FBE_TEXTURE_RGBA8 };

/// The passes whose commands were recorded, in order, and the indices each quad pass sampled.
std::vector<std::string_view> ran;
std::vector<std::uint32_t> sampled;

template <typename Constants>
void push(VkCommandBuffer commands, const Constants& constants) {
    vkCmdPushConstants(commands, fbe::gpu::pipeline_layout(),
                       flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL), 0, sizeof(Constants),
                       &constants);
}

/// Adds a pass that draws a triangle of `color` into `into`, over navy unless it draws on what's
/// there.
void triangle(std::string_view name, resource_t into, std::array<float, 4> color,
              bool clear = true) {
    require(fbe::gpu::add_pass({
        .name = name,
        .colors = { { .target = into, .clear = clear, .rgba = navy } },
        .record =
            [name, into, color](VkCommandBuffer commands) {
                ran.push_back(name);
                VkPipeline pipeline = fbe::gpu::pipeline({
                    .shader = &fbe::test_shaders::shape,
                    .color = fbe::gpu::resource_format(into),
                });
                REQUIRE(pipeline != VK_NULL_HANDLE);
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                push(commands,
                     shape_t{ .color = color, .offset = {}, .size = 0.8F, .depth = 0.5F });
                vkCmdDraw(commands, 3, 1, 0, 0);
            },
    }));
}

/// Adds a pass that draws `from` into `rect` of `into`, in clip space, over what's there, or
/// clearing it first.
void quad(std::string_view name, resource_t into, resource_t from,
          std::array<float, 4> rect = whole, bool clear = true) {
    require(fbe::gpu::add_pass({
        .name = name,
        .colors = { { .target = into, .clear = clear, .rgba = navy } },
        .uses = { { from, use_t::SAMPLED } },
        .record =
            [name, into, from, rect](VkCommandBuffer commands) {
                ran.push_back(name);
                sampled.push_back(fbe::gpu::resource_index(from));
                VkPipeline pipeline = fbe::gpu::pipeline({
                    .shader = &fbe::test_shaders::textured,
                    .color = fbe::gpu::resource_format(into),
                });
                REQUIRE(pipeline != VK_NULL_HANDLE);
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                VkDescriptorSet set = fbe::gpu::bindless_set();
                vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        fbe::gpu::pipeline_layout(), 0, 1, &set, 0, nullptr);
                push(commands, textured_t{
                                   .rect = rect,
                                   .uv = { 0.0F, 0.0F, 1.0F, 1.0F },
                                   .texture = fbe::gpu::resource_index(from),
                                   .sampler = fbe::gpu::sampler_index(
                                       FBE_TEXTURE_NEAREST, FBE_TEXTURE_CLAMP, FBE_TEXTURE_CLAMP),
                               });
                vkCmdDraw(commands, 6, 1, 0, 0);
            },
    }));
}

fbe::golden::frame_t read(std::uint64_t texture) {
    const auto width = static_cast<std::uint32_t>(fbe_texture_width(texture));
    const auto height = static_cast<std::uint32_t>(fbe_texture_height(texture));
    fbe::golden::frame_t frame{ .width = width, .height = height };
    frame.rgba.resize(std::size_t{ width } * height * 4);
    require(fbe_texture_read(texture, 0, frame.rgba.data(),
                             static_cast<std::int64_t>(frame.rgba.size())));
    return frame;
}

std::array<std::uint8_t, 4> pixel(const fbe::golden::frame_t& frame, std::uint32_t x,
                                  std::uint32_t y) {
    const std::size_t at = ((std::size_t{ y } * frame.width) + x) * 4;
    return { frame.rgba[at], frame.rgba[at + 1], frame.rgba[at + 2], frame.rgba[at + 3] };
}

/// The middle of an 8x8 triangle's pixels, as `triangle` draws it, and a corner, outside it.
std::array<std::uint8_t, 4> middle(std::uint64_t texture) {
    return pixel(read(texture), 4, 5);
}
std::array<std::uint8_t, 4> corner(std::uint64_t texture) {
    return pixel(read(texture), 0, 0);
}

constexpr std::array<std::uint8_t, 4> red8{ 255, 0, 0, 255 };
constexpr std::array<std::uint8_t, 4> green8{ 0, 255, 0, 255 };
constexpr std::array<std::uint8_t, 4> blue8{ 0, 0, 255, 255 };

void begin_test() {
    ran.clear();
    sampled.clear();
    start();
}

/// A headless window `width` by `height`, and the GPU started.
void open(std::int32_t width, std::int32_t height) {
    ran.clear();
    sampled.clear();
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_window_open(width, height, FBE_WINDOW_WINDOWED));
    require(fbe_gpu_start());
}

} // namespace

// a draws what b samples, c what d samples. Added a, c, b, d, a and c share a level, then b and d,
// a barrier command before each level and one after, not one a pass; added a, b, c, d, c joins
// b's level, which it doesn't depend on, and d needs one of its own.
TEST_CASE("passes keep their order, in levels of those that don't depend on each other", "[gpu]") {
    begin_test();
    const std::array<std::uint64_t, 4> made{
        target(8, 8),
        target(8, 8),
        target(8, 8),
        target(8, 8),
    };
    std::array<resource_t, 4> drawn{};
    for (std::size_t at = 0; at < made.size(); ++at) {
        drawn[at] = fbe::gpu::texture_resource(made[at]);
    }
    CHECK(fbe::gpu::texture_resource(made[0]) == drawn[0]);
    triangle("a", drawn[0], red);
    triangle("c", drawn[2], green);
    quad("b", drawn[1], drawn[0]);
    quad("d", drawn[3], drawn[2]);
    require(fbe::gpu::finish_frame());
    const fbe::gpu::recorded_t& recorded = fbe::gpu::last_recorded();
    CHECK(recorded.passes == std::vector<std::string_view>{ "a", "c", "b", "d" });
    CHECK(recorded.levels == std::vector<std::uint32_t>{ 0, 0, 1, 1 });
    CHECK(ran == recorded.passes);
    CHECK(recorded.dropped == 0);
    // a's and c's targets to be drawn; then to be sampled, with b's and d's to be drawn; then b's
    // and d's back at rest. a's and c's are at rest as they're sampled.
    CHECK(recorded.batches == 3);
    CHECK(recorded.barriers == 8);
    CHECK(middle(made[1]) == red8);
    CHECK(middle(made[3]) == green8);
    CHECK(corner(made[3])[2] > 100);

    triangle("a", drawn[0] = fbe::gpu::texture_resource(made[0]), red);
    drawn[1] = fbe::gpu::texture_resource(made[1]);
    drawn[2] = fbe::gpu::texture_resource(made[2]);
    drawn[3] = fbe::gpu::texture_resource(made[3]);
    quad("b", drawn[1], drawn[0]);
    triangle("c", drawn[2], blue);
    quad("d", drawn[3], drawn[2]);
    require(fbe::gpu::finish_frame());
    CHECK(recorded.passes == std::vector<std::string_view>{ "a", "b", "c", "d" });
    CHECK(recorded.levels == std::vector<std::uint32_t>{ 0, 1, 1, 2 });
    CHECK(recorded.batches == 4);
    CHECK(middle(made[3]) == blue8);
    stop_gpu();
}

// Each transient target is drawn, then drawn into the next part of the screen, in turn: kept in
// that order, two at a time are in use, not all four, as they would be if each were drawn as soon
// as it could be.
TEST_CASE("a transient target lives as long as its passes were apart as they were added", "[gpu]") {
    begin_test();
    const std::uint64_t shown = target(32, 8);
    const resource_t into = fbe::gpu::texture_resource(shown);
    for (int at = 0; at < 4; ++at) {
        const resource_t drawn = fbe::gpu::transient_resource(small);
        triangle("drawn", drawn, at % 2 == 0 ? red : green);
        const float left = -1.0F + (0.5F * static_cast<float>(at));
        quad("shown", into, drawn, { left, -1.0F, left + 0.5F, 1.0F }, at == 0);
    }
    require(fbe::gpu::finish_frame());
    CHECK(fbe::gpu::last_recorded().levels == std::vector<std::uint32_t>{ 0, 1, 1, 2, 2, 3, 3, 4 });
    CHECK(fbe::gpu::last_recorded().made == 2);
    const fbe::golden::frame_t frame = read(shown);
    CHECK(pixel(frame, 4, 5) == red8);
    CHECK(pixel(frame, 12, 5) == green8);
    CHECK(pixel(frame, 20, 5) == red8);
    CHECK(pixel(frame, 28, 5) == green8);
    stop_gpu();
}

TEST_CASE("textures sampled at rest need no barriers", "[gpu]") {
    begin_test();
    const std::uint64_t first = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, false);
    const std::uint64_t second = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, true);
    const std::uint64_t into = target(8, 8);
    REQUIRE(first != 0);
    REQUIRE(second != 0);
    require(fbe::gpu::add_pass({
        .name = "both",
        .colors = { { .target = fbe::gpu::texture_resource(into), .clear = true } },
        .uses = { { fbe::gpu::texture_resource(first), use_t::SAMPLED },
                  { fbe::gpu::texture_resource(second), use_t::SAMPLED }, },
    }));
    require(fbe::gpu::finish_frame());
    // The target to be drawn, and back.
    CHECK(fbe::gpu::last_recorded().barriers == 2);
    CHECK(fbe::gpu::last_recorded().batches == 2);
    stop_gpu();
}

TEST_CASE("what's done to a texture is done in the order it's asked for", "[gpu]") {
    begin_test();
    const std::uint64_t made = target(8, 8);
    const std::uint64_t copied = fbe_texture_create(8, 8, FBE_TEXTURE_RGBA8, true);
    REQUIRE(copied != 0);
    require(fbe_texture_clear(made, 1.0F, 0.0F, 0.0F, 1.0F));
    triangle("drawn", fbe::gpu::texture_resource(made), green, false);
    require(fbe_texture_copy(copied, 0, 0, made, 0, 0, 8, 8));
    require(fbe_texture_clear(made, 0.0F, 0.0F, 1.0F, 1.0F));
    require(fbe::gpu::finish_frame());
    CHECK(fbe::gpu::last_recorded().passes ==
          std::vector<std::string_view>{ "clear", "drawn", "copy", "clear" });
    CHECK(fbe::gpu::last_recorded().levels == std::vector<std::uint32_t>{ 0, 1, 2, 3 });
    CHECK(middle(copied) == green8);
    CHECK(corner(copied) == red8);
    CHECK(middle(made) == blue8);
    // The copy made the copied texture's mipmaps, its 1x1 level red and green mixed with navy.
    const std::vector<std::uint8_t> last = fbe::texture::read(copied, 3);
    REQUIRE(last.size() == 4);
    CHECK(last[0] > 0);
    CHECK(last[1] > 0);
    stop_gpu();
}

TEST_CASE("a pass nothing needs is dropped, as is one whose work is drawn over", "[gpu]") {
    begin_test();
    const resource_t unread = fbe::gpu::transient_resource(small);
    const resource_t drawn_over = fbe::gpu::transient_resource(small);
    const std::uint64_t shown = target(8, 8);
    triangle("unread", unread, red);
    triangle("drawn over", drawn_over, red);
    triangle("kept", drawn_over, green);
    quad("shown", fbe::gpu::texture_resource(shown), drawn_over);
    require(fbe::gpu::finish_frame());
    CHECK(fbe::gpu::last_recorded().passes == std::vector<std::string_view>{ "kept", "shown" });
    CHECK(fbe::gpu::last_recorded().dropped == 2);
    CHECK(ran == fbe::gpu::last_recorded().passes);
    CHECK(middle(shown) == green8);
    // A pass that draws on a transient target needs what drew it before.
    const resource_t again = fbe::gpu::transient_resource(small);
    triangle("under", again, red);
    triangle("over", again, green, false);
    quad("shown", fbe::gpu::texture_resource(shown), again);
    require(fbe::gpu::finish_frame());
    CHECK(fbe::gpu::last_recorded().passes ==
          std::vector<std::string_view>{ "under", "over", "shown" });
    // A pass that writes nothing the graph knows is kept, as it may do what the graph doesn't see.
    ran.clear();
    require(fbe::gpu::add_pass({
        .name = "nothing",
        .record = [](VkCommandBuffer /*commands*/) { ran.emplace_back("nothing"); },
    }));
    require(fbe::gpu::finish_frame());
    CHECK(ran == std::vector<std::string_view>{ "nothing" });
    stop_gpu();
}

// The second transient target's passes come after the first's are done, as the second is drawn
// from what the first was drawn into, so they share an image; two used at once don't, nor two
// unlike.
TEST_CASE("transient targets share an image when their passes don't overlap", "[gpu]") {
    begin_test();
    const std::uint64_t first_shown = target(8, 8);
    const std::uint64_t second_shown = target(8, 8);
    const resource_t first = fbe::gpu::transient_resource(small);
    const resource_t second = fbe::gpu::transient_resource(small);
    const resource_t shown = fbe::gpu::texture_resource(first_shown);
    const resource_t shown_too = fbe::gpu::texture_resource(second_shown);
    triangle("first", first, red);
    quad("first shown", shown, first);
    quad("second", second, shown);
    quad("second shown", shown_too, second);
    require(fbe::gpu::finish_frame());
    CHECK(fbe::gpu::last_recorded().levels == std::vector<std::uint32_t>{ 0, 1, 2, 3 });
    CHECK(fbe::gpu::last_recorded().made == 1);
    CHECK(fbe::gpu::transient_images() == 1);
    REQUIRE(sampled.size() == 3);
    CHECK(sampled[0] == sampled[2]);
    CHECK(middle(second_shown) == red8);
    CHECK(fbe_texture_count() == 2);

    sampled.clear();
    const resource_t third = fbe::gpu::transient_resource(small);
    const resource_t fourth = fbe::gpu::transient_resource(small);
    triangle("third", third, red);
    triangle("fourth", fourth, green);
    quad("third shown", fbe::gpu::texture_resource(first_shown), third);
    quad("fourth shown", fbe::gpu::texture_resource(second_shown), fourth);
    require(fbe::gpu::finish_frame());
    CHECK(fbe::gpu::last_recorded().levels == std::vector<std::uint32_t>{ 0, 0, 1, 1 });
    CHECK(fbe::gpu::last_recorded().made == 1);
    CHECK(fbe::gpu::transient_images() == 2);
    REQUIRE(sampled.size() == 2);
    CHECK(sampled[0] != sampled[1]);
    CHECK(middle(first_shown) == red8);
    CHECK(middle(second_shown) == green8);

    const resource_t bigger =
        fbe::gpu::transient_resource({ .width = 16, .height = 16, .format = FBE_TEXTURE_RGBA8 });
    const resource_t linear = fbe::gpu::transient_resource(
        { .width = 8, .height = 8, .format = FBE_TEXTURE_RGBA8_LINEAR });
    triangle("bigger", bigger, red);
    triangle("linear", linear, red);
    quad("bigger shown", fbe::gpu::texture_resource(first_shown), bigger);
    quad("linear shown", fbe::gpu::texture_resource(second_shown), linear);
    require(fbe::gpu::finish_frame());
    CHECK(fbe::gpu::last_recorded().made == 2);
    CHECK(fbe::gpu::transient_images() == 4);
    CHECK(fbe_texture_count() == 2);
    stop_gpu();
}

TEST_CASE("transient targets' images are kept for the frames that ask for them, then let go",
          "[gpu]") {
    begin_test();
    const std::uint64_t shown = target(8, 8);
    const auto frame = [shown] {
        const resource_t drawn = fbe::gpu::transient_resource(small);
        triangle("drawn", drawn, red);
        quad("shown", fbe::gpu::texture_resource(shown), drawn);
        fbe_frame_flip(false);
    };
    frame();
    CHECK(fbe::gpu::last_recorded().made == 1);
    frame();
    CHECK(fbe::gpu::last_recorded().made == 0);
    CHECK(fbe::gpu::transient_images() == 1);
    CHECK(middle(shown) == red8);
    // Three frames that don't ask for it, and it's gone.
    fbe_frame_flip(false);
    fbe_frame_flip(false);
    CHECK(fbe::gpu::transient_images() == 1);
    fbe_frame_flip(false);
    CHECK(fbe::gpu::transient_images() == 0);
    frame();
    CHECK(fbe::gpu::last_recorded().made == 1);
    CHECK(fbe_texture_count() == 1);
    stop_gpu();
}

TEST_CASE("a transient target that can't be made drops the passes that use it", "[gpu]") {
    begin_test();
    const std::uint64_t shown = target(8, 8);
    const std::uint64_t other = target(8, 8);
    const resource_t failing = fbe::gpu::transient_resource(small);
    triangle("into it", failing, red);
    quad("from it", fbe::gpu::texture_resource(shown), failing);
    triangle("other", fbe::gpu::texture_resource(other), green);
    fbe::gpu::inject(fbe::gpu::fault_t::VIEW);
    CHECK(!fbe::gpu::record_graph());
    CHECK(std::string(fbe_error()).starts_with("couldn't make a texture's view"));
    CHECK(fbe::gpu::last_recorded().passes == std::vector<std::string_view>{ "other" });
    CHECK(fbe::gpu::last_recorded().dropped == 2);
    CHECK(fbe::gpu::transient_images() == 0);
    CHECK(middle(other) == green8);
    CHECK(middle(shown) == std::array<std::uint8_t, 4>{ 0, 0, 0, 0 });
    stop_gpu();
}

TEST_CASE("the graph refuses passes it can't run", "[gpu]") {
    begin_test();
    const resource_t big = fbe::gpu::texture_resource(target(16, 16));
    const resource_t little = fbe::gpu::texture_resource(target(8, 8));
    const std::uint64_t plain = fbe_texture_create(8, 8, FBE_TEXTURE_RGBA8, false);
    REQUIRE(plain != 0);
    const auto refused = [](fbe::gpu::pass_t pass, const std::string& why) {
        CHECK(!fbe::gpu::add_pass(std::move(pass)));
        CHECK(std::string(fbe_error()) == why);
    };
    refused({ .colors = { { .target = 99 } } }, "a pass's target isn't in the frame's graph");
    refused({ .colors = { { .target = fbe::gpu::texture_resource(plain) } } },
            "the texture isn't a target, which draws");
    refused({ .colors = { { .target = big }, { .target = little } } },
            "a pass's targets are one size and draw as many samples");
    refused({ .colors = { { .target = little } }, .depth = little },
            "a pass's depth is a target's with depth");
    refused({ .colors = { { .target = little } }, .uses = { { little, use_t::SAMPLED } } },
            "a pass uses an image once, never drawing into a texture it reads");
    refused({ .uses = { { 99, use_t::SAMPLED } } },
            "a texture a pass uses isn't in the frame's graph");
    const resource_t own = fbe::gpu::external_resource({ .extent = { .width = 8, .height = 8 } });
    refused({ .uses = { { own, use_t::COPY_FROM } } },
            "an image of the frame's own is only drawn into");
    CHECK(fbe::gpu::texture_resource(0) == fbe::gpu::no_resource);
    CHECK(fbe::gpu::transient_resource({ .width = 0, .height = 8, .format = FBE_TEXTURE_RGBA8 }) ==
          fbe::gpu::no_resource);
    CHECK(std::string(fbe_error()).ends_with("pixels on a side, not 0x8"));
    CHECK(fbe::gpu::transient_resource({ .width = 8, .height = 8, .format = 99 }) ==
          fbe::gpu::no_resource);
    CHECK(std::string(fbe_error()) == "99 isn't a TextureFormat");
    CHECK(fbe::gpu::transient_resource(
              { .width = 8, .height = 8, .format = FBE_TEXTURE_RGBA8, .samples = 3 }) ==
          fbe::gpu::no_resource);
    CHECK(std::string(fbe_error()) == "a target draws 1, 2, 4, or 8 samples a pixel, not 3");
    // Nothing's added to the graph as it's recorded; the image of its own nothing drew into is
    // left as it was.
    std::string why;
    require(fbe::gpu::add_pass({
        .name = "adds",
        .record =
            [&why](VkCommandBuffer /*commands*/) {
                CHECK(!fbe::gpu::add_pass({ .name = "added" }));
                why = fbe_error();
            },
    }));
    require(fbe::gpu::finish_frame());
    CHECK(why == "a pass isn't added as the frame's graph is recorded");
    CHECK(fbe::gpu::last_recorded().passes == std::vector<std::string_view>{ "adds" });
    stop_gpu();
}

// E2's frame graph: a multisampled transient target with depth, the near red triangle over the
// far green one, and a plain one with a blue triangle, drawn side by side into the screen, which
// Flip scales into the window.
TEST_CASE("a frame drawn through transient targets matches its golden image", "[gpu][golden]") {
    open(96, 64);
    const std::uint64_t handle = fbe_gpu_screen();
    REQUIRE(handle != 0);
    const resource_t screen = fbe::gpu::texture_resource(handle);
    const resource_t deep = fbe::gpu::transient_resource(
        { .width = 48, .height = 32, .format = FBE_TEXTURE_RGBA8, .samples = 4, .depth = true });
    require(fbe::gpu::add_pass({
        .name = "deep",
        .colors = { { .target = deep, .clear = true, .rgba = navy } },
        .depth = deep,
        .clear_depth = true,
        .record =
            [](VkCommandBuffer commands) {
                VkPipeline pipeline = fbe::gpu::pipeline({
                    .shader = &fbe::test_shaders::shape,
                    .color = VK_FORMAT_R8G8B8A8_SRGB,
                    .depth = fbe::texture::depth_format,
                    .samples = VK_SAMPLE_COUNT_4_BIT,
                    .depth_test = true,
                    .depth_write = true,
                });
                REQUIRE(pipeline != VK_NULL_HANDLE);
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                push(commands, shape_t{
                                   .color = red,
                                   .offset = { -0.2F, 0.0F },
                                   .size = 0.6F,
                                   .depth = 0.75F,
                               });
                vkCmdDraw(commands, 3, 1, 0, 0);
                push(commands, shape_t{
                                   .color = green,
                                   .offset = { 0.2F, 0.1F },
                                   .size = 0.6F,
                                   .depth = 0.25F,
                               });
                vkCmdDraw(commands, 3, 1, 0, 0);
            },
    }));
    const resource_t flat =
        fbe::gpu::transient_resource({ .width = 48, .height = 32, .format = FBE_TEXTURE_RGBA8 });
    triangle("flat", flat, blue);
    require(fbe_texture_clear(handle, 0.05F, 0.05F, 0.05F, 1.0F));
    quad("deep shown", screen, deep, { -0.9F, -0.6F, -0.05F, 0.6F }, false);
    quad("flat shown", screen, flat, { 0.05F, -0.6F, 0.9F, 0.6F }, false);
    fbe_frame_flip(false);
    CHECK(fbe::gpu::last_recorded().passes ==
          std::vector<std::string_view>{ "deep", "flat", "clear", "deep shown", "flat shown",
                                         "present" });
    CHECK(fbe::gpu::last_recorded().made == 2);
    const fbe::golden::frame_t frame{
        .width = static_cast<std::uint32_t>(fbe_window_pixel_width()),
        .height = static_cast<std::uint32_t>(fbe_window_pixel_height()),
        .rgba = fbe::gpu::read_window(),
    };
    fbe::golden::check("graph", frame);
    fbe_window_close();
    stop_gpu();
}

// Each readback records the frame, which makes a transient target's image, a texture, as it goes:
// the texture read is found again after, wherever the pool's moved.
TEST_CASE("a readback that makes a transient target's image reads what was drawn", "[gpu]") {
    begin_test();
    const std::uint64_t shown = target(8, 8);
    for (std::uint32_t at = 0; at < 40; ++at) {
        INFO("readback " << at);
        const resource_t drawn = fbe::gpu::transient_resource(
            { .width = 8 + at, .height = 8, .format = FBE_TEXTURE_RGBA8 });
        triangle("drawn", drawn, red);
        quad("shown", fbe::gpu::texture_resource(shown), drawn);
        CHECK(middle(shown) == red8);
    }
    CHECK(fbe::gpu::transient_images() == 40);
    stop_gpu();
}

// The first transient target can't be made, so what draws into it is dropped, and so is what draws
// the second from it, and what draws the texture on the second, whose image holds a frame before's.
TEST_CASE("a pass that needs what a dropped pass drew is dropped too", "[gpu]") {
    begin_test();
    const std::uint64_t shown = target(16, 16);
    constexpr fbe::gpu::transient_t larger{
        .width = 16,
        .height = 16,
        .format = FBE_TEXTURE_RGBA8,
    };
    const resource_t before = fbe::gpu::transient_resource(larger);
    triangle("red before", before, red);
    quad("shown before", fbe::gpu::texture_resource(shown), before);
    require(fbe::gpu::finish_frame());
    require(fbe_texture_clear(shown, 0.0F, 0.0F, 1.0F, 1.0F));
    require(fbe::gpu::finish_frame());
    const resource_t first = fbe::gpu::transient_resource(small);
    const resource_t second = fbe::gpu::transient_resource(larger);
    triangle("first", first, green);
    quad("second", second, first);
    quad("shown", fbe::gpu::texture_resource(shown), second, whole, false);
    fbe::gpu::inject(fbe::gpu::fault_t::VIEW);
    CHECK(!fbe::gpu::record_graph());
    CHECK(fbe::gpu::last_recorded().passes.empty());
    CHECK(fbe::gpu::last_recorded().dropped == 3);
    CHECK(middle(shown) == blue8);
    stop_gpu();
}

// A write of pixels after a draw sampled the texture is a pass in the frame's commands, which a
// failed submit drops; it's carried to the next, as the uploads are.
TEST_CASE("a write that's a pass is carried past a failed submit", "[gpu]") {
    begin_test();
    const std::uint64_t texture = fbe_texture_create(2, 2, FBE_TEXTURE_RGBA8_LINEAR, false);
    REQUIRE(texture != 0);
    const std::uint64_t shown = target(2, 2);
    quad("drawn", fbe::gpu::texture_resource(shown), fbe::gpu::texture_resource(texture));
    const std::vector<std::uint8_t> written(16, 77);
    require(fbe_texture_write(texture, 0, 0, 2, 2, written.data(), 8));
    fbe::gpu::inject(fbe::gpu::fault_t::SUBMIT);
    CHECK(!fbe::gpu::finish_frame());
    CHECK(fbe::texture::read(texture, 0) == written);
    CHECK(fbe::gpu::last_recorded().passes == std::vector<std::string_view>{ "write" });
    stop_gpu();
}

// The last frame's counters: the draws and triangles its passes counted, Flip's present's too, its
// passes and barriers, the bytes it uploaded, and, a frame or two on, the GPU's time over it.
TEST_CASE("the last frame's counters say what it did", "[gpu]") {
    open(64, 32);
    const std::uint64_t texture = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, false);
    REQUIRE(texture != 0);
    fbe_frame_flip(false);
    const std::vector<std::uint8_t> pixels(64, 255);
    require(fbe_texture_write(texture, 0, 0, 4, 4, pixels.data(), 16));
    require(fbe::gpu::add_pass({
        .name = "counted",
        .colors = { { .target = fbe::gpu::texture_resource(fbe_gpu_screen()) } },
        .record = [](VkCommandBuffer /*commands*/) { fbe::gpu::count_draws(3, 12); },
    }));
    fbe_frame_flip(false);
    fbe_gpu_counters_t counters{};
    require(fbe_gpu_counters(&counters));
    CHECK(counters.draws == 4);
    CHECK(counters.triangles == 13);
    CHECK(counters.passes == 2);
    CHECK(std::cmp_equal(counters.barriers, fbe::gpu::last_recorded().barriers));
    CHECK(counters.uploaded == 64);
    CHECK(counters.transient_images == 0);
    fbe_frame_flip(false);
    fbe_frame_flip(false);
    fbe_frame_flip(false);
    require(fbe_gpu_counters(&counters));
    CHECK(counters.draws == 1);
    CHECK(counters.uploaded == 0);
    if (fbe::gpu::device()->timestamp_bits > 0) {
        CHECK(counters.gpu_milliseconds >= 0.0F);
        WARN("the GPU's time over an empty 64x32 frame: " << counters.gpu_milliseconds << " ms");
    } else {
        CHECK(counters.gpu_milliseconds == -1.0F);
    }
    CHECK(!fbe_gpu_counters(nullptr));
    fbe_window_close();
    stop_gpu();
}

// Not a check, a measure: what the graph costs the CPU for a 1280x720 frame of 65 passes, 32
// transient targets each drawn, then drawn into the screen sampling four textures besides, and
// the present: adding them, and recording them, with what their own commands cost, timed, the
// frame begun before, and the whole frame, the GPU's work too.
TEST_CASE("a frame of 65 passes through the graph, timed", "[gpu]") {
    open(1280, 720);
    std::array<resource_t, 4> extra{};
    std::array<std::uint64_t, 4> textures{};
    for (std::uint64_t& texture : textures) {
        texture = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, false);
        REQUIRE(texture != 0);
    }
    const auto add = [&textures, &extra] {
        const resource_t screen = fbe::gpu::texture_resource(fbe_gpu_screen());
        for (std::size_t at = 0; at < extra.size(); ++at) {
            extra[at] = fbe::gpu::texture_resource(textures[at]);
        }
        for (int at = 0; at < 32; ++at) {
            const resource_t drawn = fbe::gpu::transient_resource(
                { .width = 64, .height = 64, .format = FBE_TEXTURE_RGBA8 });
            triangle("drawn", drawn, red);
            const float left = -1.0F + (0.0625F * static_cast<float>(at));
            require(fbe::gpu::add_pass({
                .name = "shown",
                .colors = { { .target = screen } },
                .uses = { { drawn, use_t::SAMPLED },
                          { extra[0], use_t::SAMPLED },
                          { extra[1], use_t::SAMPLED },
                          { extra[2], use_t::SAMPLED },
                          { extra[3], use_t::SAMPLED }, },
                .record =
                    [drawn, left](VkCommandBuffer commands) {
                        VkPipeline pipeline = fbe::gpu::pipeline({
                            .shader = &fbe::test_shaders::textured,
                            .color = VK_FORMAT_R8G8B8A8_SRGB,
                        });
                        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                        VkDescriptorSet set = fbe::gpu::bindless_set();
                        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                fbe::gpu::pipeline_layout(), 0, 1, &set, 0,
                                                nullptr);
                        push(commands, textured_t{
                                           .rect = { left, -0.5F, left + 0.0625F, 0.5F },
                                           .uv = { 0.0F, 0.0F, 1.0F, 1.0F },
                                           .texture = fbe::gpu::resource_index(drawn),
                                       });
                        vkCmdDraw(commands, 6, 1, 0, 0);
                    },
            }));
        }
    };
    for (int at = 0; at < 5; ++at) {
        add();
        fbe_frame_flip(false);
    }
    constexpr int frames = 50;
    std::chrono::duration<double, std::milli> adding{};
    std::chrono::duration<double, std::milli> recording{};
    const auto started = std::chrono::steady_clock::now();
    for (int at = 0; at < frames; ++at) {
        // Begun first, which waits for the frame in flight before, as the graph doesn't.
        fbe::gpu::recording_frame();
        const auto before = std::chrono::steady_clock::now();
        add();
        const auto added = std::chrono::steady_clock::now();
        fbe::gpu::record_graph();
        recording += std::chrono::steady_clock::now() - added;
        adding += added - before;
        fbe_frame_flip(false);
    }
    vkDeviceWaitIdle(fbe::gpu::device()->device);
    const std::chrono::duration<double, std::milli> spent =
        std::chrono::steady_clock::now() - started;
    CHECK(fbe::gpu::transient_images() == 2);
    fbe_gpu_counters_t counters{};
    require(fbe_gpu_counters(&counters));
    WARN("65 passes: adding " << adding.count() / frames << " ms, recording "
                              << recording.count() / frames << " ms, the frame "
                              << spent.count() / frames << " ms, the GPU's "
                              << counters.gpu_milliseconds << " ms, on "
                              << fbe_gpu_device_name(fbe_gpu_device()));
    fbe_window_close();
    stop_gpu();
}

// A write's in turn with what else the frame does with a texture: after a clear, a copy, or a draw
// sampling it that came before, as a pass of its own, and before what comes after; a texture the
// frame hasn't used yet is written in the uploads, before the frame's commands.
TEST_CASE("a write comes in turn with what the frame does with the texture", "[gpu]") {
    begin_test();
    const std::uint64_t texture = fbe_texture_create(2, 2, FBE_TEXTURE_RGBA8_LINEAR, false);
    REQUIRE(texture != 0);
    const std::vector<std::uint8_t> blue{
        0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255,
    };
    const std::vector<std::uint8_t> red{
        255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
    };
    require(fbe_texture_clear(texture, 1.0F, 0.0F, 0.0F, 1.0F));
    require(fbe_texture_write(texture, 0, 0, 2, 2, blue.data(), 8));
    CHECK(fbe::texture::read(texture, 0) == blue);
    CHECK(fbe::gpu::last_recorded().passes == std::vector<std::string_view>{ "clear", "write" });

    const std::uint64_t green = fbe_texture_create(2, 2, FBE_TEXTURE_RGBA8_LINEAR, false);
    REQUIRE(green != 0);
    require(fbe_texture_clear(green, 0.0F, 1.0F, 0.0F, 1.0F));
    require(fbe::gpu::finish_frame());
    require(fbe_texture_copy(texture, 0, 0, green, 0, 0, 2, 2));
    require(fbe_texture_write(texture, 0, 0, 2, 2, blue.data(), 8));
    CHECK(fbe::texture::read(texture, 0) == blue);

    // Drawn with before the write, it's drawn as it was.
    const std::uint64_t shown = target(2, 2);
    quad("drawn", fbe::gpu::texture_resource(shown), fbe::gpu::texture_resource(texture));
    require(fbe_texture_write(texture, 0, 0, 2, 2, red.data(), 8));
    CHECK(pixel(read(shown), 1, 1) == blue8);
    CHECK(fbe::texture::read(texture, 0) == red);

    // Written before the frame uses it, it's in the uploads, and what's drawn with it after has it.
    require(fbe_texture_write(texture, 0, 0, 2, 2, blue.data(), 8));
    quad("drawn", fbe::gpu::texture_resource(shown), fbe::gpu::texture_resource(texture));
    CHECK(pixel(read(shown), 1, 1) == blue8);
    CHECK(fbe::gpu::last_recorded().passes == std::vector<std::string_view>{ "drawn" });
    stop_gpu();
}
