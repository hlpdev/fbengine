#ifndef FBE_SHADER_H
#define FBE_SHADER_H

// Shaders of a game's own, which materials draw with: Slang the asset pipeline compiled with the
// scene's own shader, fbengine_tools' Cooker.CookShader, so nothing compiles at run time, and each
// is a family of pipelines as the scene's is. What's loaded is what that wrote: the engine's
// version it was compiled for, a stamp of the sources it was compiled against, which are the only
// ones its draws' layout is sure to be right for, and its SPIR-V, with the scene's seven entry
// points, or a full-screen pass's two, and nothing bound that the scene's pipelines haven't. A
// shader's code the GPU runs, so a pak's shaders are as trusted as the game.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// What a cooked shader's stamped with, for this engine.
FBE_API const char* fbe_shader_stamp(void);
// Whether a cooked file's `size` bytes would load; false for what isn't one, what's cooked for
// another version of the engine or other sources, SPIR-V without the scene's entry points, or with
// what the scene's pipelines don't bind, as fbe_error says. Nothing's kept.
FBE_API bool fbe_shader_check(const uint8_t* bytes, int64_t size);
// A new shader of a cooked file's `size` bytes; 0 for what fbe_shader_check refuses, or more
// different shaders than a game can have, 4095. `name` says which it is, where an error does. The
// same bytes again are the same code, whatever their name, so no more of what's kept and no
// pipelines to make.
FBE_API uint64_t fbe_shader_create(const uint8_t* bytes, int64_t size, const char* name);
FBE_API bool fbe_shader_retain(uint64_t shader);
FBE_API bool fbe_shader_release(uint64_t shader);
FBE_API bool fbe_shader_alive(uint64_t shader);
// Whether a shader's a full-screen pass's, as fbe_camera_add_pass runs, and a material with it
// draws as one with none does.
FBE_API bool fbe_shader_is_pass(uint64_t shader);
// Puts a cooked file's bytes in the shader's place, for what draws with it from the next frame on;
// false, leaving it as it was, as fbe_shader_create gives 0.
FBE_API bool fbe_shader_replace(uint64_t shader, const uint8_t* bytes, int64_t size,
                                const char* name);

// Has a material draw with `shader`, which it holds, or as the scene's own does with 0; false for a
// freed material or shader.
FBE_API bool fbe_material_set_shader(uint64_t material, uint64_t shader);
// The shader a material draws with, 0 for the scene's own, or for a freed material.
FBE_API uint64_t fbe_material_shader(uint64_t material);
// How many parameters a material has for its shader to read, each four floats.
#define FBE_MATERIAL_PARAMETERS 8
// Sets one of a material's parameters, 0 until it's set; false for a freed material, one that isn't
// there, or what aren't numbers.
FBE_API bool fbe_material_set_parameter(uint64_t material, int32_t index, float x, float y, float z,
                                        float w);
// One of a material's parameters into `values`, four floats; false as fbe_material_set_parameter.
FBE_API bool fbe_material_parameter(uint64_t material, int32_t index, float* values);

#ifdef __cplusplus
}
#endif

#endif
