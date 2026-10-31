#ifndef FBE_TOOLS_H
#define FBE_TOOLS_H

// fbengine_tools' DLL, which the cooker and the build's other tools call, and what every area of
// its C API shares: how its functions are exported, and its errors. Each area's header
// (fbe_<area>.h) has its own binding file in packages/fbengine_tools/c/, which tools/bindgen.sh
// makes, so an area's header uses only fixed-size types and its own structs, never another
// area's. A program loads this DLL beside fbengine_native, so no function here is named as one of
// fbengine's is.

#include <stdbool.h>
#include <stdint.h>

#ifndef _WIN32
#define FBE_TOOLS_API __attribute__((visibility("default")))
#else
#ifdef FBE_TOOLS_BUILDING
#define FBE_TOOLS_API __declspec(dllexport)
#else
#define FBE_TOOLS_API __declspec(dllimport)
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The version of fbengine_tools this DLL is, like "0.1.0", which is fbengine's.
FBE_TOOLS_API const char* fbe_tools_version(void);

// How a call failed, as std.io's IOErrorKind has it.
// NOLINTBEGIN(modernize-macro-to-enum)
#define FBE_TOOLS_NOT_FOUND    0
#define FBE_TOOLS_INVALID_DATA 4
#define FBE_TOOLS_OTHER        5
// NOLINTEND(modernize-macro-to-enum)

// What the last call that failed on this thread says went wrong, and how it failed,
// FBE_TOOLS_INVALID_DATA for what it was given that isn't what it takes, and FBE_TOOLS_OTHER for
// the rest, as files that can't be read or written.
FBE_TOOLS_API const char* fbe_tools_error(void);
FBE_TOOLS_API int32_t fbe_tools_failure(void);

#ifdef __cplusplus
}
#endif

#endif
