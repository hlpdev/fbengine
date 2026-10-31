// Targets and the screen: textures drawn into, multisampled and with reversed float depth; their
// clears, copies, and pixels read back; and the screen, its own size or the window's, scaled into
// the window by Flip. Each test ends by stopping the GPU, which has to leave nothing behind.

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
#include <memory>
#include <optional>
#include <string>
#include <test_shaders.hpp>
#include <texture/texture.hpp>
#include <vector>

using fbe::tests::require;
using fbe::tests::stop_gpu;

namespace {

using fbe::gpu::flag;

void start() {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_gpu_start());
}

/// A headless window `width` by `height`, and the GPU started.
void open(std::int32_t width, std::int32_t height) {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_window_open(width, height, FBE_WINDOW_WINDOWED));
    require(fbe_gpu_start());
}

std::uint64_t target(std::int32_t width, std::int32_t height, std::int32_t samples, bool depth) {
    const std::uint64_t made =
        fbe_texture_create_target(width, height, FBE_TEXTURE_RGBA8, samples, depth);
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

/// Draws `shapes` into a target in a pass, clearing it to `clear` first, or drawing on it without
/// one, depth tested if it has depth, its depth cleared to the farthest with the color.
void draw(std::uint64_t into, std::optional<std::array<float, 4>> clear,
          const std::vector<shape_t>& shapes) {
    const fbe::texture::texture_t& found = *fbe::texture::find(into);
    const bool depth = found.depth != VK_NULL_HANDLE;
    VkPipeline pipeline = fbe::gpu::pipeline({
        .shader = &fbe::test_shaders::shape,
        .color = found.vk_format,
        .depth = depth ? fbe::texture::depth_format : VK_FORMAT_UNDEFINED,
        .samples = found.samples,
        .depth_test = depth,
        .depth_write = depth,
    });
    INFO(fbe_error());
    REQUIRE(pipeline != VK_NULL_HANDLE);
    const fbe::gpu::resource_t target = fbe::gpu::texture_resource(into);
    require(fbe::gpu::add_pass({
        .name = "shapes",
        .colors = { {
            .target = target,
            .clear = clear.has_value(),
            .rgba = clear.value_or(std::array<float, 4>{}),
        }, },
        .depth = depth ? target : fbe::gpu::no_resource,
        .clear_depth = clear.has_value(),
        .record =
            [pipeline, drawn = std::make_shared<const std::vector<shape_t>>(shapes)](
                VkCommandBuffer commands) {
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                for (const shape_t& shape : *drawn) {
                    vkCmdPushConstants(commands, fbe::gpu::pipeline_layout(),
                                       flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL), 0,
                                       sizeof(shape_t), &shape);
                    vkCmdDraw(commands, 3, 1, 0, 0);
                }
            },
    }));
}

/// A texture's first level read back through the C API.
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

/// The 8x8 test card, as texture_tests.cpp's, opaque.
std::uint64_t card() {
    const std::uint64_t texture = fbe_texture_create(8, 8, FBE_TEXTURE_RGBA8, false);
    REQUIRE(texture != 0);
    std::vector<std::uint8_t> pixels;
    for (std::uint8_t y = 0; y < 8; ++y) {
        for (std::uint8_t x = 0; x < 8; ++x) {
            pixels.push_back(static_cast<std::uint8_t>(x * 36));
            pixels.push_back(static_cast<std::uint8_t>(y * 36));
            pixels.push_back(static_cast<std::uint8_t>(255 - ((x + y) * 18)));
            pixels.push_back(255);
        }
    }
    require(fbe_texture_write(texture, 0, 0, 8, 8, pixels.data(), 32));
    return texture;
}

constexpr std::array<float, 4> red{ 1.0F, 0.0F, 0.0F, 1.0F };
constexpr std::array<float, 4> green{ 0.0F, 1.0F, 0.0F, 1.0F };
constexpr std::array<float, 4> navy{ 0.0F, 0.0F, 0.2F, 1.0F };

