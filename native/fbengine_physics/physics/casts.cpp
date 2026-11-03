// Casts: rays, swept shapes, and overlaps, against a world's bodies of some layers, but a body
// to ignore and every sensor, between its steps.

#include "fbengine_physics.h"
#include "physics/physics.hpp"

#include <algorithm>
#include <bit>
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
#include <limits>
#include <optional>
#include <ranges>
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

JPH::BodyID body_of(const JPH::RayCastResult& hit) {
    return hit.mBodyID;
}

JPH::BodyID body_of(const JPH::ShapeCastResult& hit) {
    return hit.mBodyID2;
}

// Whether `a` is nearer than `b`, or as near and of a lesser body, or of the same body and a lesser
// part, so of hits as near, which is first is the same however Jolt's broad phase holds its bodies,
// which a restore doesn't put back.
template <typename Hit>
bool nearer(const Hit& a, const Hit& b) {
    if (a.GetEarlyOutFraction() != b.GetEarlyOutFraction()) {
        return a.GetEarlyOutFraction() < b.GetEarlyOutFraction();
    }
    if (body_of(a) != body_of(b)) {
        return body_of(a) < body_of(b);
    }
    return a.mSubShapeID2.GetValue() < b.mSubShapeID2.GetValue();
}

// The nearest hit, by nearer(). Jolt's collector of the closest leaves out the bodies whose
// bounding boxes the cast reaches no nearer than the hit it has, but a body's box can be reached a
// little after its shape is, so of two hits as near it keeps the first its broad phase gives it,
// whose order a restore changes. This one leaves out only what's `margin` further, a fraction of
// the cast: for a ray, whose hits are worked out exactly, a little more than rounding can put a
// box after its shape; and for a swept shape, whose hits are found to within a tolerance, which
// can be a long way along a cast that grazes, nothing, along a cast cut short just past the hit
// Jolt's closest gives.
template <typename Collector>
class nearest_t final : public Collector {
public:
    using hit_t = Collector::ResultType;

    explicit nearest_t(float margin) : m_margin(margin) {}

    void AddHit(const hit_t& hit) override {
        if (!m_hit.has_value() || nearer(hit, *m_hit)) {
            m_hit = hit;
            const float further = hit.GetEarlyOutFraction() + m_margin;
            if (further < Collector::GetEarlyOutFraction()) {
                Collector::UpdateEarlyOutFraction(further);
            }
        }
    }

    [[nodiscard]] const std::optional<hit_t>& hit() const {
        return m_hit;
    }

private:
    float m_margin;
    std::optional<hit_t> m_hit;
};

// A ten thousandth of a ray past its nearest hit, a tenth of a millimeter in a meter.
constexpr float ray_margin = 1e-4F;

// How many floats, or how many meters, apart two hits of a body are at most that are where its
// triangles share an edge or a corner: rounding of how far along a ray a triangle is goes by the
// triangle's size, not how far.
constexpr std::uint32_t shared_ulps = 4;
constexpr float shared_meters = 1e-5F;

// How many floats `a` is past `b`, of fractions of a ray, 0 or more, which `a` isn't less than.
std::uint32_t floats_apart(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) - std::bit_cast<std::uint32_t>(b);
}

// Whether `found`, nearest first after `crossed`, of a ray going `distance`, is where the ray
// crosses a surface it's kept a hit of, as the same body's within those, whoever's are between.
bool crossed_already(const JPH::RayCastResult& found, std::span<const JPH::RayCastResult> crossed,
                     float distance) {
    const float near = shared_meters / distance;
    for (const JPH::RayCastResult& kept : std::ranges::reverse_view(crossed)) {
        if (floats_apart(found.mFraction, kept.mFraction) > shared_ulps &&
            found.mFraction - kept.mFraction > near) {
            return false;
        }
        if (kept.mBodyID == found.mBodyID) {
            return true;
        }
    }
    return false;
}

