// Collisions: moving entities swept as ellipsoids against meshes' triangles, boxes, and
// ellipsoids, with stop and slide responses and stepping up, by collision groups.
//
// Each move is worked out in the mover's own space, where its ellipsoid is a sphere: y scaled by
// its horizontal radius over its vertical. The targets are snapshot first, so every move reads
// the same targets and writes only its own result, and the moves can run side by side; the job
// system (E8 step 0) takes over for_each_move when it lands.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/math.hpp"
#include "scene/scene.hpp"
#include "scene/sweep.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <vector>

namespace fbe::scene {

namespace {

constexpr float tiny = 1e-6F;
/// A ground plane's shape, and a terrain's, as targets have them.
constexpr std::int32_t plane_shape = -1;
constexpr std::int32_t terrain_shape = -2;
constexpr std::int32_t most_iterations = 64;
constexpr float most_skin = 0.1F;

/// What a mover can touch, by the snapshot: where it is and what shape it's touched as.
struct target_t {
    std::uint32_t entity = none;
    std::int32_t group = 0;
    std::int32_t shape = FBE_COLLISION_ELLIPSOID;
    mat4_t matrix;
    bool mirrored = false;
    vec3_t min;
    vec3_t max;
    vec3_t center;
    float horizontal = 1.0F;
    float vertical = 1.0F;
    std::uint32_t geometry = none;
};

/// A mover's move, as the snapshot has it.
struct mover_t {
    std::uint32_t entity = none;
    std::int32_t group = 0;
    vec3_t from;
    vec3_t to;
    float horizontal = 1.0F;
    float vertical = 1.0F;
    float step = 0.0F;
    /// The least a normal faces up, its y in the world, on ground: its max slope's cosine.
    float ground = 0.70710678F;
};

/// Where a move ended, and what it touched, with the targets' indices.
struct result_t {
    vec3_t end;
    std::vector<contact_t> contacts;
    std::vector<std::uint32_t> touched;
};

/// What every move reads: the world's rules and targets.
struct snapshot_t {
    const world_t* world = nullptr;
    std::vector<target_t> targets;
};

/// Runs `work` for each move. Each writes only its own result, so the job system can run them on
/// its workers.
template <typename Work>
void for_each_move(std::size_t count, Work&& work) {
    for (std::size_t i = 0; i < count; ++i) {
        work(i);
    }
}

std::uint32_t pair_of(std::int32_t source, std::int32_t target) {
    return (static_cast<std::uint32_t>(source) << 16U) | static_cast<std::uint32_t>(target);
}

std::int32_t response_of(const world_t& world, std::int32_t source, std::int32_t target) {
    const std::uint32_t pair = pair_of(source, target);
    const auto found = std::ranges::find_if(
        world.collisions, [pair](const auto& rule) { return rule.first == pair; });
    return found != world.collisions.end() ? found->second : FBE_RESPONSE_NONE;
}

bool valid_group(std::int32_t group) {
    if (group < 0 || group >= FBE_COLLISION_GROUPS) {
        return fail(
            std::format("a collision group is 0 to {}, not {}", FBE_COLLISION_GROUPS - 1, group));
    }
    return true;
}

vec3_t least(vec3_t a, vec3_t b) {
    return { std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z) };
}

vec3_t greatest(vec3_t a, vec3_t b) {
    return { std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z) };
}

/// What a mover can touch in its move, in its sphere space.
struct nearby_t {
    std::vector<triangle_t> triangles;
    std::vector<ellipsoid_t> ellipsoids;
};

/// Adds a world triangle, squashed into sphere space, turning it if its target mirrors.
void add_triangle(nearby_t& nearby, std::array<vec3_t, 3> corners, float squash,
                  std::uint32_t target, bool mirrored, triangle_origin_t origin, bool from_mesh) {
    for (vec3_t& corner : corners) {
        corner.y *= squash;
    }
    triangle_t triangle;
    if (!make_triangle(corners, mirrored, triangle)) {
        return;
    }
    triangle.target = target;
    triangle.origin = origin;
    triangle.from_mesh = from_mesh;
    nearby.triangles.push_back(triangle);
}

