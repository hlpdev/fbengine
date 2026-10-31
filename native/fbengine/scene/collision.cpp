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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <vector>

namespace fbe::scene {

namespace {

constexpr float tiny = 1e-6F;
/// A surface facing up at least this much is ground a mover can stand on after stepping up.
constexpr float walkable = 0.7F;
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

/// A triangle in a mover's sphere space, front faces counterclockwise.
struct triangle_t {
    std::array<vec3_t, 3> corners;
    vec3_t normal;
    std::uint32_t target = 0;
    triangle_origin_t origin;
    bool from_mesh = false;
};

/// An ellipsoid in a mover's sphere space, grown by the mover's radius, so the mover is a point to
/// it.
struct ellipsoid_t {
    vec3_t center;
    vec3_t radii;
    std::uint32_t target = 0;
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
};

/// The nearest touch along a sweep, in sphere space.
struct hit_t {
    float time = 1.0F;
    vec3_t point;
    vec3_t normal;
    std::uint32_t target = 0;
    triangle_origin_t origin;
    bool from_mesh = false;
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

vec3_t box_corner(vec3_t min, vec3_t max, unsigned corner) {
    return { (corner & 1U) != 0 ? max.x : min.x, (corner & 2U) != 0 ? max.y : min.y,
             (corner & 4U) != 0 ? max.z : min.z };
}

/// The smallest root of a t^2 + b t + c from 0 up to `below`, if there's one.
bool lowest_root(float a, float b, float c, float below, float& root) {
    const float discriminant = (b * b) - (4.0F * a * c);
    if (discriminant < 0.0F || std::fabs(a) < 1e-12F) {
        return false;
    }
    const float square = std::sqrt(discriminant);
    float first = (-b - square) / (2.0F * a);
    float second = (-b + square) / (2.0F * a);
    if (first > second) {
        std::swap(first, second);
    }
    if (first >= 0.0F && first < below) {
        root = first;
        return true;
    }
    if (second >= 0.0F && second < below) {
        root = second;
        return true;
    }
    return false;
}

/// Whether a point on a triangle's plane is inside it.
bool inside(const triangle_t& triangle, vec3_t p) {
    const auto& c = triangle.corners;
    return dot(cross(c[1] - c[0], p - c[0]), triangle.normal) >= 0.0F &&
           dot(cross(c[2] - c[1], p - c[1]), triangle.normal) >= 0.0F &&
           dot(cross(c[0] - c[2], p - c[2]), triangle.normal) >= 0.0F;
}

/// Sweeps a sphere of radius `r` from `c` along `v` against a triangle's front, and keeps the hit
/// if it's sooner than `hit`'s.
void sweep_triangle(vec3_t c, vec3_t v, float r, const triangle_t& triangle, hit_t& hit,
                    bool& found) {
    const vec3_t n = triangle.normal;
    const float distance = dot(n, c - triangle.corners[0]);
    // A mover whose middle is behind a triangle passes out through it, and one going away from
    // its front, further than its radius, doesn't touch it.
    if (distance < 0.0F) {
        return;
    }
    const float approach = dot(n, v);
    float first = 0.0F;
    bool embedded = false;
    // Within its radius of the plane and going away, it can't touch the face, but it can still
    // go sideways onto an edge or a corner.
    bool leaving = false;
    if (std::fabs(approach) < tiny) {
        if (distance >= r) {
            return;
        }
        embedded = true;
    } else if (approach > 0.0F) {
        if (distance >= r) {
            return;
        }
        embedded = true;
        leaving = true;
    } else {
        first = (r - distance) / approach;
        const float last = (-r - distance) / approach;
        if (first > hit.time || last < 0.0F) {
            return;
        }
        first = std::fmax(first, 0.0F);
    }
    if (distance < r && !leaving) {
        // Already touching its plane: its face, if the middle is over it, or else its edges.
        const vec3_t under = c - n * distance;
        if (inside(triangle, under)) {
            if (0.0F <= hit.time) {
                hit = {
                    .time = 0.0F,
                    .point = under,
                    .normal = n,
                    .target = triangle.target,
                    .origin = triangle.origin,
                    .from_mesh = triangle.from_mesh,
                };
                found = true;
            }
            return;
        }
    } else if (!embedded) {
        const vec3_t on_plane = c - n * r + v * first;
        if (inside(triangle, on_plane)) {
            if (first < hit.time || !found) {
                hit = {
                    .time = first,
                    .point = on_plane,
                    .normal = n,
                    .target = triangle.target,
                    .origin = triangle.origin,
                    .from_mesh = triangle.from_mesh,
                };
                found = true;
            }
            return;
        }
    }
    // Its corners, then its edges.
    const float speed = dot(v, v);
    float best = found ? hit.time : 1.0F;
    bool touched = false;
    vec3_t point;
    for (const vec3_t& corner : triangle.corners) {
        const float apart = dot(corner - c, corner - c) - (r * r);
        const float closing = 2.0F * dot(v, c - corner);
        // Already over a corner, it touches it now if it's going further in, else not at all: a
        // root then would be where it leaves, which can't stop it.
        if (apart < 0.0F) {
            if (closing < 0.0F) {
                best = 0.0F;
                point = corner;
                touched = true;
            }
            continue;
        }
        float root = 0.0F;
        if (lowest_root(speed, closing, apart, best, root)) {
            best = root;
            point = corner;
            touched = true;
        }
    }
    for (std::size_t k = 0; k < 3; ++k) {
        const vec3_t start = triangle.corners[k];
        const vec3_t edge = triangle.corners[(k + 1) % 3] - start;
        const vec3_t to_start = start - c;
        const float length = dot(edge, edge);
        const float along_v = dot(edge, v);
        const float along_start = dot(edge, to_start);
        const float a = (length * -speed) + (along_v * along_v);
        const float b = (length * 2.0F * dot(v, to_start)) - (2.0F * along_v * along_start);
        const float cc =
            (length * ((r * r) - dot(to_start, to_start))) + (along_start * along_start);
        // Already over the edge's line: where it's over the edge itself, it touches it now if
        // it's going further in, else not at all; past the edge's ends, its corners say.
        if (cc > 0.0F) {
            const float fraction = -along_start / length;
            if (fraction >= 0.0F && fraction <= 1.0F && b > 0.0F) {
                best = 0.0F;
                point = start + edge * fraction;
                touched = true;
            }
            continue;
        }
        float root = 0.0F;
        if (lowest_root(a, b, cc, best, root)) {
            const float fraction = ((along_v * root) - along_start) / length;
            if (fraction >= 0.0F && fraction <= 1.0F) {
                best = root;
                point = start + edge * fraction;
                touched = true;
            }
        }
    }
    if (touched) {
        const vec3_t at = c + v * best;
        hit = {
            .time = best,
            .point = point,
            .normal = normalize(at - point),
            .target = triangle.target,
            .origin = triangle.origin,
            .from_mesh = triangle.from_mesh,
        };
        found = true;
    }
}

/// Sweeps a point from `c` along `v` against an ellipsoid already grown by the mover's radius, and
/// keeps the hit if it's sooner than `hit`'s.
void sweep_ellipsoid(vec3_t c, vec3_t v, float r, const ellipsoid_t& ellipsoid, hit_t& hit,
                     bool& found) {
    const vec3_t inverse(1.0F / ellipsoid.radii.x, 1.0F / ellipsoid.radii.y,
                         1.0F / ellipsoid.radii.z);
    const vec3_t offset = c - ellipsoid.center;
    const vec3_t q = offset * inverse;
    const vec3_t d = v * inverse;
    const float start = dot(q, q) - 1.0F;
    float time = 0.0F;
    if (start < 0.0F) {
        // Inside already: it touches only going further in.
        if (dot(q, d) >= 0.0F) {
            return;
        }
    } else if (!lowest_root(dot(d, d), 2.0F * dot(q, d), start, found ? hit.time : 1.0F + tiny,
                            time) ||
               time > 1.0F) {
        return;
    }
    const vec3_t at = c + v * time;
    const vec3_t from_center = at - ellipsoid.center;
    const vec3_t normal = normalize(from_center * inverse * inverse);
    hit = {
        .time = time,
        .point = at - normal * r,
        .normal = normal,
        .target = ellipsoid.target,
        .origin = triangle_origin_t(),
        .from_mesh = false,
    };
    found = true;
}

/// What a mover can touch in its move, in its sphere space.
struct nearby_t {
    std::vector<triangle_t> triangles;
    std::vector<ellipsoid_t> ellipsoids;
};

/// Adds a world triangle, squashed into sphere space, turning it if its target mirrors.
void add_triangle(nearby_t& nearby, std::array<vec3_t, 3> corners, float squash,
                  std::uint32_t target, bool mirrored, triangle_origin_t origin, bool from_mesh) {
    if (mirrored) {
        std::swap(corners[1], corners[2]);
    }
    for (vec3_t& corner : corners) {
        corner.y *= squash;
    }
    const vec3_t normal = normalize(cross(corners[1] - corners[0], corners[2] - corners[0]));
    if (dot(normal, normal) == 0.0F) {
        return;
    }
    nearby.triangles.push_back({
        .corners = corners,
        .normal = normal,
        .target = target,
        .origin = origin,
        .from_mesh = from_mesh,
    });
}

/// The six faces of a box, two counterclockwise triangles each, facing out, by corner index.
constexpr std::array<std::array<unsigned, 3>, 12> box_triangles{
    {
        { 0, 4, 6 },
        { 0, 6, 2 },
        { 1, 3, 7 },
        { 1, 7, 5 },
        { 0, 1, 5 },
        { 0, 5, 4 },
        { 2, 6, 7 },
        { 2, 7, 3 },
        { 0, 2, 3 },
        { 0, 3, 1 },
        { 4, 5, 7 },
        { 4, 7, 6 },
    },
};

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
            tree.overlapping(local_min, local_max, [&](std::uint32_t triangle) {
                const vec3_t* corners = tree.corners(triangle);
                const vec3_t a = transform_point(target.matrix, corners[0]);
                const vec3_t b = transform_point(target.matrix, corners[1]);
                const vec3_t c = transform_point(target.matrix, corners[2]);
                add_triangle(nearby, { a, b, c }, squash, t, target.mirrored, tree.origin(triangle),
                             true);
            });
        }
    }
    return nearby;
}

