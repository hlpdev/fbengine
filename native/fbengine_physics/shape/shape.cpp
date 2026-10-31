// Shapes: Jolt's, by handle, each counted by the handle and by the bodies made of it, so it goes
// once all of them have.

#include "fbengine_physics.h"
#include "physics/physics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace {

using namespace fbe::physics;

struct shape_t {
    JPH::RefConst<JPH::Shape> shape;
    std::int32_t kind;
};

pool_t<shape_t>& shapes() {
    static pool_t<shape_t> pool;
    return pool;
}

bool finite(const fbe_shape_vec3_t& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite(const fbe_shape_quat_t& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

JPH::Vec3 vec3(const fbe_shape_vec3_t& v) {
    return { v.x, v.y, v.z };
}

fbe_shape_vec3_t vec3(JPH::Vec3Arg v) {
    return { .x = v.GetX(), .y = v.GetY(), .z = v.GetZ() };
}

// A length more than 0, which `what` is.
bool positive(float length, std::string_view what) {
    if (!std::isfinite(length) || length <= 0.0F) {
        return fail(std::format("{} is more than 0, not {}", what, length));
    }
    return true;
}

// The rounding of a shape's edges which Jolt's collisions are fastest with, a little less than
// the shape's thinnest half.
float convex_radius(float thinnest_half) {
    return std::min(JPH::cDefaultConvexRadius, thinnest_half * 0.5F);
}

// The shape `settings` makes, as `kind`, or 0 with what Jolt says went wrong.
std::uint64_t made(const JPH::ShapeSettings& settings, std::int32_t kind, std::string_view what) {
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        fail(std::format("{} can't be made: {}", what, result.GetError()));
        return 0;
    }
    return shapes().add(std::make_unique<shape_t>(result.Get(), kind));
}

const shape_t* find(std::uint64_t handle) {
    const shape_t* shape = shapes().get(handle);
    if (shape == nullptr) {
        fail(handle == 0 ? "there's no shape" : "the shape was freed");
    }
    return shape;
}

} // namespace

namespace fbe::physics {

const JPH::Shape* find_shape(std::uint64_t handle) {
    const shape_t* shape = find(handle);
    return shape != nullptr ? shape->shape.GetPtr() : nullptr;
}

} // namespace fbe::physics

