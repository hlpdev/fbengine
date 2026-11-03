// Joints: Jolt's constraints between two bodies of a world, or a body and the world, by handle,
// each in its world's list, so a body that goes takes its joints with it before Jolt's body does.

#include "fbengine_physics.h"
#include "physics/physics.hpp"

#include <cmath>
#include <cstdint>
#include <format>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SpringSettings.h>
#include <memory>
#include <vector>

namespace {

using namespace fbe::physics;

struct joint_t {
    world_t* world;
    JPH::Ref<JPH::TwoBodyConstraint> constraint;
    std::int32_t kind;
    std::uint64_t a;
    std::uint64_t b;
    // Where it is in its world's list.
    std::size_t index;
};

pool_t<joint_t>& joints() {
    // Never freed, as Jolt isn't: statics freed as the process ends go in any order, and a world
    // left then would free its bodies into a pool that had gone.
    static auto* pool = new pool_t<joint_t>(JOINT_KIND);
    return *pool;
}

joint_t* find(std::uint64_t handle) {
    joint_t* joint = joints().get(handle);
    if (joint == nullptr) {
        fail(handle == 0 ? "there's no joint" : "the joint was freed");
    }
    return joint;
}

bool vec3(const fbe_joint_vec3_t& v, JPH::Vec3& made) {
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
        return fail("a joint's points and axes are finite");
    }
    made = JPH::Vec3(v.x, v.y, v.z);
    return true;
}

bool axis(const fbe_joint_vec3_t& v, JPH::Vec3& made) {
    if (!vec3(v, made)) {
        return false;
    }
    if (made.LengthSq() == 0.0F || !std::isfinite(made.LengthSq())) {
        return fail("a joint's axis isn't 0");
    }
    made = made.Normalized();
    return true;
}

// The bodies a joint between `a` and `b` (0 for the world) joins, and their world, if they're of
// one and either moves.
struct joined_t {
    world_t* world = nullptr;
    JPH::Body* a = nullptr;
    JPH::Body* b = &JPH::Body::sFixedToWorld;
};

bool joined(std::uint64_t a, std::uint64_t b, joined_t& bodies) {
    bodies.a = find_body(a, bodies.world);
    if (bodies.a == nullptr) {
        return false;
    }
    if (b != 0) {
        world_t* other = nullptr;
        bodies.b = find_body(b, other);
        if (bodies.b == nullptr) {
            return false;
        }
        if (other != bodies.world) {
            return fail("a joint's bodies are of one world");
        }
        if (a == b) {
            return fail("a joint is between two bodies, or a body and the world");
        }
    }
    if (bodies.a->IsStatic() && bodies.b->IsStatic()) {
        return fail("a joint has a body that moves");
    }
    return true;
}

// The joint `settings` make between `bodies`, as `kind`, waking them. Jolt measures a joint's
// position as its second body's from its first, so a body held to the world is its second.
std::uint64_t made(const JPH::TwoBodyConstraintSettings& settings, const joined_t& bodies,
                   std::int32_t kind, std::uint64_t a, std::uint64_t b) {
    world_t& world = *bodies.world;
    const JPH::Ref<JPH::TwoBodyConstraint> constraint =
        b == 0 ? settings.Create(*bodies.b, *bodies.a) : settings.Create(*bodies.a, *bodies.b);
    world.system.AddConstraint(constraint);
    world.system.GetBodyInterfaceNoLock().ActivateConstraint(constraint);
    const std::uint64_t handle = joints().add(
        std::make_unique<joint_t>(&world, constraint, kind, a, b, world.joints.size()));
    world.joints.push_back(handle);
    return handle;
}

void remove(std::uint64_t handle) {
    const joint_t* joint = joints().get(handle);
    world_t& world = *joint->world;
    world.system.RemoveConstraint(joint->constraint);
    const std::uint64_t last = world.joints.back();
    world.joints[joint->index] = last;
    joints().get(last)->index = joint->index;
    world.joints.pop_back();
    joints().remove(handle);
}

