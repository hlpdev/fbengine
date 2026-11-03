#ifndef FBENGINE_H
#define FBENGINE_H

// fbengine's whole C API, each area's header in its folder, which packages/fbengine wraps through
// the bindings tools/bindgen.sh makes of each, one file per area in packages/fbengine/c/.

#include "2d/fbe_2d.h"
#include "app/fbe_app.h"
#include "assets/fbe_assets.h"
#include "clip/fbe_clip.h"
#include "collide/fbe_collide.h"
#include "core/fbe_core.h"
#include "frame/fbe_frame.h"
#include "gpu/fbe_gpu.h"
#include "input/fbe_input.h"
#include "jobs/fbe_jobs.h"
#include "model/fbe_model.h"
#include "player/fbe_player.h"
#include "render/fbe_render.h"
#include "scene/fbe_scene.h"
#include "skin/fbe_skin.h"
#include "text/fbe_text.h"
#include "texture/fbe_texture.h"
#include "vfs/fbe_vfs.h"
#include "window/fbe_window.h"

#endif
