// Textures: made, written, read back, mipmapped, freed, and drawn through the bindless set, and
// the samplers' global LOD bias and anisotropy. Each test ends by stopping the GPU, which has to
// leave nothing behind.

#include "golden/golden.hpp"
#include "gpu_testing.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <fbengine.h>
#include <gpu/image.hpp>
#include <gpu/pipeline.hpp>
#include <gpu/vulkan.hpp>
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

std::uint64_t make(std::int32_t width, std::int32_t height, std::int32_t format, bool mipmaps) {
    const std::uint64_t texture = fbe_texture_create(width, height, format, mipmaps);
    INFO(fbe_error());
    REQUIRE(texture != 0);
    return texture;
}

/// What the textured shader's push constants are.
struct quad_t {
    std::array<float, 4> rect;
    std::array<float, 4> uv;
    std::uint32_t texture;
    std::uint32_t sampler;
};

/// Draws `quads` over a dark grey clear into an sRGB image `size` big, in the frame's commands,
/// and reads it back.
fbe::golden::frame_t draw(VkExtent2D size, const std::vector<quad_t>& quads) {
    constexpr VkFormat srgb = VK_FORMAT_R8G8B8A8_SRGB;
    fbe::gpu::image_t image = fbe::gpu::make_color_image(srgb, size);
    REQUIRE(image.image != VK_NULL_HANDLE);
    VkPipeline pipeline = fbe::gpu::pipeline({
        .shader = &fbe::test_shaders::textured,
        .color = srgb,
        .blend = fbe::gpu::blend_t::ALPHA,
    });
    REQUIRE(pipeline != VK_NULL_HANDLE);
    const fbe::gpu::device_t& gpu = *fbe::gpu::device();
    VkCommandBuffer commands = fbe::gpu::frame_commands();
    const auto output =
        flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    const auto write = flag<VkAccessFlags2>(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    fbe::gpu::barrier(commands, fbe::gpu::color_transition(image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                           output, 0, output, write));
    const VkRenderingAttachmentInfo color{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = image.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { .float32 = { 0.01F, 0.01F, 0.01F, 1.0F } } },
    };
    const VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { .extent = size },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    gpu.begin_rendering(commands, &rendering);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorSet set = fbe::gpu::bindless_set();
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, fbe::gpu::pipeline_layout(),
                            0, 1, &set, 0, nullptr);
    const VkViewport viewport{
        .width = static_cast<float>(size.width),
        .height = static_cast<float>(size.height),
        .maxDepth = 1.0F,
    };
    vkCmdSetViewport(commands, 0, 1, &viewport);
    const VkRect2D scissor{ .extent = size };
    vkCmdSetScissor(commands, 0, 1, &scissor);
    for (const quad_t& quad : quads) {
        vkCmdPushConstants(commands, fbe::gpu::pipeline_layout(),
                           flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL), 0, sizeof(quad_t), &quad);
        vkCmdDraw(commands, 6, 1, 0, 0);
    }
    gpu.end_rendering(commands);
    REQUIRE(fbe::gpu::finish_frame());
    fbe::golden::frame_t frame{
        .width = size.width,
        .height = size.height,
        .rgba = fbe::gpu::read_pixels(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
    };
    fbe::gpu::destroy_image(image);
    return frame;
}

/// A quad of `texture` through the sampler for its own filter and wraps.
quad_t quad_of(std::uint64_t texture, std::array<float, 4> rect, std::array<float, 4> uv) {
    const fbe::texture::texture_t* found = fbe::texture::find(texture);
    REQUIRE(found != nullptr);
    return {
        .rect = rect,
        .uv = uv,
        .texture = fbe::texture::index_of(texture),
        .sampler = fbe::texture::sampler_of(*found),
    };
}

/// The 8x8 test card: red across, green down, blue the other way, and every other pixel's alpha
/// half, so blending shows.
std::vector<std::uint8_t> card() {
    std::vector<std::uint8_t> pixels;
    for (std::uint8_t y = 0; y < 8; ++y) {
        for (std::uint8_t x = 0; x < 8; ++x) {
            pixels.push_back(static_cast<std::uint8_t>(x * 36));
            pixels.push_back(static_cast<std::uint8_t>(y * 36));
            pixels.push_back(static_cast<std::uint8_t>(255 - ((x + y) * 18)));
            pixels.push_back((x + y) % 2 == 0 ? 255 : 128);
        }
    }
    return pixels;
}