void wake(const joint_t& joint) {
    joint.world->system.GetBodyInterfaceNoLock().ActivateConstraint(joint.constraint);
}

JPH::EMotorState motor_state(std::int32_t mode) {
    switch (mode) {
    case FBE_JOINT_MOTOR_SPEED:
        return JPH::EMotorState::Velocity;
    case FBE_JOINT_MOTOR_POSITION:
        return JPH::EMotorState::Position;
    default:
        return JPH::EMotorState::Off;
    }
}

bool in_range(float low, float high, float lowest, float highest) {
    return std::isfinite(low) && std::isfinite(high) && low >= lowest && low <= high &&
           high <= highest;
}

} // namespace

namespace fbe::physics {

void free_joints(world_t& world) {
    while (!world.joints.empty()) {
        remove(world.joints.back());
    }
}

void free_joints_of(const world_t& world, std::uint64_t body) {
    const std::vector<std::uint64_t> all = world.joints;
    for (const std::uint64_t handle : all) {
        const joint_t* joint = joints().get(handle);
        if (joint->a == body || joint->b == body) {
            remove(handle);
        }
    }
}

} // namespace fbe::physics

extern "C" {

FBE_PHYSICS_API uint64_t fbe_joint_fixed(uint64_t a, uint64_t b) {
    const fp_mode_t fp_mode;
    joined_t bodies;
    if (!joined(a, b, bodies)) {
        return 0;
    }
    JPH::FixedConstraintSettings settings;
    settings.mAutoDetectPoint = true;
    return made(settings, bodies, FBE_JOINT_FIXED, a, b);
}

FBE_PHYSICS_API uint64_t fbe_joint_point(uint64_t a, uint64_t b, const fbe_joint_vec3_t* point) {
    const fp_mode_t fp_mode;
    joined_t bodies;
    JPH::Vec3 at;
    if (!joined(a, b, bodies) || !vec3(*point, at)) {
        return 0;
    }
    JPH::PointConstraintSettings settings;
    settings.mPoint1 = at;
    settings.mPoint2 = at;
    return made(settings, bodies, FBE_JOINT_POINT, a, b);
}

FBE_PHYSICS_API uint64_t fbe_joint_hinge(uint64_t a, uint64_t b, const fbe_joint_vec3_t* point,
                                         const fbe_joint_vec3_t* hinge_axis) {
    const fp_mode_t fp_mode;
    joined_t bodies;
    JPH::Vec3 at;
    JPH::Vec3 about;
    if (!joined(a, b, bodies) || !vec3(*point, at) || !axis(*hinge_axis, about)) {
        return 0;
    }
    JPH::HingeConstraintSettings settings;
    settings.mPoint1 = at;
    settings.mPoint2 = at;
    settings.mHingeAxis1 = about;
    settings.mHingeAxis2 = about;
    settings.mNormalAxis1 = about.GetNormalizedPerpendicular();
    settings.mNormalAxis2 = settings.mNormalAxis1;
    return made(settings, bodies, FBE_JOINT_HINGE, a, b);
}

FBE_PHYSICS_API uint64_t fbe_joint_slider(uint64_t a, uint64_t b, const fbe_joint_vec3_t* along) {
    const fp_mode_t fp_mode;
    joined_t bodies;
    JPH::Vec3 slide;
    if (!joined(a, b, bodies) || !axis(*along, slide)) {
        return 0;
    }
    JPH::SliderConstraintSettings settings;
    settings.mAutoDetectPoint = true;
    settings.mSliderAxis1 = slide;
    settings.mSliderAxis2 = slide;
    settings.mNormalAxis1 = slide.GetNormalizedPerpendicular();
    settings.mNormalAxis2 = settings.mNormalAxis1;
    return made(settings, bodies, FBE_JOINT_SLIDER, a, b);
}

FBE_PHYSICS_API uint64_t fbe_joint_distance(uint64_t a, uint64_t b, const fbe_joint_vec3_t* point_a,
                                            const fbe_joint_vec3_t* point_b) {
    const fp_mode_t fp_mode;
    joined_t bodies;
    JPH::Vec3 on_a;
    JPH::Vec3 on_b;
    if (!joined(a, b, bodies) || !vec3(*point_a, on_a) || !vec3(*point_b, on_b)) {
        return 0;
    }
    JPH::DistanceConstraintSettings settings;
    // The world's is the first, as made() has it.
    settings.mPoint1 = b == 0 ? on_b : on_a;
    settings.mPoint2 = b == 0 ? on_a : on_b;
    return made(settings, bodies, FBE_JOINT_DISTANCE, a, b);
}

FBE_PHYSICS_API bool fbe_joint_free(uint64_t joint) {
    if (find(joint) == nullptr) {
        return false;
    }
    remove(joint);
    return true;
}

FBE_PHYSICS_API int32_t fbe_joint_kind(uint64_t joint) {
    const joint_t* found = find(joint);
    return found != nullptr ? found->kind : 0;
}

FBE_PHYSICS_API bool fbe_joint_set_limits(uint64_t joint, float low, float high) {
    const fp_mode_t fp_mode;
    const joint_t* found = find(joint);
    if (found == nullptr) {
        return false;
    }
    switch (found->kind) {
    case FBE_JOINT_HINGE:
        if (!in_range(low, high, -JPH::JPH_PI, JPH::JPH_PI) || low > 0.0F || high < 0.0F) {
            return fail(
                std::format("a hinge's limits are -pi to 0 and 0 to pi, not {} and {}", low, high));
        }
        static_cast<JPH::HingeConstraint*>(found->constraint.GetPtr())->SetLimits(low, high);
        break;
    case FBE_JOINT_SLIDER:
        if (!in_range(low, high, -INFINITY, INFINITY) || low > 0.0F || high < 0.0F) {
            return fail(std::format("a slider's limits are at most 0 and at least 0, not {} and {}",
                                    low, high));
        }
        static_cast<JPH::SliderConstraint*>(found->constraint.GetPtr())->SetLimits(low, high);
        break;
    case FBE_JOINT_DISTANCE:
        if (!in_range(low, high, 0.0F, INFINITY)) {
            return fail(std::format("a distance joint's limits are 0 or more, the first no more "
                                    "than the second, not {} and {}",
                                    low, high));
        }
        static_cast<JPH::DistanceConstraint*>(found->constraint.GetPtr())->SetDistance(low, high);
        break;
    default:
        return fail("only a hinge, a slider, or a distance joint has limits");
    }
    wake(*found);
    return true;
}

FBE_PHYSICS_API bool fbe_joint_set_spring(uint64_t joint, float frequency, float damping) {
    const fp_mode_t fp_mode;
    const joint_t* found = find(joint);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(frequency) || frequency < 0.0F || !std::isfinite(damping) ||
        damping < 0.0F) {
        return fail(std::format("a spring's frequency and damping are 0 or more, not {} and {}",
                                frequency, damping));
    }
    const JPH::SpringSettings spring(JPH::ESpringMode::FrequencyAndDamping, frequency, damping);
    switch (found->kind) {
    case FBE_JOINT_HINGE:
        static_cast<JPH::HingeConstraint*>(found->constraint.GetPtr())
            ->SetLimitsSpringSettings(spring);
        break;
    case FBE_JOINT_SLIDER:
        static_cast<JPH::SliderConstraint*>(found->constraint.GetPtr())
            ->SetLimitsSpringSettings(spring);
        break;
    case FBE_JOINT_DISTANCE:
        static_cast<JPH::DistanceConstraint*>(found->constraint.GetPtr())
            ->SetLimitsSpringSettings(spring);
        break;
    default:
        return fail("only a hinge's, a slider's, or a distance joint's limits are springs");
    }
    wake(*found);
    return true;
}

