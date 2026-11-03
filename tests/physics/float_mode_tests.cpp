// What Jolt holds of a call is the same whatever floating point mode the calling thread is in: a
// compound whose center of mass is a number too small for a float to keep but as one of its
// smallest, and a fixed joint whose point is.

#include "fbengine_physics.h"
#include "physics/physics.hpp"

#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <optional>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

using namespace fbe::physics;

namespace {

constexpr fbe_body_quat_t unturned{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };

// The thread's floating point mode with numbers too small for a float flushed to 0, as a library
// built with fast math sets it as it loads, until it goes.
class flushed_t {
public:
#if defined(__x86_64__) || defined(_M_X64)
    // MXCSR's flush to zero and denormals are zero.
    flushed_t() : m_before(_mm_getcsr()) {
        _mm_setcsr(m_before | 0x8040U);
    }

    ~flushed_t() {
        _mm_setcsr(m_before);
    }
#elifdef __aarch64__
    // FPCR's flush to zero.
    flushed_t() {
        asm volatile("mrs %0, fpcr" : "=r"(m_before));
        const std::uint64_t flushed = m_before | (std::uint64_t{ 1 } << 24U);
        asm volatile("msr fpcr, %0" : : "r"(flushed));
    }

    ~flushed_t() {
        asm volatile("msr fpcr, %0" : : "r"(m_before));
    }
#endif

    flushed_t(const flushed_t&) = delete;
    flushed_t& operator=(const flushed_t&) = delete;
    flushed_t(flushed_t&&) = delete;
    flushed_t& operator=(flushed_t&&) = delete;

private:
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int m_before;
#elifdef __aarch64__
    std::uint64_t m_before = 0;
#endif
};

std::uint32_t bits(float f) {
    return std::bit_cast<std::uint32_t>(f);
}

// The bits of the center of mass's x of a compound of two balls, one 1.5e-38 m off the other,
// made with the calling thread's small numbers flushed or not.
std::uint32_t compound_center(bool flush) {
    const std::uint64_t ball = fbe_shape_sphere(1.0F);
    const fbe_shape_quat_t none{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };
    const fbe_shape_vec3_t off{ .x = 1.5e-38F, .y = 0.0F, .z = 0.0F };
    const fbe_shape_vec3_t at{ .x = 0.0F, .y = 0.0F, .z = 0.0F };
    const std::array<fbe_shape_part_t, 2> parts{
        fbe_shape_part_t{ .shape = ball, .position = off, .rotation = none },
        fbe_shape_part_t{ .shape = ball, .position = at, .rotation = none },
    };
    std::uint64_t made = 0;
    {
        std::optional<flushed_t> flushed;
        if (flush) {
            flushed.emplace();
        }
        made = fbe_shape_compound(parts.data(), 2);
    }
    const std::uint32_t center = bits(find_shape(made)->GetCenterOfMass().GetX());
    fbe_shape_free(made);
    fbe_shape_free(ball);
    return center;
}

// What a fixed joint holds of two balls 2e-38 m apart, made with the calling thread's small numbers
// flushed or not: the bits of its points' x on each, and of the first's turn after a second
// pushed.
struct fixed_t {
    std::uint32_t on_a = 0;
    std::uint32_t on_b = 0;
    std::uint32_t turned = 0;

    bool operator==(const fixed_t&) const = default;
};

fixed_t fixed(bool flush) {
    const std::uint64_t world = fbe_physics_new(16);
    const std::uint64_t ball = fbe_shape_sphere(0.5F);
    const fbe_body_vec3_t at_a{ .x = 0.0F, .y = 0.0F, .z = 0.0F };
    const fbe_body_vec3_t at_b{ .x = 2e-38F, .y = 0.0F, .z = 0.0F };
    const std::uint64_t a = fbe_body_add(world, ball, FBE_BODY_DYNAMIC, &at_a, &unturned);
    const std::uint64_t b = fbe_body_add(world, ball, FBE_BODY_DYNAMIC, &at_b, &unturned);
    fbe_body_set_gravity_factor(a, 0.0F);
    fbe_body_set_gravity_factor(b, 0.0F);
    {
        std::optional<flushed_t> flushed;
        if (flush) {
            flushed.emplace();
        }
        fbe_joint_fixed(a, b);
    }
    fixed_t held;
    {
        const JPH::Constraints all = find_world(world)->system.GetConstraints();
        const auto* joint = static_cast<const JPH::FixedConstraint*>(all.at(0).GetPtr());
        held.on_a = bits(joint->GetConstraintToBody1Matrix().GetTranslation().GetX());
        held.on_b = bits(joint->GetConstraintToBody2Matrix().GetTranslation().GetX());
    }
    const fbe_body_vec3_t push{ .x = 0.0F, .y = 0.0F, .z = 1.0F };
    fbe_body_add_impulse(a, &push);
    for (int i = 0; i < 60; ++i) {
        fbe_physics_step(world, 1.0F / 60, 1);
    }
    fbe_body_quat_t turn{};
    fbe_body_rotation(a, &turn);
    held.turned = bits(turn.y);
    fbe_physics_free(world);
    fbe_shape_free(ball);
    return held;
}

} // namespace

TEST_CASE("a compound's center of mass is the same whatever floating point mode it's made in") {
    fbe_physics_set_threads(0);
    CHECK(compound_center(true) == compound_center(false));
    CHECK(compound_center(false) != 0);
}

TEST_CASE("a fixed joint holds the same whatever floating point mode it's made in") {
    fbe_physics_set_threads(0);
    const fixed_t plain = fixed(false);
    CHECK(fixed(true) == plain);
    CHECK((plain.on_a != 0 || plain.on_b != 0));
}