/// Everything a mover could touch within `reach` of its start.
nearby_t gather(const snapshot_t& snapshot, const mover_t& mover, float reach) {
    nearby_t nearby;
    const float squash = mover.horizontal / mover.vertical;
    const vec3_t lift(0.0F, mover.step, 0.0F);
    const vec3_t low = least(mover.from, mover.to) - vec3_t(reach, reach, reach);
    const vec3_t high = greatest(mover.from, mover.to) + vec3_t(reach, reach, reach) + lift;
    for (std::uint32_t t = 0; t < snapshot.targets.size(); ++t) {
        const target_t& target = snapshot.targets[t];
        if (target.entity == mover.entity ||
            response_of(*snapshot.world, mover.group, target.group) == FBE_RESPONSE_NONE ||
            target.max.x < low.x || target.min.x > high.x || target.max.y < low.y ||
            target.min.y > high.y || target.max.z < low.z || target.min.z > high.z) {
            continue;
        }
        if (target.shape == FBE_COLLISION_ELLIPSOID) {
            const vec3_t center(target.center.x, target.center.y * squash, target.center.z);
            nearby.ellipsoids.push_back({
                .center = center,
                .radii = vec3_t(target.horizontal + mover.horizontal,
                                (target.vertical * squash) + mover.horizontal,
                                target.horizontal + mover.horizontal),
                .target = t,
            });
        } else if (target.shape == terrain_shape) {
            // Its squares under the move's box, found in its own space.
            mat4_t into;
            if (!inverse_affine(target.matrix, into)) {
                continue;
            }
            vec3_t local_min = transform_point(into, low);
            vec3_t local_max = local_min;
            for (unsigned corner = 1; corner < 8; ++corner) {
                const vec3_t p = transform_point(into, box_corner(low, high, corner));
                local_min = least(local_min, p);
                local_max = greatest(local_max, p);
            }
            std::vector<std::array<vec3_t, 3>> under;
            const terrain_t& terrain = scene().terrains[scene().entities.component[target.entity]];
            terrain_under(terrain, local_min, local_max, under);
            for (const auto& corners : under) {
                add_triangle(nearby,
                             {
                                 transform_point(target.matrix, corners[0]),
                                 transform_point(target.matrix, corners[1]),
                                 transform_point(target.matrix, corners[2]),
                             },
                             squash, t, target.mirrored, triangle_origin_t(), false);
            }
        } else if (target.shape == plane_shape) {
            std::array<std::array<vec3_t, 3>, 2> under;
            if (plane_under(target.matrix, low, high, under)) {
                for (const auto& corners : under) {
                    add_triangle(nearby, corners, squash, t, target.mirrored, triangle_origin_t(),
                                 false);
                }
            }
        } else if (target.shape == FBE_COLLISION_BOX) {
            const collider_t& collider = scene().entities.colliders[target.entity];
            for (const auto& corners : box_triangles) {
                const vec3_t a = transform_point(
                    target.matrix, box_corner(collider.box_min, collider.box_max, corners[0]));
                const vec3_t b = transform_point(
                    target.matrix, box_corner(collider.box_min, collider.box_max, corners[1]));
                const vec3_t c = transform_point(
                    target.matrix, box_corner(collider.box_min, collider.box_max, corners[2]));
                add_triangle(nearby, { a, b, c }, squash, t, target.mirrored, triangle_origin_t(),
                             false);
            }
        } else {
            mat4_t into;
            if (!inverse_affine(target.matrix, into)) {
                continue;
            }
            // The sweep's box in the mesh's own space, where its tree is.
            vec3_t local_min = transform_point(into, low);
            vec3_t local_max = local_min;
            for (unsigned corner = 1; corner < 8; ++corner) {
                const vec3_t p = transform_point(into, box_corner(low, high, corner));
                local_min = least(local_min, p);
                local_max = greatest(local_max, p);
            }
            const bvh_t& tree = scene().geometries[target.geometry].tree;
            const std::size_t first = nearby.triangles.size();
            tree.overlapping(local_min, local_max, [&](std::uint32_t triangle) {
                const vec3_t* corners = tree.corners(triangle);
                const vec3_t a = transform_point(target.matrix, corners[0]);
                const vec3_t b = transform_point(target.matrix, corners[1]);
                const vec3_t c = transform_point(target.matrix, corners[2]);
                add_triangle(nearby, { a, b, c }, squash, t, target.mirrored, tree.origin(triangle),
                             true);
            });
            // In the order of the mesh's own triangles, not its tree's leaves, so a refitted tree
            // and one built again give the same touches, ties too; targets come in their order.
            std::sort(nearby.triangles.begin() + static_cast<std::ptrdiff_t>(first),
                      nearby.triangles.end(), [](const triangle_t& a, const triangle_t& b) {
                          return a.origin.surface != b.origin.surface
                                     ? a.origin.surface < b.origin.surface
                                     : a.origin.triangle < b.origin.triangle;
                      });
        }
    }
    return nearby;
}

