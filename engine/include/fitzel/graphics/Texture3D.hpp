#pragma once

#include <cstdint>
#include <vector>

namespace fitzel {

// A floating-point 3D texture (RGBA16F), filtered and clamped.
//
// It exists for volumes the shader has to LOOK UP BY WORLD POSITION rather than
// by a surface coordinate -- the baked light grid is the first, and the reason
// the filtering matters: a probe grid is deliberately coarse, and what makes it
// read as light rather than as a lattice is the hardware interpolating between
// the eight probes around a point for free. Doing that by hand in the shader is
// eight fetches and a fistful of arithmetic for a worse answer.
//
// Half floats rather than 8-bit because the values are radiance: a sky-lit
// courtyard and the inside of a tunnel are three orders of magnitude apart, and
// a normalised format would spend all its precision on neither.
class Texture3D {
public:
    Texture3D() = default;
    ~Texture3D();

    Texture3D(const Texture3D&)            = delete;
    Texture3D& operator=(const Texture3D&) = delete;
    Texture3D(Texture3D&& other) noexcept;
    Texture3D& operator=(Texture3D&& other) noexcept;

    // Allocate and fill. `rgba` is width*height*depth*4 floats, x fastest, then
    // y, then z. An empty or mis-sized vector allocates the volume and leaves it
    // black rather than reading past the end.
    static Texture3D create(int width, int height, int depth,
                            const std::vector<float>& rgba);

    // Overwrite the contents, keeping the GL name so anything already bound to
    // it sees the new volume. The size must match; a mismatch is ignored.
    bool update(const std::vector<float>& rgba);

    bool isValid() const { return m_id != 0; }
    void bind(std::uint32_t unit) const;

    int width()  const { return m_width; }
    int height() const { return m_height; }
    int depth()  const { return m_depth; }

private:
    std::uint32_t m_id = 0;
    int m_width = 0, m_height = 0, m_depth = 0;
};

} // namespace fitzel
