// The model cooker: glTFs cooked into binary glTFs whose triangles and vertices meshoptimizer has
// ordered, which the engine decodes to the same triangles, each vertex's attributes and morph
// targets kept together; and what the engine wouldn't load, which isn't cooked.

#include "../assets/files.hpp"
#include "assets/model.hpp"
#include "cook/cook.hpp"
#include "vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <meshoptimizer.h>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {

namespace assets = fbe::assets;
namespace tools = fbe::tools;
namespace vfs = fbe::vfs;
using test::bytes_t;

constexpr int side = 8;

// A grid of `side` by `side` quads, its vertices shuffled, and one more that no triangle uses: its
// positions, UVs that are the positions' x and y, and a morph target that moves each vertex by
// half its position; its indices, and the glTF of it, its buffer in grid.bin, with a texture.
struct grid_t {
    grid_t() {
        const std::size_t vertices = static_cast<std::size_t>((side + 1) * (side + 1)) + 1;
        std::vector<std::uint32_t> order(vertices);
        std::ranges::iota(order, 0U);
        // Seeded, so each run shuffles the same.
        std::mt19937 random(7); // NOLINT(bugprone-random-generator-seed)
        std::ranges::shuffle(order, random);
        // Where each grid vertex went.
        std::vector<std::uint32_t> at(vertices);
        for (std::uint32_t i = 0; i < vertices; ++i) {
            at[order[i]] = i;
        }
        positions.resize(vertices);
        for (std::uint32_t v = 0; v < vertices; ++v) {
            const std::uint32_t grid = order[v];
            const std::uint32_t row = grid / (side + 1);
            const auto x = static_cast<float>(grid % (side + 1));
            const auto y = static_cast<float>(row);
            positions[v] = { x, y, grid + 1 == vertices ? 5.0F : 0.0F };
        }
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const auto corner = [&](int dx, int dy) {
                    return at[(static_cast<std::size_t>(y + dy) * (side + 1)) +
                              static_cast<std::size_t>(x + dx)];
                };
                indices.insert(indices.end(), {
                                                  corner(0, 0),
                                                  corner(1, 0),
                                                  corner(1, 1),
                                                  corner(0, 0),
                                                  corner(1, 1),
                                                  corner(0, 1),
                                              });
            }
        }
    }

    [[nodiscard]] std::string json(bool with_image) const {
        bytes_t buffer;
        for (const assets::vec3_t& p : positions) {
            test::append<float>(buffer, { p[0], p[1], p[2] });
        }
        const std::size_t uv_at = buffer.size();
        for (const assets::vec3_t& p : positions) {
            test::append<float>(buffer, { p[0], p[1] });
        }
        const std::size_t target_at = buffer.size();
        for (const assets::vec3_t& p : positions) {
            test::append<float>(buffer, { p[0] / 2, p[1] / 2, 0 });
        }
        // Normals, so the engine doesn't take its vertices apart for flat ones.
        const std::size_t normal_at = buffer.size();
        for (std::size_t v = 0; v < positions.size(); ++v) {
            test::append<float>(buffer, { 0, 0, 1 });
        }
        const std::size_t index_at = buffer.size();
        for (const std::uint32_t index : indices) {
            test::append<std::uint16_t>(buffer, { static_cast<std::uint16_t>(index) });
        }
        bin = buffer;
        const std::string count = std::to_string(positions.size());
        const auto view = [](std::size_t offset, std::size_t length) {
            return R"({"buffer":0,"byteOffset":)" + std::to_string(offset) + R"(,"byteLength":)" +
                   std::to_string(length) + "}";
        };
        std::string text =
            R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)" +
            std::to_string(buffer.size()) + R"(,"uri":"grid.bin"}],"bufferViews":[)" +
            view(0, uv_at) + "," + view(uv_at, target_at - uv_at) + "," +
            view(target_at, normal_at - target_at) + "," + view(normal_at, index_at - normal_at) +
            "," + view(index_at, buffer.size() - index_at) +
            R"(],"accessors":[{"bufferView":0,"componentType":5126,"count":)" + count +
            R"(,"type":"VEC3","min":[0,0,0],"max":[8,8,5]},)"
            R"({"bufferView":1,"componentType":5126,"count":)" +
            count +
            R"(,"type":"VEC2"},)"
            R"({"bufferView":2,"componentType":5126,"count":)" +
            count +
            R"(,"type":"VEC3","min":[0,0,0],"max":[4,4,0]},)"
            R"({"bufferView":3,"componentType":5126,"count":)" +
            count +
            R"(,"type":"VEC3"},)"
            R"({"bufferView":4,"componentType":5123,"count":)" +
            std::to_string(indices.size()) +
            R"(,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,)"
            R"("NORMAL":3,"TEXCOORD_0":1},"indices":4,"targets":[{"POSITION":2}])";
        if (with_image) {
            text += R"(,"material":0}],"weights":[0]}],"materials":[{"pbrMetallicRoughness":)"
                    R"({"baseColorTexture":{"index":0}}}],"textures":[{"source":0}],)"
                    R"("images":[{"uri":"tex.png"}])";
        } else {
            text += R"(}],"weights":[0]}])";
        }
        return text + R"(,"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}]})";
    }

    std::vector<assets::vec3_t> positions;
    std::vector<std::uint32_t> indices;
    mutable bytes_t bin;
};

