#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace fitzel {

// A stable, globally-unique asset identity (128 bits, stored as two 64-bit
// halves). This is what scenes, materials and prefabs reference instead of a
// filesystem path -- so moving or renaming an asset on disk never breaks a
// reference. Serialized as a 32-character lowercase hex string in the asset's
// `.meta` sidecar, mirroring Unity's GUID scheme.
struct AssetId {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;

    // A zero id means "no asset" / unset.
    bool valid() const { return hi != 0 || lo != 0; }

    bool operator==(const AssetId& o) const { return hi == o.hi && lo == o.lo; }
    bool operator!=(const AssetId& o) const { return !(*this == o); }
    bool operator<(const AssetId& o) const {
        return hi != o.hi ? hi < o.hi : lo < o.lo;
    }

    // 32 lowercase hex chars (no separators), or all-zero for an invalid id.
    std::string toString() const;

    // Parse a 32-char hex string. Returns an invalid (zero) id on malformed
    // input, so callers can treat "missing"/"garbage" the same way.
    static AssetId fromString(std::string_view hex);

    // Draw a fresh random id. Never returns the zero id.
    static AssetId generate();
};

} // namespace fitzel

// Enable use as an unordered_map key.
template <>
struct std::hash<fitzel::AssetId> {
    std::size_t operator()(const fitzel::AssetId& id) const noexcept {
        // Mix the two halves (boost-style) into one bucket hash.
        std::size_t h = std::hash<std::uint64_t>{}(id.hi);
        h ^= std::hash<std::uint64_t>{}(id.lo) + 0x9e3779b97f4a7c15ULL +
             (h << 6) + (h >> 2);
        return h;
    }
};
