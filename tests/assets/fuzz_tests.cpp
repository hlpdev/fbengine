// The loaders, given broken files: images (PNG, JPEG, KTX2 raw, Zstandard's, and Basis Universal's)
// and models (glTF and binary glTF), each made here whole and then cut short, its bytes flipped,
// its numbers made huge or zero, pieces of it repeated or taken out, from seeds that repeat; and
// the hostile files in fixtures/, the corpus the reviews found, broken the same ways. Each must
// decode or fail with an error, never crash or hang: a decode that takes more than two seconds
// fails. Under AddressSanitizer, as WSL's build has it, what reads past its end is found too.

#include "assets/image.hpp"
#include "assets/model.hpp"
#include "files.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ktx.h>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace assets = fbe::assets;
namespace vfs = fbe::vfs;
using assets::bytes_t;

/// How long a decode of a broken file may take, however broken.
constexpr auto longest = std::chrono::seconds(2);

/// A KTX2 of RGBA8, `side` square with `levels`, its texels a pattern, then as `finish` leaves it.
template <typename Finish>
bytes_t ktx2(std::uint32_t side, std::uint32_t levels, Finish finish) {
    ktxTextureCreateInfo info{};
    info.vkFormat = assets::formats::rgba8;
    info.baseWidth = side;
    info.baseHeight = side;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = levels;
    info.numLayers = 1;
    info.numFaces = 1;
    info.isArray = KTX_FALSE;
    info.generateMipmaps = KTX_FALSE;
    ktxTexture2* made = nullptr;
    REQUIRE(ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &made) == KTX_SUCCESS);
    const std::unique_ptr<ktxTexture2, decltype(&ktxTexture2_Destroy)> texture(
        made, &ktxTexture2_Destroy);
    for (std::uint32_t level = 0; level < levels; ++level) {
        const std::uint32_t across = std::max(side >> level, 1U);
        std::vector<std::uint8_t> texels;
        for (std::uint32_t at = 0; at < across * across; ++at) {
            texels.insert(texels.end(),
                          { static_cast<std::uint8_t>(at * 7), static_cast<std::uint8_t>(at * 13),
                            static_cast<std::uint8_t>(level * 50), 255 });
        }
        REQUIRE(ktxTexture_SetImageFromMemory(reinterpret_cast<ktxTexture*>(texture.get()), level,
                                              0, 0, texels.data(), texels.size()) == KTX_SUCCESS);
    }
    finish(texture.get());
    ktx_uint8_t* bytes = nullptr;
    ktx_size_t size = 0;
    REQUIRE(ktxTexture2_WriteToMemory(texture.get(), &bytes, &size) == KTX_SUCCESS);
    bytes_t written(bytes, bytes + size);
    std::free(bytes); // NOLINT(cppcoreguidelines-no-malloc): libktx's memory is malloc's.
    return written;
}

/// The images each loader starts from, whole, all but the last of which decode: a KTX2 of ETC1S,
/// which the loader doesn't take, but whose header it reads.
std::vector<bytes_t> image_seeds() {
    std::vector<std::uint8_t> texels;
    for (int at = 0; at < 8 * 8 * 4; ++at) {
        texels.push_back(static_cast<std::uint8_t>(at * 11));
    }
    std::vector<std::uint16_t> wide;
    for (int at = 0; at < 4 * 4 * 4; ++at) {
        wide.push_back(static_cast<std::uint16_t>(at * 997));
    }
    return {
        test::png(8, 8, 4, texels),
        test::png(8, 8, 1, std::vector<std::uint8_t>(texels.begin(), texels.begin() + 64)),
        test::png16(4, 4, wide),
        test::jpeg(16, 16, { 200, 120, 40 }),
        ktx2(8, 4, [](ktxTexture2*) {}),
        ktx2(8, 4,
             [](ktxTexture2* texture) {
                 REQUIRE(ktxTexture2_DeflateZstd(texture, 5) == KTX_SUCCESS);
             }),
        ktx2(16, 1,
             [](ktxTexture2* texture) {
                 ktxBasisParams params{};
                 params.structSize = sizeof(params);
                 params.uastc = KTX_TRUE;
                 params.threadCount = 1;
                 REQUIRE(ktxTexture2_CompressBasisEx(texture, &params) == KTX_SUCCESS);
             }),
        ktx2(16, 1,
             [](ktxTexture2* texture) {
                 ktxBasisParams params{};
                 params.structSize = sizeof(params);
                 params.uastc = KTX_FALSE;
                 params.threadCount = 1;
                 REQUIRE(ktxTexture2_CompressBasisEx(texture, &params) == KTX_SUCCESS);
             }),
    };
}

