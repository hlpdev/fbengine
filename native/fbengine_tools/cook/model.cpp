// Cooking models: a glTF checked first as the engine loads it, then each of its triangle lists'
// triangles ordered for the GPU's vertex cache and to draw less over themselves, and its vertices
// for the GPU's fetches, by meshoptimizer, every attribute and morph target of a primitive
// remapped as one, and the whole written as a binary glTF of one buffer, with every accessor it
// keeps repacked into it, and the images in its buffers too. Images it names by their paths stay
// named, since the cooker cooks them at the same paths.

#include "assets/model.hpp"

#include "cook/cook.hpp"
#include "cook/glb.hpp"
#include "vfs/vfs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <format>
#include <limits>
#include <meshoptimizer.h>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace fbe::tools {

namespace {

namespace gltf = fastgltf;

constexpr auto extensions =
    gltf::Extensions::KHR_texture_transform | gltf::Extensions::KHR_texture_basisu |
    gltf::Extensions::KHR_mesh_quantization | gltf::Extensions::KHR_lights_punctual |
    gltf::Extensions::KHR_materials_emissive_strength | gltf::Extensions::KHR_materials_unlit;

using blob_t = std::vector<std::byte>;

// One glTF's cooking: its buffers' bytes, and what it's repacked into, a buffer, its views, and
// the accessors it keeps, with where each old view and accessor went.
class cooker_t {
public:
    cooker_t(gltf::Asset& asset, bool optimize) : m_asset(asset), m_optimize(optimize) {}

    // The binary glTF of the asset, cooked.
    std::expected<bytes_t, std::string> cook() {
        m_view_map.assign(m_asset.bufferViews.size(), std::nullopt);
        m_accessor_map.assign(m_asset.accessors.size(), std::nullopt);
        for (gltf::Mesh& mesh : m_asset.meshes) {
            for (gltf::Primitive& primitive : mesh.primitives) {
                cook_primitive(primitive);
            }
        }
        for (gltf::Skin& skin : m_asset.skins) {
            if (skin.inverseBindMatrices) {
                skin.inverseBindMatrices = copied_accessor(*skin.inverseBindMatrices);
            }
        }
        for (gltf::Animation& animation : m_asset.animations) {
            for (gltf::AnimationSampler& sampler : animation.samplers) {
                sampler.inputAccessor = copied_accessor(sampler.inputAccessor);
                sampler.outputAccessor = copied_accessor(sampler.outputAccessor);
            }
        }
        for (gltf::Image& image : m_asset.images) {
            pack_image(image);
        }

        m_asset.bufferViews = std::move(m_views);
        m_asset.accessors = std::move(m_accessors);
        m_asset.buffers.clear();
        if (!m_blob.empty()) {
            gltf::Buffer buffer;
            buffer.byteLength = m_blob.size();
            buffer.data = gltf::sources::Vector{
                .bytes = std::move(m_blob),
                .mimeType = gltf::MimeType::GltfBuffer,
            };
            m_asset.buffers.push_back(std::move(buffer));
        }
        return write_glb(m_asset);
    }

private:
    // The bytes of the old view `view`, in its buffer, which the engine's check has seen are
    // there.
    [[nodiscard]] std::span<const std::byte> view_bytes(std::size_t view) const {
        const gltf::BufferView& found = m_asset.bufferViews[view];
        const auto& buffer =
            std::get<gltf::sources::Vector>(m_asset.buffers[found.bufferIndex].data);
        return std::span(buffer.bytes).subspan(found.byteOffset, found.byteLength);
    }

    // A new view of `bytes` at the end of the buffer, 4-byte aligned, and gives its index.
    std::size_t append(std::span<const std::byte> bytes, const gltf::Optional<std::size_t>& stride,
                       const gltf::Optional<gltf::BufferTarget>& target) {
        m_blob.resize((m_blob.size() + 3) / 4 * 4, std::byte{ 0 });
        gltf::BufferView view;
        view.bufferIndex = 0;
        view.byteOffset = m_blob.size();
        view.byteLength = bytes.size();
        view.byteStride = stride;
        view.target = target;
        m_blob.insert(m_blob.end(), bytes.begin(), bytes.end());
        m_views.push_back(std::move(view));
        return m_views.size() - 1;
    }

