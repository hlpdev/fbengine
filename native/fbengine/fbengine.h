#ifndef FBENGINE_H
#define FBENGINE_H

// fbengine's whole C API, each area's header in its folder, which packages/fbengine wraps through
// the bindings tools/bindgen.sh makes of each, one file per area in packages/fbengine/c/.

#include "2d/fbe_2d.h"
#include "core/fbe_core.h"
#include "frame/fbe_frame.h"
#include "gpu/fbe_gpu.h"
#include "texture/fbe_texture.h"
#include "window/fbe_window.h"

#endif