vfs::path_t path(std::string_view text) {
    return *vfs::parse(text);
}

// Each triangle as its corners' positions, from its least corner on, so turned the same way,
// and sorted.
std::vector<std::array<assets::vec3_t, 3>> triangles(const std::vector<assets::vec3_t>& positions,
                                                     const std::vector<std::uint32_t>& indices) {
    std::vector<std::array<assets::vec3_t, 3>> made;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        std::array<assets::vec3_t, 3> corners = {
            positions[indices[i]],
            positions[indices[i + 1]],
            positions[indices[i + 2]],
        };
        std::ranges::rotate(corners, std::ranges::min_element(corners));
        made.push_back(corners);
    }
    std::ranges::sort(made);
    return made;
}

float acmr(const std::vector<std::uint32_t>& indices, std::size_t vertices) {
    return meshopt_analyzeVertexCache(indices.data(), indices.size(), vertices, 16, 0, 0).acmr;
}

struct cooked_t {
    tools::bytes_t bytes;
    assets::model_t model;
};

cooked_t cook(const grid_t& grid, bool optimize, bool with_image = true) {
    const test::temp_folder_t folder;
    folder.write("grid.gltf", grid.json(with_image));
    folder.write("grid.bin", grid.bin);
    folder.write("tex.png", test::png(1, 1, 4, { 9, 9, 9, 255 }));
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    const vfs::bytes_t source = *files.read(path("res://grid.gltf"));
    std::expected<tools::bytes_t, tools::error_t> bytes =
        tools::cook_model(source, path("res://grid.gltf"), files.view(), { .optimize = optimize });
    if (!bytes) {
        FAIL(bytes.error().message);
    }
    // The engine decodes it at the same path, beside its texture.
    folder.write("grid.gltf", *bytes);
    std::filesystem::remove(folder.path / "grid.bin");
    assets::result_t<assets::model_t> model =
        assets::decode_model(path("res://grid.gltf"), *bytes, files.view(), {});
    if (!model) {
        FAIL(model.error().message);
    }
    return { .bytes = std::move(*bytes), .model = std::move(*model) };
}

} // namespace

