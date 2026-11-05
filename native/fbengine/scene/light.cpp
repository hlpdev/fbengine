// Lights as data: their type, color, intensity, range, and cone, the ambient light of each World,
// and each camera's fog, which drawing reads.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>
#include <vector>

namespace fbe::scene {

namespace {

fbe_color_t color_of(const std::array<float, 3>& rgb) {
    return { .r = rgb[0], .g = rgb[1], .b = rgb[2], .a = 1.0F };
}

bool finite(float r, float g, float b) {
    return std::isfinite(r) && std::isfinite(g) && std::isfinite(b);
}

} // namespace

void lights_in(const std::vector<std::uint32_t>& among, std::vector<std::uint32_t>& found) {
    const entities_t& entities = scene().entities;
    std::vector<std::uint32_t> stack(among.rbegin(), among.rend());
    while (!stack.empty()) {
        const std::uint32_t at = stack.back();
        stack.pop_back();
        if (entities.kind[at] == FBE_ENTITY_LIGHT) {
            found.push_back(at);
        }
        const std::vector<std::uint32_t>& children = entities.children[at];
        stack.insert(stack.end(), children.rbegin(), children.rend());
    }
}

void add_light(std::uint32_t entity) {
    scene_t& s = scene();
    light_t light;
    light.entity = entity;
    s.entities.component[entity] = static_cast<std::uint32_t>(s.lights.size());
    s.lights.push_back(light);
}

void remove_light(std::uint32_t entity) {
    scene_t& s = scene();
    const std::uint32_t index = s.entities.component[entity];
    if (index != s.lights.size() - 1) {
        s.lights[index] = s.lights.back();
        s.entities.component[s.lights[index].entity] = index;
    }
    s.lights.pop_back();
}

light_t* light_of(std::uint64_t handle) {
    const std::uint32_t entity = entity_slot(handle);
    if (entity == none) {
        return nullptr;
    }
    if (scene().entities.kind[entity] != FBE_ENTITY_LIGHT) {
        fail("the entity isn't a light");
        return nullptr;
    }
    return &scene().lights[scene().entities.component[entity]];
}

} // namespace fbe::scene

using fbe::scene::camera_of;
using fbe::scene::camera_t;
using fbe::scene::light_of;
using fbe::scene::light_t;

extern "C" {

FBE_API bool fbe_light_set_type(uint64_t light, int32_t type) {
    light_t* found = light_of(light);
    if (found == nullptr) {
        return false;
    }
    if (type != FBE_LIGHT_DIRECTIONAL && type != FBE_LIGHT_POINT && type != FBE_LIGHT_SPOT) {
        return fbe::fail(std::format("{} isn't a LightType", type));
    }
    found->type = type;
    return true;
}

FBE_API int32_t fbe_light_type(uint64_t light) {
    const light_t* found = light_of(light);
    return found != nullptr ? found->type : -1;
}

FBE_API bool fbe_light_set_color(uint64_t light, float r, float g, float b) {
    light_t* found = light_of(light);
    if (found == nullptr) {
        return false;
    }
    if (!fbe::scene::finite(r, g, b)) {
        return fbe::fail("a light's color is finite");
    }
    found->color = { r, g, b };
    return true;
}

FBE_API bool fbe_light_color(uint64_t light, fbe_color_t* color) {
    const light_t* found = light_of(light);
    if (found == nullptr) {
        return false;
    }
    *color = fbe::scene::color_of(found->color);
    return true;
}

FBE_API bool fbe_light_set_shadows(uint64_t light, bool shadows) {
    light_t* found = fbe::scene::light_of(light);
    if (found == nullptr) {
        return false;
    }
    found->shadows = shadows;
    return true;
}

FBE_API bool fbe_light_shadows(uint64_t light) {
    const light_t* found = fbe::scene::light_of(light);
    return found != nullptr && found->shadows;
}

FBE_API bool fbe_light_set_shadow_size(uint64_t light, int32_t size) {
    light_t* found = fbe::scene::light_of(light);
    if (found == nullptr) {
        return false;
    }
    if (size < 16 || size > 8192) {
        return fbe::fail(std::format("a shadow's tile is 16 to 8192 texels a side, not {}", size));
    }
    found->shadow_size = size;
    return true;
}

FBE_API int32_t fbe_light_shadow_size(uint64_t light) {
    const light_t* found = fbe::scene::light_of(light);
    return found != nullptr ? found->shadow_size : 0;
}

FBE_API bool fbe_light_set_shadow_bias(uint64_t light, float depth, float normal) {
    light_t* found = fbe::scene::light_of(light);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(depth) || !std::isfinite(normal) || depth < 0.0F || normal < 0.0F) {
        return fbe::fail(
            std::format("a shadow's biases are 0 or more, not {} and {}", depth, normal));
    }
    found->shadow_bias = depth;
    found->shadow_normal_bias = normal;
    return true;
}

