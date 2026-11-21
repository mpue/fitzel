#pragma once

#include <string>
#include <vector>

// Plain base64 (RFC 4648, padded), for the binary grids a scene file carries as
// text: a real place's measured ground (Component.cpp) and its woods (main.cpp).
namespace b64 {

inline std::string encode(const std::vector<unsigned char>& in) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned>(in[i]) << 16) |
                           (i + 1 < in.size() ? static_cast<unsigned>(in[i + 1]) << 8 : 0u) |
                           (i + 2 < in.size() ? static_cast<unsigned>(in[i + 2]) : 0u);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < in.size() ? k[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? k[v & 63] : '=';
    }
    return out;
}

inline std::vector<unsigned char> decode(const std::string& s) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int map[256];
    for (int& m : map) m = -1;
    for (int i = 0; i < 64; ++i) map[static_cast<unsigned char>(k[i])] = i;
    std::vector<unsigned char> out;
    out.reserve(s.size() / 4 * 3);
    unsigned v = 0;
    int bits = 0;
    for (char c : s) {
        const int d = map[static_cast<unsigned char>(c)];
        if (d < 0) continue;
        v = (v << 6) | static_cast<unsigned>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((v >> bits) & 255));
        }
    }
    return out;
}

} // namespace b64
