// glTFs decoded to the CPU's model: made here, by a builder that packs their buffers, as JSON with
// data URIs, as binary glTFs, and with buffers and images in files beside them in res://.

#include "assets/loads.hpp"
#include "assets/model.hpp"
#include "assets/shared.hpp"
#include "files.hpp"
#include "jobs/jobs.hpp"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

namespace assets = fbe::assets;
namespace vfs = fbe::vfs;
using assets::bytes_t;

constexpr int float_type = 5126;
constexpr int ushort_type = 5123;
constexpr int ubyte_type = 5121;

// A glTF's buffer, views, and accessors, made as the test asks for them.
struct builder_t {
    // An accessor of `count` values of `type`, whose bytes are `data`, and gives its index.
    int add(std::string_view type, int component, std::size_t count, const bytes_t& data,
            std::string_view extra = "") {
        buffer.resize((buffer.size() + 3) / 4 * 4, 0);
        const std::size_t offset = buffer.size();
        buffer.insert(buffer.end(), data.begin(), data.end());
        views.push_back(R"({"buffer":0,"byteOffset":)" + std::to_string(offset) +
                        R"(,"byteLength":)" + std::to_string(data.size()) + "}");
        accessors.push_back(R"({"bufferView":)" + std::to_string(views.size() - 1) +
                            R"(,"componentType":)" + std::to_string(component) + R"(,"count":)" +
                            std::to_string(count) + R"(,"type":")" + std::string(type) + "\"" +
                            std::string(extra) + "}");
        return static_cast<int>(accessors.size() - 1);
    }

    int floats(std::string_view type, std::size_t count, std::initializer_list<float> values,
               std::string_view extra = "") {
        bytes_t data;
        test::append<float>(data, values);
        return add(type, float_type, count, data, extra);
    }

    int positions(std::initializer_list<float> values) {
        // POSITION needs its bounds; these hold every test's.
        return floats("VEC3", values.size() / 3, values,
                      R"(,"min":[-10,-10,-10],"max":[10,10,10])");
    }

    int indices(std::initializer_list<std::uint16_t> values) {
        bytes_t data;
        test::append<std::uint16_t>(data, values);
        return add("SCALAR", ushort_type, values.size(), data);
    }

    // The glTF: its buffer at `uri`, or a binary glTF's own with none, and `rest`, its other
    // top-level members, each after a comma.
    [[nodiscard]] std::string json(std::string_view rest, std::string_view uri) const {
        std::string text = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)" +
                           std::to_string(buffer.size());
        if (!uri.empty()) {
            text += R"(,"uri":")" + std::string(uri) + "\"";
        }
        text += "}],\"bufferViews\":[" + joined(views) + "],\"accessors\":[" + joined(accessors) +
                "]" + std::string(rest) + "}";
        return text;
    }

    [[nodiscard]] std::string data_uri() const {
        return "data:application/octet-stream;base64," + test::base64(buffer);
    }

    static std::string joined(const std::vector<std::string>& parts) {
        std::string text;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            text += (i > 0 ? "," : "") + parts[i];
        }
        return text;
    }

    bytes_t buffer;
    std::vector<std::string> views;
    std::vector<std::string> accessors;
};

vfs::path_t path(std::string_view text) {
    return *vfs::parse(text);
}

assets::result_t<assets::model_t>
decode(std::string_view json, const vfs::vfs_t& files = vfs::vfs_t(),
       const assets::model_options_t& options = assets::model_options_t(),
       std::string_view at = "res://models/test.gltf") {
    const bytes_t bytes(json.begin(), json.end());
    return assets::decode_model(path(at), bytes, files.view(), options);
}

assets::model_t decoded(std::string_view json, const vfs::vfs_t& files = vfs::vfs_t(),
                        const assets::model_options_t& options = assets::model_options_t()) {
    assets::result_t<assets::model_t> model = decode(json, files, options);
    if (!model) {
        FAIL(model.error().message);
    }
    return std::move(*model);
}

assets::failure_t failure(std::string_view json, const vfs::vfs_t& files = vfs::vfs_t()) {
    assets::result_t<assets::model_t> model = decode(json, files);
    REQUIRE_FALSE(model.has_value());
    return model.error().failure;
}

// A red triangle's glTF, as root's mesh, with a child, in a scene.
struct triangle_t {
    triangle_t() {
        built.positions({ 0, 0, 0, 1, 0, 0, 0, 1, 0 });
        built.floats("VEC3", 3, { 0, 0, 1, 0, 0, 1, 0, 0, 1 });
        built.floats("VEC2", 3, { 0, 0, 1, 0, 0, 1 });
        built.indices({ 0, 1, 2 });
    }

    static constexpr std::string_view rest =
        R"(,"materials":[{"name":"red","pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1],)"
        R"("metallicFactor":0.25,"roughnessFactor":0.5},"doubleSided":true,"alphaMode":"MASK",)"
        R"("alphaCutoff":0.3}],)"
        R"("meshes":[{"name":"tri","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,)"
        R"("TEXCOORD_0":2},"indices":3,"material":0}]}],)"
        R"("nodes":[{"name":"root","mesh":0,"translation":[1,2,3],"children":[1]},)"
        R"({"name":"child","rotation":[0,0.7071068,0,0.7071068],"scale":[2,2,2]}],)"
        R"("scenes":[{"name":"only","nodes":[0]}],"scene":0)";

    builder_t built;
};

void check_triangle(const assets::model_t& model) {
    REQUIRE(model.meshes.size() == 1);
    REQUIRE(model.meshes[0].name == "tri");
    const assets::primitive_t& primitive = model.meshes[0].primitives.at(0);
    REQUIRE(primitive.topology == assets::topology_t::TRIANGLES);
    REQUIRE(primitive.positions ==
            std::vector<assets::vec3_t>{ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } });
    REQUIRE(primitive.normals == std::vector<assets::vec3_t>(3, { 0, 0, 1 }));
    REQUIRE(primitive.uv0 == std::vector<assets::vec2_t>{ { 0, 0 }, { 1, 0 }, { 0, 1 } });
    REQUIRE(primitive.uv1.empty());
    REQUIRE(primitive.indices == std::vector<std::uint32_t>{ 0, 1, 2 });
    REQUIRE(primitive.material == 0);

    const assets::material_t& material = model.materials.at(0);
    REQUIRE(material.name == "red");
    REQUIRE(material.base_color == assets::vec4_t{ 1, 0, 0, 1 });
    REQUIRE(material.metallic == 0.25F);
    REQUIRE(material.roughness == 0.5F);
    REQUIRE(material.double_sided);
    REQUIRE(material.alpha == assets::alpha_t::MASK);
    REQUIRE(material.alpha_cutoff == 0.3F);
    REQUIRE(material.base_color_texture.image == -1);

    REQUIRE(model.nodes.size() == 2);
    REQUIRE(model.nodes[0].name == "root");
    REQUIRE(model.nodes[0].mesh == 0);
    REQUIRE(model.nodes[0].translation == assets::vec3_t{ 1, 2, 3 });
    REQUIRE(model.nodes[0].children == std::vector<std::uint32_t>{ 1 });
    REQUIRE(model.nodes[1].rotation[1] == 0.7071068F);
    REQUIRE(model.nodes[1].scale == assets::vec3_t{ 2, 2, 2 });
    REQUIRE(model.nodes[1].mesh == -1);
    REQUIRE(model.scenes.size() == 1);
    REQUIRE(model.scenes[0].nodes == std::vector<std::uint32_t>{ 0 });
    REQUIRE(model.scene == 0);
}

} // namespace