TEST_CASE("a cooked glTF is a binary glTF of the same triangles, ordered for the GPU", "[tools]") {
    const grid_t grid;
    const cooked_t cooked = cook(grid, true);
    REQUIRE(cooked.bytes.size() > 4);
    REQUIRE(std::string(cooked.bytes.begin(), cooked.bytes.begin() + 4) == "glTF");
    const assets::primitive_t& primitive = cooked.model.meshes.at(0).primitives.at(0);
    // The unused vertex is gone.
    REQUIRE(primitive.positions.size() == grid.positions.size() - 1);
    REQUIRE(primitive.indices.size() == grid.indices.size());
    REQUIRE(triangles(primitive.positions, primitive.indices) ==
            triangles(grid.positions, grid.indices));
    // Each vertex's UV and morph target went with it.
    for (std::size_t v = 0; v < primitive.positions.size(); ++v) {
        const assets::vec3_t& p = primitive.positions[v];
        REQUIRE(primitive.uv0[v] == assets::vec2_t{ p[0], p[1] });
        REQUIRE(primitive.targets.at(0).positions[v] == assets::vec3_t{ p[0] / 2, p[1] / 2, 0 });
        REQUIRE(primitive.normals[v] == assets::vec3_t{ 0, 0, 1 });
    }
    REQUIRE(acmr(primitive.indices, primitive.positions.size()) <
            acmr(grid.indices, grid.positions.size()));
    // The texture's still named, and the material still uses it.
    REQUIRE(cooked.model.materials.at(0).base_color_texture.image == 0);
    REQUIRE(cooked.model.images.at(0).width == 1);
}

TEST_CASE("a glTF cooked without optimizing keeps its order, in a binary glTF", "[tools]") {
    const grid_t grid;
    const cooked_t cooked = cook(grid, false, false);
    REQUIRE(std::string(cooked.bytes.begin(), cooked.bytes.begin() + 4) == "glTF");
    const assets::primitive_t& primitive = cooked.model.meshes.at(0).primitives.at(0);
    REQUIRE(primitive.positions == grid.positions);
    REQUIRE(primitive.indices == grid.indices);
}

TEST_CASE("a glTF the engine wouldn't load isn't cooked", "[tools]") {
    grid_t grid;
    grid.indices.back() = static_cast<std::uint32_t>(grid.positions.size());
    const test::temp_folder_t folder;
    folder.write("grid.gltf", grid.json(false));
    folder.write("grid.bin", grid.bin);
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    const vfs::bytes_t source = *files.read(path("res://grid.gltf"));
    const std::expected<tools::bytes_t, tools::error_t> bytes =
        tools::cook_model(source, path("res://grid.gltf"), files.view(), { .optimize = true });
    REQUIRE_FALSE(bytes.has_value());
    REQUIRE(bytes.error().failure == tools::failure_t::INVALID_DATA);
    REQUIRE(bytes.error().message.contains("index past its vertices"));
    REQUIRE_FALSE(
        tools::cook_model(test::bytes("not a glTF"), path("res://x.gltf"), files.view(), {})
            .has_value());
}

namespace {

bytes_t fixture(std::string_view name) {
    std::ifstream stream(std::filesystem::path(FBENGINE_ASSETS_FIXTURES) / name, std::ios::binary);
    REQUIRE(stream.good());
    return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}

} // namespace

TEST_CASE("a glTF's images in its buffers cook into KTX2s it names through KHR_texture_basisu",
          "[tools]") {
    // A base color texture, sRGB, and a normal map, linear, each a data URI's PNG.
    const std::string red = test::base64(test::png(4, 4, 4, std::vector<std::uint8_t>(64, 200)));
    const grid_t grid;
    std::string json = grid.json(false);
    json.replace(json.rfind(R"(}],"weights")"), 2, R"(,"material":0}])");
    json.pop_back();
    json += R"(,"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}},)"
            R"("normalTexture":{"index":1}}],"textures":[{"source":0},{"source":1}],)"
            R"("images":[{"uri":"data:image/png;base64,)" +
            red +
            R"("},)"
            R"({"uri":"data:image/png;base64,)" +
            red + R"("}]})";
    const test::temp_folder_t folder;
    folder.write("grid.gltf", json);
    folder.write("grid.bin", grid.bin);
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    const fbe_cook_model_t options{
        .optimize = true,
        .images = { .format = FBE_COOK_BC7, .srgb = true, .mipmaps = true, .quality = 0 },
    };
    const std::expected<tools::bytes_t, tools::error_t> bytes = tools::cook_model(
        *files.read(path("res://grid.gltf")), path("res://grid.gltf"), files.view(), options);
    if (!bytes) {
        FAIL(bytes.error().message);
    }
    const std::string text(bytes->begin(), bytes->end());
    REQUIRE(text.contains("KHR_texture_basisu"));
    const assets::result_t<assets::model_t> model =
        assets::decode_model(path("res://cooked.glb"), *bytes, vfs::view_t(), {});
    if (!model) {
        FAIL(model.error().message);
    }
    REQUIRE(model->images.size() == 2);
    REQUIRE(model->images[0].format == assets::formats::bc7_srgb);
    REQUIRE(model->images[0].levels.size() == 3);
    REQUIRE(model->images[1].format == assets::formats::bc7);
    REQUIRE(model->materials.at(0).base_color_texture.image == 0);
    REQUIRE(model->materials.at(0).normal_texture.image == 1);

    // One that won't decode stops the cook.
    std::string broken = json;
    broken.replace(broken.find(red), red.size(), test::base64(test::bytes("not an image at all!")));
    folder.write("broken.gltf", broken);
    const std::expected<tools::bytes_t, tools::error_t> refused = tools::cook_model(
        *files.read(path("res://broken.gltf")), path("res://broken.gltf"), files.view(), options);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().failure == tools::failure_t::INVALID_DATA);
}

