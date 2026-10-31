#ifndef FBE_CORE_H
#define FBE_CORE_H

// What every area of fbengine's C API shares: how its functions are exported, and its errors.
// Each area's header (fbe_<area>.h) has its own binding file in packages/fbengine/c/, which
// tools/bindgen.sh makes, so an area's header uses only fixed-size types and its own structs,
// never another area's. Everything but fbe_version and fbe_error is the main thread's, which the
// FB API's @MainThread keeps it to.

#include <stdbool.h>
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

// What the last call that failed on this thread says went wrong.
FBE_API const char* fbe_error(void);

#ifdef __cplusplus
}
#endif

#endif
