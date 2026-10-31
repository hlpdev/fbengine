// glTF through fastgltf: the JSON (or a binary glTF's chunks) parsed, the buffers and images it
// names by relative paths read from res://, and then everything copied out into model_t, each
// index and count checked first, since the renderer and the scene trust what this gives them.

#include "assets/model.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>

// The model's vectors as fastgltf reads its accessors into them.
template <>
struct fastgltf::ElementTraits<fbe::assets::vec2_t>
    : fastgltf::ElementTraitsBase<fbe::assets::vec2_t, fastgltf::AccessorType::Vec2, float> {};
template <>
struct fastgltf::ElementTraits<fbe::assets::vec3_t>
    : fastgltf::ElementTraitsBase<fbe::assets::vec3_t, fastgltf::AccessorType::Vec3, float> {};
template <>
struct fastgltf::ElementTraits<fbe::assets::vec4_t>
    : fastgltf::ElementTraitsBase<fbe::assets::vec4_t, fastgltf::AccessorType::Vec4, float> {};
template <>
struct fastgltf::ElementTraits<fbe::assets::mat4_t>
    : fastgltf::ElementTraitsBase<fbe::assets::mat4_t, fastgltf::AccessorType::Mat4, float> {};
template <>
struct fastgltf::ElementTraits<std::array<std::uint16_t, 4>>
    : fastgltf::ElementTraitsBase<std::array<std::uint16_t, 4>, fastgltf::AccessorType::Vec4,
                                  std::uint16_t> {};

namespace fbe::assets {

namespace {

namespace gltf = fastgltf;

constexpr auto extensions =
    gltf::Extensions::KHR_texture_transform | gltf::Extensions::KHR_texture_basisu |
    gltf::Extensions::KHR_mesh_quantization | gltf::Extensions::KHR_lights_punctual |
    gltf::Extensions::KHR_materials_emissive_strength | gltf::Extensions::KHR_materials_unlit;

std::int32_t index_of(const gltf::Optional<std::size_t>& index) {
    return index ? static_cast<std::int32_t>(*index) : -1;
}

// One glTF's decoding: what it's called in errors, and the parsed asset.
class decoder_t {
public:
    decoder_t(const vfs::path_t& path, const vfs::vfs_t& files, const model_options_t& options)
        : m_path(path), m_files(files), m_options(options) {}

