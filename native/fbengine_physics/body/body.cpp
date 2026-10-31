// Bodies: Jolt's, by handle, each in its world's list, so freeing the world frees them. Every call
// is the main thread's, between steps, so none locks Jolt's bodies.

#include "fbengine_physics.h"
#include "physics/physics.hpp"

#include <cmath>
#include <cstdint>
#include <format>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace fbe::physics;

struct body_t {
    world_t* world;
    JPH::BodyID id;
    // Where it is in its world's list.
    std::size_t index;
    // Whether its shape has an inside, and so a mass, which a mesh and a flat convex hull haven't.
    bool weighed;
    // Whether its shape can be dynamic: weighed, and with no mesh in it.
    bool can_be_dynamic;
    // Whether it's a character's, which moves it.
    bool character = false;

    [[nodiscard]] JPH::BodyInterface& bodies() const {
        return world->system.GetBodyInterfaceNoLock();
    }

    [[nodiscard]] JPH::Body& body() const {
        return *world->system.GetBodyLockInterfaceNoLock().TryGetBody(id);
    }
};

pool_t<body_t>& bodies() {
    // Never freed, as Jolt isn't: statics freed as the process ends go in any order, and a world
    // left then would free its bodies into a pool that had gone.
    static auto* pool = new pool_t<body_t>(BODY_KIND);
    return *pool;
}

body_t* find(std::uint64_t handle) {
    body_t* body = bodies().get(handle);
    if (body == nullptr) {
        fail(handle == 0 ? "there's no body" : "the body was freed");
    }
    return body;
}

// The body `handle` is, if it isn't a character's, which `doing` would move it as its character
// doesn't.
body_t* find_own(std::uint64_t handle) {
    body_t* body = find(handle);
    if (body != nullptr && body->character) {
        fail("a character's body goes as its character goes");
        return nullptr;
    }
    return body;
}

// The body `handle` is, if it's one that moves, which `doing` needs.
body_t* find_moving(std::uint64_t handle, const char* doing) {
    body_t* body = find(handle);
    if (body != nullptr && body->body().IsStatic()) {
        fail(std::format("a static body doesn't move, so it has no {}", doing));
        return nullptr;
    }
    return body;
}