TEST_CASE("a glTF with a data URI decodes its meshes, materials, nodes, and scenes", "[assets]") {
    const triangle_t triangle;
    check_triangle(decoded(triangle.built.json(triangle_t::rest, triangle.built.data_uri())));
}

TEST_CASE("a binary glTF decodes as its JSON does", "[assets]") {
    const triangle_t triangle;
    const bytes_t glb = test::glb(triangle.built.json(triangle_t::rest, ""), triangle.built.buffer);
    assets::result_t<assets::model_t> model =
        assets::decode_model(path("res://test.glb"), glb, vfs::view_t(), {});
    if (!model) {
        FAIL(model.error().message);
    }
    check_triangle(*model);
}

TEST_CASE("a glTF's buffers and images are read from res://, beside it", "[assets]") {
    const test::temp_folder_t folder;
    const triangle_t triangle;
    // A base color texture, sRGB, and a normal map, linear, with a sampler and a transform.
    const std::string rest =
        R"(,"images":[{"uri":"../textures/wall.png"},{"uri":"normal%20map.png"}],)"
        R"("samplers":[{"magFilter":9728,"minFilter":9987,"wrapS":33071,"wrapT":33648}],)"
        R"("textures":[{"source":0,"sampler":0},{"source":1}],)"
        R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0,"texCoord":1,)"
        R"("extensions":{"KHR_texture_transform":{"offset":[0.5,0.25],"scale":[2,3],)"
        R"("rotation":0.5}}}},"normalTexture":{"index":1,"scale":0.75}}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":2,"TEXCOORD_1":2},)"
        R"("indices":3,"material":0}]}],)"
        R"("extensionsUsed":["KHR_texture_transform"])";
    folder.write("models/tri.gltf", triangle.built.json(rest, "tri.bin"));
    folder.write("models/tri.bin", triangle.built.buffer);
    folder.write("textures/wall.png", test::png(2, 2, 4, std::vector<std::uint8_t>(16, 99)));
    folder.write("models/normal map.png", test::png(1, 1, 4, { 128, 128, 255, 255 }));
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    const bytes_t gltf = *files.read(path("res://models/tri.gltf"));

    for (const bool with_jobs : { false, true }) {
        fbe::jobs_t jobs(2);
        std::mutex mutex;
        std::vector<float> told;
        const assets::model_options_t options{
            .jobs = with_jobs ? &jobs : nullptr,
            .progress =
                [&](float done) {
                    const std::scoped_lock lock(mutex);
                    told.push_back(done);
                },
        };
        assets::result_t<assets::model_t> model =
            assets::decode_model(path("res://Models/Tri.gltf"), gltf, files.view(), options);
        if (!model) {
            FAIL(model.error().message);
        }
        REQUIRE(model->meshes[0].primitives[0].positions.size() == 3);
        REQUIRE(model->images.size() == 2);
        REQUIRE(model->images[0].format == assets::formats::rgba8_srgb);
        REQUIRE(model->images[0].width == 2);
        REQUIRE(model->images[1].format == assets::formats::rgba8);
        REQUIRE(std::ranges::equal(model->images[1].texels.bytes(),
                                   std::vector<std::uint8_t>{ 128, 128, 255, 255 }));
        const assets::material_t& material = model->materials[0];
        REQUIRE(material.base_color_texture.image == 0);
        REQUIRE(material.base_color_texture.sampler == 0);
        REQUIRE(material.base_color_texture.uv_set == 1);
        REQUIRE(material.base_color_texture.offset == assets::vec2_t{ 0.5F, 0.25F });
        REQUIRE(material.base_color_texture.scale == assets::vec2_t{ 2, 3 });
        REQUIRE(material.base_color_texture.rotation == 0.5F);
        REQUIRE(material.normal_texture.image == 1);
        REQUIRE(material.normal_texture.sampler == -1);
        REQUIRE(material.normal_scale == 0.75F);
        REQUIRE(model->samplers.at(0).mag == 9728);
        REQUIRE(model->samplers[0].min == 9987);
        REQUIRE(model->samplers[0].wrap_u == 33071);
        REQUIRE(model->samplers[0].wrap_v == 33648);
        // Steps of the parse, the buffer, two images, and the rest, each told once, up to 1.
        const std::scoped_lock lock(mutex);
        REQUIRE(told.size() == 5);
        std::ranges::sort(told);
        REQUIRE(told.back() == 1.0F);
    }

    // What it names that isn't there fails as reading it does.
    folder.write("models/tri.bin", bytes_t{});
    std::filesystem::remove(folder.path / "models" / "tri.bin");
    assets::result_t<assets::model_t> missing =
        assets::decode_model(path("res://models/tri.gltf"), gltf, files.view(), {});
    REQUIRE(missing.error().failure == assets::failure_t::NOT_FOUND);
}

// A damaged .bin is named, as its URI says once it's decoded, not only the .gltf that reads it.
TEST_CASE("a glTF whose buffer file is short is refused naming the file", "[assets][stability]") {
    const test::temp_folder_t folder;
    const triangle_t triangle;
    folder.write("models/tri.gltf", triangle.built.json(triangle_t::rest, "tri%20part.bin"));
    const bytes_t whole = triangle.built.buffer;
    REQUIRE(whole.size() > 4);
    folder.write("models/tri part.bin", bytes_t(whole.begin(), whole.begin() + 4));
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    const bytes_t gltf = *files.read(path("res://models/tri.gltf"));
    const assets::result_t<assets::model_t> model =
        assets::decode_model(path("res://models/tri.gltf"), gltf, files.view(), {});
    REQUIRE_FALSE(model);
    INFO(model.error().message);
    CHECK(model.error().failure == assets::failure_t::INVALID_DATA);
    CHECK(model.error().message ==
          std::format("res://models/tri.gltf isn't a glTF the engine loads: its buffer "
                      "tri part.bin has 4 bytes of its {}",
                      whole.size()));
}

TEST_CASE("a primitive without normals gets its triangles' flat normals", "[assets]") {
    builder_t built;
    // A square, as two triangles sharing two vertices, at a slope.
    built.positions({ 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1 });
    built.floats("VEC2", 4, { 0, 0, 1, 0, 1, 1, 0, 1 });
    built.indices({ 0, 1, 2, 0, 2, 3 });
    const assets::model_t model = decoded(built.json(
        R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2}]}])",
        built.data_uri()));
    const assets::primitive_t& primitive = model.meshes[0].primitives[0];
    REQUIRE(primitive.positions.size() == 6);
    REQUIRE(primitive.uv0.size() == 6);
    REQUIRE(primitive.uv0[5] == assets::vec2_t{ 0, 1 });
    REQUIRE(primitive.indices == std::vector<std::uint32_t>{ 0, 1, 2, 3, 4, 5 });
    for (const assets::vec3_t& normal : primitive.normals) {
        REQUIRE(normal[0] == 0);
        REQUIRE(std::abs(normal[1] + 0.7071068F) < 1e-5F);
        REQUIRE(std::abs(normal[2] - 0.7071068F) < 1e-5F);
    }
}