    result_t<model_t> decode(std::span<const std::uint8_t> bytes) {
        if (bytes.empty()) {
            return fail("it's empty");
        }
        auto data = gltf::GltfDataBuffer::FromBytes(
            reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
        if (data.error() != gltf::Error::None) {
            return fail(gltf::getErrorMessage(data.error()));
        }
        gltf::Parser parser(extensions);
        auto parsed = parser.loadGltf(data.get(), {}, gltf::Options::DecomposeNodeMatrices);
        if (parsed.error() != gltf::Error::None) {
            return fail(gltf::getErrorMessage(parsed.error()));
        }
        m_asset = std::move(parsed.get());
        m_steps = 2 + m_asset.buffers.size() + m_asset.images.size();
        step();

        if (result_t<void> read = read_buffers(); !read) {
            return std::unexpected(std::move(read.error()));
        }
        if (const gltf::Error error = gltf::validate(m_asset); error != gltf::Error::None) {
            return fail(gltf::getErrorMessage(error));
        }
        if (result_t<void> checked = check(); !checked) {
            return std::unexpected(std::move(checked.error()));
        }

        model_t model;
        if (result_t<void> images = decode_images(model); !images) {
            return std::unexpected(std::move(images.error()));
        }
        for (const gltf::Mesh& mesh : m_asset.meshes) {
            result_t<mesh_t> decoded = decode_mesh(mesh);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            model.meshes.push_back(std::move(*decoded));
        }
        for (const gltf::Material& material : m_asset.materials) {
            model.materials.push_back(decode_material(material));
        }
        for (const gltf::Sampler& sampler : m_asset.samplers) {
            model.samplers.push_back({
                .mag = sampler.magFilter ? static_cast<std::int32_t>(*sampler.magFilter) : 0,
                .min = sampler.minFilter ? static_cast<std::int32_t>(*sampler.minFilter) : 0,
                .wrap_u = static_cast<std::int32_t>(sampler.wrapS),
                .wrap_v = static_cast<std::int32_t>(sampler.wrapT),
            });
        }
        decode_nodes(model);
        for (const gltf::Skin& skin : m_asset.skins) {
            model.skins.push_back(decode_skin(skin));
        }
        for (const gltf::Animation& animation : m_asset.animations) {
            result_t<clip_t> clip = decode_clip(animation, model);
            if (!clip) {
                return std::unexpected(std::move(clip.error()));
            }
            model.clips.push_back(std::move(*clip));
        }
        decode_cameras_and_lights(model);
        step();
        return model;
    }

private:
    [[nodiscard]] std::unexpected<error_t> fail(std::string_view why,
                                                failure_t failure = failure_t::INVALID_DATA) const {
        return std::unexpected(error_t{
            .failure = failure,
            .message = std::format("{} isn't a glTF the engine loads: {}", m_path.text(), why),
        });
    }

    void step() {
        if (m_options.progress) {
            const std::size_t done = ++m_done;
            m_options.progress(static_cast<float>(done) / static_cast<float>(m_steps));
        }
    }

    // What a relative URI names, read from res://.
    result_t<bytes_t> read_uri(const gltf::URI& uri) const {
        if (uri.isDataUri() || !uri.valid()) {
            return fail(std::format("'{}' is a URI it can't read", uri.string()));
        }
        result_t<vfs::path_t> named = vfs::resolve(m_path, uri.path());
        if (!named) {
            return std::unexpected(std::move(named.error()));
        }
        result_t<bytes_t> bytes = m_files.read(*named);
        if (!bytes) {
            return std::unexpected(error_t{
                .failure = bytes.error().failure,
                .message = std::format("{} names {}, which can't be read: {}", m_path.text(),
                                       named->text(), bytes.error().message),
            });
        }
        return bytes;
    }

    // Every buffer's bytes, a binary glTF's and data URIs' already, the rest read from res://.
    result_t<void> read_buffers() {
        for (gltf::Buffer& buffer : m_asset.buffers) {
            if (const auto* uri = std::get_if<gltf::sources::URI>(&buffer.data)) {
                result_t<bytes_t> bytes = read_uri(uri->uri);
                if (!bytes) {
                    return std::unexpected(std::move(bytes.error()));
                }
                gltf::sources::Vector read;
                read.bytes.resize(bytes->size());
                std::memcpy(read.bytes.data(), bytes->data(), bytes->size());
                buffer.data = std::move(read);
            }
            const std::size_t size = std::visit(
                [](const auto& source) -> std::size_t {
                    using source_t = std::decay_t<decltype(source)>;
                    if constexpr (std::is_same_v<source_t, gltf::sources::Array>) {
                        return source.bytes.size_bytes();
                    } else if constexpr (std::is_same_v<source_t, gltf::sources::Vector> ||
                                         std::is_same_v<source_t, gltf::sources::ByteView>) {
                        return source.bytes.size();
                    } else {
                        return 0;
                    }
                },
                buffer.data);
            if (size < buffer.byteLength) {
                return fail(
                    std::format("a buffer has {} bytes of its {}", size, buffer.byteLength));
            }
            step();
        }
        return {};
    }

    // What validate leaves: each accessor's bytes there, nodes in a forest, joints and channels'
    // nodes there.
    result_t<void> check() const {
        const auto view_fits = [this](std::size_t view) {
            return view < m_asset.bufferViews.size() &&
                   m_asset.bufferViews[view].bufferIndex < m_asset.buffers.size();
        };
        for (const gltf::Accessor& accessor : m_asset.accessors) {
            if (accessor.bufferViewIndex && !view_fits(*accessor.bufferViewIndex)) {
                return fail("an accessor's buffer view isn't there");
            }
            if (accessor.sparse && (!view_fits(accessor.sparse->indicesBufferView) ||
                                    !view_fits(accessor.sparse->valuesBufferView))) {
                return fail("a sparse accessor's buffer view isn't there");
            }
        }
        std::vector<std::int64_t> parents(m_asset.nodes.size(), -1);
        for (std::size_t i = 0; i < m_asset.nodes.size(); ++i) {
            for (const std::size_t child : m_asset.nodes[i].children) {
                if (child >= m_asset.nodes.size() || parents[child] != -1 || child == i) {
                    return fail("its nodes aren't a forest");
                }
                parents[child] = static_cast<std::int64_t>(i);
            }
        }
        const auto there = [](const gltf::Optional<std::size_t>& index, std::size_t count) {
            return !index || *index < count;
        };
        for (const gltf::Node& node : m_asset.nodes) {
            if (!there(node.meshIndex, m_asset.meshes.size()) ||
                !there(node.skinIndex, m_asset.skins.size()) ||
                !there(node.cameraIndex, m_asset.cameras.size()) ||
                !there(node.lightIndex, m_asset.lights.size())) {
                return fail("a node's mesh, skin, camera, or light isn't there");
            }
        }
        for (const gltf::Scene& scene : m_asset.scenes) {
            if (std::ranges::any_of(scene.nodeIndices, [this](std::size_t node) {
                    return node >= m_asset.nodes.size();
                })) {
                return fail("a scene's node isn't there");
            }
        }
        if (!there(m_asset.defaultScene, m_asset.scenes.size())) {
            return fail("its scene isn't there");
        }
        for (const gltf::Skin& skin : m_asset.skins) {
            if (!there(skin.skeleton, m_asset.nodes.size()) ||
                std::ranges::any_of(skin.joints, [this](std::size_t joint) {
                    return joint >= m_asset.nodes.size();
                })) {
                return fail("a skin's joint isn't there");
            }
        }
        // A node's parents, followed up, end at a root, unless they go round.
        for (std::size_t i = 0; i < m_asset.nodes.size(); ++i) {
            std::int64_t at = parents[i];
            for (std::size_t steps = 0; at != -1; ++steps) {
                if (steps > m_asset.nodes.size()) {
                    return fail("its nodes go round");
                }
                at = parents[static_cast<std::size_t>(at)];
            }
        }
        return {};
    }

    // The images its textures use, at once on the jobs if there are some, each sRGB if a color
    // texture uses it.
    result_t<void> decode_images(model_t& model) {
        const std::size_t count = m_asset.images.size();
        std::vector<char> used(count, 0);
        std::vector<char> color(count, 0);
        const auto mark = [&](const auto& info, bool is_color) {
            if (!info) {
                return;
            }
            const std::int32_t image = image_of(info->textureIndex);
            if (image >= 0) {
                used[static_cast<std::size_t>(image)] = 1;
                if (is_color) {
                    color[static_cast<std::size_t>(image)] = 1;
                }
            }
        };
        for (const gltf::Material& material : m_asset.materials) {
            mark(material.pbrData.baseColorTexture, true);
            mark(material.emissiveTexture, true);
            mark(material.pbrData.metallicRoughnessTexture, false);
            mark(material.normalTexture, false);
            mark(material.occlusionTexture, false);
        }

        model.images.resize(count);
        std::vector<std::optional<error_t>> errors(count);
        const auto decode_one = [&](std::size_t i) {
            if (used[i] != 0) {
                result_t<image_t> image = decode_one_image(i, color[i] != 0);
                if (image) {
                    model.images[i] = std::move(*image);
                } else {
                    errors[i] = std::move(image.error());
                }
            }
            step();
        };
        if (m_options.jobs != nullptr && count > 1) {
            m_options.jobs->wait(
                m_options.jobs->run_for(count, 1, [&](std::size_t begin, std::size_t end) {
                    for (std::size_t i = begin; i < end; ++i) {
                        decode_one(i);
                    }
                }));
        } else {
            for (std::size_t i = 0; i < count; ++i) {
                decode_one(i);
            }
        }
        for (std::optional<error_t>& error : errors) {
            if (error) {
                return std::unexpected(std::move(*error));
            }
        }
        return {};
    }

    result_t<image_t> decode_one_image(std::size_t index, bool srgb) const {
        const gltf::Image& image = m_asset.images[index];
        const image_options_t options{ .srgb = srgb, .transcode = m_options.transcode };
        const std::string name = std::format("{}'s image {}", m_path.text(), index);
        if (const auto* uri = std::get_if<gltf::sources::URI>(&image.data)) {
            result_t<bytes_t> bytes = read_uri(uri->uri);
            if (!bytes) {
                return std::unexpected(std::move(bytes.error()));
            }
            return decode_image(*bytes, options, name);
        }
        std::span<const std::byte> bytes;
        if (const auto* array = std::get_if<gltf::sources::Array>(&image.data)) {
            bytes = std::span(array->bytes.data(), array->bytes.size_bytes());
        } else if (const auto* view = std::get_if<gltf::sources::BufferView>(&image.data)) {
            const gltf::BufferView& buffer_view = m_asset.bufferViews[view->bufferViewIndex];
            const gltf::span<const std::byte> viewed =
                gltf::DefaultBufferDataAdapter{}(m_asset, view->bufferViewIndex);
            bytes = std::span(viewed.data(), std::min(viewed.size(), buffer_view.byteLength));
        } else {
            return fail(std::format("image {} has no data it can read", index));
        }
        return decode_image(
            std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), options,
            name);
    }

