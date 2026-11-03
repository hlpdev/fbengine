#ifndef FBE_ENVIRONMENT_H
#define FBE_ENVIRONMENT_H

// Environments: an equirectangular image of the light around a scene, baked on the GPU into what
// glTF's metal-rough shading reads for image-based lighting (shaders/pbr.slang): its diffuse light,
// and its specular light with roughness across its levels, each an octahedral texture.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bakes `source`, a texture of the light all around, y up, +X at its middle and the sky along its
// top, into an environment, in the frame's graph, so what's drawn after it reads it: its specular
// light `size` texels across at no roughness, a power of 2 from 16 to 2048, each texel taking
// `samples` samples, 1 to 4096. Mipmaps on `source` make few samples less noisy, and a small sun
// count for what it should; it holds the source until it's baked, and its light past 8192 is read
// as 8192. A big bake's submitted a part at a time, which it waits for, so the GPU's never kept
// long. A frame that fails to run it runs it in the next. It starts the GPU if it hasn't; 0, with
// fbe_error's why, if it can't be baked, for a source that isn't live, or a size or samples out of
// range. Stopping the GPU loses what it baked: its calls then fail, until it's freed.
FBE_API uint64_t fbe_environment_bake(uint64_t source, int32_t size, int32_t samples);
// Frees an environment; false if the handle isn't one's.
FBE_API bool fbe_environment_free(uint64_t environment);
// Its diffuse light's texture, its specular light's, and the split sum's table every environment
// shares, each RGBA16F, held by it, sampled through no LOD bias; 0, with fbe_error's why, if the
// handle isn't one's or the GPU's stopped since it was baked.
FBE_API uint64_t fbe_environment_irradiance(uint64_t environment);
FBE_API uint64_t fbe_environment_specular(uint64_t environment);
FBE_API uint64_t fbe_environment_table(uint64_t environment);
// The highest level of its specular light that's read, at a roughness of 1; -1, with fbe_error's
// why, if the handle isn't one's.
FBE_API int32_t fbe_environment_levels(uint64_t environment);
// How many environments there are, those a stopped GPU lost included.
FBE_API int32_t fbe_environment_count(void);

#ifdef __cplusplus
}
#endif

#endif
