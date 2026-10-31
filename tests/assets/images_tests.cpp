// Images decoded to their texels: PNGs and JPEGs written here by stb_image_write (and a 16-bit PNG
// by hand), and KTX2s by libktx's writer, raw, Zstandard-compressed, and Basis Universal's.

#include "assets/image.hpp"
#include "files.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ktx.h>
#include <memory>
#include <stb_image_write.h>
#include <thread>
#include <vector>

namespace {

namespace assets = fbe::assets;
using namespace std::string_view_literals;
using assets::bytes_t;

// A KTX2 of `levels` RGBA8 mips, `layers` layers, and `faces` faces, each texel given by `texel`
// of its level, layer, face, x, and y; then compressed as `finish` does.
template <typename Texel, typename Finish>
bytes_t ktx2(std::uint32_t width, std::uint32_t height, std::uint32_t levels, std::uint32_t layers,
             std::uint32_t faces, bool srgb, Texel texel, Finish finish) {
    ktxTextureCreateInfo info{};
    info.vkFormat = srgb ? assets::formats::rgba8_srgb : assets::formats::rgba8;
    info.baseWidth = width;
    info.baseHeight = height;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = levels;
    info.numLayers = layers;
    info.numFaces = faces;
    info.isArray = layers > 1 ? KTX_TRUE : KTX_FALSE;
    info.generateMipmaps = KTX_FALSE;
    ktxTexture2* made = nullptr;
    REQUIRE(ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &made) == KTX_SUCCESS);
    const std::unique_ptr<ktxTexture2, decltype(&ktxTexture2_Destroy)> texture(
        made, &ktxTexture2_Destroy);
    for (std::uint32_t level = 0; level < levels; ++level) {
        const std::uint32_t w = std::max(width >> level, 1U);
        const std::uint32_t h = std::max(height >> level, 1U);
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            for (std::uint32_t face = 0; face < faces; ++face) {
                std::vector<std::uint8_t> texels;
                for (std::uint32_t y = 0; y < h; ++y) {
                    for (std::uint32_t x = 0; x < w; ++x) {
                        const std::array<std::uint8_t, 4> value = texel(level, layer, face, x, y);
                        texels.insert(texels.end(), value.begin(), value.end());
                    }
                }
                REQUIRE(ktxTexture_SetImageFromMemory(reinterpret_cast<ktxTexture*>(texture.get()),
                                                      level, layer, face, texels.data(),
                                                      texels.size()) == KTX_SUCCESS);
            }
        }
    }
    finish(texture.get());
    ktx_uint8_t* bytes = nullptr;
    ktx_size_t size = 0;
    REQUIRE(ktxTexture2_WriteToMemory(texture.get(), &bytes, &size) == KTX_SUCCESS);
    bytes_t written(bytes, bytes + size);
    std::free(bytes); // NOLINT(cppcoreguidelines-no-malloc): libktx's memory is malloc's.
    return written;
}

std::array<std::uint8_t, 4> pattern(std::uint32_t level, std::uint32_t layer, std::uint32_t face,
                                    std::uint32_t x, std::uint32_t y) {
    return {
        static_cast<std::uint8_t>((x * 40) + level),
        static_cast<std::uint8_t>((y * 40) + layer),
        static_cast<std::uint8_t>(face * 30),
        static_cast<std::uint8_t>(255 - level),
    };
}

assets::image_t decoded(const bytes_t& bytes, assets::image_options_t options = {}) {
    assets::result_t<assets::image_t> image = assets::decode_image(bytes, options, "the test's");
    if (!image) {
        FAIL(image.error().message);
    }
    return std::move(*image);
}

assets::failure_t failure(const bytes_t& bytes) {
    assets::result_t<assets::image_t> image =
        assets::decode_image(bytes, assets::image_options_t(), "the test's");
    REQUIRE_FALSE(image.has_value());
    return image.error().failure;
}

} // namespace

TEST_CASE("a file's kind is what its bytes say", "[assets]") {
    using assets::file_kind_t;
    REQUIRE(assets::kind_of(test::png(1, 1, 4, { 1, 2, 3, 4 })) == file_kind_t::PNG);
    REQUIRE(assets::kind_of(test::jpeg(8, 8, { 10, 20, 30 })) == file_kind_t::JPEG);
    REQUIRE(assets::kind_of(ktx2(1, 1, 1, 1, 1, false, pattern, [](ktxTexture2*) {})) ==
            file_kind_t::KTX2);
    REQUIRE(assets::kind_of(test::bytes("glTF\x02\0\0\0"sv)) == file_kind_t::GLB);
    REQUIRE(assets::kind_of(test::bytes("\xEF\xBB\xBF \r\n\t{\"asset\":{}}"sv)) ==
            file_kind_t::GLTF);
    REQUIRE(assets::kind_of(test::bytes("xof 0303txt"sv)) == file_kind_t::UNKNOWN);
    REQUIRE(assets::kind_of({}) == file_kind_t::UNKNOWN);
}