    // The image a texture shows, its KTX2 if it has one.
    [[nodiscard]] std::int32_t image_of(std::size_t texture) const {
        if (texture >= m_asset.textures.size()) {
            return -1;
        }
        const gltf::Texture& found = m_asset.textures[texture];
        const std::int32_t image =
            found.basisuImageIndex ? index_of(found.basisuImageIndex) : index_of(found.imageIndex);
        return std::cmp_less(image, m_asset.images.size()) ? image : -1;
    }

    template <typename T>
    result_t<std::vector<T>> read(std::size_t accessor_index, std::size_t count) const {
        const gltf::Accessor& accessor = m_asset.accessors[accessor_index];
        if (accessor.count != count) {
            return fail(
                std::format("an attribute has {} values for {} vertices", accessor.count, count));
        }
        if (accessor.type != gltf::ElementTraits<T>::type) {
            return fail("an attribute's type isn't what its name says");
        }
        std::vector<T> values(accessor.count);
        gltf::copyFromAccessor<T>(m_asset, accessor, values.data());
        return values;
    }

    // The attribute named `name`, or none.
    template <typename T, typename Attributes>
    result_t<std::vector<T>> read_named(const Attributes& attributes, std::string_view name,
                                        std::size_t count) const {
        for (const gltf::Attribute& attribute : attributes) {
            if (attribute.name == name) {
                return read<T>(attribute.accessorIndex, count);
            }
        }
        return std::vector<T>{};
    }

