// Decoding images: stb_image's PNGs and JPEGs to RGBA8 (or RGBA16, for a 16-bit PNG), and
// libktx's KTX2s as they are, or transcoded from UASTC, Basis Universal's. stb_image keeps its
// failures per thread, so decoding on several at once is safe, and so is libktx's, once its tables
// are made. Their sizes are read from their headers first, and libktx's trust in a KTX2's index
// isn't given until it's checked; BasisLZ's, ETC1S's, transcoder breaks on files it's given, so
// those aren't loaded at all.

#include "assets/image.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <climits>
#include <cstring>
#include <format>
#include <ktx.h>
#include <memory>
#include <mutex>
#include <stb_image.h>
#include <string_view>
#include <utility>

namespace fbe::assets {

namespace {

using namespace std::string_view_literals;

constexpr std::string_view png_signature = "\x89PNG\r\n\x1A\n"sv;
constexpr std::string_view ktx2_signature = "\xABKTX 20\xBB\r\n\x1A\n"sv;
constexpr std::string_view glb_signature = "glTF"sv;
constexpr std::string_view bom = "\xEF\xBB\xBF"sv;
constexpr std::size_t rgba = 4;

bool starts(std::span<const std::uint8_t> bytes, std::string_view signature) {
    return bytes.size() >= signature.size() &&
           std::ranges::equal(bytes.first(signature.size()), signature, {}, {},
                              [](char c) { return static_cast<std::uint8_t>(c); });
}

error_t invalid(std::string_view name, std::string_view why) {
    return { .failure = failure_t::INVALID_DATA, .message = std::format("{} {}", name, why) };
}

std::uint32_t u32(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= std::uint32_t{ bytes[at + i] } << (8U * i);
    }
    return value;
}

std::uint64_t u64(std::span<const std::uint8_t> bytes, std::size_t at) {
    return std::uint64_t{ u32(bytes, at) } | (std::uint64_t{ u32(bytes, at + 4) } << 32U);
}

// Whether `length` bytes from `offset` are within `size`.
bool within(std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    return offset <= size && length <= size - offset;
}

result_t<image_t> decode_stb(std::span<const std::uint8_t> bytes, const image_options_t& options,
                             std::string_view name) {
    if (bytes.size() > INT_MAX) {
        return std::unexpected(invalid(name, "is too big to decode"));
    }
    const auto* data = bytes.data();
    const auto size = static_cast<int>(bytes.size());
    int width = 0;
    int height = 0;
    int channels = 0;
    // Its header first, so what it says it is is never more than an image can be.
    if (stbi_info_from_memory(data, size, &width, &height, &channels) == 0) {
        return std::unexpected(
            invalid(name, std::format("won't decode: {}", stbi_failure_reason())));
    }
    const auto past = [](int side) { return std::cmp_greater(side, most_side); };
    if (past(width) || past(height)) {
        return std::unexpected(invalid(name, std::format("is {} by {}, past the {} an image can be",
                                                         width, height, most_side)));
    }
    const bool wide = stbi_is_16_bit_from_memory(data, size) != 0;
    void* decoded = wide ? static_cast<void*>(stbi_load_16_from_memory(
                               data, size, &width, &height, &channels, static_cast<int>(rgba)))
                         : static_cast<void*>(stbi_load_from_memory(
                               data, size, &width, &height, &channels, static_cast<int>(rgba)));
    if (decoded == nullptr) {
        return std::unexpected(
            invalid(name, std::format("won't decode: {}", stbi_failure_reason())));
    }
    // stb_image's texels are malloc's, which its free lets go, kept as they are.
    const std::shared_ptr<const void> texels(
        decoded, [](const void* made) { stbi_image_free(const_cast<void*>(made)); });
    std::uint32_t format = formats::rgba16;
    if (!wide) {
        format = options.srgb ? formats::rgba8_srgb : formats::rgba8;
    }
    const auto across = static_cast<std::uint32_t>(width);
    const auto high = static_cast<std::uint32_t>(height);
    const std::size_t length = std::size_t{ across } * high * (wide ? rgba * 2 : rgba);
    image_t image{ .format = format, .width = across, .height = high };
    image.levels.push_back({ .width = across, .height = high, .offset = 0, .size = length });
    image.texels = texels_t(texels, std::span(static_cast<const std::uint8_t*>(decoded), length));
    return image;
}

ktx_transcode_fmt_e transcode_format(transcode_t transcode) {
    switch (transcode) {
    case transcode_t::BC7:
        return KTX_TTF_BC7_RGBA;
    case transcode_t::ASTC_4X4:
        return KTX_TTF_ASTC_4x4_RGBA;
    case transcode_t::RGBA8:
        break;
    }
    return KTX_TTF_RGBA32;
}

// libktx makes Basis Universal's tables the first time it transcodes, with nothing to stop two
// threads doing it at once, so transcodes take turns until one has made them.
// The mutex is never destroyed, since workers can be decoding as the program ends.
std::atomic<bool> tables_made = false;

KTX_error_code transcode(ktxTexture2* texture, ktx_transcode_fmt_e format) {
    if (tables_made) {
        return ktxTexture2_TranscodeBasis(texture, format, 0);
    }
    static auto* const first_transcodes = new std::mutex();
    const std::scoped_lock lock(*first_transcodes);
    const KTX_error_code code = ktxTexture2_TranscodeBasis(texture, format, 0);
    if (code == KTX_SUCCESS) {
        tables_made = true;
    }
    return code;
}

// A KTX2's data format descriptor, at `offset`, `length` bytes, which the header's check has seen
// are in the file: each sample of its first block, which libktx shifts 1 by its bit length for an
// uncompressed format, one of a color model below 128, less than 32 bits. A block-compressed
// format's one sample is its whole block, as UASTC's 128 bits are.
result_t<void> check_dfd(std::span<const std::uint8_t> bytes, std::uint32_t offset,
                         std::uint32_t length, std::string_view name) {
    // Its size, then the block's header, 8 bytes, its model and planes, 16, and 16 a sample.
    constexpr std::uint32_t block = 4;
    constexpr std::uint32_t samples = block + 24;
    constexpr std::uint32_t sample = 16;
    if (length < samples) {
        return {};
    }
    constexpr std::uint32_t compressed = 128;
    if ((u32(bytes, offset + block + 8) & 0xFFU) >= compressed) {
        return {};
    }
    const std::uint32_t block_size = u32(bytes, offset + block + 4) >> 16U;
    const std::uint32_t end = std::min(length, block + block_size);
    for (std::uint32_t at = samples; at + sample <= end; at += sample) {
        if (((u32(bytes, offset + at) >> 16U) & 0xFFU) >= 32) {
            return std::unexpected(invalid(name, "has a sample of 32 bits or more"));
        }
    }
    return {};
}

// What a KTX2's header says, checked before libktx reads it, which trusts it: its sizes within
// an image's, its mips no more than its sizes, its supercompression none or Zstd, and each level,
// and its other parts, within the file.
result_t<void> check_ktx2(std::span<const std::uint8_t> bytes, std::string_view name) {
    constexpr std::size_t header = 80;
    constexpr std::size_t level_index = 24;
    constexpr std::uint32_t basis_lz = 1;
    constexpr std::uint32_t zstd = 2;
    if (bytes.size() < header) {
        return std::unexpected(invalid(name, "is cut short"));
    }
    const std::uint32_t width = u32(bytes, 20);
    const std::uint32_t height = u32(bytes, 24);
    const std::uint32_t depth = u32(bytes, 28);
    const std::uint32_t layers = u32(bytes, 32);
    const std::uint32_t faces = u32(bytes, 36);
    const std::uint32_t levels = u32(bytes, 40);
    const std::uint32_t scheme = u32(bytes, 44);
    if (scheme == basis_lz) {
        return std::unexpected(invalid(
            name, "is ETC1S, in BasisLZ, which the engine doesn't load: cook it as UASTC instead"));
    }
    if (scheme != 0 && scheme != zstd) {
        return std::unexpected(invalid(
            name, "is supercompressed as the engine doesn't load, by neither Zstd nor nothing"));
    }
    if (width == 0 || width > most_side || height > most_side || depth > most_side ||
        layers > most_layers || (faces != 1 && faces != 6)) {
        return std::unexpected(invalid(
            name,
            std::format("is {} by {} by {}, of {} layers and {} faces, which an image can't be",
                        width, height, depth, layers, faces)));
    }
    // No more than an image of most_side by most_side texels, whatever its shape.
    const std::uint64_t texels = std::uint64_t{ width } * std::max(height, 1U) *
                                 std::max(depth, 1U) * std::max(layers, 1U) * faces;
    if (texels > std::uint64_t{ most_side } * most_side) {
        return std::unexpected(
            invalid(name, std::format("has {} texels, more than an image of {} by {}", texels,
                                      most_side, most_side)));
    }
    if (std::cmp_greater(levels, std::bit_width(std::max({ width, height, depth })))) {
        return std::unexpected(
            invalid(name, std::format("has {} mip levels, more than its sizes have", levels)));
    }
    const std::uint64_t size = bytes.size();
    const std::size_t count = std::max(levels, 1U);
    if (!within(header, std::uint64_t{ count } * level_index, size)) {
        return std::unexpected(invalid(name, "is cut short"));
    }
    if (!within(u32(bytes, 48), u32(bytes, 52), size) ||
        !within(u32(bytes, 56), u32(bytes, 60), size) ||
        !within(u64(bytes, 64), u64(bytes, 72), size)) {
        return std::unexpected(invalid(name, "has a part past its end"));
    }
    for (std::size_t level = 0; level < count; ++level) {
        const std::size_t at = header + (level * level_index);
        if (!within(u64(bytes, at), u64(bytes, at + 8), size)) {
            return std::unexpected(invalid(name, std::format("has level {} past its end", level)));
        }
    }
    return check_dfd(bytes, u32(bytes, 48), u32(bytes, 52), name);
}

// A stream over bytes in memory for libktx to read, which, unlike its own, allocates nothing, so
// nothing's left when it fails. It reads from data.custom_ptr, and is at readpos.
ktxStream reader_of(std::span<const std::uint8_t> bytes) {
    ktxStream stream{ .type = eStreamTypeCustom };
    stream.read = [](ktxStream* self, void* into, ktx_size_t count) {
        const auto at = static_cast<std::size_t>(self->readpos);
        if (count > self->data.custom_ptr.size - at) {
            return KTX_FILE_UNEXPECTED_EOF;
        }
        std::memcpy(into, static_cast<const std::uint8_t*>(self->data.custom_ptr.address) + at,
                    count);
        self->readpos += static_cast<ktx_off_t>(count);
        return KTX_SUCCESS;
    };
    stream.skip = [](ktxStream* self, ktx_size_t count) {
        if (count > self->data.custom_ptr.size - static_cast<std::size_t>(self->readpos)) {
            return KTX_FILE_UNEXPECTED_EOF;
        }
        self->readpos += static_cast<ktx_off_t>(count);
        return KTX_SUCCESS;
    };
    stream.write = [](ktxStream* /*self*/, const void* /*from*/, ktx_size_t /*size*/,
                      ktx_size_t /*count*/) { return KTX_INVALID_OPERATION; };
    stream.getpos = [](ktxStream* self, ktx_off_t* const at) {
        *at = self->readpos;
        return KTX_SUCCESS;
    };
    stream.setpos = [](ktxStream* self, ktx_off_t at) {
        if (at < 0 || static_cast<std::size_t>(at) > self->data.custom_ptr.size) {
            return KTX_INVALID_OPERATION;
        }
        self->readpos = at;
        return KTX_SUCCESS;
    };
    stream.getsize = [](ktxStream* self, ktx_size_t* const size) {
        *size = self->data.custom_ptr.size;
        return KTX_SUCCESS;
    };
    stream.destruct = [](ktxStream* /*self*/) {};
    // libktx only reads through it.
    stream.data.custom_ptr.address = const_cast<std::uint8_t*>(bytes.data());
    stream.data.custom_ptr.size = bytes.size();
    stream.readpos = 0;
    stream.closeOnDestruct = KTX_FALSE;
    return stream;
}

result_t<image_t> decode_ktx2(std::span<const std::uint8_t> bytes, const image_options_t& options,
                              std::string_view name) {
    if (result_t<void> checked = check_ktx2(bytes, name); !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    ktxStream stream = reader_of(bytes);
    ktxTexture2* loaded = nullptr;
    KTX_error_code code =
        ktxTexture2_CreateFromStream(&stream, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &loaded);
    if (code != KTX_SUCCESS) {
        return std::unexpected(
            invalid(name, std::format("won't decode: {}", ktxErrorString(code))));
    }
    // The texture is kept as the image's texels, which are its.
    const std::shared_ptr<ktxTexture2> texture(loaded, &ktxTexture2_Destroy);
    if (ktxTexture2_NeedsTranscoding(texture.get())) {
        code = transcode(texture.get(), transcode_format(options.transcode));
        if (code != KTX_SUCCESS) {
            return std::unexpected(
                invalid(name, std::format("won't transcode: {}", ktxErrorString(code))));
        }
    }
    if (texture->vkFormat == 0) {
        return std::unexpected(invalid(name, "has no Vulkan format"));
    }
    // A ktxTexture2 starts as a ktxTexture does, as libktx's own ktxTexture() casts it.
    auto* base = reinterpret_cast<ktxTexture*>(texture.get());
    image_t image{
        .format = texture->vkFormat,
        .width = texture->baseWidth,
        .height = texture->baseHeight,
        .depth = texture->baseDepth,
        .layers = texture->numLayers,
        .faces = texture->numFaces,
    };
    for (std::uint32_t level = 0; level < texture->numLevels; ++level) {
        ktx_size_t offset = 0;
        code = ktxTexture_GetImageOffset(base, level, 0, 0, &offset);
        if (code != KTX_SUCCESS) {
            return std::unexpected(invalid(name, std::format("has no level {}", level)));
        }
        image.levels.push_back({
            .width = std::max(texture->baseWidth >> level, 1U),
            .height = std::max(texture->baseHeight >> level, 1U),
            .depth = std::max(texture->baseDepth >> level, 1U),
            .offset = offset,
            .size = ktxTexture_GetImageSize(base, level),
        });
    }
    image.texels = texels_t(texture, std::span(texture->pData, texture->dataSize));
    return image;
}

} // namespace

file_kind_t kind_of(std::span<const std::uint8_t> bytes) {
    if (starts(bytes, png_signature)) {
        return file_kind_t::PNG;
    }
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return file_kind_t::JPEG;
    }
    if (starts(bytes, ktx2_signature)) {
        return file_kind_t::KTX2;
    }
    if (starts(bytes, glb_signature)) {
        return file_kind_t::GLB;
    }
    // A glTF is a JSON object. A byte order mark and white space before it say so here, for the
    // error that fits, though fastgltf then refuses them.
    std::size_t at = starts(bytes, bom) ? bom.size() : 0;
    while (at < bytes.size() &&
           (bytes[at] == ' ' || bytes[at] == '\t' || bytes[at] == '\r' || bytes[at] == '\n')) {
        ++at;
    }
    return at < bytes.size() && bytes[at] == '{' ? file_kind_t::GLTF : file_kind_t::UNKNOWN;
}

result_t<image_t> decode_image(std::span<const std::uint8_t> bytes, const image_options_t& options,
                               std::string_view name) {
    switch (kind_of(bytes)) {
    case file_kind_t::PNG:
    case file_kind_t::JPEG:
        return decode_stb(bytes, options, name);
    case file_kind_t::KTX2:
        return decode_ktx2(bytes, options, name);
    case file_kind_t::GLB:
    case file_kind_t::GLTF:
    case file_kind_t::UNKNOWN:
        break;
    }
    return std::unexpected(invalid(name, "isn't an image the engine loads: a PNG, JPEG, or KTX2"));
}

} // namespace fbe::assets
