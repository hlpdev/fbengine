// The cooker's images: mips made in linear light, and PNGs cooked into KTX2s of each format,
// read back by libktx.

#include "../assets/files.hpp"
#include "cook/cook.hpp"

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <ktx.h>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace tools = fbe::tools;

using texture_ptr_t = std::unique_ptr<ktxTexture2, decltype(&ktxTexture2_Destroy)>;

texture_ptr_t read(const tools::bytes_t& bytes) {
    ktxTexture2* texture = nullptr;
    REQUIRE(ktxTexture2_CreateFromMemory(bytes.data(), bytes.size(),
                                         KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                         &texture) == KTX_SUCCESS);
    return { texture, &ktxTexture2_Destroy };
}

tools::bytes_t cooked(const tools::bytes_t& image, fbe_cook_image_t options) {
    std::expected<tools::bytes_t, std::string> bytes = tools::cook_image(image, options);
    if (!bytes) {
        FAIL(bytes.error());
    }
    return std::move(*bytes);
}

// A gradient, so its mips differ from it.
std::vector<std::uint8_t> gradient(int width, int height) {
    std::vector<std::uint8_t> texels;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            texels.push_back(static_cast<std::uint8_t>(x * 255 / width));
            texels.push_back(static_cast<std::uint8_t>(y * 255 / height));
            texels.push_back(128);
            texels.push_back(255);
        }
    }
    return texels;
}

} // namespace

TEST_CASE("a mip averages in linear light, alpha as it is", "[tools]") {
    const tools::rgba_t image{
        .width = 2,
        .height = 2,
        .texels = { 0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 255, 255, 255, 255 },
    };
    const tools::rgba_t srgb = tools::half(image, true);
    REQUIRE(srgb.width == 1);
    REQUIRE(srgb.height == 1);
    // Half of white's light is sRGB's 188, not 128; alpha is 128 either way.
    REQUIRE(srgb.texels == std::vector<std::uint8_t>{ 188, 188, 188, 128 });
    REQUIRE(tools::half(image, false).texels == std::vector<std::uint8_t>{ 128, 128, 128, 128 });
}

TEST_CASE("a mip of an odd size takes in its last row and column", "[tools]") {
    // 3 by 1: the one texel of its mip is the mean of all three.
    const tools::rgba_t image{
        .width = 3,
        .height = 1,
        .texels = { 0, 0, 0, 255, 90, 0, 0, 255, 180, 0, 0, 255 },
    };
    const tools::rgba_t next = tools::half(image, false);
    REQUIRE(next.width == 1);
    REQUIRE(next.height == 1);
    REQUIRE(next.texels[0] == 90);
    // 5 by 3 halves to 2 by 1, its second texel the mean of columns 2 to 4 of all three rows.
    tools::rgba_t wide{ .width = 5, .height = 3 };
    for (int i = 0; i < 15; ++i) {
        wide.texels.insert(wide.texels.end(),
                           { static_cast<std::uint8_t>((i % 5) * 10), 0, 0, 255 });
    }
    const tools::rgba_t halved = tools::half(wide, false);
    REQUIRE(halved.width == 2);
    REQUIRE(halved.height == 1);
    REQUIRE(halved.texels[0] == 5);
    REQUIRE(halved.texels[4] == 30);
}

TEST_CASE("an image cooks to a KTX2 of each format, with its mips", "[tools]") {
    const tools::bytes_t png = test::png(16, 8, 4, gradient(16, 8));
    struct expect_t {
        std::int32_t format = 0;
        bool srgb = false;
        std::uint32_t vk_format = 0;
        bool transcodes = false;
    };
    for (const expect_t& expected : {
             expect_t{ .format = FBE_COOK_RGBA8, .srgb = true, .vk_format = 43 },
             expect_t{ .format = FBE_COOK_RGBA8, .srgb = false, .vk_format = 37 },
             expect_t{ .format = FBE_COOK_BC7, .srgb = true, .vk_format = 146 },
             expect_t{ .format = FBE_COOK_BC7, .srgb = false, .vk_format = 145 },
             expect_t{ .format = FBE_COOK_ASTC, .srgb = true, .vk_format = 158 },
             expect_t{ .format = FBE_COOK_UASTC, .srgb = true, .transcodes = true },
         }) {
        const tools::bytes_t bytes = cooked(
            png,
            { .format = expected.format, .srgb = expected.srgb, .mipmaps = true, .quality = 0 });
        const texture_ptr_t texture = read(bytes);
        REQUIRE(texture->baseWidth == 16);
        REQUIRE(texture->baseHeight == 8);
        // 16, 8, 4, 2, and 1 across.
        REQUIRE(texture->numLevels == 5);
        REQUIRE((ktxTexture2_NeedsTranscoding(texture.get()) != 0) == expected.transcodes);
        if (!expected.transcodes) {
            REQUIRE(texture->vkFormat == expected.vk_format);
            // As the file has it: libktx inflates it as it loads.
            REQUIRE(bytes.at(44) == KTX_SS_ZSTD);
        } else {
            // Transcoded as it loads, to what a GPU reads.
            REQUIRE(ktxTexture2_TranscodeBasis(texture.get(), KTX_TTF_RGBA32, 0) == KTX_SUCCESS);
            REQUIRE(texture->vkFormat == 43);
        }
    }
}

TEST_CASE("a cooked image's texels are its own", "[tools]") {
    const std::vector<std::uint8_t> texels = gradient(8, 4);
    const tools::bytes_t bytes = cooked(
        test::png(8, 4, 4, texels), { .format = FBE_COOK_RGBA8, .srgb = false, .mipmaps = false });
    const texture_ptr_t texture = read(bytes);
    REQUIRE(texture->numLevels == 1);
    ktx_size_t offset = 0;
    REQUIRE(ktxTexture_GetImageOffset(reinterpret_cast<ktxTexture*>(texture.get()), 0, 0, 0,
                                      &offset) == KTX_SUCCESS);
    REQUIRE(std::equal(texels.begin(), texels.end(), texture->pData + offset));

    // A size that isn't a power of two has every mip down to 1x1.
    const texture_ptr_t odd =
        read(cooked(test::png(13, 7, 4, gradient(13, 7)),
                    { .format = FBE_COOK_BC7, .srgb = true, .mipmaps = true }));
    REQUIRE(std::cmp_equal(odd->numLevels, std::bit_width(13U)));
}

TEST_CASE("what isn't a PNG or a JPEG doesn't cook", "[tools]") {
    const tools::bytes_t text = test::bytes("not an image");
    REQUIRE_FALSE(tools::cook_image(text, { .format = FBE_COOK_BC7 }).has_value());
    REQUIRE_FALSE(
        tools::cook_image(test::png(2, 2, 4, gradient(2, 2)), { .format = 9 }).has_value());
}
