#ifndef FBE_COOK_H
#define FBE_COOK_H

// The cooker: a game's source files made into what a release loads fastest, each file into one
// of the same name, since the engine knows a file's format by its bytes. Images become KTX2s of
// GPU blocks with their mips; glTFs become binary glTFs of meshes meshoptimizer ordered. Paths are
// UTF-8, on the disk.

#include "../tools/fbe_tools.h"

#ifdef __cplusplus
extern "C" {
#endif

// C's constants and structs, which C++'s enums and usings aren't.
// NOLINTBEGIN(modernize-macro-to-enum, modernize-use-using)

// What an image is cooked to: Basis Universal's UASTC, transcoded as it loads to BC7 or ASTC 4x4,
// whichever the GPU reads; BC7, for desktop GPUs; ASTC 4x4, for Apple's and phones'; or its texels
// as they are; each Zstandard-compressed in the KTX2. Not ETC1S, whose BasisLZ the engine doesn't
// load, since libktx's transcoder of it breaks on files it's given.
#define FBE_COOK_UASTC 0
#define FBE_COOK_BC7   1
#define FBE_COOK_ASTC  2
#define FBE_COOK_RGBA8 3

// How an image is cooked: to `format`; its color sRGB, as a color texture's is, or linear, as a
// normal map's is; with its mips down to 1x1, each made in linear light; and how hard the
// encoder works, 0 (fastest) to 4 (best).
typedef struct fbe_cook_image_t {
    int32_t format;
    bool srgb;
    bool mipmaps;
    int32_t quality;
} fbe_cook_image_t;

// NOLINTEND(modernize-macro-to-enum, modernize-use-using)

// Cooks the image at `from`, a PNG or a JPEG, into the KTX2 at `to`; false if it can't, as
// fbe_tools_error says.
FBE_TOOLS_API bool fbe_cook_image(const char* from, const char* to,
                                  const fbe_cook_image_t* options);

#ifdef __cplusplus
}
#endif

#endif
