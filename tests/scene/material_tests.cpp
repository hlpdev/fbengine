// Materials through the C API: how long they live as surfaces, meshes, and handles hold them, and
// what their settings take.

#include "fbengine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// A world of the test's own, freed at its end.
struct test_world_t {
    std::uint64_t handle = fbe_world_create();

    test_world_t() = default;
    test_world_t(const test_world_t&) = delete;
    test_world_t& operator=(const test_world_t&) = delete;
    test_world_t(test_world_t&&) = delete;
    test_world_t& operator=(test_world_t&&) = delete;

    ~test_world_t() {
        fbe_world_free(handle);
        std::array<std::uint64_t, 16> some{};
        while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    }
};

} // namespace

TEST_CASE("a material lives while a surface draws in it, through clones", "[scene]") {
    const std::uint64_t material = fbe_material_create();
    const std::uint64_t geometry = fbe_geometry_cube();
    REQUIRE(fbe_surface_set_material(geometry, 0, material));
    const std::uint64_t clone = fbe_geometry_clone(geometry);
    REQUIRE(fbe_material_release(material));
    CHECK(fbe_material_alive(material));
    fbe_geometry_release(geometry);
    CHECK(fbe_material_alive(material));
    CHECK(fbe_surface_material(clone, 0) == material);
    fbe_geometry_release(clone);
    CHECK_FALSE(fbe_material_alive(material));
}

TEST_CASE("a mesh's material lives with it and its copies", "[scene]") {
    const test_world_t world;
    const std::uint64_t material = fbe_material_create();
    const std::uint64_t mesh = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    REQUIRE(fbe_mesh_set_material(mesh, material));
    fbe_mesh_set_tint(mesh, 1.0F, 0.0F, 0.0F, 0.5F);
    const std::uint64_t copy = fbe_entity_copy(mesh, 0, 0);
    fbe_material_release(material);
    fbe_entity_free(mesh);
    CHECK(fbe_material_alive(material));
    CHECK(fbe_mesh_material(copy) == material);
    fbe_color_t tint{};
    fbe_mesh_tint(copy, &tint);
    CHECK(tint.a == 0.5F);
    REQUIRE(fbe_mesh_set_material(copy, material));
    CHECK(fbe_material_alive(material));
    REQUIRE(fbe_mesh_set_material(copy, 0));
    CHECK_FALSE(fbe_material_alive(material));
}

TEST_CASE("a material's settings take only what they can be", "[scene]") {
    const std::uint64_t material = fbe_material_create();
    CHECK_FALSE(fbe_material_set_layer(material, 8, FBE_LAYER_ADD, 0));
    CHECK(std::string_view(fbe_error()) == "a material's layers are 0 to 7");
    CHECK_FALSE(fbe_material_set_layer(material, 0, FBE_LAYER_BUMP + 1, 0));
    CHECK_FALSE(fbe_material_set_layer(material, 0, FBE_LAYER_ADD, 2));
    CHECK(fbe_material_layer_blend(material, -1) == -1);
    CHECK_FALSE(fbe_material_set_shininess(material, std::numeric_limits<float>::quiet_NaN()));
    CHECK_FALSE(fbe_material_set_alpha_cutoff(material, 1.5F));
    CHECK_FALSE(fbe_material_set_shading(material, 3));
    CHECK_FALSE(fbe_material_set_blend(material, 5));
    CHECK_FALSE(
        fbe_material_set_color(material, 0.0F, 0.0F, 0.0F, std::numeric_limits<float>::infinity()));
    const std::uint64_t geometry = fbe_geometry_cube();
    CHECK_FALSE(fbe_surface_set_material(geometry, 1, material));
    CHECK_FALSE(fbe_surface_set_material(geometry, 0, geometry));
    CHECK(std::string_view(fbe_error()) == "that handle isn't a material's");
    fbe_geometry_release(geometry);
    fbe_material_release(material);
}