TEST_CASE("strips and fans become lists that face one way", "[assets]") {
    builder_t built;
    built.positions({ 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 2, 0, 0 });
    built.floats("VEC3", 5, { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 });
    const std::string meshes =
        R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"mode":5},)"
        R"({"attributes":{"POSITION":0,"NORMAL":1},"mode":6},)"
        R"({"attributes":{"POSITION":0,"NORMAL":1},"mode":3},)"
        R"({"attributes":{"POSITION":0,"NORMAL":1},"mode":2},)"
        R"({"attributes":{"POSITION":0,"NORMAL":1},"mode":0}]}])";
    const assets::model_t model = decoded(built.json(meshes, built.data_uri()));
    const auto& primitives = model.meshes[0].primitives;
    // glTF's order: every other triangle's last two the other way round.
    REQUIRE(primitives[0].indices == std::vector<std::uint32_t>{ 0, 1, 2, 1, 3, 2, 2, 3, 4 });
    REQUIRE(primitives[1].indices == std::vector<std::uint32_t>{ 0, 1, 2, 0, 2, 3, 0, 3, 4 });
    REQUIRE(primitives[2].topology == assets::topology_t::LINES);
    REQUIRE(primitives[2].indices == std::vector<std::uint32_t>{ 0, 1, 1, 2, 2, 3, 3, 4 });
    REQUIRE(primitives[3].indices == std::vector<std::uint32_t>{ 0, 1, 1, 2, 2, 3, 3, 4, 4, 0 });
    REQUIRE(primitives[4].topology == assets::topology_t::POINTS);
    REQUIRE(primitives[4].indices.size() == 5);
}

TEST_CASE("skins, morph targets, and clips decode", "[assets]") {
    builder_t built;
    const int positions = built.positions({ 0, 0, 0, 1, 0, 0, 0, 1, 0 });
    const int normals = built.floats("VEC3", 3, { 0, 0, 1, 0, 0, 1, 0, 0, 1 });
    bytes_t joints;
    test::append<std::uint8_t>(joints, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
    const int joint = built.add("VEC4", ubyte_type, 3, joints);
    const int weights = built.floats("VEC4", 3, { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 });
    const int moved =
        built.floats("VEC3", 3, { 0, 0, 1, 0, 0, 1, 0, 0, 1 }, R"(,"min":[0,0,1],"max":[0,0,1])");
    const int binds =
        built.floats("MAT4", 1, { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1, -2, -3, 1 });
    const int times = built.floats("SCALAR", 2, { 0, 1.5F }, R"(,"min":[0],"max":[1.5])");
    const int translations = built.floats("VEC3", 2, { 0, 0, 0, 0, 5, 0 });
    const int rotations = built.floats("VEC4", 2, { 0, 0, 0, 1, 0, 1, 0, 0 });
    // Cubic: an in tangent, a value, and an out tangent a keyframe, of one target's weight.
    const int cubic = built.floats("SCALAR", 6, { 0, 0, 0, 0, 1, 0 });
    const std::string rest =
        R"(,"meshes":[{"weights":[0.5],"primitives":[{"attributes":{"POSITION":)" +
        std::to_string(positions) + R"(,"NORMAL":)" + std::to_string(normals) + R"(,"JOINTS_0":)" +
        std::to_string(joint) + R"(,"WEIGHTS_0":)" + std::to_string(weights) +
        R"(},"targets":[{"POSITION":)" + std::to_string(moved) + R"(}]}]}],)" +
        R"("nodes":[{"name":"body","mesh":0,"skin":0,"children":[1]},{"name":"bone"}],)" +
        R"("skins":[{"joints":[1],"skeleton":1,"inverseBindMatrices":)" + std::to_string(binds) +
        R"(}],"animations":[{"name":"walk","samplers":[)" + R"({"input":)" + std::to_string(times) +
        R"(,"output":)" + std::to_string(translations) + R"(},{"input":)" + std::to_string(times) +
        R"(,"output":)" + std::to_string(rotations) + R"(,"interpolation":"STEP"},{"input":)" +
        std::to_string(times) + R"(,"output":)" + std::to_string(cubic) +
        R"(,"interpolation":"CUBICSPLINE"}],"channels":[)" +
        R"({"sampler":0,"target":{"node":1,"path":"translation"}},)" +
        R"({"sampler":1,"target":{"node":1,"path":"rotation"}},)" +
        R"({"sampler":2,"target":{"node":0,"path":"weights"}}]}])";
    const assets::model_t model = decoded(built.json(rest, built.data_uri()));

    const assets::primitive_t& primitive = model.meshes[0].primitives[0];
    REQUIRE(primitive.joints.size() == 3);
    REQUIRE(primitive.weights[1] == assets::vec4_t{ 1, 0, 0, 0 });
    REQUIRE(primitive.targets.size() == 1);
    REQUIRE(primitive.targets[0].positions[2] == assets::vec3_t{ 0, 0, 1 });
    REQUIRE(primitive.targets[0].normals.empty());
    REQUIRE(model.meshes[0].weights == std::vector<float>{ 0.5F });

    const assets::skin_t& skin = model.skins.at(0);
    REQUIRE(skin.joints == std::vector<std::uint32_t>{ 1 });
    REQUIRE(skin.skeleton == 1);
    REQUIRE(skin.inverse_binds.at(0)[12] == -1);
    REQUIRE(skin.inverse_binds[0][14] == -3);
    REQUIRE(model.nodes[0].skin == 0);

    const assets::clip_t& clip = model.clips.at(0);
    REQUIRE(clip.name == "walk");
    REQUIRE(clip.length == 1.5F);
    REQUIRE(clip.channels.size() == 3);
    REQUIRE(clip.channels[0].property == assets::property_t::TRANSLATION);
    REQUIRE(clip.channels[0].node == 1);
    REQUIRE(clip.channels[0].interpolation == assets::interpolation_t::LINEAR);
    REQUIRE(clip.channels[0].times == std::vector<float>{ 0, 1.5F });
    REQUIRE(clip.channels[0].values == std::vector<float>{ 0, 0, 0, 0, 5, 0 });
    REQUIRE(clip.channels[1].property == assets::property_t::ROTATION);
    REQUIRE(clip.channels[1].interpolation == assets::interpolation_t::STEP);
    REQUIRE(clip.channels[1].width == 4);
    REQUIRE(clip.channels[2].property == assets::property_t::WEIGHTS);
    REQUIRE(clip.channels[2].interpolation == assets::interpolation_t::CUBIC);
    REQUIRE(clip.channels[2].width == 1);
    REQUIRE(clip.channels[2].values.size() == 6);
}

TEST_CASE("cameras, lights, and the materials' extensions decode", "[assets]") {
    builder_t built;
    built.positions({ 0, 0, 0, 1, 0, 0, 0, 1, 0 });
    const std::string rest =
        R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)"
        R"("materials":[{"emissiveFactor":[1,0.5,0],"alphaMode":"BLEND","extensions":{)"
        R"("KHR_materials_emissive_strength":{"emissiveStrength":4},"KHR_materials_unlit":{}}}],)"
        R"("cameras":[{"type":"perspective","perspective":{"yfov":1.2,"znear":0.05,"zfar":100}},)"
        R"({"type":"orthographic","orthographic":{"xmag":3,"ymag":2,"znear":0.1,"zfar":50}}],)"
        R"("extensions":{"KHR_lights_punctual":{"lights":[{"type":"spot","color":[1,0.9,0.8],)"
        R"("intensity":20,"range":8,"spot":{"innerConeAngle":0.2,"outerConeAngle":0.6}},)"
        R"({"type":"directional","intensity":3}]}},)"
        R"("nodes":[{"camera":0},{"camera":1},{"extensions":{"KHR_lights_punctual":{"light":0}}}],)"
        R"("extensionsUsed":["KHR_lights_punctual","KHR_materials_emissive_strength",)"
        R"("KHR_materials_unlit"])";
    const assets::model_t model = decoded(built.json(rest, built.data_uri()));
    const assets::material_t& material = model.materials.at(0);
    REQUIRE(material.emissive == assets::vec3_t{ 4, 2, 0 });
    REQUIRE(material.unlit);
    REQUIRE(material.alpha == assets::alpha_t::BLEND);
    // Without normals, flat ones, and without indices, its vertices in order.
    REQUIRE(model.meshes[0].primitives[0].normals.size() == 3);
    REQUIRE(model.cameras.size() == 2);
    REQUIRE_FALSE(model.cameras[0].orthographic);
    REQUIRE(model.cameras[0].fov == 1.2F);
    REQUIRE(model.cameras[0].z_near == 0.05F);
    REQUIRE(model.cameras[0].z_far == 100);
    REQUIRE(model.cameras[1].orthographic);
    REQUIRE(model.cameras[1].magnification == assets::vec2_t{ 3, 2 });
    REQUIRE(model.lights.size() == 2);
    REQUIRE(model.lights[0].kind == assets::light_kind_t::SPOT);
    REQUIRE(model.lights[0].intensity == 20);
    REQUIRE(model.lights[0].range == 8);
    REQUIRE(model.lights[0].inner_cone == 0.2F);
    REQUIRE(model.lights[0].outer_cone == 0.6F);
    REQUIRE(model.lights[1].kind == assets::light_kind_t::DIRECTIONAL);
    REQUIRE(model.nodes[2].light == 0);
    REQUIRE(model.nodes[1].camera == 1);
    REQUIRE(model.scene == -1);
}

