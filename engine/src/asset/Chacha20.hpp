#pragma once

#include <cstddef>
#include <cstdint>

// ChaCha20 (RFC 8439) as a bare keystream generator -- the cipher behind the
// .fpak archives. A stream cipher is the right shape here: an archive entry is
// encrypted and decrypted as one linear run of bytes, no padding, no block
// alignment, ciphertext exactly as long as the plaintext, so offsets in the
// index mean the same thing before and after.
//
// No authentication tag. The archive protects content from being copied out of
// a folder, not from being tampered with by someone who already owns the file
// -- and a MAC over multi-hundred-megabyte assets would cost load time for a
// guarantee nobody is asking for here.
namespace fitzel::crypt {

class ChaCha20 {
public:
    // `nonce` must be unique per key. The archive uses the entry's own nonce,
    // stored alongside its offset in the index.
    ChaCha20(const std::uint8_t key[32], std::uint64_t nonce) {
        static const char kSigma[] = "expand 32-byte k";
        for (int i = 0; i < 4; ++i)
            m_state[i] = word(reinterpret_cast<const std::uint8_t*>(kSigma) + i * 4);
        for (int i = 0; i < 8; ++i) m_state[4 + i] = word(key + i * 4);
        m_state[12] = 0; // block counter
        m_state[13] = 0; // nonce, 96 bits: 32 unused + the 64-bit entry nonce
        m_state[14] = static_cast<std::uint32_t>(nonce & 0xFFFFFFFFu);
        m_state[15] = static_cast<std::uint32_t>(nonce >> 32);
    }

    // XOR the keystream over `data` in place -- encrypt and decrypt are the same
    // operation. Call repeatedly to walk a large file in chunks: the counter and
    // the position inside the current block carry over between calls.
    void apply(std::uint8_t* data, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            if (m_used == 64) { nextBlock(); m_used = 0; }
            data[i] ^= m_block[m_used++];
        }
    }

private:
    static std::uint32_t word(const std::uint8_t* p) {
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[3]) << 24);
    }
    static std::uint32_t rotl(std::uint32_t x, int n) {
        return (x << n) | (x >> (32 - n));
    }
    static void quarter(std::uint32_t& a, std::uint32_t& b,
                        std::uint32_t& c, std::uint32_t& d) {
        a += b; d ^= a; d = rotl(d, 16);
        c += d; b ^= c; b = rotl(b, 12);
        a += b; d ^= a; d = rotl(d, 8);
        c += d; b ^= c; b = rotl(b, 7);
    }

    void nextBlock() {
        std::uint32_t x[16];
        for (int i = 0; i < 16; ++i) x[i] = m_state[i];
        for (int r = 0; r < 10; ++r) {          // 10 double rounds = 20 rounds
            quarter(x[0], x[4], x[8],  x[12]);
            quarter(x[1], x[5], x[9],  x[13]);
            quarter(x[2], x[6], x[10], x[14]);
            quarter(x[3], x[7], x[11], x[15]);
            quarter(x[0], x[5], x[10], x[15]);
            quarter(x[1], x[6], x[11], x[12]);
            quarter(x[2], x[7], x[8],  x[13]);
            quarter(x[3], x[4], x[9],  x[14]);
        }
        for (int i = 0; i < 16; ++i) {
            const std::uint32_t v = x[i] + m_state[i];
            m_block[i * 4 + 0] = static_cast<std::uint8_t>(v);
            m_block[i * 4 + 1] = static_cast<std::uint8_t>(v >> 8);
            m_block[i * 4 + 2] = static_cast<std::uint8_t>(v >> 16);
            m_block[i * 4 + 3] = static_cast<std::uint8_t>(v >> 24);
        }
        ++m_state[12];
    }

    std::uint32_t m_state[16]{};
    std::uint8_t  m_block[64]{};
    std::size_t   m_used = 64; // force a block on first use
};

} // namespace fitzel::crypt
