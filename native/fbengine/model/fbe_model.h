#ifndef FBE_MODEL_H
#define FBE_MODEL_H

// Models: what a glTF a model load decoded makes on the engine's side: its meshes' geometry, its
// materials, its images' textures, and a scene's nodes as entities. Each thing made is held once,
// as its area's create holds it, and let go of by its area's release.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// What a material reads a texture for.
#define FBE_MODEL_BASE_COLOR         0
#define FBE_MODEL_METALLIC_ROUGHNESS 1
#define FBE_MODEL_NORMAL             2
#define FBE_MODEL_OCCLUSION          3
#define FBE_MODEL_EMISSIVE           4

// Whether the last call here failed for what's in its file, as fbe_error says, not for its
// arguments or what the engine could do.
FBE_API bool fbe_model_invalid(void);
// A new material of the model's material at `index`: its color, how much it shines, its shading,
// blend, alpha cutoff, and sides; 0 if there's no such material.
FBE_API uint64_t fbe_model_material(uint64_t load, int32_t index);
// A new geometry of the model's mesh at `index`, a surface a primitive, in their order, each of
// the `materials`, what fbe_model_material gave, `count` of them, by the model's indices, and its
// vertices and triangles, empty for lines and points; 0 if it can't be.
FBE_API uint64_t fbe_model_geometry(uint64_t load, int32_t index, const uint64_t* materials,
                                    int32_t count);
// How many levels the model's image at `index` has, 0 for one no material reads, as a KTX2's PNG
// fallback is, which isn't decoded and makes no texture; -1 for none. After fbe_model_drop_images,
// as it was.
FBE_API int32_t fbe_model_image_levels(uint64_t load, int32_t index);
// Lets go of its images' texels, which its textures have now.
FBE_API bool fbe_model_drop_images(uint64_t load);
// A new texture of the model's image at `index`, filtered and wrapped as the first of its
// materials to read it says, with mipmaps made if `mipmaps` and it has one level, starting the GPU
// if it hasn't; 0 if it can't be.
FBE_API uint64_t fbe_model_texture(uint64_t load, int32_t index, bool mipmaps);
// The image of the model's that a material reads, for an FBE_MODEL_ map, -1 for none or a material
// or a map that isn't there.
FBE_API int32_t fbe_model_material_image(uint64_t load, int32_t material, int32_t map);
// Has a material, what fbe_model_material gave of the model's material at `index`, read what it
// reads that the renderer takes, its base color, as its layer 0, multiplied, of its UV set, its
// UVs moved as its file says, from `textures`, what fbe_model_texture gave, `count` of them, by
// the model's indices. Where that texture's sampled otherwise than this material says, as when two
// materials read one image, it reads a new texture of the image, held once, in `other`, for the
// caller to let go of, or 0. The other maps are the modern renderer's, for it to take. False if it
// can't, and for one of no base color's texture, true.
FBE_API bool fbe_model_material_textures(uint64_t load, int32_t index, uint64_t material,
                                         const uint64_t* textures, int32_t count, uint64_t* other);
// What the model's file calls its material at `index`, and its image at `index`: an image's name,
// or for none, its URI, its percents decoded, up to any NUL. "" for none, and for what's not there;
// valid while the load is.
FBE_API const char* fbe_model_material_name(uint64_t load, int32_t index);
FBE_API const char* fbe_model_image_name(uint64_t load, int32_t index);
// The scene the model's file says to show, -1 for a file of no scenes, and how many it has; -1 for
// a load that isn't a model load done and loaded.
FBE_API int32_t fbe_model_scene(uint64_t load);
FBE_API int32_t fbe_model_scene_count(uint64_t load);
// Makes a scene's nodes, and those under them, entities under `root`, each named and placed as
// its node is: `scene` from 0, or -1 for the nodes in no other node, for a file of no scenes. A
// node with a mesh is a mesh of `geometries`, what fbe_model_geometry gave, `geometry_count` of
// them, skinned by `skins`, what fbe_skin_from_model gave, `skin_count` of them, its joints found
// under `root`; else a camera, hidden, so a file's doesn't draw over a game's until it's shown, or
// a light, with one more under it for a mesh with one of those; or a pivot. False, with what it
// made under `root` still there, if it can't.
FBE_API bool fbe_model_spawn(uint64_t load, int32_t scene, uint64_t root,
                             const uint64_t* geometries, int32_t geometry_count,
                             const uint64_t* skins, int32_t skin_count);

#ifdef __cplusplus
}
#endif

#endif
