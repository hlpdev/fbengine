#ifndef FBE_JOINT_H
#define FBE_JOINT_H

// Joints: two bodies of a world held together, or a body held to the world itself, as a door to
// its frame or a lamp to its chain. Each is made where its bodies are, in the world's space, so it
// holds them as they are, and goes when either body does.

#include "../physics/fbe_physics.h"

// C's constants and structs, which C++'s enums and usings aren't.
// NOLINTBEGIN(modernize-macro-to-enum, modernize-use-using)

// What a joint is, as fbe_joint_kind says.
#define FBE_JOINT_FIXED    1
#define FBE_JOINT_POINT    2
#define FBE_JOINT_HINGE    3
#define FBE_JOINT_SLIDER   4
#define FBE_JOINT_DISTANCE 5

// How a hinge's or a slider's motor drives it.
#define FBE_JOINT_MOTOR_OFF      0
#define FBE_JOINT_MOTOR_SPEED    1
#define FBE_JOINT_MOTOR_POSITION 2

#ifdef __cplusplus
extern "C" {
#endif

// A vector in the world's space.
typedef struct fbe_joint_vec3_t {
    float x;
    float y;
    float z;
} fbe_joint_vec3_t;

// NOLINTEND(modernize-macro-to-enum, modernize-use-using)

// Holds body `a` to body `b`, or with `b` 0 to the world, as they are: welded, so neither moves on
// the other. Each of these is 0 if a body isn't one, they're of two worlds, neither moves, or a
// vector isn't finite or, for an axis, is 0.
FBE_PHYSICS_API uint64_t fbe_joint_fixed(uint64_t a, uint64_t b);

// Holds `a` and `b` together at `point`, about which either turns any way, as a ball in a socket.
FBE_PHYSICS_API uint64_t fbe_joint_point(uint64_t a, uint64_t b, const fbe_joint_vec3_t* point);

// Holds `a` and `b` together at `point`, about which either turns only about `axis`, as a door on
// its hinges; its position is how far `b` is turned from `a` from how it was made, in radians by
// the right-hand rule about the axis, or with `b` 0, how far `a` is turned.
FBE_PHYSICS_API uint64_t fbe_joint_hinge(uint64_t a, uint64_t b, const fbe_joint_vec3_t* point,
                                         const fbe_joint_vec3_t* axis);

// Lets `a` and `b` slide on each other only along `axis`, as a drawer, never turning; its position
// is how far `b` has slid along the axis from `a` since it was made, in meters, or with `b` 0, how
// far `a` has.
FBE_PHYSICS_API uint64_t fbe_joint_slider(uint64_t a, uint64_t b, const fbe_joint_vec3_t* axis);

// Keeps `point_a` on `a` and `point_b` on `b` as far apart as they are, as a rod does, or with
// limits, a rope; its position is how far apart they are, in meters.
FBE_PHYSICS_API uint64_t fbe_joint_distance(uint64_t a, uint64_t b, const fbe_joint_vec3_t* point_a,
                                            const fbe_joint_vec3_t* point_b);

// Frees `joint`, letting its bodies go; false if it isn't a joint.
FBE_PHYSICS_API bool fbe_joint_free(uint64_t joint);

// What `joint` is, FBE_JOINT_FIXED to FBE_JOINT_DISTANCE, or 0 if it isn't a joint.
FBE_PHYSICS_API int32_t fbe_joint_kind(uint64_t joint);

// Holds a hinge between `low` and `high` radians from how it was made, from -pi to 0 and 0 to pi;
// a slider between `low` and `high` meters, at most 0 and at least 0; or a distance joint's points
// between `low` and `high` meters apart, 0 or more. False if it isn't one of those, or a limit's
// out of range.
FBE_PHYSICS_API bool fbe_joint_set_limits(uint64_t joint, float low, float high);

// Makes a hinge's, a slider's, or a distance joint's limits springs, of `frequency` a second, or 0
// for none, rigid, as at first, damped by `damping`, 0 or more: 0 bounces on, and 1 comes to rest
// without bouncing. False if it isn't one of those, or a number's out of range.
FBE_PHYSICS_API bool fbe_joint_set_spring(uint64_t joint, float frequency, float damping);

// Drives a hinge or a slider: at `target` radians or meters a second, or to `target` radians or
// meters from how it was made, with at most `strength` newton meters or newtons, more than 0, or
// not at all; waking its bodies. False if it isn't a hinge or a slider, or a number's out of range.
FBE_PHYSICS_API bool fbe_joint_set_motor(uint64_t joint, int32_t mode, float target,
                                         float strength);

// Puts `joint`'s position, as each kind says, in `position`; false if it isn't a hinge, a slider,
// or a distance joint.
FBE_PHYSICS_API bool fbe_joint_position(uint64_t joint, float* position);

// Turns `joint` off, letting its bodies go until it's on again, or on, waking them; false if it
// isn't a joint.
FBE_PHYSICS_API bool fbe_joint_set_enabled(uint64_t joint, bool enabled);
FBE_PHYSICS_API bool fbe_joint_enabled(uint64_t joint);

#ifdef __cplusplus
}
#endif

#endif