bool finite(const fbe_body_vec3_t& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// `v` as Jolt's, if it's finite; false, saying so, if it isn't.
bool vec3(const fbe_body_vec3_t& v, JPH::Vec3& made) {
    if (!finite(v)) {
        return fail("a vector is finite");
    }
    made = JPH::Vec3(v.x, v.y, v.z);
    return true;
}

fbe_body_vec3_t vec3(JPH::Vec3Arg v) {
    return { .x = v.GetX(), .y = v.GetY(), .z = v.GetZ() };
}

// `q` as Jolt's, normalized, if it's finite and not 0; false, saying so, if it isn't.
bool quat(const fbe_body_quat_t& q, JPH::Quat& made) {
    return normalized(q.x, q.y, q.z, q.w, made);
}

// Why a shape that's `weighed` or not, and `must_be_static` or not, can't be dynamic, or nullptr
// if it can.
const char* not_dynamic(bool weighed, bool must_be_static) {
    if (must_be_static) {
        return "a dynamic body has no mesh in its shape, which only a static or kinematic body has";
    }
    if (!weighed) {
        return "a dynamic body's shape has an inside to weigh, which a flat convex hull hasn't";
    }
    return nullptr;
}

bool motion(std::int32_t motion, JPH::EMotionType& made) {
    switch (motion) {
    case FBE_BODY_STATIC:
        made = JPH::EMotionType::Static;
        return true;
    case FBE_BODY_DYNAMIC:
        made = JPH::EMotionType::Dynamic;
        return true;
    case FBE_BODY_KINEMATIC:
        made = JPH::EMotionType::Kinematic;
        return true;
    default:
        return fail(std::format("{} isn't a Motion", motion));
    }
}

// What a vector of a body's is, by `get`, into `out`; false if it isn't a body.
template <typename Get>
bool get_vec3(std::uint64_t handle, fbe_body_vec3_t* out, Get get) {
    const body_t* body = find(handle);
    if (body == nullptr) {
        return false;
    }
    *out = vec3(get(*body));
    return true;
}

// Does `push` to a body with a vector, if it's dynamic; false if it isn't a body or the vector
// isn't finite.
template <typename Push>
bool push(std::uint64_t handle, const fbe_body_vec3_t* v, Push push) {
    const body_t* body = find(handle);
    JPH::Vec3 made;
    if (body == nullptr || !vec3(*v, made)) {
        return false;
    }
    push(*body, made);
    return true;
}

// A float of what moves a body that moves, by `get`, or -1.
template <typename Get>
float get_motion(std::uint64_t handle, Get get) {
    const body_t* body = find_moving(handle, "motion");
    return body != nullptr ? get(*body->body().GetMotionProperties()) : -1.0F;
}

// Sets a float of what moves a body that moves, its `what`, by `set`, if `in_range`; false,
// saying `range`, if it isn't, or it isn't a body that moves.
template <typename Set>
bool set_motion_float(std::uint64_t handle, float value, bool in_range, const char* what,
                      const char* range, Set set) {
    const body_t* body = find_moving(handle, what);
    if (body == nullptr) {
        return false;
    }
    if (!std::isfinite(value) || !in_range) {
        return fail(std::format("{}, not {}", range, value));
    }
    set(*body->body().GetMotionProperties(), value);
    return true;
}

void forget(const body_t& body) {
    std::vector<std::uint64_t>& list = body.world->bodies;
    const std::uint64_t last = list.back();
    list[body.index] = last;
    bodies().get(last)->index = body.index;
    list.pop_back();
}

} // namespace

namespace fbe::physics {

void free_bodies(world_t& world) {
    std::vector<JPH::BodyID> ids;
    ids.reserve(world.bodies.size());
    for (const std::uint64_t handle : world.bodies) {
        // A character's goes with its character.
        if (!bodies().get(handle)->character) {
            ids.push_back(bodies().get(handle)->id);
        }
        bodies().remove(handle);
    }
    if (!ids.empty()) {
        JPH::BodyInterface& face = world.system.GetBodyInterfaceNoLock();
        face.RemoveBodies(ids.data(), static_cast<int>(ids.size()));
        face.DestroyBodies(ids.data(), static_cast<int>(ids.size()));
    }
    world.bodies.clear();
}

JPH::Body* find_body(std::uint64_t handle, world_t*& world) {
    const body_t* found = find(handle);
    if (found == nullptr) {
        return nullptr;
    }
    world = found->world;
    return &found->body();
}

std::uint64_t add_character_body(world_t& world, JPH::BodyID id) {
    auto body = std::make_unique<body_t>(&world, id, world.bodies.size(), true, false);
    body->character = true;
    const std::uint64_t handle = bodies().add(std::move(body));
    world.bodies.push_back(handle);
    return handle;
}

void drop_character_body(std::uint64_t handle) {
    const body_t* found = bodies().get(handle);
    if (found == nullptr) {
        return;
    }
    forget_contacts(*found->world, found->id);
    free_joints_of(*found->world, handle);
    forget(*found);
    bodies().remove(handle);
}

} // namespace fbe::physics

