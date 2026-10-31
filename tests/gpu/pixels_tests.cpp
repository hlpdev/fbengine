// Pixels on the CPU: a texture's first level read back once until the GPU changes it, what's
// written there sent before the GPU next uses the texture, in order with drawing, and what's
// refused. Each test ends by stopping the GPU, which has to leave nothing behind.

#include "gpu_testing.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <fbengine.h>
#include <gpu/vulkan.hpp>
#include <span>
#include <string>
#include <texture/texture.hpp>
#include <vector>

using fbe::tests::require;
using fbe::tests::stop_gpu;

namespace {

void start() {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_gpu_start());
}

using rgba_t = std::array<std::uint8_t, 4>;

fbe_2d_pen_t pen(rgba_t color) {
    return { .red = color[0], .green = color[1], .blue = color[2], .alpha = color[3] };
}

/// A texture's CPU pixels, as a span of its bytes.
std::span<std::uint8_t> pixels_of(std::uint64_t texture) {
    std::int64_t size = 0;
    std::uint8_t* pixels = fbe_texture_pixels(texture, &size);
    INFO(fbe_error());
    REQUIRE(pixels != nullptr);
    return { pixels, static_cast<std::size_t>(size) };
}

rgba_t at(std::span<const std::uint8_t> pixels, std::uint32_t width, std::uint32_t x,
          std::uint32_t y) {
    const std::size_t from = ((std::size_t{ y } * width) + x) * 4;
    return { pixels[from], pixels[from + 1], pixels[from + 2], pixels[from + 3] };
}

void set(std::span<std::uint8_t> pixels, std::uint32_t width, std::uint32_t x, std::uint32_t y,
         rgba_t color) {
    std::memcpy(pixels.data() + (((std::size_t{ y } * width) + x) * 4), color.data(), 4);
}

/// A texture's first level read back from the GPU, as fbe_texture_read gives it.
std::vector<std::uint8_t> read(std::uint64_t texture, std::size_t bytes) {
    std::vector<std::uint8_t> read(bytes);
    require(fbe_texture_read(texture, 0, read.data(), static_cast<std::int64_t>(bytes)));
    return read;
}

constexpr rgba_t red{ 255, 0, 0, 255 };
constexpr rgba_t green{ 0, 255, 0, 255 };
constexpr rgba_t blue{ 0, 0, 255, 255 };

} // namespace

TEST_CASE("a texture's pixels are read back once, until the GPU changes them", "[gpu]") {
    start();
    const std::uint64_t made = fbe_texture_create_target(4, 4, FBE_TEXTURE_RGBA8_LINEAR, 1, false);
    REQUIRE(made != 0);
    require(fbe_texture_clear(made, 1.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t before = fbe::texture::pixel_readbacks();
    std::span<std::uint8_t> pixels = pixels_of(made);
    CHECK(pixels.size() == 64);
    CHECK(at(pixels, 4, 2, 2) == red);
    CHECK(fbe::texture::pixel_readbacks() == before + 1);
    // Again, and after a Flip: the same copy, nothing read back.
    CHECK(pixels_of(made).data() == pixels.data());
    fbe_frame_flip(false);
    pixels = pixels_of(made);
    CHECK(fbe::texture::pixel_readbacks() == before + 1);
    // Drawn into: read back again.
    const fbe_2d_pen_t ink = pen(green);
    require(fbe_2d_rect(made, &ink, 0, 0, 4, 4, true));
    pixels = pixels_of(made);
    CHECK(at(pixels, 1, 1, 1) == green);
    CHECK(fbe::texture::pixel_readbacks() == before + 2);
    stop_gpu();
}

TEST_CASE("pixels written reach the GPU before it next uses the texture", "[gpu]") {
    start();
    const std::uint64_t texture = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8_LINEAR, false);
    const std::uint64_t shown = fbe_texture_create_target(4, 4, FBE_TEXTURE_RGBA8_LINEAR, 1, false);
    REQUIRE(texture != 0);
    REQUIRE(shown != 0);
    const std::span<std::uint8_t> pixels = pixels_of(texture);
    set(pixels, 4, 1, 2, blue);
    set(pixels, 4, 2, 3, green);
    require(fbe_texture_pixels_written(texture, 1, 2, 2, 2));
    // Drawn into another target, it has them; read back from the GPU too.
    const fbe_2d_pen_t ink = pen({ 255, 255, 255, 255 });
    const fbe_2d_sprite_t all{ .scale_x = 1, .scale_y = 1 };
    require(fbe_2d_texture(shown, &ink, texture, &all, 0, 0));
    const std::vector<std::uint8_t> drawn = read(shown, 64);
    CHECK(at(drawn, 4, 1, 2) == blue);
    CHECK(at(drawn, 4, 2, 3) == green);
    const std::vector<std::uint8_t> back = read(texture, 64);
    CHECK(at(back, 4, 1, 2) == blue);
    stop_gpu();
}

// Written, drawn over, written again: each in its turn, whatever the GPU and the CPU do between.
TEST_CASE("pixels written and drawing keep their order", "[gpu]") {
    start();
    const std::uint64_t made = fbe_texture_create_target(4, 4, FBE_TEXTURE_RGBA8_LINEAR, 1, false);
    REQUIRE(made != 0);
    std::span<std::uint8_t> pixels = pixels_of(made);
    set(pixels, 4, 1, 1, red);
    set(pixels, 4, 3, 3, red);
    require(fbe_texture_pixels_written(made, 1, 1, 3, 3));
    const fbe_2d_pen_t ink = pen(green);
    require(fbe_2d_rect(made, &ink, 0, 0, 2, 2, true));
    pixels = pixels_of(made);
    CHECK(at(pixels, 4, 1, 1) == green);
    CHECK(at(pixels, 4, 3, 3) == red);
    set(pixels, 4, 0, 0, blue);
    require(fbe_texture_pixels_written(made, 0, 0, 1, 1));
    const std::vector<std::uint8_t> back = read(made, 64);
    CHECK(at(back, 4, 0, 0) == blue);
    CHECK(at(back, 4, 1, 1) == green);
    CHECK(at(back, 4, 3, 3) == red);
    stop_gpu();
}

TEST_CASE("pixels that can't be on the CPU, or written there, are refused", "[gpu]") {
    start();
    const std::uint64_t multisampled = fbe_texture_create_target(4, 4, FBE_TEXTURE_RGBA8, 4, false);
    REQUIRE(multisampled != 0);
    CHECK(fbe_texture_pixels(multisampled, nullptr) == nullptr);
    CHECK(std::string(fbe_error()) == "a multisampled target's pixels are only drawn");
    const std::uint64_t texture = fbe_texture_create(4, 2, FBE_TEXTURE_R8, false);
    REQUIRE(texture != 0);
    CHECK(!fbe_texture_pixels_written(texture, 0, 0, 1, 1));
    CHECK(std::string(fbe_error()) ==
          "the texture's pixels weren't on the CPU, as fbe_texture_pixels puts them");
    // An R8 texture's are a byte a pixel.
    CHECK(pixels_of(texture).size() == 8);
    CHECK(!fbe_texture_pixels_written(texture, 3, 1, 2, 1));
    CHECK(std::string(fbe_error()) == "a 2x1 region at 3, 1 isn't in a 4x2 texture");
    // Written and freed before it's sent: nothing's sent, nothing's left.
    pixels_of(texture)[0] = 9;
    require(fbe_texture_pixels_written(texture, 0, 0, 1, 1));
    REQUIRE(fbe_texture_release(texture));
    fbe_frame_flip(false);
    stop_gpu();
}
