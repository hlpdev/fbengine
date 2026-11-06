// Instances: one entity drawing a geometry many times over, each with a matrix and a color of its
// own, in bulk: grass, debris, crowds' props, where an entity each would cost too much. Kept in an
// order where those near each other are together, so what's out of view goes a cluster at a time;
// those whose matrices mirror come after the rest, drawn by the other front face, so nothing per
// instance goes in what shades them.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <new>
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

/// Whether a matrix turns what it's given inside out, as the shader's normals go by.
bool instance_mirrors(const fbe_instance_t& instance) {
    const float* m = instance.matrix.m;
    const vec3_t a(m[0], m[1], m[2]);
    const vec3_t b(m[4], m[5], m[6]);
    const vec3_t c(m[8], m[9], m[10]);
    return dot(a, cross(b, c)) < 0.0F;
}

/// Which cluster a place in the order's in: the unmirrored's first, then the mirrored's, each from
/// its own start.
std::uint32_t cluster_at(const instances_t& found, std::uint32_t place) {
    const auto unmirrored = static_cast<std::uint32_t>(found.mirrored_first);
    if (place < unmirrored) {
        return place / cluster_size;
    }
    return ((unmirrored + cluster_size - 1) / cluster_size) + ((place - unmirrored) / cluster_size);
}

/// Puts instances in an order where those that mirror are after those that don't, and those near
/// each other are together: along a curve through their places, as Morton's goes through a grid.
void put_in_order(instances_t& found, const fbe_instance_t* given, std::size_t count) {
    std::vector<fbe_instance_t> items(count);
    std::vector<std::uint32_t> slots(count);
    std::vector<std::uint32_t> indices(count);
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
        // What's no number or goes on forever is somewhere: its order's all that hangs on it.
        return std::isfinite(along)
                   ? static_cast<std::uint32_t>(std::clamp(along, 0.0F, 1.0F) * 1023.0F)
                   : 0U;
    };
    std::vector<std::pair<std::uint32_t, std::uint32_t>> order(count);
    for (std::size_t i = 0; i < count; ++i) {
        const vec3_t place = place_of(given[i]);
        order[i] = { (instance_mirrors(given[i]) ? 0x80000000U : 0U) |
                         spread_bits(part(place.x, low.x, high.x)) |
                         (spread_bits(part(place.y, low.y, high.y)) << 1U) |
                         (spread_bits(part(place.z, low.z, high.z)) << 2U),
                     static_cast<std::uint32_t>(i) };
    }
    std::ranges::sort(order);
    std::size_t faded = 0;
    std::size_t unmirrored = 0;
    for (std::size_t at = 0; at < count; ++at) {
        const std::uint32_t index = order[at].second;
        unmirrored += order[at].first < 0x80000000U ? 1U : 0U;
        items[at] = given[index];
        slots[index] = static_cast<std::uint32_t>(at);
        indices[at] = index;
        faded += given[index].color.a < 1.0F ? 1U : 0U;
    }
    found.items.swap(items);
    found.slots.swap(slots);
    found.indices.swap(indices);
    found.faded = faded;
    found.mirrored_first = unmirrored;
}