/// The screen, cleared to a gray, with the test card copied into three corners of it.
void paint_screen() {
    const std::uint64_t screen = fbe_gpu_screen();
    REQUIRE(screen != 0);
    require(fbe_texture_clear(screen, 0.05F, 0.05F, 0.05F, 1.0F));
    const std::uint64_t pattern = card();
    const std::int32_t width = fbe_texture_width(screen);
    const std::int32_t height = fbe_texture_height(screen);
    require(fbe_texture_copy(screen, 0, 0, pattern, 0, 0, 8, 8));
    require(fbe_texture_copy(screen, width - 8, 0, pattern, 0, 0, 8, 8));
    require(fbe_texture_copy(screen, (width / 2) - 4, height - 8, pattern, 0, 0, 8, 8));
    REQUIRE(fbe_texture_release(pattern));
}

/// The headless window's image after a Flip.
fbe::golden::frame_t flipped() {
    fbe_frame_flip(false);
    return {
        .width = static_cast<std::uint32_t>(fbe_window_pixel_width()),
        .height = static_cast<std::uint32_t>(fbe_window_pixel_height()),
        .rgba = fbe::gpu::read_window(),
    };
}

} // namespace

TEST_CASE("a target has the samples and depth it was made with", "[gpu]") {
    start();
    const std::uint64_t made = target(64, 32, 4, true);
    CHECK(fbe_texture_is_target(made));
    CHECK(fbe_texture_samples(made) == 4);
    CHECK(fbe_texture_has_depth(made));
    CHECK(fbe_texture_levels(made) == 1);
    const std::uint64_t plain = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, false);
    CHECK(!fbe_texture_is_target(plain));
    CHECK(fbe_texture_samples(plain) == 1);
    CHECK(!fbe_texture_has_depth(plain));
    CHECK(fbe_texture_create_target(4, 4, FBE_TEXTURE_RGBA16F, 1, false) != 0);
    CHECK(fbe_texture_create_target(4, 4, FBE_TEXTURE_RGBA8, 3, false) == 0);
    CHECK(std::string(fbe_error()) == "a target draws 1, 2, 4, or 8 samples a pixel, not 3");
    CHECK(fbe_texture_create_target(0, 4, FBE_TEXTURE_RGBA8, 1, false) == 0);
    // Only a target's drawn into.
    CHECK(!fbe::gpu::add_pass({ .colors = { { .target = fbe::gpu::texture_resource(plain) } } }));
    CHECK(std::string(fbe_error()) == "the texture isn't a target, which draws");
    stop_gpu();
}

// E2's readback: a multisampled target with depth, drawn into and resolved, read back.
TEST_CASE("a target drawn with depth and multisampled matches its golden image", "[gpu][golden]") {
    start();
    const std::uint64_t made = target(128, 128, 4, true);
    // The near red one first; the far green one behind it shows only where red isn't.
    draw(made, navy,
         {
             { .color = red, .offset = { -0.2F, 0.0F }, .size = 0.6F, .depth = 0.75F },
             { .color = green, .offset = { 0.2F, 0.1F }, .size = 0.6F, .depth = 0.25F },
         });
    const fbe::golden::frame_t frame = read(made);
    // Where they overlap, the nearer, red; the clear's navy in a corner.
    CHECK(pixel(frame, 64, 70) == std::array<std::uint8_t, 4>{ 255, 0, 0, 255 });
    CHECK(pixel(frame, 2, 2)[2] > 100);
    fbe::golden::check("target", frame);
    stop_gpu();
}