/// Whether a touch, by its normal in sphere space, is on ground the mover climbs: back in the
/// world, within its max slope of up.
bool on_ground(const mover_t& mover, vec3_t normal) {
    const float squash = mover.horizontal / mover.vertical;
    return normalize(vec3_t(normal.x, normal.y * squash, normal.z)).y >= mover.ground;
}

/// The first touch of a sphere of radius `r` going from `c` along `v`, if it touches anything near.
bool first_touch(const nearby_t& nearby, vec3_t c, vec3_t v, float r, hit_t& hit) {
    bool found = false;
    for (const triangle_t& triangle : nearby.triangles) {
        sweep_triangle(c, v, r, triangle, hit, found);
    }
    for (const ellipsoid_t& ellipsoid : nearby.ellipsoids) {
        sweep_ellipsoid(c, v, r, ellipsoid, hit, found);
    }
    return found;
}

/// Where a mover at `at`, in what it touched at `touched`, goes pushed out of it to `wanted`:
/// there, unless something else is in the way, when it stops where that is; but between two that
/// face each other, its middle goes halfway across the gap between them along the push, sharing the
/// overlap, so squeezed below its diameter it sits in the gap's middle, never with its middle in
/// either.
vec3_t pushed_out(const nearby_t& nearby, vec3_t at, vec3_t wanted, vec3_t touched, float r) {
    const vec3_t push = wanted - at;
    const float size = length(push);
    hit_t blocking;
    if (size <= 0.0F || !first_touch(nearby, at, push, r, blocking)) {
        return wanted;
    }
    const vec3_t direction = push * (1.0F / size);
    const float facing = dot(blocking.normal, direction);
    if (facing > -0.5F) {
        return at + push * blocking.time;
    }
    const float behind = length(at - touched);
    // Two facing each other: halfway across the gap between them along the push, measured to the
    // second's plane and back to what it touched.
    const float ahead = std::fmax(dot(blocking.point - at, blocking.normal) / facing, 0.0F);
    return at + direction * std::clamp((ahead - behind) * 0.5F, -behind, size);
}

/// After a move from `from` used up its iterations on its touches, the mover pushed out of every
/// plane it still touches at once, a few rounds of moving out of each in turn, if that leaves it
/// less in them than it is, and swept there as a push is; but back at `from` if it's still further
/// in them than it was there. Out of what's steeper than ground, no higher than `ceiling`.
vec3_t settle(const mover_t& mover, const nearby_t& nearby, vec3_t from, vec3_t base,
              const std::vector<hit_t>& hits, float r, float skin, float ceiling) {
    // Only what it's still at: a plane it slid off, over an edge, isn't one to push out of.
    std::vector<hit_t> touched;
    for (const hit_t& hit : hits) {
        if (length(base - hit.point) <= r + (2.0F * skin) + tiny) {
            touched.push_back(hit);
        }
    }
    const auto deepest = [&](vec3_t at) {
        float most = 0.0F;
        for (const hit_t& hit : touched) {
            most = std::fmax(most, r + skin - dot(at - hit.point, hit.normal));
        }
        return most;
    };
    vec3_t settled = base;
    const float highest = std::fmax(ceiling, base.y);
    for (int round = 0; round < 4; ++round) {
        for (const hit_t& hit : touched) {
            const float apart = dot(settled - hit.point, hit.normal);
            if (apart >= r + skin) {
                continue;
            }
            vec3_t out = hit.normal * (r + skin - apart);
            if (!on_ground(mover, hit.normal)) {
                out.y = std::fmin(out.y, std::fmax(highest - settled.y, 0.0F));
            }
            settled = settled + out;
        }
    }
    vec3_t ended = base;
    if (deepest(settled) < deepest(base)) {
        hit_t blocking;
        const vec3_t push = settled - base;
        ended =
            first_touch(nearby, base, push, r, blocking) ? base + push * blocking.time : settled;
    }
    // Further in than it started, as along a crease into a wedge that narrows ahead, with no way
    // out: it doesn't go.
    return deepest(ended) > deepest(from) + tiny ? from : ended;
}

