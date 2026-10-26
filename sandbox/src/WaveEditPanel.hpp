#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include "WaveDoc.hpp"

namespace fitzel { class Audio; class SamplePlayer; }

// The editor's "Wave editor" window (Assets menu, or "Edit in Wave editor" on a
// sound in the Assets panel): cut, fade, normalise and loop the sounds a project
// is made of, and hear every change at once.
//
//   * The waveform per channel -- peak outline over an RMS body, down to the
//     single samples when zoomed in -- under a time ruler, with an overview of
//     the whole sound above it that is also the scroll bar.
//   * Preview through the engine (SamplePlayer): from the cursor, the selection,
//     or round the loop exactly as a looping Sound plays it in the game; loop
//     points moved while it plays are heard on the next pass. Level meters,
//     volume and speed.
//   * Loop points as markers, set from the selection or the cursor, snapped to
//     a zero crossing, the end found by matching what follows it to what
//     follows the start, the seam crossfaded away. Saved in the WAV's smpl chunk.
//   * Edits: cut/copy/paste/delete, crop, silence, insert silence, fades with
//     five curves, gain, normalise, reverse, invert, remove DC, trim silence,
//     mono/stereo, resample -- on one channel or both, each one undo step.
//   * Analysis of the selection: peak, RMS, DC, clipping, a spectrum.
//   * Gain, normalise and reverse also in the Process menu and on the waveform's
//     right-click menu; the bar between the waveform and the tools under it is a
//     splitter (dragged: the waveform's height; double-clicked: back as it was).
//
// Several sounds at once, one tab each. Made to be used without a steady hand:
// a click sets the cursor, Shift+click the other end of a selection (dragging
// works too); every amount is a stepper; the loop points have number fields
// beside their markers. While the window has the keyboard it claims it like a
// text field does (io.WantTextInput), so the scene's shortcuts stand aside.
namespace waveui {

class WaveEditor {
public:
    struct Deps {
        const std::string&        currentProject;   // the open .fitzel ("" = none)
        std::string&              status;           // the editor's footer line
        fitzel::Audio*            audio;            // null: no preview
        std::vector<std::string>* drops = nullptr;  // files dropped on the editor this frame
    };
    explicit WaveEditor(Deps d);
    ~WaveEditor();
    WaveEditor(const WaveEditor&)            = delete;
    WaveEditor& operator=(const WaveEditor&) = delete;

    void panel(bool& show);
    // A tab for this file (the one it already has, if it is open). False + a
    // status line when it cannot be read.
    bool open(const std::string& path);
    wave::Document& newDocument(int channels, int rate, double seconds);
    // The window had the keyboard last frame.
    bool hasKeyboard() const;
    // Stop the preview (Play mode starting, a scene loading).
    void stopPreview();

    // For wavecheck.
    void setSelection(wave::Frames a, wave::Frames b);
    void setSnap(bool on) { m_snapZero = on; }
    wave::Document* document();
    wave::Frames cursorFrame() const;
    void selection(wave::Frames& a, wave::Frames& b) const;
    double framesPerPixel() const;
    // Where frame `f` was on screen in the last frame drawn, `lane01` down the
    // first channel's lane (0 top, 1 bottom); the ruler for a negative lane01.
    ImVec2 toScreen(wave::Frames f, float lane01) const;
    float  waveHeight() const { return m_waveSize.y; }   // the wave area last drawn
    float  splitterY() const { return m_splitY; }        // the middle of the splitter bar

private:
    struct View;
    Deps m_deps;
    std::vector<std::unique_ptr<View>> m_views;
    int  m_active    = 0;
    int  m_focusFrame = -100;
    ImVec2 m_waveMin{0, 0}, m_waveSize{0, 0};   // the wave area last drawn
    float  m_laneH = 0.0f;
    float  m_toolsH = 0.0f;   // the tools under the splitter, high (0 = the default)
    float  m_splitY = 0.0f;

    // --- The preview -----------------------------------------------------------
    std::unique_ptr<fitzel::SamplePlayer> m_player;
    View*         m_playing = nullptr;     // the view whose sound the player holds
    std::uint64_t m_playerVersion = 0;
    bool          m_loopMode  = true;      // play round the loop (or the selection)
    bool          m_follow    = true;      // keep the playhead in view
    float         m_volumeDb  = -6.0f;
    float         m_speed     = 1.0f;
    float         m_meter[2]  = {0.0f, 0.0f};   // shown level (linear, decaying)
    float         m_hold[2]   = {0.0f, 0.0f};   // peak hold
    double        m_holdTime[2] = {0.0, 0.0};

    // --- Settings the tools share -------------------------------------------------
    int   m_units       = 0;      // 0 time, 1 samples
    bool  m_snapZero    = true;
    int   m_chanMask    = 0;      // 0 both, 1 left, 2 right
    int   m_fadeCurve   = 1;
    float m_gainDb      = 3.0f;
    float m_normDb      = -1.0f;
    float m_silenceMs   = 500.0f;
    float m_trimDb      = -60.0f;
    int   m_rateChoice  = 2;
    float m_xfadeMs     = 50.0f;
    int   m_saveFormat  = -1;     // -1 as it came, else fitzel::WavFormat
    float m_vzoom       = 1.0f;   // vertical zoom of the waveform
    wave::Clip m_clip;            // the editor's clipboard

    // --- Helpers (WaveEditPanel.cpp) ------------------------------------------------
    View* cur();
    void  menuBar(View* v);
    void  transport(View* v);
    void  overview(View& v, float height);
    void  waveArea(View& v, float height);
    void  toolColumns(View& v);
    void  loopTools(View& v);
    void  editTools(View& v);
    void  analysis(View& v);
    void  shortcuts(View& v);
    void  contextMenu(View& v);
    void  processItems(View& v);   // the Process menu, also on the right-click menu
    void  applyGain(View& v);
    void  applyNormalize(View& v);
    void  applyReverse(View& v);

    void  openDialog();
    bool  save(View& v, bool as);
    void  closeView(int i);

    unsigned mask(const View& v) const;
    void  edit(View& v, const char* label, const std::function<void(wave::Document&)>& op);
    void  cutCopy(View& v, bool cut);
    void  paste(View& v);
    void  play(View& v, bool selectionOnly);
    void  stop();
    bool  playing() const;
    void  syncPlayer();
    std::string fmtTime(const View& v, wave::Frames f) const;
};

} // namespace waveui
