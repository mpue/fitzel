// Compiles the miniaudio implementation.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#if defined(__EMSCRIPTEN__)
// miniaudio's Web Audio glue calls this from JavaScript, as
// Module._ma_device__on_notification_unlocked. Compiled as C++ (this file) its
// name would be mangled and the call would find nothing -- so it is declared
// with C linkage first, which the definition inside the implementation keeps.
#include <miniaudio.h>
extern "C" void ma_device__on_notification_unlocked(ma_device* pDevice);
#endif
// Ogg Vorbis: miniaudio decodes it only with stb_vorbis declared before its
// implementation (and defined after it). Without it every .ogg failed to load
// with MA_INVALID_FILE -- the sounds of an imported game among them.
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