    // Where the old view `view` went, copied as it is the first time it's asked for.
    std::size_t copied_view(std::size_t view) {
        std::optional<std::size_t>& mapped = m_view_map[view];
        if (!mapped) {
            const gltf::BufferView& old = m_asset.bufferViews[view];
            mapped = append(view_bytes(view), old.byteStride, old.target);
        }
        return mapped.value();
    }

    static std::optional<gltf::AccessorBoundsArray>
    cloned(const std::optional<gltf::AccessorBoundsArray>& bounds) {
        if (!bounds) {
            return std::nullopt;
        }
        if (bounds->isType<double>()) {
            auto made = gltf::AccessorBoundsArray::ForType<double>(bounds->size());
            for (std::size_t i = 0; i < bounds->size(); ++i) {
                made.set<double>(i, bounds->get<double>(i));
            }
            return made;
        }
        auto made = gltf::AccessorBoundsArray::ForType<std::int64_t>(bounds->size());
        for (std::size_t i = 0; i < bounds->size(); ++i) {
            made.set<std::int64_t>(i, bounds->get<std::int64_t>(i));
        }
        return made;
    }

    // Where the old accessor `index` went, copied as it is, its views too, the first time it's
    // asked for.
    std::size_t copied_accessor(std::size_t index) {
        if (const std::optional<std::size_t> mapped = m_accessor_map[index]; mapped) {
            return *mapped;
        }
        const gltf::Accessor& old = m_asset.accessors[index];
        gltf::Accessor made;
        made.byteOffset = old.byteOffset;
        made.count = old.count;
        made.type = old.type;
        made.componentType = old.componentType;
        made.normalized = old.normalized;
        made.max = cloned(old.max);
        made.min = cloned(old.min);
        made.name = old.name;
        if (old.bufferViewIndex) {
            made.bufferViewIndex = copied_view(*old.bufferViewIndex);
        }
        if (old.sparse) {
            gltf::SparseAccessor sparse = *old.sparse;
            sparse.indicesBufferView = copied_view(sparse.indicesBufferView);
            sparse.valuesBufferView = copied_view(sparse.valuesBufferView);
            made.sparse = sparse;
        }
        m_accessors.push_back(std::move(made));
        m_accessor_map[index] = m_accessors.size() - 1;
        return m_accessors.size() - 1;
    }

    // An accessor's values, each `size` bytes, packed one after another: from its view at its
    // stride, zeros without one, and its sparse values over them.
    [[nodiscard]] blob_t elements(const gltf::Accessor& accessor, std::size_t size) const {
        blob_t packed(accessor.count * size, std::byte{ 0 });
        if (accessor.bufferViewIndex) {
            const std::span<const std::byte> view = view_bytes(*accessor.bufferViewIndex);
            const std::size_t stride =
                m_asset.bufferViews[*accessor.bufferViewIndex].byteStride.value_or(size);
            for (std::size_t i = 0; i < accessor.count; ++i) {
                std::memcpy(packed.data() + (i * size),
                            view.data() + accessor.byteOffset + (i * stride), size);
            }
        }
        if (accessor.sparse) {
            const gltf::SparseAccessor& sparse = *accessor.sparse;
            const std::span<const std::byte> indices =
                view_bytes(sparse.indicesBufferView).subspan(sparse.indicesByteOffset);
            const std::span<const std::byte> values =
                view_bytes(sparse.valuesBufferView).subspan(sparse.valuesByteOffset);
            const std::size_t width = gltf::getComponentByteSize(sparse.indexComponentType);
            for (std::size_t i = 0; i < sparse.count; ++i) {
                std::uint32_t at = 0;
                std::memcpy(&at, indices.data() + (i * width), width);
                if (at < accessor.count) {
                    std::memcpy(packed.data() + (at * size), values.data() + (i * size), size);
                }
            }
        }
        return packed;
    }