/// A move from `base` along `v` in sphere space, touching and sliding as the rules say, at most
/// `iterations` times, never sliding higher than `ceiling` but up ground; gives where it ended and
/// adds what it touched.
vec3_t slide(const snapshot_t& snapshot, const mover_t& mover, const nearby_t& nearby, vec3_t base,
             vec3_t v, float ceiling, std::vector<hit_t>& hits) {
    const float r = mover.horizontal;
    const float skin = snapshot.world->collision_skin;
    const vec3_t from = base;
    const vec3_t original = v;
    vec3_t previous_normal;
    bool has_previous = false;
    const std::size_t first = hits.size();
    // Whether it didn't move, or went away from a face it was in, as one something closed on is.
    const bool still = dot(v, v) < tiny * tiny;
    bool left_a_face = false;
    std::int32_t i = 0;
    for (; i < snapshot.world->collision_iterations; ++i) {
        if (dot(v, v) < tiny * tiny) {
            break;
        }
        hit_t hit;
        bool found = false;
        for (const triangle_t& triangle : nearby.triangles) {
            sweep_triangle(base, v, r, triangle, hit, found, &left_a_face);
        }
        for (const ellipsoid_t& ellipsoid : nearby.ellipsoids) {
            sweep_ellipsoid(base, v, r, ellipsoid, hit, found);
        }
        if (!found) {
            base = base + v;
            break;
        }
        hits.push_back(hit);
        const bool ground = on_ground(mover, hit.normal);
        const vec3_t at = base + v * hit.time;
        const vec3_t out = normalize(at - hit.point);
        const vec3_t away = dot(out, out) > 0.0F ? out : hit.normal;
        // What's left of the move, from where it ends up, toward where it was going.
        vec3_t remaining = v * (1.0F - hit.time);
        if (hit.time <= 0.0F) {
            // Touching it from the start, it may be in it, as when it moved into the mover: pushed
            // out skin away only as far as nothing else is, so a mover squeezed between two stays
            // where it is, never pushed into either.
            base = pushed_out(nearby, at, hit.point + away * (r + skin), hit.point, r);
        } else {
            // Back the way it came until it's skin away from what it touched, never further back
            // than where this part of the move began: where it's been is clear, while a push off
            // what it touched could go into something it's leaving. What it went back is left to
            // go, along what it touched.
            const float travelled = length(v) * hit.time;
            const vec3_t heading = v * (1.0F / length(v));
            const float closing = -dot(heading, away);
            const float back = closing > tiny ? std::fmin(skin / closing, travelled) : 0.0F;
            base = at - heading * back;
            remaining = remaining + heading * back;
            if (!ground) {
                // Nor does it end higher than its move and step allow.
                base.y = std::fmin(base.y, std::fmax(ceiling, at.y));
            }
        }
        const std::int32_t response =
            response_of(*snapshot.world, mover.group, snapshot.targets[hit.target].group);
        if (response == FBE_RESPONSE_STOP) {
            break;
        }
        vec3_t next = remaining - hit.normal * dot(remaining, hit.normal);
        if (has_previous && dot(next, previous_normal) < 0.0F) {
            // Between two surfaces, along the crease where they meet, or not at all.
            const vec3_t crease = cross(previous_normal, hit.normal);
            const float size = length(crease);
            next = size > tiny ? crease * (dot(next, crease) / (size * size)) : vec3_t();
        }
        if (response == FBE_RESPONSE_SLIDE_HORIZONTAL) {
            const float across =
                std::sqrt((remaining.x * remaining.x) + (remaining.z * remaining.z));
            const float size = length(next);
            if (size > across) {
                next = size > 0.0F ? next * (across / size) : vec3_t();
            }
        }
        // Up ground, a slide raises it as far as the slope goes; up anything steeper, edges and
        // corners too, no higher than its move and its step allow, so a tall mover sliding up an
        // edge isn't thrown up by its slide.
        if (!ground) {
            next.y = std::fmin(next.y, std::fmax(ceiling - base.y, 0.0F));
        }
        if (dot(next, original) <= 0.0F) {
            break;
        }
        v = next;
        previous_normal = hit.normal;
        has_previous = true;
    }
    if (i == snapshot.world->collision_iterations && hits.size() > first) {
        const std::vector<hit_t> touched(hits.begin() + static_cast<std::ptrdiff_t>(first),
                                         hits.end());
        base = settle(mover, nearby, from, base, touched, r, skin, ceiling);
    }
    // Still in a face it moved away from, as when something closed on it from behind as it went
    // the other way: pushed out of the deepest, as from a touch at its start. Only a mover that
    // didn't move, or left a face it was in, can be.
    const triangle_t* deepest = nullptr;
    vec3_t foot;
    float least = r;
    if (still || left_a_face) {
        for (const triangle_t& triangle : nearby.triangles) {
            vec3_t under;
            float distance = 0.0F;
            if (in_face(base, r, triangle, under, distance) && distance < least - tiny) {
                deepest = &triangle;
                foot = under;
                least = distance;
            }
        }
    }
    if (deepest != nullptr) {
        vec3_t wanted = foot + deepest->normal * (r + skin);
        if (!on_ground(mover, deepest->normal)) {
            wanted.y = std::fmin(wanted.y, std::fmax(ceiling, base.y));
        }
        base = pushed_out(nearby, base, wanted, foot, r);
    }
    return base;
}