FBE_PHYSICS_API bool fbe_joint_set_motor(uint64_t joint, int32_t mode, float target,
                                         float strength) {
    const fp_mode_t fp_mode;
    const joint_t* found = find(joint);
    if (found == nullptr) {
        return false;
    }
    if (found->kind != FBE_JOINT_HINGE && found->kind != FBE_JOINT_SLIDER) {
        return fail("only a hinge or a slider has a motor");
    }
    if (mode < FBE_JOINT_MOTOR_OFF || mode > FBE_JOINT_MOTOR_POSITION) {
        return fail(std::format("{} isn't a motor's mode", mode));
    }
    if (mode != FBE_JOINT_MOTOR_OFF &&
        (!std::isfinite(target) || !std::isfinite(strength) || strength <= 0.0F)) {
        return fail(std::format("a motor's target is finite, and its strength more than 0, not {} "
                                "and {}",
                                target, strength));
    }
    const JPH::EMotorState state = motor_state(mode);
    if (found->kind == FBE_JOINT_HINGE) {
        auto* hinge = static_cast<JPH::HingeConstraint*>(found->constraint.GetPtr());
        hinge->GetMotorSettings().SetTorqueLimit(strength);
        hinge->SetMotorState(state);
        if (state == JPH::EMotorState::Velocity) {
            hinge->SetTargetAngularVelocity(target);
        } else if (state == JPH::EMotorState::Position) {
            hinge->SetTargetAngle(target);
        }
    } else {
        auto* slider = static_cast<JPH::SliderConstraint*>(found->constraint.GetPtr());
        slider->GetMotorSettings().SetForceLimit(strength);
        slider->SetMotorState(state);
        if (state == JPH::EMotorState::Velocity) {
            slider->SetTargetVelocity(target);
        } else if (state == JPH::EMotorState::Position) {
            slider->SetTargetPosition(target);
        }
    }
    wake(*found);
    return true;
}