    // A component's value, at `at` in a value of `type`.
    static double component(const std::byte* at, gltf::ComponentType type) {
        const auto read = [at]<typename T>(T /*kind*/) {
            T value{};
            std::memcpy(&value, at, sizeof(T));
            return static_cast<double>(value);
        };
        switch (type) {
        case gltf::ComponentType::Byte:
            return read(std::int8_t{});
        case gltf::ComponentType::UnsignedByte:
            return read(std::uint8_t{});
        case gltf::ComponentType::Short:
            return read(std::int16_t{});
        case gltf::ComponentType::UnsignedShort:
            return read(std::uint16_t{});
        case gltf::ComponentType::UnsignedInt:
            return read(std::uint32_t{});
        case gltf::ComponentType::Float:
            return read(float{});
        default:
            return 0;
        }
    }

    // A new accessor of `count` values like `like`'s, packed in `bytes`, in a view of its own, with
    // its bounds when `bounds` asks, as POSITION's need.
    std::size_t packed_accessor(const gltf::Accessor& like, const blob_t& bytes, std::size_t count,
                                gltf::BufferTarget target, bool bounds) {
        gltf::Accessor made;
        made.count = count;
        made.type = like.type;
        made.componentType = like.componentType;
        made.normalized = like.normalized;
        made.name = like.name;
        made.bufferViewIndex = append(bytes, gltf::Optional<std::size_t>(), target);
        if (bounds && count > 0) {
            const std::size_t components = gltf::getNumComponents(like.type);
            const std::size_t width = gltf::getComponentByteSize(like.componentType);
            const bool real = like.componentType == gltf::ComponentType::Float;
            std::vector<double> least(components, std::numeric_limits<double>::infinity());
            std::vector<double> most(components, -std::numeric_limits<double>::infinity());
            for (std::size_t i = 0; i < count; ++i) {
                for (std::size_t c = 0; c < components; ++c) {
                    const double value = component(bytes.data() + (((i * components) + c) * width),
                                                   like.componentType);
                    least[c] = std::min(least[c], value);
                    most[c] = std::max(most[c], value);
                }
            }
            made.min = real ? gltf::AccessorBoundsArray::ForType<double>(components)
                            : gltf::AccessorBoundsArray::ForType<std::int64_t>(components);
            made.max = real ? gltf::AccessorBoundsArray::ForType<double>(components)
                            : gltf::AccessorBoundsArray::ForType<std::int64_t>(components);
            for (std::size_t c = 0; c < components; ++c) {
                if (real) {
                    made.min->set<double>(c, least[c]);
                    made.max->set<double>(c, most[c]);
                } else {
                    made.min->set<std::int64_t>(c, static_cast<std::int64_t>(least[c]));
                    made.max->set<std::int64_t>(c, static_cast<std::int64_t>(most[c]));
                }
            }
        }
        m_accessors.push_back(std::move(made));
        return m_accessors.size() - 1;
    }

    // A primitive's vertex stream: an attribute's accessor, its values packed, and their size.
    struct stream_t {
        gltf::Attribute* attribute = nullptr;
        blob_t bytes;
        std::size_t size = 0;
    };

    // Copies a primitive's accessors as they are.
    void copy_primitive(gltf::Primitive& primitive) {
        for (gltf::Attribute& attribute : primitive.attributes) {
            attribute.accessorIndex = copied_accessor(attribute.accessorIndex);
        }
        for (auto& target : primitive.targets) {
            for (gltf::Attribute& attribute : target) {
                attribute.accessorIndex = copied_accessor(attribute.accessorIndex);
            }
        }
        if (primitive.indicesAccessor) {
            primitive.indicesAccessor = copied_accessor(*primitive.indicesAccessor);
        }
    }

