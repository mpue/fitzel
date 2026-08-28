#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fitzel/graphics/Shader.hpp"

namespace fitzel {

// Image-based lighting from an equirectangular HDR environment (.hdr / .exr):
// loads the panorama into a radiance cubemap, then precomputes a diffuse
// irradiance cubemap and a roughness-mipped specular prefilter cubemap. The lit
// shader samples the irradiance (diffuse ambient) and the prefilter (specular),
// and the sky pass can draw the environment cubemap as the background.
//
// Owns GL resources -- create after a GL context exists. Non-copyable.
class EnvironmentIBL {
public:
    EnvironmentIBL();
    ~EnvironmentIBL();
    EnvironmentIBL(const EnvironmentIBL&)            = delete;
    EnvironmentIBL& operator=(const EnvironmentIBL&) = delete;

    // The panorama on the CPU, decoded and rescaled exactly as load() does it.
    //
    // Exposed because a second consumer exists -- the offline path tracer lights
    // from the same sky the raster path does -- and the one thing that must not
    // be duplicated is the NORMALISATION. It is not a plain decode: the mean
    // luminance is measured and the whole image rescaled to a common brightness
    // (see normalizeEquirect), so a renderer that merely re-read the file would
    // light its scene at a different exposure than the viewport and there would
    // be nothing in either to say which was right.
    //
    // No GL, so this is safe off the render thread. Empty pixels = load failed.
    struct Panorama {
        std::vector<float> pixels;  // equirectangular RGB, row 0 = up
        int   width = 0, height = 0;
        float exposureScale = 1.0f; // the factor the rescale applied
        bool  valid() const { return !pixels.empty() && width > 0 && height > 0; }
    };
    static Panorama loadPanorama(const std::string& path);

    // Load an equirectangular HDR file and build the env / irradiance /
    // prefilter cubemaps. Returns false (and leaves valid() false) on failure.
    bool load(const std::string& equirectPath);
    bool valid() const { return m_valid; }

    void bindIrradiance(std::uint32_t unit) const; // diffuse ambient
    void bindPrefilter(std::uint32_t unit) const;  // specular (mipped by roughness)
    void bindEnvCube(std::uint32_t unit) const;    // raw radiance, for the skybox

    int prefilterMipLevels() const { return m_prefilterMips; }

    // Factor the loaded panorama was rescaled by to reach a common brightness
    // (see normalizeEquirect). 1.0 when nothing is loaded.
    float exposureScale() const { return m_exposureScale; }

private:
    void buildCubeVao();
    void renderCube() const;      // draws the unit cube (36 verts)
    void deleteMaps();

    Shader m_toCube;      // equirectangular -> cubemap
    Shader m_irradiance;  // diffuse convolution
    Shader m_prefilterSh; // GGX specular prefilter

    std::uint32_t m_cubeVao = 0, m_cubeVbo = 0;
    std::uint32_t m_captureFbo = 0, m_captureRbo = 0;

    std::uint32_t m_envCube    = 0; // radiance
    std::uint32_t m_irradMap   = 0; // diffuse
    std::uint32_t m_prefilter  = 0; // specular
    int           m_prefilterMips = 5;
    float         m_exposureScale = 1.0f;
    bool          m_valid = false;
};

} // namespace fitzel
