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
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