TEST_CASE("a PNG decodes to RGBA8, sRGB unless it's asked not to be", "[assets]") {
    const std::vector<std::uint8_t> texels = {
        255, 0,  0,  255, 0,  255, 0,  128, 0,  0,   255, 0,
        10,  20, 30, 40,  50, 60,  70, 80,  90, 100, 110, 120,
    };
    const bytes_t png = test::png(3, 2, 4, texels);
    const assets::image_t image = decoded(png);
    REQUIRE(image.format == assets::formats::rgba8_srgb);
    REQUIRE(image.width == 3);
    REQUIRE(image.height == 2);
    REQUIRE(image.layers == 1);
    REQUIRE(image.faces == 1);
    REQUIRE(image.levels.size() == 1);
    REQUIRE(image.levels[0].size == 24);
    REQUIRE(image.texels == texels);
    REQUIRE(decoded(png, { .srgb = false }).format == assets::formats::rgba8);

    // Gray and RGB have alpha 255 added.
    const assets::image_t gray = decoded(test::png(2, 1, 1, { 7, 200 }));
    REQUIRE(gray.texels == std::vector<std::uint8_t>{ 7, 7, 7, 255, 200, 200, 200, 255 });
    const assets::image_t rgb = decoded(test::png(1, 1, 3, { 1, 2, 3 }));
    REQUIRE(rgb.texels == std::vector<std::uint8_t>{ 1, 2, 3, 255 });
}

TEST_CASE("a 16-bit PNG keeps its 16 bits", "[assets]") {
    const assets::image_t image =
        decoded(test::png16(2, 1, { 0x1234, 0x5678, 0x9ABC, 0xFFFF, 0, 1, 2, 0x8000 }));
    REQUIRE(image.format == assets::formats::rgba16);
    REQUIRE(image.levels[0].size == 16);
    std::vector<std::uint16_t> wide(8);
    std::memcpy(wide.data(), image.texels.data(), 16);
    REQUIRE(wide == std::vector<std::uint16_t>{ 0x1234, 0x5678, 0x9ABC, 0xFFFF, 0, 1, 2, 0x8000 });
}

TEST_CASE("a JPEG decodes near its colors", "[assets]") {
    const assets::image_t image = decoded(test::jpeg(16, 16, { 200, 100, 50 }));
    REQUIRE(image.format == assets::formats::rgba8_srgb);
    REQUIRE(image.width == 16);
    REQUIRE(image.texels.size() == std::size_t{ 16 } * 16 * 4);
    for (std::size_t i = 0; i < image.texels.size(); i += 4) {
        REQUIRE(std::abs(image.texels[i] - 200) <= 4);
        REQUIRE(std::abs(image.texels[i + 1] - 100) <= 4);
        REQUIRE(std::abs(image.texels[i + 2] - 50) <= 4);
        REQUIRE(image.texels[i + 3] == 255);
    }
}

TEST_CASE("what isn't a whole image is invalid data", "[assets]") {
    const bytes_t png = test::png(4, 4, 4, std::vector<std::uint8_t>(64, 9));
    REQUIRE(failure(bytes_t(png.begin(), png.begin() + 40)) == assets::failure_t::INVALID_DATA);
    REQUIRE(failure(test::bytes("not an image at all"sv)) == assets::failure_t::INVALID_DATA);
    REQUIRE(failure({}) == assets::failure_t::INVALID_DATA);
    const bytes_t ktx = ktx2(4, 4, 1, 1, 1, false, pattern, [](ktxTexture2*) {});
    REQUIRE(failure(bytes_t(ktx.begin(), ktx.end() - 10)) == assets::failure_t::INVALID_DATA);
    // A glTF isn't an image.
    REQUIRE(failure(test::bytes("{\"asset\":{\"version\":\"2.0\"}}"sv)) ==
            assets::failure_t::INVALID_DATA);
}