FBE_API float fbe_light_shadow_bias(uint64_t light) {
    const light_t* found = fbe::scene::light_of(light);
    return found != nullptr ? found->shadow_bias : 0.0F;
}

FBE_API float fbe_light_shadow_normal_bias(uint64_t light) {
    const light_t* found = fbe::scene::light_of(light);
    return found != nullptr ? found->shadow_normal_bias : 0.0F;
}

FBE_API bool fbe_light_set_cascades(uint64_t light, int32_t cascades) {
    light_t* found = fbe::scene::light_of(light);
    if (found == nullptr) {
        return false;
    }
    if (cascades < 1 || cascades > 4) {
        return fbe::fail(std::format("a light has 1 to 4 cascades, not {}", cascades));
    }
    found->cascades = cascades;
    return true;
}

FBE_API int32_t fbe_light_cascades(uint64_t light) {
    const light_t* found = fbe::scene::light_of(light);
    return found != nullptr ? found->cascades : 0;
}

FBE_API bool fbe_light_set_shadow_distance(uint64_t light, float distance) {
    light_t* found = fbe::scene::light_of(light);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(distance) || distance <= 0.0F) {
        return fbe::fail(std::format("a shadow reaches a distance above 0, not {}", distance));
    }
    found->shadow_distance = distance;
    return true;
}

FBE_API float fbe_light_shadow_distance(uint64_t light) {
    const light_t* found = fbe::scene::light_of(light);
    return found != nullptr ? found->shadow_distance : 0.0F;
}

FBE_API bool fbe_light_set_intensity(uint64_t light, float intensity) {
    light_t* found = light_of(light);
    if (found == nullptr) {
        return false;
    }
    if (intensity < 0.0F || !std::isfinite(intensity)) {
        return fbe::fail(std::format("a light's intensity is 0 or more, not {}", intensity));
    }
    found->intensity = intensity;
    return true;
}

FBE_API float fbe_light_intensity(uint64_t light) {
    const light_t* found = light_of(light);
    return found != nullptr ? found->intensity : 0.0F;
}

FBE_API bool fbe_light_set_range(uint64_t light, float range) {
    light_t* found = light_of(light);
    if (found == nullptr) {
        return false;
    }
    if (range <= 0.0F || !std::isfinite(range)) {
        return fbe::fail(std::format("a light's range is above 0, not {}", range));
    }
    found->range = range;
    return true;
}

FBE_API float fbe_light_range(uint64_t light) {
    const light_t* found = light_of(light);
    return found != nullptr ? found->range : 0.0F;
}

FBE_API bool fbe_light_set_cone(uint64_t light, float inner, float outer) {
    light_t* found = light_of(light);
    if (found == nullptr) {
        return false;
    }
    if (std::isnan(inner) || std::isnan(outer) || inner < 0.0F || outer < inner ||
        outer > std::numbers::pi_v<float> / 2.0F) {
        return fbe::fail(std::format(
            "a spot light's cone is 0 <= inner <= outer <= Pi / 2, not {} and {}", inner, outer));
    }
    found->inner_cone = inner;
    found->outer_cone = outer;
    return true;
}

FBE_API float fbe_light_inner_cone(uint64_t light) {
    const light_t* found = light_of(light);
    return found != nullptr ? found->inner_cone : 0.0F;
}

FBE_API float fbe_light_outer_cone(uint64_t light) {
    const light_t* found = light_of(light);
    return found != nullptr ? found->outer_cone : 0.0F;
}

