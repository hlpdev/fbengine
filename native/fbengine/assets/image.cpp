// Decoding images: stb_image's PNGs and JPEGs to RGBA8 (or RGBA16, for a 16-bit PNG), and
// libktx's KTX2s as they are, or transcoded from Basis Universal. stb_image keeps its failures per
// thread, so decoding on several at once is safe, and so is libktx's, once its tables are made.

#include "assets/image.hpp"

#include <algorithm>
#include <array>
#include <atomic>
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

// An image of one level, layer, and face, `bytes_a_texel` a texel.
image_t flat(std::uint32_t format, std::uint32_t width, std::uint32_t height,
             std::size_t bytes_a_texel, const void* texels) {
    const std::size_t size = std::size_t{ width } * height * bytes_a_texel;
    image_t image{ .format = format, .width = width, .height = height };
    image.levels.push_back({ .width = width, .height = height, .offset = 0, .size = size });
    image.texels.resize(size);
    std::memcpy(image.texels.data(), texels, size);
    return image;
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
    const bool wide = stbi_is_16_bit_from_memory(data, size) != 0;
    // stb_image's texels are malloc's, which free lets go.
    const std::unique_ptr<void, decltype(&stbi_image_free)> texels(
        wide ? static_cast<void*>(stbi_load_16_from_memory(data, size, &width, &height, &channels,
                                                           static_cast<int>(rgba)))
             : static_cast<void*>(stbi_load_from_memory(data, size, &width, &height, &channels,
                                                        static_cast<int>(rgba))),
        &stbi_image_free);
    if (texels == nullptr) {
        return std::unexpected(
            invalid(name, std::format("won't decode: {}", stbi_failure_reason())));
    }
    std::uint32_t format = formats::rgba16;
    if (!wide) {
        format = options.srgb ? formats::rgba8_srgb : formats::rgba8;
    }
    return flat(format, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                wide ? rgba * 2 : rgba, texels.get());
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

result_t<image_t> decode_ktx2(std::span<const std::uint8_t> bytes, const image_options_t& options,
                              std::string_view name) {
    ktxTexture2* loaded = nullptr;
    KTX_error_code code = ktxTexture2_CreateFromMemory(
        bytes.data(), bytes.size(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &loaded);
    if (code != KTX_SUCCESS) {
        return std::unexpected(
            invalid(name, std::format("won't decode: {}", ktxErrorString(code))));
    }
    const std::unique_ptr<ktxTexture2, decltype(&ktxTexture2_Destroy)> texture(
        loaded, &ktxTexture2_Destroy);
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
    image.texels.assign(texture->pData, texture->pData + texture->dataSize);
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
    // A glTF is a JSON object, after a byte order mark and white space, if it has them.
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
