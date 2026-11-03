// glTF's metal-rough shading and image-based lighting, on spheres a test shader works out for each
// pixel, against the same equations here in doubles, and an environment's bake against what it has
// to give. Each test ends by stopping the GPU, which has to leave nothing behind.

#include "golden/golden.hpp"
#include "gpu_testing.hpp"
#include "test_shaders.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <environment/environment.hpp>
#include <fbengine.h>
#include <gpu/graph.hpp>
#include <gpu/pipeline.hpp>
#include <gpu/vulkan.hpp>
#include <limits>
#include <numbers>
#include <string>
#include <texture/texture.hpp>
#include <tuple>
#include <utility>
#include <vector>

using fbe::gpu::flag;
using fbe::tests::require;
using fbe::tests::stop_gpu;

namespace {

using vec3_t = std::array<double, 3>;

constexpr double pi = std::numbers::pi;

void start() {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_gpu_start());
}

/// A half float's value.
float from_half(std::uint32_t half) {
    const std::uint32_t sign = (half & 0x8000U) << 16U;
    const std::uint32_t exponent = (half >> 10U) & 0x1FU;
    const std::uint32_t mantissa = half & 0x3FFU;
    if (exponent == 31) {
        return std::bit_cast<float>(sign | 0x7F800000U | (mantissa << 13U));
    }
    if (exponent == 0) {
        const float small = std::ldexp(static_cast<float>(mantissa), -24);
        return sign != 0 ? -small : small;
    }
    const std::uint32_t bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    return std::bit_cast<float>(bits);
}

/// A half float for `value`, as a texture's written.
std::uint16_t to_half(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    if ((bits & 0x7FFFFFFFU) < 0x38800000U) {
        return sign;
    }
    return static_cast<std::uint16_t>(sign |
                                      (((bits & 0x7FFFFFFFU) - 0x38000000U + 0x1000U) >> 13U));
}

/// An RGBA16F texture's or target's level, read back: its pixels as floats, rows first.
struct floats_t {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> values;

    [[nodiscard]] std::array<float, 4> at(std::uint32_t x, std::uint32_t y) const {
        const std::size_t first = ((std::size_t{ y } * width) + x) * 4;
        return { values[first], values[first + 1], values[first + 2], values[first + 3] };
    }
};

floats_t read(std::uint64_t texture, std::uint32_t level = 0) {
    const std::vector<std::uint8_t> bytes = fbe::texture::read(texture, level);
    floats_t into{
        .width = std::max(static_cast<std::uint32_t>(fbe_texture_width(texture)) >> level, 1U),
        .height = std::max(static_cast<std::uint32_t>(fbe_texture_height(texture)) >> level, 1U),
    };
    REQUIRE(bytes.size() == std::size_t{ into.width } * into.height * 8);
    into.values.resize(bytes.size() / 2);
    for (std::size_t at = 0; at < into.values.size(); ++at) {
        std::uint16_t half = 0;
        std::memcpy(&half, &bytes[at * 2], sizeof(half));
        into.values[at] = from_half(half);
    }
    return into;
}

/// An RGBA16F texture `width` by `height`, each pixel what `color` says for its row and column.
template <typename Color>
std::uint64_t source_of(std::int32_t width, std::int32_t height, bool mipmaps, const Color& color) {
    const std::uint64_t texture = fbe_texture_create(width, height, FBE_TEXTURE_RGBA16F, mipmaps);
    INFO(fbe_error());
    REQUIRE(texture != 0);
    std::vector<std::uint16_t> pixels;
    pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            const std::array<float, 3> rgb = color(x, y);
            pixels.push_back(to_half(rgb[0]));
            pixels.push_back(to_half(rgb[1]));
            pixels.push_back(to_half(rgb[2]));
            pixels.push_back(to_half(1.0F));
        }
    }
    std::vector<std::uint8_t> bytes(pixels.size() * sizeof(std::uint16_t));
    std::memcpy(bytes.data(), pixels.data(), bytes.size());
    require(fbe_texture_write(texture, 0, 0, width, height, bytes.data(), width * 8));
    return texture;
}

std::uint64_t even_source(std::array<float, 3> color) {
    return source_of(4, 2, false, [color](std::int32_t, std::int32_t) { return color; });
}

/// What spheres.slang's Spheres is.
struct spheres_t {
    fbe::environment::shader_data_t environment{};
    alignas(16) std::array<float, 4> to_light{ 0.0F, 0.0F, 1.0F, 0.0F };
    std::array<float, 4> radiance{};
    std::array<float, 4> base_color{ 1.0F, 1.0F, 1.0F, 0.0F };
    std::array<float, 4> grid{ 0.5F, 1.0F, 1.0F, 1.0F };
    std::array<float, 4> texel{ 0.5F, 0.5F, 1.0F, 1.0F };
    std::array<float, 4> emissive{};
};

constexpr std::int32_t grid_size = 64;

/// Spheres drawn into an RGBA16F target `size` across, reading `environment` if there is one, read
/// back.
floats_t draw(spheres_t spheres, std::uint64_t environment = 0, std::int32_t size = grid_size) {
    spheres.emissive[3] = static_cast<float>(size);
    const std::uint64_t target =
        fbe_texture_create_target(size, size, FBE_TEXTURE_RGBA16F, 1, false);
    INFO(fbe_error());
    REQUIRE(target != 0);
    const fbe::gpu::resource_t resource = fbe::gpu::texture_resource(target);
    REQUIRE(resource != fbe::gpu::no_resource);
    std::vector<std::pair<fbe::gpu::resource_t, fbe::gpu::use_t>> uses;
    if (environment != 0) {
        REQUIRE(fbe::environment::shader_data(environment, spheres.environment));
        spheres.to_light[3] = 1.0F;
        for (const std::uint64_t texture : {
                 fbe_environment_irradiance(environment),
                 fbe_environment_specular(environment),
                 fbe_environment_table(environment),
             }) {
            uses.emplace_back(fbe::gpu::texture_resource(texture), fbe::gpu::use_t::SAMPLED);
        }
    }
    VkPipeline pipeline = fbe::gpu::pipeline({
        .shader = &fbe::test_shaders::spheres,
        .color = VK_FORMAT_R16G16B16A16_SFLOAT,
    });
    REQUIRE(pipeline != VK_NULL_HANDLE);
    require(fbe::gpu::add_pass({
        .name = "spheres",
        .colors = { { .target = resource, .clear = true } },
        .uses = std::move(uses),
        .record =
            [spheres, pipeline](VkCommandBuffer commands) {
                VkDescriptorSet set = fbe::gpu::bindless_set();
                vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        fbe::gpu::pipeline_layout(), 0, 1, &set, 0, nullptr);
                vkCmdPushConstants(commands, fbe::gpu::pipeline_layout(),
                                   flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL), 0,
                                   sizeof(spheres), &spheres);
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdDraw(commands, 3, 1, 0, 0);
            },
    }));
    floats_t read_back = read(target);
    require(fbe_texture_release(target));
    return read_back;
}