/// One-pixel black and white stripes, 64 by 64, which a texture's mipmaps average to gray.
std::vector<std::uint8_t> stripes() {
    std::vector<std::uint8_t> pixels;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const std::uint8_t level = x % 2 == 0 ? 0 : 255;
            pixels.insert(pixels.end(), { level, level, level, 255 });
        }
    }
    return pixels;
}

std::uint8_t at(const fbe::golden::frame_t& frame, std::uint32_t x, std::uint32_t y,
                std::uint32_t channel) {
    return frame.rgba[(((std::size_t{ y } * frame.width) + x) * 4) + channel];
}

} // namespace

TEST_CASE("a texture is made transparent black, the size and format asked", "[gpu]") {
    start();
    const std::int32_t before = fbe_texture_count();
    const std::uint64_t texture = make(4, 3, FBE_TEXTURE_RGBA8_LINEAR, false);
    CHECK(fbe_texture_count() == before + 1);
    CHECK(fbe_texture_width(texture) == 4);
    CHECK(fbe_texture_height(texture) == 3);
    CHECK(fbe_texture_format(texture) == FBE_TEXTURE_RGBA8_LINEAR);
    CHECK(fbe_texture_levels(texture) == 1);
    CHECK(fbe_texture_filter(texture) == FBE_TEXTURE_LINEAR);
    CHECK(fbe_texture_wrap_u(texture) == FBE_TEXTURE_REPEAT);
    CHECK(fbe_texture_wrap_v(texture) == FBE_TEXTURE_REPEAT);
    CHECK(fbe::texture::read(texture, 0) == std::vector<std::uint8_t>(std::size_t{ 48 }, 0));
    REQUIRE(fbe_texture_release(texture));
    CHECK(fbe_texture_count() == before);
    CHECK(fbe_texture_width(texture) == -1);
    CHECK(std::string(fbe_error()) == "the texture was freed");
    CHECK(!fbe_texture_release(texture));
    stop_gpu();
}

TEST_CASE("a texture's mipmaps are a full chain, down to a pixel", "[gpu]") {
    start();
    CHECK(fbe_texture_levels(make(16, 4, FBE_TEXTURE_RGBA8, true)) == 5);
    CHECK(fbe_texture_levels(make(1, 1, FBE_TEXTURE_RGBA8, true)) == 1);
    CHECK(fbe_texture_levels(make(300, 200, FBE_TEXTURE_R8, true)) == 9);
    stop_gpu();
}

TEST_CASE("a texture made before the GPU starts starts it", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    REQUIRE(!fbe_gpu_started());
    make(2, 2, FBE_TEXTURE_RGBA8, false);
    CHECK(fbe_gpu_started());
    stop_gpu();
}

TEST_CASE("sizes and formats a texture can't have are refused", "[gpu]") {
    start();
    const auto largest =
        static_cast<std::int32_t>(fbe::gpu::device()->properties.limits.maxImageDimension2D);
    CHECK(fbe_texture_create(0, 4, FBE_TEXTURE_RGBA8, false) == 0);
    CHECK(std::string(fbe_error()) ==
          "a texture is 1 to " + std::to_string(largest) + " pixels on a side, not 0x4");
    CHECK(fbe_texture_create(4, -1, FBE_TEXTURE_RGBA8, false) == 0);
    CHECK(fbe_texture_create(largest + 1, 4, FBE_TEXTURE_RGBA8, false) == 0);
    CHECK(fbe_texture_create(4, 4, 9, false) == 0);
    CHECK(std::string(fbe_error()) == "9 isn't a TextureFormat");
    CHECK(fbe_texture_count() == 0);
    stop_gpu();
}

