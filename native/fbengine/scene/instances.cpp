// Instances: one entity drawing a geometry many times over, each with a matrix and a color of its
// own, in bulk: grass, debris, crowds' props, where an entity each would cost too much. Kept in an
// order where those near each other are together, so what's out of view goes a cluster at a time.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <utility>
#include <vector>

namespace fbe::scene {

namespace {

constexpr std::int32_t most_instances = 4194304;
constexpr std::uint32_t cluster_size = 128;

/// A number's low 10 bits, two zeros after each, so three can interleave.
std::uint32_t spread_bits(std::uint32_t bits) {
    std::uint32_t spread = bits & 0x3FFU;
    spread = (spread | (spread << 16U)) & 0x030000FFU;
    spread = (spread | (spread << 8U)) & 0x0300F00FU;
    spread = (spread | (spread << 4U)) & 0x030C30C3U;
    return (spread | (spread << 2U)) & 0x09249249U;
}

vec3_t place_of(const fbe_instance_t& instance) {
    return { instance.matrix.m[12], instance.matrix.m[13], instance.matrix.m[14] };
}

/// Puts instances in an order where those near each other are together: along a curve through
/// their places, as Morton's goes through a grid.
void put_in_order(instances_t& found, const fbe_instance_t* given, std::size_t count) {
    vec3_t low(0.0F, 0.0F, 0.0F);
    vec3_t high(0.0F, 0.0F, 0.0F);
    for (std::size_t i = 0; i < count; ++i) {
        const vec3_t place = place_of(given[i]);
        low = i == 0 ? place
                     : vec3_t(std::fmin(low.x, place.x), std::fmin(low.y, place.y),
                              std::fmin(low.z, place.z));
        high = i == 0 ? place
                      : vec3_t(std::fmax(high.x, place.x), std::fmax(high.y, place.y),
                               std::fmax(high.z, place.z));
    }
    const auto part = [](float at, float from, float to) {
        const float wide = to - from;
        const float along = wide > 0.0F ? (at - from) / wide : 0.0F;
        return static_cast<std::uint32_t>(std::clamp(along, 0.0F, 1.0F) * 1023.0F);
    };
    std::vector<std::pair<std::uint32_t, std::uint32_t>> order(count);
    for (std::size_t i = 0; i < count; ++i) {
        const vec3_t place = place_of(given[i]);
        order[i] = { spread_bits(part(place.x, low.x, high.x)) |
                         (spread_bits(part(place.y, low.y, high.y)) << 1U) |
                         (spread_bits(part(place.z, low.z, high.z)) << 2U),
                     static_cast<std::uint32_t>(i) };
    }
    std::ranges::sort(order);
    found.items.resize(count);
    found.slots.resize(count);
    found.indices.resize(count);
    for (std::size_t at = 0; at < count; ++at) {
        const std::uint32_t index = order[at].second;
        found.items[at] = given[index];
        found.slots[index] = static_cast<std::uint32_t>(at);
        found.indices[at] = index;
    }
}

} // namespace

instances_t* instances_of(std::uint64_t handle) {
    const std::uint32_t entity = entity_slot(handle);
    if (entity == none) {
        return nullptr;
    }
    if (scene().entities.kind[entity] != FBE_ENTITY_INSTANCES) {
        fail("the entity isn't an Instances");
        return nullptr;
    }
    return &scene().instances[scene().entities.component[entity]];
}

void add_instances(std::uint32_t entity) {
    scene_t& s = scene();
    s.entities.component[entity] = static_cast<std::uint32_t>(s.instances.size());
    instances_t made;
    made.entity = entity;
    s.instances.push_back(std::move(made));
}

void remove_instances(std::uint32_t entity) {
    scene_t& s = scene();
    const std::uint32_t index = s.entities.component[entity];
    if (s.instances[index].geometry != none) {
        release_geometry(s.instances[index].geometry);
    }
    release_material(s.instances[index].material);
    if (index != s.instances.size() - 1) {
        s.instances[index] = std::move(s.instances.back());
        s.entities.component[s.instances[index].entity] = index;
    }
    s.instances.pop_back();
}

void copy_instances(std::uint32_t from, std::uint32_t to) {
    scene_t& s = scene();
    instances_t& copy = s.instances[s.entities.component[to]];
    copy = s.instances[s.entities.component[from]];
    copy.entity = to;
    if (copy.geometry != none) {
        retain_geometry(copy.geometry);
    }
    retain_material(copy.material);
}

const std::vector<instance_cluster_t>& instance_clusters(instances_t& found) {
    const scene_t& s = scene();
    const std::uint64_t version = found.geometry != none ? s.geometries[found.geometry].version : 0;
    if (!found.clusters_stale && found.clusters_geometry == found.geometry &&
        found.clusters_version == version) {
        return found.clusters;
    }
    found.clusters_stale = false;
    found.clusters_geometry = found.geometry;
    found.clusters_version = version;
    found.clusters.clear();
    if (found.geometry == none) {
        return found.clusters;
    }
    vec3_t low;
    vec3_t high;
    geometry_bounds(found.geometry, low, high);
    const vec3_t both = low + high;
    const vec3_t across = high - low;
    const vec3_t middle = both * 0.5F;
    const vec3_t reach = across * 0.5F;
    const auto count = static_cast<std::uint32_t>(found.items.size());
    for (std::uint32_t first = 0; first < count; first += cluster_size) {
        instance_cluster_t cluster;
        cluster.first = first;
        cluster.count = std::min(cluster_size, count - first);
        for (std::uint32_t at = first; at < first + cluster.count; ++at) {
            mat4_t matrix;
            std::ranges::copy(found.items[at].matrix.m, matrix.m.begin());
            // Its box's middle through the matrix, and how far its corners go from it each way.
            const vec3_t center = transform_point(matrix, middle);
            const vec3_t x = matrix.axis(0) * reach.x;
            const vec3_t y = matrix.axis(1) * reach.y;
            const vec3_t z = matrix.axis(2) * reach.z;
            const vec3_t out(std::fabs(x.x) + std::fabs(y.x) + std::fabs(z.x),
                             std::fabs(x.y) + std::fabs(y.y) + std::fabs(z.y),
                             std::fabs(x.z) + std::fabs(y.z) + std::fabs(z.z));
            const vec3_t from = center - out;
            const vec3_t to = center + out;
            cluster.low = at == first ? from
                                      : vec3_t(std::fmin(cluster.low.x, from.x),
                                               std::fmin(cluster.low.y, from.y),
                                               std::fmin(cluster.low.z, from.z));
            cluster.high = at == first ? to
                                       : vec3_t(std::fmax(cluster.high.x, to.x),
                                                std::fmax(cluster.high.y, to.y),
                                                std::fmax(cluster.high.z, to.z));
        }
        found.clusters.push_back(cluster);
    }
    return found.clusters;
}

bool instances_bounds(instances_t& found, vec3_t& min, vec3_t& max) {
    const std::vector<instance_cluster_t>& clusters = instance_clusters(found);
    if (clusters.empty()) {
        return false;
    }
    min = clusters[0].low;
    max = clusters[0].high;
    for (const instance_cluster_t& cluster : clusters) {
        min = vec3_t(std::fmin(min.x, cluster.low.x), std::fmin(min.y, cluster.low.y),
                     std::fmin(min.z, cluster.low.z));
        max = vec3_t(std::fmax(max.x, cluster.high.x), std::fmax(max.y, cluster.high.y),
                     std::fmax(max.z, cluster.high.z));
    }
    return true;
}

} // namespace fbe::scene