TEST_CASE("a glTF that's broken, or names what isn't there, doesn't decode", "[assets]") {
    using assets::failure_t;
    SECTION("an index past the vertices") {
        builder_t built;
        built.positions({ 0, 0, 0, 1, 0, 0, 0, 1, 0 });
        built.indices({ 0, 1, 3 });
        REQUIRE(failure(built.json(
                    R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}])",
                    built.data_uri())) == failure_t::INVALID_DATA);
    }
    SECTION("an attribute shorter than the positions") {
        builder_t built;
        built.positions({ 0, 0, 0, 1, 0, 0, 0, 1, 0 });
        built.floats("VEC2", 2, { 0, 0, 1, 0 });
        REQUIRE(failure(built.json(R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":0,)"
                                   R"("TEXCOORD_0":1}}]}])",
                                   built.data_uri())) == failure_t::INVALID_DATA);
    }
    SECTION("nodes that aren't a forest") {
        builder_t built;
        built.positions({ 0, 0, 0 });
        REQUIRE(failure(built.json(R"(,"nodes":[{"children":[1]},{"children":[0]}])",
                                   built.data_uri())) == failure_t::INVALID_DATA);
        REQUIRE(failure(built.json(R"(,"nodes":[{"children":[2]},{"children":[2]},{}])",
                                   built.data_uri())) == failure_t::INVALID_DATA);
        REQUIRE(failure(built.json(R"(,"nodes":[{"children":[0]}])", built.data_uri())) ==
                failure_t::INVALID_DATA);
    }
    SECTION("a buffer shorter than it says") {
        builder_t built;
        built.positions({ 0, 0, 0, 1, 0, 0, 0, 1, 0 });
        const std::string json = built.json("", "data:application/octet-stream;base64,AAAA");
        REQUIRE(failure(json) == failure_t::INVALID_DATA);
    }
    SECTION("a buffer that isn't there") {
        builder_t built;
        built.positions({ 0, 0, 0 });
        REQUIRE(failure(built.json("", "nowhere.bin")) == failure_t::NOT_FOUND);
    }
    SECTION("not a glTF") {
        REQUIRE(failure("{ not json") == failure_t::INVALID_DATA);
        REQUIRE(failure(R"({"asset":{"version":"1.0"}})") == failure_t::INVALID_DATA);
        REQUIRE(failure("") == failure_t::INVALID_DATA);
    }
}

namespace {

bytes_t fixture(std::string_view name) {
    std::ifstream stream(std::filesystem::path(FBENGINE_ASSETS_FIXTURES) / name, std::ios::binary);
    REQUIRE(stream.good());
    return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}

} // namespace