vec3_t cross(const vec3_t& a, const vec3_t& b) {
    return { (a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]),
             (a[0] * b[1]) - (a[1] * b[0]) };
}

double dot(const vec3_t& a, const vec3_t& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

vec3_t normalized(const vec3_t& a) {
    const double length = std::sqrt(dot(a, a));
    return { a[0] / length, a[1] / length, a[2] / length };
}

/// glTF's BRDF times the cosine, for one channel, as the specification writes it but for Smith's
/// visibility, height-correlated as Khronos's viewer has it, where it writes the separable form.
double shaded(double base, double metallic, double roughness, const vec3_t& normal,
              const vec3_t& view, const vec3_t& light, double radiance) {
    const double n_dot_l = std::clamp(dot(normal, light), 0.0, 1.0);
    if (n_dot_l <= 0.0) {
        return 0.0;
    }
    const vec3_t halfway =
        normalized({ view[0] + light[0], view[1] + light[1], view[2] + light[2] });
    const double n_dot_v = std::clamp(dot(normal, view), 0.0, 1.0);
    const double n_dot_h = std::clamp(dot(normal, halfway), 0.0, 1.0);
    const double v_dot_h = std::clamp(dot(view, halfway), 0.0, 1.0);
    const double alpha = std::max(roughness * roughness, 0.002);
    const double squared = alpha * alpha;
    const double across = (n_dot_h * n_dot_h * (squared - 1.0)) + 1.0;
    const double distribution = squared / (pi * across * across);
    const double viewed = n_dot_l * std::sqrt((n_dot_v * n_dot_v * (1.0 - squared)) + squared);
    const double lit = n_dot_v * std::sqrt((n_dot_l * n_dot_l * (1.0 - squared)) + squared);
    const double visibility = 0.5 / (viewed + lit);
    const double f0 = 0.04 + ((base - 0.04) * metallic);
    const double fresnel = f0 + ((1.0 - f0) * std::pow(1.0 - v_dot_h, 5.0));
    const double diffuse = (1.0 - fresnel) * base * (1.0 - metallic) / pi;
    return (diffuse + (fresnel * visibility * distribution)) * radiance * n_dot_l;
}

/// The normal spheres.slang works out at a pixel of a grid of one sphere, and whether it's on it.
bool normal_at(std::uint32_t x, std::uint32_t y, vec3_t& normal) {
    const double from_x = (((static_cast<double>(x) + 0.5) / grid_size) * 2.0) - 1.0;
    const double from_y = (((static_cast<double>(y) + 0.5) / grid_size) * 2.0) - 1.0;
    const double squared = (from_x * from_x) + (from_y * from_y);
    if (squared >= 1.0) {
        return false;
    }
    normal = normalized({ from_x, -from_y, std::sqrt(1.0 - squared) });
    return true;
}

/// The direction of an octahedral map's texel, as environment.slang works it out.
vec3_t octahedral_direction(std::uint32_t x, std::uint32_t y, std::uint32_t size) {
    const double from_x = ((static_cast<double>(x) / (size - 1)) * 2.0) - 1.0;
    const double from_y = ((static_cast<double>(y) / (size - 1)) * 2.0) - 1.0;
    const double up = 1.0 - std::abs(from_x) - std::abs(from_y);
    double across = from_x;
    double down = from_y;
    if (up < 0.0) {
        across = (1.0 - std::abs(from_y)) * (from_x >= 0.0 ? 1.0 : -1.0);
        down = (1.0 - std::abs(from_x)) * (from_y >= 0.0 ? 1.0 : -1.0);
    }
    return normalized({ across, up, down });
}

/// A smooth lobe of light, and what it integrates to.
const vec3_t lobe_axis = normalized({ 0.6, 0.5, -0.6 });

double lobe(const vec3_t& direction) {
    const double along = std::max(dot(direction, lobe_axis), 0.0);
    return 4.0 * along * along;
}

/// The lobe as an equirectangular image, +X at its middle and y up.
std::uint64_t lobe_source(std::int32_t width, std::int32_t height) {
    return source_of(width, height, true, [width, height](std::int32_t x, std::int32_t y) {
        const double phi = (((static_cast<double>(x) + 0.5) / width) - 0.5) * 2.0 * pi;
        const double theta = ((static_cast<double>(y) + 0.5) / height) * pi;
        const auto value = static_cast<float>(lobe(
            { std::cos(phi) * std::sin(theta), std::cos(theta), std::sin(phi) * std::sin(theta) }));
        return std::array<float, 3>{ value, value, value };
    });
}

/// A `local` direction, z along `normal`, in the world, in the frame environment.slang's is.
vec3_t in_frame_of(const vec3_t& normal, const vec3_t& local) {
    const vec3_t other =
        std::abs(normal[1]) < 0.999 ? vec3_t{ 0.0, 1.0, 0.0 } : vec3_t{ 1.0, 0.0, 0.0 };
    const vec3_t across = normalized(cross(other, normal));
    const vec3_t up = cross(normal, across);
    return { (across[0] * local[0]) + (up[0] * local[1]) + (normal[0] * local[2]),
             (across[1] * local[0]) + (up[1] * local[1]) + (normal[1] * local[2]),
             (across[2] * local[0]) + (up[2] * local[1]) + (normal[2] * local[2]) };
}

constexpr int quadrature_steps = 200;

double lobe_irradiance(const vec3_t& normal) {
    double sum = 0.0;
    for (int ring = 0; ring < quadrature_steps; ++ring) {
        const double u = (ring + 0.5) / quadrature_steps;
        for (int spoke = 0; spoke < quadrature_steps; ++spoke) {
            const double angle = 2.0 * pi * (spoke + 0.5) / quadrature_steps;
            sum +=
                lobe(in_frame_of(normal, { std::sqrt(u) * std::cos(angle),
                                           std::sqrt(u) * std::sin(angle), std::sqrt(1.0 - u) }));
        }
    }
    return sum / (quadrature_steps * quadrature_steps);
}

/// The lobe through GGX's distribution around `normal`, seen along it, weighted by the cosine.
double lobe_specular(const vec3_t& normal, double roughness) {
    if (roughness <= 0.0) {
        return lobe(normal);
    }
    const double squared = roughness * roughness * roughness * roughness;
    double sum = 0.0;
    double weights = 0.0;
    for (int ring = 0; ring < quadrature_steps; ++ring) {
        const double u = (ring + 0.5) / quadrature_steps;
        const double up_squared = (1.0 - u) / (1.0 + ((squared - 1.0) * u));
        const double up = std::sqrt(up_squared);
        const double out = std::sqrt(std::max(1.0 - up_squared, 0.0));
        for (int spoke = 0; spoke < quadrature_steps; ++spoke) {
            const double angle = 2.0 * pi * (spoke + 0.5) / quadrature_steps;
            const vec3_t local{ 2.0 * up * out * std::cos(angle), 2.0 * up * out * std::sin(angle),
                                (2.0 * up_squared) - 1.0 };
            if (local[2] > 0.0) {
                sum += lobe(in_frame_of(normal, local)) * local[2];
                weights += local[2];
            }
        }
    }
    return sum / weights;
}

/// Every texel of what an environment's baked: its table, its diffuse light, and its specular
/// light's levels.
std::vector<floats_t> all_of(std::uint64_t environment) {
    std::vector<floats_t> all;
    all.push_back(read(fbe_environment_table(environment)));
    all.push_back(read(fbe_environment_irradiance(environment)));
    const std::uint64_t specular = fbe_environment_specular(environment);
    for (std::int32_t level = 0; level < fbe_texture_levels(specular); ++level) {
        all.push_back(read(specular, static_cast<std::uint32_t>(level)));
    }
    return all;
}

/// The average red light over the sphere of an octahedral map, its texels on the map's edges, each
/// covering a solid angle of 1 / r^3 of the map's own area, r how far out the octahedron is there.
double average_light(const floats_t& frame) {
    const std::uint32_t last = frame.width - 1;
    double sum = 0.0;
    double angle = 0.0;
    for (std::uint32_t y = 0; y <= last; ++y) {
        for (std::uint32_t x = 0; x <= last; ++x) {
            const double from_x = ((static_cast<double>(x) / last) * 2.0) - 1.0;
            const double from_y = ((static_cast<double>(y) / last) * 2.0) - 1.0;
            const double up = 1.0 - std::abs(from_x) - std::abs(from_y);
            const double out_x = std::min(std::abs(from_x), 1.0 - std::abs(from_y));
            const double out_y = std::min(std::abs(from_y), 1.0 - std::abs(from_x));
            const double squared = (out_x * out_x) + (out_y * out_y) + (up * up);
            const double edges =
                ((x == 0 || x == last) ? 0.5 : 1.0) * ((y == 0 || y == last) ? 0.5 : 1.0);
            const double weight = edges / std::pow(squared, 1.5);
            sum += frame.at(x, y)[0] * weight;
            angle += weight;
        }
    }
    return sum / angle;
}

} // namespace

