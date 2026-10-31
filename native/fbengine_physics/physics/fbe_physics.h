#ifndef FBE_PHYSICS_H
#define FBE_PHYSICS_H

// fbengine_physics' worlds, on Jolt, and what every area of its C API shares: how its functions
// are exported, and its errors. Each area's header (fbe_<area>.h) has its own binding file in
// packages/fbengine_physics/c/, which tools/bindgen.sh makes, so an area's header uses only
// fixed-size types and its own structs, never another area's. A program loads this DLL beside
// fbengine_native, so no function here is named as one of fbengine's is. Everything but
// fbe_physics_version and fbe_physics_error is the main thread's, which the FB API's @MainThread
// keeps it to.

#include <stdbool.h>
#include <stdint.h>

#ifndef _WIN32
#define FBE_PHYSICS_API __attribute__((visibility("default")))
#else
#ifdef FBE_PHYSICS_BUILDING
#define FBE_PHYSICS_API __declspec(dllexport)
#else
#define FBE_PHYSICS_API __declspec(dllimport)
#endif
#endif

// C's constants and structs, which C++'s enums and usings aren't.
// NOLINTBEGIN(modernize-macro-to-enum, modernize-use-using)

// The most bodies a world can be made for.
#define FBE_PHYSICS_MAX_BODIES 1048576
// The most collision steps a step can be cut into.
#define FBE_PHYSICS_MAX_COLLISION_STEPS 16

#ifdef __cplusplus
extern "C" {
#endif

// A vector in std.math's axes: right-handed, Y up, -Z forward, in meters.
typedef struct fbe_physics_vec3_t {
    float x;
    float y;
    float z;
} fbe_physics_vec3_t;

// NOLINTEND(modernize-macro-to-enum, modernize-use-using)

// The version of fbengine_physics this DLL is, like "0.1.0", which is fbengine's.
FBE_PHYSICS_API const char* fbe_physics_version(void);

// What the last call that failed on this thread says went wrong.
FBE_PHYSICS_API const char* fbe_physics_error(void);

// How many threads Jolt's job system has, which step a world with the thread that asks: one a
// core, less the one asking, and none on a machine of one core. They start with the first world.
FBE_PHYSICS_API int32_t fbe_physics_threads(void);

// A new world, with no bodies and Earth's gravity, for at most `max_bodies`, 1 to
// FBE_PHYSICS_MAX_BODIES; 0 if that's out of range.
FBE_PHYSICS_API uint64_t fbe_physics_new(uint32_t max_bodies);

// Frees `world`, and its bodies; false if it isn't one.
FBE_PHYSICS_API bool fbe_physics_free(uint64_t world);

// How many bodies `world` has, -1 if it isn't a world.
FBE_PHYSICS_API int32_t fbe_physics_bodies(uint64_t world);

// Sorts `world`'s bodies for finding which might touch quickly, as is worth doing once after
// adding many at once, as loading a level does, rather than every step; false if it isn't a world.
FBE_PHYSICS_API bool fbe_physics_optimize(uint64_t world);

// The most bodies `world` was made for, 0 if it isn't a world.
FBE_PHYSICS_API uint32_t fbe_physics_max_bodies(uint64_t world);

// Sets `world`'s gravity, in meters a second a second; false if it isn't a world or the vector
// isn't finite.
FBE_PHYSICS_API bool fbe_physics_set_gravity(uint64_t world, const fbe_physics_vec3_t* gravity);

// Puts `world`'s gravity in `gravity`; false if it isn't a world.
FBE_PHYSICS_API bool fbe_physics_gravity(uint64_t world, fbe_physics_vec3_t* gravity);

// Steps `world` on by `seconds`, more than 0 and at most 1, its collisions found
// `collision_steps` times, 1 to FBE_PHYSICS_MAX_COLLISION_STEPS, on Jolt's job system; false if
// it isn't a world or either is out of range.
FBE_PHYSICS_API bool fbe_physics_step(uint64_t world, float seconds, int32_t collision_steps);

// How many times `world` has been stepped, -1 if it isn't a world.
FBE_PHYSICS_API int64_t fbe_physics_steps(uint64_t world);

// Whether `world`'s last step ran out of room for the pairs of bodies touching or their contacts,
// and so left some out, as a world of too many touching bodies for its size does.
FBE_PHYSICS_API bool fbe_physics_overflowed(uint64_t world);

#ifdef __cplusplus
}
#endif

#endif