TEST_CASE("hostile glTFs fail as they should, here and on a job", "[assets]") {
    using assets::failure_t;
    struct hostile_t {
        std::string_view name;
        failure_t failure;
    };
    const std::vector<hostile_t> cases = {
        // Indices validate would read, and sizes whose products wrap round.
        { .name = "skin_365.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "anim_input_oob.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "anim_output_oob.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "anim_sampler_oob.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "count_wraps_indices.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "count_wraps_positions.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "count_wraps_times.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "ibm_count_wraps.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "sparse_count_past_views.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "sparse_indices_at_buffer_end.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "sparse_indices_offset.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "sparse_indices_view_oob.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "sparse_values_offset.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "sparse_values_view_oob.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "target_accessor_oob.gltf", .failure = failure_t::INVALID_DATA },
        // Zeros that a few hundred bytes can ask for a billion of.
        { .name = "no_view_1e8.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "no_view_1e9.gltf", .failure = failure_t::INVALID_DATA },
        // Base64 that isn't a multiple of 4, which fastgltf's own decoder writes past its end on.
        { .name = "b64_skin105.gltf", .failure = failure_t::INVALID_DATA },
        // A joint past its skin's.
        { .name = "joints_past_skin.gltf", .failure = failure_t::INVALID_DATA },
        // An attribute the engine doesn't read, shorter than the positions, which the cooker's
        // remap would read past.
        { .name = "short_uv2.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "short_custom.gltf", .failure = failure_t::INVALID_DATA },
        // URIs that aren't the game's files, and paths that go above res://.
        { .name = "uri_abs.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "uri_drive.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "uri_file.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "uri_http.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "uri_unc.gltf", .failure = failure_t::INVALID_DATA },
        { .name = "uri_backslash.gltf", .failure = failure_t::INVALID_PATH },
        { .name = "uri_dotdot.gltf", .failure = failure_t::INVALID_PATH },
        { .name = "uri_dotdot_space.gltf", .failure = failure_t::INVALID_PATH },
        { .name = "uri_pct.gltf", .failure = failure_t::INVALID_PATH },
        // res:// is the game's, and this one's is empty.
        { .name = "uri_res.gltf", .failure = failure_t::NOT_FOUND },
    };
    vfs::vfs_t files;
    files.mount(path("res://models"), vfs::folder(FBENGINE_ASSETS_FIXTURES));
    fbe::jobs_t jobs(2);
    for (const hostile_t& hostile : cases) {
        INFO(hostile.name);
        const vfs::path_t at = path(std::string("res://models/") + std::string(hostile.name));
        const assets::result_t<assets::model_t> here =
            assets::decode_model(at, fixture(hostile.name), files.view(), {});
        REQUIRE_FALSE(here.has_value());
        REQUIRE(here.error().failure == hostile.failure);

        // As a load does, on a job, whose work mustn't throw.
        assets::load_t load;
        load.kind = assets::load_kind_t::MODEL;
        load.path = at;
        load.files = files.view();
        jobs.wait(jobs.run([&load, &jobs] { assets::run_load(load, &jobs); }));
        REQUIRE(
            load.error.value_or(assets::error_t{ .failure = assets::failure_t::OTHER }).failure ==
            hostile.failure);
        REQUIRE(load.error.has_value());
    }
}

TEST_CASE("a data URI that isn't base64 doesn't decode", "[assets]") {
    builder_t built;
    built.positions({ 0, 0, 0, 1, 0, 0, 0, 1, 0 });
    for (const std::string_view base64 :
         { "A=", "==", "AB=", "A", "AAAAA", "AA*A", "A=AA", "AAA==" }) {
        INFO(base64);
        const std::string json = built.json(
            "", std::string("data:application/octet-stream;base64,") + std::string(base64));
        REQUIRE(failure(json) == assets::failure_t::INVALID_DATA);
    }
}

TEST_CASE("a model's parts that are tied to each other agree", "[assets]") {
    using assets::failure_t;
    builder_t built;
    const int positions = built.positions({ 0, 0, 0, 1, 0, 0, 0, 1, 0 });
    const int uvs = built.floats("VEC2", 3, { 0, 0, 1, 0, 0, 1 });
    bytes_t joint_bytes;
    test::append<std::uint8_t>(joint_bytes, { 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0 });
    const int joints = built.add("VEC4", ubyte_type, 3, joint_bytes);
    const int weights = built.floats("VEC4", 3, { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 });
    const int moved =
        built.floats("VEC3", 3, { 0, 0, 1, 0, 0, 1, 0, 0, 1 }, R"(,"min":[0,0,1],"max":[0,0,1])");
    const int times = built.floats("SCALAR", 2, { 0, 1 }, R"(,"min":[0],"max":[1])");
    const int two_weights = built.floats("SCALAR", 4, { 0, 0, 1, 1 });
    const std::string uri = built.data_uri();
    const auto p = [](int index) { return std::to_string(index); };
    const std::string image = R"(,"images":[{"uri":"data:image/png;base64,)" +
                              test::base64(test::png(1, 1, 4, { 1, 2, 3, 4 })) +
                              R"("}],"textures":[{"source":0}])";

    SECTION("a joint past the skin of a node it's drawn by") {
        // Joint 1 of a skin of one joint.
        const std::string rest =
            R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":)" + p(positions) +
            R"(,"JOINTS_0":)" + p(joints) + R"(,"WEIGHTS_0":)" + p(weights) +
            R"(}}]}],"nodes":[{"mesh":0,"skin":0},{}],"skins":[{"joints":[1]}])";
        REQUIRE(failure(built.json(rest, uri)) == failure_t::INVALID_DATA);
        // Two joints: it's there.
        const std::string fine =
            R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":)" + p(positions) +
            R"(,"JOINTS_0":)" + p(joints) + R"(,"WEIGHTS_0":)" + p(weights) +
            R"(}}]}],"nodes":[{"mesh":0,"skin":0},{},{}],"skins":[{"joints":[1,2]}])";
        REQUIRE(decoded(built.json(fine, uri)).skins.size() == 1);
    }
    SECTION("joints without weights") {
        const std::string rest = R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":)" +
                                 p(positions) + R"(,"JOINTS_0":)" + p(joints) + R"(}}]}])";
        REQUIRE(failure(built.json(rest, uri)) == failure_t::INVALID_DATA);
    }
    SECTION("primitives with different numbers of morph targets") {
        const std::string rest = R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":)" +
                                 p(positions) + R"(},"targets":[{"POSITION":)" + p(moved) +
                                 R"(}]},{"attributes":{"POSITION":)" + p(positions) + R"(}}]}])";
        REQUIRE(failure(built.json(rest, uri)) == failure_t::INVALID_DATA);
    }
    SECTION("weights that aren't one a morph target") {
        const std::string mesh =
            R"(,"meshes":[{"weights":[1,2],"primitives":[{"attributes":{"POSITION":)" +
            p(positions) + R"(},"targets":[{"POSITION":)" + p(moved) + R"(}]}]}])";
        REQUIRE(failure(built.json(mesh, uri)) == failure_t::INVALID_DATA);
        const std::string node = R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":)" +
                                 p(positions) + R"(},"targets":[{"POSITION":)" + p(moved) +
                                 R"(}]}]}],"nodes":[{"mesh":0,"weights":[1,2]}])";
        REQUIRE(failure(built.json(node, uri)) == failure_t::INVALID_DATA);
        // A clip's weights, two a keyframe, for a mesh of one target.
        const std::string clip = R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":)" +
                                 p(positions) + R"(},"targets":[{"POSITION":)" + p(moved) +
                                 R"(}]}]}],"nodes":[{"mesh":0}],"animations":[{"channels":[)"
                                 R"({"sampler":0,"target":{"node":0,"path":"weights"}}],)"
                                 R"("samplers":[{"input":)" +
                                 p(times) + R"(,"output":)" + p(two_weights) + "}]}]";
        REQUIRE(failure(built.json(clip, uri)) == failure_t::INVALID_DATA);
    }
    SECTION("a texture's UV set the primitive hasn't") {
        const auto with = [&](std::string_view attributes, std::string_view texture) {
            return built.json(R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":)" +
                                  p(positions) + std::string(attributes) +
                                  R"(},"material":0}]}],)"
                                  R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":)" +
                                  std::string(texture) + "}}]" + image,
                              uri);
        };
        // The first set, which it has, and the second, which it hasn't; a third, which a surface
        // doesn't keep, is the material's to leave out, whichever it has.
        const std::string first = R"(,"TEXCOORD_0":)" + p(uvs);
        REQUIRE(decoded(with(first, R"({"index":0})")).materials.size() == 1);
        REQUIRE(failure(with(first, R"({"index":0,"texCoord":1})")) == failure_t::INVALID_DATA);
        REQUIRE(decoded(with(first + R"(,"TEXCOORD_1":)" + p(uvs) + R"(,"TEXCOORD_2":)" + p(uvs),
                             R"({"index":0,"texCoord":2})"))
                    .materials.size() == 1);
        REQUIRE(decoded(with(first, R"({"index":0,"texCoord":2})")).materials.size() == 1);
        REQUIRE(failure(with("", R"({"index":0})")) == failure_t::INVALID_DATA);
    }
}

