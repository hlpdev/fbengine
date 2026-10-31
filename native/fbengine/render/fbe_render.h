#ifndef FBE_RENDER_H
#define FBE_RENDER_H

// Drawing the scene: what each camera draws, read from the scene into a list once a frame, so
// drawing never reads the live scene, and a render thread could take drawing over without the API
// changing.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// NOLINTBEGIN(modernize-use-using)
// One thing a camera draws: the entity, and its FBE_ENTITY_ kind; for a mesh, its geometry and
// the surface of it, else 0 and -1 (a terrain is drawn from its own heights); the material it's
// drawn in, 0 for the default, and the FBE_BLEND_ it's drawn with; its matrix to the world, column
// by column, tweened, or for a sprite the quad's as the camera sees it; its color and alpha, its
// own times its fade; and how far in front of the camera its place is, or a terrain's grid's
// middle.
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
} fbe_draw_t;
// NOLINTEND(modernize-use-using)

// What a camera draws in a target of this size, `tween` of the way from its World's last capture
// to now, in the order it draws them: by their materials' orders, lower first; in an order, what's
// opaque first, by material, nearest first, then what blends, furthest first; ties as the entities
// were made, a mesh's surfaces in order. Only the showing meshes, sprites, ground planes, and
// terrains of its World it might see, with some alpha left after their fade; none for a hidden
// camera or one whose viewport has no size, which it sees from where it is `tween` of the way.
// Writes up to `capacity` and gives how many there are, -1 for a freed camera, or a tween outside
// 0 to 1.
FBE_API int32_t fbe_render_draws(uint64_t camera, float target_width, float target_height,
                                 float tween, fbe_draw_t* draws, int32_t capacity);

#ifdef __cplusplus
}
#endif

#endif