extern "C" {

FBE_PHYSICS_API uint64_t fbe_body_add(uint64_t world, uint64_t shape, int32_t motion_type,
                                      const fbe_body_vec3_t* position,
                                      const fbe_body_quat_t* rotation) {
    const fp_mode_t fp_mode;
    world_t* in = find_world(world);
    if (in == nullptr) {
        return 0;
    }
    const JPH::Shape* of = find_shape(shape);
    JPH::EMotionType type{};
    JPH::Vec3 at;
    JPH::Quat turned;
    if (of == nullptr || !motion(motion_type, type) || !vec3(*position, at) ||
        !quat(*rotation, turned)) {
        return 0;
    }
    if (in->bodies.size() >= in->system.GetMaxBodies()) {
        fail(std::format("the world has room for {} bodies, and has them all",
                         in->system.GetMaxBodies()));
        return 0;
    }
    const float mass = of->GetMassProperties().mMass;
    const bool weighed = std::isfinite(mass) && mass > 0.0F;
    const char* why = not_dynamic(weighed, of->MustBeStatic());
    if (type == JPH::EMotionType::Dynamic && why != nullptr) {
        fail(why);
        return 0;
    }

    const bool moves = type != JPH::EMotionType::Static;
    JPH::BodyCreationSettings settings(of, at, turned, type, object_layer(0, moves));
    if (moves && !weighed) {
        // A kinematic mesh weighs nothing, since nothing pushes it; Jolt asks for a mass all the
        // same.
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
        settings.mMassPropertiesOverride.SetMassAndInertiaOfSolidBox(JPH::Vec3::sOne(), 1000.0F);
    }
    JPH::BodyInterface& face = in->system.GetBodyInterfaceNoLock();
    JPH::Body* made = face.CreateBody(settings);
    if (made == nullptr) {
        fail(std::format("the world has room for {} bodies, and has them all",
                         in->system.GetMaxBodies()));
        return 0;
    }
    face.AddBody(made->GetID(),
                 moves ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    changed(*in);
    const std::uint64_t handle = bodies().add(
        std::make_unique<body_t>(in, made->GetID(), in->bodies.size(), weighed, why == nullptr));
    in->bodies.push_back(handle);
    made->SetUserData(handle);
    return handle;
}

FBE_PHYSICS_API bool fbe_body_free(uint64_t body) {
    const body_t* found = find_own(body);
    if (found == nullptr) {
        return false;
    }
    free_joints_of(*found->world, body);
    forget_contacts(*found->world, found->id);
    JPH::BodyInterface& face = found->bodies();
    face.RemoveBody(found->id);
    face.DestroyBody(found->id);
    changed(*found->world);
    forget(*found);
    return bodies().remove(body);
}

FBE_PHYSICS_API int32_t fbe_body_motion(uint64_t body) {
    const body_t* found = find(body);
    if (found == nullptr) {
        return -1;
    }
    switch (found->body().GetMotionType()) {
    case JPH::EMotionType::Static:
        return FBE_BODY_STATIC;
    case JPH::EMotionType::Dynamic:
        return FBE_BODY_DYNAMIC;
    case JPH::EMotionType::Kinematic:
        return FBE_BODY_KINEMATIC;
    }
    return -1;
}

FBE_PHYSICS_API bool fbe_body_set_motion(uint64_t body, int32_t motion_type) {
    const fp_mode_t fp_mode;
    const body_t* found = find_own(body);
    JPH::EMotionType type{};
    if (found == nullptr || !motion(motion_type, type)) {
        return false;
    }
    if (found->body().IsStatic() || type == JPH::EMotionType::Static) {
        return fail("a static body stays static, and a body that moves keeps moving");
    }
    if (type == JPH::EMotionType::Dynamic && !found->can_be_dynamic) {
        return fail(not_dynamic(found->weighed, found->body().GetShape()->MustBeStatic()));
    }
    found->bodies().SetMotionType(found->id, type, JPH::EActivation::Activate);
    // A kinematic body is never continuous, as fbe_body_set_continuous has it.
    if (type == JPH::EMotionType::Kinematic) {
        found->bodies().SetMotionQuality(found->id, JPH::EMotionQuality::Discrete);
    }
    // A kinematic sensor notices what doesn't move, as fbe_body_set_sensor has it.
    JPH::Body& made = found->body();
    made.SetCollideKinematicVsNonDynamic(made.IsSensor() && made.IsKinematic());
    return true;
}

FBE_PHYSICS_API bool fbe_body_position(uint64_t body, fbe_body_vec3_t* position) {
    const fp_mode_t fp_mode;
    return get_vec3(body, position, [](const body_t& b) { return b.bodies().GetPosition(b.id); });
}

FBE_PHYSICS_API bool fbe_body_rotation(uint64_t body, fbe_body_quat_t* rotation) {
    const fp_mode_t fp_mode;
    const body_t* found = find(body);
    if (found == nullptr) {
        return false;
    }
    const JPH::Quat q = found->bodies().GetRotation(found->id);
    *rotation = { .x = q.GetX(), .y = q.GetY(), .z = q.GetZ(), .w = q.GetW() };
    return true;
}

FBE_PHYSICS_API bool fbe_body_set_position(uint64_t body, const fbe_body_vec3_t* position) {
    const fp_mode_t fp_mode;
    return find_own(body) != nullptr && push(body, position, [](const body_t& b, JPH::Vec3Arg v) {
               b.bodies().SetPosition(b.id, v, JPH::EActivation::Activate);
           });
}

FBE_PHYSICS_API bool fbe_body_set_rotation(uint64_t body, const fbe_body_quat_t* rotation) {
    const fp_mode_t fp_mode;
    const body_t* found = find_own(body);
    JPH::Quat turned;
    if (found == nullptr || !quat(*rotation, turned)) {
        return false;
    }
    found->bodies().SetRotation(found->id, turned, JPH::EActivation::Activate);
    return true;
}

FBE_PHYSICS_API bool fbe_body_velocity(uint64_t body, fbe_body_vec3_t* velocity) {
    const fp_mode_t fp_mode;
    return get_vec3(body, velocity,
                    [](const body_t& b) { return b.bodies().GetLinearVelocity(b.id); });
}

FBE_PHYSICS_API bool fbe_body_angular_velocity(uint64_t body, fbe_body_vec3_t* velocity) {
    const fp_mode_t fp_mode;
    return get_vec3(body, velocity,
                    [](const body_t& b) { return b.bodies().GetAngularVelocity(b.id); });
}

FBE_PHYSICS_API bool fbe_body_set_velocity(uint64_t body, const fbe_body_vec3_t* velocity) {
    const fp_mode_t fp_mode;
    return find_own(body) != nullptr && find_moving(body, "velocity") != nullptr &&
           push(body, velocity,
                [](const body_t& b, JPH::Vec3Arg v) { b.bodies().SetLinearVelocity(b.id, v); });
}

FBE_PHYSICS_API bool fbe_body_set_angular_velocity(uint64_t body, const fbe_body_vec3_t* velocity) {
    const fp_mode_t fp_mode;
    return find_own(body) != nullptr && find_moving(body, "angular velocity") != nullptr &&
           push(body, velocity,
                [](const body_t& b, JPH::Vec3Arg v) { b.bodies().SetAngularVelocity(b.id, v); });
}

FBE_PHYSICS_API bool fbe_body_add_force(uint64_t body, const fbe_body_vec3_t* force) {
    const fp_mode_t fp_mode;
    return push(body, force, [](const body_t& b, JPH::Vec3Arg v) {
        b.bodies().AddForce(b.id, v, JPH::EActivation::Activate);
    });
}

FBE_PHYSICS_API bool fbe_body_add_force_at(uint64_t body, const fbe_body_vec3_t* force,
                                           const fbe_body_vec3_t* point) {
    const fp_mode_t fp_mode;
    JPH::Vec3 at;
    return vec3(*point, at) && push(body, force, [at](const body_t& b, JPH::Vec3Arg v) {
               b.bodies().AddForce(b.id, v, at, JPH::EActivation::Activate);
           });
}

FBE_PHYSICS_API bool fbe_body_add_torque(uint64_t body, const fbe_body_vec3_t* torque) {
    const fp_mode_t fp_mode;
    return push(body, torque, [](const body_t& b, JPH::Vec3Arg v) {
        b.bodies().AddTorque(b.id, v, JPH::EActivation::Activate);
    });
}

FBE_PHYSICS_API bool fbe_body_add_impulse(uint64_t body, const fbe_body_vec3_t* impulse) {
    const fp_mode_t fp_mode;
    return push(body, impulse,
                [](const body_t& b, JPH::Vec3Arg v) { b.bodies().AddImpulse(b.id, v); });
}

FBE_PHYSICS_API bool fbe_body_add_impulse_at(uint64_t body, const fbe_body_vec3_t* impulse,
                                             const fbe_body_vec3_t* point) {
    const fp_mode_t fp_mode;
    JPH::Vec3 at;
    return vec3(*point, at) && push(body, impulse, [at](const body_t& b, JPH::Vec3Arg v) {
               b.bodies().AddImpulse(b.id, v, at);
           });
}

FBE_PHYSICS_API bool fbe_body_add_angular_impulse(uint64_t body, const fbe_body_vec3_t* impulse) {
    const fp_mode_t fp_mode;
    return push(body, impulse,
                [](const body_t& b, JPH::Vec3Arg v) { b.bodies().AddAngularImpulse(b.id, v); });
}

FBE_PHYSICS_API float fbe_body_mass(uint64_t body) {
    const fp_mode_t fp_mode;
    const body_t* found = find(body);
    if (found == nullptr || !found->weighed || found->body().IsStatic()) {
        return 0.0F;
    }
    // A kinematic body keeps the mass it has as a dynamic one.
    return 1.0F / found->body().GetMotionProperties()->GetInverseMassUnchecked();
}

FBE_PHYSICS_API bool fbe_body_set_mass(uint64_t body, float mass) {
    const fp_mode_t fp_mode;
    const body_t* found = find_moving(body, "mass");
    if (found == nullptr) {
        return false;
    }
    if (!found->weighed) {
        return fail("a mesh has no inside to weigh");
    }
    if (!std::isfinite(mass) || mass <= 0.0F) {
        return fail(std::format("a mass is more than 0, not {}", mass));
    }
    found->body().GetMotionProperties()->ScaleToMass(mass);
    return true;
}

FBE_PHYSICS_API float fbe_body_friction(uint64_t body) {
    const fp_mode_t fp_mode;
    const body_t* found = find(body);
    return found != nullptr ? found->body().GetFriction() : -1.0F;
}

FBE_PHYSICS_API float fbe_body_restitution(uint64_t body) {
    const fp_mode_t fp_mode;
    const body_t* found = find(body);
    return found != nullptr ? found->body().GetRestitution() : -1.0F;
}

FBE_PHYSICS_API bool fbe_body_set_friction(uint64_t body, float friction) {
    const fp_mode_t fp_mode;
    const body_t* found = find(body);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(friction) || friction < 0.0F) {
        return fail(std::format("a friction is 0 or more, not {}", friction));
    }
    found->body().SetFriction(friction);
    return true;
}

FBE_PHYSICS_API bool fbe_body_set_restitution(uint64_t body, float restitution) {
    const fp_mode_t fp_mode;
    const body_t* found = find(body);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(restitution) || restitution < 0.0F || restitution > 1.0F) {
        return fail(std::format("a restitution is 0 to 1, not {}", restitution));
    }
    found->body().SetRestitution(restitution);
    return true;
}