TEST_CASE("a long chain of nodes takes time as its nodes do", "[assets]") {
    constexpr int count = 100'000;
    builder_t built;
    built.positions({ 0, 0, 0 });
    std::string nodes = R"(,"nodes":[)";
    for (int i = 0; i < count; ++i) {
        nodes += i + 1 < count ? R"({"children":[)" + std::to_string(i + 1) + "]}," : "{}";
    }
    nodes += ']';
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(decoded(built.json(nodes, built.data_uri())).nodes.size() == count);
    // Following each node's parents up takes seconds.
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
    // And one that goes round, at the end of the chain.
    std::string round = nodes;
    round.replace(round.rfind("{}"), 2, R"({"children":[0]})");
    REQUIRE(failure(built.json(round, built.data_uri())) == assets::failure_t::INVALID_DATA);
}

TEST_CASE("a load reads its file and its sidecar through one view of the mounts", "[assets]") {
    const test::temp_folder_t first;
    const test::temp_folder_t second;
    builder_t built;
    built.positions({ 0, 0, 0, 1, 0, 0, 0, 1, 0 });
    const std::string json = built.json(
        R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}])", built.data_uri());
    first.write("tri.gltf", json);
    first.write("tri.gltf.toml", "optimize = false\n");
    second.write("tri.gltf", "not a glTF");
    second.write("tri.gltf.toml", "optimize = true\n");
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(first.path));

    assets::load_t load;
    load.kind = assets::load_kind_t::MODEL;
    load.path = path("res://tri.gltf");
    load.options.settings = true;
    load.files = files.view();
    // Mounted over it before it runs, which the load doesn't see.
    files.mount(path("res://"), vfs::folder(second.path));
    assets::run_load(load, nullptr);
    REQUIRE_FALSE(load.error.has_value());
    REQUIRE(load.settings == "optimize = false\n");
    REQUIRE(load.model.meshes.size() == 1);
}

namespace {

// A folder whose files' stamps count how many times each has been read, as a file written just
// after each read of it would: what's stamped before it's read is 0.
class counting_source_t final : public vfs::source_t {
public:
    explicit counting_source_t(const std::filesystem::path& folder)
        : m_inner(vfs::folder(folder)) {}

    vfs::result_t<std::optional<bytes_t>> read(std::span<const std::string> names,
                                               std::span<const std::string> keys,
                                               std::uint64_t most) override {
        vfs::result_t<std::optional<bytes_t>> bytes = m_inner->read(names, keys, most);
        if (bytes && *bytes) {
            const std::scoped_lock lock(m_mutex);
            ++m_reads[joined(keys)];
        }
        return bytes;
    }
    vfs::result_t<vfs::kind_t> kind(std::span<const std::string> names,
                                    std::span<const std::string> keys) override {
        return m_inner->kind(names, keys);
    }
    vfs::result_t<std::optional<std::vector<vfs::entry_t>>>
    list(std::span<const std::string> names, std::span<const std::string> keys) override {
        return m_inner->list(names, keys);
    }
    std::optional<vfs::stamp_t> stamp(std::span<const std::string> names,
                                      std::span<const std::string> keys) override {
        std::optional<vfs::stamp_t> stamp = m_inner->stamp(names, keys);
        if (stamp) {
            const std::scoped_lock lock(m_mutex);
            stamp->written = m_reads[joined(keys)];
        }
        return stamp;
    }

private:
    static std::string joined(std::span<const std::string> keys) {
        std::string text;
        for (const std::string& key : keys) {
            text += "/" + key;
        }
        return text;
    }

    std::shared_ptr<vfs::source_t> m_inner;
    std::mutex m_mutex;
    std::map<std::string, std::int64_t> m_reads;
};

} // namespace

TEST_CASE("a load's stamps are each file's from before it was read, a model's buffers' and "
          "images' too, and a failed load's",
          "[assets]") {
    const test::temp_folder_t folder;
    const triangle_t triangle;
    const std::string rest =
        R"(,"images":[{"uri":"../textures/wall.png"}],"textures":[{"source":0}],)"
        R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":2},)"
        R"("indices":3,"material":0}]}])";
    folder.write("models/tri.gltf", triangle.built.json(rest, "tri.bin"));
    folder.write("models/tri.gltf.toml", "optimize = false\n");
    folder.write("models/tri.bin", triangle.built.buffer);
    folder.write("textures/wall.png", test::png(2, 2, 4, std::vector<std::uint8_t>(16, 99)));
    // One whose image isn't there, read after its buffer.
    std::string missing = rest;
    missing.replace(missing.find("wall.png"), 8, "none.png");
    folder.write("models/bad.gltf", triangle.built.json(missing, "tri.bin"));

    // What one that has none is here.
    const vfs::stamp_t none{ .size = UINT64_MAX, .written = -1 };
    const auto stamp_of = [&](const assets::load_t& load, std::string_view key) {
        for (const auto& [read, stamp] : load.stamps) {
            if (read == key) {
                return stamp;
            }
        }
        return none;
    };
    for (const bool with_jobs : { false, true }) {
        fbe::jobs_t jobs(2);
        vfs::vfs_t files;
        files.mount(path("res://"), std::make_shared<counting_source_t>(folder.path));
        assets::load_t load;
        load.kind = assets::load_kind_t::MODEL;
        load.path = path("res://models/tri.gltf");
        load.options.settings = true;
        load.files = files.view();
        assets::run_load(load, with_jobs ? &jobs : nullptr);
        REQUIRE_FALSE(load.error.has_value());
        REQUIRE(load.stamps.size() == 4);
        for (const char* key : {
                 "res://models/tri.gltf",
                 "res://models/tri.gltf.toml",
                 "res://models/tri.bin",
                 "res://textures/wall.png",
             }) {
            INFO(key);
            // Before its read, where it's 1 after.
            REQUIRE(stamp_of(load, key).written == 0);
            REQUIRE(files.view().stamp(path(key)).value_or(none).written == 1);
        }
        REQUIRE(stamp_of(load, "res://models/tri.bin").size == triangle.built.buffer.size());

        // One that doesn't decode has what it read's stamps still.
        assets::load_t bad;
        bad.kind = assets::load_kind_t::MODEL;
        bad.path = path("res://models/bad.gltf");
        bad.files = files.view();
        assets::run_load(bad, with_jobs ? &jobs : nullptr);
        REQUIRE(bad.error.has_value());
        REQUIRE(stamp_of(bad, "res://models/bad.gltf").written == 0);
        REQUIRE(stamp_of(bad, "res://models/tri.bin").size == triangle.built.buffer.size());
        REQUIRE(stamp_of(bad, "res://textures/none.png") == none);
        // And what it read, or tried to, which reloads it when it changes, or comes.
        REQUIRE(std::ranges::contains(bad.reads, "res://models/tri.bin"));
        REQUIRE(std::ranges::contains(bad.reads, "res://textures/none.png"));
    }

    // One that isn't there has none.
    const vfs::vfs_t empty;
    assets::load_t absent;
    absent.kind = assets::load_kind_t::BYTES;
    absent.path = path("res://models/tri.bin");
    absent.files = empty.view();
    assets::run_load(absent, nullptr);
    REQUIRE(absent.error.has_value());
    REQUIRE(absent.stamps.empty());
}

