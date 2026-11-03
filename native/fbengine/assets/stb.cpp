// stb_image's code, for PNGs and JPEGs only, whose failures it keeps per thread.

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
// Under MinGW's emutls a thread_local is what a worker can be stopped registering as a process
// ends. Its failure text is a static string, read right after a failure, so a race is harmless.
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