TEST_CASE("a KTX2 keeps its mips, layers, and faces", "[assets]") {
    SECTION("mips and layers") {
        const assets::image_t image =
            decoded(ktx2(8, 4, 3, 2, 1, true, pattern, [](ktxTexture2*) {}));
        REQUIRE(image.format == assets::formats::rgba8_srgb);
        REQUIRE(image.width == 8);
        REQUIRE(image.height == 4);
        REQUIRE(image.layers == 2);
        REQUIRE(image.faces == 1);
        REQUIRE(image.levels.size() == 3);
        for (std::uint32_t level = 0; level < 3; ++level) {
            const assets::level_t& at = image.levels[level];
            REQUIRE(at.width == std::max(8U >> level, 1U));
            REQUIRE(at.height == std::max(4U >> level, 1U));
            REQUIRE(at.size == std::size_t{ at.width } * at.height * 4);
            for (std::uint32_t layer = 0; layer < 2; ++layer) {
                for (std::uint32_t y = 0; y < at.height; ++y) {
                    for (std::uint32_t x = 0; x < at.width; ++x) {
                        const std::size_t texel = at.offset + (layer * at.size) +
                                                  (((std::size_t{ y } * at.width) + x) * 4);
                        const std::array<std::uint8_t, 4> want = pattern(level, layer, 0, x, y);
                        REQUIRE(
                            std::equal(want.begin(), want.end(),
                                       image.texels.begin() + static_cast<std::ptrdiff_t>(texel)));
                    }
                }
            }
        }
    }
    SECTION("a cube map's six faces") {
        const assets::image_t image =
            decoded(ktx2(4, 4, 1, 1, 6, false, pattern, [](ktxTexture2*) {}));
        REQUIRE(image.format == assets::formats::rgba8);
        REQUIRE(image.faces == 6);
        const assets::level_t& level = image.levels[0];
        for (std::uint32_t face = 0; face < 6; ++face) {
            REQUIRE(image.texels[level.offset + (face * level.size) + 2] == face * 30);
        }
    }
    SECTION("Zstandard-compressed") {
        const assets::image_t image =
            decoded(ktx2(8, 8, 2, 1, 1, false, pattern, [](ktxTexture2* texture) {
                REQUIRE(ktxTexture2_DeflateZstd(texture, 5) == KTX_SUCCESS);
            }));
        REQUIRE(image.levels.size() == 2);
        REQUIRE(image.texels[image.levels[1].offset + 4] == 40 + 1);
    }
}

TEST_CASE("a Basis Universal KTX2 transcodes to what it's asked for", "[assets]") {
    // Flat colors, which ETC1S and UASTC keep nearly as they are.
    const auto flat = [](std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t x,
                         std::uint32_t) -> std::array<std::uint8_t, 4> {
        return x < 8 ? std::array<std::uint8_t, 4>{ 200, 40, 40, 255 }
                     : std::array<std::uint8_t, 4>{ 40, 40, 200, 255 };
    };
    for (const bool uastc : { false, true }) {
        const bytes_t basis = ktx2(16, 16, 1, 1, 1, true, flat, [uastc](ktxTexture2* texture) {
            ktxBasisParams params{};
            params.structSize = sizeof(params);
            params.uastc = uastc ? KTX_TRUE : KTX_FALSE;
            params.threadCount = 1;
            params.qualityLevel = 128;
            REQUIRE(ktxTexture2_CompressBasisEx(texture, &params) == KTX_SUCCESS);
        });

        const assets::image_t rgba = decoded(basis, { .transcode = assets::transcode_t::RGBA8 });
        REQUIRE(rgba.format == assets::formats::rgba8_srgb);
        REQUIRE(rgba.levels[0].size == std::size_t{ 16 } * 16 * 4);
        REQUIRE(std::abs(rgba.texels[0] - 200) <= 12);
        REQUIRE(std::abs(rgba.texels[(15 * 4) + 2] - 200) <= 12);

        const assets::image_t bc7 = decoded(basis, { .transcode = assets::transcode_t::BC7 });
        REQUIRE(bc7.format == assets::formats::bc7_srgb);
        // 16 bytes a 4x4 block.
        REQUIRE(bc7.levels[0].size == std::size_t{ 4 } * 4 * 16);

        const assets::image_t astc = decoded(basis, { .transcode = assets::transcode_t::ASTC_4X4 });
        REQUIRE(astc.format == assets::formats::astc_4x4_srgb);
        REQUIRE(astc.levels[0].size == std::size_t{ 4 } * 4 * 16);
    }
}

TEST_CASE("Basis Universal KTX2s transcode on many threads at once", "[assets]") {
    const bytes_t basis = ktx2(32, 32, 1, 1, 1, false, pattern, [](ktxTexture2* texture) {
        ktxBasisParams params{};
        params.structSize = sizeof(params);
        params.uastc = KTX_TRUE;
        params.threadCount = 1;
        REQUIRE(ktxTexture2_CompressBasisEx(texture, &params) == KTX_SUCCESS);
    });
    // Each thread's first decode is at once, as the process's first transcodes can be.
    std::vector<std::vector<bytes_t>> texels(8);
    std::atomic<int> failed = 0;
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (std::size_t t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 5; ++i) {
                const assets::result_t<assets::image_t> image =
                    assets::decode_image(basis, assets::image_options_t(), "the test's");
                if (image) {
                    texels[t].push_back(image->texels);
                } else {
                    ++failed;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    REQUIRE(failed == 0);
    int different = 0;
    for (const std::vector<bytes_t>& each : texels) {
        for (const bytes_t& decoded_texels : each) {
            different += decoded_texels != texels[0][0] ? 1 : 0;
        }
    }
    REQUIRE(different == 0);
}
