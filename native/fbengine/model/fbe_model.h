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
#define FBE_MODEL_MAPS               5

// Whether the last call here failed for what's in its file, as fbe_error says, not for its
// arguments or what the engine could do.
FBE_API bool fbe_model_invalid(void);
// A new material of the model's material at `index`: its color, how much it shines, its shading,
// blend, alpha cutoff, sides, metalness, roughness, what it emits, its normal scale, and its
// occlusion strength; 0 if there's no such material.
FBE_API uint64_t fbe_model_material(uint64_t load, int32_t index);
// A new geometry of the model's mesh at `index`, a surface a primitive, in their order, each of
// the `materials`, what fbe_model_material gave, `count` of them, by the model's indices, or for
// one that names none, glTF's default, which its surfaces hold, and its vertices, tangents if the
// file has them, and triangles, empty for lines and points; 0 if it can't be.
FBE_API uint64_t fbe_model_geometry(uint64_t load, int32_t index, const uint64_t* materials,
                                    int32_t count);
// How many levels the model's image at `index` has, 0 for one no material reads, as a KTX2's PNG
// fallback is, which isn't decoded and makes no texture; -1 for none. After fbe_model_drop_images,
// as it was. One that's a file of its own, left undecoded as a texture of that file was there
// already, has 1 until it's decoded or dropped.
FBE_API int32_t fbe_model_image_levels(uint64_t load, int32_t index);
// Lets go of its images' texels, which its textures have now.
FBE_API bool fbe_model_drop_images(uint64_t load);
// A texture of the model's image at `index`, filtered and wrapped as the first of its materials to
// read it says, with mipmaps made if `mipmaps` and it has one level, starting the GPU if it hasn't,
// held once for the caller; 0 if it can't be. An image inside the model's file makes a new one. An
// image that's a file of its own, a URI image, has one texture that every model naming the file
// holds, for each way it's made: what's in the file, the color space it's read in, its mipmaps, and
// its sampling; it's made by the first, held again by the rest, and freed after the last lets go.
// Its path, as fbe_texture_path gives it, is the image file's res:// path.
FBE_API uint64_t fbe_model_texture(uint64_t load, int32_t index, bool mipmaps);
// The image of the model's that a material reads, for an FBE_MODEL_ map, -1 for none or a material
// or a map that isn't there.
FBE_API int32_t fbe_model_material_image(uint64_t load, int32_t material, int32_t map);
// Has a material, what fbe_model_material gave of the model's material at `index`, read what it
// reads: its base color, as its layer 0, multiplied, and its other maps, as its FBE_MAP_ maps, each
// of its UV set, its UVs moved as its file says, from `textures`, what fbe_model_texture gave,
// `count` of them, by the model's indices. Where a texture's sampled otherwise than this material
// says, as when two materials read one image, or is decoded as a color for a map that holds none,
// as when one image is both, it reads another texture of the image: one in `others`, or one more
// as fbe_model_texture gives them, held once, added there with its image's index in `other_images`,
// each with room for `capacity`, and `made` counting them, which the caller starts at 0, for all
// its materials, and lets go of whatever this returns. A map on a UV set past 1 is left out, said
// once on stderr. False if it can't, and for one reading nothing, true.
FBE_API bool fbe_model_material_textures(uint64_t load, int32_t index, uint64_t material,
                                         const uint64_t* textures, int32_t count, uint64_t* others,
                                         int32_t* other_images, int32_t capacity, int32_t* made);
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
