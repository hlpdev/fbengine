#ifndef FBE_RENDER_H
#define FBE_RENDER_H

// Drawing the scene: what each camera draws, and what lights it, read from the scene into lists
// once a frame, so drawing never reads the live scene, and a render thread could take drawing
// over without the API changing. Meshes, sprites, emitters, Instances, ground planes, and terrains
// it might see go in them, past what's wholly behind its occluders, in material order 0.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// How a draw meets the depth what's drawn before it left: it tests it and writes its own, as the
// scene's opaque draws do; it tests it only, as what blends in the scene does; or neither, as
// every material order but 0 does, drawn furthest first, behind the scene or over it, as skies
// and overlays want.
#define FBE_DEPTH_TEST_AND_WRITE 0
#define FBE_DEPTH_TEST           1
#define FBE_DEPTH_NONE           2

// The lights' clusters: a camera's viewport cut into FBE_CLUSTERS_ACROSS by FBE_CLUSTERS_UP, and
// its view from its near plane to its far one into FBE_CLUSTERS_DEEP slices, each deeper one
// longer by the same ratio for a perspective camera, all as long for an orthographic one.
#define FBE_CLUSTERS_ACROSS 16
#define FBE_CLUSTERS_UP     9
#define FBE_CLUSTERS_DEEP   24
#define FBE_CLUSTERS        3456

// The most lights an FBE_SHADING_VERTEX draw is lit by: the first eight in the tree's order.
#define FBE_DRAW_LIGHTS 8

// NOLINTBEGIN(modernize-use-using)
// One thing a camera draws: the entity, and its FBE_ENTITY_ kind; for a mesh, its geometry and
// the surface of it, else 0 and -1 (a terrain is drawn from its own heights); the material it's
// drawn in, 0 for the default, and the FBE_BLEND_ it's drawn with; its matrix to the world, column
// by column, tweened, or for a sprite the quad's as the camera sees it; its color and alpha, its
// own times its fade; how far in front of the camera its place is, or a terrain's grid's middle;
// its FBE_DEPTH_; and its pipeline, which draws that share one share: bits 0 and 1 its material's
// FBE_SHADING_, 2 to 4 its FBE_BLEND_, 5 whether its material has an alpha cutoff, 6 whether it
// takes vertex colors, 7 whether it's two-sided, 8 and 9 the camera's FBE_FOG_, or none when the
// material isn't fogged, 10 and 11 its FBE_DEPTH_, and 12 whether its matrix mirrors it, its
// determinant below 0, which turns its triangles' winding around, 13 to 16 how many of its
// material's layers have textures, and from 17 those layers', four bits each from the lowest, the
// FBE_LAYER_ blend and over it the UV set, 49 whether it's wireframe, 50 whether it's an emitter's
// particles, 51 whether it's an Instances', and from 52 the number of its material's shader of its
// own's code, 0 for the scene's own; the default material is a new one's, but for a sprite or an
// emitter, which is full-bright with none. A target's format and samples aren't in it, since a
// camera's draws all go into one target, and they're in the pipeline a pass makes of it. An
// FBE_SHADING_VERTEX draw has its own lights, `light_count` of fbe_render_draw_lights' indices from
// `light_first`; any other, 0 of them, as it's lit by its clusters.
typedef struct fbe_draw_t {
    uint64_t entity;
    int32_t kind;
    int32_t surface;
    uint64_t geometry;
    uint64_t material;
    int32_t blend;
    float matrix[16];
    float color[4];
    float depth;
    int32_t depth_mode;
    uint64_t pipeline;
    uint32_t light_first;
    uint32_t light_count;
} fbe_draw_t;

// A light as drawing lights with it: the entity, and its FBE_LIGHT_ type; its color times its
// intensity, linear; where it is and the way it shines, in the camera's view, x right, y up,
// looking down -z, `tween` of the way, as the draws are; its range, where a point or spot light
// ends; and the cosines of a spot light's cone, from its axis to where it starts to fade and to
// where it's out. Per pixel, a distance d from a point or spot light has its color times glTF's
// KHR_lights_punctual falloff, clamp(1 - (d / range)^4, 0, 1)^2 / max(d^2, 0.0001), which ends at
// its range, as clusters need; per vertex, FBE_SHADING_VERTEX, min(range / d, 1), with no end, as
// fixed-function hardware's: each model's range means its own.
typedef struct fbe_draw_light_t {
    uint64_t light;
    int32_t type;
    float color[3];
    float position[3];
    float direction[3];
    float range;
    float inner_cosine;
    float outer_cosine;
    int32_t shadow;
} fbe_draw_light_t;

