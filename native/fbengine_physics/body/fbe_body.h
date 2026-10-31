#ifndef FBE_BODY_H
#define FBE_BODY_H

// Bodies: a shape at a place in a world, which doesn't move (static), is moved by gravity, forces,
// and what it touches (dynamic), or is moved by the game alone, pushing dynamic bodies out of its
// way (kinematic). Positions are the shape's origin's, in the world's space, in std.math's axes
// and meters; velocities in meters and radians a second.

#include "../physics/fbe_physics.h"

// C's constants and structs, which C++'s enums and usings aren't.
// NOLINTBEGIN(modernize-macro-to-enum, modernize-use-using)

// How a body moves.
#define FBE_BODY_STATIC    0
#define FBE_BODY_DYNAMIC   1
#define FBE_BODY_KINEMATIC 2

#ifdef __cplusplus
extern "C" {
#endif

// A vector in the world's space.
typedef struct fbe_body_vec3_t {
    float x;
    float y;
    float z;
} fbe_body_vec3_t;

// A rotation, std.math's Quat.
typedef struct fbe_body_quat_t {
    float x;
    float y;
    float z;
    float w;
} fbe_body_quat_t;

// NOLINTEND(modernize-macro-to-enum, modernize-use-using)

// A new body of `shape` in `world`, moving as `motion` says, at `position`, turned by `rotation`,
// which is normalized, and awake unless it's static; 0 if the world or the shape isn't one, the
// world has all the bodies it has room for, or a dynamic body's shape would have a mesh in it,
// or no inside to weigh, as a flat convex hull hasn't.
FBE_PHYSICS_API uint64_t fbe_body_add(uint64_t world, uint64_t shape, int32_t motion,
                                      const fbe_body_vec3_t* position,
                                      const fbe_body_quat_t* rotation);

// Frees `body`, taking it out of its world, with its joints; false if it isn't a body, or is a
// character's, which goes with its character.
FBE_PHYSICS_API bool fbe_body_free(uint64_t body);

// How `body` moves, FBE_BODY_STATIC to FBE_BODY_KINEMATIC, or -1 if it isn't a body.
FBE_PHYSICS_API int32_t fbe_body_motion(uint64_t body);

// Makes a dynamic `body` kinematic, or a kinematic one dynamic; false if it isn't a body, it's
// static or would be, it would be dynamic of a shape fbe_body_add refuses a dynamic one, or it's a
// character's, which only its character moves.
FBE_PHYSICS_API bool fbe_body_set_motion(uint64_t body, int32_t motion);

// Where `body` is, and how it's turned; false if it isn't a body.
FBE_PHYSICS_API bool fbe_body_position(uint64_t body, fbe_body_vec3_t* position);
FBE_PHYSICS_API bool fbe_body_rotation(uint64_t body, fbe_body_quat_t* rotation);

// Puts `body` at `position`, or turns it to `rotation`, at once, waking it; false if it isn't a
// body, or is a character's, or the vector or rotation isn't finite, or the rotation is 0.
FBE_PHYSICS_API bool fbe_body_set_position(uint64_t body, const fbe_body_vec3_t* position);
FBE_PHYSICS_API bool fbe_body_set_rotation(uint64_t body, const fbe_body_quat_t* rotation);

// How fast `body` moves, and turns about each axis; false if it isn't a body.
FBE_PHYSICS_API bool fbe_body_velocity(uint64_t body, fbe_body_vec3_t* velocity);
FBE_PHYSICS_API bool fbe_body_angular_velocity(uint64_t body, fbe_body_vec3_t* velocity);

// Sets how fast `body` moves or turns, waking it; false if it isn't a body, it's static or a
// character's, or the vector isn't finite.
FBE_PHYSICS_API bool fbe_body_set_velocity(uint64_t body, const fbe_body_vec3_t* velocity);
FBE_PHYSICS_API bool fbe_body_set_angular_velocity(uint64_t body, const fbe_body_vec3_t* velocity);

// Pushes a dynamic `body` through the next step with `force`, in newtons, at its center of mass or
// at `point` in the world, or turns it with `torque`, in newton meters, waking it; a static or
// kinematic body takes none. False if it isn't a body or a vector isn't finite.
FBE_PHYSICS_API bool fbe_body_add_force(uint64_t body, const fbe_body_vec3_t* force);
FBE_PHYSICS_API bool fbe_body_add_force_at(uint64_t body, const fbe_body_vec3_t* force,
                                           const fbe_body_vec3_t* point);
FBE_PHYSICS_API bool fbe_body_add_torque(uint64_t body, const fbe_body_vec3_t* torque);

// Changes a dynamic `body`'s velocity at once by `impulse`, in newton seconds, at its center of
// mass or at `point` in the world, or its angular velocity by `impulse`, in newton meter seconds,
// waking it; a static or kinematic body takes none. False if it isn't a body or a vector isn't
// finite.
FBE_PHYSICS_API bool fbe_body_add_impulse(uint64_t body, const fbe_body_vec3_t* impulse);
FBE_PHYSICS_API bool fbe_body_add_impulse_at(uint64_t body, const fbe_body_vec3_t* impulse,
                                             const fbe_body_vec3_t* point);
FBE_PHYSICS_API bool fbe_body_add_angular_impulse(uint64_t body, const fbe_body_vec3_t* impulse);

// `body`'s mass, in kilograms, from its shape's volume at 1000 kilograms a cubic meter (water's)
// unless it's been set; 0 for a static body, a mesh, or what isn't a body.
FBE_PHYSICS_API float fbe_body_mass(uint64_t body);

// Sets `body`'s mass, more than 0, which its inertia follows; false if it isn't a body, it's
// static or a mesh, or the mass is out of range.
FBE_PHYSICS_API bool fbe_body_set_mass(uint64_t body, float mass);

// How much `body` grips what it slides on, from 0 on, 0.2 at first, and how much it bounces, from
// 0, not at all, to 1, wholly, 0 at first: of two bodies touching, the friction is their
// frictions' geometric mean and the bounce the more of the two. -1 if it isn't a body.
FBE_PHYSICS_API float fbe_body_friction(uint64_t body);
FBE_PHYSICS_API float fbe_body_restitution(uint64_t body);
// False if it isn't a body, or the number's out of range.
FBE_PHYSICS_API bool fbe_body_set_friction(uint64_t body, float friction);
FBE_PHYSICS_API bool fbe_body_set_restitution(uint64_t body, float restitution);

// How much of `body`'s velocity, and of its angular velocity, goes a second, as air slows it, from
// 0 on, 0.05 at first; how much of the world's gravity pulls it, 1 at first. -1 for a static body
// or what isn't a body.
FBE_PHYSICS_API float fbe_body_linear_damping(uint64_t body);
FBE_PHYSICS_API float fbe_body_angular_damping(uint64_t body);
FBE_PHYSICS_API float fbe_body_gravity_factor(uint64_t body);
// False if it isn't a body, it's static, or the number isn't finite or is less than 0 for a
// damping.
FBE_PHYSICS_API bool fbe_body_set_linear_damping(uint64_t body, float damping);
FBE_PHYSICS_API bool fbe_body_set_angular_damping(uint64_t body, float damping);
FBE_PHYSICS_API bool fbe_body_set_gravity_factor(uint64_t body, float factor);

// Whether `body` is awake: moving, or touched since it came to rest; a body that's rested a
// while sleeps, costing nothing, until something wakes it. False for a static body or what isn't
// a body.
FBE_PHYSICS_API bool fbe_body_active(uint64_t body);

// Wakes `body`, or puts it to sleep; false if it isn't a body.
FBE_PHYSICS_API bool fbe_body_wake(uint64_t body);
FBE_PHYSICS_API bool fbe_body_sleep(uint64_t body);

// The layer `body` is in, 0 to 31, 0 at first, which its world says what it collides with of; -1
// if it isn't a body.
FBE_PHYSICS_API int32_t fbe_body_layer(uint64_t body);

// Puts `body` in `layer`, from the next step; false if it isn't a body or the layer's out of range.
FBE_PHYSICS_API bool fbe_body_set_layer(uint64_t body, int32_t layer);

// Whether `body` is a sensor, which collides with nothing, but says what comes into it and leaves
// it, as a trigger does; a static one notices what moves, a kinematic one what doesn't, too.
FBE_PHYSICS_API bool fbe_body_sensor(uint64_t body);

// Makes `body` a sensor, or not, waking it; false if it isn't a body, or is a character's.
FBE_PHYSICS_API bool fbe_body_set_sensor(uint64_t body, bool sensor);

// The bodies `body` touches, or that are in it if it's a sensor, as of the last step, up to
// `capacity` of them in `bodies`, giving how many there are, -1 if it isn't a body.
FBE_PHYSICS_API int32_t fbe_body_touching(uint64_t body, uint64_t* bodies, int32_t capacity);

#ifdef __cplusplus
}
#endif

#endif