TEST_CASE("a punctual light shades as glTF's BRDF says", "[gpu]") {
    start();
    const vec3_t view{ 0.0, 0.0, 1.0 };
    const vec3_t light = normalized({ 0.4, 0.6, 0.7 });
    for (const auto& [metallic, roughness] : {
             std::pair{ 0.0F, 0.5F },
             std::pair{ 1.0F, 0.3F },
             std::pair{ 0.5F, 0.8F },
             std::pair{ 1.0F, 0.0F },
         }) {
        INFO("metallic " << metallic << ", roughness " << roughness);
        const floats_t frame = draw({
            .to_light = { static_cast<float>(light[0]), static_cast<float>(light[1]),
                          static_cast<float>(light[2]), 0.0F, },
            .radiance = { 3.0F, 2.0F, 1.0F, 0.0F },
            .base_color = { 0.8F, 0.5F, 0.2F, metallic },
            .grid = { roughness, 1.0F, 1.0F, 1.0F },
        });
        for (std::uint32_t y = 0; y < grid_size; y += 3) {
            for (std::uint32_t x = 0; x < grid_size; x += 3) {
                vec3_t normal{};
                const std::array<float, 4> pixel = frame.at(x, y);
                if (!normal_at(x, y, normal)) {
                    CHECK(pixel[3] == 0.0F);
                    continue;
                }
                const std::array<double, 3> base{ 0.8, 0.5, 0.2 };
                const std::array<double, 3> given{ 3.0, 2.0, 1.0 };
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const double wanted = shaded(base.at(channel), metallic, roughness, normal,
                                                 view, light, given.at(channel));
                    INFO("pixel " << x << ", " << y << ", channel " << channel);
                    // The half floats' rounding, in proportion, and a mirror's narrow peak aside.
                    CHECK(std::abs(pixel.at(channel) - wanted) <= (0.002 * wanted) + 0.002);
                }
            }
        }
    }
    stop_gpu();
}

// A flat map's the normal itself; a tilted one's what the tangent frame makes of it, red along the
// tangent, green along the bitangent, which the tangent's w says the way of, and its scale scales
// the tilt.
TEST_CASE("a normal map's texel turns the normal in the tangent frame", "[gpu]") {
    start();
    const vec3_t view{ 0.0, 0.0, 1.0 };
    const spheres_t lit{
        .radiance = { 1.0F, 1.0F, 1.0F, 0.0F },
        .base_color = { 0.5F, 0.5F, 0.5F, 0.0F },
    };
    spheres_t flat = lit;
    flat.radiance[3] = 1.0F;
    CHECK(draw(flat).values == draw(lit).values);

    const std::uint32_t middle = grid_size / 2;
    vec3_t normal{};
    REQUIRE(normal_at(middle, middle, normal));
    const vec3_t tangent = normalized(
        { 1.0 - (normal[0] * normal[0]), -normal[0] * normal[1], -normal[0] * normal[2] });
    const vec3_t bitangent = cross(normal, tangent);
    // The middle pixel seen under a light off to a side, so a turn the wrong way is seen.
    const auto middle_under = [&](vec3_t light, spheres_t spheres) {
        spheres.to_light = { static_cast<float>(light[0]), static_cast<float>(light[1]),
                             static_cast<float>(light[2]), 0.0F };
        return draw(spheres).at(middle, middle)[0];
    };
    const auto turned = [&normal](const vec3_t& along, double by) {
        return normalized({ normal[0] + (along[0] * by), normal[1] + (along[1] * by),
                            normal[2] + (along[2] * by) });
    };
    for (const float w : { 1.0F, -1.0F }) {
        INFO("w " << w);
        spheres_t tilted = lit;
        tilted.radiance[3] = w;
        const vec3_t side = normalized({ 0.5, 0.0, 0.8 });
        tilted.texel = { 0.75F, 0.5F, 1.0F, 1.0F };
        const double along_tangent = shaded(0.5, 0.0, 0.5, turned(tangent, 0.5), view, side, 1.0);
        CHECK(std::abs(middle_under(side, tilted) - along_tangent) < 0.002);
        tilted.texel = { 0.625F, 0.5F, 1.0F, 2.0F };
        CHECK(std::abs(middle_under(side, tilted) - along_tangent) < 0.002);

        const vec3_t above = normalized({ 0.0, 0.5, 0.8 });
        tilted.texel = { 0.5F, 0.75F, 1.0F, 1.0F };
        const double along_bitangent =
            shaded(0.5, 0.0, 0.5, turned(bitangent, 0.5 * w), view, above, 1.0);
        CHECK(std::abs(middle_under(above, tilted) - along_bitangent) < 0.002);
    }
    stop_gpu();
}

