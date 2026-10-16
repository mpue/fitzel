#pragma once

#include <cstdint>
#include <cstring>

// The archive key, kept out of a plain `strings` dump of the binary.
//
// The bytes below are the real key XOR'd with a simple positional mask, and the
// key is only ever assembled into a caller-owned local. That is deliberately
// modest: anyone who disassembles the player finds it. The point is that the key
// is not lying in the executable as a readable 32-byte run for a grep to spot --
// the step from "open the folder" to "attach a debugger" is the whole game here.
//
// Editor and player build this from the same source, so an archive packed by the
// editor opens in the player it was exported next to. Change the blob and every
// previously exported .fpak stops opening -- re-export, don't patch.
//
// The blob below is in the public source, so anyone with the source can open
// an archive sealed with it. A private build sets its own key without touching
// this file:
//   cmake ... -DFITZEL_PAK_KEY_HEX=<64 hex chars>
// (engine/CMakeLists.txt checks it and hands it to the compiler; the editor's
// export says when the default is still in use -- pak::usesDefaultKey).
namespace fitzel::pak {

namespace keydetail {
constexpr int nib(char c) {
    return (c >= '0' && c <= '9') ? c - '0'
         : (c >= 'a' && c <= 'f') ? c - 'a' + 10
         : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                  : -1;
}
// Exactly 64 hex digits: a typo must stop the build, not quietly become zero
// bytes in the key (which it did -- an invalid digit read as 0).
constexpr bool validHex(const char* s) {
    int n = 0;
    for (; s[n]; ++n)
        if (nib(s[n]) < 0) return false;
    return n == 64;
}
} // namespace keydetail

#ifdef FITZEL_PAK_KEY_HEX
static_assert(keydetail::validHex(FITZEL_PAK_KEY_HEX),
              "FITZEL_PAK_KEY_HEX must be exactly 64 hexadecimal characters");
#endif

inline void archiveKey(std::uint8_t out[32]) {
#ifdef FITZEL_PAK_KEY_HEX
    const char* hex = FITZEL_PAK_KEY_HEX;
    for (int i = 0; i < 32; ++i)
        out[i] = static_cast<std::uint8_t>((keydetail::nib(hex[i * 2]) << 4) |
                                           keydetail::nib(hex[i * 2 + 1]));
    return;
#endif
    static const std::uint8_t kBlob[32] = {
        0x69, 0x0a, 0xe4, 0x24, 0x07, 0xda, 0xc7, 0xed,
        0xc1, 0xdb, 0x2e, 0xb1, 0x4d, 0x05, 0xc8, 0x72,
        0xa3, 0xc2, 0x6e, 0x91, 0xd8, 0xd8, 0xaf, 0x62,
        0xae, 0xe2, 0x72, 0xcd, 0x1f, 0xab, 0x33, 0xdf,
    };
    for (int i = 0; i < 32; ++i)
        out[i] = static_cast<std::uint8_t>(kBlob[i] ^ (0x5A + i * 37));
}

} // namespace fitzel::pak