/// Where a sphere of radius `r` going from `c` along `v` first touches anything near, or its end.
vec3_t unblocked(const nearby_t& nearby, vec3_t c, vec3_t v, float r) {
    hit_t hit;
    bool found = false;
    for (const triangle_t& triangle : nearby.triangles) {
        sweep_triangle(c, v, r, triangle, hit, found);
    }
    for (const ellipsoid_t& ellipsoid : nearby.ellipsoids) {
        sweep_ellipsoid(c, v, r, ellipsoid, hit, found);
    }
    return c + v * (found ? hit.time : 1.0F);
}

/// A move from `base` along `v` in sphere space, touching and sliding as the rules say, at most
/// `iterations` times, never sliding higher than `ceiling`; gives where it ended and adds what it
/// touched.
vec3_t slide(const snapshot_t& snapshot, const mover_t& mover, const nearby_t& nearby, vec3_t base,
             vec3_t v, float ceiling, std::vector<hit_t>& hits) {
    const float r = mover.horizontal;
    const float skin = snapshot.world->collision_skin;
    const vec3_t original = v;
    vec3_t previous_normal;
    bool has_previous = false;
    for (std::int32_t i = 0; i < snapshot.world->collision_iterations; ++i) {
        if (dot(v, v) < tiny * tiny) {
            break;
        }
        hit_t hit;
        bool found = false;
        for (const triangle_t& triangle : nearby.triangles) {
            sweep_triangle(base, v, r, triangle, hit, found);
        }
        for (const ellipsoid_t& ellipsoid : nearby.ellipsoids) {
            sweep_ellipsoid(base, v, r, ellipsoid, hit, found);
        }
        if (!found) {
            base = base + v;
            break;
        }
        hits.push_back(hit);
        // Just off what it touched, skin away. Touching it from the start, it may be in it, as
        // when it moved into the mover: pushed out only as far as nothing else is, so a mover
        // squeezed between two stays where it is, never pushed into either.
        const vec3_t at = base + v * hit.time;
        const vec3_t out = normalize(at - hit.point);
        base = hit.point + (dot(out, out) > 0.0F ? out : hit.normal) * (r + skin);
        if (hit.time <= 0.0F) {
            base = unblocked(nearby, at, base - at, r);
        } else {
            // Nor does the skin raise it past what its move and step allow: still no nearer
            // what it touched than its radius, since only the rise is taken off.
            base.y = std::fmin(base.y, std::fmax(ceiling, at.y));
        }
        const std::int32_t response =
            response_of(*snapshot.world, mover.group, snapshot.targets[hit.target].group);
        if (response == FBE_RESPONSE_STOP) {
            break;
        }
        const vec3_t remaining = v * (1.0F - hit.time);
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
        // Sliding up what it touched raises it no higher than its move and its step allow, so a
        // tall mover sliding up an edge isn't thrown up by its slide.
        next.y = std::fmin(next.y, std::fmax(ceiling - base.y, 0.0F));
        if (dot(next, original) <= 0.0F) {
            break;
        }
        v = next;
        previous_normal = hit.normal;
        has_previous = true;
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
    // No slide takes a mover higher than its move rises, and its step height.
    const float step = mover.step * squash;
    const float ceiling = start.y + std::fmax(wanted.y, 0.0F) + step;
    std::vector<hit_t> hits;
    vec3_t end = slide(snapshot, mover, nearby, start, wanted, ceiling, hits);

    // Blocked going across: up the step, across, and back down, if that lands on ground no
    // higher than the step and gets further.
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
            const vec3_t normal = normalize(
                vec3_t(stepped[i].normal.x, stepped[i].normal.y * squash, stepped[i].normal.z));
            landed = landed || normal.y >= walkable;
        }
        if (landed && down.y <= ceiling + tiny && across(start, down) > across(start, end) + tiny) {
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
            });
        }

        target_t target;
        target.entity = entity;
        target.group = collider.group;
        target.matrix = entities.world_matrix[entity];
        target.mirrored =
            dot(cross(target.matrix.axis(0), target.matrix.axis(1)), target.matrix.axis(2)) < 0.0F;
        target.horizontal = collider.horizontal;
        target.vertical = collider.vertical;
        std::int32_t shape = collider.shape;
        const bool mesh = entities.kind[entity] == FBE_ENTITY_MESH;
        const std::uint32_t geometry = mesh ? s.meshes[entities.component[entity]].geometry : none;
        if (shape == FBE_COLLISION_AUTO) {
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
