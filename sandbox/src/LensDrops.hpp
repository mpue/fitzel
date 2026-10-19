#pragma once
// Liquid on the camera lens: splashes, drops that run down the glass, the film
// they leave that dries to a stain. Not pictures laid over the frame -- a lens:
// the image behind every drop is refracted through it and goes out of focus,
// thick liquid absorbs (blood: deep red, a thin film light and see-through),
// the wet surface catches a highlight and a dark rim, and what has dried only
// tints and hazes.
//
// State lives in two half-resolution targets: the FILM (R wet thickness, G dried
// amount), kept from frame to frame and slowly drying (ping-pong), and the HEADS
// of the drops still running, drawn fresh every frame. Splashes are stamped into
// the film from a texture (its alpha is the thickness); a running drop writes its
// trail into the film as it goes. The drops themselves are a small CPU simulation
// (they run, catch, run on, and stop when spent).
//
// Driven by scripts (game.lensSplash / game.lensClear); without anything on the
// lens the pass is skipped and costs nothing.
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/RenderTarget.hpp>
#include <fitzel/graphics/Shader.hpp>

namespace fitzel { class AssetDatabase; class Mesh; class Texture; }
class PostChain;

struct LensSplash {
    glm::vec2   pos{0.5f};          // centre, 0..1 from the top left of the view
    float       size   = 0.3f;      // height, as a share of the view's height
    float       aspect = 1.0f;      // width / height of the stamp
    float       angle  = 0.0f;      // degrees
    float       amount = 1.0f;      // thickness the stamp's full alpha gives
    std::string image;              // texture asset (file name); empty = a round blob
    bool        dry    = false;     // straight into the dried layer (dirt), no drops
    int         drops  = -1;        // drops that run from it; -1 = by its size
    bool        round  = false;     // no image: a bead (a hemisphere), not a soft blob
};

class LensDrops {
public:
    bool init();

    void splash(const LensSplash& s);
    void clear();
    // The liquid's colour: how strongly it absorbs each channel per unit of
    // thickness (blood by default), and what it is tinted when dry.
    void setAbsorb(const glm::vec3& a) { m_absorb = a; }
    void setDryTint(const glm::vec3& t) { m_dryTint = t; }
    // How fast the wet film thins (seconds to 1/e) and what share of what goes
    // stays as a stain. Blood by default; water barely leaves anything.
    void setDrying(float wetTau, float dryGain) { m_wetTau = wetTau; m_dryGain = dryGain; }
    // A running drop gathers up what stands in its way (a clear lane behind it,
    // as on a shower screen) instead of only adding its streak.
    void setSweep(bool on) { m_sweep = on; }
    // A wiper blade went over the lens: the sector between angles a0 and a1
    // (radians, 0 straight up, positive to the right) round `pivot` (0..1, y
    // down), from radius r0 to r1 (shares of the height), is wiped -- the wet
    // gone, the dried mostly, the running drops in it with them. Call it every
    // frame with the blade's last and present angle.
    void wipe(float a0, float a1, glm::vec2 pivot, float r0, float r1);
    // One drop that runs from `pos` (0..1, y down), radius `r` and how far it
    // can run (`mass`), both in shares of the height.
    void drop(glm::vec2 pos, float r, float mass);
    bool active() const { return m_active; }

    // Once per frame, before the panes: run the drops, dry the film, stamp what
    // came in. `w`, `h`: the post chain's image size.
    void step(float dt, int w, int h, fitzel::Mesh& fsQuad, fitzel::AssetDatabase& assets);
    // Per pane, after PostChain::run: the lens over its result (which becomes the
    // chain's result for present()).
    void apply(PostChain& post, fitzel::Mesh& fsQuad);
    // Not a lens but a surface (GlassDrops): the film and the running drops'
    // heads together, into the framebuffer that is bound (the film's size):
    // R wet thickness, G dried.
    void compose(fitzel::Mesh& fsQuad);
    int filmWidth() const  { return m_film[m_cur].width(); }
    int filmHeight() const { return m_film[m_cur].height(); }

private:
    struct Drop {
        glm::vec2 pos;     // 0..1, y DOWN the view
        float     r;       // radius, share of the view's height
        float     mass;    // how far it can still run (share of height)
        float     pause;   // seconds caught before it runs on
        bool      still = false;
        float     stillT = 0.0f;
    };

    void resize(int w, int h);
    std::shared_ptr<fitzel::Texture> stamp(const std::string& name, fitzel::AssetDatabase& assets);
    // One quad into the bound target: centre / half size in NDC, rotation, and
    // what it writes (0 texture alpha, 1 a hemisphere, 2 a streak across x).
    void quad(fitzel::Mesh& fsQuad, glm::vec2 centerUv, glm::vec2 halfUv, float angleRad,
              int mode, float amount, glm::vec2 channel);

    fitzel::Shader m_stampSh, m_drySh, m_lensSh, m_wipeSh;
    struct Wipe { float a0, a1; glm::vec2 pivot; float r0, r1; };
    std::vector<Wipe> m_wipes;           // this frame's, into the film
    fitzel::RenderTarget m_film[2]{ {1, 1, fitzel::RenderTarget::Format::RGBA16F},
                                    {1, 1, fitzel::RenderTarget::Format::RGBA16F} };
    fitzel::RenderTarget m_heads{1, 1, fitzel::RenderTarget::Format::RGBA16F};
    fitzel::RenderTarget m_out{1, 1, fitzel::RenderTarget::Format::RGBA8};
    int  m_cur = 0;
    int  m_w = 0, m_h = 0;           // the image's size (the film is half)
    bool m_active = false;
    bool m_clearFilm = true;

    std::vector<LensSplash> m_pending;
    std::vector<Drop>       m_drops;
    struct Trail { glm::vec2 from, to; float r; };
    std::vector<Trail>      m_trails;     // this frame's, into the film
    std::unordered_map<std::string, std::shared_ptr<fitzel::Texture>> m_stamps;
    std::mt19937 m_rng{1234u};

    float     m_wetTau  = 11.0f;   // s: the wet film's thickness falls to 1/e
    float     m_dryGain = 0.55f;   // what of the liquid that dries stays as a stain
    bool      m_sweep   = false;
    glm::vec3 m_absorb{0.45f, 3.4f, 3.8f};
    glm::vec3 m_dryTint{0.50f, 0.30f, 0.24f};
};