extern "C" {

FBE_PHYSICS_API uint64_t fbe_shape_box(const fbe_shape_vec3_t* size) {
    if (!positive(size->x, "a box's width") || !positive(size->y, "a box's height") ||
        !positive(size->z, "a box's depth")) {
        return 0;
    }
    const JPH::Vec3 half = vec3(*size) * 0.5F;
    const JPH::BoxShapeSettings settings(half, convex_radius(half.ReduceMin()));
    return made(settings, FBE_SHAPE_BOX, "the box");
}

FBE_PHYSICS_API uint64_t fbe_shape_sphere(float radius) {
    if (!positive(radius, "a sphere's radius")) {
        return 0;
    }
    return made(JPH::SphereShapeSettings(radius), FBE_SHAPE_SPHERE, "the sphere");
}

FBE_PHYSICS_API uint64_t fbe_shape_capsule(float height, float radius) {
    if (!positive(radius, "a capsule's radius")) {
        return 0;
    }
    if (!std::isfinite(height) || height < radius * 2.0F) {
        fail(std::format("a capsule's height is at least twice its radius, {}, not {}",
                         radius * 2.0F, height));
        return 0;
    }
    const JPH::CapsuleShapeSettings settings((height * 0.5F) - radius, radius);
    return made(settings, FBE_SHAPE_CAPSULE, "the capsule");
}

FBE_PHYSICS_API uint64_t fbe_shape_cylinder(float height, float radius) {
    if (!positive(height, "a cylinder's height") || !positive(radius, "a cylinder's radius")) {
        return 0;
    }
    const float half = height * 0.5F;
    const JPH::CylinderShapeSettings settings(half, radius, convex_radius(std::min(half, radius)));
    return made(settings, FBE_SHAPE_CYLINDER, "the cylinder");
}

FBE_PHYSICS_API uint64_t fbe_shape_convex_hull(const fbe_shape_vec3_t* points, int32_t count) {
    if (count < 3) {
        fail(std::format("a convex hull is of 3 points or more, not {}", count));
        return 0;
    }
    JPH::Array<JPH::Vec3> hull;
    hull.reserve(static_cast<std::size_t>(count));
    for (const fbe_shape_vec3_t& point : std::span(points, static_cast<std::size_t>(count))) {
        if (!finite(point)) {
            fail("a convex hull's points are finite");
            return 0;
        }
        hull.push_back(vec3(point));
    }
    const JPH::ConvexHullShapeSettings settings(hull);
    return made(settings, FBE_SHAPE_CONVEX_HULL, "the convex hull");
}

FBE_PHYSICS_API uint64_t fbe_shape_mesh(const fbe_shape_vec3_t* vertices, int32_t vertex_count,
                                        const uint32_t* indices, int32_t index_count) {
    if (vertex_count < 3 || index_count < 3 || index_count % 3 != 0) {
        fail(std::format("a mesh is of 3 vertices or more and three indices a triangle, not {} "
                         "vertices and {} indices",
                         vertex_count, index_count));
        return 0;
    }
    JPH::VertexList points;
    points.reserve(static_cast<std::size_t>(vertex_count));
    for (const fbe_shape_vec3_t& vertex :
         std::span(vertices, static_cast<std::size_t>(vertex_count))) {
        if (!finite(vertex)) {
            fail("a mesh's vertices are finite");
            return 0;
        }
        points.emplace_back(vertex.x, vertex.y, vertex.z);
    }
    const std::span corners(indices, static_cast<std::size_t>(index_count));
    JPH::IndexedTriangleList triangles;
    triangles.reserve(corners.size() / 3);
    for (std::size_t i = 0; i < corners.size(); i += 3) {
        for (std::size_t corner = i; corner < i + 3; ++corner) {
            if (std::cmp_greater_equal(corners[corner], vertex_count)) {
                fail(std::format("index {} of the mesh is {}, past its {} vertices", corner,
                                 corners[corner], vertex_count));
                return 0;
            }
        }
        // Each keeps its number, which a cast hits it by.
        triangles.emplace_back(corners[i], corners[i + 1], corners[i + 2], 0,
                               static_cast<std::uint32_t>(i / 3));
    }
    JPH::MeshShapeSettings settings(std::move(points), std::move(triangles));
    settings.mPerTriangleUserData = true;
    return made(settings, FBE_SHAPE_MESH, "the mesh");
}

FBE_PHYSICS_API uint64_t fbe_shape_compound(const fbe_shape_part_t* parts, int32_t count) {
    if (count < 1) {
        fail(std::format("a compound is of a part or more, not {}", count));
        return 0;
    }
    JPH::StaticCompoundShapeSettings settings;
    for (const fbe_shape_part_t& part : std::span(parts, static_cast<std::size_t>(count))) {
        const JPH::Shape* shape = find_shape(part.shape);
        if (shape == nullptr) {
            return 0;
        }
        const fbe_shape_quat_t& q = part.rotation;
        const JPH::Quat rotation(q.x, q.y, q.z, q.w);
        if (!finite(part.position) || !finite(q) || rotation.LengthSq() == 0.0F) {
            fail("a compound's parts are where a finite position and rotation put them");
            return 0;
        }
        settings.AddShape(vec3(part.position), rotation.Normalized(), shape);
    }
    return made(settings, FBE_SHAPE_COMPOUND, "the compound");
}

FBE_PHYSICS_API bool fbe_shape_free(uint64_t shape) {
    return find(shape) != nullptr && shapes().remove(shape);
}

FBE_PHYSICS_API int32_t fbe_shape_kind(uint64_t shape) {
    const shape_t* found = find(shape);
    return found != nullptr ? found->kind : 0;
}

FBE_PHYSICS_API bool fbe_shape_bounds(uint64_t shape, fbe_shape_vec3_t* low,
                                      fbe_shape_vec3_t* high) {
    const shape_t* found = find(shape);
    if (found == nullptr) {
        return false;
    }
    // Jolt keeps a shape about its center of mass.
    const JPH::AABox bounds = found->shape->GetLocalBounds();
    const JPH::Vec3 center = found->shape->GetCenterOfMass();
    *low = vec3(bounds.mMin + center);
    *high = vec3(bounds.mMax + center);
    return true;
}

FBE_PHYSICS_API float fbe_shape_volume(uint64_t shape) {
    const shape_t* found = find(shape);
    return found != nullptr ? std::max(found->shape->GetVolume(), 0.0F) : 0.0F;
}

} // extern "C"