TEST_CASE("pixels written read back as written, in every format", "[gpu]") {
    start();
    // A 2x2 region at 1, 1 of a 4x3 texture, from rows 12 bytes apart, of which 8 are pixels.
    const std::uint64_t rgba = make(4, 3, FBE_TEXTURE_RGBA8_LINEAR, false);
    const std::vector<std::uint8_t> region{
        1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 9, 10, 11, 12, 13, 14, 15, 16, 0, 0, 0, 0,
    };
    require(fbe_texture_write(rgba, 1, 1, 2, 2, region.data(), 12));
    // Rows of 16 bytes: the region starts 4 bytes into the second and third.
    std::vector<std::uint8_t> expected(std::size_t{ 48 }, 0);
    for (std::size_t i = 0; i < 8; ++i) {
        expected[20 + i] = region[i];
        expected[36 + i] = region[12 + i];
    }
    CHECK(fbe::texture::read(rgba, 0) == expected);

    // sRGB textures keep their bytes; only sampling decodes them.
    const std::uint64_t srgb = make(1, 1, FBE_TEXTURE_RGBA8, false);
    const std::vector<std::uint8_t> orange{ 255, 128, 0, 200 };
    require(fbe_texture_write(srgb, 0, 0, 1, 1, orange.data(), 4));
    CHECK(fbe::texture::read(srgb, 0) == orange);

    const std::uint64_t r8 = make(3, 1, FBE_TEXTURE_R8, false);
    const std::vector<std::uint8_t> grays{ 0, 127, 255 };
    require(fbe_texture_write(r8, 0, 0, 3, 1, grays.data(), 3));
    CHECK(fbe::texture::read(r8, 0) == grays);

    // Half floats: 1.0 is 0x3C00, 2.0 0x4000, 0.5 0x3800, 0 0.
    const std::uint64_t half = make(1, 1, FBE_TEXTURE_RGBA16F, false);
    const std::vector<std::uint8_t> floats{ 0x00, 0x3C, 0x00, 0x40, 0x00, 0x38, 0x00, 0x00 };
    require(fbe_texture_write(half, 0, 0, 1, 1, floats.data(), 8));
    CHECK(fbe::texture::read(half, 0) == floats);
    stop_gpu();
}

TEST_CASE("a region outside the texture, or a pitch shorter than a row, is refused", "[gpu]") {
    start();
    const std::uint64_t texture = make(4, 4, FBE_TEXTURE_RGBA8, false);
    const std::vector<std::uint8_t> pixels(64, 255);
    CHECK(!fbe_texture_write(texture, 3, 0, 2, 1, pixels.data(), 8));
    CHECK(std::string(fbe_error()) == "a 2x1 region at 3, 0 isn't in a 4x4 texture");
    CHECK(!fbe_texture_write(texture, 0, -1, 1, 1, pixels.data(), 4));
    CHECK(!fbe_texture_write(texture, 0, 0, 0, 1, pixels.data(), 4));
    CHECK(!fbe_texture_write(texture, 0, 0, 2, 2, pixels.data(), 7));
    CHECK(!fbe_texture_write(texture, 0, 0, 1, 1, nullptr, 4));
    CHECK(fbe::texture::read(texture, 0) == std::vector<std::uint8_t>(64, 0));
    stop_gpu();
}

TEST_CASE("mipmaps average the level above, in linear light for sRGB", "[gpu]") {
    start();
    // Black and white pixels, two of each, average to half: 128 as bytes, and, in sRGB, the
    // byte whose light is half white's, 188.
    const std::vector<std::uint8_t> pixels{
        0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 255,
    };
    const std::uint64_t linear = make(2, 2, FBE_TEXTURE_RGBA8_LINEAR, true);
    require(fbe_texture_write(linear, 0, 0, 2, 2, pixels.data(), 8));
    const std::uint64_t srgb = make(2, 2, FBE_TEXTURE_RGBA8, true);
    require(fbe_texture_write(srgb, 0, 0, 2, 2, pixels.data(), 8));
    const std::vector<std::uint8_t> linear_level = fbe::texture::read(linear, 1);
    const std::vector<std::uint8_t> srgb_level = fbe::texture::read(srgb, 1);
    REQUIRE(linear_level.size() == 4);
    REQUIRE(srgb_level.size() == 4);
    CHECK(linear_level[0] >= 127);
    CHECK(linear_level[0] <= 128);
    CHECK(srgb_level[0] >= 187);
    CHECK(srgb_level[0] <= 189);
    CHECK(srgb_level[3] == 255);
    // A level that isn't there.
    CHECK(fbe::texture::read(linear, 2).empty());
    stop_gpu();
}

