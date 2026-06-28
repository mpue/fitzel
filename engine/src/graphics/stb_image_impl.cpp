// Single translation unit that compiles the stb implementations we use.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_DEPRECATION
#include <stb_image.h>

#define STB_PERLIN_IMPLEMENTATION
#include <stb_perlin.h>