TEST_CASE("a clip's frame rate is its animation's extras'", "[assets]") {
    builder_t built;
    built.positions({ 0, 0, 0 });
    const int times = built.floats("SCALAR", 2, { 0, 1 }, R"(,"min":[0],"max":[1])");
    const int moves = built.floats("VEC3", 2, { 0, 0, 0, 0, 1, 0 });
    const auto clip = [&](std::string_view extras) {
        return R"({"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}],)"
               R"("samplers":[{"input":)" +
               std::to_string(times) + R"(,"output":)" + std::to_string(moves) + "}]" +
               std::string(extras) + "}";
    };
    const std::string rest =
        R"(,"nodes":[{}],"animations":[)" + clip(R"(,"extras":{"frameRate":30})") + "," +
        clip(R"(,"extras":{"frameRate":23.976,"other":1})") + "," + clip("") + "," +
        clip(R"(,"extras":{"frameRate":-5})") + "," + clip(R"(,"extras":{"frameRate":"30"})") +
        "," + clip(R"(,"extras":{"frameRate":1e300})") + "," +
        clip(R"(,"extras":{"frameRate":0})") + "]";
    const assets::model_t model = decoded(built.json(rest, built.data_uri()));
    REQUIRE(model.clips.size() == 7);
    REQUIRE(model.clips[0].frame_rate == 30);
    REQUIRE(model.clips[1].frame_rate == 23.976F);
    for (std::size_t i = 2; i < 7; ++i) {
        INFO(i);
        REQUIRE(model.clips[i].frame_rate == 0);
    }
}

TEST_CASE("a glTF's images aren't decoded when it's asked not to", "[assets]") {
    builder_t built;
    built.positions({ 0, 0, 0 });
    const std::string rest =
        R"(,"images":[{"uri":"data:image/png;base64,)" +
        test::base64(test::png(2, 2, 4, std::vector<std::uint8_t>(16, 3))) +
        R"("}],"textures":[{"source":0}],"materials":[{"pbrMetallicRoughness":)"
        R"({"baseColorTexture":{"index":0}}}])";
    const std::string json = built.json(rest, built.data_uri());
    REQUIRE(decoded(json).images.at(0).width == 2);
    const assets::model_t unread = decoded(json, vfs::vfs_t(), { .images = false });
    REQUIRE(unread.images.size() == 1);
    REQUIRE(unread.images[0].width == 0);
    REQUIRE(unread.images[0].texels.size() == 0);
}

TEST_CASE("an image file a texture's shared of already is left undecoded", "[assets]") {
    const test::temp_folder_t folder;
    builder_t built;
    built.positions({ 0, 0, 0 });
    const std::string rest =
        R"(,"images":[{"uri":"../wall.png"},{"uri":"data:image/png;base64,)" +
        test::base64(test::png(2, 2, 4, std::vector<std::uint8_t>(16, 3))) +
        R"("}],"textures":[{"source":0},{"source":1}],"materials":[{"pbrMetallicRoughness":)"
        R"({"baseColorTexture":{"index":0}},"normalTexture":{"index":1}}])";
    const std::vector<std::uint8_t> wall = test::png(2, 2, 4, std::vector<std::uint8_t>(16, 99));
    folder.write("wall.png", wall);
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    const std::string json = built.json(rest, built.data_uri());
    const std::string file =
        assets::shared_file("res://wall.png", assets::shared_hash(wall.data(), wall.size()));

    // With none shared, it's decoded, and says where it's from; one inside the file says nothing.
    const assets::model_t first = decoded(json, files);
    REQUIRE(first.image_sources.size() == 2);
    REQUIRE(first.images[0].width == 2);
    REQUIRE(first.image_sources[0].file == file);
    REQUIRE(first.image_sources[0].path == "res://wall.png");
    REQUIRE(first.image_sources[0].options.srgb);
    REQUIRE(first.image_sources[0].encoded.empty());
    REQUIRE(first.image_sources[1].file.empty());

    // With one shared, it's asked of that file as a color, and left, its bytes kept; the one
    // inside is decoded as ever.
    std::vector<std::string> asked;
    const assets::model_options_t options{
        .shared =
            [&](std::string_view of, bool srgb) {
                asked.emplace_back(of);
                return srgb;
            },
    };
    const assets::model_t second = decoded(json, files, options);
    REQUIRE(asked == std::vector<std::string>{ file });
    REQUIRE(second.images[0].levels.empty());
    REQUIRE(second.images[0].texels.size() == 0);
    REQUIRE(second.image_sources[0].encoded == wall);
    REQUIRE(second.images[1].width == 2);

    // What's in the file is what it's known by: another's bytes are another's.
    REQUIRE(assets::shared_file("res://wall.png", assets::shared_hash(wall.data(), 8)) != file);
}

TEST_CASE("an image's sidecar says srgb at its top, or doesn't", "[assets]") {
    REQUIRE(assets::sidecar_srgb("srgb = true\n") == true);
    REQUIRE(assets::sidecar_srgb(
                "# a normal map\r\nmipmaps = true\r\n  srgb=false  # numbers\r\n") == false);
    REQUIRE(assets::sidecar_srgb("") == std::nullopt);
    REQUIRE(assets::sidecar_srgb("compression = \"bc7\"\n") == std::nullopt);
    REQUIRE(assets::sidecar_srgb("# srgb = false\n") == std::nullopt);
    REQUIRE(assets::sidecar_srgb("srgb_too = false\n") == std::nullopt);
    // Another table's isn't the image's.
    REQUIRE(assets::sidecar_srgb("[images]\nsrgb = false\n") == std::nullopt);
    REQUIRE(assets::sidecar_srgb("srgb = 1\n") == std::nullopt);
    // A key in quotes is the same key.
    REQUIRE(assets::sidecar_srgb("\"srgb\" = false\n") == false);
    REQUIRE(assets::sidecar_srgb("'srgb' = true\n") == true);
    REQUIRE(assets::sidecar_srgb("\"srgb' = true\n") == std::nullopt);
}

TEST_CASE("a glTF's image file is read as its model reads it, and its sidecar can't say otherwise",
          "[assets]") {
    const test::temp_folder_t folder;
    builder_t built;
    built.positions({ 0, 0, 0 });
    const std::string json =
        built.json(R"(,"images":[{"uri":"map.png"}],"textures":[{"source":0}],)"
                   R"("materials":[{"occlusionTexture":{"index":0}}])",
                   built.data_uri());
    folder.write("models/map.png", test::png(2, 2, 4, std::vector<std::uint8_t>(16, 99)));
    vfs::vfs_t files;
    files.mount(path("res://"), vfs::folder(folder.path));
    // Numbers, with no sidecar, and with one that says so.
    REQUIRE_FALSE(decoded(json, files).image_sources.at(0).options.srgb);
    folder.write("models/map.png.toml", std::string_view("srgb = false\n"));
    const assets::model_t agreed = decoded(json, files);
    REQUIRE(std::ranges::contains(agreed.reads, "res://models/map.png.toml"));
    // One that says it's a color is refused, by both names.
    folder.write("models/map.png.toml", std::string_view("srgb = true\n"));
    const assets::result_t<assets::model_t> refused = decode(json, files);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().failure == assets::failure_t::INVALID_DATA);
    REQUIRE(refused.error().message.contains("test.gltf"));
    REQUIRE(refused.error().message.contains("res://models/map.png.toml says srgb = true"));
    // Its cook as numbers, beside it, is what's read, where there's one.
    folder.write("models/map.png.toml", std::string_view("mipmaps = true\n"));
    folder.write("models/map.png.linear.ktx2",
                 test::png(4, 4, 4, std::vector<std::uint8_t>(64, 7)));
    const assets::model_t cooked = decoded(json, files);
    REQUIRE(cooked.images.at(0).width == 4);
    REQUIRE(cooked.image_sources.at(0).path == "res://models/map.png");
    REQUIRE(std::ranges::contains(cooked.reads, "res://models/map.png.linear.ktx2"));
}