    result_t<mesh_t> decode_mesh(const gltf::Mesh& mesh) const {
        mesh_t decoded{ .name = std::string(mesh.name) };
        decoded.weights.assign(mesh.weights.begin(), mesh.weights.end());
        for (const gltf::Primitive& primitive : mesh.primitives) {
            result_t<primitive_t> made = decode_primitive(primitive);
            if (!made) {
                return std::unexpected(std::move(made.error()));
            }
            decoded.primitives.push_back(std::move(*made));
        }
        return decoded;
    }

    result_t<primitive_t> decode_primitive(const gltf::Primitive& primitive) const {
        primitive_t made{ .material = index_of(primitive.materialIndex) };
        if (std::cmp_greater_equal(made.material, m_asset.materials.size())) {
            return fail("a primitive's material isn't there");
        }
        const auto* const position = primitive.findAttribute("POSITION");
        if (position == primitive.attributes.end()) {
            return fail("a primitive has no positions");
        }
        const std::size_t count = m_asset.accessors[position->accessorIndex].count;
        const auto& attributes = primitive.attributes;
        // Each attribute, as long as the positions, or none.
        auto positions = read<vec3_t>(position->accessorIndex, count);
        auto normals = read_named<vec3_t>(attributes, "NORMAL", count);
        auto tangents = read_named<vec4_t>(attributes, "TANGENT", count);
        auto uv0 = read_named<vec2_t>(attributes, "TEXCOORD_0", count);
        auto uv1 = read_named<vec2_t>(attributes, "TEXCOORD_1", count);
        auto joints = read_named<std::array<std::uint16_t, 4>>(attributes, "JOINTS_0", count);
        auto weights = read_named<vec4_t>(attributes, "WEIGHTS_0", count);
        for (const result_t<void>& failed : {
                 status(positions),
                 status(normals),
                 status(tangents),
                 status(uv0),
                 status(uv1),
                 status(joints),
                 status(weights),
             }) {
            if (!failed) {
                return std::unexpected(failed.error());
            }
        }
        made.positions = std::move(*positions);
        made.normals = std::move(*normals);
        made.tangents = std::move(*tangents);
        made.uv0 = std::move(*uv0);
        made.uv1 = std::move(*uv1);
        made.joints = std::move(*joints);
        made.weights = std::move(*weights);
        if (result_t<void> colors = read_colors(primitive, count, made.colors); !colors) {
            return std::unexpected(std::move(colors.error()));
        }

        for (const auto& target : primitive.targets) {
            auto moved = read_named<vec3_t>(target, "POSITION", count);
            auto turned = read_named<vec3_t>(target, "NORMAL", count);
            auto tangent = read_named<vec3_t>(target, "TANGENT", count);
            for (const result_t<void>& failed :
                 { status(moved), status(turned), status(tangent) }) {
                if (!failed) {
                    return std::unexpected(failed.error());
                }
            }
            made.targets.push_back({
                .positions = std::move(*moved),
                .normals = std::move(*turned),
                .tangents = std::move(*tangent),
            });
        }

        if (result_t<void> indexed = read_indices(primitive, made); !indexed) {
            return std::unexpected(std::move(indexed.error()));
        }
        if (made.topology == topology_t::TRIANGLES && made.normals.empty()) {
            flatten(made);
        }
        return made;
    }