// Reversed 32-bit float depth keeps far things apart with a near plane of 0.01: depth near / z
// from an infinite reversed projection, at 900 and 900.5 units.
TEST_CASE("reversed float depth tells things 900 units away apart, near 0.01", "[gpu]") {
    start();
    const std::uint64_t made = target(16, 16, 1, true);
    constexpr float nearest = 0.01F;
    const float nearer = nearest / 900.0F;
    const float farther = nearest / 900.5F;
    REQUIRE(nearer != farther);
    draw(made, navy,
         {
             { .color = green, .offset = {}, .size = 1.0F, .depth = farther },
             { .color = red, .offset = {}, .size = 1.0F, .depth = nearer },
         });
    CHECK(pixel(read(made), 8, 8) == std::array<std::uint8_t, 4>{ 255, 0, 0, 255 });
    draw(made, navy,
         {
             { .color = red, .offset = {}, .size = 1.0F, .depth = nearer },
             { .color = green, .offset = {}, .size = 1.0F, .depth = farther },
         });
    CHECK(pixel(read(made), 8, 8) == std::array<std::uint8_t, 4>{ 255, 0, 0, 255 });
    stop_gpu();
}

TEST_CASE("a pass keeps what the last drew unless it clears", "[gpu]") {
    start();
    const std::uint64_t made = target(16, 16, 4, false);
    draw(made, navy, { { .color = red, .offset = { -0.5F, 0.0F }, .size = 0.4F, .depth = 0.5F } });
    // A second pass that draws on what's there.
    draw(made, {}, { { .color = green, .offset = { 0.5F, 0.0F }, .size = 0.4F, .depth = 0.5F } });
    const fbe::golden::frame_t frame = read(made);
    CHECK(pixel(frame, 4, 9) == std::array<std::uint8_t, 4>{ 255, 0, 0, 255 });
    CHECK(pixel(frame, 12, 9) == std::array<std::uint8_t, 4>{ 0, 255, 0, 255 });
    stop_gpu();
}

TEST_CASE("a clear sets every level, and a target's depth", "[gpu]") {
    start();
    const std::uint64_t mipmapped = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8_LINEAR, true);
    require(fbe_texture_clear(mipmapped, 1.0F, 0.5F, 0.0F, 1.0F));
    const std::vector<std::uint8_t> last = fbe::texture::read(mipmapped, 2);
    REQUIRE(last.size() == 4);
    CHECK(last[0] == 255);
    // 0.5 is 127 or 128, as a GPU rounds it.
    CHECK((last[1] == 127 || last[1] == 128));
    CHECK(last[2] == 0);
    CHECK(last[3] == 255);
    // The target's depth is cleared to the farthest, so anything nearer draws.
    const std::uint64_t made = target(8, 8, 1, true);
    draw(made, navy, { { .color = red, .offset = {}, .size = 1.0F, .depth = 0.9F } });
    require(fbe_texture_clear(made, 0.0F, 0.0F, 0.0F, 1.0F));
    CHECK(pixel(read(made), 4, 4) == std::array<std::uint8_t, 4>{ 0, 0, 0, 255 });
    draw(made, {}, { { .color = green, .offset = {}, .size = 1.0F, .depth = 0.1F } });
    CHECK(pixel(read(made), 4, 4) == std::array<std::uint8_t, 4>{ 0, 255, 0, 255 });
    stop_gpu();
}