TEST_CASE("a handle that isn't a live texture's is refused", "[gpu]") {
    start();
    const std::uint64_t texture = make(2, 2, FBE_TEXTURE_RGBA8, false);
    // The same slot and generation as another kind's handle.
    const std::uint64_t other_kind =
        (texture & ~(std::uint64_t{ 0xFF } << 56U)) | (std::uint64_t{ 2 } << 56U);
    CHECK(fbe_texture_width(other_kind) == -1);
    CHECK(fbe_texture_width(0) == -1);
    CHECK(std::string(fbe_error()) == "no texture was given");
    // The white texture, slot 0, is the engine's.
    const std::uint64_t white = (std::uint64_t{ fbe::texture::kind } << 56U) | (1ULL << 32U);
    CHECK(fbe_texture_width(white) == -1);
    CHECK(!fbe_texture_release(white));
    CHECK(fbe_texture_width(texture) == 2);
    stop_gpu();
}

TEST_CASE("a retained texture lives until it's released as often", "[gpu]") {
    start();
    const std::uint64_t texture = make(2, 2, FBE_TEXTURE_RGBA8, false);
    REQUIRE(fbe_texture_retain(texture));
    REQUIRE(fbe_texture_release(texture));
    CHECK(fbe_texture_width(texture) == 2);
    REQUIRE(fbe_texture_release(texture));
    CHECK(fbe_texture_width(texture) == -1);
    CHECK(!fbe_texture_retain(texture));
    stop_gpu();
}

// A frame in flight may draw with a texture freed after it was recorded, so its slot, which is its
// index in the bindless set, waits for the GPU to finish the frames before it's used again.
TEST_CASE("a freed texture's slot waits for the frames that could draw it", "[gpu]") {
    start();
    const std::uint64_t first = make(2, 2, FBE_TEXTURE_RGBA8, false);
    const std::uint32_t slot = fbe::texture::index_of(first);
    REQUIRE(fbe_texture_release(first));
    const std::uint64_t second = make(2, 2, FBE_TEXTURE_RGBA8, false);
    CHECK(fbe::texture::index_of(second) != slot);
    // Once the GPU's done, the next Flip frees what the frame left, though nothing was recorded.
    fbe_frame_flip(false);
    vkDeviceWaitIdle(fbe::gpu::device()->device);
    fbe_frame_flip(false);
    const std::uint64_t third = make(2, 2, FBE_TEXTURE_RGBA8, false);
    CHECK(fbe::texture::index_of(third) == slot);
    // The old handle stays freed, though its slot's a new texture's.
    CHECK(fbe_texture_width(first) == -1);
    stop_gpu();
}

TEST_CASE("stopping the GPU frees every texture, and new ones are made after", "[gpu]") {
    start();
    const std::uint64_t texture = make(2, 2, FBE_TEXTURE_RGBA8, true);
    fbe_frame_flip(false);
    stop_gpu();
    CHECK(fbe_texture_count() == 0);
    CHECK(fbe_texture_width(texture) == -1);
    start();
    const std::uint64_t again = make(2, 2, FBE_TEXTURE_RGBA8, true);
    CHECK(again != texture);
    CHECK(fbe_texture_width(again) == 2);
    CHECK(fbe_texture_width(texture) == -1);
    stop_gpu();
}

TEST_CASE("a texture's filter and wraps pick its sampler", "[gpu]") {
    start();
    const std::uint64_t texture = make(2, 2, FBE_TEXTURE_RGBA8, false);
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
    require(fbe_texture_set_wrap(texture, FBE_TEXTURE_CLAMP, FBE_TEXTURE_MIRROR));
    CHECK(fbe_texture_filter(texture) == FBE_TEXTURE_NEAREST);
    CHECK(fbe_texture_wrap_u(texture) == FBE_TEXTURE_CLAMP);
    CHECK(fbe_texture_wrap_v(texture) == FBE_TEXTURE_MIRROR);
    CHECK(fbe::texture::sampler_of(*fbe::texture::find(texture)) ==
          fbe::gpu::sampler_index(FBE_TEXTURE_NEAREST, FBE_TEXTURE_CLAMP, FBE_TEXTURE_MIRROR));
    CHECK(!fbe_texture_set_filter(texture, 2));
    CHECK(std::string(fbe_error()) == "2 isn't a TextureFilter");
    CHECK(!fbe_texture_set_wrap(texture, FBE_TEXTURE_REPEAT, 3));
    CHECK(fbe_texture_wrap_v(texture) == FBE_TEXTURE_MIRROR);
    stop_gpu();
}