/// A cluster's box and biggest, from its instances and its geometry's box's middle and half.
void work_out_cluster(const instances_t& found, instance_cluster_t& cluster, vec3_t middle,
                      vec3_t reach) {
    const float geometry_radius = length(reach);
    cluster.radius = 0.0F;
    for (std::uint32_t at = cluster.first; at < cluster.first + cluster.count; ++at) {
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
        cluster.low = at == cluster.first ? from
                                          : vec3_t(std::fmin(cluster.low.x, from.x),
                                                   std::fmin(cluster.low.y, from.y),
                                                   std::fmin(cluster.low.z, from.z));
        cluster.high = at == cluster.first ? to
                                           : vec3_t(std::fmax(cluster.high.x, to.x),
                                                    std::fmax(cluster.high.y, to.y),
                                                    std::fmax(cluster.high.z, to.z));
        const float scale = std::fmax(std::fmax(length(matrix.axis(0)), length(matrix.axis(1))),
                                      length(matrix.axis(2)));
        cluster.radius = std::fmax(cluster.radius, geometry_radius * scale);
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
    copy.all_changed = true;
    copy.changed.clear();
    if (copy.geometry != none) {
        retain_geometry(copy.geometry);
    }
    retain_material(copy.material);
}

const std::vector<instance_cluster_t>& instance_clusters(instances_t& found) {
    const scene_t& s = scene();
    const std::uint64_t version = found.geometry != none ? s.geometries[found.geometry].version : 0;
    const std::uint64_t cull_version =
        found.geometry != none ? s.geometries[found.geometry].cull_version : 0;
    const bool whole = found.clusters_stale || found.clusters_geometry != found.geometry ||
                       found.clusters_version != version ||
                       found.clusters_cull_version != cull_version;
    if (!whole && found.stale_clusters.empty()) {
        return found.clusters;
    }
    vec3_t low;
    vec3_t high;
    // Each instance's box is its geometry's cull box, if it has one, as what its shader moves
    // goes out of its bounds; else its bounds.
    if (found.geometry != none && s.geometries[found.geometry].has_cull_box) {
        low = s.geometries[found.geometry].cull_min;
        high = s.geometries[found.geometry].cull_max;
    } else if (found.geometry != none) {
        geometry_bounds(found.geometry, low, high);
    }
    const vec3_t both = low + high;
    const vec3_t across = high - low;
    const vec3_t middle = both * 0.5F;
    const vec3_t reach = across * 0.5F;
    if (!whole) {
        for (const std::uint32_t at : found.stale_clusters) {
            work_out_cluster(found, found.clusters[at], middle, reach);
        }
        found.stale_clusters.clear();
        return found.clusters;
    }
    std::vector<instance_cluster_t> clusters;
    if (found.geometry != none) {
        const auto count = static_cast<std::uint32_t>(found.items.size());
        const auto unmirrored = static_cast<std::uint32_t>(found.mirrored_first);
        clusters.reserve((count / cluster_size) + 2);
        const auto add_run = [&](std::uint32_t from, std::uint32_t to, bool mirrored) {
            for (std::uint32_t first = from; first < to; first += cluster_size) {
                instance_cluster_t cluster;
                cluster.first = first;
                cluster.count = std::min(cluster_size, to - first);
                cluster.mirrored = mirrored;
                work_out_cluster(found, cluster, middle, reach);
                clusters.push_back(cluster);
            }
        };
        add_run(0, unmirrored, false);
        add_run(unmirrored, count, true);
    }
    found.clusters.swap(clusters);
    found.clusters_stale = false;
    found.clusters_geometry = found.geometry;
    found.clusters_version = version;
    found.clusters_cull_version = cull_version;
    found.stale_clusters.clear();
    return found.clusters;
}

std::uint32_t cluster_lod(const instances_t& found, const instance_cluster_t& cluster,
                          const mat4_t& matrix, const camera_t& camera, vec3_t eye,
                          float half_height) {
    if (scene().geometries[found.geometry].lods.empty()) {
        return found.geometry;
    }
    const float scale = std::fmax(std::fmax(length(matrix.axis(0)), length(matrix.axis(1))),
                                  length(matrix.axis(2)));
    const vec3_t both = cluster.low + cluster.high;
    const float apart = length(transform_point(matrix, both * 0.5F) - eye) -
                        (length(cluster.high - cluster.low) * 0.5F * scale);
    return lod_at(found.geometry, std::fmax(apart, 0.0F), cluster.radius * scale, camera,
                  half_height);
}

std::vector<instance_level_t> instances_levels(instances_t& found, const mat4_t& matrix,
                                               const camera_t& camera, vec3_t eye,
                                               float half_height) {
    std::vector<instance_level_t> levels;
    for (const instance_cluster_t& cluster : instance_clusters(found)) {
        const instance_level_t level{
            .level = cluster_lod(found, cluster, matrix, camera, eye, half_height),
            .mirrored = cluster.mirrored,
        };
        const bool known = std::ranges::any_of(levels, [&level](const instance_level_t& other) {
            return other.level == level.level && other.mirrored == level.mirrored;
        });
        if (level.level != none && !known) {
            levels.push_back(level);
        }
    }
    return levels;
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

FBE_API bool fbe_instances_set(uint64_t instances, const fbe_instance_t* all, int32_t count) try {
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
    found->all_changed = true;
    found->changed.clear();
    ++found->version;
    return true;
} catch (const std::bad_alloc&) {
    return fbe::fail("out of memory");
}

FBE_API int32_t fbe_instances_count(uint64_t instances) {
    const instances_t* found = fbe::scene::instances_of(instances);
    return found != nullptr ? static_cast<std::int32_t>(found->items.size()) : -1;
}

FBE_API bool fbe_instances_set_one(uint64_t instances, int32_t index,
                                   const fbe_instance_t* one) try {
    instances_t* found = fbe::scene::instances_of(instances);
    if (found == nullptr) {
        return false;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= found->items.size()) {
        return fbe::fail(
            std::format("an Instances has {} of them, not one at {}", found->items.size(), index));
    }
    const std::uint32_t place = found->slots[static_cast<std::size_t>(index)];
    // Coming to mirror, or not, it goes with those it now draws with, which moves them all.
    if (fbe::scene::instance_mirrors(*one) != fbe::scene::instance_mirrors(found->items[place])) {
        std::vector<fbe_instance_t> all(found->items.size());
        for (std::size_t at = 0; at < all.size(); ++at) {
            all[found->indices[at]] = found->items[at];
        }
        all[static_cast<std::size_t>(index)] = *one;
        fbe::scene::put_in_order(*found, all.data(), all.size());
        found->clusters_stale = true;
        found->all_changed = true;
        found->changed.clear();
        ++found->version;
        return true;
    }
    found->faded -= found->items[place].color.a < 1.0F ? 1U : 0U;
    found->faded += one->color.a < 1.0F ? 1U : 0U;
    found->items[place] = *one;
    ++found->version;
    // Only what's changed goes again, unless so much has that it may as well all.
    if (!found->all_changed) {
        if (found->changed.size() >= found->items.size() / 4) {
            found->all_changed = true;
            found->changed.clear();
        } else {
            try {
                found->changed.push_back(place);
            } catch (const std::bad_alloc&) {
                found->all_changed = true;
                found->changed.clear();
            }
        }
    }
    if (!found->clusters_stale) {
        const std::uint32_t cluster = fbe::scene::cluster_at(*found, place);
        if (found->stale_clusters.empty() || found->stale_clusters.back() != cluster) {
            try {
                found->stale_clusters.push_back(cluster);
            } catch (const std::bad_alloc&) {
                found->clusters_stale = true;
            }
        }
    }
    return true;
} catch (const std::bad_alloc&) {
    return fbe::fail("out of memory");
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