TEST_CASE("a copy between textures is exact, and refuses what can't be copied", "[gpu]") {
    start();
    const std::uint64_t made = target(32, 32, 4, true);
    draw(made, navy, { { .color = red, .offset = {}, .size = 0.8F, .depth = 0.5F } });
    const std::uint64_t copied = fbe_texture_create(16, 16, FBE_TEXTURE_RGBA8_LINEAR, true);
    require(fbe_texture_copy(copied, 0, 0, made, 8, 8, 16, 16));
    const fbe::golden::frame_t whole = read(made);
    const fbe::golden::frame_t part = read(copied);
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            INFO(x << ", " << y);
            REQUIRE(pixel(part, x, y) == pixel(whole, x + 8, y + 8));
        }
    }
    const std::uint64_t gray = fbe_texture_create(16, 16, FBE_TEXTURE_R8, false);
    CHECK(!fbe_texture_copy(gray, 0, 0, made, 0, 0, 4, 4));
    CHECK(std::string(fbe_error()) ==
          "textures of formats of different sizes aren't copied between");
    CHECK(!fbe_texture_copy(copied, 0, 0, copied, 4, 4, 4, 4));
    CHECK(!fbe_texture_copy(copied, 8, 8, made, 0, 0, 16, 16));
    CHECK(std::string(fbe_error()) == "a 16x16 region at 8, 8 isn't in a 16x16 texture");
    CHECK(!fbe_texture_copy(made, 0, 0, copied, 0, 0, 4, 4));
    CHECK(std::string(fbe_error()) == "a multisampled target is drawn into, not copied into");
    const std::vector<std::uint8_t> pixels(64, 255);
    CHECK(!fbe_texture_write(made, 0, 0, 4, 4, pixels.data(), 16));
    stop_gpu();
}

TEST_CASE("a texture's pixels read back in its format, into a buffer their size", "[gpu]") {
    start();
    const std::uint64_t half = fbe_texture_create(1, 1, FBE_TEXTURE_RGBA16F, false);
    require(fbe_texture_clear(half, 2.0F, 1.0F, 0.5F, 0.0F));
    std::array<std::uint8_t, 8> bytes{};
    require(fbe_texture_read(half, 0, bytes.data(), 8));
    CHECK(bytes == std::array<std::uint8_t, 8>{ 0x00, 0x40, 0x00, 0x3C, 0x00, 0x38, 0x00, 0x00 });
    CHECK(!fbe_texture_read(half, 0, bytes.data(), 4));
    CHECK(std::string(fbe_error()) == "the level's 8 bytes, not 4");
    CHECK(!fbe_texture_read(half, 1, bytes.data(), 8));
    stop_gpu();
}

TEST_CASE("a window's screen is its size in pixels, and Flip shows it", "[gpu]") {
    open(160, 100);
    const std::uint64_t screen = fbe_gpu_screen();
    REQUIRE(screen != 0);
    CHECK(fbe_texture_width(screen) == fbe_window_pixel_width());
    CHECK(fbe_texture_height(screen) == fbe_window_pixel_height());
    CHECK(fbe_texture_is_target(screen));
    CHECK(fbe_texture_has_depth(screen));
    require(fbe_texture_clear(screen, 1.0F, 0.0F, 0.0F, 1.0F));
    const fbe::golden::frame_t frame = flipped();
    REQUIRE(frame.rgba.size() == std::size_t{ frame.width } * frame.height * 4);
    CHECK(pixel(frame, 0, 0) == std::array<std::uint8_t, 4>{ 255, 0, 0, 255 });
    CHECK(pixel(frame, frame.width - 1, frame.height - 1) ==
          std::array<std::uint8_t, 4>{ 255, 0, 0, 255 });
    // Its pixels stay from one Flip to the next.
    const fbe::golden::frame_t again = flipped();
    CHECK(again.rgba == frame.rgba);
    fbe_window_close();
    CHECK(fbe_gpu_screen() == 0);
    stop_gpu();
}

TEST_CASE("the screen follows the window's size, as of the last Flip", "[gpu]") {
    open(160, 100);
    const std::uint64_t first = fbe_gpu_screen();
    require(fbe_window_open(200, 120, FBE_WINDOW_WINDOWED));
    fbe_frame_flip(false);
    const std::uint64_t second = fbe_gpu_screen();
    CHECK(second != first);
    CHECK(fbe_texture_width(first) == -1);
    CHECK(fbe_texture_width(second) == fbe_window_pixel_width());
    stop_gpu();
}

