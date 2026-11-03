// A model load's clips and skins as the clip and skin areas' own, through the C API: each node by
// its name, played and skinned on entities named as the model's nodes, and what's refused.

#include "fbengine.h"

#include <array>
#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

bool says(std::string_view message) {
    return std::string_view(fbe_error()).contains(message);
}

/// A rig's glTF.
constexpr std::string_view whole = R"({
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": 216, "uri": "rig.bin"}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 8},
                        {"buffer": 0, "byteOffset": 8, "byteLength": 24},
                        {"buffer": 0, "byteOffset": 32, "byteLength": 32},
                        {"buffer": 0, "byteOffset": 64, "byteLength": 24},
                        {"buffer": 0, "byteOffset": 88, "byteLength": 128}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR",
                       "min": [0], "max": [1]},
                      {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5126, "count": 2, "type": "VEC4"},
                      {"bufferView": 3, "componentType": 5126, "count": 2, "type": "VEC3"},
                      {"bufferView": 4, "componentType": 5126, "count": 2, "type": "MAT4"}],
        "nodes": [{"name": "hips", "children": [1]}, {"name": "knee"}, {}],
        "scenes": [{"nodes": [0, 2]}],
        "skins": [{"joints": [0, 1], "inverseBindMatrices": 4}],
        "animations": [{"name": "walk", "extras": {"frameRate": 24},
                        "samplers": [{"input": 0, "output": 1},
                                     {"input": 0, "output": 2, "interpolation": "STEP"},
                                     {"input": 0, "output": 3}],
                        "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}},
                                     {"sampler": 1, "target": {"node": 1, "path": "rotation"}},
                                     {"sampler": 2, "target": {"node": 2, "path": "scale"}}]}]
    })";

/// A rig's glTF and its buffer, in a folder of its own named `name`, mounted at res://anim-rig:
/// nodes "hips", with "knee" under it, and one with no name; a skin of the hips and knee; and a
/// clip "walk", at 24 frames a second, its keys at `times`, moving the hips from x 0 to 2,
/// stepping the knee to a half turn about Y, and scaling the unnamed node.
class rig_t {
public:
    explicit rig_t(std::string_view name, std::array<float, 2> times = { 0.0F, 1.0F },
                   std::string_view file = whole)
        : m_folder(std::filesystem::temp_directory_path() / name) {
        std::filesystem::create_directories(m_folder);
        std::vector<float> floats(times.begin(), times.end());
        floats.insert(floats.end(), { 0, 0, 0, 2, 0, 0 });
        floats.insert(floats.end(), { 0, 0, 0, 1, 0, 1, 0, 0 });
        floats.insert(floats.end(), { 1, 1, 1, 3, 3, 3 });
        floats.insert(floats.end(), { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 });
        floats.insert(floats.end(), { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1 });
        std::ofstream bin(m_folder / "rig.bin", std::ios::binary);
        for (const float value : floats) {
            const auto bits = std::bit_cast<std::array<char, 4>>(value);
            bin.write(bits.data(), bits.size());
        }
        bin.close();
        std::ofstream(m_folder / "rig.gltf") << file;
        fbe_vfs_mount_folder("res://anim-rig", m_folder.string().c_str());
    }

    rig_t(const rig_t&) = delete;
    rig_t& operator=(const rig_t&) = delete;
    rig_t(rig_t&&) = delete;
    rig_t& operator=(rig_t&&) = delete;

    // Only remove_all's allocations can throw, which end the tests.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~rig_t() {
        fbe_vfs_unmount("res://anim-rig");
        std::error_code ignored;
        std::filesystem::remove_all(m_folder, ignored);
    }

private:
    std::filesystem::path m_folder;
};

std::uint64_t pivot(std::uint64_t world, std::uint64_t parent, const char* name) {
    const std::uint64_t made = fbe_entity_create(world, parent, FBE_ENTITY_PIVOT);
    fbe_entity_set_name(made, name);
    return made;
}

} // namespace