/// A glTF with something of every part the loader reads: a mesh of positions, normals, UVs, joints,
/// weights, and indices, a morph target, a material with a texture, nodes in a tree, a skin, an
/// animation, and a sparse accessor; its buffer and image as data URIs, or, binary, its own.
bytes_t model_seed(bool binary) {
    bytes_t buffer;
    // 3 positions, normals, UVs, joints, weights; 3 indices; a target's 3 offsets; 2 key times
    // and 2 rotations; one inverse bind matrix; a sparse accessor's index and value.
    test::append<float>(buffer, { 0, 0, 0, 1, 0, 0, 0, 1, 0 });
    test::append<float>(buffer, { 0, 0, 1, 0, 0, 1, 0, 0, 1 });
    test::append<float>(buffer, { 0, 0, 1, 0, 0, 1 });
    test::append<std::uint8_t>(buffer, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
    test::append<float>(buffer, { 1, 0, 0, 0, 0.5F, 0.5F, 0, 0, 1, 0, 0, 0 });
    test::append<std::uint16_t>(buffer, { 0, 1, 2, 0 });
    test::append<float>(buffer, { 0, 0, 0.1F, 0, 0, 0.1F, 0, 0, 0.1F });
    test::append<float>(buffer, { 0, 1 });
    test::append<float>(buffer, { 0, 0, 0, 1, 0, 0.7071F, 0, 0.7071F });
    test::append<float>(buffer, { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 });
    test::append<std::uint16_t>(buffer, { 1, 0 });
    test::append<float>(buffer, { 2, 2, 2 });
    std::vector<std::uint8_t> texels(4 * 4 * 4, 200);
    const bytes_t image = test::png(4, 4, 4, texels);
    const std::string views =
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
        R"({"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24},)"
        R"({"buffer":0,"byteOffset":96,"byteLength":12},{"buffer":0,"byteOffset":108,"byteLength":48},)"
        R"({"buffer":0,"byteOffset":156,"byteLength":8},{"buffer":0,"byteOffset":164,"byteLength":36},)"
        R"({"buffer":0,"byteOffset":200,"byteLength":8},{"buffer":0,"byteOffset":208,"byteLength":32},)"
        R"({"buffer":0,"byteOffset":240,"byteLength":64},{"buffer":0,"byteOffset":304,"byteLength":4},)"
        R"({"buffer":0,"byteOffset":308,"byteLength":12}])";
    const std::string accessors =
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
        R"("min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},)"
        R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},)"
        R"({"bufferView":3,"componentType":5121,"count":3,"type":"VEC4"},)"
        R"({"bufferView":4,"componentType":5126,"count":3,"type":"VEC4"},)"
        R"({"bufferView":5,"componentType":5123,"count":3,"type":"SCALAR"},)"
        R"({"bufferView":6,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0,0,0.1]},)"
        R"({"bufferView":7,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},)"
        R"({"bufferView":8,"componentType":5126,"count":2,"type":"VEC4"},)"
        R"({"bufferView":9,"componentType":5126,"count":1,"type":"MAT4"},)"
        R"({"componentType":5126,"count":3,"type":"VEC3","sparse":{"count":1,)"
        R"("indices":{"bufferView":10,"componentType":5123},"values":{"bufferView":11}}}])";
    const std::string rest =
        R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2,)"
        R"("JOINTS_0":3,"WEIGHTS_0":4},"indices":5,"material":0,"targets":[{"POSITION":6}]}],)"
        R"("weights":[0.5]}],"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)"
        R"("textures":[{"source":0}],"nodes":[{"children":[1],"name":"root"},)"
        R"({"mesh":0,"skin":0,"name":"body"},{"translation":[0,0,0]}],)"
        R"("skins":[{"joints":[2],"inverseBindMatrices":9}],)"
        R"("animations":[{"channels":[{"sampler":0,"target":{"node":2,"path":"rotation"}}],)"
        R"("samplers":[{"input":7,"output":8}]}],"scenes":[{"nodes":[0]}],"scene":0})";
    if (binary) {
        bytes_t both = buffer;
        both.resize((both.size() + 3) / 4 * 4, 0);
        const std::size_t imageAt = both.size();
        both.insert(both.end(), image.begin(), image.end());
        const std::string json =
            R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)" +
            std::to_string(both.size()) + "}]," + views.substr(0, views.size() - 1) +
            R"(,{"buffer":0,"byteOffset":)" + std::to_string(imageAt) + R"(,"byteLength":)" +
            std::to_string(image.size()) + R"(}],)" + accessors +
            R"(,"images":[{"bufferView":12,"mimeType":"image/png"}])" + rest;
        return test::glb(json, both);
    }
    const std::string json =
        R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)" + std::to_string(buffer.size()) +
        R"(,"uri":"data:application/octet-stream;base64,)" + test::base64(buffer) + R"("}],)" +
        views + "," + accessors + R"(,"images":[{"uri":"data:image/png;base64,)" +
        test::base64(image) + R"("}])" + rest;
    return test::bytes(json);
}