FBE_PHYSICS_API float fbe_body_linear_damping(uint64_t body) {
    const fp_mode_t fp_mode;
    return get_motion(body, [](const JPH::MotionProperties& m) { return m.GetLinearDamping(); });
}

FBE_PHYSICS_API float fbe_body_angular_damping(uint64_t body) {
    const fp_mode_t fp_mode;
    return get_motion(body, [](const JPH::MotionProperties& m) { return m.GetAngularDamping(); });
}

FBE_PHYSICS_API float fbe_body_gravity_factor(uint64_t body) {
    const fp_mode_t fp_mode;
    return get_motion(body, [](const JPH::MotionProperties& m) { return m.GetGravityFactor(); });
}

FBE_PHYSICS_API bool fbe_body_set_linear_damping(uint64_t body, float damping) {
    const fp_mode_t fp_mode;
    return set_motion_float(body, damping, damping >= 0.0F, "damping", "a damping is 0 or more",
                            [](JPH::MotionProperties& m, float v) { m.SetLinearDamping(v); });
}

FBE_PHYSICS_API bool fbe_body_set_angular_damping(uint64_t body, float damping) {
    const fp_mode_t fp_mode;
    return set_motion_float(body, damping, damping >= 0.0F, "damping", "a damping is 0 or more",
                            [](JPH::MotionProperties& m, float v) { m.SetAngularDamping(v); });
}