    // A triangle list's triangles ordered for the vertex cache and to draw less over themselves,
    // and its vertices for the fetches, then packed; any other primitive copied.
    void cook_primitive(gltf::Primitive& primitive) {
        const auto* position = primitive.findAttribute("POSITION");
        if (!m_optimize || primitive.type != gltf::PrimitiveType::Triangles ||
            position == primitive.attributes.end()) {
            copy_primitive(primitive);
            return;
        }
        const gltf::Accessor& positions = m_asset.accessors[position->accessorIndex];
        const std::size_t vertices = positions.count;
        std::vector<unsigned int> indices(vertices);
        if (primitive.indicesAccessor) {
            const gltf::Accessor& accessor = m_asset.accessors[*primitive.indicesAccessor];
            indices.resize(accessor.count);
            gltf::copyFromAccessor<std::uint32_t>(m_asset, accessor, indices.data());
        } else {
            std::ranges::iota(indices, 0U);
        }
        if (indices.empty() || indices.size() % 3 != 0) {
            copy_primitive(primitive);
            return;
        }

        std::vector<stream_t> streams;
        const auto add = [&](gltf::Attribute& attribute) {
            const gltf::Accessor& accessor = m_asset.accessors[attribute.accessorIndex];
            const std::size_t size =
                gltf::getElementByteSize(accessor.type, accessor.componentType);
            streams.push_back(
                { .attribute = &attribute, .bytes = elements(accessor, size), .size = size });
        };
        for (gltf::Attribute& attribute : primitive.attributes) {
            add(attribute);
        }
        for (auto& target : primitive.targets) {
            for (gltf::Attribute& attribute : target) {
                add(attribute);
            }
        }

        std::vector<unsigned int> ordered(indices.size());
        meshopt_optimizeVertexCache(ordered.data(), indices.data(), indices.size(), vertices);
        if (positions.type == gltf::AccessorType::Vec3 &&
            positions.componentType == gltf::ComponentType::Float && !positions.normalized) {
            // The positions' stream, the first of POSITION's name, which a target's come after.
            const auto points_of = std::ranges::find_if(streams, [](const stream_t& stream) {
                return stream.attribute->name == "POSITION";
            });
            std::vector<float> points(vertices * 3);
            std::memcpy(points.data(), points_of->bytes.data(), points.size() * sizeof(float));
            meshopt_optimizeOverdraw(indices.data(), ordered.data(), ordered.size(), points.data(),
                                     vertices, 3 * sizeof(float), 1.05F);
        } else {
            indices = ordered;
        }
        std::vector<unsigned int> remap(vertices);
        const std::size_t kept = meshopt_optimizeVertexFetchRemap(remap.data(), indices.data(),
                                                                  indices.size(), vertices);
        meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());

        for (stream_t& stream : streams) {
            blob_t remapped(kept * stream.size);
            meshopt_remapVertexBuffer(remapped.data(), stream.bytes.data(), vertices, stream.size,
                                      remap.data());
            const gltf::Accessor& like = m_asset.accessors[stream.attribute->accessorIndex];
            stream.attribute->accessorIndex =
                packed_accessor(like, remapped, kept, gltf::BufferTarget::ArrayBuffer,
                                stream.attribute->name == "POSITION");
        }

        // 16-bit indices while no index is 65535, which glTF keeps for restarting strips.
        gltf::Accessor like;
        like.type = gltf::AccessorType::Scalar;
        blob_t packed;
        if (kept < std::numeric_limits<std::uint16_t>::max()) {
            like.componentType = gltf::ComponentType::UnsignedShort;
            packed.resize(indices.size() * sizeof(std::uint16_t));
            for (std::size_t i = 0; i < indices.size(); ++i) {
                const auto index = static_cast<std::uint16_t>(indices[i]);
                std::memcpy(packed.data() + (i * sizeof(index)), &index, sizeof(index));
            }
        } else {
            like.componentType = gltf::ComponentType::UnsignedInt;
            packed.resize(indices.size() * sizeof(unsigned int));
            std::memcpy(packed.data(), indices.data(), packed.size());
        }
        primitive.indicesAccessor = packed_accessor(like, packed, indices.size(),
                                                    gltf::BufferTarget::ElementArrayBuffer, false);
    }