// What has no direction leaves the normal as it was, or shades as the normal says: a tangent of no
// length, a texel of no direction, and a light straight against the view, all finite.
TEST_CASE("what has no direction shades finitely", "[gpu]") {
    start();
    const spheres_t lit{
        .radiance = { 1.0F, 1.0F, 1.0F, 0.0F },
        .base_color = { 0.5F, 0.5F, 0.5F, 0.0F },
    };
    const floats_t plain = draw(lit);
    spheres_t no_tangent = lit;
    no_tangent.radiance[3] = 2.0F;
    no_tangent.texel = { 0.75F, 0.75F, 1.0F, 1.0F };
    CHECK(draw(no_tangent).values == plain.values);
    spheres_t no_direction = lit;
    no_direction.radiance[3] = 1.0F;
    no_direction.texel = { 0.5F, 0.5F, 0.5F, 1.0F };
    CHECK(draw(no_direction).values == plain.values);

    // A texel pointing straight into the surface, and a light behind it: head on, the view and the
    // light are opposite. The BRDF there, with the normal as its halfway: all of Fresnel, GGX's
    // peak, 1 / (pi alpha^2), and Smith's 0.5 / alpha, at an alpha of 0.25.
    constexpr std::int32_t odd = 65;
    const floats_t against = draw(
        {
            .to_light = { 0.0F, 0.0F, -1.0F, 0.0F },
            .radiance = { 1.0F, 1.0F, 1.0F, 1.0F },
            .base_color = { 0.5F, 0.5F, 0.5F, 0.0F },
            .texel = { 0.5F, 0.5F, 0.0F, 1.0F },
        },
        0, odd);
    for (const float value : against.values) {
        CHECK(std::isfinite(value));
    }
    const double peak = (0.5 / 0.25) / (pi * 0.25 * 0.25);
    CHECK(std::abs(against.at(32, 32)[0] - peak) < 0.01 * peak);
    stop_gpu();
}

// A table that isn't baked is all zeros, as it was for a bake a frame that failed lost: white metal
// divides by what multiple scattering gives back, which has to stay finite.
TEST_CASE("shading through an empty table is finite", "[gpu]") {
    start();
    const std::uint64_t environment =
        fbe_environment_bake(even_source({ 1.0F, 1.0F, 1.0F }), 16, 4);
    REQUIRE(environment != 0);
    const std::uint64_t empty = source_of(
        128, 128, false, [](std::int32_t, std::int32_t) { return std::array<float, 3>{}; });
    // Everything's at rest for the pass, which doesn't declare what it reads.
    CHECK(read(empty).values[0] == 0.0F);
    spheres_t spheres{
        .base_color = { 1.0F, 1.0F, 1.0F, 1.0F },
    };
    REQUIRE(fbe::environment::shader_data(environment, spheres.environment));
    spheres.environment.table = fbe::texture::index_of(empty);
    spheres.to_light[3] = 1.0F;
    for (const float value : draw(spheres).values) {
        CHECK(std::isfinite(value));
    }
    stop_gpu();
}

TEST_CASE("an even environment's light is its own color", "[gpu]") {
    start();
    const std::array<float, 3> color{ 0.25F, 0.5F, 2.0F };
    // At one sample it all has to come from that one, which at the roughest is below the horizon.
    for (const std::int32_t samples : { 16, 1 }) {
        INFO(samples << " samples");
        const std::uint64_t source = even_source(color);
        const std::uint64_t environment = fbe_environment_bake(source, 16, samples);
        INFO(fbe_error());
        REQUIRE(environment != 0);
        // What's baked is in the frame's commands, which hold their source however it's let go of.
        require(fbe_texture_release(source));
        CHECK(fbe_environment_levels(environment) == 2);
        const auto check_all = [&color](const floats_t& frame) {
            for (std::uint32_t y = 0; y < frame.height; ++y) {
                for (std::uint32_t x = 0; x < frame.width; ++x) {
                    const std::array<float, 4> pixel = frame.at(x, y);
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        CHECK(std::abs(pixel.at(channel) - color.at(channel)) <=
                              0.005F * color.at(channel));
                    }
                }
            }
        };
        check_all(read(fbe_environment_irradiance(environment)));
        const std::uint64_t specular = fbe_environment_specular(environment);
        CHECK(fbe_texture_width(specular) == 16);
        REQUIRE(fbe_texture_levels(specular) == 5);
        for (std::uint32_t level = 0; level < 5; ++level) {
            INFO("level " << level);
            check_all(read(specular, level));
        }
        CHECK(fbe_environment_free(environment));
    }
    stop_gpu();
}

// The table's what the split sum needs: all the light at a mirror head on, no more than all of it
// anywhere, and less the rougher, for what single scattering loses.
TEST_CASE("the split sum's table holds no more light than there is", "[gpu]") {
    start();
    const std::uint64_t environment =
        fbe_environment_bake(even_source({ 1.0F, 1.0F, 1.0F }), 16, 4);
    REQUIRE(environment != 0);
    const floats_t table = read(fbe_environment_table(environment));
    CHECK(table.width == 128);
    const std::array<float, 4> mirror = table.at(127, 0);
    CHECK(std::abs(mirror[0] - 1.0F) < 0.02F);
    CHECK(std::abs(mirror[1]) < 0.02F);
    for (std::uint32_t y = 0; y < table.height; ++y) {
        for (std::uint32_t x = 0; x < table.width; ++x) {
            const std::array<float, 4> pixel = table.at(x, y);
            INFO(x << ", " << y);
            CHECK(pixel[0] >= 0.0F);
            CHECK(pixel[1] >= 0.0F);
            CHECK(pixel[0] + pixel[1] <= 1.01F);
        }
    }
    // The integrals themselves, by quadrature in doubles, in the middles of their texels.
    for (const auto& [x, y, scaled, added] :
         { std::tuple{ 63U, 63U, 0.8363, 0.0232 }, std::tuple{ 95U, 95U, 0.6359, 0.0017 },
           std::tuple{ 31U, 95U, 0.7195, 0.0289 } }) {
        INFO(x << ", " << y);
        CHECK(std::abs(table.at(x, y)[0] - scaled) < 0.004);
        CHECK(std::abs(table.at(x, y)[1] - added) < 0.004);
    }
    // Near grazing, where half a texel's the most, as far from what it isn't.
    CHECK(std::abs(table.at(3, 95)[0] - 0.8682) < 0.003);
    // Head on, a rough surface reflects less at once than a smooth one.
    CHECK(table.at(127, 127)[0] + table.at(127, 127)[1] <
          table.at(127, 40)[0] + table.at(127, 40)[1]);
    // Every environment shares it.
    const std::uint64_t other = fbe_environment_bake(even_source({ 1.0F, 1.0F, 1.0F }), 16, 4);
    CHECK(fbe_environment_table(other) == fbe_environment_table(environment));
    stop_gpu();
}

