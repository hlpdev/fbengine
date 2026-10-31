// Cooking images: decoded by stb_image to RGBA8, their mips made in linear light, and encoded by
// libktx: Basis Universal's UASTC, which BC7 and ASTC 4x4 are transcoded from here, or the texels
// as they are, then Zstandard-compressed.

#include "assets/image.hpp"

#include "cook/cook.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <format>
#include <ktx.h>
#include <memory>
#include <stb_image.h>
#include <thread>
#include <utility>

namespace fbe::tools {

namespace {

constexpr std::size_t rgba = 4;
// How hard Zstandard works on a cooked KTX2, of its 1 to 22.
constexpr ktx_uint32_t zstd_level = 18;

// sRGB's 8-bit values in linear light.
const std::array<float, 256>& linear_of() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> values{};
        for (std::size_t i = 0; i < values.size(); ++i) {
            const float c = static_cast<float>(i) / 255.0F;
            values[i] = c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
        }
        return values;
    }();
    return table;
}

std::uint8_t srgb_of(float linear) {
    const float c =
        linear <= 0.0031308F ? linear * 12.92F : (1.055F * std::pow(linear, 1.0F / 2.4F)) - 0.055F;
    return static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0F, 1.0F) * 255.0F));
}

using texture_ptr_t = std::unique_ptr<ktxTexture2, decltype(&ktxTexture2_Destroy)>;

std::string failed(std::string_view doing, KTX_error_code code) {
    return std::format("couldn't {}: {}", doing, ktxErrorString(code));
}

// The texture of `image` and its mips, RGBA8, sRGB or linear.
std::expected<texture_ptr_t, std::string> texture_of(rgba_t image,
                                                     const fbe_cook_image_t& options) {
    const std::uint32_t largest = std::max(image.width, image.height);
    const auto levels = options.mipmaps ? static_cast<std::uint32_t>(std::bit_width(largest)) : 1U;
    ktxTextureCreateInfo info{};
    info.vkFormat = options.srgb ? 43 : 37;
    info.baseWidth = image.width;
    info.baseHeight = image.height;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = levels;
    info.numLayers = 1;
    info.numFaces = 1;
    info.isArray = KTX_FALSE;
    info.generateMipmaps = KTX_FALSE;
    ktxTexture2* made = nullptr;
    const KTX_error_code code = ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &made);
    if (code != KTX_SUCCESS) {
        return std::unexpected(failed("make a texture", code));
    }
    texture_ptr_t texture(made, &ktxTexture2_Destroy);
    for (std::uint32_t level = 0; level < levels; ++level) {
        if (level > 0) {
            image = half(image, options.srgb);
        }
        const KTX_error_code set =
            ktxTexture_SetImageFromMemory(reinterpret_cast<ktxTexture*>(texture.get()), level, 0, 0,
                                          image.texels.data(), image.texels.size());
        if (set != KTX_SUCCESS) {
            return std::unexpected(failed("set a mip", set));
        }
    }
    return texture;
}

ktx_uint32_t threads() {
    return std::max(1U, std::thread::hardware_concurrency());
}

std::expected<void, std::string> encode(ktxTexture2* texture, const fbe_cook_image_t& options) {
    const auto quality = static_cast<std::uint32_t>(std::clamp(options.quality, 0, 4));
    if (options.format != FBE_COOK_RGBA8) {
        ktxBasisParams params{};
        params.structSize = sizeof(params);
        params.threadCount = threads();
        params.uastc = KTX_TRUE;
        params.uastcFlags = static_cast<ktx_pack_uastc_flags>(quality);
        const KTX_error_code code = ktxTexture2_CompressBasisEx(texture, &params);
        if (code != KTX_SUCCESS) {
            return std::unexpected(failed("encode it", code));
        }
    }
    if (options.format == FBE_COOK_BC7 || options.format == FBE_COOK_ASTC) {
        const KTX_error_code code = ktxTexture2_TranscodeBasis(
            texture, options.format == FBE_COOK_BC7 ? KTX_TTF_BC7_RGBA : KTX_TTF_ASTC_4x4_RGBA, 0);
        if (code != KTX_SUCCESS) {
            return std::unexpected(failed("transcode it", code));
        }
    }
    const KTX_error_code code = ktxTexture2_DeflateZstd(texture, zstd_level);
    if (code != KTX_SUCCESS) {
        return std::unexpected(failed("compress it", code));
    }
    return {};
}

} // namespace