TEST_CASE("the screen's own size and samples make it again at once", "[gpu]") {
    open(160, 100);
    require(fbe_gpu_set_render_size(64, 48));
    require(fbe_gpu_set_render_samples(4));
    const std::uint64_t screen = fbe_gpu_screen();
    CHECK(fbe_texture_width(screen) == 64);
    CHECK(fbe_texture_height(screen) == 48);
    CHECK(fbe_texture_samples(screen) == 4);
    CHECK(fbe_gpu_render_width() == 64);
    CHECK(!fbe_gpu_set_render_size(-1, 48));
    CHECK(!fbe_gpu_set_render_size(0, 48));
    CHECK(!fbe_gpu_set_render_samples(16));
    CHECK(!fbe_gpu_set_scaling(3, true));
    require(fbe_gpu_set_render_size(0, 0));
    CHECK(fbe_texture_width(fbe_gpu_screen()) == fbe_window_pixel_width());
    require(fbe_gpu_set_render_samples(1));
    stop_gpu();
}

// A 64x48 screen in a 160x100 window: fitted, 133x100 with bars at the sides, smoothly; by whole
// pixels, twice its size, by its nearest pixels; and stretched, smoothly. Fitted by nearest pixels,
// at 2.08 times, some pixels' centers fall on the screen's pixels' edges, which drivers round
// either way, so its image is the smooth one's.
TEST_CASE("a screen of its own size is scaled into the window as asked", "[gpu][golden]") {
    open(160, 100);
    require(fbe_gpu_set_render_size(64, 48));
    paint_screen();
    fbe_gpu_rect_t area{};

    require(fbe_gpu_set_scaling(FBE_GPU_FIT, true));
    require(fbe_gpu_screen_area(&area));
    CHECK(area.x == 13);
    CHECK(area.y == 0);
    CHECK(area.width == 133);
    CHECK(area.height == 100);
    const fbe::golden::frame_t fitted = flipped();
    CHECK(pixel(fitted, 0, 50) == std::array<std::uint8_t, 4>{ 0, 0, 0, 255 });
    fbe::golden::check("screen_fit", fitted);

    require(fbe_gpu_set_scaling(FBE_GPU_INTEGER, false));
    require(fbe_gpu_screen_area(&area));
    CHECK(area.x == 16);
    CHECK(area.y == 2);
    CHECK(area.width == 128);
    CHECK(area.height == 96);
    fbe::golden::check("screen_integer", flipped());

    require(fbe_gpu_set_scaling(FBE_GPU_STRETCH, true));
    require(fbe_gpu_screen_area(&area));
    CHECK(area.width == 160);
    fbe::golden::check("screen_stretch", flipped());
    require(fbe_gpu_set_render_size(0, 0));
    require(fbe_gpu_set_scaling(FBE_GPU_FIT, true));
    stop_gpu();
}

// Not a check, a measure: a 1920x1080 Flip, its screen drawn into the window, timed.
TEST_CASE("a 1920x1080 Flip, its screen scaled in, timed", "[gpu]") {
    open(1920, 1080);
    constexpr int frames = 50;
    for (int i = 0; i < 5; ++i) {
        fbe_frame_flip(false);
    }
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        fbe_frame_flip(false);
    }
    vkDeviceWaitIdle(fbe::gpu::device()->device);
    const std::chrono::duration<double, std::milli> spent =
        std::chrono::steady_clock::now() - started;
    WARN("a 1920x1080 Flip with its screen: " << spent.count() / frames << " ms on "
                                              << fbe_gpu_device_name(fbe_gpu_device()));
    stop_gpu();
}