// How far past Jolt's closest hit a swept shape's cast `length` long is cast again, as a fraction
// of it: a ray's margin, or a millimeter, ten times the tolerance Jolt finds a swept shape's hits
// to, if that's more.
float shape_margin(double length) {
    return length > 0.0 ? std::max(ray_margin, static_cast<float>(1e-3 / length)) : 1.0F;
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

// How a ray hits: a convex shape from inside it too, at 0, and a mesh's triangles from their
// front only, unless `back_faces`.
JPH::RayCastSettings ray_settings(bool back_faces) {
    JPH::RayCastSettings settings;
    settings.mTreatConvexAsSolid = true;
    settings.mBackFaceModeTriangles =
        back_faces ? JPH::EBackFaceMode::CollideWithBackFaces : JPH::EBackFaceMode::IgnoreBackFaces;
    return settings;
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
                                          uint32_t layers, uint64_t ignore, bool back_faces,
                                          fbe_physics_hit_t* hit) {
    using namespace fbe::physics;
    const fp_mode_t fp_mode;
    cast_t cast;
    JPH::RRayCast ray;
    if (!cast_of(world, ignore, cast) || !ray_of(*origin, *direction, distance, ray)) {
        return false;
    }
    nearest_t<JPH::CastRayCollector> collector(ray_margin);
    const layer_mask_t mask(layers);
    const bodies_but_t bodies(cast.ignore);
    cast.world->system.GetNarrowPhaseQueryNoLock().CastRay(
        ray, ray_settings(back_faces), collector, JPH::BroadPhaseLayerFilter(), mask, bodies);
    if (!collector.hit().has_value()) {
        return fail("the ray hits nothing");
    }
    *hit = hit_of(*cast.world, ray, distance, *collector.hit());
    return true;
}

FBE_PHYSICS_API int32_t fbe_physics_cast_ray_all(uint64_t world, const fbe_physics_vec3_t* origin,
                                                 const fbe_physics_vec3_t* direction,
                                                 float distance, uint32_t layers, uint64_t ignore,
                                                 bool back_faces, fbe_physics_hit_t* hits,
                                                 int32_t capacity) {
    using namespace fbe::physics;
    const fp_mode_t fp_mode;
    cast_t cast;
    JPH::RRayCast ray;
    if (!cast_of(world, ignore, cast) || !ray_of(*origin, *direction, distance, ray)) {
        return -1;
    }
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const layer_mask_t mask(layers);
    const bodies_but_t bodies(cast.ignore);
    cast.world->system.GetNarrowPhaseQueryNoLock().CastRay(
        ray, ray_settings(back_faces), collector, JPH::BroadPhaseLayerFilter(), mask, bodies);
    // Nearest first, and hits as near in the same order however Jolt's broad phase has them.
    std::ranges::sort(collector.mHits, nearer<JPH::RayCastResult>);
    // One hit where the ray crosses a surface: an edge or a corner its triangles share gives one
    // of each, the first of which, as CastRay gives, is kept; surfaces of a body further apart are
    // each hit.
    std::vector<JPH::RayCastResult> crossed;
    for (const JPH::RayCastResult& found : collector.mHits) {
        if (!crossed_already(found, crossed, distance)) {
            crossed.push_back(found);
        }
    }
    const auto count = std::min(crossed.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    for (std::size_t i = 0; i < count; ++i) {
        hits[i] = hit_of(*cast.world, ray, distance, crossed[i]);
    }
    return static_cast<int32_t>(crossed.size());
}

FBE_PHYSICS_API bool fbe_physics_cast_shape(uint64_t world, uint64_t shape,
                                            const fbe_physics_vec3_t* position,
                                            const fbe_physics_quat_t* rotation,
                                            const fbe_physics_vec3_t* motion, uint32_t layers,
                                            uint64_t ignore, fbe_physics_hit_t* hit) {
    using namespace fbe::physics;
    const fp_mode_t fp_mode;
    cast_t cast;
    const JPH::Shape* of = nullptr;
    JPH::RMat44 transform;
    if (!cast_of(world, ignore, cast) || !placed(shape, *position, *rotation, of, transform)) {
        return false;
    }
    if (!finite(*motion)) {
        return fail("a shape is cast by a finite motion");
    }
    JPH::ShapeCastSettings settings;
    settings.mReturnDeepestPoint = true;
    const layer_mask_t mask(layers);
    const bodies_but_t bodies(cast.ignore);
    const JPH::NarrowPhaseQuery& query = cast.world->system.GetNarrowPhaseQueryNoLock();
    // Jolt's closest hit, quickly, then every hit along the way to just past it, of which the
    // nearest, so it's the same of hits as near however the broad phase holds their bodies.
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> closest;
    query.CastShape(JPH::RShapeCast(of, JPH::Vec3::sOne(), transform, vec3(*motion)), settings,
                    JPH::RVec3::sZero(), closest, JPH::BroadPhaseLayerFilter(), mask, bodies);
    if (!closest.HadHit()) {
        return fail("the shape hits nothing");
    }
    const float cut =
        std::min(1.0F, std::max(closest.mHit.mFraction, 0.0F) + shape_margin(length(*motion)));
    nearest_t<JPH::CastShapeCollector> collector(std::numeric_limits<float>::infinity());
    query.CastShape(JPH::RShapeCast(of, JPH::Vec3::sOne(), transform, vec3(*motion) * cut),
                    settings, JPH::RVec3::sZero(), collector, JPH::BroadPhaseLayerFilter(), mask,
                    bodies);
    JPH::ShapeCastResult result = closest.mHit;
    if (collector.hit().has_value()) {
        result = *collector.hit();
        // Its fraction is of the cast cut short.
        result.mFraction *= cut;
    }
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
    const fp_mode_t fp_mode;
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
