#pragma once

// A glTF decoded on the CPU: its meshes, materials and their images, nodes and scenes, skins, and
// clips, as the scene and the renderer take them. Its units and axes are the engine's: meters,
// right-handed, Y up. Indices into its arrays are -1 for none.

#include "assets/image.hpp"
#include "jobs/jobs.hpp"
#include "vfs/vfs.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fbe::assets {

using vec2_t = std::array<float, 2>;
using vec3_t = std::array<float, 3>;
using vec4_t = std::array<float, 4>;
/// Column after column.
using mat4_t = std::array<float, 16>;

/// How a primitive's indices make shapes. Strips and fans are made lists as they're decoded.
enum class topology_t : std::uint8_t { TRIANGLES, LINES, POINTS };

/// Where a morph target moves its primitive's vertices, as differences; empty for what it
/// doesn't move.
struct morph_target_t {
    std::vector<vec3_t> positions;
    std::vector<vec3_t> normals;
    std::vector<vec3_t> tangents;
};

/// A primitive's vertices, an attribute an array, each as long as positions or empty, and its
/// indices. UVs are from the top left of a texture; colors are linear.
struct primitive_t {
    topology_t topology = topology_t::TRIANGLES;
    std::vector<vec3_t> positions;
    /// The flat normals of its triangles when the file has none, which takes its vertices apart.
    std::vector<vec3_t> normals;
    /// xyz, and w the sign of the bitangent.
    std::vector<vec4_t> tangents;
    std::vector<vec2_t> uv0;
    std::vector<vec2_t> uv1;
    std::vector<vec4_t> colors;
    std::vector<std::array<std::uint16_t, 4>> joints;
    std::vector<vec4_t> weights;
    std::vector<std::uint32_t> indices;
    std::int32_t material = -1;
    std::vector<morph_target_t> targets;
};

struct mesh_t {
    std::string name;
    std::vector<primitive_t> primitives;
    /// Its morph targets' weights when its node gives none.
    std::vector<float> weights;
};

/// glTF's filters and wraps, by their numbers.
struct sampler_t {
    /// 9728 NEAREST, 9729 LINEAR; 0 for the renderer's own.
    std::int32_t mag = 0;
    /// Those and the four MIPMAP ones, 9984 to 9987; 0 for the renderer's own.
    std::int32_t min = 0;
    /// 10497 REPEAT, 33071 CLAMP_TO_EDGE, 33648 MIRRORED_REPEAT.
    std::int32_t wrap_u = 10497;
    std::int32_t wrap_v = 10497;
};

/// A texture a material reads: its image, sampler, UV set, and how its UVs are moved first.
struct texture_ref_t {
    std::int32_t image = -1;
    std::int32_t sampler = -1;
    std::uint32_t uv_set = 0;
    vec2_t offset = { 0, 0 };
    vec2_t scale = { 1, 1 };
    float rotation = 0;
};

enum class alpha_t : std::uint8_t { OPAQUE, MASK, BLEND };

struct material_t {
    std::string name;
    vec4_t base_color = { 1, 1, 1, 1 };
    texture_ref_t base_color_texture;
    float metallic = 1;
    float roughness = 1;
    /// Roughness in G, metalness in B.
    texture_ref_t metallic_roughness_texture;
    texture_ref_t normal_texture;
    float normal_scale = 1;
    texture_ref_t occlusion_texture;
    float occlusion_strength = 1;
    /// Its color times its strength.
    vec3_t emissive = { 0, 0, 0 };
    texture_ref_t emissive_texture;
    alpha_t alpha = alpha_t::OPAQUE;
    float alpha_cutoff = 0.5F;
    bool double_sided = false;
    bool unlit = false;
};

struct node_t {
    std::string name;
    vec3_t translation = { 0, 0, 0 };
    /// x, y, z, w.
    vec4_t rotation = { 0, 0, 0, 1 };
    vec3_t scale = { 1, 1, 1 };
    std::int32_t mesh = -1;
    std::int32_t skin = -1;
    std::int32_t camera = -1;
    std::int32_t light = -1;
    std::vector<std::uint32_t> children;
    std::vector<float> weights;
};

struct scene_t {
    std::string name;
    std::vector<std::uint32_t> nodes;
};

struct skin_t {
    std::string name;
    std::vector<std::uint32_t> joints;
    /// One a joint, identities when the file has none.
    std::vector<mat4_t> inverse_binds;
    std::int32_t skeleton = -1;
};

/// What of a node a channel moves.
enum class property_t : std::uint8_t { TRANSLATION, ROTATION, SCALE, WEIGHTS };
enum class interpolation_t : std::uint8_t { STEP, LINEAR, CUBIC };

