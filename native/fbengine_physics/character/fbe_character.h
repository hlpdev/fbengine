#ifndef FBE_CHARACTER_H
#define FBE_CHARACTER_H

// Characters: a shape the game moves through a world, as a player or an NPC walks, sliding along
// walls, walking up steps and slopes as steep as it's let, keeping to the floor going down them,
// and pushing dynamic bodies as hard as its strength, but pushed by none. Each has a kinematic
// body of its own shape, which casts hit, sensors notice, and dynamic bodies bump into, and which
// goes with it. Positions are its shape's origin's, in the world's space, as a body's are.

#include "../physics/fbe_physics.h"

// C's constants and structs, which C++'s enums and usings aren't.
// NOLINTBEGIN(modernize-macro-to-enum, modernize-use-using)

// What a character stands on, as fbe_character_ground says.
#define FBE_CHARACTER_ON_GROUND       0
#define FBE_CHARACTER_ON_STEEP_GROUND 1
#define FBE_CHARACTER_NOT_SUPPORTED   2
#define FBE_CHARACTER_IN_AIR          3

#ifdef __cplusplus
extern "C" {
#endif

// A vector in the world's space.
typedef struct fbe_character_vec3_t {
    float x;
    float y;
    float z;
} fbe_character_vec3_t;

// NOLINTEND(modernize-macro-to-enum, modernize-use-using)

// A new character of `shape`, a capsule as a rule, in `world` at `position`, unturned, with a body
// in layer 0; 0 if the world or the shape isn't one, the shape has a mesh in it, the position's
// more than 1e9 from 0, or the world has all the bodies it has room for.
FBE_PHYSICS_API uint64_t fbe_character_new(uint64_t world, uint64_t shape,
                                           const fbe_character_vec3_t* position);

// Frees `character`, and its body; false if it isn't a character.
FBE_PHYSICS_API bool fbe_character_free(uint64_t character);

// The handle of `character`'s body, 0 if it isn't a character.
FBE_PHYSICS_API uint64_t fbe_character_body(uint64_t character);

// Moves `character` at `velocity`, at most 1e6 each way, for `seconds`, more than 0 and at most 1,
// as far as what's in its way lets it, between steps; false if it isn't a character or a number's
// out of range. The game
// gives it its velocity each time, adding gravity's itself, as fbe_character_ground tells it to.
FBE_PHYSICS_API bool fbe_character_move(uint64_t character, const fbe_character_vec3_t* velocity,
                                        float seconds);

// Where `character` is; false if it isn't a character.
FBE_PHYSICS_API bool fbe_character_position(uint64_t character, fbe_character_vec3_t* position);

// Puts `character` at `position` at once, as a teleport does; false if it isn't a character or the
// position's more than 1e9 from 0.
FBE_PHYSICS_API bool fbe_character_set_position(uint64_t character,
                                                const fbe_character_vec3_t* position);

// How fast `character` went in its last move, which what was in its way slowed; false if it isn't
// a character.
FBE_PHYSICS_API bool fbe_character_velocity(uint64_t character, fbe_character_vec3_t* velocity);

// What `character` stood on after its last move, FBE_CHARACTER_ON_GROUND to
// FBE_CHARACTER_IN_AIR, -1 if it isn't a character: ground no steeper than its max slope, ground
// too steep, something it touches but can't stand on, or nothing.
FBE_PHYSICS_API int32_t fbe_character_ground(uint64_t character);

// The normal of the ground `character` stands on, how fast that ground moves where it stands, as a
// lift's does, and the handle of its body, 0 for none; false, or 0, if it isn't a character.
FBE_PHYSICS_API bool fbe_character_ground_normal(uint64_t character, fbe_character_vec3_t* normal);
FBE_PHYSICS_API bool fbe_character_ground_velocity(uint64_t character,
                                                   fbe_character_vec3_t* velocity);
FBE_PHYSICS_API uint64_t fbe_character_ground_body(uint64_t character);

// Makes `character` of `shape`, as crouching does, if it fits where it is; false if it isn't a
// character, the shape isn't one or has a mesh in it, or it doesn't fit, as standing up under a
// table doesn't.
FBE_PHYSICS_API bool fbe_character_set_shape(uint64_t character, uint64_t shape);

// The steepest ground `character` walks on, in radians from level, 50 degrees at first; its mass,
// in kilograms, 70 at first; its strength, the most force it pushes with, in newtons, 100 at
// first; and the highest step it walks up, and so the furthest down it keeps to the floor going
// down, in meters, 0.4 at first. -1 if it isn't a character.
FBE_PHYSICS_API float fbe_character_max_slope(uint64_t character);
FBE_PHYSICS_API float fbe_character_mass(uint64_t character);
FBE_PHYSICS_API float fbe_character_strength(uint64_t character);
FBE_PHYSICS_API float fbe_character_step_height(uint64_t character);

// Sets them: a max slope of 0 to pi / 2, a mass and a strength of more than 0, and a step height of
// 0 or more; false if it isn't a character or the number's out of range.
FBE_PHYSICS_API bool fbe_character_set_max_slope(uint64_t character, float radians);
FBE_PHYSICS_API bool fbe_character_set_mass(uint64_t character, float mass);
FBE_PHYSICS_API bool fbe_character_set_strength(uint64_t character, float strength);
FBE_PHYSICS_API bool fbe_character_set_step_height(uint64_t character, float height);

// How many characters there are, made and not freed.
FBE_PHYSICS_API int32_t fbe_character_count(void);

#ifdef __cplusplus
}
#endif

#endif