    template <typename T>
    static result_t<void> status(const result_t<T>& result) {
        if (!result) {
            return std::unexpected(result.error());
        }
        return {};
    }

    result_t<void> read_colors(const gltf::Primitive& primitive, std::size_t count,
                               std::vector<vec4_t>& colors) const {
        const auto* const color = primitive.findAttribute("COLOR_0");
        if (color == primitive.attributes.end()) {
            return {};
        }
        if (m_asset.accessors[color->accessorIndex].type == gltf::AccessorType::Vec3) {
            result_t<std::vector<vec3_t>> rgb = read<vec3_t>(color->accessorIndex, count);
            if (!rgb) {
                return std::unexpected(std::move(rgb.error()));
            }
            colors.reserve(rgb->size());
            for (const vec3_t& value : *rgb) {
                colors.push_back({ value[0], value[1], value[2], 1 });
            }
            return {};
        }
        result_t<std::vector<vec4_t>> rgba = read<vec4_t>(color->accessorIndex, count);
        if (!rgba) {
            return std::unexpected(std::move(rgba.error()));
        }
        colors = std::move(*rgba);
        return {};
    }

    // The indices as lists, of triangles, lines, or points, each checked against the vertices.
    result_t<void> read_indices(const gltf::Primitive& primitive, primitive_t& made) const {
        const std::size_t count = made.positions.size();
        std::vector<std::uint32_t> given;
        if (primitive.indicesAccessor) {
            const gltf::Accessor& accessor = m_asset.accessors[*primitive.indicesAccessor];
            if (accessor.type != gltf::AccessorType::Scalar) {
                return fail("a primitive's indices aren't numbers");
            }
            given.resize(accessor.count);
            gltf::copyFromAccessor<std::uint32_t>(m_asset, accessor, given.data());
            if (std::ranges::any_of(given,
                                    [count](std::uint32_t index) { return index >= count; })) {
                return fail("a primitive has an index past its vertices");
            }
        } else {
            if (count > std::numeric_limits<std::uint32_t>::max()) {
                return fail("a primitive has too many vertices");
            }
            given.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                given[i] = static_cast<std::uint32_t>(i);
            }
        }

        switch (primitive.type) {
        case gltf::PrimitiveType::Triangles:
            given.resize(given.size() - (given.size() % 3));
            made.indices = std::move(given);
            break;
        case gltf::PrimitiveType::TriangleStrip:
            // Every other triangle the other way round, so all face the same way.
            for (std::size_t i = 2; i < given.size(); ++i) {
                const bool even = i % 2 == 0;
                made.indices.insert(made.indices.end(), {
                                                            given[i - 2],
                                                            given[even ? i - 1 : i],
                                                            given[even ? i : i - 1],
                                                        });
            }
            break;
        case gltf::PrimitiveType::TriangleFan:
            for (std::size_t i = 2; i < given.size(); ++i) {
                made.indices.insert(made.indices.end(), { given[0], given[i - 1], given[i] });
            }
            break;
        case gltf::PrimitiveType::Lines:
            made.topology = topology_t::LINES;
            given.resize(given.size() - (given.size() % 2));
            made.indices = std::move(given);
            break;
        case gltf::PrimitiveType::LineStrip:
        case gltf::PrimitiveType::LineLoop:
            made.topology = topology_t::LINES;
            for (std::size_t i = 1; i < given.size(); ++i) {
                made.indices.insert(made.indices.end(), { given[i - 1], given[i] });
            }
            if (primitive.type == gltf::PrimitiveType::LineLoop && given.size() > 2) {
                made.indices.insert(made.indices.end(), { given.back(), given.front() });
            }
            break;
        case gltf::PrimitiveType::Points:
            made.topology = topology_t::POINTS;
            made.indices = std::move(given);
            break;
        }
        return {};
    }