FBE_API int32_t fbe_world_lights(uint64_t world, uint64_t* lights, int32_t capacity) {
    const std::uint32_t slot = fbe::scene::world_slot(world);
    if (slot == fbe::scene::none) {
        return -1;
    }
    std::vector<std::uint32_t> found;
    fbe::scene::lights_in(fbe::scene::scene().worlds[slot].roots, found);
    const auto count = std::min(found.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    for (std::size_t i = 0; i < count; ++i) {
        lights[i] = fbe::scene::scene().entities.slots.handle(found[i]);
    }
    return static_cast<std::int32_t>(found.size());
}

FBE_API bool fbe_world_set_environment(uint64_t world, uint64_t environment, float intensity) {
    const std::uint32_t slot = fbe::scene::world_slot(world);
    if (slot == fbe::scene::none) {
        return false;
    }
    if (!(intensity >= 0.0F) || !std::isfinite(intensity)) {
        return fbe::fail(std::format("an environment's intensity is 0 or more, not {}", intensity));
    }
    if (environment != 0 && fbe_environment_levels(environment) < 0) {
        return false;
    }
    fbe::scene::scene().worlds[slot].environment = environment;
    fbe::scene::scene().worlds[slot].environment_intensity = intensity;
    return true;
}

FBE_API uint64_t fbe_world_environment(uint64_t world) {
    const std::uint32_t slot = fbe::scene::world_slot(world);
    return slot != fbe::scene::none ? fbe::scene::scene().worlds[slot].environment : 0;
}

FBE_API float fbe_world_environment_intensity(uint64_t world) {
    const std::uint32_t slot = fbe::scene::world_slot(world);
    return slot != fbe::scene::none ? fbe::scene::scene().worlds[slot].environment_intensity : 0.0F;
}

FBE_API bool fbe_world_set_sky_atmosphere(uint64_t world, uint64_t sun, float brightness) {
    const std::uint32_t slot = fbe::scene::world_slot(world);
    if (slot == fbe::scene::none) {
        return false;
    }
    if (sun != 0) {
        const light_t* found = fbe::scene::light_of(sun);
        if (found == nullptr) {
            return false;
        }
        if (found->type != FBE_LIGHT_DIRECTIONAL) {
            return fbe::fail("a sky's sun is a directional light");
        }
        if (fbe::scene::scene().entities.world[found->entity] != slot) {
            return fbe::fail("a sky's sun is in its World");
        }
    }
    if (!std::isfinite(brightness) || brightness < 0.0F) {
        return fbe::fail(std::format("a sky's brightness is 0 or more, not {}", brightness));
    }
    fbe::scene::world_t& found = fbe::scene::scene().worlds[slot];
    fbe::scene::release_texture(found.sky_texture);
    found.sky_texture = 0;
    found.sky = 1;
    found.sky_light = sun;
    found.sky_brightness = brightness;
    found.sky_sun = fbe::scene::vec3_t(0.0F, 1.0F, 0.0F);
    if (sun != 0) {
        const std::uint32_t entity = fbe::scene::entity_slot(sun);
        fbe::scene::update_world(entity);
        found.sky_sun = fbe::scene::scene().entities.world_rotation[entity] *
                        fbe::scene::vec3_t(0.0F, 0.0F, 1.0F);
    }
    return true;
}

FBE_API bool fbe_world_set_sky_texture(uint64_t world, uint64_t texture, float brightness) {
    const std::uint32_t slot = fbe::scene::world_slot(world);
    if (slot == fbe::scene::none) {
        return false;
    }
    if (!std::isfinite(brightness) || brightness < 0.0F) {
        return fbe::fail(std::format("a sky's brightness is 0 or more, not {}", brightness));
    }
    if (!fbe_texture_retain(texture)) {
        return false;
    }
    fbe::scene::world_t& found = fbe::scene::scene().worlds[slot];
    fbe::scene::release_texture(found.sky_texture);
    found.sky_texture = texture;
    found.sky = 2;
    found.sky_light = 0;
    found.sky_brightness = brightness;
    return true;
}

FBE_API bool fbe_world_clear_sky(uint64_t world) {
    const std::uint32_t slot = fbe::scene::world_slot(world);
    if (slot == fbe::scene::none) {
        return false;
    }
    fbe::scene::world_t& found = fbe::scene::scene().worlds[slot];
    fbe::scene::release_texture(found.sky_texture);
    found.sky_texture = 0;
    found.sky = 0;
    found.sky_light = 0;
    return true;
}

FBE_API bool fbe_world_set_ambient(uint64_t world, float r, float g, float b) {
    const std::uint32_t slot = fbe::scene::world_slot(world);
    if (slot == fbe::scene::none) {
        return false;
    }
    if (!fbe::scene::finite(r, g, b)) {
        return fbe::fail("the ambient light is finite");
    }
    fbe::scene::scene().worlds[slot].ambient = { r, g, b };
    return true;
}

FBE_API bool fbe_world_ambient(uint64_t world, fbe_color_t* color) {
    const std::uint32_t slot = fbe::scene::world_slot(world);
    if (slot == fbe::scene::none) {
        return false;
    }
    *color = fbe::scene::color_of(fbe::scene::scene().worlds[slot].ambient);
    return true;
}

FBE_API bool fbe_camera_set_fog_mode(uint64_t camera, int32_t mode) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (mode < FBE_FOG_NONE || mode > FBE_FOG_EXPONENTIAL_SQUARED) {
        return fbe::fail(std::format("{} isn't a FogMode", mode));
    }
    found->fog_mode = mode;
    return true;
}

FBE_API int32_t fbe_camera_fog_mode(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->fog_mode : -1;
}

FBE_API bool fbe_camera_set_fog_color(uint64_t camera, float r, float g, float b) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (!fbe::scene::finite(r, g, b)) {
        return fbe::fail("a fog's color is finite");
    }
    found->fog_color = { r, g, b };
    return true;
}