// A white environment on a white surface gives back all its light, metal or not, however rough,
// from every side: the test of energy, which losing what a single scatter loses would fail.
TEST_CASE("a white furnace is white at any roughness", "[gpu]") {
    start();
    const std::uint64_t environment =
        fbe_environment_bake(even_source({ 1.0F, 1.0F, 1.0F }), 16, 8);
    REQUIRE(environment != 0);
    for (const float metallic : { 0.0F, 1.0F }) {
        for (const float roughness : { 0.0F, 0.2F, 0.5F, 0.8F, 1.0F }) {
            INFO("metallic " << metallic << ", roughness " << roughness);
            const floats_t frame = draw(
                {
                    .base_color = { 1.0F, 1.0F, 1.0F, metallic },
                    .grid = { roughness, 1.0F, 1.0F, 1.0F },
                },
                environment);
            for (std::uint32_t y = 0; y < grid_size; y += 2) {
                for (std::uint32_t x = 0; x < grid_size; x += 2) {
                    vec3_t normal{};
                    if (normal_at(x, y, normal)) {
                        INFO(x << ", " << y);
                        CHECK(std::abs(frame.at(x, y)[0] - 1.0F) < 0.01F);
                    }
                }
            }
        }
    }
    stop_gpu();
}

// Occlusion scales an environment's light, and not a punctual light's or emission.
TEST_CASE("occlusion and emission are what they should be", "[gpu]") {
    start();
    const std::uint64_t environment =
        fbe_environment_bake(even_source({ 1.0F, 1.0F, 1.0F }), 16, 8);
    REQUIRE(environment != 0);
    const std::uint32_t middle = grid_size / 2;
    const floats_t half = draw({ .grid = { 0.5F, 1.0F, 1.0F, 0.5F } }, environment);
    CHECK(std::abs(half.at(middle, middle)[0] - 0.5F) < 0.01F);
    const floats_t glowing = draw({
        .grid = { 0.5F, 1.0F, 1.0F, 0.5F },
        .emissive = { 0.25F, 0.5F, 4.0F, 0.0F },
    });
    CHECK(glowing.at(middle, middle)[0] == 0.25F);
    CHECK(glowing.at(middle, middle)[2] == 4.0F);

    // What a punctual light adds is the same however occluded, with everything else on.
    const auto added_by_light = [&](float occlusion) {
        spheres_t spheres{
            .to_light = { 0.0F, 0.0F, 1.0F, 1.0F },
            .grid = { 0.5F, 1.0F, 1.0F, occlusion },
            .emissive = { 0.25F, 0.25F, 0.25F, 0.0F },
        };
        const float unlit = draw(spheres, environment).at(middle, middle)[0];
        spheres.radiance = { 2.0F, 2.0F, 2.0F, 0.0F };
        return draw(spheres, environment).at(middle, middle)[0] - unlit;
    };
    const float unoccluded = added_by_light(1.0F);
    CHECK(unoccluded > 0.1F);
    CHECK(std::abs(added_by_light(0.5F) - unoccluded) < 0.002F);
    stop_gpu();
}

// The sky's along the top of the image, +Y at the map's middle and -Y at its corners; +X at the
// image's middle comes out on the +X side of the map.
TEST_CASE("an environment's directions are the image's", "[gpu]") {
    start();
    const std::uint64_t sky = source_of(64, 32, false, [](std::int32_t, std::int32_t y) {
        return std::array<float, 3>{ y < 16 ? 1.0F : 0.0F, 0.0F, 0.0F };
    });
    const std::uint64_t day = fbe_environment_bake(sky, 16, 256);
    REQUIRE(day != 0);
    const floats_t irradiance = read(fbe_environment_irradiance(day));
    // Texels 0 and 31 are on the map's corners; 15.5 is its middle.
    CHECK(irradiance.at(15, 15)[0] > 0.95F);
    CHECK(irradiance.at(0, 0)[0] < 0.05F);
    CHECK(std::abs(irradiance.at(0, 15)[0] - 0.5F) < 0.05F);
    const floats_t mirror = read(fbe_environment_specular(day));
    CHECK(mirror.at(7, 7)[0] > 0.99F);
    CHECK(mirror.at(15, 15)[0] < 0.01F);

    // A spot at the image's middle row and column, on the horizon at +X.
    const std::uint64_t spot = source_of(64, 32, false, [](std::int32_t x, std::int32_t y) {
        const bool lit = std::abs(x - 32) < 4 && std::abs(y - 16) < 4;
        return std::array<float, 3>{ lit ? 8.0F : 0.0F, 0.0F, 0.0F };
    });
    const std::uint64_t spotted = fbe_environment_bake(spot, 32, 64);
    REQUIRE(spotted != 0);
    const floats_t sharp = read(fbe_environment_specular(spotted));
    std::uint32_t brightest_x = 0;
    std::uint32_t brightest_y = 0;
    float brightest = -1.0F;
    for (std::uint32_t y = 0; y < sharp.height; ++y) {
        for (std::uint32_t x = 0; x < sharp.width; ++x) {
            if (sharp.at(x, y)[0] > brightest) {
                brightest = sharp.at(x, y)[0];
                brightest_x = x;
                brightest_y = y;
            }
        }
    }
    const vec3_t found = octahedral_direction(brightest_x, brightest_y, 32);
    INFO(found[0] << ", " << found[1] << ", " << found[2]);
    CHECK(found[0] > 0.9);
    stop_gpu();
}