/// How far a move went across, in X and Z.
float across(vec3_t from, vec3_t to) {
    return std::sqrt(((to.x - from.x) * (to.x - from.x)) + ((to.z - from.z) * (to.z - from.z)));
}

result_t move(const snapshot_t& snapshot, const mover_t& mover) {
    const float squash = mover.horizontal / mover.vertical;
    const auto into = [squash](vec3_t p) { return vec3_t(p.x, p.y * squash, p.z); };
    const auto out = [squash](vec3_t p) { return vec3_t(p.x, p.y / squash, p.z); };
    const float reach = length(mover.to - mover.from) +
                        std::fmax(mover.horizontal, mover.vertical) +
                        snapshot.world->collision_skin;
    const nearby_t nearby = gather(snapshot, mover, reach);
    const vec3_t start = into(mover.from);
    const vec3_t wanted = into(mover.to) - start;
    // No slide along what's steeper than ground takes a mover higher than its move rises, and its
    // step height.
    const float step = mover.step * squash;
    const float ceiling = start.y + std::fmax(wanted.y, 0.0F) + step;
    std::vector<hit_t> hits;
    vec3_t end = slide(snapshot, mover, nearby, start, wanted, ceiling, hits);

    // Blocked going across: up the step, across, and back down, if that lands on ground no
    // higher than the step, within its max slope, and gets further.
    if (step > 0.0F && !hits.empty() && across(start, end) + tiny < across(start, start + wanted)) {
        std::vector<hit_t> stepped;
        const vec3_t up =
            slide(snapshot, mover, nearby, start, { 0.0F, step, 0.0F }, ceiling, stepped);
        const vec3_t over =
            slide(snapshot, mover, nearby, up, { wanted.x, 0.0F, wanted.z }, ceiling, stepped);
        const std::size_t before_down = stepped.size();
        const vec3_t down = slide(snapshot, mover, nearby, over,
                                  { 0.0F, -(over.y - start.y) + std::fmin(wanted.y, 0.0F), 0.0F },
                                  ceiling, stepped);
        bool landed = false;
        for (std::size_t i = before_down; i < stepped.size(); ++i) {
            landed = landed || on_ground(mover, stepped[i].normal);
        }
        // Within the skin of the step's height: a step as high as it is stepped onto.
        const float skin = snapshot.world->collision_skin;
        if (landed && down.y <= ceiling + skin + tiny &&
            across(start, down) > across(start, end) + tiny) {
            end = down;
            hits = std::move(stepped);
        }
    }

    result_t result;
    result.end = out(end);
    for (const hit_t& hit : hits) {
        const target_t& target = snapshot.targets[hit.target];
        contact_t contact;
        contact.entity = scene().entities.slots.handle(target.entity);
        contact.point = out(hit.point);
        contact.normal = normalize(vec3_t(hit.normal.x, hit.normal.y * squash, hit.normal.z));
        contact.time = hit.time;
        if (hit.from_mesh) {
            contact.geometry = scene().geometry_slots.handle(target.geometry);
            contact.surface = static_cast<std::int32_t>(hit.origin.surface);
            contact.triangle = static_cast<std::int32_t>(hit.origin.triangle);
        }
        result.contacts.push_back(contact);
        result.touched.push_back(hit.target);
    }
    return result;
}

/// Whether an entity's group moves in its world: a source in a pair that collides.
bool moves(const world_t& world, std::int32_t group) {
    if (group == 0) {
        return false;
    }
    return std::ranges::any_of(world.collisions, [group](const auto& pair) {
        return static_cast<std::int32_t>(pair.first >> 16U) == group &&
               pair.second != FBE_RESPONSE_NONE;
    });
}

