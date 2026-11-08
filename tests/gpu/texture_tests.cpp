// Textures: made, written, read back, mipmapped, freed, and drawn through the bindless set, and
// the samplers' global LOD bias and anisotropy. Each test ends by stopping the GPU, which has to
// leave nothing behind.

#include "golden/golden.hpp"
#include "gpu_testing.hpp"

#include <algorithm>
#include <array>
#include <assets/loads.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <fbengine.h>
#include <gpu/graph.hpp>
#include <gpu/image.hpp>
#include <gpu/pipeline.hpp>
#include <gpu/vulkan.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <test_shaders.hpp>
#include <texture/texture.hpp>
#include <utility>
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

/// Draws `quads` over a dark grey clear into an sRGB image `size` big, the frame's own, in a pass,
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
    const fbe::gpu::resource_t into = fbe::gpu::external_resource({
        .image = image.image,
        .view = image.view,
        .extent = size,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .stages = flag<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_COPY_BIT),
        .access = flag<VkAccessFlags2>(VK_ACCESS_2_TRANSFER_READ_BIT),
    });
    require(fbe::gpu::add_pass({
        .name = "quads",
        .colors = { { .target = into, .clear = true, .rgba = { 0.01F, 0.01F, 0.01F, 1.0F } } },
        .record =
            [pipeline,
             drawn = std::make_shared<const std::vector<quad_t>>(quads)](VkCommandBuffer commands) {
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                VkDescriptorSet set = fbe::gpu::bindless_set();
                vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        fbe::gpu::pipeline_layout(), 0, 1, &set, 0, nullptr);
                for (const quad_t& quad : *drawn) {
                    vkCmdPushConstants(commands, fbe::gpu::pipeline_layout(),
                                       flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL), 0,
                                       sizeof(quad_t), &quad);
                    vkCmdDraw(commands, 6, 1, 0, 0);
                }
            },
    }));
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

TEST_CASE("a level written is read back as written, and the others are left as they were",
          "[gpu]") {
    start();
    const std::uint64_t texture = make(4, 4, FBE_TEXTURE_RGBA8_LINEAR, true);
    REQUIRE(fbe_texture_levels(texture) == 3);
    require(fbe_texture_write(texture, 0, 0, 4, 4, std::vector<std::uint8_t>(64, 255).data(), 16));
    const std::vector<std::uint8_t> dim{
        10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160,
    };
    require(fbe_texture_write_level(texture, 1, 2, 2, dim.data(), 8));
    std::vector<std::uint8_t> read(16);
    REQUIRE(fbe_texture_read(texture, 1, read.data(), 16));
    CHECK(read == dim);
    CHECK(fbe::texture::read(texture, 0) == std::vector<std::uint8_t>(64, 255));
    CHECK(fbe::texture::read(texture, 2) == std::vector<std::uint8_t>(4, 255));
    // The last, a pixel, with its rows further apart than they need be.
    const std::vector<std::uint8_t> last{ 1, 2, 3, 4, 0, 0, 0, 0 };
    require(fbe_texture_write_level(texture, 2, 1, 1, last.data(), 8));
    CHECK(fbe::texture::read(texture, 2) == std::vector<std::uint8_t>{ 1, 2, 3, 4 });
    CHECK(fbe::texture::read(texture, 1) == dim);
    // The first, written whole, makes the others again from it.
    require(fbe_texture_write(texture, 0, 0, 4, 4, std::vector<std::uint8_t>(64, 255).data(), 16));
    CHECK(fbe::texture::read(texture, 1) == std::vector<std::uint8_t>(16, 255));
    CHECK(fbe::texture::read(texture, 2) == std::vector<std::uint8_t>(4, 255));
    // And the first by itself is only itself.
    require(fbe_texture_write_level(texture, 0, 4, 4, std::vector<std::uint8_t>(64, 0).data(), 16));
    CHECK(fbe::texture::read(texture, 0) == std::vector<std::uint8_t>(64, 0));
    CHECK(fbe::texture::read(texture, 1) == std::vector<std::uint8_t>(16, 255));
    stop_gpu();
}