    // Takes the triangles' vertices apart, so each has its own, and gives each its triangle's
    // normal, as glTF asks of a primitive with none.
    static void flatten(primitive_t& made) {
        const auto spread = [&made](auto& values) {
            if (values.empty()) {
                return;
            }
            std::remove_cvref_t<decltype(values)> spread_out;
            spread_out.reserve(made.indices.size());
            for (const std::uint32_t index : made.indices) {
                spread_out.push_back(values[index]);
            }
            values = std::move(spread_out);
        };
        spread(made.positions);
        spread(made.tangents);
        spread(made.uv0);
        spread(made.uv1);
        spread(made.colors);
        spread(made.joints);
        spread(made.weights);
        for (morph_target_t& target : made.targets) {
            spread(target.positions);
            spread(target.normals);
            spread(target.tangents);
        }
        made.normals.resize(made.positions.size());
        for (std::size_t i = 0; i + 2 < made.positions.size(); i += 3) {
            const vec3_t& a = made.positions[i];
            const vec3_t& b = made.positions[i + 1];
            const vec3_t& c = made.positions[i + 2];
            const vec3_t ab = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            const vec3_t ac = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            vec3_t normal = {
                (ab[1] * ac[2]) - (ab[2] * ac[1]),
                (ab[2] * ac[0]) - (ab[0] * ac[2]),
                (ab[0] * ac[1]) - (ab[1] * ac[0]),
            };
            const float length = std::hypot(normal[0], normal[1], normal[2]);
            if (length > 0) {
                normal = { normal[0] / length, normal[1] / length, normal[2] / length };
            } else {
                normal = { 0, 1, 0 };
            }
            made.normals[i] = made.normals[i + 1] = made.normals[i + 2] = normal;
        }
        made.indices.resize(made.positions.size());
        for (std::size_t i = 0; i < made.indices.size(); ++i) {
            made.indices[i] = static_cast<std::uint32_t>(i);
        }
    }

    [[nodiscard]] texture_ref_t texture_of(const gltf::TextureInfo& info) const {
        texture_ref_t ref{
            .image = image_of(info.textureIndex),
            .uv_set = static_cast<std::uint32_t>(info.texCoordIndex),
        };
        if (info.textureIndex < m_asset.textures.size()) {
            const std::int32_t sampler = index_of(m_asset.textures[info.textureIndex].samplerIndex);
            ref.sampler = std::cmp_less(sampler, m_asset.samplers.size()) ? sampler : -1;
        }
        if (info.transform) {
            ref.offset = { info.transform->uvOffset.x(), info.transform->uvOffset.y() };
            ref.scale = { info.transform->uvScale.x(), info.transform->uvScale.y() };
            ref.rotation = info.transform->rotation;
            if (info.transform->texCoordIndex) {
                ref.uv_set = static_cast<std::uint32_t>(*info.transform->texCoordIndex);
            }
        }
        return ref;
    }

    [[nodiscard]] material_t decode_material(const gltf::Material& material) const {
        material_t made{
            .name = std::string(material.name),
            .base_color = { material.pbrData.baseColorFactor.x(),
                            material.pbrData.baseColorFactor.y(),
                            material.pbrData.baseColorFactor.z(),
                            material.pbrData.baseColorFactor.w(), },
            .metallic = material.pbrData.metallicFactor,
            .roughness = material.pbrData.roughnessFactor,
            .emissive = { material.emissiveFactor.x() * material.emissiveStrength,
                          material.emissiveFactor.y() * material.emissiveStrength,
                          material.emissiveFactor.z() * material.emissiveStrength, },
            .alpha_cutoff = material.alphaCutoff,
            .double_sided = material.doubleSided,
            .unlit = material.unlit,
        };
        if (material.pbrData.baseColorTexture) {
            made.base_color_texture = texture_of(*material.pbrData.baseColorTexture);
        }
        if (material.pbrData.metallicRoughnessTexture) {
            made.metallic_roughness_texture =
                texture_of(*material.pbrData.metallicRoughnessTexture);
        }
        if (material.normalTexture) {
            made.normal_texture = texture_of(*material.normalTexture);
            made.normal_scale = material.normalTexture->scale;
        }
        if (material.occlusionTexture) {
            made.occlusion_texture = texture_of(*material.occlusionTexture);
            made.occlusion_strength = material.occlusionTexture->strength;
        }
        if (material.emissiveTexture) {
            made.emissive_texture = texture_of(*material.emissiveTexture);
        }
        switch (material.alphaMode) {
        case gltf::AlphaMode::Mask:
            made.alpha = alpha_t::MASK;
            break;
        case gltf::AlphaMode::Blend:
            made.alpha = alpha_t::BLEND;
            break;
        case gltf::AlphaMode::Opaque:
            break;
        }
        return made;
    }

