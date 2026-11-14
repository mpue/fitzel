#pragma once

#include <cstdint>
#include <string>
#include <vector>

// QR codes (ISO/IEC 18004): text in, the square of dark and light modules out.
//
// Every version from 1 (21 x 21) to 40 (177 x 177), the four error correction
// levels, the three ways a text is packed -- digits, the 45 capitals and signs
// of alphanumeric mode, bytes (UTF-8, as phones read it) -- whichever is
// shortest for the whole text. The smallest version the text fits in is taken,
// and of the eight masks the one the standard's penalty rules score lowest.
//
// Pure: no fitzel, no GL. What the modules become is QrCode.hpp's.
namespace qrencode {

// How much of the code may be lost and still be read. Saved as an index:
// append only.
enum class Ecc { Low, Medium, Quartile, High, Count };
const char* eccName(Ecc e);      // "L (7 %)" ...
char        eccLetter(Ecc e);    // 'L' 'M' 'Q' 'H'

// How the text was packed.
enum class Mode { Numeric, Alphanumeric, Byte };
const char* modeName(Mode m);

struct Code {
    int  version = 0;     // 1..40; 0 = nothing (the text did not fit)
    int  size    = 0;     // modules per side, 17 + 4 * version
    Ecc  ecc     = Ecc::Medium;
    int  mask    = 0;     // 0..7
    Mode mode    = Mode::Byte;
    int  dataBytes = 0;   // data codewords the text and its header filled ...
    int  capacity  = 0;   // ... of this many at this version and level
    std::vector<std::uint8_t> dark;   // size * size, row by row, 1 = dark

    bool valid() const { return version > 0; }
    // Row `y` from the top, column `x` from the left. Outside is light.
    bool at(int x, int y) const {
        return x >= 0 && y >= 0 && x < size && y < size &&
               dark[static_cast<std::size_t>(y * size + x)] != 0;
    }
};

// Encode `utf8`. `minVersion` keeps the code at least that big; `mask` -1
// picks the best one, 0..7 forces it. Returns an invalid Code when the text is
// longer than version 40 holds at this level.
Code encode(const std::string& utf8, Ecc ecc, int minVersion = 1, int mask = -1);

// The most bytes of text (byte mode) a version holds at a level.
int byteCapacity(int version, Ecc ecc);

// --- The pieces, public for the check -------------------------------------------

int  dataCodewords(int version, Ecc ecc);   // data codewords of a version
int  rawCodewords(int version);             // data + error correction
// How the codewords are split: this many blocks, each with this many error
// correction codewords (the last raw % blocks blocks one data codeword longer).
void blockLayout(int version, Ecc ecc, int& blocks, int& eccPerBlock);
std::vector<int> alignmentPositions(int version);
std::uint32_t formatBits(Ecc ecc, int mask);   // the 15 bits, masked
std::uint32_t versionBits(int version);        // the 18 bits (version >= 7)
// The Reed-Solomon remainder of `data` for `degree` error correction codewords.
std::vector<std::uint8_t> reedSolomon(const std::vector<std::uint8_t>& data, int degree);
std::uint8_t gfMul(std::uint8_t a, std::uint8_t b);   // GF(256), x^8+x^4+x^3+x^2+1
// The data codewords before error correction (header, text, padding).
std::vector<std::uint8_t> dataCodewordsOf(const std::string& utf8, Ecc ecc, int version, Mode& mode);
// The four penalty rules' score of a finished symbol.
int penalty(const Code& c);

} // namespace qrencode