/// What a clip moves of one node: its keyframes' times, in seconds, and values, each `width`
/// floats (3, 4, or the node's morph targets), and for CUBIC, the in and out tangents around each.
struct channel_t {
    std::uint32_t node = 0;
    property_t property = property_t::TRANSLATION;
    interpolation_t interpolation = interpolation_t::LINEAR;
    std::uint32_t width = 3;
    std::vector<float> times;
    std::vector<float> values;
};

struct clip_t {
    std::string name;
    std::vector<channel_t> channels;
    /// Its last keyframe's time.
    float length = 0;
    /// The frames a second it was made at, as its glTF's extras give it (`"frameRate": 30`); 0
    /// when they give none, or one that isn't a positive finite number.
    float frame_rate = 0;
};

struct camera_t {
    std::string name;
    bool orthographic = false;
    /// Perspective's vertical field of view in radians, and its aspect, 0 for the viewport's.
    float fov = 0;
    float aspect = 0;
    /// Orthographic's half width and height.
    vec2_t magnification = { 1, 1 };
    float z_near = 0.01F;
    /// 0 for infinite.
    float z_far = 0;
};

enum class light_kind_t : std::uint8_t { DIRECTIONAL, POINT, SPOT };

/// KHR_lights_punctual's lights, shining down their node's -Z.
struct light_t {
    std::string name;
    light_kind_t kind = light_kind_t::POINT;
    vec3_t color = { 1, 1, 1 };
    /// Candela for point and spot lights, lux for directional ones.
    float intensity = 1;
    /// 0 for no end.
    float range = 0;
    float inner_cone = 0;
    float outer_cone = 0.785398F;
};

/// Where a model's image is from, for one that's a file of its own, a URI image, which the models
/// naming it share a texture of.
struct image_source_t {
    /// What its texture's shared by, shared_file's of its res:// path and its bytes; "" for an
    /// image inside its model's file, which isn't shared.
    std::string file;
    /// Its res:// path, normalized.
    std::string path;
    /// As it's decoded: whether as a color, and what a Basis Universal KTX2 is transcoded to.
    image_options_t options;
    /// Its file's bytes, for one left undecoded since a texture of it was there already, which
    /// are decoded if that's gone by the time it's wanted, or is made another way.
    std::vector<std::uint8_t> encoded;
};

struct model_t {
    std::vector<mesh_t> meshes;
    std::vector<material_t> materials;
    std::vector<image_t> images;
    /// What each image's called: its name, or for none, its URI, percents decoded, or "".
    std::vector<std::string> image_names;
    /// Where each image is from, as many as there are images, for a model decoded with them.
    std::vector<image_source_t> image_sources;
    std::vector<sampler_t> samplers;
    std::vector<node_t> nodes;
    std::vector<scene_t> scenes;
    /// The scene to show, or 0 when there are scenes and the file names none.
    std::int32_t scene = -1;
    std::vector<skin_t> skins;
    std::vector<clip_t> clips;
    std::vector<camera_t> cameras;
    std::vector<light_t> lights;
    /// The res:// paths, normalized, of what it read besides its own file, its buffers' and its
    /// images', which hot reload loads it again for when they change.
    std::vector<std::string> reads;
};

/// What's after an image file's name in the name of its cook as numbers, not a color, beside it in
/// a pak: the cooker's, for an image that models read both ways, as wall.png.linear.ktx2.
inline constexpr std::string_view linear_cook = ".linear.ktx2";

/// Whether an image's sidecar, its import settings' TOML, says `srgb`, at its top, and what; none
/// for one that doesn't say.
std::optional<bool> sidecar_srgb(std::string_view text);

struct model_options_t {
    /// What Basis Universal KTX2 images are transcoded to.
    transcode_t transcode = transcode_t::RGBA8;
    /// Decodes the images on these jobs at once, or one by one on the calling thread.
    jobs_t* jobs = nullptr;
    /// Told how much is done, from 0 to 1, as it goes, on the threads that do it.
    std::function<void(float done)> progress;
    /// Whether it decodes its images; without, each is there, and empty, as the cooker's check of
    /// a glTF has them.
    bool images = true;
    /// Whether a texture of a file, shared_file's, decoded as a color or not, is there already, on
    /// the threads that decode: then the image is left undecoded, its file's bytes in its source.
    std::function<bool(std::string_view file, bool srgb)> shared;
};

/// The glTF (or binary glTF) at `path`, whose `bytes` are read, with the buffers and images it
/// names by relative paths read from `files`. A file that isn't a glTF, or is broken, or names
/// what isn't there, fails, as INVALID_DATA, or as reading what it names fails.
result_t<model_t> decode_model(const vfs::path_t& path, std::span<const std::uint8_t> bytes,
                               const vfs::view_t& files, const model_options_t& options);

} // namespace fbe::assets