void collide(std::uint32_t world_slot) {
    scene_t& s = scene();
    entities_t& entities = s.entities;
    const world_t& world = s.worlds[world_slot];
    snapshot_t snapshot;
    snapshot.world = &world;
    std::vector<mover_t> movers;
    for (std::uint32_t entity = 0; entity < entities.slots.size(); ++entity) {
        if (!entities.slots.find(entities.slots.handle(entity)) ||
            entities.world[entity] != world_slot) {
            continue;
        }
        collider_t& collider = entities.colliders[entity];
        collider.contacts.clear();
        if (collider.group == 0 || !showing(entity)) {
            continue;
        }
        update_world(entity);
        const vec3_t position = entities.world_matrix[entity].axis(3);
        const bool moving = moves(world, collider.group);
        if (moving) {
            if (!collider.has_previous) {
                collider.previous = position;
                collider.has_previous = true;
            }
            movers.push_back({
                .entity = entity,
                .group = collider.group,
                .from = collider.previous,
                .to = position,
                .horizontal = collider.horizontal,
                .vertical = collider.vertical,
                .step = collider.step_height,
                .ground = std::cos(collider.max_slope),
            });
        }

        target_t target;
        target.entity = entity;
        target.group = collider.group;
        target.matrix = entities.world_matrix[entity];
        target.mirrored = mirrors(target.matrix);
        target.horizontal = collider.horizontal;
        target.vertical = collider.vertical;
        std::int32_t shape = collider.shape;
        const bool mesh = entities.kind[entity] == FBE_ENTITY_MESH;
        const std::uint32_t geometry = mesh ? s.meshes[entities.component[entity]].geometry : none;
        if (entities.kind[entity] == FBE_ENTITY_PLANE &&
            (shape == FBE_COLLISION_AUTO || shape == FBE_COLLISION_MESH)) {
            shape = plane_shape;
        } else if (entities.kind[entity] == FBE_ENTITY_TERRAIN &&
                   (shape == FBE_COLLISION_AUTO || shape == FBE_COLLISION_MESH)) {
            shape = terrain_shape;
        } else if (shape == FBE_COLLISION_AUTO) {
            shape = mesh && geometry != none ? FBE_COLLISION_MESH : FBE_COLLISION_ELLIPSOID;
        }
        if (shape == FBE_COLLISION_MESH && geometry == none) {
            continue;
        }
        target.shape = shape;
        if (shape == FBE_COLLISION_ELLIPSOID) {
            // A mover is touched where its last move ended.
            target.center = moving ? collider.previous : position;
            const vec3_t radii(collider.horizontal, collider.vertical, collider.horizontal);
            target.min = target.center - radii;
            target.max = target.center + radii;
        } else if (shape == terrain_shape) {
            vec3_t min;
            vec3_t max;
            terrain_bounds(s.terrains[entities.component[entity]], min, max);
            world_box(entity, min, max, target.min, target.max);
        } else if (shape == plane_shape) {
            // Everywhere: every move's box is over some of it.
            target.min = vec3_t(-INFINITY, -INFINITY, -INFINITY);
            target.max = vec3_t(INFINITY, INFINITY, INFINITY);
        } else if (shape == FBE_COLLISION_BOX) {
            world_box(entity, collider.box_min, collider.box_max, target.min, target.max);
        } else {
            target.geometry = geometry;
            vec3_t min;
            vec3_t max;
            geometry_bounds(geometry, min, max);
            world_box(entity, min, max, target.min, target.max);
            // Built now, since the moves only read it.
            geometry_tree(geometry);
        }
        snapshot.targets.push_back(target);
    }

    std::vector<result_t> results(movers.size());
    for_each_move(movers.size(), [&](std::size_t i) { results[i] = move(snapshot, movers[i]); });

    for (std::size_t i = 0; i < movers.size(); ++i) {
        const mover_t& mover = movers[i];
        collider_t& collider = entities.colliders[mover.entity];
        const vec3_t end = results[i].end;
        if (end.x != mover.to.x || end.y != mover.to.y || end.z != mover.to.z) {
            set_world_position(mover.entity, end);
        }
        collider.previous = end;
        for (std::size_t c = 0; c < results[i].contacts.size(); ++c) {
            const contact_t& contact = results[i].contacts[c];
            collider.contacts.push_back(contact);
            // What it touched has the contact too, with the mover as its other.
            contact_t theirs = contact;
            theirs.entity = entities.slots.handle(mover.entity);
            theirs.geometry = 0;
            theirs.surface = -1;
            theirs.triangle = -1;
            const std::uint32_t touched = snapshot.targets[results[i].touched[c]].entity;
            entities.colliders[touched].contacts.push_back(theirs);
        }
    }
}