// A cluster's lights: `count` of the light indices from `first`.
typedef struct fbe_cluster_t {
    uint32_t first;
    uint32_t count;
} fbe_cluster_t;
// NOLINTEND(modernize-use-using)

// What a camera draws in a target of this size, `tween` of the way from its World's last capture
// to now, in the order it draws them: by their materials' orders, lower first. In order 0, the
// scene's, what's opaque first, by pipeline, by geometry, and nearest first, then what blends,
// furthest first; in the others, all furthest first, what's opaque first at one depth. Ties as the
// entities were made, a mesh's surfaces in order. Only the showing meshes, sprites, ground planes,
// and terrains of its World it might see, with some alpha left after their fade; none for a hidden
// camera or one whose viewport has no size, which it sees from where it is `tween` of the way.
// Writes up to `capacity` and gives how many there are, -1 for a freed camera, or a tween outside 0
// to 1.
FBE_API int32_t fbe_render_draws(uint64_t camera, float target_width, float target_height,
                                 float tween, fbe_draw_t* draws, int32_t capacity);

// What lights what a camera draws in a target of this size, `tween` of the way from its World's
// last capture: its World's showing lights with an intensity above 0, the directional ones first,
// then the point and spot lights that reach its view or light one of its FBE_SHADING_VERTEX draws,
// each kind as they were made; and each of its FBE_CLUSTERS clusters' point and spot lights, those
// whose range reaches its part of the view, perhaps with a few that only come near it, by their
// indices in the lights, the clusters across fastest, then down from the top, then out from the
// near plane. Writes up to `light_capacity` lights, `index_capacity` indices, and the clusters,
// unless `clusters` is null, and gives how many lights there are in `light_count` and indices in
// `index_count`: none for a hidden camera or one whose viewport has no size. False for a freed
// camera, or a tween outside 0 to 1.
FBE_API bool fbe_render_lights(uint64_t camera, float target_width, float target_height,
                               float tween, fbe_draw_light_t* lights, int32_t light_capacity,
                               int32_t* light_count, fbe_cluster_t* clusters, uint32_t* indices,
                               int32_t index_capacity, int32_t* index_count);

// The lights of a camera's FBE_SHADING_VERTEX draws, by their indices in fbe_render_lights' lights:
// the first FBE_DRAW_LIGHTS of its World's showing lights, in the tree's order, directional ones
// among them, whatever they reach, which every such draw takes, from its `light_first`. Writes up
// to `capacity` and gives how many there are, -1 for a freed camera, or a tween outside 0 to 1.
FBE_API int32_t fbe_render_draw_lights(uint64_t camera, float target_width, float target_height,
                                       float tween, uint32_t* indices, int32_t capacity);

// Draws a camera into its target, `tween` of the way from its World's last capture, as a pass of
// the frame, starting the GPU if nothing has, unless it's hidden: first it clears what it clears of
// its viewport, then every showing mirror of its World with an inverse reflects the whole scene
// into its viewport and depth, no clip plane, then the scene, which covers that
// unless what's on the mirror's plane lets it through. An order below 0 draws after the
// reflections, so a sky hides them: a mirror's game leaves its sky out of what's opaque. False,
// with fbe_error's why, for a freed camera, a tween outside 0 to 1, a GPU that can't start, or with
// no target and no window.
FBE_API bool fbe_render_camera(uint64_t camera, float tween);
// The most a side of the atlas shadows share can be, 256 to 8192, 4096 at first, up to what the
// device goes to. It's as big as they need up to there, and let go of once they're gone.
FBE_API bool fbe_render_set_shadow_atlas_limit(int32_t side);
FBE_API int32_t fbe_render_shadow_atlas_limit(void);
// Draws every one of a World's cameras, in their order; false as fbe_render_camera is, or for a
// freed World.
FBE_API bool fbe_render_world(uint64_t world, float tween);

#ifdef __cplusplus
}
#endif

#endif
