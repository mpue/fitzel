#include "fitzel/asset/AssetId.hpp"

#include <array>
#include <cctype>
#include <random>

namespace fitzel {

namespace {

char hexDigit(std::uint64_t nibble) {
    return "0123456789abcdef"[nibble & 0xF];
}

// Append `v` as 16 hex chars (big-endian nibble order) to `out`.
void appendHex64(std::string& out, std::uint64_t v) {
    for (int shift = 60; shift >= 0; shift -= 4)
        out.push_back(hexDigit(v >> shift));
}

// Decode one hex char to 0..15, or -1 if not a hex digit.
int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

} // namespace

std::string AssetId::toString() const {
    std::string out;
    out.reserve(32);
    appendHex64(out, hi);
    appendHex64(out, lo);
    return out;
}

AssetId AssetId::fromString(std::string_view hex) {
    if (hex.size() != 32) return {};
    std::uint64_t halves[2] = {0, 0};
    for (std::size_t i = 0; i < 32; ++i) {
        const int v = hexValue(hex[i]);
        if (v < 0) return {}; // malformed -> treat as invalid
        halves[i / 16] = (halves[i / 16] << 4) | static_cast<std::uint64_t>(v);
    }
    return AssetId{halves[0], halves[1]};
}

AssetId AssetId::generate() {
    // Seed a 64-bit Mersenne Twister from the platform entropy source. A single
    // random_device draw can be low-entropy on some libstdc++ builds, so mix a
    // few draws into the seed sequence.
    static thread_local std::mt19937_64 rng([] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        std::array<std::uint64_t, 2> seed{};
        seq.generate(reinterpret_cast<std::uint32_t*>(seed.data()),
                     reinterpret_cast<std::uint32_t*>(seed.data() + seed.size()));
        return std::mt19937_64(seed[0] ^ seed[1]);
    }());

    AssetId id;
    do {
        id.hi = rng();
        id.lo = rng();
    } while (!id.valid()); // astronomically unlikely, but never hand out zero
    return id;
}

} // namespace fitzel