    void decode_nodes(model_t& model) const {
        for (const gltf::Node& node : m_asset.nodes) {
            node_t made{
                .name = std::string(node.name),
                .mesh = index_of(node.meshIndex),
                .skin = index_of(node.skinIndex),
                .camera = index_of(node.cameraIndex),
                .light = index_of(node.lightIndex),
            };
            if (const auto* trs = std::get_if<gltf::TRS>(&node.transform)) {
                made.translation = {
                    trs->translation.x(),
                    trs->translation.y(),
                    trs->translation.z(),
                };
                made.rotation = {
                    trs->rotation.x(),
                    trs->rotation.y(),
                    trs->rotation.z(),
                    trs->rotation.w(),
                };
                made.scale = { trs->scale.x(), trs->scale.y(), trs->scale.z() };
            }
            for (const std::size_t child : node.children) {
                made.children.push_back(static_cast<std::uint32_t>(child));
            }
            made.weights.assign(node.weights.begin(), node.weights.end());
            model.nodes.push_back(std::move(made));
        }
        for (const gltf::Scene& scene : m_asset.scenes) {
            scene_t made{ .name = std::string(scene.name) };
            for (const std::size_t node : scene.nodeIndices) {
                made.nodes.push_back(static_cast<std::uint32_t>(node));
            }
            model.scenes.push_back(std::move(made));
        }
        if (m_asset.defaultScene) {
            model.scene = static_cast<std::int32_t>(*m_asset.defaultScene);
        } else if (!m_asset.scenes.empty()) {
            model.scene = 0;
        }
    }

    [[nodiscard]] skin_t decode_skin(const gltf::Skin& skin) const {
        skin_t made{ .name = std::string(skin.name), .skeleton = index_of(skin.skeleton) };
        for (const std::size_t joint : skin.joints) {
            made.joints.push_back(static_cast<std::uint32_t>(joint));
        }
        made.inverse_binds.assign(made.joints.size(),
                                  mat4_t{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 });
        if (skin.inverseBindMatrices) {
            const gltf::Accessor& accessor = m_asset.accessors[*skin.inverseBindMatrices];
            if (accessor.type == gltf::AccessorType::Mat4 &&
                accessor.count >= made.inverse_binds.size()) {
                std::vector<mat4_t> binds(accessor.count);
                gltf::copyFromAccessor<mat4_t>(m_asset, accessor, binds.data());
                std::copy_n(binds.begin(), made.inverse_binds.size(), made.inverse_binds.begin());
            }
        }
        return made;
    }