// Texels on the map's edges are their mirrors', the same direction, so filtering across an edge
// meets what's on the other side.
TEST_CASE("an octahedral map's edges meet their mirrors", "[gpu]") {
    start();
    const std::uint64_t source = source_of(64, 32, true, [](std::int32_t x, std::int32_t y) {
        return std::array<float, 3>{
            static_cast<float>(x % 7) / 7.0F,
            static_cast<float>(y % 5) / 5.0F,
            static_cast<float>((x + y) % 3),
        };
    });
    const std::uint64_t environment = fbe_environment_bake(source, 32, 32);
    REQUIRE(environment != 0);
    const auto check_edges = [](const floats_t& frame) {
        const std::uint32_t last = frame.width - 1;
        for (std::uint32_t along = 0; along <= last; ++along) {
            INFO("along " << along);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                CHECK(std::abs(frame.at(0, along).at(channel) -
                               frame.at(0, last - along).at(channel)) < 0.02F);
                CHECK(std::abs(frame.at(last, along).at(channel) -
                               frame.at(last, last - along).at(channel)) < 0.02F);
                CHECK(std::abs(frame.at(along, 0).at(channel) -
                               frame.at(last - along, 0).at(channel)) < 0.02F);
                CHECK(std::abs(frame.at(along, last).at(channel) -
                               frame.at(last - along, last).at(channel)) < 0.02F);
            }
        }
    };
    check_edges(read(fbe_environment_irradiance(environment)));
    check_edges(read(fbe_environment_specular(environment), 0));
    check_edges(read(fbe_environment_specular(environment), 2));
    stop_gpu();
}

// The bake reads no LOD bias, as 3D's samplers have, and the shading's the same with one set.
TEST_CASE("an environment ignores the LOD bias", "[gpu]") {
    start();
    const std::uint64_t source = source_of(64, 32, true, [](std::int32_t x, std::int32_t) {
        return std::array<float, 3>{ static_cast<float>(x % 2), 0.0F, 0.0F };
    });
    const std::uint64_t plain = fbe_environment_bake(source, 16, 16);
    REQUIRE(plain != 0);
    require(fbe_gpu_set_lod_bias(8.0F));
    const std::uint64_t biased = fbe_environment_bake(source, 16, 16);
    REQUIRE(biased != 0);
    CHECK(read(fbe_environment_specular(plain)).values ==
          read(fbe_environment_specular(biased)).values);
    // And shading through it doesn't either, through the sampler that has none, which a driver that
    // doesn't bias a level it's given wouldn't show.
    fbe::environment::shader_data_t data{};
    REQUIRE(fbe::environment::shader_data(plain, data));
    CHECK(data.sampler == fbe::gpu::sampler_2d_index(FBE_TEXTURE_LINEAR, false));
    const spheres_t spheres{
        .base_color = { 1.0F, 1.0F, 1.0F, 1.0F },
        .grid = { 0.4F, 1.0F, 1.0F, 1.0F },
    };
    const floats_t under_bias = draw(spheres, plain);
    require(fbe_gpu_set_lod_bias(0.0F));
    CHECK(draw(spheres, plain).values == under_bias.values);
    stop_gpu();
}

TEST_CASE("an environment that can't be baked is refused", "[gpu]") {
    start();
    const std::uint64_t source = even_source({ 1.0F, 1.0F, 1.0F });
    const std::int32_t held = fbe_environment_count();
    for (const std::int32_t size : { 0, 8, 15, 100, 4096, -16 }) {
        INFO("size " << size);
        CHECK(fbe_environment_bake(source, size, 16) == 0);
        CHECK(std::string(fbe_error()).starts_with("an environment's size is a power of 2"));
    }
    for (const std::int32_t samples : { 0, -1, 4097 }) {
        INFO("samples " << samples);
        CHECK(fbe_environment_bake(source, 16, samples) == 0);
        CHECK(std::string(fbe_error()).starts_with("an environment takes 1 to 4096 samples"));
    }
    CHECK(fbe_environment_bake(0, 16, 16) == 0);
    CHECK(fbe_environment_count() == held);
    CHECK(!fbe_environment_free(0));
    CHECK(!fbe_environment_free(source));
    CHECK(fbe_environment_irradiance(source) == 0);
    CHECK(fbe_environment_levels(source) == -1);

    // A texture that can't be made leaves nothing behind.
    fbe::gpu::inject(fbe::gpu::fault_t::VIEW);
    CHECK(fbe_environment_bake(source, 16, 16) == 0);
    CHECK(fbe_environment_count() == held);
    fbe_frame_flip(false);
    const std::uint64_t baked = fbe_environment_bake(source, 16, 16);
    REQUIRE(baked != 0);
    CHECK(fbe_environment_count() == held + 1);
    CHECK(fbe_environment_free(baked));
    CHECK(!fbe_environment_free(baked));
    CHECK(fbe_environment_count() == held);
    stop_gpu();
}

// What's baked is in the frame's commands, which have what they draw into and read however it's let
// go of before they run.
TEST_CASE("an environment freed before its frame runs is baked and gone", "[gpu]") {
    start();
    const std::uint64_t source = even_source({ 1.0F, 1.0F, 1.0F });
    const std::int32_t held = fbe_texture_count();
    const std::uint64_t environment = fbe_environment_bake(source, 16, 4);
    REQUIRE(environment != 0);
    require(fbe_texture_release(source));
    CHECK(fbe_environment_free(environment));
    fbe_frame_flip(false);
    fbe_frame_flip(false);
    CHECK(fbe_texture_count() == held - 1);
    CHECK(!fbe_gpu_lost());
    stop_gpu();
}

// A bake's done once its frame has run: one that fails to, for a submit or a target that can't be
// made, is done by the next, the table too, as if nothing had happened, its source held.
TEST_CASE("a bake whose frame fails is baked by the next", "[gpu]") {
    const auto bake_it = [] {
        const std::uint64_t source = lobe_source(64, 32);
        const std::uint64_t environment = fbe_environment_bake(source, 16, 8);
        REQUIRE(environment != 0);
        require(fbe_texture_release(source));
        return environment;
    };
    start();
    const std::vector<floats_t> undisturbed = all_of(bake_it());
    stop_gpu();
    for (const fbe::gpu::fault_t fault : { fbe::gpu::fault_t::SUBMIT, fbe::gpu::fault_t::VIEW }) {
        INFO("fault " << static_cast<int>(fault));
        start();
        const std::int32_t textures = fbe_texture_count();
        const std::uint64_t environment = bake_it();
        fbe::gpu::inject(fault);
        fbe::gpu::finish_frame();
        // Twice running, and the frame's own submit.
        fbe::gpu::inject(fault);
        fbe_frame_flip(false);
        fbe_frame_flip(false);
        const std::vector<floats_t> baked = all_of(environment);
        REQUIRE(baked.size() == undisturbed.size());
        for (std::size_t at = 0; at < baked.size(); ++at) {
            INFO("texture " << at);
            CHECK(baked[at].values == undisturbed[at].values);
        }
        // Done, and it's let go of its source.
        fbe_frame_flip(false);
        CHECK(fbe_texture_count() == textures);
        stop_gpu();
    }
}

