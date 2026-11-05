// glTF through fastgltf: the JSON (or a binary glTF's chunks) parsed, the buffers and images it
// names by relative paths read from res://, and then everything copied out into model_t. Every
// index, count, and size is checked before fastgltf's validate, which trusts them, or anything
// reads an accessor, and what ties one part to another (a primitive's joints to its node's skin,
// its UVs to its material's textures, its morph targets to its mesh's and nodes' weights) after,
// since the renderer and the scene trust what this gives them. fastgltf's buffers for data URIs
// and a binary glTF's chunk are the decoder's own, and so is its base64 decoding, which refuses
// what isn't base64.

#include "assets/model.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <fbengine_simdjson.h>
#include <format>
#include <limits>
#include <mutex>
#include <new>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>

// The simdjson fastgltf fetched and was built with, not one installed elsewhere, as Homebrew's in
// an Intel Mac's /usr/local/include, whose types fastgltf's wouldn't match (cmake/assets.cmake).
static_assert(std::string_view(SIMDJSON_VERSION) == std::string_view(FBE_SIMDJSON_VERSION),
              "simdjson.h isn't the one fastgltf fetched");

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

// The most values an accessor without a buffer view, all zeros, can have.
constexpr std::size_t most_zeros = std::size_t{ 1 } << 24U;

std::int32_t index_of(const gltf::Optional<std::size_t>& index) {
    return index ? static_cast<std::int32_t>(*index) : -1;
}

// Whether `count` values of `size` bytes, `stride` apart from `offset`, end within `length`.
bool fits(std::uint64_t offset, std::uint64_t count, std::uint64_t stride, std::uint64_t size,
          std::uint64_t length) {
    if (offset > length) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    if (size > length - offset) {
        return false;
    }
    return stride == 0 || count - 1 <= (length - offset - size) / stride;
}

// The bytes `text` is in base64, or nullopt when it isn't base64: 4 characters for each 3 bytes,
// of A-Z, a-z, 0-9, + and /, with one or two = at its end for 2 or 1 bytes.
std::optional<std::size_t> base64_size(std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::size_t padding = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '=') {
            if (i + 2 < text.size()) {
                return std::nullopt;
            }
            ++padding;
            continue;
        }
        const bool letter =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if ((!letter && c != '+' && c != '/') || padding > 0) {
            return std::nullopt;
        }
    }
    return (text.size() / 4 * 3) - padding;
}

std::uint32_t base64_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<std::uint32_t>(c - 'A');
    }
    if (c >= 'a' && c <= 'z') {
        return static_cast<std::uint32_t>(c - 'a' + 26);
    }
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint32_t>(c - '0' + 52);
    }
    return c == '+' ? 62 : 63;
}

// One glTF's decoding: what it's called in errors, and the parsed asset.
class decoder_t {
public:
    decoder_t(const vfs::path_t& path, const vfs::view_t& files, const model_options_t& options)
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
        m_bytes = bytes.size();
        m_chunk = has_chunk(bytes);
        parser.setUserPointer(this);
        parser.setBufferAllocationCallback(map);
        parser.setBase64DecodeCallback(decode_base64);
        parser.setExtrasParseCallback(read_extras);
        auto parsed = parser.loadGltf(data.get(), {}, gltf::Options::DecomposeNodeMatrices);
        if (m_refused) {
            return fail(*m_refused, m_refused_failure);
        }
        if (parsed.error() != gltf::Error::None) {
            return fail(gltf::getErrorMessage(parsed.error()));
        }
        m_asset = std::move(parsed.get());
        m_steps = 2 + m_asset.buffers.size() + m_asset.images.size();
        step();