    result_t<clip_t> decode_clip(const gltf::Animation& animation, const model_t& model) const {
        clip_t clip{ .name = std::string(animation.name) };
        for (const gltf::AnimationChannel& channel : animation.channels) {
            if (!channel.nodeIndex || channel.samplerIndex >= animation.samplers.size()) {
                continue;
            }
            const gltf::AnimationSampler& sampler = animation.samplers[channel.samplerIndex];
            const gltf::Accessor& input = m_asset.accessors[sampler.inputAccessor];
            const gltf::Accessor& output = m_asset.accessors[sampler.outputAccessor];
            channel_t made{ .node = static_cast<std::uint32_t>(*channel.nodeIndex) };
            switch (sampler.interpolation) {
            case gltf::AnimationInterpolation::Step:
                made.interpolation = interpolation_t::STEP;
                break;
            case gltf::AnimationInterpolation::CubicSpline:
                made.interpolation = interpolation_t::CUBIC;
                break;
            case gltf::AnimationInterpolation::Linear:
                break;
            }
            if (input.type != gltf::AccessorType::Scalar || input.count == 0) {
                return fail("a clip's keyframe times aren't numbers");
            }
            made.times.resize(input.count);
            gltf::copyFromAccessor<float>(m_asset, input, made.times.data());
            const std::size_t keys =
                made.interpolation == interpolation_t::CUBIC ? input.count * 3 : input.count;
            switch (channel.path) {
            case gltf::AnimationPath::Translation:
            case gltf::AnimationPath::Scale:
                made.property = channel.path == gltf::AnimationPath::Translation
                                    ? property_t::TRANSLATION
                                    : property_t::SCALE;
                made.width = 3;
                if (output.type != gltf::AccessorType::Vec3 || output.count != keys) {
                    return fail("a clip's values don't match its times");
                }
                made.values = flat(read_all<vec3_t>(output));
                break;
            case gltf::AnimationPath::Rotation:
                made.property = property_t::ROTATION;
                made.width = 4;
                if (output.type != gltf::AccessorType::Vec4 || output.count != keys) {
                    return fail("a clip's values don't match its times");
                }
                made.values = flat(read_all<vec4_t>(output));
                break;
            case gltf::AnimationPath::Weights: {
                made.property = property_t::WEIGHTS;
                if (output.type != gltf::AccessorType::Scalar || output.count % keys != 0) {
                    return fail("a clip's weights don't match its times");
                }
                made.width = static_cast<std::uint32_t>(output.count / keys);
                made.values = read_all<float>(output);
                break;
            }
            default:
                continue;
            }
            if (made.node >= model.nodes.size()) {
                return fail("a clip moves a node that isn't there");
            }
            clip.length = std::max(clip.length, made.times.back());
            clip.channels.push_back(std::move(made));
        }
        return clip;
    }

    template <typename T>
    [[nodiscard]] std::vector<T> read_all(const gltf::Accessor& accessor) const {
        std::vector<T> values(accessor.count);
        gltf::copyFromAccessor<T>(m_asset, accessor, values.data());
        return values;
    }

    template <std::size_t N>
    static std::vector<float> flat(const std::vector<std::array<float, N>>& values) {
        std::vector<float> floats;
        floats.reserve(values.size() * N);
        for (const std::array<float, N>& value : values) {
            floats.insert(floats.end(), value.begin(), value.end());
        }
        return floats;
    }

    void decode_cameras_and_lights(model_t& model) const {
        for (const gltf::Camera& camera : m_asset.cameras) {
            camera_t made{ .name = std::string(camera.name) };
            if (const auto* perspective = std::get_if<gltf::Camera::Perspective>(&camera.camera)) {
                made.fov = perspective->yfov;
                made.aspect = perspective->aspectRatio ? *perspective->aspectRatio : 0;
                made.z_near = perspective->znear;
                made.z_far = perspective->zfar ? *perspective->zfar : 0;
            } else if (const auto* ortho =
                           std::get_if<gltf::Camera::Orthographic>(&camera.camera)) {
                made.orthographic = true;
                made.magnification = { ortho->xmag, ortho->ymag };
                made.z_near = ortho->znear;
                made.z_far = ortho->zfar;
            }
            model.cameras.push_back(std::move(made));
        }
        for (const gltf::Light& light : m_asset.lights) {
            light_t made{
                .name = std::string(light.name),
                .color = { light.color.x(), light.color.y(), light.color.z() },
                .intensity = light.intensity,
                .range = light.range ? *light.range : 0,
            };
            switch (light.type) {
            case gltf::LightType::Directional:
                made.kind = light_kind_t::DIRECTIONAL;
                break;
            case gltf::LightType::Spot:
                made.kind = light_kind_t::SPOT;
                made.inner_cone = light.innerConeAngle ? *light.innerConeAngle : 0;
                made.outer_cone = light.outerConeAngle ? *light.outerConeAngle : made.outer_cone;
                break;
            case gltf::LightType::Point:
                break;
            }
            model.lights.push_back(std::move(made));
        }
    }

    const vfs::path_t& m_path;
    const vfs::vfs_t& m_files;
    const model_options_t& m_options;
    gltf::Asset m_asset;
    std::size_t m_steps = 1;
    std::atomic<std::size_t> m_done = 0;
};

} // namespace

result_t<model_t> decode_model(const vfs::path_t& path, std::span<const std::uint8_t> bytes,
                               const vfs::vfs_t& files, const model_options_t& options) {
    decoder_t decoder(path, files, options);
    return decoder.decode(bytes);
}

} // namespace fbe::assets