TEST_CASE("a texture with a KTX2 of its own keeps it, losing only its fallback", "[tools]") {
    // Its fallback a 4 by 4 PNG, and its own a KTX2 of an 8 by 8 one.
    const fbe_cook_image_t image{
        .format = FBE_COOK_BC7,
        .srgb = true,
        .mipmaps = false,
        .quality = 0,
    };
    const std::expected<tools::bytes_t, tools::error_t> own =
        tools::cook_image(test::png(8, 8, 4, std::vector<std::uint8_t>(256, 50)), image);
    if (!own) {
        FAIL(own.error().message);
    }
    const grid_t grid;
    std::string json = grid.json(false);
    json.replace(json.rfind(R"(}],"weights")"), 2, R"(,"material":0}])");
    json.pop_back();
    json += R"(,"extensionsUsed":["KHR_texture_basisu"],)"
            R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)"
            R"("textures":[{"source":0,"extensions":{"KHR_texture_basisu":{"source":1}}}],)"
            R"("images":[{"uri":"data:image/png;base64,)" +
            test::base64(test::png(4, 4, 4, std::vector<std::uint8_t>(64, 200))) +
            R"("},{"uri":"data:image/ktx2;base64,)" + test::base64(*own) + R"("}]})";
    const test::temp_folder_t folder;
    folder.write("grid.gltf", json);
    folder.write("grid.bin", grid.bin);
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    const fbe_cook_model_t options{ .optimize = true, .images = image };
    const std::expected<tools::bytes_t, tools::error_t> bytes = tools::cook_model(
        *files.read(path("res://grid.gltf")), path("res://grid.gltf"), files.view(), options);
    if (!bytes) {
        FAIL(bytes.error().message);
    }
    const assets::result_t<assets::model_t> model =
        assets::decode_model(path("res://cooked.glb"), *bytes, vfs::view_t(), {});
    if (!model) {
        FAIL(model.error().message);
    }
    REQUIRE(model->materials.at(0).base_color_texture.image == 1);
    REQUIRE(model->images.at(1).width == 8);
}

TEST_CASE("a cooked glTF keeps its clips' frame rates", "[tools]") {
    vfs::vfs_t files;
    files.mount(path("res://models"), vfs::folder(FBENGINE_ASSETS_FIXTURES));
    const vfs::bytes_t source = fixture("clip_rate.gltf");
    const assets::result_t<assets::model_t> before =
        assets::decode_model(path("res://models/clip_rate.gltf"), source, files.view(), {});
    if (!before) {
        FAIL(before.error().message);
    }
    REQUIRE(before->clips.at(0).frame_rate == 30);
    const std::expected<tools::bytes_t, tools::error_t> bytes = tools::cook_model(
        source, path("res://models/clip_rate.gltf"), files.view(), { .optimize = true });
    if (!bytes) {
        FAIL(bytes.error().message);
    }
    const assets::result_t<assets::model_t> after =
        assets::decode_model(path("res://models/cooked.glb"), *bytes, files.view(), {});
    if (!after) {
        FAIL(after.error().message);
    }
    REQUIRE(after->clips.at(0).frame_rate == 30);
}