using fbe::scene::instances_t;
using fbe::scene::scene;

extern "C" {

FBE_API bool fbe_instances_set_geometry(uint64_t instances, uint64_t geometry) {
    instances_t* found = fbe::scene::instances_of(instances);
    if (found == nullptr) {
        return false;
    }
    std::uint32_t slot = fbe::scene::none;
    if (geometry != 0) {
        slot = fbe::scene::geometry_slot(geometry);
        if (slot == fbe::scene::none) {
            return false;
        }
        fbe::scene::retain_geometry(slot);
    }
    if (found->geometry != fbe::scene::none) {
        fbe::scene::release_geometry(found->geometry);
    }
    found->geometry = slot;
    found->clusters_stale = true;
    return true;
}

FBE_API uint64_t fbe_instances_geometry(uint64_t instances) {
    const instances_t* found = fbe::scene::instances_of(instances);
    return found != nullptr && found->geometry != fbe::scene::none
               ? scene().geometry_slots.handle(found->geometry)
               : 0;
}

FBE_API bool fbe_instances_set_material(uint64_t instances, uint64_t material) {
    instances_t* found = fbe::scene::instances_of(instances);
    if (found == nullptr) {
        return false;
    }
    std::uint32_t slot = fbe::scene::none;
    if (material != 0) {
        slot = fbe::scene::material_slot(material);
        if (slot == fbe::scene::none) {
            return false;
        }
    }
    fbe::scene::retain_material(slot);
    fbe::scene::release_material(found->material);
    found->material = slot;
    return true;
}

FBE_API uint64_t fbe_instances_material(uint64_t instances) {
    const instances_t* found = fbe::scene::instances_of(instances);
    return found != nullptr && found->material != fbe::scene::none
               ? scene().material_slots.handle(found->material)
               : 0;
}

FBE_API bool fbe_instances_set(uint64_t instances, const fbe_instance_t* all, int32_t count) {
    instances_t* found = fbe::scene::instances_of(instances);
    if (found == nullptr) {
        return false;
    }
    if (count < 0 || count > fbe::scene::most_instances) {
        return fbe::fail(std::format("an Instances has 0 to {} of them, not {}",
                                     fbe::scene::most_instances, count));
    }
    fbe::scene::put_in_order(*found, all, static_cast<std::size_t>(count));
    found->clusters_stale = true;
    ++found->version;
    return true;
}

FBE_API int32_t fbe_instances_count(uint64_t instances) {
    const instances_t* found = fbe::scene::instances_of(instances);
    return found != nullptr ? static_cast<std::int32_t>(found->items.size()) : -1;
}

FBE_API bool fbe_instances_set_one(uint64_t instances, int32_t index, const fbe_instance_t* one) {
    instances_t* found = fbe::scene::instances_of(instances);
    if (found == nullptr) {
        return false;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= found->items.size()) {
        return fbe::fail(
            std::format("an Instances has {} of them, not one at {}", found->items.size(), index));
    }
    found->items[found->slots[static_cast<std::size_t>(index)]] = *one;
    found->clusters_stale = true;
    ++found->version;
    return true;
}

FBE_API bool fbe_instances_get(uint64_t instances, int32_t index, fbe_instance_t* one) {
    const instances_t* found = fbe::scene::instances_of(instances);
    if (found == nullptr) {
        return false;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= found->items.size()) {
        return fbe::fail(
            std::format("an Instances has {} of them, not one at {}", found->items.size(), index));
    }
    *one = found->items[found->slots[static_cast<std::size_t>(index)]];
    return true;
}

} // extern "C"
