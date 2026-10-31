// Casts: rays, swept shapes, and overlaps, against a world's bodies of some layers, but a body
// to ignore and every sensor, between its steps.

#include "fbengine_physics.h"
#include "physics/physics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <span>
#include <vector>

namespace fbe::physics {

namespace {

// The layers a cast hits, a bit each.
class layer_mask_t final : public JPH::ObjectLayerFilter {
public:
    explicit layer_mask_t(std::uint32_t mask) : m_mask(mask) {}

    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return ((m_mask >> layer_of(layer)) & 1U) != 0;
    }

private:
    std::uint32_t m_mask;
};

// Every body but one, and but the sensors.
class bodies_but_t final : public JPH::BodyFilter {
public:
    explicit bodies_but_t(JPH::BodyID ignore) : m_ignore(ignore) {}

    [[nodiscard]] bool ShouldCollide(const JPH::BodyID& body) const override {
        return body != m_ignore;
    }

    [[nodiscard]] bool ShouldCollideLocked(const JPH::Body& body) const override {
        return !body.IsSensor();
    }

private:
    JPH::BodyID m_ignore;
};

// What a cast's given, checked: its world, the body it ignores, and its layers.
struct cast_t {
    world_t* world = nullptr;
    JPH::BodyID ignore;
};

bool cast_of(std::uint64_t world, std::uint64_t ignore, cast_t& cast) {
    cast.world = find_world(world);
    if (cast.world == nullptr) {
        return false;
    }
    if (ignore != 0) {
        world_t* in = nullptr;
        const JPH::Body* body = find_body(ignore, in);
        if (body == nullptr) {
            return false;
        }
        // Another world's body's id may be one of this world's.
        if (in != cast.world) {
            return fail("the ignored body is of another world");
        }
        cast.ignore = body->GetID();
    }
    return true;
}

// How long `v` is, in doubles, which a float's square of can't overflow.
double length(const fbe_physics_vec3_t& v) {
    return std::sqrt((double{ v.x } * v.x) + (double{ v.y } * v.y) + (double{ v.z } * v.z));
}

bool finite(const fbe_physics_vec3_t& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

JPH::Vec3 vec3(const fbe_physics_vec3_t& v) {
    return { v.x, v.y, v.z };
}

fbe_physics_vec3_t vec3(JPH::Vec3Arg v) {
    return { .x = v.GetX(), .y = v.GetY(), .z = v.GetZ() };
}

// The ray a cast from `origin` along `direction` as far as `distance` is, if they're in range.
bool ray_of(const fbe_physics_vec3_t& origin, const fbe_physics_vec3_t& direction, float distance,
            JPH::RRayCast& ray) {
    const JPH::Vec3 along = vec3(direction);
    if (!finite(origin) || !finite(direction) || along.LengthSq() == 0.0F ||
        !std::isfinite(along.LengthSq())) {
        return fail("a ray is from a finite origin along a finite direction that isn't 0");
    }
    if (!std::isfinite(distance) || distance < 0.0F) {
        return fail(std::format("a ray goes 0 meters or more, not {}", distance));
    }
    ray = JPH::RRayCast(vec3(origin), along.Normalized() * distance);
    return true;
}

// What a ray going `distance` hit, which `world`'s body there's locked for.
fbe_physics_hit_t hit_of(const world_t& world, const JPH::RRayCast& ray, float distance,
                         const JPH::RayCastResult& result) {
    const JPH::BodyLockRead lock(world.system.GetBodyLockInterfaceNoLock(), result.mBodyID);
    const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
    if (!lock.Succeeded()) {
        return { .body = 0, .position = {}, .normal = {}, .distance = 0.0F, .triangle = -1 };
    }
    const JPH::Body& body = lock.GetBody();
    return {
        .body = body.GetUserData(),
        .position = vec3(JPH::Vec3(point)),
        .normal = vec3(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point)),
        .distance = result.mFraction * distance,
        .triangle = triangle_of(body, result.mSubShapeID2),
    };
}

// The shape a sweep or an overlap is of, where it's put, and turned, if they're in range.
bool placed(std::uint64_t shape, const fbe_physics_vec3_t& position,
            const fbe_physics_quat_t& rotation, const JPH::Shape*& found, JPH::RMat44& transform) {
    found = find_shape(shape);
    if (found == nullptr) {
        return false;
    }
    // Jolt casts a mesh's triangles against nothing.
    if (found->MustBeStatic()) {
        return fail("a cast's shape has no mesh in it");
    }
    JPH::Quat turned;
    if (!finite(position)) {
        return fail("a shape is cast from a finite position");
    }
    if (!normalized(rotation.x, rotation.y, rotation.z, rotation.w, turned)) {
        return false;
    }
    // Jolt's shape is about its center of mass, which is off the shape's origin.
    transform = JPH::RMat44::sRotationTranslation(turned, vec3(position))
                    .PreTranslated(found->GetCenterOfMass());
    return true;
}

} // namespace