collider_t* collider_of(std::uint64_t entity) {
    const std::uint32_t slot = entity_slot(entity);
    return slot != none ? &scene().entities.colliders[slot] : nullptr;
}

} // namespace

} // namespace fbe::scene

using fbe::scene::collider_of;
using fbe::scene::collider_t;
using fbe::scene::none;
using fbe::scene::scene;
using fbe::scene::world_slot;

extern "C" {

FBE_API bool fbe_entity_set_collision_group(uint64_t entity, int32_t group) {
    collider_t* found = collider_of(entity);
    if (found == nullptr || !fbe::scene::valid_group(group)) {
        return false;
    }
    found->group = group;
    found->has_previous = false;
    return true;
}

FBE_API int32_t fbe_entity_collision_group(uint64_t entity) {
    const collider_t* found = collider_of(entity);
    return found != nullptr ? found->group : -1;
}

FBE_API bool fbe_entity_set_collision_radius(uint64_t entity, float horizontal, float vertical) {
    collider_t* found = collider_of(entity);
    if (found == nullptr) {
        return false;
    }
    if (!(horizontal > 0.0F && vertical > 0.0F && std::isfinite(horizontal) &&
          std::isfinite(vertical))) {
        return fbe::fail(
            std::format("collision radii are above 0, not {} and {}", horizontal, vertical));
    }
    found->horizontal = horizontal;
    found->vertical = vertical;
    fbe::scene::pickable_moved(fbe::scene::entity_slot(entity));
    return true;
}

FBE_API bool fbe_entity_collision_radius(uint64_t entity, fbe_vec2_t* radius) {
    const collider_t* found = collider_of(entity);
    if (found == nullptr) {
        return false;
    }
    *radius = { .x = found->horizontal, .y = found->vertical };
    return true;
}

FBE_API bool fbe_entity_set_collision_shape(uint64_t entity, int32_t shape) {
    collider_t* found = collider_of(entity);
    if (found == nullptr) {
        return false;
    }
    if (shape < FBE_COLLISION_AUTO || shape > FBE_COLLISION_MESH) {
        return fbe::fail(std::format("{} isn't a CollisionShape", shape));
    }
    found->shape = shape;
    return true;
}

FBE_API int32_t fbe_entity_collision_shape(uint64_t entity) {
    const collider_t* found = collider_of(entity);
    return found != nullptr ? found->shape : -1;
}

FBE_API bool fbe_entity_set_collision_box(uint64_t entity, const fbe_aabb_t* box) {
    collider_t* found = collider_of(entity);
    if (found == nullptr) {
        return false;
    }
    const bool ordered =
        box->min.x <= box->max.x && box->min.y <= box->max.y && box->min.z <= box->max.z;
    if (!ordered) {
        return fbe::fail(std::format("a collision box has its least corner at or below its "
                                     "greatest, not ({}, {}, {}) to ({}, {}, {})",
                                     box->min.x, box->min.y, box->min.z, box->max.x, box->max.y,
                                     box->max.z));
    }
    found->box_min = { box->min.x, box->min.y, box->min.z };
    found->box_max = { box->max.x, box->max.y, box->max.z };
    fbe::scene::pickable_moved(fbe::scene::entity_slot(entity));
    return true;
}

FBE_API bool fbe_entity_collision_box(uint64_t entity, fbe_aabb_t* box) {
    const collider_t* found = collider_of(entity);
    if (found == nullptr) {
        return false;
    }
    *box = {
        .min = { .x = found->box_min.x, .y = found->box_min.y, .z = found->box_min.z },
        .max = { .x = found->box_max.x, .y = found->box_max.y, .z = found->box_max.z },
    };
    return true;
}

FBE_API bool fbe_entity_set_step_height(uint64_t entity, float height) {
    collider_t* found = collider_of(entity);
    if (found == nullptr) {
        return false;
    }
    if (std::isnan(height) || height < 0.0F || !std::isfinite(height)) {
        return fbe::fail(std::format("a step height is 0 or more, not {}", height));
    }
    found->step_height = height;
    return true;
}

FBE_API float fbe_entity_step_height(uint64_t entity) {
    const collider_t* found = collider_of(entity);
    return found != nullptr ? found->step_height : 0.0F;
}

FBE_API bool fbe_entity_set_max_slope(uint64_t entity, float radians) {
    collider_t* found = collider_of(entity);
    if (found == nullptr) {
        return false;
    }
    if (std::isnan(radians) || radians < 0.0F || radians > std::numbers::pi_v<float> / 2.0F) {
        return fbe::fail(std::format("a max slope is 0 to Pi / 2, not {}", radians));
    }
    found->max_slope = radians;
    return true;
}

FBE_API float fbe_entity_max_slope(uint64_t entity) {
    const collider_t* found = collider_of(entity);
    return found != nullptr ? found->max_slope : 0.0F;
}

FBE_API bool fbe_entity_reset_collisions(uint64_t entity) {
    collider_t* found = collider_of(entity);
    if (found == nullptr) {
        return false;
    }
    found->has_previous = false;
    found->contacts.clear();
    return true;
}

FBE_API int32_t fbe_entity_collisions(uint64_t entity, fbe_collision_t* collisions,
                                      int32_t capacity) {
    const collider_t* found = collider_of(entity);
    if (found == nullptr) {
        return -1;
    }
    const std::size_t count =
        std::min(found->contacts.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    for (std::size_t i = 0; i < count; ++i) {
        const fbe::scene::contact_t& contact = found->contacts[i];
        collisions[i] = {
            .entity = contact.entity,
            .point = { .x = contact.point.x, .y = contact.point.y, .z = contact.point.z },
            .normal = { .x = contact.normal.x, .y = contact.normal.y, .z = contact.normal.z },
            .time = contact.time,
            .geometry = contact.geometry,
            .surface = contact.surface,
            .triangle = contact.triangle,
        };
    }
    return static_cast<std::int32_t>(found->contacts.size());
}

FBE_API bool fbe_world_set_collision(uint64_t world, int32_t source, int32_t target,
                                     int32_t response) {
    const std::uint32_t slot = world_slot(world);
    if (slot == none || !fbe::scene::valid_group(source) || !fbe::scene::valid_group(target)) {
        return false;
    }
    if (source == 0 || target == 0) {
        return fbe::fail("group 0 is no group, which collides with nothing");
    }
    if (response < FBE_RESPONSE_NONE || response > FBE_RESPONSE_SLIDE_HORIZONTAL) {
        return fbe::fail(std::format("{} isn't a CollisionResponse", response));
    }
    const std::uint32_t pair = fbe::scene::pair_of(source, target);
    auto& rules = scene().worlds[slot].collisions;
    std::erase_if(rules, [pair](const auto& rule) { return rule.first == pair; });
    if (response != FBE_RESPONSE_NONE) {
        rules.emplace_back(pair, response);
    }
    return true;
}

FBE_API int32_t fbe_world_collision(uint64_t world, int32_t source, int32_t target) {
    const std::uint32_t slot = world_slot(world);
    if (slot == none || !fbe::scene::valid_group(source) || !fbe::scene::valid_group(target)) {
        return -1;
    }
    return fbe::scene::response_of(scene().worlds[slot], source, target);
}

FBE_API bool fbe_world_clear_collisions(uint64_t world) {
    const std::uint32_t slot = world_slot(world);
    if (slot == none) {
        return false;
    }
    scene().worlds[slot].collisions.clear();
    return true;
}

FBE_API bool fbe_world_set_collision_iterations(uint64_t world, int32_t iterations) {
    const std::uint32_t slot = world_slot(world);
    if (slot == none) {
        return false;
    }
    if (iterations < 1 || iterations > fbe::scene::most_iterations) {
        return fbe::fail(std::format("a move touches 1 to {} times, not {}",
                                     fbe::scene::most_iterations, iterations));
    }
    scene().worlds[slot].collision_iterations = iterations;
    return true;
}

FBE_API int32_t fbe_world_collision_iterations(uint64_t world) {
    const std::uint32_t slot = world_slot(world);
    return slot != none ? scene().worlds[slot].collision_iterations : -1;
}

FBE_API bool fbe_world_set_collision_skin(uint64_t world, float skin) {
    const std::uint32_t slot = world_slot(world);
    if (slot == none) {
        return false;
    }
    if (std::isnan(skin) || skin < 0.0F || skin > fbe::scene::most_skin) {
        return fbe::fail(
            std::format("a collision skin is 0 to {}, not {}", fbe::scene::most_skin, skin));
    }
    scene().worlds[slot].collision_skin = skin;
    return true;
}

FBE_API float fbe_world_collision_skin(uint64_t world) {
    const std::uint32_t slot = world_slot(world);
    return slot != none ? scene().worlds[slot].collision_skin : 0.0F;
}

FBE_API bool fbe_world_collide(uint64_t world) {
    const std::uint32_t slot = world_slot(world);
    if (slot == none) {
        return false;
    }
    fbe::scene::collide(slot);
    return true;
}

} // extern "C"
