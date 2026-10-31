// stb_image's code, for PNGs and JPEGs only, whose failures it keeps per thread.

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