TEST_CASE("the LOD bias and anisotropy are every texture's, within their range", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    CHECK(fbe_gpu_max_anisotropy() == 0.0F);
    require(fbe_gpu_set_lod_bias(-0.4F));
    require(fbe_gpu_set_anisotropy(4.0F));
    start();
    CHECK(fbe_gpu_lod_bias() == -0.4F);
    CHECK(fbe_gpu_anisotropy() == 4.0F);
    CHECK(fbe_gpu_max_anisotropy() >= 1.0F);
    // Changing them while the GPU runs makes its samplers again.
    make(2, 2, FBE_TEXTURE_RGBA8, true);
    require(fbe_gpu_set_lod_bias(0.8F));
    require(fbe_gpu_set_anisotropy(16.0F));
    CHECK(!fbe_gpu_set_lod_bias(17.0F));
    CHECK(std::string(fbe_error()) == "a LOD bias is -16 to 16, not 17");
    CHECK(!fbe_gpu_set_anisotropy(0.5F));
    CHECK(fbe_gpu_lod_bias() == 0.8F);
    fbe_frame_flip(false);
    stop_gpu();
}

// A submit that fails for want of memory keeps the frame's uploads, and their staging, for the
// next submit, which runs them first: the pixels written before it arrive, and the textures are
// as their uses expect.
TEST_CASE("a failed submit's texture uploads run in the next", "[gpu]") {
    start();
    const std::uint64_t texture = make(2, 2, FBE_TEXTURE_RGBA8_LINEAR, false);
    const std::vector<std::uint8_t> pixels(16, 200);
    require(fbe_texture_write(texture, 0, 0, 2, 2, pixels.data(), 8));
    fbe::gpu::inject(fbe::gpu::fault_t::SUBMIT);
    CHECK(!fbe::gpu::finish_frame());
    CHECK(fbe::texture::read(texture, 0) == pixels);
    // At a Flip, with nothing recorded after, the carried uploads still run.
    const std::vector<std::uint8_t> others(16, 77);
    require(fbe_texture_write(texture, 0, 0, 2, 2, others.data(), 8));
    fbe::gpu::inject(fbe::gpu::fault_t::SUBMIT);
    fbe_frame_flip(false);
    fbe_frame_flip(false);
    CHECK(fbe::texture::read(texture, 0) == others);
    // Twice running, both are carried, in order.
    require(fbe_texture_write(texture, 0, 0, 2, 2, pixels.data(), 8));
    fbe::gpu::inject(fbe::gpu::fault_t::SUBMIT);
    CHECK(!fbe::gpu::finish_frame());
    require(fbe_texture_write(texture, 0, 0, 1, 1, others.data(), 4));
    fbe::gpu::inject(fbe::gpu::fault_t::SUBMIT);
    CHECK(!fbe::gpu::finish_frame());
    std::vector<std::uint8_t> both = pixels;
    std::copy_n(others.begin(), 4, both.begin());
    CHECK(fbe::texture::read(texture, 0) == both);
    stop_gpu();
}

// Samplers are all made before the old ones go, so one that can't be made leaves the set as it
// was, and the setting too.
TEST_CASE("samplers that can't be made leave the old ones and their settings", "[gpu]") {
    start();
    const std::vector<std::uint8_t> pixels = card();
    const std::uint64_t texture = make(8, 8, FBE_TEXTURE_RGBA8, true);
    require(fbe_texture_write(texture, 0, 0, 8, 8, pixels.data(), 32));
    const auto drawn = [texture] {
        return draw({ .width = 32, .height = 32 },
                    { quad_of(texture, { -1.0F, -1.0F, 1.0F, 1.0F }, { 0.0F, 0.0F, 1.0F, 1.0F }) });
    };
    const fbe::golden::frame_t before = drawn();
    fbe::gpu::inject(fbe::gpu::fault_t::SAMPLER);
    CHECK(!fbe_gpu_set_lod_bias(0.5F));
    CHECK(std::string(fbe_error()).starts_with("couldn't make the GPU's samplers"));
    CHECK(fbe_gpu_lod_bias() == 0.0F);
    fbe::gpu::inject(fbe::gpu::fault_t::SAMPLER);
    CHECK(!fbe_gpu_set_anisotropy(8.0F));
    CHECK(fbe_gpu_anisotropy() == 1.0F);
    CHECK(drawn().rgba == before.rgba);
    stop_gpu();
}

