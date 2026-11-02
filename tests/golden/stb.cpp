// stb's PNG reading and writing, compiled once for the golden image harness.

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#include <stb_image_write.h>
