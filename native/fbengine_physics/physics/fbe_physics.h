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
// The most threads Jolt's job system has besides the one stepping a world: Jolt does at most 32
// things at once.
#define FBE_PHYSICS_MAX_THREADS 31
// How many layers a world's bodies are in, 0 to 31.
#define FBE_PHYSICS_LAYERS 32

#ifdef __cplusplus
extern "C" {
#endif

// A vector in std.math's axes: right-handed, Y up, -Z forward, in meters.
typedef struct fbe_physics_vec3_t {
    float x;
    float y;
    float z;
} fbe_physics_vec3_t;

// A rotation, std.math's Quat.
typedef struct fbe_physics_quat_t {
    float x;
    float y;
    float z;
    float w;
} fbe_physics_quat_t;

// What a cast hit: a body, the point it hit it at, and the body's surface's normal there, how far
// along the cast it was, in meters, and the number of the mesh's triangle it hit, or -1.
typedef struct fbe_physics_hit_t {
    uint64_t body;
    fbe_physics_vec3_t position;
    fbe_physics_vec3_t normal;
    float distance;
    int32_t triangle;
} fbe_physics_hit_t;

// Two bodies that began touching, `a` and `b`, where, the normal from a to b, and how fast they
// came together along it, in meters a second.
typedef struct fbe_physics_contact_t {
    uint64_t a;
    uint64_t b;
    fbe_physics_vec3_t position;
    fbe_physics_vec3_t normal;
    float speed;
} fbe_physics_contact_t;

// NOLINTEND(modernize-macro-to-enum, modernize-use-using)

// The version of fbengine_physics this DLL is, like "0.1.0", which is fbengine's.
FBE_PHYSICS_API const char* fbe_physics_version(void);

// What the last call that failed on this thread says went wrong.
FBE_PHYSICS_API const char* fbe_physics_error(void);

// How many threads Jolt's job system has, which step a world with the thread that asks: as
// fbe_physics_set_threads said, or else one a core, less the one asking, at most
// FBE_PHYSICS_MAX_THREADS, and none on a machine of one core. They start with the first world.
FBE_PHYSICS_API int32_t fbe_physics_threads(void);

// Gives Jolt's job system `threads`, 0 to FBE_PHYSICS_MAX_THREADS, from the first world on, or at
// once if it's started, between steps; false if it's out of range.
FBE_PHYSICS_API bool fbe_physics_set_threads(int32_t threads);

// How many worlds there are, made and not freed.
FBE_PHYSICS_API int32_t fbe_physics_worlds(void);

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
// `collision_steps` times, 1 to FBE_PHYSICS_MAX_COLLISION_STEPS, a step of seconds over
// collision_steps each, on Jolt's job system, gathering what began and ended touching in all of
// them; false if it isn't a world or either is out of range. A world made and stepped by the same
// calls in the same order steps the same, bit for bit, on any number of threads, whatever floating
// point mode the calling thread is in, while no step overflows; Jolt is built to make it the same
// on every target, compiler, and build too, which is checked on Windows x64 so far.
FBE_PHYSICS_API bool fbe_physics_step(uint64_t world, float seconds, int32_t collision_steps);

// How many times `world` has been stepped, -1 if it isn't a world.
FBE_PHYSICS_API int64_t fbe_physics_steps(uint64_t world);

// Whether `world`'s last step ran out of room for the pairs of bodies touching or their contacts,
// and so left some out, as a world of too many touching bodies for its size does. Which it left
// out depends on its threads, so a step that overflows isn't deterministic.
FBE_PHYSICS_API bool fbe_physics_overflowed(uint64_t world);

// Makes the bodies of layers `a` and `b`, 0 to FBE_PHYSICS_LAYERS - 1, collide, or not, from the
// next step; every layer collides with every one at first. False if it isn't a world or a layer is
// out of range.
FBE_PHYSICS_API bool fbe_physics_set_collides(uint64_t world, int32_t a, int32_t b, bool collides);

// Whether the bodies of layers `a` and `b` collide; false if it isn't a world or a layer's out of
// range.
FBE_PHYSICS_API bool fbe_physics_collides(uint64_t world, int32_t a, int32_t b);

// Casts a ray from `origin` along `direction`, which isn't 0, as far as `distance`, from 0 on, at
// the bodies of the `layers` (a bit for each), but `ignore`'s, and puts the nearest hit in `hit`;
// false if it hits none, isn't a world, `ignore` is another world's, or a number's out of range,
// when fbe_physics_error says why. It hits a mesh's triangles from the front only, unless
// `back_faces`, so a ray from inside a room hits its walls, which face in, and one from outside
// goes in; and a convex shape from inside too, at 0. Casts never hit a sensor.
// (Jolt's test of a ray against a triangle isn't watertight: a ray aimed just where triangles meet
// can pass between them, which a small sphere's fbe_physics_cast_shape doesn't.)
FBE_PHYSICS_API bool fbe_physics_cast_ray(uint64_t world, const fbe_physics_vec3_t* origin,
                                          const fbe_physics_vec3_t* direction, float distance,
                                          uint32_t layers, uint64_t ignore, bool back_faces,
                                          fbe_physics_hit_t* hit);

// As fbe_physics_cast_ray, but every hit, nearest first, up to `capacity` of them in `hits`,
// giving how many there were, -1 if it isn't a world or a number's out of range: one where it
// crosses each surface, though it crosses where a body's triangles share an edge or a corner.
FBE_PHYSICS_API int32_t fbe_physics_cast_ray_all(uint64_t world, const fbe_physics_vec3_t* origin,
                                                 const fbe_physics_vec3_t* direction,
                                                 float distance, uint32_t layers, uint64_t ignore,
                                                 bool back_faces, fbe_physics_hit_t* hits,
                                                 int32_t capacity);

// Sweeps `shape`, which has no mesh in it, its origin at `position` and turned by `rotation`, by
// `motion`, at the bodies of the `layers` but `ignore`'s, and puts the first hit in `hit`, its
// distance how far along the motion it got; false as fbe_physics_cast_ray is. A shape already
// touching a body hits it at 0.
FBE_PHYSICS_API bool fbe_physics_cast_shape(uint64_t world, uint64_t shape,
                                            const fbe_physics_vec3_t* position,
                                            const fbe_physics_quat_t* rotation,
                                            const fbe_physics_vec3_t* motion, uint32_t layers,
                                            uint64_t ignore, fbe_physics_hit_t* hit);

// The bodies of the `layers` but `ignore`'s that `shape`, which has no mesh in it, its origin at
// `position` and turned by `rotation`, touches, up to `capacity` of them in `bodies`, in an order
// that's the same each run, giving how many there were, -1 if it isn't a world or a number's out of
// range.
FBE_PHYSICS_API int32_t fbe_physics_overlap(uint64_t world, uint64_t shape,
                                            const fbe_physics_vec3_t* position,
                                            const fbe_physics_quat_t* rotation, uint32_t layers,
                                            uint64_t ignore, uint64_t* bodies, int32_t capacity);

// Saves all `world`'s steps change, and `carried`, the time an Update carries to the next, as a
// state the world keeps until it's next saved, which fbe_physics_saved copies, giving how many
// bytes it takes, -1 if it isn't a world. Its steps go on from it, restored, exactly as they went
// on from where it was saved.
FBE_PHYSICS_API int64_t fbe_physics_save(uint64_t world, double carried);

// Copies the state fbe_physics_save last saved of `world` into up to `capacity` bytes at `bytes`,
// if there's room for it, giving how many bytes it takes, 0 if it's never been saved, -1 if it
// isn't a world.
FBE_PHYSICS_API int64_t fbe_physics_saved(uint64_t world, uint8_t* bytes, int64_t capacity);

// Restores `world` from the `size` bytes of a state at `bytes`, putting what it carried in
// `carried`; false, leaving the world as it was, if they aren't the state of this world's bodies,
// joints, and characters as they are, saved by this version of fbengine_physics, as it saved
// them.
FBE_PHYSICS_API bool fbe_physics_restore(uint64_t world, const uint8_t* bytes, int64_t size,
                                         double* carried);

// The pairs of bodies that began touching in `world`'s last step, up to `capacity` of them in
// `contacts`, in an order that's the same each run, giving how many there were, -1 if it isn't a
// world. A sensor's are the bodies that came into it.
FBE_PHYSICS_API int32_t fbe_physics_began(uint64_t world, fbe_physics_contact_t* contacts,
                                          int32_t capacity);

// The pairs of bodies that stopped touching in `world`'s last step, two handles each, up to
// `capacity` pairs in `pairs`, giving how many there were, -1 if it isn't a world. A body that's
// freed stops touching what it touched with none.
FBE_PHYSICS_API int32_t fbe_physics_ended(uint64_t world, uint64_t* pairs, int32_t capacity);

#ifdef __cplusplus
}
#endif

#endif