// The window's resized after a Flip, and the frame's drawn into the screen there is, as of that
// Flip: the next Flip shows that frame, fitted into the new size, and then makes a screen the new
// size for the frame after.
TEST_CASE("the frame drawn as the window resizes is the one Flip shows", "[gpu]") {
    open(160, 100);
    require(fbe_texture_clear(fbe_gpu_screen(), 1.0F, 0.0F, 0.0F, 1.0F));
    fbe_frame_flip(false);
    require(fbe_window_open(200, 120, FBE_WINDOW_WINDOWED));
    const std::uint64_t drawn_into = fbe_gpu_screen();
    CHECK(fbe_texture_width(drawn_into) == 160);
    require(fbe_texture_clear(drawn_into, 0.0F, 1.0F, 0.0F, 1.0F));
    const fbe::golden::frame_t frame = flipped();
    REQUIRE(frame.width == 200);
    CHECK(pixel(frame, 100, 60) == std::array<std::uint8_t, 4>{ 0, 255, 0, 255 });
    // 160x100 fitted into 200x120 is 192x120, with bars 4 wide.
    CHECK(pixel(frame, 1, 60) == std::array<std::uint8_t, 4>{ 0, 0, 0, 255 });
    CHECK(fbe_gpu_screen() != drawn_into);
    CHECK(fbe_texture_width(fbe_gpu_screen()) == 200);
    CHECK(fbe_texture_height(fbe_gpu_screen()) == 120);
    stop_gpu();
}

// Set before the GPU starts, which can't say yet what it draws, a count it lacks gives the most it
// draws below; set after, it's refused, and the screen stays as it was.
TEST_CASE("the screen draws only samples the GPU has", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_gpu_set_render_samples(2));
    require(fbe_window_open(64, 32, FBE_WINDOW_WINDOWED));
    require(fbe_gpu_start());
    const VkPhysicalDeviceLimits& limits = fbe::gpu::device()->properties.limits;
    const auto draws = [&limits](std::int32_t count) {
        return (limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts &
                static_cast<std::uint32_t>(count)) != 0;
    };
    const std::int32_t first = draws(2) ? 2 : 1;
    REQUIRE(fbe_gpu_screen() != 0);
    CHECK(fbe_texture_samples(fbe_gpu_screen()) == first);
    CHECK(fbe_gpu_render_samples() == first);
    for (const std::int32_t count : { 2, 4, 8 }) {
        INFO(count << " samples");
        const std::int32_t before = fbe_gpu_render_samples();
        if (draws(count)) {
            require(fbe_gpu_set_render_samples(count));
            CHECK(fbe_texture_samples(fbe_gpu_screen()) == count);
        } else {
            CHECK(!fbe_gpu_set_render_samples(count));
            CHECK(std::string(fbe_error()) ==
                  "this GPU doesn't draw " + std::to_string(count) + " samples a pixel");
            CHECK(fbe_gpu_render_samples() == before);
        }
    }
    require(fbe_gpu_set_render_samples(1));
    CHECK(fbe_texture_samples(fbe_gpu_screen()) == 1);
    stop_gpu();
}

// Nothing but the screen starts the GPU for a headless window, whose screen's there at once.
TEST_CASE("a headless window's screen is there before anything starts the GPU", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_window_open(32, 16, FBE_WINDOW_WINDOWED));
    REQUIRE(!fbe_gpu_started());
    const std::uint64_t screen = fbe_gpu_screen();
    CHECK(screen != 0);
    CHECK(fbe_gpu_started());
    CHECK(fbe_texture_width(screen) == 32);
    stop_gpu();
}

// A target's clip space is Vulkan's, y down: a shape at clip y -0.5 is in its top half.
TEST_CASE("a target's clip space is y down", "[gpu]") {
    start();
    const std::uint64_t made = target(16, 16, 1, false);
    draw(made, navy, { { .color = red, .offset = { 0.0F, -0.5F }, .size = 0.4F, .depth = 0.5F } });
    const fbe::golden::frame_t frame = read(made);
    CHECK(pixel(frame, 8, 4) == std::array<std::uint8_t, 4>{ 255, 0, 0, 255 });
    CHECK(pixel(frame, 8, 12)[0] == 0);
    stop_gpu();
}