        if (result_t<void> read = read_buffers(); !read) {
            return std::unexpected(std::move(read.error()));
        }
        if (result_t<void> checked = check(); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        if (const gltf::Error error = gltf::validate(m_asset); error != gltf::Error::None) {
            return fail(gltf::getErrorMessage(error));
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
            if (!all_finite(*decoded)) {
                return fail("a mesh has a vertex whose numbers aren't finite");
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
        name_nodes(model);
        for (const gltf::Skin& skin : m_asset.skins) {
            model.skins.push_back(decode_skin(skin));
        }
        for (std::size_t i = 0; i < m_asset.animations.size(); ++i) {
            result_t<clip_t> clip = decode_clip(m_asset.animations[i], model);
            if (!clip) {
                return std::unexpected(std::move(clip.error()));
            }
            clip->frame_rate = i < m_frame_rates.size() ? m_frame_rates[i] : 0;
            model.clips.push_back(std::move(*clip));
        }
        decode_cameras_and_lights(model);
        if (result_t<void> placed = check_entities(model); !placed) {
            return std::unexpected(std::move(placed.error()));
        }
        {
            const std::scoped_lock lock(m_reads_mutex);
            std::ranges::sort(m_reads);
            const auto repeated = std::ranges::unique(m_reads);
            m_reads.erase(repeated.begin(), repeated.end());
            model.reads = std::move(m_reads);
        }
        if (result_t<void> tied = check_ties(model); !tied) {
            return std::unexpected(std::move(tied.error()));
        }
        step();
        return model;
    }

private:
    // A buffer for fastgltf to put a data URI's bytes, or a binary glTF's chunk, in: one of the
    // decoder's. A data URI's base64 shorter than 4 characters makes fastgltf's size wrap round,
    // past what any URI in these bytes could hold, which is refused, given nothing that's written.
    static gltf::BufferInfo map(std::uint64_t size, void* user) {
        auto& self = *static_cast<decoder_t*>(user);
        if (size > self.m_bytes) {
            self.refuse("a data URI isn't base64");
            return { .mappedMemory = &self.m_nothing, .customId = 0 };
        }
        // A binary glTF's chunk is the first that's mapped, before its JSON's data URIs.
        const bool chunk = self.m_chunk;
        self.m_chunk = false;
        try {
            if (vfs::faulted(vfs::fault_t::OUT_OF_MEMORY)) {
                throw std::bad_alloc();
            }
            self.m_mapped.emplace_back(static_cast<std::size_t>(size));
        } catch (const std::bad_alloc&) {
            self.refuse("there isn't the memory for its buffers", failure_t::OTHER);
            // With none, fastgltf leaves out a binary glTF's chunk, which it would read into a
            // byte; a data URI's it would allocate itself, so it gets the byte nothing writes.
            return { .mappedMemory = chunk ? nullptr : &self.m_nothing, .customId = 0 };
        }
        return {
            .mappedMemory = self.m_mapped.back().data(),
            .customId = self.m_mapped.size() - 1,
        };
    }

    // Decodes a data URI's base64 into what map gave, `size` bytes, or refuses what isn't base64,
    // writing nothing.
    static void decode_base64(std::string_view text, std::uint8_t* into, std::size_t /*padding*/,
                              std::size_t size, void* user) {
        auto& self = *static_cast<decoder_t*>(user);
        if (base64_size(text) != size || self.m_refused) {
            self.refuse("a data URI isn't base64");
            return;
        }
        std::size_t out = 0;
        for (std::size_t at = 0; at + 4 <= text.size(); at += 4) {
            std::uint32_t group = 0;
            for (std::size_t i = 0; i < 4; ++i) {
                group = (group << 6U) | (text[at + i] == '=' ? 0 : base64_value(text[at + i]));
            }
            for (std::size_t i = 0; i < 3 && out < size; ++i) {
                into[out++] = static_cast<std::uint8_t>(group >> (16U - (8U * i)));
            }
        }
    }

    // What the engine reads of a part's extras: an animation's frame rate, a positive finite
    // number, as animation tools write it.
    static void read_extras(simdjson::dom::object* extras, std::size_t index,
                            gltf::Category category, void* user) {
        auto& self = *static_cast<decoder_t*>(user);
        double rate = 0;
        if (category != gltf::Category::Animations ||
            (*extras)["frameRate"].get_double().get(rate) != simdjson::SUCCESS ||
            !std::isfinite(rate) || rate <= 0 || rate > std::numeric_limits<float>::max()) {
            return;
        }
        try {
            if (self.m_frame_rates.size() <= index) {
                self.m_frame_rates.resize(index + 1, 0);
            }
            self.m_frame_rates[index] = static_cast<float>(rate);
        } catch (const std::bad_alloc&) {
            self.refuse("there isn't the memory for its clips");
        }
    }

    // Whether `bytes` are a binary glTF's with a binary chunk after its JSON's, which fastgltf
    // maps before it reads the JSON.
    static bool has_chunk(std::span<const std::uint8_t> bytes) {
        constexpr std::size_t header = 12;
        constexpr std::size_t chunk = 8;
        if (bytes.size() < header + chunk ||
            !std::ranges::equal(bytes.first(4), std::string_view("glTF"), {}, {},
                                [](char c) { return static_cast<std::uint8_t>(c); })) {
            return false;
        }
        const auto u32 = [&bytes](std::size_t at) {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes.data() + at, sizeof(value));
            return value;
        };
        const std::uint64_t json = u32(header);
        const std::uint64_t binary = header + chunk + json;
        return binary + chunk <= bytes.size() && u32(static_cast<std::size_t>(binary)) != 0;
    }

    void refuse(std::string_view why, failure_t failure = failure_t::INVALID_DATA) {
        if (!m_refused) {
            m_refused = std::string(why);
            m_refused_failure = failure;
        }
    }

    // The bytes fastgltf put in one of the decoder's buffers.
    [[nodiscard]] std::span<const std::byte> mapped(gltf::CustomBufferId id) const {
        return id < m_mapped.size() ? std::span<const std::byte>(m_mapped[id])
                                    : std::span<const std::byte>();
    }

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

    // What a URI names, read from res://: a path from the glTF's folder, or a res:// one. A URI of
    // any other scheme, or a path from the root of a disk, isn't one of the game's files.
    result_t<bytes_t> read_uri(const gltf::URI& uri) const {
        const std::string_view text = uri.string();
        const std::size_t colon = text.find(':');
        const std::size_t slash = text.find_first_of("/\\");
        const bool scheme = colon != std::string_view::npos && colon < slash;
        if (uri.isDataUri() || !uri.valid() ||
            (scheme && vfs::fold(text.substr(0, colon)) != "res") || (!scheme && slash == 0)) {
            return fail(std::format("'{}' is a URI it doesn't read: a path from its folder, or a "
                                    "res:// one, is",
                                    text));
        }
        result_t<vfs::path_t> named = vfs::resolve(m_path, text);
        if (!named) {
            return std::unexpected(std::move(named.error()));
        }
        {
            const std::scoped_lock lock(m_reads_mutex);
            m_reads.push_back(named->key());
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
            if (const auto* custom = std::get_if<gltf::sources::CustomBuffer>(&buffer.data)) {
                gltf::sources::Vector taken;
                if (custom->id < m_mapped.size()) {
                    taken.bytes = std::move(m_mapped[custom->id]);
                }
                buffer.data = std::move(taken);
            } else if (const auto* uri = std::get_if<gltf::sources::URI>(&buffer.data)) {
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

    // Every index to be there, and every accessor's bytes within its buffer view's, and its view's
    // within its buffer's, with no sum or product that wraps round, all before validate or anything
    // reads them; the nodes a forest.
    result_t<void> check() const {
        const std::size_t accessors = m_asset.accessors.size();
        const std::size_t views = m_asset.bufferViews.size();
        for (const gltf::BufferView& view : m_asset.bufferViews) {
            if (view.bufferIndex >= m_asset.buffers.size() ||
                !fits(view.byteOffset, 1, 0, view.byteLength,
                      m_asset.buffers[view.bufferIndex].byteLength)) {
                return fail("a buffer view isn't within its buffer");
            }
            if (view.byteStride && (*view.byteStride < 4 || *view.byteStride > 252)) {
                return fail("a buffer view's stride isn't 4 to 252 bytes");
            }
        }
        for (const gltf::Accessor& accessor : m_asset.accessors) {
            if (result_t<void> checked = check_accessor(accessor); !checked) {
                return checked;
            }
        }
        const auto there = [](const gltf::Optional<std::size_t>& index, std::size_t count) {
            return !index || *index < count;
        };
        for (const gltf::Mesh& mesh : m_asset.meshes) {
            for (const gltf::Primitive& primitive : mesh.primitives) {
                const auto in_accessors = [accessors](const gltf::Attribute& attribute) {
                    return attribute.accessorIndex < accessors;
                };
                if (!std::ranges::all_of(primitive.attributes, in_accessors) ||
                    !std::ranges::all_of(primitive.targets,
                                         [&](const auto& target) {
                                             return std::ranges::all_of(target, in_accessors);
                                         }) ||
                    !there(primitive.indicesAccessor, accessors)) {
                    return fail("a primitive's accessor isn't there");
                }
                if (!there(primitive.materialIndex, m_asset.materials.size())) {
                    return fail("a primitive's material isn't there");
                }
            }
        }
        for (const gltf::Image& image : m_asset.images) {
            const auto* view = std::get_if<gltf::sources::BufferView>(&image.data);
            if (view != nullptr && view->bufferViewIndex >= views) {
                return fail("an image's buffer view isn't there");
            }
        }
        for (const gltf::Texture& texture : m_asset.textures) {
            if (!there(texture.imageIndex, m_asset.images.size()) ||
                !there(texture.basisuImageIndex, m_asset.images.size()) ||
                !there(texture.samplerIndex, m_asset.samplers.size())) {
                return fail("a texture's image or sampler isn't there");
            }
        }
        for (const gltf::Material& material : m_asset.materials) {
            const std::size_t textures = m_asset.textures.size();
            const auto texture_there = [textures](const auto& info) {
                return !info || info->textureIndex < textures;
            };
            if (!texture_there(material.pbrData.baseColorTexture) ||
                !texture_there(material.pbrData.metallicRoughnessTexture) ||
                !texture_there(material.normalTexture) ||
                !texture_there(material.occlusionTexture) ||
                !texture_there(material.emissiveTexture)) {
                return fail("a material's texture isn't there");
            }
        }
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
                !there(skin.inverseBindMatrices, accessors) ||
                std::ranges::any_of(skin.joints, [this](std::size_t joint) {
                    return joint >= m_asset.nodes.size();
                })) {
                return fail("a skin's joint, skeleton, or inverse binds aren't there");
            }
        }
        for (const gltf::Animation& animation : m_asset.animations) {
            for (const gltf::AnimationSampler& sampler : animation.samplers) {
                if (sampler.inputAccessor >= accessors || sampler.outputAccessor >= accessors) {
                    return fail("a clip's keyframes aren't there");
                }
            }
            for (const gltf::AnimationChannel& channel : animation.channels) {
                if (channel.samplerIndex >= animation.samplers.size() ||
                    !there(channel.nodeIndex, m_asset.nodes.size())) {
                    return fail("a clip's sampler or node isn't there");
                }
            }
        }
        return check_forest();
    }

    // An accessor's values within its buffer view, and its sparse indices and values within theirs;
    // one without a view, its values zeros, no more than most_zeros of them.
    [[nodiscard]] result_t<void> check_accessor(const gltf::Accessor& accessor) const {
        const std::size_t element = gltf::getElementByteSize(accessor.type, accessor.componentType);
        if (element == 0) {
            return fail("an accessor's type isn't one");
        }
        if (accessor.bufferViewIndex) {
            if (*accessor.bufferViewIndex >= m_asset.bufferViews.size()) {
                return fail("an accessor's buffer view isn't there");
            }
            const gltf::BufferView& view = m_asset.bufferViews[*accessor.bufferViewIndex];
            if (!fits(accessor.byteOffset, accessor.count, view.byteStride.value_or(element),
                      element, view.byteLength)) {
                return fail("an accessor's values are past its buffer view");
            }
        } else if (accessor.count > most_zeros) {
            return fail(std::format("an accessor of zeros has {} values, more than {}",
                                    accessor.count, most_zeros));
        }
        if (!accessor.sparse) {
            return {};
        }
        const gltf::SparseAccessor& sparse = *accessor.sparse;
        const std::size_t index = gltf::getComponentByteSize(sparse.indexComponentType);
        const std::size_t views = m_asset.bufferViews.size();
        if (sparse.count > accessor.count || index == 0 || sparse.indicesBufferView >= views ||
            sparse.valuesBufferView >= views) {
            return fail("a sparse accessor's indices or values aren't there");
        }
        if (!fits(sparse.indicesByteOffset, sparse.count, index, index,
                  m_asset.bufferViews[sparse.indicesBufferView].byteLength) ||
            !fits(sparse.valuesByteOffset, sparse.count, element, element,
                  m_asset.bufferViews[sparse.valuesBufferView].byteLength)) {
            return fail("a sparse accessor's indices or values are past their buffer views");
        }
        return {};
    }

    // The nodes a forest: each a child of one node at most, and each reached from a root, which a
    // node that goes round isn't.
    [[nodiscard]] result_t<void> check_forest() const {
        const std::size_t count = m_asset.nodes.size();
        std::vector<char> parented(count, 0);
        for (std::size_t i = 0; i < count; ++i) {
            for (const std::size_t child : m_asset.nodes[i].children) {
                if (child >= count || parented[child] != 0 || child == i) {
                    return fail("its nodes aren't a forest");
                }
                parented[child] = 1;
            }
        }
        std::vector<std::size_t> next;
        for (std::size_t i = 0; i < count; ++i) {
            if (parented[i] == 0) {
                next.push_back(i);
            }
        }
        std::size_t reached = 0;
        while (!next.empty()) {
            const std::size_t node = next.back();
            next.pop_back();
            ++reached;
            next.insert(next.end(), m_asset.nodes[node].children.begin(),
                        m_asset.nodes[node].children.end());
        }
        if (reached != count) {
            return fail("its nodes go round");
        }
        return {};
    }

    // What ties one part of the model to another: a primitive's joints and weights each other's and
    // within its nodes' skins, its UV sets its material's textures', each primitive of a mesh with
    // as many morph targets, and the weights of the mesh, its nodes, and the clips that move them
    // as many as those.
    [[nodiscard]] result_t<void> check_ties(const model_t& model) const {
        std::vector<std::size_t> targets(model.meshes.size(), 0);
        for (std::size_t m = 0; m < model.meshes.size(); ++m) {
            const mesh_t& mesh = model.meshes[m];
            for (const primitive_t& primitive : mesh.primitives) {
                if (&primitive != mesh.primitives.data() &&
                    primitive.targets.size() != mesh.primitives.front().targets.size()) {
                    return fail("a mesh's primitives have different numbers of morph targets");
                }
                if (primitive.joints.empty() != primitive.weights.empty()) {
                    return fail(
                        "a primitive has joints without weights, or weights without joints");
                }
                if (result_t<void> uvs = check_uvs(primitive, model); !uvs) {
                    return uvs;
                }
            }
            targets[m] = mesh.primitives.empty() ? 0 : mesh.primitives.front().targets.size();
            if (!mesh.weights.empty() && mesh.weights.size() != targets[m]) {
                return fail("a mesh's weights aren't one a morph target");
            }
        }
        for (const node_t& node : model.nodes) {
            if (!node.weights.empty() &&
                (node.mesh < 0 ||
                 node.weights.size() != targets[static_cast<std::size_t>(node.mesh)])) {
                return fail("a node's weights aren't one a morph target of its mesh");
            }
            if (node.mesh < 0 || node.skin < 0) {
                continue;
            }
            const std::size_t joints =
                model.skins[static_cast<std::size_t>(node.skin)].joints.size();
            for (const primitive_t& primitive :
                 model.meshes[static_cast<std::size_t>(node.mesh)].primitives) {
                for (const std::array<std::uint16_t, 4>& vertex : primitive.joints) {
                    if (std::ranges::any_of(
                            vertex, [joints](std::uint16_t joint) { return joint >= joints; })) {
                        return fail("a vertex's joint is past its skin's joints");
                    }
                }
            }
        }
        for (const clip_t& clip : model.clips) {
            for (const channel_t& channel : clip.channels) {
                if (channel.property != property_t::WEIGHTS) {
                    continue;
                }
                const std::int32_t mesh = model.nodes[channel.node].mesh;
                if (mesh < 0 || channel.width != targets[static_cast<std::size_t>(mesh)] ||
                    channel.width == 0) {
                    return fail("a clip's weights aren't one a morph target of its node's mesh");
                }
            }
        }
        return {};
    }

    // A primitive's UV sets those its material's textures read: the first or the second.
    [[nodiscard]] result_t<void> check_uvs(const primitive_t& primitive,
                                           const model_t& model) const {
        if (primitive.material < 0) {
            return {};
        }
        const material_t& material = model.materials[static_cast<std::size_t>(primitive.material)];
        for (const texture_ref_t* texture : {
                 &material.base_color_texture,
                 &material.metallic_roughness_texture,
                 &material.normal_texture,
                 &material.occlusion_texture,
                 &material.emissive_texture,
             }) {
            if (texture->image < 0) {
                continue;
            }
            // Legal, and past the two a surface keeps: that map's left out when a material's made.
            if (texture->uv_set > 1) {
                continue;
            }
            if ((texture->uv_set == 0 ? primitive.uv0 : primitive.uv1).empty()) {
                return fail(std::format("a primitive's material reads UV set {}, which it hasn't",
                                        texture->uv_set));
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
        model.image_names.reserve(count);
        for (const gltf::Image& image : m_asset.images) {
            const auto* uri = std::get_if<gltf::sources::URI>(&image.data);
            model.image_names.emplace_back(!image.name.empty() || uri == nullptr
                                               ? std::string(image.name)
                                               : std::string(uri->uri.string()));
        }
        if (!m_options.images) {
            return {};
        }
        std::vector<std::optional<error_t>> errors(count);
        const auto decode_one = [&](std::size_t i) {
            // On a job, whose work mustn't throw: what an image says it has can be more than
            // there's memory for.
            try {
                if (used[i] != 0) {
                    result_t<image_t> image = decode_one_image(i, color[i] != 0);
                    if (image) {
                        model.images[i] = std::move(*image);
                    } else {
                        errors[i] = std::move(image.error());
                    }
                }
            } catch (const std::exception&) {
                errors[i] = error_t{
                    .failure = failure_t::OTHER,
                    .message =
                        std::format("there isn't the memory for {}'s image {}", m_path.text(), i),
                };
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
        } else if (const auto* custom = std::get_if<gltf::sources::CustomBuffer>(&image.data)) {
            bytes = mapped(custom->id);
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
        // Every attribute, those the engine doesn't read too, and every target's, as long as the
        // positions, as what reads a primitive's vertices, the cooker's remap among them, trusts.
        const auto as_long = [&](const auto& named) {
            return std::ranges::all_of(named, [&](const gltf::Attribute& attribute) {
                return m_asset.accessors[attribute.accessorIndex].count == count;
            });
        };
        if (!as_long(attributes) || !std::ranges::all_of(primitive.targets, as_long)) {
            return fail("a primitive has an attribute, or a morph target's, not as long as its "
                        "positions");
        }
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

    // Whether every number of a mesh's vertices is finite, as the scene's bounds and picking need.
    static bool all_finite(const mesh_t& mesh) {
        const auto finite = [](const auto& values) {
            return std::ranges::all_of(values, [](const auto& value) {
                return std::ranges::all_of(value, [](float part) { return std::isfinite(part); });
            });
        };
        return std::ranges::all_of(mesh.primitives, [&](const primitive_t& primitive) {
            return finite(primitive.positions) && finite(primitive.normals) &&
                   finite(primitive.tangents) && finite(primitive.uv0) && finite(primitive.uv1) &&
                   finite(primitive.colors) && finite(primitive.weights) &&
                   std::ranges::all_of(primitive.targets, [&](const morph_target_t& target) {
                       return finite(target.positions) && finite(target.normals) &&
                              finite(target.tangents);
                   });
        });
    }

    // Gives every node a name of its own, which clips and skins find it by: node_<index> for one
    // with none, and for the second of a name, the name with #2 after it, and so on.
    static void name_nodes(model_t& model) {
        std::unordered_set<std::string> taken;
        for (const node_t& node : model.nodes) {
            taken.insert(node.name);
        }
        std::unordered_set<std::string> seen;
        for (std::size_t at = 0; at < model.nodes.size(); ++at) {
            std::string& name = model.nodes[at].name;
            if (name.empty()) {
                name = std::format("node_{}", at);
                for (std::size_t again = 2; taken.contains(name); ++again) {
                    name = std::format("node_{}#{}", at, again);
                }
                taken.insert(name);
            } else if (seen.contains(name)) {
                const std::string first = name;
                for (std::size_t again = 2; taken.contains(name); ++again) {
                    name = std::format("{}#{}", first, again);
                }
                taken.insert(name);
            }
            seen.insert(name);
        }
    }

    // What entities can't be, of what's on its nodes: it's refused here, so a file that loads
    // spawns.
    result_t<void> check_entities(const model_t& model) const {
        constexpr float most_turn = std::numbers::pi_v<float>;
        const auto finite = [](const auto& values) {
            return std::ranges::all_of(values, [](float part) { return std::isfinite(part); });
        };
        // What a camera with no nearest starts from: fbe_camera_set_range's new camera's.
        constexpr float new_nearest = 0.1F;
        for (const camera_t& camera : model.cameras) {
            const bool shaped = camera.orthographic
                                    ? camera.magnification[1] > 0 && finite(camera.magnification)
                                    : camera.fov > 0 && camera.fov < most_turn;
            const float nearest = camera.z_near > 0 ? camera.z_near : new_nearest;
            if (!shaped || !std::isfinite(camera.z_near) || !std::isfinite(camera.z_far) ||
                camera.z_near < 0 || (camera.z_far > 0 && camera.z_far <= nearest)) {
                return fail(std::format("the camera \"{}\" can't see as it says", camera.name));
            }
        }
        for (const light_t& light : model.lights) {
            if (!finite(light.color) || !std::isfinite(light.intensity) || light.intensity < 0 ||
                !std::isfinite(light.range) || light.range < 0 ||
                (light.kind == light_kind_t::SPOT &&
                 (!std::isfinite(light.inner_cone) || !std::isfinite(light.outer_cone) ||
                  light.inner_cone < 0 || light.inner_cone > light.outer_cone ||
                  light.outer_cone > most_turn / 2))) {
                return fail(std::format("the light \"{}\" can't shine as it says", light.name));
            }
        }
        for (const node_t& node : model.nodes) {
            const auto& turn = node.rotation;
            if (!finite(node.translation) || !finite(turn) || !finite(node.scale) ||
                (turn[0] == 0 && turn[1] == 0 && turn[2] == 0 && turn[3] == 0)) {
                return fail(std::format("the node \"{}\" can't be placed as it says", node.name));
            }
        }
        return {};
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
    const vfs::view_t& m_files;
    const model_options_t& m_options;
    gltf::Asset m_asset;
    // The size of the glTF's bytes; the buffers fastgltf put its data URIs and a binary glTF's
    // chunk in, by their ids; where it puts what's refused; and why it refused what it read, if it
    // did.
    std::size_t m_bytes = 0;
    std::vector<std::vector<std::byte>> m_mapped;
    std::byte m_nothing{};
    bool m_chunk = false;
    std::optional<std::string> m_refused;
    failure_t m_refused_failure = failure_t::INVALID_DATA;
    // Its animations' frame rates from their extras, by index; 0 for none.
    std::vector<float> m_frame_rates;
    // What it's read, by key, which images' decodes on the jobs add to too.
    mutable std::mutex m_reads_mutex;
    mutable std::vector<std::string> m_reads;
    std::size_t m_steps = 1;
    std::atomic<std::size_t> m_done = 0;
};

} // namespace

result_t<model_t> decode_model(const vfs::path_t& path, std::span<const std::uint8_t> bytes,
                               const vfs::view_t& files, const model_options_t& options) {
    decoder_t decoder(path, files, options);
    return decoder.decode(bytes);
}

} // namespace fbe::assets