TEST_CASE("a model's clip plays on entities named as its nodes") {
    const rig_t rig("fbengine-anim-plays");
    const std::uint64_t load =
        fbe_assets_load(FBE_ASSETS_MODEL, "res://anim-rig/rig.gltf", false, false);
    REQUIRE(load != 0);
    REQUIRE(!fbe_assets_failed(load));
    const std::uint64_t clip = fbe_clip_from_model(load, 0);
    REQUIRE(clip != 0);
    CHECK(std::string_view(fbe_clip_name(clip)) == "walk");
    CHECK(fbe_clip_frame_rate(clip) == 24.0F);
    CHECK(fbe_clip_length(clip) == 1.0F);
    CHECK(fbe_clip_channel_count(clip) == 3);
    // It lives on its own once the load's gone.
    fbe_assets_free(load);
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t hips = pivot(world, 0, "hips");
    const std::uint64_t knee = pivot(world, hips, "knee");
    const std::uint64_t player = fbe_player_create(hips);
    REQUIRE(fbe_player_play(player, 0, clip, FBE_PLAYER_ONCE, 1.0F));
    fbe_player_set_time(player, 0, 0.5F);
    fbe_vec3_t at{};
    fbe_entity_position(hips, FBE_SPACE_LOCAL, &at);
    CHECK(at.x == Catch::Approx(1.0F));
    fbe_quat_t turned{};
    fbe_entity_rotation(knee, FBE_SPACE_LOCAL, &turned);
    CHECK(turned.w == 1.0F);
    fbe_player_set_time(player, 0, 1.0F);
    fbe_entity_rotation(knee, FBE_SPACE_LOCAL, &turned);
    CHECK(turned.y == 1.0F);
    fbe_player_free(player);
    fbe_clip_release(clip);
    fbe_world_free(world);
}

TEST_CASE("a model's skin binds its joints by their nodes' names") {
    const rig_t rig("fbengine-anim-skin");
    const std::uint64_t load =
        fbe_assets_load(FBE_ASSETS_MODEL, "res://anim-rig/rig.gltf", false, false);
    const std::uint64_t skin = fbe_skin_from_model(load, 0);
    REQUIRE(skin != 0);
    CHECK(fbe_skin_joint_count(skin) == 2);
    CHECK(std::string_view(fbe_skin_joint_name(skin, 0)) == "hips");
    CHECK(std::string_view(fbe_skin_joint_name(skin, 1)) == "knee");
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t body = pivot(world, 0, "body");
    const std::uint64_t mesh = fbe_entity_create(world, body, FBE_ENTITY_MESH);
    const std::uint64_t hips = pivot(world, body, "hips");
    const std::uint64_t knee = pivot(world, hips, "knee");
    fbe_entity_set_position(knee, 0.0F, 1.0F, 0.0F, FBE_SPACE_LOCAL);
    REQUIRE(fbe_skin_attach(mesh, skin, body));
    std::array<float, 32> palette{};
    REQUIRE(fbe_skin_palette(mesh, palette.data(), 2) == 2);
    // The knee bound one unit up, where it is: nothing moves.
    CHECK(palette[16 + 13] == Catch::Approx(0.0F).margin(1e-6));
    fbe_skin_release(skin);
    fbe_assets_free(load);
    fbe_world_free(world);
}

TEST_CASE("a model's clip or skin that isn't there, or a load that isn't a model's, is refused") {
    const rig_t rig("fbengine-anim-refused");
    const std::uint64_t load =
        fbe_assets_load(FBE_ASSETS_MODEL, "res://anim-rig/rig.gltf", false, false);
    CHECK(fbe_clip_from_model(load, 1) == 0);
    CHECK(says("the model has 1 clips; there's no clip 1"));
    CHECK(fbe_clip_from_model(load, -1) == 0);
    CHECK(fbe_skin_from_model(load, 1) == 0);
    CHECK(says("the model has 1 skins; there's no skin 1"));
    const std::uint64_t bytes =
        fbe_assets_load(FBE_ASSETS_BYTES, "res://anim-rig/rig.gltf", false, false);
    CHECK(fbe_clip_from_model(bytes, 0) == 0);
    CHECK(says("isn't a model load done and loaded"));
    CHECK(fbe_skin_from_model(bytes, 0) == 0);
    const std::uint64_t missing =
        fbe_assets_load(FBE_ASSETS_MODEL, "res://anim-rig/none.gltf", false, false);
    CHECK(fbe_clip_from_model(missing, 0) == 0);
    fbe_assets_free(load);
    CHECK(fbe_clip_from_model(load, 0) == 0);
    fbe_assets_free(bytes);
    fbe_assets_free(missing);
}