    // An image in a buffer, or a data URI's, put in the new buffer; one named by its path stays.
    void pack_image(gltf::Image& image) {
        if (const auto* view = std::get_if<gltf::sources::BufferView>(&image.data)) {
            image.data = gltf::sources::BufferView{
                .bufferViewIndex = copied_view(view->bufferViewIndex),
                .mimeType = view->mimeType,
            };
        } else if (const auto* array = std::get_if<gltf::sources::Array>(&image.data)) {
            const std::size_t at =
                append(std::span(array->bytes.data(), array->bytes.size_bytes()),
                       gltf::Optional<std::size_t>(), gltf::Optional<gltf::BufferTarget>());
            image.data =
                gltf::sources::BufferView{ .bufferViewIndex = at, .mimeType = array->mimeType };
        } else if (const auto* vector = std::get_if<gltf::sources::Vector>(&image.data)) {
            const std::size_t at = append(vector->bytes, gltf::Optional<std::size_t>(),
                                          gltf::Optional<gltf::BufferTarget>());
            image.data =
                gltf::sources::BufferView{ .bufferViewIndex = at, .mimeType = vector->mimeType };
        }
    }

    gltf::Asset& m_asset;
    bool m_optimize;
    blob_t m_blob;
    std::vector<gltf::BufferView> m_views;
    std::vector<gltf::Accessor> m_accessors;
    std::vector<std::optional<std::size_t>> m_view_map;
    std::vector<std::optional<std::size_t>> m_accessor_map;
};

// A buffer's bytes: a binary glTF's or a data URI's already, the rest read from `files`, as the
// engine reads them.
std::expected<void, std::string> read_buffers(gltf::Asset& asset, const vfs::path_t& path,
                                              const vfs::view_t& files) {
    for (gltf::Buffer& buffer : asset.buffers) {
        blob_t bytes;
        if (const auto* uri = std::get_if<gltf::sources::URI>(&buffer.data)) {
            vfs::result_t<vfs::path_t> named = vfs::resolve(path, uri->uri.string());
            if (!named) {
                return std::unexpected(named.error().message);
            }
            vfs::result_t<vfs::bytes_t> read = files.read(*named);
            if (!read) {
                return std::unexpected(read.error().message);
            }
            const auto* start = reinterpret_cast<const std::byte*>(read->data());
            bytes.assign(start, start + read->size());
        } else if (const auto* array = std::get_if<gltf::sources::Array>(&buffer.data)) {
            bytes.assign(array->bytes.data(), array->bytes.data() + array->bytes.size_bytes());
        } else if (const auto* vector = std::get_if<gltf::sources::Vector>(&buffer.data)) {
            bytes = vector->bytes;
        } else if (const auto* view = std::get_if<gltf::sources::ByteView>(&buffer.data)) {
            bytes.assign(view->bytes.begin(), view->bytes.end());
        }
        buffer.data = gltf::sources::Vector{
            .bytes = std::move(bytes),
            .mimeType = gltf::MimeType::GltfBuffer,
        };
    }
    return {};
}

} // namespace

std::expected<bytes_t, std::string> cook_model(std::span<const std::uint8_t> bytes,
                                               const vfs::path_t& path, const vfs::view_t& files,
                                               const fbe_cook_model_t& options) {
    // What the engine wouldn't load isn't cooked, and what it would is safe to read here.
    const assets::result_t<assets::model_t> checked =
        assets::decode_model(path, bytes, files, { .images = false });
    if (!checked) {
        return std::unexpected(checked.error().message);
    }
    auto data = gltf::GltfDataBuffer::FromBytes(reinterpret_cast<const std::byte*>(bytes.data()),
                                                bytes.size());
    if (data.error() != gltf::Error::None) {
        return std::unexpected(std::string(gltf::getErrorMessage(data.error())));
    }
    gltf::Parser parser(extensions);
    auto parsed = parser.loadGltf(data.get(), {}, gltf::Options::None);
    if (parsed.error() != gltf::Error::None) {
        return std::unexpected(std::string(gltf::getErrorMessage(parsed.error())));
    }
    gltf::Asset asset = std::move(parsed.get());
    if (std::expected<void, std::string> read = read_buffers(asset, path, files); !read) {
        return std::unexpected(read.error());
    }
    cooker_t cooker(asset, options.optimize);
    return cooker.cook();
}

} // namespace fbe::tools