FBE_PHYSICS_API bool fbe_body_set_gravity_factor(uint64_t body, float factor) {
    const fp_mode_t fp_mode;
    return set_motion_float(body, factor, true, "gravity factor", "a gravity factor is finite",
                            [](JPH::MotionProperties& m, float v) { m.SetGravityFactor(v); });
}

FBE_PHYSICS_API bool fbe_body_active(uint64_t body) {
    const body_t* found = find(body);
    return found != nullptr && found->body().IsActive();
}

FBE_PHYSICS_API bool fbe_body_wake(uint64_t body) {
    const body_t* found = find_own(body);
    if (found == nullptr) {
        return false;
    }
    found->bodies().ActivateBody(found->id);
    return true;
}

FBE_PHYSICS_API bool fbe_body_sleep(uint64_t body) {
    const body_t* found = find_own(body);
    if (found == nullptr) {
        return false;
    }
    found->bodies().DeactivateBody(found->id);
    return true;
}

FBE_PHYSICS_API int32_t fbe_body_layer(uint64_t body) {
    const body_t* found = find(body);
    return found != nullptr ? static_cast<int32_t>(layer_of(found->body().GetObjectLayer())) : -1;
}

FBE_PHYSICS_API bool fbe_body_set_layer(uint64_t body, int32_t layer) {
    const fp_mode_t fp_mode;
    const body_t* found = find(body);
    if (found == nullptr) {
        return false;
    }
    if (layer < 0 || layer >= FBE_PHYSICS_LAYERS) {
        return fail(std::format("a layer is 0 to {}, not {}", FBE_PHYSICS_LAYERS - 1, layer));
    }
    found->bodies().SetObjectLayer(
        found->id, object_layer(static_cast<std::uint32_t>(layer), !found->body().IsStatic()));
    return true;
}