rgba_t half(const rgba_t& image, bool srgb) {
    rgba_t next{
        .width = std::max(image.width / 2, 1U),
        .height = std::max(image.height / 2, 1U),
    };
    next.texels.resize(std::size_t{ next.width } * next.height * rgba);
    const std::array<float, 256>& linear = linear_of();
    for (std::uint32_t y = 0; y < next.height; ++y) {
        for (std::uint32_t x = 0; x < next.width; ++x) {
            // Each color in linear light, weighted by its alpha, and as it is, for a texel all
            // clear, and the alphas.
            std::array<float, 3> weighted{};
            std::array<float, 3> plain{};
            float alpha = 0;
            std::uint32_t count = 0;
            // The last texel of an odd row or column takes in the one past it, which halving
            // would drop.
            const std::uint32_t across = x + 1 == next.width && image.width % 2 == 1 ? 3 : 2;
            const std::uint32_t down = y + 1 == next.height && image.height % 2 == 1 ? 3 : 2;
            for (std::uint32_t dy = 0; dy < down; ++dy) {
                for (std::uint32_t dx = 0; dx < across; ++dx) {
                    const std::uint32_t from_x = (x * 2) + dx;
                    const std::uint32_t from_y = (y * 2) + dy;
                    if (from_x >= image.width || from_y >= image.height) {
                        continue;
                    }
                    const std::size_t at = ((std::size_t{ from_y } * image.width) + from_x) * rgba;
                    const float opacity = static_cast<float>(image.texels[at + 3]) / 255.0F;
                    for (std::size_t c = 0; c < 3; ++c) {
                        const std::uint8_t value = image.texels[at + c];
                        const float color =
                            srgb ? linear.at(value) : static_cast<float>(value) / 255.0F;
                        weighted.at(c) += color * opacity;
                        plain.at(c) += color;
                    }
                    alpha += opacity;
                    ++count;
                }
            }
            const std::size_t at = ((std::size_t{ y } * next.width) + x) * rgba;
            for (std::size_t c = 0; c < 3; ++c) {
                const float mean =
                    alpha > 0 ? weighted.at(c) / alpha : plain.at(c) / static_cast<float>(count);
                next.texels[at + c] =
                    srgb ? srgb_of(mean) : static_cast<std::uint8_t>(std::lround(mean * 255.0F));
            }
            next.texels[at + 3] =
                static_cast<std::uint8_t>(std::lround(alpha / static_cast<float>(count) * 255.0F));
        }
    }
    return next;
}

std::expected<bytes_t, error_t> cook_image(std::span<const std::uint8_t> bytes,
                                           const fbe_cook_image_t& options) {
    if (options.format < FBE_COOK_UASTC || options.format > FBE_COOK_RGBA8) {
        return std::unexpected(other(std::format(
            "an image is cooked to FBE_COOK_UASTC, BC7, ASTC, or RGBA8, not {}", options.format)));
    }
    if (options.quality < 0 || options.quality > 4) {
        return std::unexpected(
            invalid(std::format("an image's quality is 0 to 4, not {}", options.quality)));
    }
    if (bytes.size() > INT_MAX) {
        return std::unexpected(invalid("it's too big to decode"));
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    // Its header first, so what it says it is is never more than the engine loads.
    const auto* data = bytes.data();
    const auto size = static_cast<int>(bytes.size());
    if (stbi_info_from_memory(data, size, &width, &height, &channels) == 0) {
        return std::unexpected(invalid(std::format("it won't decode: {}", stbi_failure_reason())));
    }
    const auto past = [](int side) { return std::cmp_greater(side, assets::most_side); };
    if (past(width) || past(height)) {
        return std::unexpected(invalid(std::format("it's {} by {}, past the {} the engine loads",
                                                   width, height, assets::most_side)));
    }
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> texels(
        stbi_load_from_memory(data, size, &width, &height, &channels, static_cast<int>(rgba)),
        &stbi_image_free);
    if (texels == nullptr) {
        return std::unexpected(invalid(std::format("it won't decode: {}", stbi_failure_reason())));
    }
    rgba_t image{
        .width = static_cast<std::uint32_t>(width),
        .height = static_cast<std::uint32_t>(height),
    };
    image.texels.assign(texels.get(),
                        texels.get() + (std::size_t{ image.width } * image.height * rgba));
    std::expected<texture_ptr_t, std::string> texture = texture_of(std::move(image), options);
    if (!texture) {
        return std::unexpected(other(std::move(texture.error())));
    }
    if (std::expected<void, std::string> encoded = encode(texture->get(), options); !encoded) {
        return std::unexpected(other(std::move(encoded.error())));
    }
    ktx_uint8_t* written = nullptr;
    ktx_size_t length = 0;
    const KTX_error_code code = ktxTexture2_WriteToMemory(texture->get(), &written, &length);
    if (code != KTX_SUCCESS) {
        return std::unexpected(other(failed("write it", code)));
    }
    // libktx's memory is malloc's.
    const std::unique_ptr<ktx_uint8_t, decltype(&std::free)> owned(written, &std::free);
    return bytes_t(written, written + length);
}

} // namespace fbe::tools
