// Images decoded to their texels: PNGs and JPEGs written here by stb_image_write (and a 16-bit PNG
// by hand), and KTX2s by libktx's writer, raw, Zstandard-compressed, and Basis Universal's.

#include "assets/image.hpp"
#include "files.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ktx.h>
#include <memory>
#include <random>
#include <stb_image_write.h>
#include <string>
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
    REQUIRE(std::ranges::equal(image.texels.bytes(), texels));
    REQUIRE(decoded(png, { .srgb = false }).format == assets::formats::rgba8);

    // Gray and RGB have alpha 255 added.
    const assets::image_t gray = decoded(test::png(2, 1, 1, { 7, 200 }));
    REQUIRE(std::ranges::equal(gray.texels.bytes(),
                               std::vector<std::uint8_t>{ 7, 7, 7, 255, 200, 200, 200, 255 }));
    const assets::image_t rgb = decoded(test::png(1, 1, 3, { 1, 2, 3 }));
    REQUIRE(std::ranges::equal(rgb.texels.bytes(), std::vector<std::uint8_t>{ 1, 2, 3, 255 }));
}

TEST_CASE("a 16-bit PNG of color is its high bytes, and of data keeps its 16 bits", "[assets]") {
    const bytes_t png = test::png16(2, 1, { 0x1234, 0x5678, 0x9ABC, 0xFFFF, 0, 1, 2, 0x8000 });
    const assets::image_t color = decoded(png);
    REQUIRE(color.format == assets::formats::rgba8_srgb);
    REQUIRE(color.levels[0].size == 8);
    REQUIRE(std::vector<std::uint8_t>(color.texels.data(), color.texels.data() + 8) ==
            std::vector<std::uint8_t>{ 0x12, 0x56, 0x9A, 0xFF, 0, 0, 0, 0x80 });
    auto linear = assets::decode_image(png, { .srgb = false }, "wide");
    REQUIRE(linear.has_value());
    const assets::image_t image = std::move(*linear);
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

TEST_CASE("a PNG whose first IDAT is empty decodes", "[assets]") {
    // An empty IDAT, its CRC that of "IDAT" alone, before the first.
    bytes_t png = test::png(4, 4, 4, std::vector<std::uint8_t>(64, 9));
    const auto idat = std::ranges::search(png, std::string_view("IDAT")).begin() - 4;
    const bytes_t empty = { 0, 0, 0, 0, 'I', 'D', 'A', 'T', 0x35, 0xAF, 0x06, 0x1E };
    png.insert(idat, empty.begin(), empty.end());
    const assets::image_t image = decoded(png);
    REQUIRE(image.width == 4);
    const auto nine = [](std::uint8_t texel) { return texel == 9; };
    REQUIRE(std::ranges::all_of(image.texels.bytes(), nine));
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
                        REQUIRE(std::equal(want.begin(), want.end(),
                                           image.texels.bytes().begin() +
                                               static_cast<std::ptrdiff_t>(texel)));
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

TEST_CASE("a UASTC KTX2 transcodes to what it's asked for", "[assets]") {
    // Flat colors, which UASTC keeps nearly as they are.
    const auto flat = [](std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t x,
                         std::uint32_t) -> std::array<std::uint8_t, 4> {
        return x < 8 ? std::array<std::uint8_t, 4>{ 200, 40, 40, 255 }
                     : std::array<std::uint8_t, 4>{ 40, 40, 200, 255 };
    };
    {
        const bytes_t basis = ktx2(16, 16, 1, 1, 1, true, flat, [](ktxTexture2* texture) {
            ktxBasisParams params{};
            params.structSize = sizeof(params);
            params.uastc = KTX_TRUE;
            params.threadCount = 1;
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
                    texels[t].emplace_back(image->texels.bytes().begin(),
                                           image->texels.bytes().end());
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

namespace {

bytes_t fixture(std::string_view name) {
    std::ifstream stream(std::filesystem::path(FBENGINE_ASSETS_FIXTURES) / name, std::ios::binary);
    REQUIRE(stream.good());
    return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}

void put_u32(bytes_t& bytes, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[at + i] = static_cast<std::uint8_t>(value >> (8U * i));
    }
}

std::uint32_t u32_of(const bytes_t& bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= std::uint32_t{ bytes.at(at + i) } << (8U * i);
    }
    return value;
}

void put_u64(bytes_t& bytes, std::size_t at, std::uint64_t value) {
    put_u32(bytes, at, static_cast<std::uint32_t>(value));
    put_u32(bytes, at + 4, static_cast<std::uint32_t>(value >> 32U));
}

std::string message(const bytes_t& bytes) {
    assets::result_t<assets::image_t> image =
        assets::decode_image(bytes, assets::image_options_t(), "the test's");
    REQUIRE_FALSE(image.has_value());
    REQUIRE(image.error().failure == assets::failure_t::INVALID_DATA);
    return image.error().message;
}

} // namespace

TEST_CASE("an image bigger than an image can be isn't decoded", "[assets]") {
    // A PNG one texel past, which says so in its header, before anything's decoded.
    REQUIRE(message(test::png(assets::most_side + 1, 1, 1,
                              std::vector<std::uint8_t>(assets::most_side + 1, 0)))
                .contains("16384"));
    REQUIRE(
        decoded(test::png(assets::most_side, 1, 1, std::vector<std::uint8_t>(assets::most_side, 0)))
            .width == assets::most_side);
    // A 622-byte JPEG that asks for 1.33 GB.
    REQUIRE(failure(fixture("jpeg_bomb.jpg")) == assets::failure_t::INVALID_DATA);

    // A KTX2's header, by its sizes, its layers, and all of them at once.
    const bytes_t ktx = ktx2(4, 4, 1, 1, 1, false, pattern, [](ktxTexture2*) {});
    for (const auto& [at, value] : std::vector<std::pair<std::size_t, std::uint32_t>>{
             { 20, assets::most_side + 1 },
             { 24, assets::most_side + 1 },
             { 28, assets::most_side + 1 },
             { 32, assets::most_layers + 1 },
             { 36, 2 },
         }) {
        bytes_t changed = ktx;
        put_u32(changed, at, value);
        INFO(at);
        REQUIRE(failure(changed) == assets::failure_t::INVALID_DATA);
    }
    bytes_t huge = ktx;
    put_u32(huge, 20, assets::most_side);
    put_u32(huge, 24, assets::most_side);
    put_u32(huge, 32, 4);
    REQUIRE(message(huge).contains("texels"));
}

TEST_CASE("an ETC1S KTX2, in BasisLZ, isn't loaded", "[assets]") {
    const bytes_t etc1s = ktx2(16, 16, 1, 1, 1, true, pattern, [](ktxTexture2* texture) {
        ktxBasisParams params{};
        params.structSize = sizeof(params);
        params.uastc = KTX_FALSE;
        params.threadCount = 1;
        REQUIRE(ktxTexture2_CompressBasisEx(texture, &params) == KTX_SUCCESS);
    });
    REQUIRE(message(etc1s).contains("ETC1S"));
    // The review's, whose transcode asks for a gigabyte.
    REQUIRE(failure(fixture("etc1s_bomb.ktx2")) == assets::failure_t::INVALID_DATA);
}

TEST_CASE("a KTX2's levels and parts are within it, as many as its sizes have", "[assets]") {
    for (const bool zstd : { false, true }) {
        INFO(zstd);
        const bytes_t ktx = ktx2(8, 8, 4, 1, 1, false, pattern, [zstd](ktxTexture2* texture) {
            if (zstd) {
                REQUIRE(ktxTexture2_DeflateZstd(texture, 5) == KTX_SUCCESS);
            }
        });
        REQUIRE(decoded(ktx).levels.size() == 4);
        // Each level's offset and length, past the end, and wrapping round.
        for (std::size_t level = 0; level < 4; ++level) {
            const std::size_t at = 80 + (level * 24);
            bytes_t past = ktx;
            put_u64(past, at + 8, ktx.size());
            REQUIRE(message(past).contains("past its end"));
            bytes_t wraps = ktx;
            put_u64(wraps, at, ~std::uint64_t{ 0 } - 4);
            put_u64(wraps, at + 8, 16);
            REQUIRE(message(wraps).contains("past its end"));
        }
        // The data format, key/values, and supercompression's global data.
        for (const std::size_t at : { 48U, 56U }) {
            bytes_t past = ktx;
            put_u32(past, at, static_cast<std::uint32_t>(ktx.size()));
            put_u32(past, at + 4, 1);
            REQUIRE(message(past).contains("past its end"));
        }
        bytes_t global = ktx;
        put_u64(global, 64, ktx.size());
        put_u64(global, 72, 1);
        REQUIRE(message(global).contains("past its end"));
        // More levels than an 8 by 8 has, and as many as libktx would shift by.
        for (const std::uint32_t levels : { 5U, 33U, 0xFFFF'FFFFU }) {
            bytes_t more = ktx;
            put_u32(more, 40, levels);
            REQUIRE(message(more).contains("mip levels"));
        }
    }
}

// The fuzz test's: a level's images are as big as its data format says a texel's bytes are, which
// libktx doesn't hold to what the level has, so one saying more than its level has isn't loaded.
TEST_CASE("a KTX2 whose texels take more than its levels have isn't loaded", "[assets]") {
    for (const std::uint32_t faces : { 1U, 6U }) {
        INFO(faces);
        bytes_t ktx = ktx2(4, 4, 1, 1, faces, false, pattern, [](ktxTexture2*) {});
        REQUIRE(decoded(ktx).levels.size() == 1);
        // The basic block's bytesPlane0: past its total size, its header, and its texel block's
        // dimensions.
        const std::size_t planes = u32_of(ktx, 48) + 4 + 16;
        REQUIRE(ktx.at(planes) == 4);
        // A cube's first face of 8 bytes a texel is within its 384, and its sixth is past them.
        ktx.at(planes) = faces == 6 ? 8 : 223;
        REQUIRE(message(ktx).contains("has level 0 past its texels"));
    }
}

// The fuzz test's: libktx's offsets are from where the first level in the file starts, the
// smallest, so a level that says it starts before that has one that wraps round, which isn't
// loaded, as it's out of a KTX2's order.
TEST_CASE("a KTX2 level before its texels isn't loaded", "[assets]") {
    const bytes_t ktx = ktx2(8, 4, 3, 2, 1, true, pattern, [](ktxTexture2*) {});
    REQUIRE(decoded(ktx).levels.size() == 3);
    const auto offset_at = [](std::size_t level) { return 80 + (level * 24); };
    // Level 2 is first in the file, then 1, then 0.
    REQUIRE(u32_of(ktx, offset_at(2)) < u32_of(ktx, offset_at(1)));
    bytes_t before = ktx;
    put_u64(before, offset_at(1), u32_of(ktx, offset_at(2)) - 14);
    REQUIRE(message(before).contains("has level 1 out of the order"));
}

// ASan's, in the fuzz test: libktx allocates what a KTX2's levels say they inflate to before it
// reads them, so what they say is held to what they are, uncompressed, and to what its texels could
// be, of 16 bytes each, Zstd's or not.
TEST_CASE("a KTX2's levels inflate to no more than its texels could be", "[assets]") {
    for (const bool zstd : { false, true }) {
        INFO(zstd);
        const bytes_t ktx = ktx2(4, 4, 2, 1, 1, false, pattern, [zstd](ktxTexture2* texture) {
            if (zstd) {
                REQUIRE(ktxTexture2_DeflateZstd(texture, 3) == KTX_SUCCESS);
            }
        });
        REQUIRE(decoded(ktx).levels.size() == 2);
        // A level's uncompressedByteLength: as a wrapped sum's, a terabyte, and one byte past an
        // image of 4 by 4 of 16 bytes a texel, mips and all, with the other level's 16.
        const std::uint64_t first = 80 + 16;
        for (const std::uint64_t length : { ~std::uint64_t{ 0 } - 34, std::uint64_t{ 1 } << 40U,
                                            std::uint64_t{ (16 * 16 * 2) - 16 + 1 } }) {
            INFO(length);
            bytes_t inflating = ktx;
            put_u64(inflating, first, length);
            REQUIRE(message(inflating).contains("more than its texels could be"));
        }
        // Two levels each of half of what wraps round.
        bytes_t halves = ktx;
        put_u64(halves, first, std::uint64_t{ 1 } << 63U);
        put_u64(halves, first + 24, std::uint64_t{ 1 } << 63U);
        REQUIRE(message(halves).contains("more than its texels could be"));
    }
    // Uncompressed, a level's is what it is.
    bytes_t raw = ktx2(4, 4, 1, 1, 1, false, pattern, [](ktxTexture2*) {});
    put_u64(raw, 80 + 16, 63);
    REQUIRE(message(raw).contains("level 0 inflating to 63 bytes"));
}

// ASan's, in the fuzz test: libktx's texels are from where the last level starts to where level 0
// ends, so a level before the one or past the other has it allocate what wraps round.
TEST_CASE("a KTX2's levels are in the order a KTX2's are", "[assets]") {
    const bytes_t ktx = ktx2(8, 4, 3, 1, 1, false, pattern, [](ktxTexture2*) {});
    REQUIRE(decoded(ktx).levels.size() == 3);
    const auto offset_at = [](std::size_t level) { return 80 + (level * 24); };
    // Level 0 at the file's start, before the others, and level 2, the first, past level 0's end.
    bytes_t first_first = ktx;
    put_u64(first_first, offset_at(0), 0);
    REQUIRE(message(first_first).contains("has level 0 out of the order"));
    bytes_t last_last = ktx;
    put_u64(last_last, offset_at(2), u32_of(ktx, offset_at(0)) + u32_of(ktx, offset_at(0) + 8));
    put_u64(last_last, offset_at(2) + 8, 0);
    REQUIRE(message(last_last).contains("out of the order"));
    // Level 0 over level 1, a byte long, so level 1 ends past it: libktx would copy level 1 past
    // the texels it allocated.
    bytes_t past_last = ktx;
    put_u64(past_last, offset_at(0), u32_of(ktx, offset_at(1)));
    put_u64(past_last, offset_at(0) + 8, 1);
    put_u64(past_last, offset_at(0) + 16, 1);
    REQUIRE(message(past_last).contains("has level 1 out of the order"));
}

// The fuzz test's, which hung: libktx walks a data format descriptor's blocks by their sizes, so
// one of no size, as the words past a block cut short read, has it loop forever; one past the rest
// would have it read past them.
TEST_CASE("a KTX2's data format descriptor's blocks fill it as they say", "[assets]") {
    const bytes_t ktx = ktx2(8, 4, 3, 2, 1, true, pattern, [](ktxTexture2*) {});
    const std::size_t dfd = u32_of(ktx, 48);
    // Its total size, its block's size, and 4 samples, of 16 bytes each.
    REQUIRE(u32_of(ktx, dfd) == u32_of(ktx, 52));
    REQUIRE(ktx.at(dfd + 10) == 88);
    for (const std::uint8_t size : { 72, 0, 90, 92 }) {
        INFO(int{ size });
        bytes_t block = ktx;
        block.at(dfd + 10) = size;
        REQUIRE(message(block).contains("data format descriptor block of the wrong size"));
    }
    bytes_t total = ktx;
    put_u32(total, dfd, u32_of(ktx, dfd) + 4);
    REQUIRE(message(total).contains("data format descriptor of the wrong size"));
}

TEST_CASE("a KTX2 supercompressed but by Zstd isn't loaded", "[assets]") {
    bytes_t zlib = ktx2(4, 4, 1, 1, 1, false, pattern, [](ktxTexture2*) {});
    put_u32(zlib, 44, 3);
    REQUIRE(message(zlib).contains("supercompressed"));
}

TEST_CASE("an image's texels are the decoder's own, not a copy", "[assets]") {
    const bytes_t ktx = ktx2(4, 4, 1, 1, 1, false, pattern, [](ktxTexture2*) {});
    assets::image_t image = decoded(ktx);
    // Kept as they are while the image is, and its copies, which share them.
    const assets::texels_t kept = image.texels;
    image = assets::image_t();
    REQUIRE(kept.size() == std::size_t{ 4 } * 4 * 4);
    REQUIRE(kept[0] == pattern(0, 0, 0, 0, 0)[0]);
}

TEST_CASE("a KTX2's sample of 32 bits or more isn't loaded", "[assets]") {
    // The review's, whose samples libktx would shift 1 by 119, 26 in a Zstd one, and a cube map's.
    for (const std::string_view name :
         { "dfd_cube_0.ktx2", "dfd_uastc_119.ktx2", "dfd_zstd_26.ktx2", "assert_crash.ktx2" }) {
        INFO(name);
        REQUIRE(failure(fixture(name)) == assets::failure_t::INVALID_DATA);
    }
    // One of 32 bits, made here, and the 8 bits a real RGBA8's are.
    const bytes_t ktx = ktx2(4, 4, 1, 1, 1, false, pattern, [](ktxTexture2*) {});
    REQUIRE(decoded(ktx).width == 4);
    bytes_t wide = ktx;
    const std::uint32_t dfd =
        static_cast<std::uint32_t>(wide[48]) | (static_cast<std::uint32_t>(wide[49]) << 8U);
    // The first sample's bit length, its word's third byte, at the block's 24th byte.
    wide[dfd + 4 + 24 + 2] = 32;
    REQUIRE(message(wide).contains("32 bits"));
}

TEST_CASE("a BMP decodes, whatever it's called, and a DDS is refused, saying so", "[assets]") {
    // A BMP's headers, 1 by 1, 24 bits, then its pixel, blue first, padded to 4 bytes.
    bytes_t bmp(58, 0);
    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[2] = 58;
    bmp[10] = 54;
    bmp[14] = 40;
    bmp[18] = 1;
    bmp[22] = 1;
    bmp[26] = 1;
    bmp[28] = 24;
    bmp[34] = 4;
    bmp[54] = 3;
    bmp[55] = 2;
    bmp[56] = 1;
    const assets::image_t image = decoded(bmp);
    REQUIRE(image.format == assets::formats::rgba8_srgb);
    REQUIRE(image.width == 1);
    REQUIRE(image.texels[0] == 1);
    REQUIRE(image.texels[1] == 2);
    REQUIRE(image.texels[2] == 3);
    REQUIRE(image.texels[3] == 255);
    const bytes_t dds{ 'D', 'D', 'S', ' ', 0, 0, 0, 0 };
    REQUIRE(message(dds).contains("DDS"));
    REQUIRE(failure(dds) == assets::failure_t::INVALID_DATA);
    // The fuzz test's: one of no width, which stb_image decodes, isn't an image.
    bmp[18] = 0;
    REQUIRE(failure(bmp) == assets::failure_t::INVALID_DATA);
    REQUIRE(message(bmp).contains("0 by 1, which has no texels"));
}

namespace {

/// A copy of `seed` mutated by `random`: bits flipped, bytes set, a word of 32 bits set to a size
/// that's nearly too big or none, a stretch copied over another, or the end cut.
bytes_t mutated(const bytes_t& seed, std::mt19937& random) {
    bytes_t bytes = seed;
    const auto at = [&](std::size_t size) {
        return std::uniform_int_distribution<std::size_t>(0, size - 1)(random);
    };
    const int changes = std::uniform_int_distribution<int>(1, 4)(random);
    for (int change = 0; change < changes && !bytes.empty(); ++change) {
        switch (std::uniform_int_distribution<int>(0, 4)(random)) {
        case 0:
            bytes[at(bytes.size())] ^= static_cast<std::uint8_t>(1U << at(8));
            break;
        case 1:
            bytes[at(bytes.size())] = static_cast<std::uint8_t>(at(256));
            break;
        case 2: {
            constexpr std::array<std::uint32_t, 8> edges{
                0, 1, 2, 0x7FFF, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
            };
            const std::uint32_t value = edges.at(at(edges.size()));
            const std::size_t where = at(bytes.size());
            for (std::size_t byte = 0; byte < 4 && where + byte < bytes.size(); ++byte) {
                bytes[where + byte] = static_cast<std::uint8_t>(value >> (8 * byte));
            }
            break;
        }
        case 3: {
            const std::size_t from = at(bytes.size());
            const std::size_t to = at(bytes.size());
            const std::size_t length = std::min(at(16) + 1, bytes.size() - std::max(from, to));
            std::copy_n(seed.begin() + static_cast<std::ptrdiff_t>(from), length,
                        bytes.begin() + static_cast<std::ptrdiff_t>(to));
            break;
        }
        default:
            bytes.resize(at(bytes.size()));
            break;
        }
    }
    return bytes;
}

/// That a decoded image is what it says: its sizes within the most an image can be, and each of
/// its levels within its texels, each byte of which is read.
void whole(const assets::image_t& image) {
    REQUIRE(image.width >= 1);
    REQUIRE(image.height >= 1);
    REQUIRE(image.width <= assets::most_side);
    REQUIRE(image.height <= assets::most_side);
    REQUIRE(image.layers <= assets::most_layers);
    REQUIRE(!image.levels.empty());
    // Each image of each level, one a layer and a face or a slice.
    for (const assets::level_t& level : image.levels) {
        const std::uint64_t parts =
            std::uint64_t{ std::max(image.layers, 1U) } * image.faces * level.depth;
        REQUIRE(level.offset <= image.texels.size());
        REQUIRE(level.size * parts <= image.texels.size() - level.offset);
    }
    unsigned sum = 0;
    for (const std::uint8_t texel : image.texels.bytes()) {
        sum += texel;
    }
    CHECK(sum < UINT_MAX);
}

} // namespace

// Each kind of file decoded, mutated: each mutant is refused as invalid data, or decodes to an
// image that's what it says, and nothing reads or writes past what it has, which ASan checks where
// it runs. FBE_FUZZ_ROUNDS asks for more than the 400 a seed CI runs.
TEST_CASE("an image's mutants are refused or decode whole", "[assets]") {
    const char* asked = std::getenv("FBE_FUZZ_ROUNDS");
    const int rounds = asked != nullptr ? std::atoi(asked) : 400;
    bytes_t bmp(58, 0);
    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[2] = 58;
    bmp[10] = 54;
    bmp[14] = 40;
    bmp[18] = 1;
    bmp[22] = 1;
    bmp[26] = 1;
    bmp[28] = 24;
    bmp[34] = 4;
    const auto basis = [](ktxTexture2* texture) {
        ktxBasisParams params{};
        params.structSize = sizeof(params);
        params.uastc = KTX_TRUE;
        params.threadCount = 1;
        REQUIRE(ktxTexture2_CompressBasisEx(texture, &params) == KTX_SUCCESS);
    };
    const auto zstd = [](ktxTexture2* texture) {
        REQUIRE(ktxTexture2_DeflateZstd(texture, 3) == KTX_SUCCESS);
    };
    const std::vector<std::pair<std::string_view, bytes_t>> seeds{
        { "PNG", test::png(8, 8, 4, std::vector<std::uint8_t>(256, 77)) },
        { "gray PNG", test::png(5, 3, 1, std::vector<std::uint8_t>(15, 30)) },
        { "16-bit PNG", test::png16(2, 2, std::vector<std::uint16_t>(16, 0x1234)) },
        { "JPEG", test::jpeg(16, 16, { 200, 100, 50 }) },
        { "BMP", bmp },
        { "KTX2", ktx2(8, 4, 3, 2, 1, true, pattern, [](ktxTexture2*) {}) },
        { "cube KTX2", ktx2(4, 4, 1, 1, 6, false, pattern, [](ktxTexture2*) {}) },
        { "Zstd KTX2", ktx2(8, 8, 2, 1, 1, false, pattern, zstd) },
        { "UASTC KTX2", ktx2(8, 8, 1, 1, 1, false, pattern, basis) },
    };
    std::mt19937 random(20261106);
    for (const auto& [kind, seed] : seeds) {
        CAPTURE(kind);
        // The seed itself decodes.
        whole(decoded(seed));
        int refused = 0;
        for (int round = 0; round < rounds; ++round) {
            const bytes_t bytes = mutated(seed, random);
            CAPTURE(round);
            const auto transcode = static_cast<assets::transcode_t>(round % 3);
            const assets::result_t<assets::image_t> image = assets::decode_image(
                bytes, { .srgb = round % 2 == 0, .transcode = transcode }, "a mutant");
            if (image) {
                whole(*image);
            } else {
                REQUIRE(image.error().failure == assets::failure_t::INVALID_DATA);
                ++refused;
            }
        }
        // Not every mutant's refused, nor every one taken, or it's not the decoder that's tried.
        CAPTURE(refused);
        CHECK(refused > 0);
        CHECK(refused < rounds);
    }
}