TEST_CASE("what Add copies holds its surfaces' materials", "[scene]") {
    const std::uint64_t material = fbe_material_create();
    const std::uint64_t from = fbe_geometry_cube();
    REQUIRE(fbe_surface_set_material(from, 0, material));
    const std::uint64_t into = fbe_geometry_create();
    const fbe_mat4_t identity{ .m = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
    REQUIRE(fbe_geometry_add(into, from, &identity));
    fbe_material_release(material);
    fbe_geometry_release(from);
    CHECK(fbe_material_alive(material));
    CHECK(fbe_surface_material(into, 0) == material);
    // A new material may take the freed slot; letting go of `into` must not touch it.
    const std::uint64_t other = fbe_material_create();
    fbe_geometry_release(into);
    CHECK(fbe_material_alive(other));
    CHECK_FALSE(fbe_material_alive(material));
    fbe_material_release(other);
}

TEST_CASE("alpha is 0 to 1, and colors any finite value", "[scene]") {
    const test_world_t world;
    const std::uint64_t material = fbe_material_create();
    CHECK(fbe_material_set_color(material, -2.0F, 5.0F, 0.0F, 1.0F));
    CHECK_FALSE(fbe_material_set_color(material, 1.0F, 1.0F, 1.0F, 1.5F));
    CHECK(std::string_view(fbe_error()) == "a material's alpha is 0 to 1, not 1.5");
    CHECK_FALSE(fbe_material_set_color(material, 1.0F, 1.0F, 1.0F, -0.1F));
    const std::uint64_t mesh = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    CHECK_FALSE(fbe_mesh_set_tint(mesh, 1.0F, 1.0F, 1.0F, 2.0F));
    CHECK(fbe_mesh_set_tint(mesh, 3.0F, 1.0F, 1.0F, 0.0F));
    fbe_material_release(material);
}

TEST_CASE("a mesh's surfaces are drawn with blends its alpha settles", "[scene]") {
    const test_world_t world;
    const std::uint64_t geometry = fbe_geometry_cube();
    const std::uint64_t mesh = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    fbe_mesh_set_geometry(mesh, geometry);
    // The default material is AUTO and opaque.
    CHECK(fbe_mesh_blend(mesh, 0) == FBE_BLEND_OPAQUE);
    fbe_mesh_set_tint(mesh, 1.0F, 1.0F, 1.0F, 0.5F);
    CHECK(fbe_mesh_blend(mesh, 0) == FBE_BLEND_ALPHA);
    const std::uint64_t opaque = fbe_material_create();
    fbe_material_set_blend(opaque, FBE_BLEND_OPAQUE);
    fbe_material_set_color(opaque, 1.0F, 1.0F, 1.0F, 0.25F);
    fbe_surface_set_material(geometry, 0, opaque);
    // OPAQUE leaves its own alpha out, but the mesh's still fades it.
    CHECK(fbe_mesh_blend(mesh, 0) == FBE_BLEND_ALPHA);
    fbe_mesh_set_tint(mesh, 1.0F, 1.0F, 1.0F, 1.0F);
    CHECK(fbe_mesh_blend(mesh, 0) == FBE_BLEND_OPAQUE);
    fbe_material_set_blend(opaque, FBE_BLEND_AUTO);
    CHECK(fbe_mesh_blend(mesh, 0) == FBE_BLEND_ALPHA);
    const std::uint64_t glow = fbe_material_create();
    fbe_material_set_blend(glow, FBE_BLEND_ADD);
    fbe_mesh_set_material(mesh, glow);
    CHECK(fbe_mesh_blend(mesh, 0) == FBE_BLEND_ADD);
    // A surface it doesn't have is refused, whatever material draws the rest.
    CHECK(fbe_mesh_blend(mesh, 3) == -1);
    CHECK(fbe_mesh_blend(mesh, -1) == -1);
    fbe_mesh_set_material(mesh, 0);
    CHECK(fbe_mesh_blend(mesh, 1) == -1);
    fbe_material_release(opaque);
    fbe_material_release(glow);
    fbe_geometry_release(geometry);
}

namespace {

/// SPIR-V's words: its header, and an entry point of each model and name.
std::vector<std::uint32_t>
spirv_of(std::initializer_list<std::pair<std::uint32_t, std::string_view>> entries) {
    std::vector<std::uint32_t> words{ 0x07230203, 0x00010500, 0, 1, 0 };
    for (const auto& [model, name] : entries) {
        const std::size_t name_words = (name.size() / 4) + 1;
        words.push_back(static_cast<std::uint32_t>((3 + name_words) << 16U) | 15U);
        words.push_back(model);
        words.push_back(1);
        std::vector<std::uint32_t> named(name_words, 0);
        std::memcpy(named.data(), name.data(), name.size());
        words.insert(words.end(), named.begin(), named.end());
    }
    return words;
}

std::vector<std::uint32_t> good_spirv() {
    return spirv_of({
        { 0, "vertexMain" },
        { 4, "fragmentMain" },
        { 0, "particleVertex" },
        { 0, "shadowVertex" },
        { 4, "shadowFragment" },
        { 0, "instanceVertex" },
        { 0, "shadowInstanceVertex" },
        { 0, "prepassVertex" },
        { 0, "prepassInstanceVertex" },
        { 4, "prepassFragment" },
    });
}

/// A cooked shader's bytes: for a version, and SPIR-V's words, of the sources it says.
std::vector<std::uint8_t> cooked(std::string_view version, const std::vector<std::uint32_t>& words,
                                 std::string_view stamp = fbe_shader_stamp()) {
    std::vector<std::uint8_t> bytes;
    const std::string_view magic = "FBESHADR";
    bytes.insert(bytes.end(), magic.begin(), magic.end());
    const auto length = static_cast<std::uint32_t>(version.size());
    bytes.resize(bytes.size() + sizeof(length));
    std::memcpy(bytes.data() + magic.size(), &length, sizeof(length));
    bytes.insert(bytes.end(), version.begin(), version.end());
    bytes.insert(bytes.end(), stamp.begin(), stamp.end());
    const std::size_t at = bytes.size();
    bytes.resize(at + (words.size() * sizeof(std::uint32_t)));
    std::memcpy(bytes.data() + at, words.data(), words.size() * sizeof(std::uint32_t));
    return bytes;
}

std::uint64_t shader_of(const std::vector<std::uint8_t>& bytes) {
    return fbe_shader_create(bytes.data(), static_cast<std::int64_t>(bytes.size()), "a test's");
}

bool error_says(std::string_view what) {
    return std::string_view(fbe_error()).contains(what);
}

} // namespace

TEST_CASE("a shader's file is one cooked for this engine, with the scene's entry points") {
    const std::uint64_t made = shader_of(cooked(fbe_version(), good_spirv()));
    REQUIRE(made != 0);
    CHECK(fbe_shader_alive(made));
    CHECK(fbe_shader_release(made));
    CHECK_FALSE(fbe_shader_alive(made));
    CHECK_FALSE(fbe_shader_release(made));

    CHECK(fbe_shader_create(nullptr, 4, "a test's") == 0);
    const std::string_view source = "float4 shade(Surface surface)";
    CHECK(shader_of({ source.begin(), source.end() }) == 0);
    CHECK(error_says("isn't a cooked shader"));
    CHECK(shader_of(cooked("0.0.1", good_spirv())) == 0);
    CHECK(error_says("cooked for fbengine 0.0.1"));
    CHECK(shader_of(cooked(fbe_version(), spirv_of({ { 0, "vertexMain" } }))) == 0);
    CHECK(error_says("hasn't the scene's fragmentMain"));
    // One of each pass's short, as what's cooked by an older engine's sources would be.
    CHECK(shader_of(cooked(fbe_version(),
                           spirv_of({ { 0, "vertexMain" }, { 4, "fragmentMain" } }))) == 0);
    CHECK(error_says("hasn't the scene's particleVertex"));
    CHECK(shader_of(cooked(fbe_version(), spirv_of({ { 0, "vertexMain" },
                                                     { 4, "fragmentMain" },
                                                     { 0, "particleVertex" } }))) == 0);
    CHECK(error_says("hasn't the scene's shadowVertex"));
    CHECK(shader_of(cooked(fbe_version(), spirv_of({ { 0, "vertexMain" },
                                                     { 4, "fragmentMain" },
                                                     { 0, "particleVertex" },
                                                     { 0, "shadowVertex" } }))) == 0);
    CHECK(error_says("hasn't the scene's shadowFragment"));
    CHECK(shader_of(cooked(fbe_version(), spirv_of({ { 0, "vertexMain" },
                                                     { 4, "fragmentMain" },
                                                     { 0, "particleVertex" },
                                                     { 0, "shadowVertex" },
                                                     { 4, "shadowFragment" } }))) == 0);
    CHECK(error_says("hasn't the scene's instanceVertex"));
    CHECK(shader_of(cooked(fbe_version(), spirv_of({ { 0, "vertexMain" },
                                                     { 4, "fragmentMain" },
                                                     { 0, "particleVertex" },
                                                     { 0, "shadowVertex" },
                                                     { 4, "shadowFragment" },
                                                     { 0, "instanceVertex" } }))) == 0);
    CHECK(error_says("hasn't the scene's shadowInstanceVertex"));
    CHECK(shader_of(cooked(fbe_version(), spirv_of({ { 0, "vertexMain" },
                                                     { 4, "fragmentMain" },
                                                     { 0, "particleVertex" },
                                                     { 0, "shadowVertex" },
                                                     { 4, "shadowFragment" },
                                                     { 0, "instanceVertex" },
                                                     { 0, "shadowInstanceVertex" },
                                                     { 0, "prepassVertex" },
                                                     { 0, "prepassInstanceVertex" } }))) == 0);
    CHECK(error_says("hasn't the scene's prepassFragment"));
    // Named right, for the wrong stage.
    CHECK(shader_of(cooked(fbe_version(), spirv_of({ { 4, "vertexMain" },
                                                     { 0, "fragmentMain" },
                                                     { 0, "particleVertex" },
                                                     { 0, "shadowVertex" },
                                                     { 4, "shadowFragment" } }))) == 0);
    CHECK(error_says("hasn't the scene's vertexMain"));
    // An instruction past its end, one of no length, part of a word, and what isn't SPIR-V.
    std::vector<std::uint32_t> broken = good_spirv();
    broken[5] = (1000U << 16U) | 15U;
    CHECK(shader_of(cooked(fbe_version(), broken)) == 0);
    broken[5] = 15U;
    CHECK(shader_of(cooked(fbe_version(), broken)) == 0);
    std::vector<std::uint8_t> cut = cooked(fbe_version(), good_spirv());
    cut.pop_back();
    CHECK(shader_of(cut) == 0);
    cut.resize(10);
    CHECK(shader_of(cut) == 0);
    broken = good_spirv();
    broken[0] = 1;
    CHECK(shader_of(cooked(fbe_version(), broken)) == 0);
    // A version said to be longer than the file.
    std::vector<std::uint8_t> long_version = cooked(fbe_version(), good_spirv());
    long_version[8] = 255;
    long_version[11] = 127;
    CHECK(shader_of(long_version) == 0);
    CHECK(error_says("cut short"));
    // Of this version, but other sources' layout, or as old as this stamp would need more of it.
    const std::string other_stamp(std::string_view(fbe_shader_stamp()).size(), '0');
    CHECK(shader_of(cooked(fbe_version(), good_spirv(), other_stamp)) == 0);
    CHECK(error_says("other sources"));
    const std::vector<std::uint8_t> no_stamp = cooked(fbe_version(), {}, "");
    CHECK(shader_of(no_stamp) == 0);
    CHECK(error_says("cut short"));
    // Of another version that goes on and on, only so much of it's said.
    const std::string rambling(4000, 'x');
    CHECK(shader_of(cooked(rambling, good_spirv())) == 0);
    CHECK(error_says("cooked for fbengine xxx"));
    CHECK(std::string_view(fbe_error()).size() < 200);
}

TEST_CASE("a shader binds only what the scene's pipelines have") {
    const auto decorated = [](std::uint32_t kind, std::uint32_t value) {
        std::vector<std::uint32_t> words = good_spirv();
        words.insert(words.end(), { (4U << 16U) | 71U, 9, kind, value });
        return cooked(fbe_version(), words);
    };
    // The bindless set's bindings, as gpu/vulkan.hpp has them: its samplers, 0, its multisampled
    // images, 1, and its textures, 2.
    const std::vector<std::uint8_t> good = decorated(33, 2);
    const std::uint64_t made = shader_of(good);
    CHECK(made != 0);
    fbe_shader_release(made);
    CHECK(fbe_shader_check(good.data(), static_cast<std::int64_t>(good.size())));
    CHECK(shader_of(decorated(33, 0)) != 0);
    CHECK(shader_of(decorated(34, 0)) != 0);
    // At 1, an array of 2D multisampled images of floats, to be sampled, as the set's is, and
    // nothing else there.
    struct image_t {
        std::uint32_t multisampled = 1;
        /// 1 to be sampled, 2 a storage image.
        std::uint32_t sampled = 1;
        /// What it's of: the float, 20, or the int, 25.
        std::uint32_t of = 20;
        bool sized = true;
        /// How many of its 9 words there are, as the last instruction.
        std::size_t there = 9;
    };
    const auto at_samples = [](const image_t& image) {
        std::vector<std::uint32_t> words = good_spirv();
        // A float, an int, and 64 of it.
        words.insert(words.end(), { (3U << 16U) | 22U, 20, 32, (4U << 16U) | 21U, 25, 32, 0,
                                    (4U << 16U) | 43U, 25, 26, 64 });
        if (image.sized) {
            words.insert(words.end(), { (4U << 16U) | 28U, 22, 21, 26 });
        } else {
            words.insert(words.end(), { (3U << 16U) | 29U, 22, 21 });
        }
        // A pointer to them, a variable of it, at binding 1, and the images last.
        words.insert(words.end(), { (4U << 16U) | 32U, 23, 0, 22, (4U << 16U) | 59U, 23, 24, 0,
                                    (4U << 16U) | 71U, 24, 33, 1 });
        const std::array<std::uint32_t, 9> type{
            (9U << 16U) | 25U, 21, image.of, 1, 0, 0, image.multisampled, image.sampled, 0,
        };
        words.insert(words.end(), type.begin(),
                     type.begin() + static_cast<std::ptrdiff_t>(image.there));
        return cooked(fbe_version(), words);
    };
    const std::uint64_t samples = shader_of(at_samples({}));
    CHECK(samples != 0);
    fbe_shader_release(samples);
    const std::uint64_t unsized = shader_of(at_samples({ .sized = false }));
    CHECK(unsized != 0);
    fbe_shader_release(unsized);
    CHECK(shader_of(at_samples({ .multisampled = 0 })) == 0);
    CHECK(error_says("don't bind"));
    // The review's: a storage image, one of ints, and one whose last words aren't there, as far
    // as what it's checked by is there.
    CHECK(shader_of(at_samples({ .sampled = 2 })) == 0);
    CHECK(error_says("don't bind"));
    CHECK(shader_of(at_samples({ .of = 25 })) == 0);
    CHECK(error_says("don't bind"));
    CHECK(shader_of(at_samples({ .there = 8 })) == 0);
    CHECK(error_says("don't bind"));
    CHECK(shader_of(decorated(33, 1)) == 0);
    // A global's, as Slang gives one, past the set's, a texture's, and a set of its own.
    CHECK(shader_of(decorated(33, 3)) == 0);
    CHECK(error_says("don't bind"));
    CHECK(shader_of(decorated(33, 4)) == 0);
    CHECK(shader_of(decorated(34, 1)) == 0);
    const std::vector<std::uint8_t> bad = decorated(33, 3);
    CHECK_FALSE(fbe_shader_check(bad.data(), static_cast<std::int64_t>(bad.size())));
    CHECK(error_says("don't bind"));
    // A check keeps nothing, and refuses what creating it does.
    const std::vector<std::uint8_t> source{ 1, 2, 3 };
    CHECK_FALSE(fbe_shader_check(source.data(), 3));
    CHECK_FALSE(fbe_shader_check(nullptr, 4));
}

TEST_CASE("the same bytes again and again aren't more than a game's shaders can be") {
    const std::vector<std::uint8_t> good = cooked(fbe_version(), good_spirv());
    for (int i = 0; i < 9000; ++i) {
        const std::uint64_t made = shader_of(good);
        REQUIRE(made != 0);
        REQUIRE(fbe_shader_release(made));
    }
}

TEST_CASE("a material holds its shader, which goes with the last of what holds it") {
    const std::uint64_t material = fbe_material_create();
    const std::uint64_t shader = shader_of(cooked(fbe_version(), good_spirv()));
    CHECK(fbe_material_shader(material) == 0);
    CHECK(fbe_material_set_shader(material, shader));
    CHECK(fbe_material_shader(material) == shader);
    // A clone holds it too; the game letting go leaves it to them.
    const std::uint64_t clone = fbe_material_clone(material);
    CHECK(fbe_material_shader(clone) == shader);
    CHECK(fbe_shader_release(shader));
    CHECK(fbe_shader_alive(shader));
    CHECK(fbe_material_release(material));
    CHECK(fbe_shader_alive(shader));
    // Set again, to the one it has, it isn't let go of.
    CHECK(fbe_material_set_shader(clone, shader));
    CHECK(fbe_shader_alive(shader));
    CHECK(fbe_material_set_shader(clone, 0));
    CHECK_FALSE(fbe_shader_alive(shader));
    CHECK(fbe_material_shader(clone) == 0);
    CHECK_FALSE(fbe_material_set_shader(clone, shader));
    CHECK(fbe_material_release(clone));
    CHECK_FALSE(fbe_material_set_shader(clone, 0));
    CHECK(fbe_material_shader(clone) == 0);
}

TEST_CASE("a shader replaced stays itself, and refusing leaves it as it was") {
    const std::vector<std::uint8_t> good = cooked(fbe_version(), good_spirv());
    const std::uint64_t shader = shader_of(good);
    CHECK(fbe_shader_replace(shader, good.data(), static_cast<std::int64_t>(good.size()), "new"));
    const std::vector<std::uint8_t> bad{ 1, 2, 3 };
    CHECK_FALSE(fbe_shader_replace(shader, bad.data(), 3, "new"));
    CHECK(fbe_shader_alive(shader));
    CHECK(fbe_shader_release(shader));
    CHECK_FALSE(
        fbe_shader_replace(shader, good.data(), static_cast<std::int64_t>(good.size()), "new"));
}

TEST_CASE("a material's parameters are its own, 8 of them, of numbers") {
    const std::uint64_t material = fbe_material_create();
    std::array<float, 4> read{ 9.0F, 9.0F, 9.0F, 9.0F };
    CHECK(fbe_material_parameter(material, 7, read.data()));
    CHECK(read == std::array<float, 4>{ 0.0F, 0.0F, 0.0F, 0.0F });
    CHECK(fbe_material_set_parameter(material, 7, 1.0F, 2.0F, 3.0F, 4.0F));
    CHECK(fbe_material_parameter(material, 7, read.data()));
    CHECK(read == std::array<float, 4>{ 1.0F, 2.0F, 3.0F, 4.0F });
    CHECK_FALSE(fbe_material_set_parameter(material, 8, 0.0F, 0.0F, 0.0F, 0.0F));
    CHECK_FALSE(fbe_material_set_parameter(material, -1, 0.0F, 0.0F, 0.0F, 0.0F));
    CHECK_FALSE(fbe_material_parameter(material, 8, read.data()));
    for (const float none : {
             std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity(),
             std::numeric_limits<float>::quiet_NaN(),
         }) {
        CHECK_FALSE(fbe_material_set_parameter(material, 0, none, 0.0F, 0.0F, 0.0F));
        CHECK_FALSE(fbe_material_set_parameter(material, 0, 0.0F, none, 0.0F, 0.0F));
        CHECK_FALSE(fbe_material_set_parameter(material, 0, 0.0F, 0.0F, none, 0.0F));
        CHECK_FALSE(fbe_material_set_parameter(material, 0, 0.0F, 0.0F, 0.0F, none));
    }
    CHECK(fbe_material_parameter(material, 0, read.data()));
    CHECK(read == std::array<float, 4>{ 0.0F, 0.0F, 0.0F, 0.0F });
    const std::uint64_t clone = fbe_material_clone(material);
    CHECK(fbe_material_set_parameter(material, 7, 5.0F, 5.0F, 5.0F, 5.0F));
    CHECK(fbe_material_parameter(clone, 7, read.data()));
    CHECK(read[0] == 1.0F);
    fbe_material_release(clone);
    fbe_material_release(material);
    CHECK_FALSE(fbe_material_set_parameter(material, 0, 0.0F, 0.0F, 0.0F, 0.0F));
    CHECK_FALSE(fbe_material_parameter(material, 0, read.data()));
}