/// The corpus: the hostile files the reviews found.
std::vector<std::pair<std::string, bytes_t>> corpus() {
    std::vector<std::pair<std::string, bytes_t>> files;
    for (const auto& entry : std::filesystem::directory_iterator(FBENGINE_ASSETS_FIXTURES)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream stream(entry.path(), std::ios::binary);
        files.emplace_back(
            entry.path().filename().string(),
            bytes_t(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()));
    }
    std::ranges::sort(files);
    return files;
}

/// `seed` broken `count` ways by `random`: cut short at its first 48 lengths and at others, a few
/// of its bytes flipped, a 4-byte number in it made 0, 1, huge, or negative, a piece of it
/// repeated or taken out, and, in text, its digits made long.
std::vector<bytes_t> broken(const bytes_t& seed, std::mt19937& random, int count) {
    std::vector<bytes_t> out;
    if (seed.empty()) {
        return out;
    }
    for (std::size_t length = 0; length < std::min<std::size_t>(seed.size(), 48); ++length) {
        out.emplace_back(seed.begin(), seed.begin() + static_cast<std::ptrdiff_t>(length));
    }
    const auto at = [&random](std::size_t size) {
        return std::uniform_int_distribution<std::size_t>(0, size - 1)(random);
    };
    constexpr std::array<std::uint32_t, 6> numbers{ 0,           1,           0x7fffffffU,
                                                    0xffffffffU, 0x80000000U, 0x10000U };
    for (int made = 0; made < count; ++made) {
        bytes_t bytes = seed;
        switch (random() % 6) {
        case 0:
            bytes.resize(at(bytes.size()));
            break;
        case 1:
            for (std::uint32_t flips = 1 + random() % 4; flips > 0; --flips) {
                bytes[at(bytes.size())] ^= static_cast<std::uint8_t>(1U << (random() % 8));
            }
            break;
        case 2:
            if (bytes.size() >= 4) {
                const std::size_t from = at(bytes.size() - 3) & ~std::size_t{ 3 };
                const std::uint32_t number = numbers.at(random() % numbers.size());
                for (std::size_t byte = 0; byte < 4 && from + byte < bytes.size(); ++byte) {
                    bytes[from + byte] = static_cast<std::uint8_t>(number >> (8U * byte));
                }
            }
            break;
        case 3: {
            const std::size_t from = at(bytes.size());
            const std::size_t length =
                std::min<std::size_t>(1 + random() % 64, bytes.size() - from);
            const bytes_t piece(bytes.begin() + static_cast<std::ptrdiff_t>(from),
                                bytes.begin() + static_cast<std::ptrdiff_t>(from + length));
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(from), piece.begin(),
                         piece.end());
            break;
        }
        case 4: {
            const std::size_t from = at(bytes.size());
            const std::size_t length =
                std::min<std::size_t>(1 + random() % 64, bytes.size() - from);
            bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(from),
                        bytes.begin() + static_cast<std::ptrdiff_t>(from + length));
            break;
        }
        default:
            for (int tries = 0; tries < 8; ++tries) {
                const std::size_t where = at(bytes.size());
                if (bytes[where] >= '0' && bytes[where] <= '9') {
                    const std::string_view longer = random() % 2 == 0 ? "99999999999" : "-1";
                    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(where), longer.begin(),
                                 longer.end());
                    break;
                }
            }
            break;
        }
        out.push_back(std::move(bytes));
    }
    return out;
}

vfs::path_t path(std::string_view text) {
    return *vfs::parse(text);
}