std::int32_t triangle_of(const JPH::Body& body, const JPH::SubShapeID& part) {
    JPH::SubShapeID rest;
    const JPH::Shape* leaf = body.GetShape()->GetLeafShape(part, rest);
    if (leaf == nullptr || leaf->GetSubType() != JPH::EShapeSubType::Mesh) {
        return -1;
    }
    return static_cast<std::int32_t>(
        static_cast<const JPH::MeshShape*>(leaf)->GetTriangleUserData(rest));
}

} // namespace fbe::physics

extern "C" {

FBE_PHYSICS_API bool fbe_physics_cast_ray(uint64_t world, const fbe_physics_vec3_t* origin,
                                          const fbe_physics_vec3_t* direction, float distance,
                                          uint32_t layers, uint64_t ignore,
                                          fbe_physics_hit_t* hit) {
    using namespace fbe::physics;
    cast_t cast;
    JPH::RRayCast ray;
    if (!cast_of(world, ignore, cast) || !ray_of(*origin, *direction, distance, ray)) {
        return false;
    }
    JPH::RayCastResult result;
    const layer_mask_t mask(layers);
    const bodies_but_t bodies(cast.ignore);
    if (!cast.world->system.GetNarrowPhaseQueryNoLock().CastRay(
            ray, result, JPH::BroadPhaseLayerFilter(), mask, bodies)) {
        return fail("the ray hits nothing");
    }
    *hit = hit_of(*cast.world, ray, distance, result);
    return true;
}

FBE_PHYSICS_API int32_t fbe_physics_cast_ray_all(uint64_t world, const fbe_physics_vec3_t* origin,
                                                 const fbe_physics_vec3_t* direction,
                                                 float distance, uint32_t layers, uint64_t ignore,
                                                 fbe_physics_hit_t* hits, int32_t capacity) {
    using namespace fbe::physics;
    cast_t cast;
    JPH::RRayCast ray;
    if (!cast_of(world, ignore, cast) || !ray_of(*origin, *direction, distance, ray)) {
        return -1;
    }
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const layer_mask_t mask(layers);
    const bodies_but_t bodies(cast.ignore);
    cast.world->system.GetNarrowPhaseQueryNoLock().CastRay(
        ray, JPH::RayCastSettings(), collector, JPH::BroadPhaseLayerFilter(), mask, bodies);
    // Nearest first, and hits as near in the same order however Jolt's threads found them.
    std::ranges::sort(collector.mHits,
                      [](const JPH::RayCastResult& x, const JPH::RayCastResult& y) {
                          if (x.mFraction != y.mFraction) {
                              return x.mFraction < y.mFraction;
                          }
                          if (x.mBodyID != y.mBodyID) {
                              return x.mBodyID < y.mBodyID;
                          }
                          return x.mSubShapeID2.GetValue() < y.mSubShapeID2.GetValue();
                      });
    const auto count =
        std::min(collector.mHits.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    for (std::size_t i = 0; i < count; ++i) {
        hits[i] = hit_of(*cast.world, ray, distance, collector.mHits[i]);
    }
    return static_cast<int32_t>(collector.mHits.size());
}

FBE_PHYSICS_API bool fbe_physics_cast_shape(uint64_t world, uint64_t shape,
                                            const fbe_physics_vec3_t* position,
                                            const fbe_physics_quat_t* rotation,
                                            const fbe_physics_vec3_t* motion, uint32_t layers,
                                            uint64_t ignore, fbe_physics_hit_t* hit) {
    using namespace fbe::physics;
    cast_t cast;
    const JPH::Shape* of = nullptr;
    JPH::RMat44 transform;
    if (!cast_of(world, ignore, cast) || !placed(shape, *position, *rotation, of, transform)) {
        return false;
    }
    if (!finite(*motion)) {
        return fail("a shape is cast by a finite motion");
    }
    const JPH::RShapeCast sweep(of, JPH::Vec3::sOne(), transform, vec3(*motion));
    JPH::ShapeCastSettings settings;
    settings.mReturnDeepestPoint = true;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const layer_mask_t mask(layers);
    const bodies_but_t bodies(cast.ignore);
    cast.world->system.GetNarrowPhaseQueryNoLock().CastShape(
        sweep, settings, JPH::RVec3::sZero(), collector, JPH::BroadPhaseLayerFilter(), mask,
        bodies);
    if (!collector.HadHit()) {
        return fail("the shape hits nothing");
    }
    const JPH::ShapeCastResult& result = collector.mHit;
    const JPH::BodyLockRead lock(cast.world->system.GetBodyLockInterfaceNoLock(), result.mBodyID2);
    if (!lock.Succeeded()) {
        return fail("the shape hits nothing");
    }
    const JPH::Body& body = lock.GetBody();
    const JPH::Vec3 axis = result.mPenetrationAxis;
    *hit = {
        .body = body.GetUserData(),
        .position = vec3(JPH::Vec3(result.mContactPointOn2)),
        .normal = vec3(axis.LengthSq() > 0.0F ? -axis.Normalized() : JPH::Vec3::sZero()),
        .distance = static_cast<float>(result.mFraction * length(*motion)),
        .triangle = triangle_of(body, result.mSubShapeID2),
    };
    return true;
}

FBE_PHYSICS_API int32_t fbe_physics_overlap(uint64_t world, uint64_t shape,
                                            const fbe_physics_vec3_t* position,
                                            const fbe_physics_quat_t* rotation, uint32_t layers,
                                            uint64_t ignore, uint64_t* bodies, int32_t capacity) {
    using namespace fbe::physics;
    cast_t cast;
    const JPH::Shape* of = nullptr;
    JPH::RMat44 transform;
    if (!cast_of(world, ignore, cast) || !placed(shape, *position, *rotation, of, transform)) {
        return -1;
    }
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const layer_mask_t mask(layers);
    const bodies_but_t but(cast.ignore);
    cast.world->system.GetNarrowPhaseQueryNoLock().CollideShape(
        of, JPH::Vec3::sOne(), transform, JPH::CollideShapeSettings(), JPH::RVec3::sZero(),
        collector, JPH::BroadPhaseLayerFilter(), mask, but);
    // Each body once, in its id's order.
    std::vector<JPH::BodyID> touched;
    touched.reserve(collector.mHits.size());
    for (const JPH::CollideShapeResult& result : collector.mHits) {
        touched.push_back(result.mBodyID2);
    }
    const auto by_id = [](const JPH::BodyID& x, const JPH::BodyID& y) { return x < y; };
    std::ranges::sort(touched, by_id);
    const auto repeated = std::ranges::unique(touched);
    touched.erase(repeated.begin(), repeated.end());
    const auto count = std::min(touched.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    for (std::size_t i = 0; i < count; ++i) {
        const JPH::BodyLockRead lock(cast.world->system.GetBodyLockInterfaceNoLock(), touched[i]);
        bodies[i] = lock.Succeeded() ? lock.GetBody().GetUserData() : 0;
    }
    return static_cast<int32_t>(touched.size());
}

} // extern "C"