TEST_CASE("a model's clip with keys a clip can't sample is refused, as one made in code is") {
    const rig_t rig("fbengine-anim-falling", { 1.0F, 0.5F });
    const std::uint64_t load =
        fbe_assets_load(FBE_ASSETS_MODEL, "res://anim-rig/rig.gltf", false, false);
    REQUIRE(!fbe_assets_failed(load));
    CHECK(fbe_clip_from_model(load, 0) == 0);
    CHECK(says("0.5 isn't after 1"));
    fbe_assets_free(load);
}

TEST_CASE("what's wrong in a model's file is said apart from a mistake in asking for it") {
    const rig_t rig("fbengine-anim-kinds");
    const std::uint64_t load =
        fbe_assets_load(FBE_ASSETS_MODEL, "res://anim-rig/rig.gltf", false, false);
    REQUIRE(fbe_clip_from_model(load, 0) != 0);
    CHECK(!fbe_clip_model_invalid());
    CHECK(fbe_clip_from_model(load, 7) == 0);
    CHECK(!fbe_clip_model_invalid());
    CHECK(fbe_skin_from_model(load, 7) == 0);
    CHECK(!fbe_skin_model_invalid());
    fbe_assets_free(load);
    const rig_t falling("fbengine-anim-kinds-falling", { 1.0F, 0.5F });
    const std::uint64_t bad =
        fbe_assets_load(FBE_ASSETS_MODEL, "res://anim-rig/rig.gltf", false, false);
    CHECK(fbe_clip_from_model(bad, 0) == 0);
    CHECK(fbe_clip_model_invalid());
    fbe_assets_free(bad);
}

TEST_CASE("a model moving one name's property twice, or a skin of unnamed joints, is refused") {
    // Two nodes named "a", each moved; two named "b", one a joint, the other too; and one unnamed.
    constexpr std::string_view twice = R"({
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": 216, "uri": "rig.bin"}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 8},
                        {"buffer": 0, "byteOffset": 8, "byteLength": 24},
                        {"buffer": 0, "byteOffset": 88, "byteLength": 128}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR",
                       "min": [0], "max": [1]},
                      {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5126, "count": 2, "type": "MAT4"}],
        "nodes": [{"name": "a"}, {"name": "a"}, {}, {}],
        "skins": [{"joints": [0, 1], "inverseBindMatrices": 2},
                  {"joints": [2, 0], "inverseBindMatrices": 2}],
        "animations": [{"samplers": [{"input": 0, "output": 1}],
                        "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}},
                                     {"sampler": 0, "target": {"node": 1, "path": "translation"}}]},
                       {"samplers": [{"input": 0, "output": 1}],
                        "channels": [{"sampler": 0, "target": {"node": 2, "path": "translation"}},
                                     {"sampler": 0, "target": {"node": 3, "path": "translation"}},
                                     {"sampler": 0, "target": {"node": 0, "path": "scale"}},
                                     {"sampler": 0, "target": {"node": 1, "path": "translation"}}]}]
    })";
    const rig_t rig("fbengine-anim-twice", { 0.0F, 1.0F }, twice);
    const std::uint64_t load =
        fbe_assets_load(FBE_ASSETS_MODEL, "res://anim-rig/rig.gltf", false, false);
    REQUIRE(!fbe_assets_failed(load));
    CHECK(fbe_clip_from_model(load, 0) == 0);
    CHECK(fbe_clip_model_invalid());
    CHECK(says("moves the position of \"a\" twice"));
    // Unnamed nodes move nothing, so they're no clash, nor are a name's other properties.
    const std::uint64_t fine = fbe_clip_from_model(load, 1);
    CHECK(fine != 0);
    CHECK(!fbe_clip_model_invalid());
    fbe_clip_release(fine);
    CHECK(fbe_skin_from_model(load, 0) == 0);
    CHECK(fbe_skin_model_invalid());
    CHECK(says("two joints named \"a\""));
    CHECK(fbe_skin_from_model(load, 1) == 0);
    CHECK(fbe_skin_model_invalid());
    CHECK(says("a node with no name"));
    fbe_assets_free(load);
}