// What's let go of while a bake's being done again is no trouble, nor is a GPU that goes.
TEST_CASE("a bake that never runs leaves nothing behind", "[gpu]") {
    start();
    {
        const std::uint64_t source = even_source({ 1.0F, 1.0F, 1.0F });
        const std::uint64_t environment = fbe_environment_bake(source, 16, 4);
        REQUIRE(environment != 0);
        require(fbe_texture_release(source));
        CHECK(fbe_environment_free(environment));
        fbe::gpu::inject(fbe::gpu::fault_t::SUBMIT);
        CHECK(!fbe::gpu::finish_frame());
        fbe_frame_flip(false);
        fbe_frame_flip(false);
    }
    {
        // Stopped with it to do.
        const std::uint64_t source = even_source({ 1.0F, 1.0F, 1.0F });
        const std::uint64_t environment = fbe_environment_bake(source, 16, 4);
        REQUIRE(environment != 0);
        require(fbe_texture_release(source));
        stop_gpu();
        CHECK(fbe_environment_free(environment));
    }
    start();
    {
        // Lost with it to do.
        const std::uint64_t source = even_source({ 1.0F, 1.0F, 1.0F });
        const std::uint64_t environment = fbe_environment_bake(source, 16, 4);
        REQUIRE(environment != 0);
        fbe::gpu::inject(fbe::gpu::fault_t::LOST);
        fbe_frame_flip(false);
        CHECK(fbe_gpu_lost());
        CHECK(fbe_environment_bake(source, 16, 4) == 0);
        CHECK(fbe_environment_free(environment));
    }
    stop_gpu();
}

// Each level holds the light there is, a sun smaller than its texels too, and even over the sphere
// comes to what the source's does.
TEST_CASE("every level of a bake holds the source's light", "[gpu]") {
    start();
    const auto is_sun = [](std::int32_t x, std::int32_t y) {
        return x >= 300 && x < 304 && y >= 200 && y < 204;
    };
    const std::uint64_t sky = source_of(1024, 512, true, [&is_sun](std::int32_t x, std::int32_t y) {
        return std::array<float, 3>{ is_sun(x, y) ? 1000.0F : 0.0F, 0.0F, 0.0F };
    });
    double wanted = 0.0;
    for (std::int32_t y = 200; y < 204; ++y) {
        wanted += 4.0 * 1000.0 * std::sin(pi * (y + 0.5) / 512.0) * (2.0 * pi / 1024.0) *
                  (pi / 512.0) / (4.0 * pi);
    }
    const std::uint64_t environment = fbe_environment_bake(sky, 64, 256);
    REQUIRE(environment != 0);
    const std::uint64_t specular = fbe_environment_specular(environment);
    // The last, 4 texels across, is too few to add up this way, and the lobe's test has it.
    for (std::int32_t level = 0; level < fbe_environment_levels(environment); ++level) {
        INFO("level " << level);
        const double average = average_light(read(specular, static_cast<std::uint32_t>(level)));
        CHECK(std::abs(average - wanted) < 0.1 * wanted);
    }
    const double diffuse = average_light(read(fbe_environment_irradiance(environment)));
    CHECK(std::abs(diffuse - wanted) < 0.1 * wanted);
    stop_gpu();
}

// One texel of infinity, as anything past 65504 is in a half float, doesn't take a bake with it.
TEST_CASE("a source's light is bounded", "[gpu]") {
    start();
    const std::uint64_t texture = fbe_texture_create(16, 8, FBE_TEXTURE_RGBA16F, false);
    REQUIRE(texture != 0);
    std::vector<std::uint16_t> pixels(std::size_t{ 16 } * 8 * 4, to_half(0.5F));
    const std::size_t sun = ((std::size_t{ 3 } * 16) + 8) * 4;
    pixels[sun] = 0x7C00;
    pixels[sun + 1] = 0x7C00;
    pixels[sun + 2] = 0x7C00;
    std::vector<std::uint8_t> bytes(pixels.size() * sizeof(std::uint16_t));
    std::memcpy(bytes.data(), pixels.data(), bytes.size());
    require(fbe_texture_write(texture, 0, 0, 16, 8, bytes.data(), 16 * 8));
    const std::uint64_t environment = fbe_environment_bake(texture, 16, 16);
    REQUIRE(environment != 0);
    for (const floats_t& frame : all_of(environment)) {
        for (const float value : frame.values) {
            CHECK(std::isfinite(value));
            CHECK(value <= 8192.0F);
        }
    }
    stop_gpu();
}

// A big bake's drawn in tiles, a submit between them, which come to the same pixels.
TEST_CASE("no draw of a bake takes more samples than its budget", "[gpu]") {
    start();
    const std::uint64_t source = lobe_source(64, 32);
    const std::uint64_t whole = fbe_environment_bake(source, 32, 16);
    REQUIRE(whole != 0);
    CHECK(fbe::environment::largest_draw() == std::uint64_t{ 128 } * 128 * 1024);
    const std::vector<floats_t> drawn = all_of(whole);
    fbe::environment::set_sample_budget(4096);
    const std::uint64_t tiled = fbe_environment_bake(source, 32, 16);
    REQUIRE(tiled != 0);
    CHECK(fbe::environment::largest_draw() == 4096);
    fbe::environment::set_sample_budget(0);
    const std::vector<floats_t> in_tiles = all_of(tiled);
    REQUIRE(in_tiles.size() == drawn.size());
    for (std::size_t at = 0; at < drawn.size(); ++at) {
        INFO("texture " << at);
        CHECK(in_tiles[at].values == drawn[at].values);
    }
    stop_gpu();
}