FBE_PHYSICS_API bool fbe_body_sensor(uint64_t body) {
    const body_t* found = find(body);
    return found != nullptr && found->body().IsSensor();
}

FBE_PHYSICS_API bool fbe_body_set_sensor(uint64_t body, bool sensor) {
    const body_t* found = find_own(body);
    if (found == nullptr) {
        return false;
    }
    JPH::Body& made = found->body();
    made.SetIsSensor(sensor);
    // A sensor is never continuous, as fbe_body_set_continuous has it.
    if (sensor && !made.IsStatic()) {
        found->bodies().SetMotionQuality(found->id, JPH::EMotionQuality::Discrete);
    }
    // A kinematic sensor notices what doesn't move, and other kinematic bodies, too.
    made.SetCollideKinematicVsNonDynamic(sensor && made.IsKinematic());
    found->bodies().ActivateBody(found->id);
    return true;
}

FBE_PHYSICS_API int32_t fbe_body_touching(uint64_t body, uint64_t* bodies, int32_t capacity) {
    const body_t* found = find(body);
    if (found == nullptr) {
        return -1;
    }
    std::int32_t count = 0;
    for (const auto& [key, pair] : found->world->touching) {
        if (pair.a != body && pair.b != body) {
            continue;
        }
        if (count < capacity) {
            bodies[count] = pair.a == body ? pair.b : pair.a;
        }
        ++count;
    }
    return count;
}