TEST_CASE("shared textures are found by what they're of and how they're made", "[assets]") {
    const std::string file = assets::shared_file("res://shared-tests/wall.png", 7);
    const std::size_t before = assets::shared_count();
    REQUIRE_FALSE(assets::shared_known(file, true));
    assets::shared_add(file, true, "1 1 0 0 0", 41);
    assets::shared_add(file, true, "1 1 1 0 0", 42);
    REQUIRE(assets::shared_known(file, true));
    REQUIRE_FALSE(assets::shared_known(file, false));
    REQUIRE_FALSE(
        assets::shared_known(assets::shared_file("res://shared-tests/wall.png", 8), true));
    REQUIRE(assets::shared_find(file, true, "1 1 0 0 0") == 41);
    REQUIRE(assets::shared_find(file, true, "1 1 1 0 0") == 42);
    REQUIRE(assets::shared_find(file, true, "0 1 0 0 0") == 0);
    REQUIRE(assets::shared_count() == before + 2);
    // One made in another's place takes it, and the one it took it from being freed leaves it.
    assets::shared_add(file, true, "1 1 0 0 0", 43);
    assets::shared_forget(41);
    REQUIRE(assets::shared_find(file, true, "1 1 0 0 0") == 43);
    assets::shared_forget(43);
    assets::shared_forget(42);
    assets::shared_forget(42);
    REQUIRE_FALSE(assets::shared_known(file, true));
    REQUIRE(assets::shared_count() == before);
}

TEST_CASE("what fastgltf reads past a short string's end in is refused", "[assets]") {
    // An empty data URI, and an accessor's type of "", which fastgltf looked into before its
    // patch, in a debug build checked by libstdc++ too.
    builder_t built;
    built.positions({ 0, 0, 0 });
    REQUIRE(failure(built.json("", "data:application/octet-stream;base64,")) ==
            assets::failure_t::INVALID_DATA);
    const test::temp_folder_t folder;
    vfs::vfs_t files;
    files.mount(path("res://models"), vfs::folder(FBENGINE_ASSETS_FIXTURES));
    const vfs::bytes_t bytes = *files.read(path("res://models/empty_type.gltf"));
    const assets::result_t<assets::model_t> model =
        assets::decode_model(path("res://models/empty_type.gltf"), bytes, files.view(), {});
    REQUIRE_FALSE(model.has_value());
    REQUIRE(model.error().failure == assets::failure_t::INVALID_DATA);
}

TEST_CASE("a glTF's buffer there isn't the memory for fails as OTHER", "[assets]") {
    builder_t built;
    built.positions({ 0, 0, 0, 1, 0, 0, 0, 1, 0 });
    // A data URI's, which fastgltf would otherwise allocate itself in a function that can't throw.
    vfs::inject(vfs::fault_t::OUT_OF_MEMORY);
    REQUIRE(failure(built.json("", built.data_uri())) == assets::failure_t::OTHER);
    // A binary glTF's chunk, which it would otherwise read into a byte.
    const std::string json =
        built.json(R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}])", "");
    const bytes_t glb = test::glb(json, built.buffer);
    vfs::inject(vfs::fault_t::OUT_OF_MEMORY);
    const assets::result_t<assets::model_t> model =
        assets::decode_model(path("res://test.glb"), glb, vfs::view_t(), {});
    REQUIRE_FALSE(model.has_value());
    REQUIRE(model.error().failure == assets::failure_t::OTHER);
    // Once.
    REQUIRE(assets::decode_model(path("res://test.glb"), glb, vfs::view_t(), {}).has_value());
}

TEST_CASE("a sparse morph target with no buffer view decodes", "[assets]") {
    builder_t built;
    const int positions = built.positions({ 0, 0, 0, 1, 0, 0, 0, 1, 0 });
    bytes_t indices;
    test::append<std::uint16_t>(indices, { 2 });
    built.buffer.resize((built.buffer.size() + 3) / 4 * 4, 0);
    const std::size_t index_at = built.buffer.size();
    built.buffer.insert(built.buffer.end(), indices.begin(), indices.end());
    built.views.push_back(R"({"buffer":0,"byteOffset":)" + std::to_string(index_at) +
                          R"(,"byteLength":2})");
    const std::size_t index_view = built.views.size() - 1;
    bytes_t values;
    test::append<float>(values, { 7, 8, 9 });
    built.buffer.resize((built.buffer.size() + 3) / 4 * 4, 0);
    const std::size_t value_at = built.buffer.size();
    built.buffer.insert(built.buffer.end(), values.begin(), values.end());
    built.views.push_back(R"({"buffer":0,"byteOffset":)" + std::to_string(value_at) +
                          R"(,"byteLength":12})");
    const std::size_t value_view = built.views.size() - 1;
    built.accessors.push_back(
        R"({"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[7,8,9],"sparse":{"count":1,"indices":{"bufferView":)" +
        std::to_string(index_view) + R"(,"componentType":5123},"values":{"bufferView":)" +
        std::to_string(value_view) + R"(}}})");
    const std::size_t moved = built.accessors.size() - 1;
    const std::string rest =
        R"(,"meshes":[{"weights":[1],"primitives":[{"attributes":{"POSITION":)" +
        std::to_string(positions) + R"(},"targets":[{"POSITION":)" + std::to_string(moved) +
        R"(}]}]}],"nodes":[{"name":"body","mesh":0}])";
    const assets::model_t model = decoded(built.json(rest, built.data_uri()));
    const assets::morph_target_t& target = model.meshes[0].primitives[0].targets.at(0);
    REQUIRE(target.positions.size() == 3);
    CHECK(target.positions[0] == assets::vec3_t{ 0, 0, 0 });
    CHECK(target.positions[2] == assets::vec3_t{ 7, 8, 9 });
}

TEST_CASE("a glTF's images are called their names, or their URIs, or nothing", "[assets]") {
    const std::string image =
        "data:image/png;base64," + test::base64(test::png(1, 1, 4, { 255, 0, 0, 255 }));
    builder_t built;
    built.positions({ 0, 0, 0 });
    const std::string rest = R"(,"images":[{"name":"GFX\\old.bmp","uri":")" + image +
                             R"("},{"uri":")" + image + R"("}])";
    const assets::model_t model = decoded(built.json(rest, built.data_uri()));
    REQUIRE(model.image_names.size() == 2);
    CHECK(model.image_names[0] == "GFX\\old.bmp");
    CHECK(model.image_names[1].empty());
}
