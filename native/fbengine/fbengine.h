#ifndef FBENGINE_H
#define FBENGINE_H

// fbengine's C API, which packages/fbengine wraps, through the bindings fb-bindgen makes of this
// header. Every type here is the same size on every target, so the bindings are too.

#include <stdint.h>

#ifndef _WIN32
#define FBE_API __attribute__((visibility("default")))
#else
#ifdef FBE_BUILDING
#define FBE_API __declspec(dllexport)
#else
#define FBE_API __declspec(dllimport)
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The version of fbengine this DLL is, like "0.1.0", which is its packages'.
FBE_API const char* fbe_version(void);

#ifdef __cplusplus
}
#endif

#endif