FBE_API bool fbe_camera_fog_color(uint64_t camera, fbe_color_t* color) {
    const camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    *color = fbe::scene::color_of(found->fog_color);
    return true;
}

FBE_API bool fbe_camera_set_fog_range(uint64_t camera, float start, float end) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (std::isnan(start) || start < 0.0F || end <= start || !std::isfinite(end)) {
        return fbe::fail(
            std::format("a fog's range is 0 <= start < end, finite, not {} to {}", start, end));
    }
    found->fog_start = start;
    found->fog_end = end;
    return true;
}

FBE_API float fbe_camera_fog_start(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->fog_start : 0.0F;
}

FBE_API float fbe_camera_fog_end(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->fog_end : 0.0F;
}

FBE_API bool fbe_camera_set_hdr(uint64_t camera, bool hdr) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (hdr && found->stored_values) {
        return fbe::fail("a camera that draws in stored values isn't HDR, as they have no light "
                         "past white to tonemap");
    }
    found->hdr = hdr;
    return true;
}

FBE_API bool fbe_camera_hdr(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr && found->hdr;
}

FBE_API bool fbe_camera_set_exposure(uint64_t camera, float exposure) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (!(exposure >= 0.0F) || !std::isfinite(exposure)) {
        return fbe::fail(std::format("a camera's exposure is 0 or more, not {}", exposure));
    }
    found->exposure = exposure;
    return true;
}

FBE_API float fbe_camera_exposure(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->exposure : 0.0F;
}

FBE_API bool fbe_camera_set_tonemap(uint64_t camera, int32_t tonemap) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (tonemap < FBE_TONEMAP_NONE || tonemap > FBE_TONEMAP_NEUTRAL) {
        return fbe::fail(std::format("there's no tonemap {}", tonemap));
    }
    found->tonemap = tonemap;
    return true;
}

FBE_API int32_t fbe_camera_tonemap(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->tonemap : -1;
}

FBE_API bool fbe_camera_set_gamma(uint64_t camera, float gamma) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    // What it's raised to is one over it, which too small a gamma's isn't a number.
    if (!(gamma > 0.0F) || !std::isfinite(gamma) || !std::isfinite(1.0F / gamma)) {
        return fbe::fail(std::format("a camera's gamma is above 0, not {}", gamma));
    }
    found->gamma = gamma;
    return true;
}

FBE_API float fbe_camera_gamma(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->gamma : 0.0F;
}

FBE_API bool fbe_camera_set_bloom(uint64_t camera, float intensity, float threshold) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (!(intensity >= 0.0F) || !std::isfinite(intensity) || !(threshold >= 0.0F) ||
        !std::isfinite(threshold)) {
        return fbe::fail(
            std::format("a camera's bloom's intensity and threshold are 0 or more, not "
                        "{} and {}",
                        intensity, threshold));
    }
    found->bloom_intensity = intensity;
    found->bloom_threshold = threshold;
    return true;
}

FBE_API float fbe_camera_bloom_intensity(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->bloom_intensity : 0.0F;
}

FBE_API float fbe_camera_bloom_threshold(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->bloom_threshold : 0.0F;
}

FBE_API bool fbe_camera_set_grade(uint64_t camera, const float* values) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    for (int at = 0; at < 10; ++at) {
        if (!std::isfinite(values[at])) {
            return fbe::fail("a camera's color grade is finite");
        }
    }
    for (int at = 0; at < 3; ++at) {
        if (values[at] < 0.0F || values[6 + at] <= 0.0F) {
            return fbe::fail("a camera's color grade's slope is 0 or more, and its power above 0");
        }
    }
    if (values[9] < 0.0F) {
        return fbe::fail("a camera's color grade's saturation is 0 or more");
    }
    found->grade_slope = { values[0], values[1], values[2] };
    found->grade_offset = { values[3], values[4], values[5] };
    found->grade_power = { values[6], values[7], values[8] };
    found->grade_saturation = values[9];
    return true;
}

FBE_API bool fbe_camera_grade(uint64_t camera, float* values) {
    const camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    std::ranges::copy(found->grade_slope, values);
    std::ranges::copy(found->grade_offset, values + 3);
    std::ranges::copy(found->grade_power, values + 6);
    values[9] = found->grade_saturation;
    return true;
}

FBE_API bool fbe_camera_set_fog_density(uint64_t camera, float density) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (density < 0.0F || !std::isfinite(density)) {
        return fbe::fail(std::format("a fog's density is 0 or more, not {}", density));
    }
    found->fog_density = density;
    return true;
}

FBE_API float fbe_camera_fog_density(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->fog_density : 0.0F;
}

} // extern "C"