// The bake and its reading against a smooth lobe of light, whose integrals are known by quadrature
// here, rather than from what the bake made of them, as the golden image's are.
TEST_CASE("a bake and its reading are what a lobe of light integrates to", "[gpu]") {
    start();
    const std::uint64_t environment = fbe_environment_bake(lobe_source(1024, 512), 64, 4096);
    REQUIRE(environment != 0);
    const floats_t irradiance = read(fbe_environment_irradiance(environment));
    for (const auto& [x, y] : { std::pair{ 3U, 5U }, std::pair{ 15U, 15U }, std::pair{ 25U, 6U },
                                std::pair{ 9U, 27U }, std::pair{ 28U, 28U } }) {
        INFO("irradiance " << x << ", " << y);
        const double wanted = lobe_irradiance(octahedral_direction(x, y, 32));
        CHECK(std::abs(irradiance.at(x, y)[0] - wanted) < 0.01);
    }
    const std::uint64_t specular = fbe_environment_specular(environment);
    const std::int32_t levels = fbe_environment_levels(environment);
    for (std::int32_t level = 0; level <= levels; ++level) {
        const floats_t frame = read(specular, static_cast<std::uint32_t>(level));
        const std::uint32_t last = frame.width - 1;
        const double roughness = static_cast<double>(level) / levels;
        for (const auto& [x, y] :
             { std::pair{ last / 5, last / 3 }, std::pair{ last / 2, last / 2 },
               std::pair{ last * 4 / 5, last / 5 }, std::pair{ last / 4, last * 7 / 8 } }) {
            INFO("level " << level << ", " << x << ", " << y);
            const double wanted = lobe_specular(octahedral_direction(x, y, frame.width), roughness);
            CHECK(std::abs(frame.at(x, y)[0] - wanted) < 0.01);
        }
    }
    // A white metal mirror reads the light along the reflection, all but where the map's folds run
    // through its texels.
    const floats_t mirror = draw(
        {
            .base_color = { 1.0F, 1.0F, 1.0F, 1.0F },
            .grid = { 0.0F, 1.0F, 1.0F, 1.0F },
        },
        environment);
    double sum = 0.0;
    int count = 0;
    for (std::uint32_t y = 0; y < grid_size; ++y) {
        for (std::uint32_t x = 0; x < grid_size; ++x) {
            vec3_t normal{};
            if (normal_at(x, y, normal)) {
                const double twice = 2.0 * normal[2];
                const vec3_t reflected{ twice * normal[0], twice * normal[1],
                                        (twice * normal[2]) - 1.0 };
                const double away = std::abs(mirror.at(x, y)[0] - lobe(reflected));
                INFO(x << ", " << y);
                CHECK(away < 0.07);
                sum += away;
                ++count;
            }
        }
    }
    CHECK(sum / count < 0.005);
    stop_gpu();
}

// Floats go in as half floats, as near as they can.
TEST_CASE("floats are written as half floats", "[gpu]") {
    start();
    const std::uint64_t texture = fbe_texture_create(2, 2, FBE_TEXTURE_RGBA16F, false);
    REQUIRE(texture != 0);
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::array<float, 16> values{
        0.5F,  4.0F,  1000.0F, 1.0F,     70000.0F, inf,  -1.0F, nan,
        6e-5F, 1e-6F, 1e-9F,   65504.0F, 1.5F,     1.0F, 0.1F,  1.0F
    };
    require(fbe_texture_write_floats(texture, 0, 0, 2, 2, values.data()));
    const floats_t back = read(texture);
    const std::array<float, 16> wanted{ 0.5F,
                                        4.0F,
                                        1000.0F,
                                        1.0F,
                                        65504.0F,
                                        65504.0F,
                                        0.0F,
                                        0.0F,
                                        from_half(0x03EFU),
                                        from_half(0x0011U),
                                        0.0F,
                                        65504.0F,
                                        1.5F,
                                        1.0F,
                                        from_half(0x2E66U),
                                        1.0F };
    for (std::size_t at = 0; at < wanted.size(); ++at) {
        INFO("value " << at);
        CHECK(back.values[at] == wanted.at(at));
    }
    const std::uint64_t bytes = fbe_texture_create(2, 2, FBE_TEXTURE_RGBA8, false);
    CHECK(!fbe_texture_write_floats(bytes, 0, 0, 2, 2, values.data()));
    CHECK(!fbe_texture_write_floats(texture, 1, 1, 2, 2, values.data()));
    CHECK(!fbe_texture_write_floats(texture, 0, 0, 2, 2, nullptr));
    stop_gpu();
}

// Stopping the GPU loses what's baked, and the table's made again for what's baked next.
TEST_CASE("an environment doesn't outlive the GPU", "[gpu]") {
    start();
    const std::uint64_t first = fbe_environment_bake(even_source({ 1.0F, 1.0F, 1.0F }), 16, 4);
    REQUIRE(first != 0);
    stop_gpu();
    CHECK(fbe_environment_irradiance(first) == 0);
    CHECK(std::string(fbe_error()) == "the GPU stopped since this environment was baked");
    start();
    const std::uint64_t second = fbe_environment_bake(even_source({ 1.0F, 1.0F, 1.0F }), 16, 4);
    REQUIRE(second != 0);
    CHECK(read(fbe_environment_table(second)).at(127, 0)[0] > 0.9F);
    CHECK(fbe_environment_free(first));
    CHECK(fbe_environment_free(second));
    stop_gpu();
}

// E6's spheres: smooth to rough across, dielectric to metal down, gold, under a sky with a sun and
// a light from the upper left.
TEST_CASE("PBR spheres match their golden image", "[gpu][golden]") {
    start();
    const std::uint64_t sky = source_of(128, 64, true, [](std::int32_t x, std::int32_t y) {
        const float height = 1.0F - (static_cast<float>(y) / 32.0F);
        const bool sun = std::abs(x - 40) < 4 && std::abs(y - 18) < 4;
        if (sun) {
            return std::array<float, 3>{ 12.0F, 11.0F, 9.0F };
        }
        return height > 0.0F ? std::array<float, 3>{ 0.3F + (0.3F * (1.0F - height)),
                                                     0.5F + (0.2F * (1.0F - height)), 0.9F, }
                             : std::array<float, 3>{ 0.2F, 0.15F, 0.1F };
    });
    const std::uint64_t environment = fbe_environment_bake(sky, 64, 64);
    REQUIRE(environment != 0);
    const vec3_t light = normalized({ -0.5, 0.5, 0.7 });
    const floats_t frame = draw(
        {
            .to_light = { static_cast<float>(light[0]), static_cast<float>(light[1]),
                          static_cast<float>(light[2]), 1.0F, },
            .radiance = { 1.0F, 1.0F, 1.0F, 0.0F },
            .base_color = { 1.0F, 0.6F, 0.15F, -1.0F },
            .grid = { -1.0F, 5.0F, 5.0F, 1.0F },
        },
        environment, 200);
    // Tonemapped to what a golden image holds, as the frame's tonemap will be in E6's step 3: a
    // plain Reinhard's, then sRGB's curve.
    fbe::golden::frame_t golden{ .width = frame.width, .height = frame.height };
    golden.rgba.reserve(std::size_t{ frame.width } * frame.height * 4);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::array<float, 4> pixel = frame.at(x, y);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const double mapped = pixel.at(channel) / (1.0 + pixel.at(channel));
                const double curved = mapped <= 0.0031308
                                          ? mapped * 12.92
                                          : (1.055 * std::pow(mapped, 1.0 / 2.4)) - 0.055;
                golden.rgba.push_back(static_cast<std::uint8_t>(std::lround(curved * 255.0)));
            }
            golden.rgba.push_back(255);
        }
    }
    fbe::golden::check("pbr_spheres", golden);
    stop_gpu();
}