// The E2 milestone's textured quad: nearest and linear filtering of the test card, repeating and
// clamped, alpha blended, and stripes minified through their mipmaps.
TEST_CASE("textured quads match their golden image", "[gpu][golden]") {
    start();
    const std::vector<std::uint8_t> pixels = card();
    const std::uint64_t nearest = make(8, 8, FBE_TEXTURE_RGBA8, false);
    require(fbe_texture_write(nearest, 0, 0, 8, 8, pixels.data(), 32));
    require(fbe_texture_set_filter(nearest, FBE_TEXTURE_NEAREST));
    const std::uint64_t linear = make(8, 8, FBE_TEXTURE_RGBA8, false);
    require(fbe_texture_write(linear, 0, 0, 8, 8, pixels.data(), 32));
    const std::uint64_t clamped = make(8, 8, FBE_TEXTURE_RGBA8, false);
    require(fbe_texture_write(clamped, 0, 0, 8, 8, pixels.data(), 32));
    require(fbe_texture_set_wrap(clamped, FBE_TEXTURE_CLAMP, FBE_TEXTURE_CLAMP));
    const std::vector<std::uint8_t> lines = stripes();
    const std::uint64_t striped = make(64, 64, FBE_TEXTURE_RGBA8, true);
    require(fbe_texture_write(striped, 0, 0, 64, 64, lines.data(), 256));

    const fbe::golden::frame_t frame =
        draw({ .width = 128, .height = 128 },
             {
                 quad_of(nearest, { -1.0F, -1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F, 1.0F }),
                 quad_of(linear, { 0.0F, -1.0F, 1.0F, 0.0F }, { 0.0F, 0.0F, 2.0F, 2.0F }),
                 quad_of(clamped, { 0.0F, 0.0F, 1.0F, 1.0F }, { -0.5F, -0.5F, 1.5F, 1.5F }),
                 quad_of(striped, { -1.0F, 0.0F, -0.75F, 0.25F }, { 0.0F, 0.0F, 1.0F, 1.0F }),
             });
    // The nearest card's first pixel, magnified 8 times, is its own: red 0, blue 255, alpha 1.
    CHECK(at(frame, 3, 3, 0) == 0);
    CHECK(at(frame, 3, 3, 2) == 255);
    // The stripes, a quarter their size, are gray: half white's light.
    CHECK(at(frame, 8, 72, 0) >= 180);
    CHECK(at(frame, 8, 72, 0) <= 196);
    fbe::golden::check("textured_quads", frame);
    stop_gpu();
}

// Not a check, a measure: uploading a 1024x1024 texture and making its mipmaps, the CPU's part,
// and the GPU's to finish, shown on every run.
TEST_CASE("a 1024x1024 texture's upload, timed", "[gpu]") {
    start();
    const std::vector<std::uint8_t> pixels(std::size_t{ 1024 } * 1024 * 4, 128);
    const std::uint64_t texture = make(1024, 1024, FBE_TEXTURE_RGBA8, true);
    REQUIRE(fbe::gpu::finish_frame());
    const auto started = std::chrono::steady_clock::now();
    require(fbe_texture_write(texture, 0, 0, 1024, 1024, pixels.data(), 4096));
    const auto written = std::chrono::steady_clock::now();
    REQUIRE(fbe::gpu::finish_frame());
    const auto finished = std::chrono::steady_clock::now();
    const std::chrono::duration<double, std::milli> cpu = written - started;
    const std::chrono::duration<double, std::milli> gpu = finished - written;
    WARN("a 1024x1024 texture with mipmaps: " << cpu.count() << " ms to write, " << gpu.count()
                                              << " ms for the GPU, on "
                                              << fbe_gpu_device_name(fbe_gpu_device()));
    stop_gpu();
}
