// Single translation unit that compiles the stb implementations we use.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_DEPRECATION
// Make stb_image's "flip vertically on load" state thread-local, so decoding
// asset thumbnails on worker threads can't race the main thread's texture loads
// on the flip flag.
//
// THE RULE, everywhere in the engine and the editor: set the flag with
// stbi_set_flip_vertically_on_load_THREAD() right before a load, and put it
// back to 0 after a load that set it to 1. Never the plain global setter: once
// a thread has called the _thread one, stb ignores the global on that thread
// for good -- which is how a thumbnail decoded on the render thread left every
// later flipped texture and HDR panorama there upside down. And loads that set
// nothing (an icon, a model's external image) rely on 0 being the resting value.
#define STBI_THREAD_LOCAL thread_local
#include <stb_image.h>

#define STB_PERLIN_IMPLEMENTATION
#include <stb_perlin.h>
