// Single translation unit that compiles the stb implementations we use.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_DEPRECATION
// Make stb_image's "flip vertically on load" state thread-local, so decoding
// asset thumbnails on worker threads can't race the main thread's texture loads
// on the flip flag. Use stbi_set_flip_vertically_on_load_thread() off-thread.
#define STBI_THREAD_LOCAL thread_local
#include <stb_image.h>

#define STB_PERLIN_IMPLEMENTATION
#include <stb_perlin.h>