FBE_PHYSICS_API bool fbe_body_move_to(uint64_t body, const fbe_body_vec3_t* position,
                                      const fbe_body_quat_t* rotation, float seconds) {
    const fp_mode_t fp_mode;
    const body_t* found = find_own(body);
    JPH::Vec3 at;
    JPH::Quat turned;
    if (found == nullptr || !vec3(*position, at) || !quat(*rotation, turned)) {
        return false;
    }
    if (!found->body().IsKinematic()) {
        return fail("only a kinematic body is moved to where it's put");
    }
    if (!std::isfinite(seconds) || seconds <= 0.0F) {
        return fail(std::format("a body's moved there over more than 0 seconds, not {}", seconds));
    }
    // Its velocities, as Jolt works them out, which a time too short for the way makes faster than
    // Jolt keeps a body, whose solver goes wrong past them, for a body it pushes too.
    const JPH::Body& moved = found->body();
    const JPH::Vec3 center = turned * moved.GetShape()->GetCenterOfMass();
    const JPH::Vec3 way = at + center - moved.GetCenterOfMassPosition();
    const JPH::Vec3 linear = way / seconds;
    const JPH::Quat turn = turned * moved.GetRotation().Conjugated();
    const JPH::Vec3 angular = turn.GetAngularVelocity(seconds);
    const JPH::MotionProperties& motion = *moved.GetMotionProperties();
    if (!(linear.Length() <= motion.GetMaxLinearVelocity()) ||
        !(angular.Length() <= motion.GetMaxAngularVelocity())) {
        return fail(std::format("a body moved there over {} seconds is faster than its most, {} "
                                "m/s and {} radians a second",
                                seconds, motion.GetMaxLinearVelocity(),
                                motion.GetMaxAngularVelocity()));
    }
    found->bodies().MoveKinematic(found->id, at, turned, seconds);
    return true;
}

FBE_PHYSICS_API bool fbe_body_continuous(uint64_t body) {
    const body_t* found = find(body);
    return found != nullptr && !found->body().IsStatic() &&
           found->body().GetMotionProperties()->GetMotionQuality() ==
               JPH::EMotionQuality::LinearCast;
}

FBE_PHYSICS_API bool fbe_body_set_continuous(uint64_t body, bool continuous) {
    const body_t* found = find_moving(body, "collisions to find along its way");
    if (found == nullptr) {
        return false;
    }
    // Jolt's sweeps are of dynamic bodies, and go through sensors.
    if (continuous && found->body().IsKinematic()) {
        return fail("a kinematic body goes where it's moved, so it has no collisions to find along "
                    "its way");
    }
    if (continuous && found->body().IsSensor()) {
        return fail(
            "a sensor collides with nothing, so it has no collisions to find along its way");
    }
    found->bodies().SetMotionQuality(found->id, continuous ? JPH::EMotionQuality::LinearCast
                                                           : JPH::EMotionQuality::Discrete);
    return true;
}

FBE_PHYSICS_API bool fbe_body_can_sleep(uint64_t body) {
    const body_t* found = find(body);
    return found != nullptr && !found->body().IsStatic() && found->body().GetAllowSleeping();
}

FBE_PHYSICS_API bool fbe_body_set_can_sleep(uint64_t body, bool can_sleep) {
    if (find_own(body) == nullptr) {
        return false;
    }
    const body_t* found = find_moving(body, "sleep");
    if (found == nullptr) {
        return false;
    }
    found->body().SetAllowSleeping(can_sleep);
    if (!can_sleep) {
        found->bodies().ActivateBody(found->id);
    }
    return true;
}

} // extern "C"