FBE_PHYSICS_API bool fbe_joint_position(uint64_t joint, float* position) {
    const fp_mode_t fp_mode;
    const joint_t* found = find(joint);
    if (found == nullptr) {
        return false;
    }
    switch (found->kind) {
    case FBE_JOINT_HINGE:
        *position =
            static_cast<JPH::HingeConstraint*>(found->constraint.GetPtr())->GetCurrentAngle();
        return true;
    case FBE_JOINT_SLIDER:
        *position =
            static_cast<JPH::SliderConstraint*>(found->constraint.GetPtr())->GetCurrentPosition();
        return true;
    case FBE_JOINT_DISTANCE: {
        const JPH::TwoBodyConstraint& constraint = *found->constraint;
        const JPH::RVec3 a = constraint.GetBody1()->GetCenterOfMassTransform() *
                             constraint.GetConstraintToBody1Matrix().GetTranslation();
        const JPH::RVec3 b = constraint.GetBody2()->GetCenterOfMassTransform() *
                             constraint.GetConstraintToBody2Matrix().GetTranslation();
        *position = JPH::Vec3(b - a).Length();
        return true;
    }
    default:
        return fail("only a hinge, a slider, or a distance joint has a position");
    }
}

FBE_PHYSICS_API bool fbe_joint_set_enabled(uint64_t joint, bool enabled) {
    const fp_mode_t fp_mode;
    const joint_t* found = find(joint);
    if (found == nullptr) {
        return false;
    }
    found->constraint->SetEnabled(enabled);
    wake(*found);
    return true;
}

FBE_PHYSICS_API bool fbe_joint_enabled(uint64_t joint) {
    const joint_t* found = find(joint);
    return found != nullptr && found->constraint->GetEnabled();
}

} // extern "C"