TEST_CASE("a level that isn't there, of another size, or with a short pitch, is refused", "[gpu]") {
    start();
    const std::uint64_t texture = make(4, 2, FBE_TEXTURE_RGBA8, true);
    REQUIRE(fbe_texture_levels(texture) == 3);
    const std::vector<std::uint8_t> pixels(64, 255);
    CHECK(!fbe_texture_write_level(texture, 3, 1, 1, pixels.data(), 4));
    CHECK(std::string(fbe_error()) == "the texture has 3 levels, and no level 3");
    CHECK(!fbe_texture_write_level(texture, -1, 4, 2, pixels.data(), 16));
    CHECK(!fbe_texture_write_level(texture, 1, 2, 2, pixels.data(), 8));
    CHECK(std::string(fbe_error()) == "level 1 of a 4x2 texture is 2x1, not 2x2");
    // Its last level is a pixel, not none, along the side that's run out.
    CHECK(!fbe_texture_write_level(texture, 2, 1, 0, pixels.data(), 4));
    CHECK(!fbe_texture_write_level(texture, 1, 2, 1, pixels.data(), 7));
    CHECK(!fbe_texture_write_level(texture, 1, 2, 1, nullptr, 8));
    CHECK(std::string(fbe_error()) == "there are no pixels to write into level 1");
    CHECK(!fbe_texture_write_level(0, 0, 4, 2, pixels.data(), 16));
    CHECK(fbe::texture::read(texture, 0) == std::vector<std::uint8_t>(32, 0));
    CHECK(fbe::texture::read(texture, 1) == std::vector<std::uint8_t>(8, 0));
    // A multisampled target is drawn into, its one level too.
    const std::uint64_t multisampled = fbe_texture_create_target(4, 2, FBE_TEXTURE_RGBA8, 4, false);
    REQUIRE(multisampled != 0);
    CHECK(!fbe_texture_write_level(multisampled, 0, 4, 2, pixels.data(), 16));
    CHECK(std::string(fbe_error()) == "a multisampled target is drawn into, not written");
    CHECK(fbe_texture_release(multisampled));
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

// A GPU whose samplers have no LOD bias, as Metal's through MoltenVK haven't, makes them with none,
// which the validation layers would find, though the setting's kept. What's read straight through a
// sampler draws as with none; the scene's layers take it through their shader, as scene_tests
// shows.
TEST_CASE("a GPU with no LOD bias draws with none", "[gpu]") {
    const std::vector<std::uint8_t> pixels = card();
    const auto drawn = [&pixels] {
        const std::uint64_t texture = make(8, 8, FBE_TEXTURE_RGBA8, true);
        require(fbe_texture_write(texture, 0, 0, 8, 8, pixels.data(), 32));
        return draw({ .width = 8, .height = 8 },
                    { quad_of(texture, { -1.0F, -1.0F, 1.0F, 1.0F }, { 0.0F, 0.0F, 1.0F, 1.0F }) });
    };
    start();
    const fbe::golden::frame_t before = drawn();
    require(fbe_gpu_set_lod_bias(8.0F));
    if (fbe::gpu::device()->max_lod_bias > 0.0F) {
        CHECK(drawn().rgba != before.rgba);
    }
    stop_gpu();
    fbe::gpu::inject(fbe::gpu::fault_t::NO_LOD_BIAS);
    start();
    CHECK(fbe::gpu::device()->max_lod_bias == 0.0F);
    CHECK(fbe_gpu_lod_bias() == 8.0F);
    CHECK(drawn().rgba == before.rgba);
    require(fbe_gpu_set_lod_bias(-3.0F));
    CHECK(drawn().rgba == before.rgba);
    require(fbe_gpu_set_lod_bias(0.0F));
    stop_gpu();
}

// Textures are as many as the device's set holds: past what it promises a set, as much as it says
// it holds, and otherwise what it promises, as MoltenVK's on a Mac is, as long as that's what
// fbengine needs.
TEST_CASE("the textures are as many as the device's set holds", "[gpu]") {
    fbe::gpu::inject(fbe::gpu::fault_t::PROMISED_DESCRIPTORS);
    start();
    CHECK(fbe::gpu::texture_capacity() == 1212 - fbe::gpu::sampler_slots - fbe::gpu::samples_slots);
    CHECK(fbe::gpu::pool_images() == fbe::gpu::texture_capacity() + fbe::gpu::samples_slots);
    const std::vector<std::uint8_t> pixels = card();
    const std::uint64_t texture = make(8, 8, FBE_TEXTURE_RGBA8_LINEAR, false);
    require(fbe_texture_write(texture, 0, 0, 8, 8, pixels.data(), 32));
    CHECK(fbe::texture::read(texture, 0) == pixels);
    stop_gpu();
    // A device that isn't asked to pretend holds at least what fbengine needs.
    start();
    CHECK(fbe::gpu::texture_capacity() >= 1024);
    CHECK(fbe::gpu::pool_images() == fbe::gpu::texture_capacity() + fbe::gpu::samples_slots);
    stop_gpu();
}

TEST_CASE("a device whose set holds too few textures is refused", "[gpu]") {
    REQUIRE(fbe_window_set_headless(true));
    fbe::gpu::inject(fbe::gpu::fault_t::FEWEST_DESCRIPTORS);
    CHECK(!fbe_gpu_start());
    const std::string why = fbe_error();
    INFO(why);
    CHECK(why.ends_with("holds 1023 textures at once, and fbengine needs 1024"));
    CHECK(!fbe_gpu_started());
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

namespace {

namespace assets = fbe::assets;

/// An image load done and loaded, of `image`, with its texels `bytes`, which outlive it.
std::uint64_t image_load(assets::image_t image, const std::vector<std::uint8_t>& bytes) {
    image.texels = assets::texels_t(nullptr, std::span(bytes));
    auto load = std::make_shared<assets::load_t>();
    load->kind = assets::load_kind_t::IMAGE;
    load->image = std::move(image);
    load->done = true;
    return assets::loads().add(std::move(load));
}

/// A 4x4 image of `format`, of `bytes` bytes.
assets::image_t block_image(std::uint32_t format, std::size_t bytes) {
    return {
        .format = format,
        .width = 4,
        .height = 4,
        .levels = { { .width = 4, .height = 4, .size = bytes } },
    };
}

/// Whether the started device samples a Vulkan format.
bool samples_format(VkFormat format) {
    VkFormatProperties support{};
    vkGetPhysicalDeviceFormatProperties(fbe::gpu::device()->physical, format, &support);
    return (support.optimalTilingFeatures &
            flag<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) != 0;
}

} // namespace

TEST_CASE("a file's BC7 is sampled, and nothing else", "[gpu]") {
    start();
    if (!samples_format(VK_FORMAT_BC7_SRGB_BLOCK)) {
        stop_gpu();
        SKIP("this GPU doesn't sample BC7");
    }
    const std::vector<std::uint8_t> bytes(16, 0x40);
    const std::uint64_t load = image_load(block_image(assets::formats::bc7_srgb, 16), bytes);
    const std::uint64_t texture = fbe_texture_from_load(load, true, true);
    INFO(fbe_error());
    REQUIRE(texture != 0);
    // Its blocks can't be blit into mipmaps.
    CHECK(fbe_texture_levels(texture) == 1);
    CHECK(fbe_texture_format(texture) == FBE_TEXTURE_FROM_FILE);
    CHECK(fbe_texture_vulkan_format(texture) == assets::formats::bc7_srgb);
    const std::array<std::uint8_t, 4> pixel{};
    CHECK(!fbe_texture_write(texture, 0, 0, 1, 1, pixel.data(), 4));
    CHECK(!fbe_texture_clear(texture, 0, 0, 0, 0));
    CHECK(fbe_texture_pixels(texture, nullptr) == nullptr);
    std::array<std::uint8_t, 16> read{};
    CHECK(!fbe_texture_read(texture, 0, read.data(), 16));
    const std::array<std::uint8_t, 64> level{};
    CHECK(!fbe_texture_write_level(texture, 0, 4, 4, level.data(), 16));
    CHECK(std::string(fbe_error()).starts_with("a texture of a file's own format is only sampled"));
    const std::uint64_t other = make(4, 4, FBE_TEXTURE_RGBA8, false);
    CHECK(!fbe_texture_copy(other, 0, 0, texture, 0, 0, 4, 4));
    CHECK(!fbe_texture_copy(texture, 0, 0, other, 0, 0, 4, 4));
    // A frame with it, for the validation layers to see.
    REQUIRE(fbe::gpu::finish_frame());
    CHECK(fbe_texture_release(texture));
    CHECK(fbe_texture_release(other));
    CHECK(fbe_assets_free(load));
    stop_gpu();
}

TEST_CASE("a file's sRGB BC7 is what's stored to UNLIT, as its undecoded twin is", "[gpu]") {
    start();
    if (!samples_format(VK_FORMAT_BC7_SRGB_BLOCK) || !samples_format(VK_FORMAT_BC7_UNORM_BLOCK)) {
        stop_gpu();
        SKIP("this GPU doesn't sample BC7");
    }
    const std::vector<std::uint8_t> bytes(16, 0x40);
    const std::uint64_t load = image_load(block_image(assets::formats::bc7_srgb, 16), bytes);
    const std::uint64_t decoded = fbe_texture_from_load(load, true, false);
    const std::uint64_t undecoded = fbe_texture_from_load(load, false, false);
    REQUIRE(decoded != 0);
    REQUIRE(undecoded != 0);
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t target = fbe_texture_create_target(4, 4, FBE_TEXTURE_RGBA8, 1, true);
    const std::uint64_t camera = fbe_entity_create(world, 0, FBE_ENTITY_CAMERA);
    REQUIRE(fbe_camera_set_target(camera, target));
    const std::uint64_t material = fbe_material_create();
    REQUIRE(fbe_material_set_shading(material, FBE_SHADING_UNLIT));
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    const std::uint64_t geometry = fbe_geometry_cube();
    REQUIRE(fbe_mesh_set_geometry(mesh, geometry));
    REQUIRE(fbe_mesh_set_material(mesh, material));
    REQUIRE(fbe_entity_set_position(mesh, 0.0F, 0.0F, -2.0F, FBE_SPACE_WORLD));
    const auto drawn_with = [&](std::uint64_t texture) {
        REQUIRE(fbe_material_set_texture(material, 0, texture));
        REQUIRE(fbe_render_world(world, 1.0F));
        std::array<std::uint8_t, 64> read{};
        REQUIRE(fbe_texture_read(target, 0, read.data(), 64));
        fbe_frame_flip(false);
        return std::array<std::uint8_t, 3>{ read[40], read[41], read[42] };
    };
    const std::array<std::uint8_t, 3> stored = drawn_with(undecoded);
    CHECK(stored[0] + stored[1] + stored[2] > 0);
    const std::array<std::uint8_t, 3> through = drawn_with(decoded);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        CAPTURE(channel, int{ stored[channel] }, int{ through[channel] });
        CHECK(std::abs(stored[channel] - through[channel]) <= 1);
    }
    fbe_world_free(world);
    std::array<std::uint64_t, 64> some{};
    while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    fbe_geometry_release(geometry);
    fbe_material_release(material);
    fbe_texture_release(target);
    CHECK(fbe_texture_release(decoded));
    CHECK(fbe_texture_release(undecoded));
    CHECK(fbe_assets_free(load));
    stop_gpu();
}

TEST_CASE("a file's own format is a map that holds no color unless it's sRGB", "[gpu]") {
    start();
    if (!samples_format(VK_FORMAT_BC7_SRGB_BLOCK) || !samples_format(VK_FORMAT_BC7_UNORM_BLOCK)) {
        stop_gpu();
        SKIP("this GPU doesn't sample BC7");
    }
    const std::vector<std::uint8_t> bytes(16, 0x40);
    const std::uint64_t color_load = image_load(block_image(assets::formats::bc7_srgb, 16), bytes);
    const std::uint64_t plain_load = image_load(block_image(assets::formats::bc7, 16), bytes);
    const std::uint64_t color = fbe_texture_from_load(color_load, true, true);
    const std::uint64_t plain = fbe_texture_from_load(plain_load, true, true);
    INFO(fbe_error());
    REQUIRE(color != 0);
    REQUIRE(plain != 0);
    CHECK(fbe_texture_srgb(color));
    CHECK(!fbe_texture_srgb(plain));
    CHECK(!fbe_texture_srgb(0));
    // Whatever the format, as far as this GPU samples them.
    struct format_t {
        VkFormat format;
        std::size_t bytes;
        bool srgb;
    };
    for (const format_t& each : {
             format_t{ .format = VK_FORMAT_R8G8B8A8_SRGB, .bytes = 64, .srgb = true },
             format_t{ .format = VK_FORMAT_R8G8B8A8_UNORM, .bytes = 64, .srgb = false },
             format_t{ .format = VK_FORMAT_BC1_RGB_SRGB_BLOCK, .bytes = 8, .srgb = true },
             format_t{ .format = VK_FORMAT_BC1_RGB_UNORM_BLOCK, .bytes = 8, .srgb = false },
             format_t{ .format = VK_FORMAT_BC1_RGBA_SRGB_BLOCK, .bytes = 8, .srgb = true },
             format_t{ .format = VK_FORMAT_BC2_SRGB_BLOCK, .bytes = 16, .srgb = true },
             format_t{ .format = VK_FORMAT_BC3_SRGB_BLOCK, .bytes = 16, .srgb = true },
             format_t{ .format = VK_FORMAT_BC5_UNORM_BLOCK, .bytes = 16, .srgb = false },
             format_t{ .format = VK_FORMAT_ASTC_4x4_SRGB_BLOCK, .bytes = 16, .srgb = true },
         }) {
        if (!samples_format(each.format)) {
            continue;
        }
        const std::vector<std::uint8_t> texels(each.bytes, 0x40);
        const std::uint64_t load =
            image_load(block_image(static_cast<std::uint32_t>(each.format), each.bytes), texels);
        const std::uint64_t texture = fbe_texture_from_load(load, true, false);
        INFO("the Vulkan format " << static_cast<int>(each.format) << ": " << fbe_error());
        REQUIRE(texture != 0);
        CHECK(fbe_texture_srgb(texture) == each.srgb);
        CHECK(fbe_texture_release(texture));
        CHECK(fbe_assets_free(load));
    }
    const std::uint64_t material = fbe_material_create();
    for (const std::int32_t map : { FBE_MAP_METAL_ROUGH, FBE_MAP_NORMAL, FBE_MAP_OCCLUSION }) {
        CHECK(!fbe_material_set_map(material, map, color, 0));
        CHECK(fbe_material_map(material, map) == 0);
        CHECK(fbe_material_set_map(material, map, plain, 0));
        CHECK(fbe_material_map(material, map) == plain);
    }
    CHECK(fbe_material_set_map(material, FBE_MAP_EMISSIVE, color, 0));
    CHECK(fbe_material_release(material));
    CHECK(fbe_texture_release(color));
    CHECK(fbe_texture_release(plain));
    CHECK(fbe_assets_free(color_load));
    CHECK(fbe_assets_free(plain_load));
    stop_gpu();
}

TEST_CASE("a model's own formats read as their maps say, or say which can't", "[gpu]") {
    start();
    if (!samples_format(VK_FORMAT_BC7_SRGB_BLOCK) || !samples_format(VK_FORMAT_BC7_UNORM_BLOCK)) {
        stop_gpu();
        SKIP("this GPU doesn't sample BC7");
    }
    const std::vector<std::uint8_t> bytes(16, 0x40);
    /// A model of one image of `format`, and a material of it.
    const auto load_of = [&](std::uint32_t format, const assets::material_t& material) {
        assets::image_t image = block_image(format, 16);
        image.texels = assets::texels_t(nullptr, std::span(bytes));
        auto load = std::make_shared<assets::load_t>();
        load->kind = assets::load_kind_t::MODEL;
        load->model.images.push_back(std::move(image));
        load->model.materials.push_back(material);
        load->done = true;
        return assets::loads().add(std::move(load));
    };
    std::array<std::uint64_t, FBE_MODEL_MAPS> others{};
    std::array<std::int32_t, FBE_MODEL_MAPS> images{};
    const auto read = [&](std::uint64_t load, std::uint64_t material, std::uint64_t texture,
                          std::int32_t& made) {
        made = 0;
        return fbe_model_material_textures(load, 0, material, &texture, 1, others.data(),
                                           images.data(), FBE_MODEL_MAPS, &made);
    };
    // A color and a map of one sRGB image: the map reads its twin, of the same bytes.
    assets::material_t both;
    both.base_color_texture.image = 0;
    both.metallic_roughness_texture.image = 0;
    std::uint64_t load = load_of(assets::formats::bc7_srgb, both);
    std::uint64_t material = fbe_model_material(load, 0);
    std::uint64_t texture = fbe_model_texture(load, 0, true);
    REQUIRE(texture != 0);
    std::int32_t made = 0;
    INFO(fbe_error());
    CHECK(read(load, material, texture, made));
    REQUIRE(made == 1);
    CHECK(images[0] == 0);
    CHECK(fbe_material_texture(material, 0) == texture);
    CHECK(fbe_material_map(material, FBE_MAP_METAL_ROUGH) == others[0]);
    CHECK(fbe_texture_vulkan_format(texture) == assets::formats::bc7_srgb);
    CHECK(fbe_texture_vulkan_format(others[0]) == assets::formats::bc7);
    CHECK(!fbe_texture_srgb(others[0]));
    fbe_material_release(material);
    fbe_texture_release(others[0]);
    fbe_texture_release(texture);
    CHECK(fbe_assets_free(load));
    // A map alone of an sRGB image is its file's mistake, and no copy hides it.
    assets::material_t map;
    map.normal_texture.image = 0;
    load = load_of(assets::formats::bc7_srgb, map);
    material = fbe_model_material(load, 0);
    texture = fbe_model_texture(load, 0, true);
    CHECK(!read(load, material, texture, made));
    CHECK(fbe_model_invalid());
    CHECK(made == 0);
    CHECK(std::string_view(fbe_error()).contains("normal"));
    fbe_material_release(material);
    fbe_texture_release(texture);
    CHECK(fbe_assets_free(load));
    // A color of a UNORM image can't be decoded, so it says which it is.
    assets::material_t color;
    color.base_color_texture.image = 0;
    load = load_of(assets::formats::bc7, color);
    material = fbe_model_material(load, 0);
    texture = fbe_model_texture(load, 0, true);
    CHECK(!read(load, material, texture, made));
    CHECK(fbe_model_invalid());
    CHECK(std::string_view(fbe_error()).contains("base color"));
    fbe_material_release(material);
    fbe_texture_release(texture);
    CHECK(fbe_assets_free(load));
    stop_gpu();
}

TEST_CASE("glTF's default material is only its surfaces', and goes with them", "[gpu]") {
    start();
    assets::primitive_t primitive;
    primitive.positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
    primitive.indices = { 0, 1, 2 };
    assets::mesh_t mesh;
    mesh.primitives = { primitive, primitive };
    auto made = std::make_shared<assets::load_t>();
    made->kind = assets::load_kind_t::MODEL;
    made->model.meshes.push_back(std::move(mesh));
    made->done = true;
    const std::uint64_t load = assets::loads().add(std::move(made));
    const std::uint64_t geometry = fbe_model_geometry(load, 0, nullptr, 0);
    INFO(fbe_error());
    REQUIRE(geometry != 0);
    const std::uint64_t plain = fbe_surface_material(geometry, 0);
    REQUIRE(plain != 0);
    // One for a geometry's primitives, however many.
    CHECK(fbe_surface_material(geometry, 1) == plain);
    CHECK(fbe_material_metallic(plain) == 1.0F);
    CHECK(fbe_material_roughness(plain) == 1.0F);
    CHECK(fbe_geometry_release(geometry));
    // Nothing's left holding it.
    CHECK(!fbe_material_set_metal_rough(plain, 0.5F, 0.5F));
    CHECK(fbe_assets_free(load));
    stop_gpu();
}

TEST_CASE("a file's levels are its texture's, and one level is made a chain", "[gpu]") {
    start();
    const std::vector<std::uint8_t> bytes(20, 0xFF);
    assets::image_t two = block_image(assets::formats::rgba8, 16);
    two.width = 2;
    two.height = 2;
    two.levels = {
        { .width = 2, .height = 2, .size = 16 },
        { .width = 1, .height = 1, .offset = 16, .size = 4 },
    };
    const std::uint64_t levels = image_load(std::move(two), bytes);
    const std::uint64_t kept = fbe_texture_from_load(levels, true, true);
    INFO(fbe_error());
    REQUIRE(kept != 0);
    CHECK(fbe_texture_levels(kept) == 2);
    CHECK(fbe_texture_format(kept) == FBE_TEXTURE_RGBA8_LINEAR);
    std::array<std::uint8_t, 4> last{};
    REQUIRE(fbe_texture_read(kept, 1, last.data(), 4));
    CHECK(last[0] == 0xFF);

    const std::vector<std::uint8_t> flat(64, 0xFF);
    assets::image_t one = block_image(assets::formats::rgba8_srgb, 64);
    const std::uint64_t single = image_load(std::move(one), flat);
    const std::uint64_t made = fbe_texture_from_load(single, true, true);
    REQUIRE(made != 0);
    CHECK(fbe_texture_levels(made) == 3);
    CHECK(fbe_texture_format(made) == FBE_TEXTURE_RGBA8);
    const std::uint64_t data = fbe_texture_from_load(single, false, false);
    REQUIRE(data != 0);
    CHECK(fbe_texture_levels(data) == 1);
    CHECK(fbe_texture_format(data) == FBE_TEXTURE_RGBA8_LINEAR);
    for (const std::uint64_t texture : { kept, made, data }) {
        CHECK(fbe_texture_release(texture));
    }
    CHECK(fbe_assets_free(levels));
    CHECK(fbe_assets_free(single));
    stop_gpu();
}

TEST_CASE("what a file says that a texture can't be is refused, as its file's", "[gpu]") {
    start();
    const std::vector<std::uint8_t> bytes(64, 0);
    const auto refused = [&bytes](assets::image_t image) {
        const std::uint64_t load = image_load(std::move(image), bytes);
        CHECK(fbe_texture_from_load(load, true, true) == 0);
        CHECK(fbe_texture_load_invalid());
        CHECK(fbe_assets_free(load));
    };
    assets::image_t cube = block_image(assets::formats::rgba8, 64);
    cube.faces = 6;
    refused(std::move(cube));
    assets::image_t layers = block_image(assets::formats::rgba8, 64);
    layers.layers = 2;
    refused(std::move(layers));
    assets::image_t deep = block_image(assets::formats::rgba8, 64);
    deep.depth = 2;
    refused(std::move(deep));
    // A format it doesn't know the size of, and a depth format.
    refused(block_image(VK_FORMAT_R5G6B5_UNORM_PACK16, 32));
    refused(block_image(VK_FORMAT_D32_SFLOAT, 64));
    // Levels whose sizes aren't what their format's are, or past the texels, or too many.
    refused(block_image(assets::formats::rgba8, 63));
    assets::image_t past = block_image(assets::formats::rgba8, 64);
    past.levels[0].offset = 8;
    refused(std::move(past));
    assets::image_t none = block_image(assets::formats::rgba8, 64);
    none.levels.clear();
    refused(std::move(none));
    assets::image_t many = block_image(assets::formats::rgba8, 64);
    many.levels.resize(4, many.levels[0]);
    refused(std::move(many));
    stop_gpu();
}

TEST_CASE("a load that isn't an image's makes no texture", "[gpu]") {
    start();
    CHECK(fbe_texture_from_load(0, true, true) == 0);
    CHECK(!fbe_texture_load_invalid());
    auto bytes_load = std::make_shared<assets::load_t>();
    bytes_load->done = true;
    const std::uint64_t load = assets::loads().add(std::move(bytes_load));
    CHECK(fbe_texture_from_load(load, true, true) == 0);
    CHECK(fbe_texture_from_model(load, 0, true, true) == 0);
    CHECK(fbe_assets_free(load));
    stop_gpu();
}

TEST_CASE("what a GPU can't do with a file's image says so, not that the file's wrong", "[gpu]") {
    start();
    const std::vector<std::uint8_t> bytes(64, 0);
    const std::uint64_t load = image_load(block_image(assets::formats::rgba8, 64), bytes);
    const std::int32_t count = fbe_texture_count();
    fbe::gpu::inject(fbe::gpu::fault_t::NO_FILE_FORMAT);
    CHECK(fbe_texture_from_load(load, true, true) == 0);
    CHECK(!fbe_texture_load_invalid());
    CHECK(std::string(fbe_error()).contains("doesn't sample"));
    fbe::gpu::inject(fbe::gpu::fault_t::UPLOAD);
    CHECK(fbe_texture_from_load(load, true, true) == 0);
    CHECK(!fbe_texture_load_invalid());
    CHECK(fbe_texture_count() == count);
    CHECK(fbe_assets_free(load));
    stop_gpu();
}

TEST_CASE("a reload takes a file's image in place, or leaves the texture as it was", "[gpu]") {
    start();
    const std::vector<std::uint8_t> red{ 255, 0, 0, 255 };
    const std::vector<std::uint8_t> blue(64, 0);
    assets::image_t one = block_image(assets::formats::rgba8, 4);
    one.width = 1;
    one.height = 1;
    one.levels = { { .width = 1, .height = 1, .size = 4 } };
    const std::uint64_t first = image_load(std::move(one), red);
    const std::uint64_t texture = fbe_texture_from_load(first, true, false);
    REQUIRE(texture != 0);
    REQUIRE(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
    REQUIRE(fbe_texture_retain(texture));
    const std::int32_t count = fbe_texture_count();
    const std::uint32_t index = fbe::texture::index_of(texture);

    const std::uint64_t second = image_load(block_image(assets::formats::rgba8, 64), blue);
    REQUIRE(fbe_texture_reload(texture, second, true, true));
    // The handle, its slot in the bindless set, its sampler, and its holds are as they were.
    CHECK(fbe_texture_width(texture) == 4);
    CHECK(fbe_texture_levels(texture) == 3);
    CHECK(fbe_texture_filter(texture) == FBE_TEXTURE_NEAREST);
    CHECK(fbe_texture_count() == count);
    CHECK(fbe::texture::index_of(texture) == index);
    std::array<std::uint8_t, 64> read{};
    REQUIRE(fbe_texture_read(texture, 0, read.data(), 64));
    CHECK(read[0] == 0);

    assets::image_t cube = block_image(assets::formats::rgba8, 64);
    cube.faces = 6;
    const std::uint64_t bad = image_load(std::move(cube), blue);
    CHECK(!fbe_texture_reload(texture, bad, true, true));
    CHECK(fbe_texture_load_invalid());
    fbe::gpu::inject(fbe::gpu::fault_t::UPLOAD);
    CHECK(!fbe_texture_reload(texture, second, true, true));
    CHECK(fbe_texture_width(texture) == 4);
    REQUIRE(fbe_texture_read(texture, 0, read.data(), 64));
    CHECK(!fbe_texture_reload(0, second, true, true));
    CHECK(fbe_texture_release(texture));
    CHECK(fbe_texture_release(texture));
    CHECK(fbe_texture_count() == count - 1);
    for (const std::uint64_t load : { first, second, bad }) {
        CHECK(fbe_assets_free(load));
    }
    stop_gpu();
}

TEST_CASE("files' images are transcoded to what the started GPU samples", "[gpu]") {
    fbe_gpu_stop();
    CHECK(fbe::texture::device_transcode() == assets::transcode_t::RGBA8);
    start();
    const assets::transcode_t best = fbe::texture::device_transcode();
    if (samples_format(VK_FORMAT_BC7_SRGB_BLOCK) && samples_format(VK_FORMAT_BC7_UNORM_BLOCK)) {
        CHECK(best == assets::transcode_t::BC7);
    } else if (samples_format(VK_FORMAT_ASTC_4x4_SRGB_BLOCK)) {
        CHECK(best == assets::transcode_t::ASTC_4X4);
    } else {
        CHECK(best == assets::transcode_t::RGBA8);
    }
    // What a load's asked for is what it's transcoded to, the GPU's for now.
    const auto asked = [] {
        const std::uint64_t load =
            fbe_assets_load(FBE_ASSETS_IMAGE, "res://none.ktx2", false, false);
        REQUIRE(load != 0);
        const assets::transcode_t transcode = assets::loads().get(load)->options.transcode;
        CHECK(fbe_assets_free(load));
        return transcode;
    };
    CHECK(asked() == best);
    fbe::gpu::inject(fbe::gpu::fault_t::NO_COMPRESSED);
    CHECK(asked() == assets::transcode_t::RGBA8);
    stop_gpu();
}

TEST_CASE("a file's 8-bit image's alpha is seen, and a reload's is the new one's", "[gpu]") {
    start();
    std::vector<std::uint8_t> clear(64, 0xFF);
    clear[3] = 0x80;
    const std::vector<std::uint8_t> whole(64, 0xFF);
    const std::uint64_t seen = image_load(block_image(assets::formats::rgba8, 64), clear);
    const std::uint64_t solid = image_load(block_image(assets::formats::rgba8, 64), whole);
    const std::uint64_t texture = fbe_texture_from_load(seen, true, false);
    REQUIRE(texture != 0);
    CHECK(fbe_texture_alpha(texture));
    REQUIRE(fbe_texture_reload(texture, solid, true, false));
    CHECK(!fbe_texture_alpha(texture));
    CHECK(fbe_texture_release(texture));
    CHECK(fbe_assets_free(seen));
    CHECK(fbe_assets_free(solid));
    stop_gpu();
}

TEST_CASE("a target isn't reloaded", "[gpu]") {
    start();
    const std::vector<std::uint8_t> bytes(64, 0xFF);
    const std::uint64_t load = image_load(block_image(assets::formats::rgba8, 64), bytes);
    const std::uint64_t target = fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA8, 1, false);
    REQUIRE(target != 0);
    CHECK(!fbe_texture_reload(target, load, true, true));
    CHECK(fbe_texture_is_target(target));
    CHECK(fbe_texture_width(target) == 8);
    CHECK(fbe_texture_release(target));
    CHECK(fbe_assets_free(load));
    stop_gpu();
}

TEST_CASE("a reload needs no free slot, with the bindless set full", "[gpu]") {
    start();
    const std::vector<std::uint8_t> bytes(64, 0xFF);
    const std::uint64_t load = image_load(block_image(assets::formats::rgba8, 64), bytes);
    std::vector<std::uint64_t> held;
    while (true) {
        const std::uint64_t texture = fbe_texture_create(1, 1, FBE_TEXTURE_RGBA8, false);
        if (texture == 0) {
            break;
        }
        held.push_back(texture);
    }
    REQUIRE(!held.empty());
    REQUIRE(fbe_texture_reload(held.back(), load, true, true));
    CHECK(fbe_texture_width(held.back()) == 4);
    // And it leaves none taken, nor adds one.
    CHECK(fbe_texture_create(1, 1, FBE_TEXTURE_RGBA8, false) == 0);
    REQUIRE(fbe_texture_release(held.back()));
    REQUIRE(fbe::gpu::finish_frame());
    for (std::size_t at = 0; at + 1 < held.size(); ++at) {
        CHECK(fbe_texture_release(held[at]));
    }
    CHECK(fbe_assets_free(load));
    stop_gpu();
}

TEST_CASE("a file's own format says how to keep its pixels", "[gpu]") {
    start();
    if (!samples_format(VK_FORMAT_BC7_SRGB_BLOCK)) {
        stop_gpu();
        SKIP("this GPU doesn't sample BC7");
    }
    const std::vector<std::uint8_t> bytes(16, 0x40);
    const std::uint64_t load = image_load(block_image(assets::formats::bc7_srgb, 16), bytes);
    const std::uint64_t texture = fbe_texture_from_load(load, true, true);
    REQUIRE(texture != 0);
    std::array<std::uint8_t, 16> read{};
    CHECK(!fbe_texture_read(texture, 0, read.data(), 16));
    CHECK(std::string(fbe_error()).contains("compression = \"none\""));
    CHECK(fbe_texture_release(texture));
    CHECK(fbe_assets_free(load));
    stop_gpu();
}

// What Model's reload does to what reads its textures: each material's layers and maps of one take
// the other, a layer only of the same format and a map only what it can read, held by what reads it
// as any it's given is; and a texture of none, or what isn't one, is refused, changing nothing.
TEST_CASE("textures replaced in every material are held as given, and none is refused", "[gpu]") {
    start();
    const std::uint64_t old = make(1, 1, FBE_TEXTURE_RGBA8, false);
    const std::uint64_t now = make(1, 1, FBE_TEXTURE_RGBA8, false);
    const std::uint64_t numbers = make(1, 1, FBE_TEXTURE_RGBA8_LINEAR, false);
    const std::uint64_t material = fbe_material_create();
    REQUIRE(fbe_material_set_texture(material, 0, old));
    REQUIRE(fbe_material_set_map(material, FBE_MAP_NORMAL, numbers, 0));

    // None, or what isn't a texture, changes nothing, the empty layers included.
    const std::uint64_t none = 0;
    CHECK_FALSE(fbe_material_replace_textures(&none, &now, 1));
    CHECK(std::string(fbe_error()) == "a texture replaced is one, not none");
    CHECK(fbe_material_texture(material, 1) == 0);
    CHECK_FALSE(fbe_material_replace_textures(&old, &none, 1));
    CHECK_FALSE(fbe_material_replace_textures(&old, &now, -1));
    CHECK(fbe_material_texture(material, 0) == old);
    CHECK(fbe_material_replace_textures(nullptr, nullptr, 0));

    // Replaced, what was there is let go of by the material, and what's given held.
    CHECK(fbe_material_replace_textures(&old, &now, 1));
    CHECK(fbe_material_texture(material, 0) == now);
    const std::int32_t held = fbe_texture_count();
    CHECK(fbe_texture_release(old));
    CHECK(fbe_texture_count() == held - 1);
    CHECK(fbe_texture_release(now));
    CHECK(fbe_texture_format(now) == FBE_TEXTURE_RGBA8);

    // A layer takes only its own format, and a map only what it can read.
    CHECK(fbe_material_replace_textures(&now, &numbers, 1));
    CHECK(fbe_material_texture(material, 0) == now);
    const std::uint64_t color = make(1, 1, FBE_TEXTURE_RGBA8, false);
    CHECK(fbe_material_replace_textures(&numbers, &color, 1));
    CHECK(fbe_material_map(material, FBE_MAP_NORMAL) == numbers);

    CHECK(fbe_material_release(material));
    CHECK(fbe_texture_release(numbers));
    CHECK(fbe_texture_release(color));
    CHECK(fbe_texture_count() == 0);
    stop_gpu();
}