/// Keeps what's about to be decoded in the file FBENGINE_FUZZ_LAST names, if it names one, so a
/// decode that hangs leaves what it hung on.
void about_to(const bytes_t& bytes) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): the tests read the environment on one thread.
    const char* last = std::getenv("FBENGINE_FUZZ_LAST");
    if (last == nullptr) {
        return;
    }
    std::ofstream stream(last, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

/// Decodes `bytes` as an image every way, timed: it must decode or fail, in time.
void image_survives(const bytes_t& bytes) {
    about_to(bytes);
    for (const assets::transcode_t transcode :
         { assets::transcode_t::RGBA8, assets::transcode_t::BC7, assets::transcode_t::ASTC_4X4 }) {
        const auto started = std::chrono::steady_clock::now();
        const assets::result_t<assets::image_t> image =
            assets::decode_image(bytes, { .srgb = true, .transcode = transcode }, "fuzz");
        REQUIRE(std::chrono::steady_clock::now() - started < longest);
        if (image.has_value()) {
            // What it says it has is what it has.
            for (const assets::level_t& level : image->levels) {
                REQUIRE(level.offset + level.size * image->layers * image->faces <=
                        image->texels.size());
            }
        }
    }
}

/// Decodes `bytes` as a model at `at`, timed: it must decode or fail, in time.
void model_survives(const bytes_t& bytes, const vfs::path_t& at, const vfs::view_t& files) {
    about_to(bytes);
    const auto started = std::chrono::steady_clock::now();
    const assets::result_t<assets::model_t> model = assets::decode_model(at, bytes, files, {});
    REQUIRE(std::chrono::steady_clock::now() - started < longest);
    (void)model;
}

} // namespace

TEST_CASE("broken images fail, never crash or hang", "[assets][fuzz]") {
    std::mt19937 random(20261106);
    const std::vector<bytes_t> seeds = image_seeds();
    for (std::size_t at = 0; at < seeds.size(); ++at) {
        const bytes_t& seed = seeds[at];
        INFO(at);
        REQUIRE(assets::decode_image(seed, {}, "seed").has_value() == (at + 1 < seeds.size()));
        for (const bytes_t& bytes : broken(seed, random, 2000)) {
            image_survives(bytes);
        }
    }
}

// The fuzzer's find: a KTX2 whose data format descriptor has a block of size 0, which libktx walks
// forever; and one whose descriptor says it's longer than the header does, which libktx reads past.
TEST_CASE("a KTX2 whose format descriptor is the wrong size fails in time", "[assets][fuzz]") {
    std::ifstream stream(std::filesystem::path(FBENGINE_ASSETS_FIXTURES) / "zstd_hang.ktx2",
                         std::ios::binary);
    const bytes_t hung{ std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
    REQUIRE(hung.size() == 651);
    image_survives(hung);
    const assets::result_t<assets::image_t> image = assets::decode_image(hung, {}, "hung");
    REQUIRE_FALSE(image.has_value());
    REQUIRE(image.error().failure == assets::failure_t::INVALID_DATA);

    bytes_t longer = ktx2(8, 4, [](ktxTexture2*) {});
    REQUIRE(assets::decode_image(longer, {}, "seed").has_value());
    // The descriptor's own size, at the offset the header gives at byte 48, made 4 more.
    const std::size_t descriptor = longer[48] | (longer[49] << 8U);
    longer[descriptor] = static_cast<std::uint8_t>(longer[descriptor] + 4);
    const assets::result_t<assets::image_t> past = assets::decode_image(longer, {}, "longer");
    REQUIRE_FALSE(past.has_value());
    REQUIRE(past.error().failure == assets::failure_t::INVALID_DATA);
}

TEST_CASE("broken models fail, never crash or hang", "[assets][fuzz]") {
    vfs::vfs_t files;
    files.mount(path("res://models"), vfs::folder(FBENGINE_ASSETS_FIXTURES));
    std::mt19937 random(20261107);
    for (const bool binary : { false, true }) {
        INFO(binary);
        const bytes_t seed = model_seed(binary);
        const vfs::path_t at = path(binary ? "res://models/fuzz.glb" : "res://models/fuzz.gltf");
        const assets::result_t<assets::model_t> whole =
            assets::decode_model(at, seed, files.view(), {});
        INFO((whole.has_value() ? std::string() : whole.error().message));
        REQUIRE(whole.has_value());
        for (const bytes_t& bytes : broken(seed, random, 10000)) {
            model_survives(bytes, at, files.view());
        }
    }
}

TEST_CASE("the corpus, broken further, fails, never crashing or hanging", "[assets][fuzz]") {
    vfs::vfs_t files;
    files.mount(path("res://models"), vfs::folder(FBENGINE_ASSETS_FIXTURES));
    std::mt19937 random(20261108);
    for (const auto& [name, seed] : corpus()) {
        INFO(name);
        const bool model = name.ends_with(".gltf") || name.ends_with(".glb");
        const vfs::path_t at = path("res://models/" + name);
        for (const bytes_t& bytes : broken(seed, random, 300)) {
            if (model) {
                model_survives(bytes, at, files.view());
            } else {
                image_survives(bytes);
            }
        }
    }
}
