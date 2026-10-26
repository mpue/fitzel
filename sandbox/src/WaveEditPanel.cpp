#include "WaveEditPanel.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>

#include <imgui.h>

#include <fitzel/audio/Audio.hpp>
#include <fitzel/audio/SamplePlayer.hpp>

#include "FolderDialog.hpp"
#include "UiStyle.hpp"

namespace waveui {

using wave::Frames;

namespace {

constexpr float kRulerH = 22.0f;
const int kRates[] = {22050, 32000, 44100, 48000};

const ImU32 kBg       = IM_COL32(20, 22, 26, 255);
const ImU32 kLaneLine = IM_COL32(60, 64, 72, 255);
const ImU32 kGrid     = IM_COL32(44, 48, 56, 255);
const ImU32 kPeak     = IM_COL32(80, 170, 235, 255);
const ImU32 kRms      = IM_COL32(150, 215, 255, 255);
const ImU32 kClip     = IM_COL32(240, 70, 60, 255);
const ImU32 kSel      = IM_COL32(90, 130, 200, 70);
const ImU32 kSelEdge  = IM_COL32(120, 160, 230, 200);
const ImU32 kLoop     = IM_COL32(70, 190, 110, 38);
const ImU32 kLoopLine = IM_COL32(90, 225, 130, 255);
const ImU32 kCursor   = IM_COL32(240, 240, 240, 230);
const ImU32 kPlayhead = IM_COL32(255, 170, 60, 255);
const ImU32 kText     = IM_COL32(170, 176, 186, 255);

bool hasExt(const std::string& p, const char* ext) {
    std::string e = std::filesystem::path(p).extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == ext;
}
bool isAudioFile(const std::string& p) {
    return hasExt(p, ".wav") || hasExt(p, ".ogg") || hasExt(p, ".mp3") || hasExt(p, ".flac");
}

// A row of buttons, one per choice, the chosen one lit -- every option one
// click away (the Stairs panel's control, for the same reason).
bool choice(const char* id, int& cur, const char* const* labels, int n, float width = 0.0f) {
    bool changed = false;
    ImGui::PushID(id);
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float total = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    const float w = (total - gap * static_cast<float>(n - 1)) / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        if (i) ImGui::SameLine();
        const bool on = i == cur;
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        ImGui::PushID(i);
        if (ImGui::Button(labels[i], ImVec2(w, 0.0f)) && !on) { cur = i; changed = true; }
        ImGui::PopID();
        if (on) ImGui::PopStyleColor(2);
    }
    ImGui::PopID();
    return changed;
}

// A tick step for a ruler: 1, 2 or 5 times a power of ten, no closer than
// `minUnits` (seconds or frames).
double niceStep(double minUnits) {
    double p = std::pow(10.0, std::floor(std::log10(std::max(minUnits, 1e-9))));
    for (;;) {
        for (double m : {1.0, 2.0, 5.0})
            if (m * p >= minUnits) return m * p;
        p *= 10.0;
    }
}

std::string clockOf(double s, int decimals) {
    if (s < 0.0) s = 0.0;
    const int m = static_cast<int>(s / 60.0);
    const double r = s - m * 60.0;
    char b[48];
    if (decimals <= 0) std::snprintf(b, sizeof b, "%d:%02d", m, static_cast<int>(r + 1e-9));
    else std::snprintf(b, sizeof b, "%d:%0*.*f", m, decimals + 3, decimals, r);
    return b;
}

ImU32 meterColor(float db) {
    if (db > -3.0f) return IM_COL32(240, 80, 60, 255);
    if (db > -12.0f) return IM_COL32(235, 200, 70, 255);
    return IM_COL32(90, 210, 110, 255);
}

} // namespace

// --- One open sound ----------------------------------------------------------------

struct WaveEditor::View {
    wave::Document doc;
    Frames cursor = 0, selA = 0, selB = 0;
    double viewStart = 0.0;   // frame at the left edge
    double fpp       = 0.0;   // frames per pixel; 0 = fit on the next draw
    float  areaW     = 1.0f;
    int    drag      = 0;     // 1 select, 2 loop start, 3 loop end, 4 overview
    Frames anchor    = 0;
    Frames grab      = 0;     // a dragged marker's distance from the mouse
    bool   closeArmed = false;
    Frames hover     = -1;    // frame under the mouse in the wave area
    float  hoverValue = 0.0f;
    // Caches, keyed by version and range.
    std::uint64_t statsVer = 0, specVer = 0;
    Frames statsA = -1, statsB = -1, specA = -1, specB = -1;
    wave::Stats stats;
    std::vector<float> spec;
    std::string loopNote;

    bool hasSel() const { return selB > selA; }
    Frames lo() const { return hasSel() ? selA : 0; }
    Frames hi() const { return hasSel() ? selB : doc.frames(); }

    void clampAll() {
        const Frames n = doc.frames();
        cursor = std::clamp<Frames>(cursor, 0, n);
        selA = std::clamp<Frames>(selA, 0, n);
        selB = std::clamp<Frames>(selB, 0, n);
        if (selB < selA) std::swap(selA, selB);
    }
    void clampView() {
        const double n = static_cast<double>(std::max<Frames>(doc.frames(), 1));
        const double maxFpp = std::max(n / std::max(areaW, 1.0f), 1.0 / 64.0);
        fpp = std::clamp(fpp, 1.0 / 64.0, maxFpp);
        viewStart = std::clamp(viewStart, 0.0, std::max(0.0, n - fpp * areaW));
    }
    void fit() {
        fpp = static_cast<double>(std::max<Frames>(doc.frames(), 1)) / std::max(areaW, 1.0f);
        viewStart = 0.0;
        clampView();
    }
    void zoomAt(double factor, float px) {
        const double f = viewStart + px * fpp;
        fpp *= factor;
        clampView();
        viewStart = f - px * fpp;
        clampView();
    }
    void show(Frames a, Frames b) {   // zoom to a range, a little margin round it
        if (b <= a) return;
        fpp = static_cast<double>(b - a) * 1.1 / std::max(areaW, 1.0f);
        clampView();
        viewStart = static_cast<double>(a) - static_cast<double>(b - a) * 0.05;
        clampView();
    }
    float xOf(Frames f, float x0) const { return x0 + static_cast<float>((f - viewStart) / fpp); }
    Frames frameAt(float x, float x0) const {
        return std::clamp<Frames>(static_cast<Frames>(std::llround(viewStart + (x - x0) * fpp)), 0,
                                  doc.frames());
    }
};

WaveEditor::WaveEditor(Deps d) : m_deps(d) {}
WaveEditor::~WaveEditor() = default;

WaveEditor::View* WaveEditor::cur() {
    if (m_views.empty()) return nullptr;
    m_active = std::clamp(m_active, 0, static_cast<int>(m_views.size()) - 1);
    return m_views[m_active].get();
}

bool WaveEditor::hasKeyboard() const { return m_focusFrame >= ImGui::GetFrameCount() - 1; }

void WaveEditor::setSelection(Frames a, Frames b) {
    if (View* v = cur()) {
        v->selA = a; v->selB = b; v->cursor = a;
        v->clampAll();
    }
}

wave::Document* WaveEditor::document() { View* v = cur(); return v ? &v->doc : nullptr; }

Frames WaveEditor::cursorFrame() const {
    return m_views.empty() ? 0 : m_views[std::clamp(m_active, 0, static_cast<int>(m_views.size()) - 1)]->cursor;
}

void WaveEditor::selection(Frames& a, Frames& b) const {
    a = b = 0;
    if (m_views.empty()) return;
    const View& v = *m_views[std::clamp(m_active, 0, static_cast<int>(m_views.size()) - 1)];
    a = v.selA; b = v.selB;
}

double WaveEditor::framesPerPixel() const {
    return m_views.empty() ? 0.0 : m_views[std::clamp(m_active, 0, static_cast<int>(m_views.size()) - 1)]->fpp;
}

ImVec2 WaveEditor::toScreen(Frames f, float lane01) const {
    if (m_views.empty()) return ImVec2(0, 0);
    const View& v = *m_views[std::clamp(m_active, 0, static_cast<int>(m_views.size()) - 1)];
    const float x = v.xOf(f, m_waveMin.x);
    const float y = lane01 < 0.0f ? m_waveMin.y + kRulerH * 0.5f
                                  : m_waveMin.y + kRulerH + lane01 * m_laneH;
    return ImVec2(x, y);
}

bool WaveEditor::open(const std::string& path) {
    for (int i = 0; i < static_cast<int>(m_views.size()); ++i)
        if (m_views[i]->doc.path == path) { m_active = i; return true; }
    auto v = std::make_unique<View>();
    std::string err;
    if (!v->doc.load(path, &err)) {
        m_deps.status = "Wave editor: " + err;
        return false;
    }
    m_views.push_back(std::move(v));
    m_active = static_cast<int>(m_views.size()) - 1;
    m_deps.status = "Opened " + m_views.back()->doc.name() + ".";
    return true;
}

wave::Document& WaveEditor::newDocument(int channels, int rate, double seconds) {
    auto v = std::make_unique<View>();
    v->doc.newSilence(channels, rate, seconds);
    m_views.push_back(std::move(v));
    m_active = static_cast<int>(m_views.size()) - 1;
    return m_views.back()->doc;
}

void WaveEditor::closeView(int i) {
    if (i < 0 || i >= static_cast<int>(m_views.size())) return;
    if (m_playing == m_views[i].get()) { stop(); m_playing = nullptr; }
    m_views.erase(m_views.begin() + i);
    m_active = std::max(0, std::min(m_active, static_cast<int>(m_views.size()) - 1));
}

unsigned WaveEditor::mask(const View& v) const {
    if (v.doc.channels() < 2 || m_chanMask == 0) return wave::kAllChannels;
    return m_chanMask == 1 ? 1u : 2u;
}

std::string WaveEditor::fmtTime(const View& v, Frames f) const {
    if (m_units == 1) {
        char b[32];
        std::snprintf(b, sizeof b, "%lld", static_cast<long long>(f));
        return b;
    }
    return clockOf(static_cast<double>(f) / std::max(v.doc.rate(), 1), 3);
}

// --- Editing --------------------------------------------------------------------------

void WaveEditor::edit(View& v, const char* label, const std::function<void(wave::Document&)>& op) {
    v.doc.checkpoint(label);
    op(v.doc);
    v.clampAll();
    v.clampView();
    m_deps.status = std::string(label) + ".";
}

void WaveEditor::cutCopy(View& v, bool cut) {
    if (!v.hasSel()) { m_deps.status = "Select something first."; return; }
    m_clip = wave::copy(v.doc, v.selA, v.selB);
    if (!cut) {
        m_deps.status = "Copied " + fmtTime(v, v.selB - v.selA) + ".";
        return;
    }
    const Frames a = v.selA, b = v.selB;
    edit(v, "Cut", [&](wave::Document& d) { wave::erase(d, a, b); });
    v.cursor = v.selA = v.selB = a;
}

void WaveEditor::paste(View& v) {
    if (m_clip.frames() <= 0) { m_deps.status = "Nothing to paste."; return; }
    const Frames at = v.hasSel() ? v.selA : v.cursor;
    const Frames a = v.selA, b = v.selB;
    const bool replace = v.hasSel();
    const Frames before = v.doc.frames() - (replace ? b - a : 0);
    edit(v, "Paste", [&](wave::Document& d) {
        if (replace) wave::erase(d, a, b);
        wave::insert(d, at, m_clip);
    });
    v.selA = at;
    v.selB = at + (v.doc.frames() - before);
    v.cursor = v.selA;
    v.clampAll();
}

// --- The preview ------------------------------------------------------------------------

bool WaveEditor::playing() const { return m_player && m_player->isPlaying(); }

void WaveEditor::stop() {
    if (m_player) m_player->stop();
}

void WaveEditor::stopPreview() { stop(); }

void WaveEditor::play(View& v, bool selectionOnly) {
    if (!m_deps.audio || !m_deps.audio->ok()) {
        m_deps.status = "No sound device: the preview is off.";
        return;
    }
    if (v.doc.frames() <= 0) return;
    if (!m_player || m_player->channels() != v.doc.channels() ||
        m_player->sampleRate() != v.doc.rate()) {
        if (m_player) m_player->stop();
        m_player = std::make_unique<fitzel::SamplePlayer>(
            fitzel::SamplePlayer::create(*m_deps.audio, v.doc.channels(), v.doc.rate()));
        m_playing = nullptr;
    }
    if (!m_player->isValid()) return;
    if (m_playing != &v || m_playerVersion != v.doc.version()) {
        m_player->setSamples(v.doc.data.samples);
        m_playing = &v;
        m_playerVersion = v.doc.version();
    }
    Frames from = v.cursor, to = -1;
    if (selectionOnly && v.hasSel()) { from = v.selA; to = v.selB; }
    else if (v.hasSel() && m_loopMode && !v.doc.data.hasLoop()) from = v.selA;
    if (from >= v.doc.frames()) from = 0;
    m_player->setVolume(wave::fromDb(m_volumeDb));
    m_player->setRate(m_speed);
    syncPlayer();   // the loop, before the first block
    m_player->play(from, to);
}

// Every frame: the player's copy of the sound and its loop follow the document
// -- an edit or a moved loop point is heard on the next pass, not the next Play.
void WaveEditor::syncPlayer() {
    if (!m_player || !m_player->isValid() || !m_playing) {
        m_meter[0] = m_meter[1] = 0.0f;
        return;
    }
    bool alive = false;
    for (const auto& v : m_views) alive |= v.get() == m_playing;
    if (!alive) { stop(); m_playing = nullptr; return; }
    View& v = *m_playing;
    if (m_playerVersion != v.doc.version()) {
        m_player->setSamples(v.doc.data.samples);
        m_playerVersion = v.doc.version();
    }
    if (m_loopMode && v.doc.data.hasLoop())
        m_player->setLoop(true, v.doc.data.loopStart, v.doc.data.loopEnd);
    else if (m_loopMode && v.hasSel())
        m_player->setLoop(true, v.selA, v.selB);
    else
        m_player->setLoop(false, 0, 0);
    m_player->setVolume(wave::fromDb(m_volumeDb));
    m_player->setRate(m_speed);

    float l = 0.0f, r = 0.0f;
    m_player->takePeak(l, r);
    const float dt = ImGui::GetIO().DeltaTime;
    const double now = ImGui::GetTime();
    const float in[2] = {l, r};
    for (int i = 0; i < 2; ++i) {
        m_meter[i] = std::max(in[i], m_meter[i] * std::exp(-dt * 6.0f));
        if (in[i] >= m_hold[i] || now - m_holdTime[i] > 1.5) { m_hold[i] = in[i]; m_holdTime[i] = now; }
    }
    // Keep the playhead in view: page along when it runs off the right edge.
    if (m_follow && playing() && v.drag == 0) {
        const double p = static_cast<double>(m_player->position());
        const double w = v.fpp * v.areaW;
        if (p < v.viewStart || p > v.viewStart + w) {
            v.viewStart = p - w * 0.05;
            v.clampView();
        }
    }
}

// --- Files ------------------------------------------------------------------------------

void WaveEditor::openDialog() {
    std::string dir;
    if (!m_deps.currentProject.empty()) {
        const auto proj = std::filesystem::path(m_deps.currentProject).parent_path();
        dir = std::filesystem::exists(proj / "sounds") ? (proj / "sounds").string() : proj.string();
    }
    std::string out;
    if (ed::pickFile(out, dir, "Audio (wav, ogg, mp3, flac)", "*.wav;*.ogg;*.mp3;*.flac"))
        open(out);
}

bool WaveEditor::save(View& v, bool as) {
    const auto& d = v.doc.data;
    fitzel::WavFormat fmt;
    if (m_saveFormat >= 0) fmt = static_cast<fitzel::WavFormat>(m_saveFormat);
    else if (d.sourceFloat || d.sourceBits == 32) fmt = fitzel::WavFormat::Float32;
    else if (d.sourceBits == 24) fmt = fitzel::WavFormat::Pcm24;
    else fmt = fitzel::WavFormat::Pcm16;

    std::string path = v.doc.path;
    if (as || path.empty() || !hasExt(path, ".wav")) {
        // Not a WAV (or never saved): next to where it came from, as a .wav.
        std::string dir, name = "sound.wav";
        if (!path.empty()) {
            dir  = std::filesystem::path(path).parent_path().string();
            name = std::filesystem::path(path).stem().string() + ".wav";
        } else if (!m_deps.currentProject.empty()) {
            const auto proj = std::filesystem::path(m_deps.currentProject).parent_path();
            dir = std::filesystem::exists(proj / "sounds") ? (proj / "sounds").string() : proj.string();
        }
        std::string out;
        if (!ed::saveFile(out, dir, name, "WAV audio", "*.wav", "wav")) return false;
        path = out;
    }
    std::string err;
    if (!v.doc.save(path, fmt, &err)) {
        m_deps.status = "Wave editor: " + err;
        return false;
    }
    const char* fn = fmt == fitzel::WavFormat::Pcm16 ? "16-bit" : fmt == fitzel::WavFormat::Pcm24 ? "24-bit" : "32-bit float";
    m_deps.status = "Saved " + v.doc.name() + " (" + fn +
                    (d.hasLoop() ? ", loop " + fmtTime(v, d.loopStart) + " - " + fmtTime(v, d.loopEnd) : std::string()) +
                    ").";
    return true;
}

// --- The window ---------------------------------------------------------------------------

void WaveEditor::panel(bool& show) {
    syncPlayer();
    ImGui::SetNextWindowSize(ImVec2(1180.0f, 760.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Wave editor", &show, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focused) m_focusFrame = ImGui::GetFrameCount();

    // Sound files dropped on the window open here.
    if (m_deps.drops && !m_deps.drops->empty() &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        auto& dr = *m_deps.drops;
        for (auto it = dr.begin(); it != dr.end();) {
            if (isAudioFile(*it)) { open(*it); it = dr.erase(it); }
            else ++it;
        }
    }

    View* v = cur();
    menuBar(v);

    if (m_views.empty()) {
        ui::hint("Open a sound (wav, ogg, mp3, flac) -- or right-click one in the Assets "
                 "panel, \"Edit in Wave editor\" -- or drop one on this window.");
        if (ImGui::Button("Open...", ImVec2(160.0f, 34.0f))) openDialog();
        ImGui::SameLine();
        if (ImGui::Button("New (1 s silence)", ImVec2(180.0f, 34.0f))) newDocument(2, 48000, 1.0);
        ImGui::End();
        if (focused) ImGui::GetIO().WantTextInput = true;
        return;
    }

    // One tab per sound; closing a changed one takes a second click.
    int closeReq = -1;
    if (ImGui::BeginTabBar("##waveTabs", ImGuiTabBarFlags_AutoSelectNewTabs |
                                             ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int i = 0; i < static_cast<int>(m_views.size()); ++i) {
            View& t = *m_views[i];
            char label[256];
            std::snprintf(label, sizeof label, "%s%s###wave%p", t.doc.name().c_str(),
                          t.doc.dirty ? " *" : "", static_cast<void*>(&t));
            bool open = true;
            if (ImGui::BeginTabItem(label, &open)) {
                m_active = i;
                ImGui::EndTabItem();
            }
            if (!open) closeReq = i;
        }
        ImGui::EndTabBar();
    }
    if (closeReq >= 0) {
        View& t = *m_views[closeReq];
        if (t.doc.dirty && !t.closeArmed) {
            t.closeArmed = true;
            m_deps.status = t.doc.name() + " has unsaved changes -- close it again to discard them.";
        } else {
            closeView(closeReq);
        }
    }
    v = cur();
    if (!v) { ImGui::End(); return; }

    transport(v);
    overview(*v, 44.0f);
    // The waveform and the tools share what is left, with a bar between them.
    // Dragged, the bar moves the line (the tools keep their height when the
    // window is resized); double-clicked, it goes back to where it started.
    const float sp       = ImGui::GetStyle().ItemSpacing.y;
    const float barH     = 7.0f;
    const float avail    = ImGui::GetContentRegionAvail().y;
    const float bottom   = ImGui::GetCursorScreenPos().y + avail;
    const float minWave  = 160.0f;
    const float minTools = ImGui::GetFrameHeightWithSpacing() * 3.0f;
    const float room     = avail - barH - 2.0f * sp;
    const float toolsH   = std::clamp(m_toolsH > 0.0f ? m_toolsH
                                                      : std::max(ImGui::GetFontSize() * 16.0f, 250.0f),
                                      minTools, std::max(minTools, room - minWave));
    waveArea(*v, std::max(minWave, room - toolsH));
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##waveSplit", ImVec2(w, barH));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        // From where the mouse is, not by how far it moved: the bar stays under
        // it, also past the limits and back. Only once it has moved, so the
        // double-click that resets the line is not undone by its own press.
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))
            m_toolsH = bottom - ImGui::GetIO().MousePos.y - barH * 0.5f - sp;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) m_toolsH = 0.0f;
        ImGui::SetItemTooltip("Drag: the waveform's height. Double-click: as it was.");
        m_splitY = p.y + barH * 0.5f;
        const ImU32 c = hot ? IM_COL32(150, 160, 180, 255) : IM_COL32(80, 86, 98, 255);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + w * 0.5f - 24.0f, m_splitY - 1.5f),
                                                  ImVec2(p.x + w * 0.5f + 24.0f, m_splitY + 1.5f), c, 1.5f);
    }
    toolColumns(*v);

    if (focused && !ImGui::GetIO().WantTextInput) shortcuts(*v);
    ImGui::End();
    // While the window has the keyboard, claim it like a text field does: the
    // scene's shortcuts (undo, Q/W/E, the camera keys ...) stand aside.
    if (focused) ImGui::GetIO().WantTextInput = true;
}

void WaveEditor::menuBar(View* v) {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New (1 s silence)")) newDocument(2, 48000, 1.0);
        if (ImGui::MenuItem("Open...", "Ctrl+O")) openDialog();
        ImGui::BeginDisabled(!v);
        if (ImGui::MenuItem("Save", "Ctrl+S") && v) save(*v, false);
        if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S") && v) save(*v, true);
        ImGui::EndDisabled();
        ImGui::Separator();
        const char* fmts[] = {"As it came", "16-bit", "24-bit", "32-bit float"};
        int f = m_saveFormat + 1;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Save as", &f, fmts, 4)) m_saveFormat = f - 1;
        ImGui::SetItemTooltip("Bit depth of the WAV written. 16-bit is dithered.");
        ImGui::Separator();
        ImGui::BeginDisabled(!v);
        if (ImGui::MenuItem("Close") && v) closeView(m_active);
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }
    if (v && ImGui::BeginMenu("Edit")) {
        wave::Document& d = v->doc;
        std::string u = "Undo " + d.undoLabel(), r = "Redo " + d.redoLabel();
        if (ImGui::MenuItem(u.c_str(), "Ctrl+Z", false, d.canUndo())) { d.undo(); v->clampAll(); v->clampView(); }
        if (ImGui::MenuItem(r.c_str(), "Ctrl+Y", false, d.canRedo())) { d.redo(); v->clampAll(); v->clampView(); }
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, v->hasSel())) cutCopy(*v, true);
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, v->hasSel())) cutCopy(*v, false);
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, m_clip.frames() > 0)) paste(*v);
        if (ImGui::MenuItem("Delete", "Del", false, v->hasSel())) {
            const Frames a = v->selA, b = v->selB;
            edit(*v, "Delete", [&](wave::Document& dd) { wave::erase(dd, a, b); });
            v->selB = v->selA;
        }
        if (ImGui::MenuItem("Crop to selection", nullptr, false, v->hasSel())) {
            const Frames a = v->selA, b = v->selB;
            edit(*v, "Crop", [&](wave::Document& dd) { wave::crop(dd, a, b); });
            v->selA = 0; v->selB = v->doc.frames(); v->cursor = 0;
            v->fit();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select all", "Ctrl+A")) { v->selA = 0; v->selB = d.frames(); }
        if (ImGui::MenuItem("Select loop", nullptr, false, d.data.hasLoop())) {
            v->selA = d.data.loopStart; v->selB = d.data.loopEnd;
        }
        ImGui::EndMenu();
    }
    if (v && ImGui::BeginMenu("Process")) {
        processItems(*v);
        ImGui::EndMenu();
    }
    if (v && ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Zoom in", "+")) v->zoomAt(0.5, v->areaW * 0.5f);
        if (ImGui::MenuItem("Zoom out", "-")) v->zoomAt(2.0, v->areaW * 0.5f);
        if (ImGui::MenuItem("Whole sound", "F")) v->fit();
        if (ImGui::MenuItem("Zoom to selection", "Z", false, v->hasSel())) v->show(v->selA, v->selB);
        if (ImGui::MenuItem("Zoom to loop", nullptr, false, v->doc.data.hasLoop()))
            v->show(v->doc.data.loopStart, v->doc.data.loopEnd);
        ImGui::Separator();
        ImGui::MenuItem("Follow the playhead", nullptr, &m_follow);
        ImGui::MenuItem("Snap to zero crossings", nullptr, &m_snapZero);
        const char* units[] = {"Time", "Samples"};
        ImGui::SetNextItemWidth(140.0f);
        ImGui::Combo("Units", &m_units, units, 2);
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void WaveEditor::transport(View* vp) {
    View& v = *vp;
    const bool canPlay = m_deps.audio && m_deps.audio->ok();
    const float em = ImGui::GetFontSize();
    const ImVec2 big(em * 4.2f, em * 1.7f);
    if (ImGui::Button("|<", ImVec2(em * 2.2f, big.y))) {
        v.cursor = 0;
        v.viewStart = 0.0;
        if (playing()) play(v, false);
    }
    ImGui::SetItemTooltip("To the start (Home)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!canPlay);
    const bool isPlaying = playing() && m_playing == &v;
    if (isPlaying) {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(170, 90, 40, 255));
        if (ImGui::Button("Stop", big)) stop();
        ImGui::PopStyleColor();
    } else if (ImGui::Button("Play", big)) {
        play(v, false);
    }
    ImGui::SetItemTooltip("Space. From the cursor (or the selection's start); with Loop on, "
                          "round the loop points -- or the selection when there are none.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.hasSel());
    if (ImGui::Button("Play sel.", ImVec2(em * 4.6f, big.y))) play(v, true);
    ImGui::SetItemTooltip("Play just the selection (round it, with Loop on).");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!canPlay) ImGui::SetItemTooltip("No sound device: the preview is off.");
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &m_loopMode);
    ImGui::SetItemTooltip("L. Play round the loop points as the game will, or the selection "
                          "when the sound has none.");
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &m_follow);
    ImGui::SameLine();
    ImGui::TextDisabled("Vol");
    ImGui::SameLine();
    ui::stepper("##vol", m_volumeDb, 1.0f, -40.0f, 6.0f, "%.0f dB");
    ImGui::SetItemTooltip("Preview volume (the sound itself is not changed).");
    ImGui::SameLine();
    ImGui::TextDisabled("Speed");
    ImGui::SameLine();
    ui::stepper("##speed", m_speed, 0.05f, 0.25f, 2.0f, "%.2fx");
    ImGui::SetItemTooltip("Preview speed -- and pitch with it, like a tape.");

    // Meters: -60..0 dB, a hold mark that waits 1.5 s.
    ImGui::SameLine();
    {
        const float w = em * 6.0f, h = em * 0.55f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(w, big.y));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < 2; ++i) {
            const float y = p.y + big.y * 0.5f - h - 1.0f + i * (h + 2.0f);
            dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), IM_COL32(30, 32, 38, 255), 2.0f);
            const float db = wave::toDb(m_meter[i]);
            const float f = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w * f, y + h), meterColor(db), 2.0f);
            const float hdb = wave::toDb(m_hold[i]);
            const float hf = std::clamp((hdb + 60.0f) / 60.0f, 0.0f, 1.0f);
            if (hf > 0.0f)
                dl->AddLine(ImVec2(p.x + w * hf, y), ImVec2(p.x + w * hf, y + h), meterColor(hdb), 2.0f);
        }
    }
    // Where we are, large.
    ImGui::SameLine();
    {
        const Frames at = (isPlaying && m_player) ? m_player->position() : v.cursor;
        ImGui::PushFont(ui::boldFont());
        ImGui::Text("%s", fmtTime(v, at).c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("/ %s", fmtTime(v, v.doc.frames()).c_str());
    }
}

// The whole sound in a strip -- and the scroll bar: click or drag to look there.
void WaveEditor::overview(View& v, float height) {
    const float W = ImGui::GetContentRegionAvail().x;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##overview", ImVec2(W, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + height), IM_COL32(28, 30, 35, 255), 3.0f);
    const Frames n = v.doc.frames();
    if (n <= 0) return;
    const double fpc = static_cast<double>(n) / W;
    const float mid = o.y + height * 0.5f, amp = height * 0.45f;
    const auto& lods = v.doc.lods();
    const wave::Lod* lod = nullptr;
    for (const wave::Lod& l : lods)
        if ((Frames(1) << l.shift) <= fpc) lod = &l;
    for (int px = 0; px < static_cast<int>(W); ++px) {
        const Frames f0 = static_cast<Frames>(px * fpc), f1 = std::max(f0 + 1, static_cast<Frames>((px + 1) * fpc));
        float lo = 0.0f, hi = 0.0f;
        for (int c = 0; c < v.doc.channels(); ++c) {
            if (lod) {
                for (Frames b = f0 >> lod->shift; b <= (f1 - 1) >> lod->shift && b < static_cast<Frames>(lod->mn[c].size()); ++b) {
                    lo = std::min(lo, lod->mn[c][b]); hi = std::max(hi, lod->mx[c][b]);
                }
            } else {
                for (Frames f = f0; f < f1 && f < n; ++f) {
                    const float s = v.doc.sample(f, c);
                    lo = std::min(lo, s); hi = std::max(hi, s);
                }
            }
        }
        dl->AddLine(ImVec2(o.x + px + 0.5f, mid - hi * amp), ImVec2(o.x + px + 0.5f, mid - lo * amp + 1.0f),
                    IM_COL32(90, 130, 170, 255));
    }
    auto X = [&](double f) { return o.x + static_cast<float>(f / n * W); };
    if (v.doc.data.hasLoop())
        dl->AddRectFilled(ImVec2(X(v.doc.data.loopStart), o.y), ImVec2(X(v.doc.data.loopEnd), o.y + height),
                          IM_COL32(70, 190, 110, 50));
    const double vs = v.viewStart, ve = v.viewStart + v.fpp * v.areaW;
    dl->AddRectFilled(ImVec2(X(vs), o.y), ImVec2(std::max(X(ve), X(vs) + 3.0f), o.y + height), IM_COL32(255, 255, 255, 28));
    dl->AddRect(ImVec2(X(vs), o.y), ImVec2(std::max(X(ve), X(vs) + 3.0f), o.y + height), IM_COL32(220, 220, 230, 160), 2.0f);
    if (playing() && m_playing == &v) {
        const float x = X(static_cast<double>(m_player->position()));
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + height), kPlayhead, 1.5f);
    }
    if (ImGui::IsItemActive()) {
        const double f = (ImGui::GetIO().MousePos.x - o.x) / W * n;
        v.viewStart = f - v.fpp * v.areaW * 0.5;
        v.clampView();
    }
    ImGui::SetItemTooltip("The whole sound. Click to look there.");
}

void WaveEditor::waveArea(View& v, float height) {
    ImGuiIO& io = ImGui::GetIO();
    const float W = std::max(ImGui::GetContentRegionAvail().x, 50.0f);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##wave", ImVec2(W, height),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();
    v.areaW = W;
    if (v.fpp <= 0.0) v.fit();
    v.clampView();
    wave::Document& d = v.doc;
    const Frames n = d.frames();
    const int ch = std::max(d.channels(), 1);
    const float x0 = o.x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(o, ImVec2(o.x + W, o.y + height), true);
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + height), kBg);

    const float top = o.y + kRulerH;
    const float laneGap = 3.0f;
    const float laneH = (height - kRulerH - laneGap * (ch - 1)) / ch;
    m_waveMin = o;
    m_waveSize = ImVec2(W, height);
    m_laneH = laneH;
    const auto& fs = d.data;

    // --- The mouse.
    const ImVec2 m = io.MousePos;
    const Frames mf = v.frameAt(m.x, x0);
    v.hover = hovered ? mf : -1;
    v.hoverValue = 0.0f;
    if (hovered && m.y >= top) {
        const int lane = std::clamp(static_cast<int>((m.y - top) / (laneH + laneGap)), 0, ch - 1);
        v.hoverValue = d.sample(mf, lane);
    }
    auto snap = [&](Frames f) {
        if (!m_snapZero) return f;
        const Frames reach = std::clamp<Frames>(static_cast<Frames>(v.fpp * 6.0), 16, 4096);
        return wave::nearestZeroCrossing(d, f, reach);
    };
    const float xls = fs.hasLoop() ? v.xOf(fs.loopStart, x0) : -1e9f;
    const float xle = fs.hasLoop() ? v.xOf(fs.loopEnd, x0) : -1e9f;
    const bool nearStart = fs.hasLoop() && std::abs(m.x - xls) < 6.0f;
    const bool nearEnd   = fs.hasLoop() && std::abs(m.x - xle) < 6.0f;
    // Shift always means "the selection": a Shift+click beside a marker
    // extends it rather than picking the marker up.
    const bool onMarker = (nearStart || nearEnd) && !io.KeyShift;
    if (hovered && onMarker) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        if (onMarker) {
            v.drag = (nearStart && (!nearEnd || std::abs(m.x - xls) <= std::abs(m.x - xle))) ? 2 : 3;
            // Picked up where it is: it moves with the mouse, it does not jump to it.
            v.grab = (v.drag == 2 ? fs.loopStart : fs.loopEnd) - mf;
            d.checkpoint("Move loop point");
        } else if (m.y < top) {
            v.cursor = snap(mf);   // the ruler: just the cursor (and the playhead)
            v.selA = v.selB = v.cursor;
            v.drag = 0;
            if (playing() && m_playing == &v) play(v, false);
        } else if (io.KeyShift) {
            v.anchor = v.cursor;
            v.drag = 1;
        } else {
            v.cursor = v.anchor = snap(mf);
            v.selA = v.selB = v.cursor;
            v.drag = 1;
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && m.y >= top) {
        v.selA = 0; v.selB = n; v.drag = 0;
    }
    if (active && v.drag && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // Past an edge: the view runs along.
        if (m.x < x0) { v.viewStart -= v.fpp * 12.0; v.clampView(); }
        if (m.x > x0 + W) { v.viewStart += v.fpp * 12.0; v.clampView(); }
        const Frames raw = v.frameAt(std::clamp(m.x, x0, x0 + W), x0);
        const Frames f = snap(v.drag >= 2 ? std::clamp<Frames>(raw + v.grab, 0, n) : raw);
        if (v.drag == 1) {
            v.selA = std::min(v.anchor, f);
            v.selB = std::max(v.anchor, f);
        } else if (v.drag == 2) {
            d.data.loopStart = std::clamp<Frames>(f, 0, d.data.loopEnd - 16);
            d.dirty = true;
        } else if (v.drag == 3) {
            d.data.loopEnd = std::clamp<Frames>(f, d.data.loopStart + 16, n);
            d.dirty = true;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) v.drag = 0;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##waveCtx");
    if (hovered) {
        if (io.MouseWheel != 0.0f) {
            if (io.KeyShift) { v.viewStart -= io.MouseWheel * v.fpp * W * 0.15; v.clampView(); }
            else v.zoomAt(std::pow(0.8, static_cast<double>(io.MouseWheel)), m.x - x0);
        }
        if (io.MouseWheelH != 0.0f) { v.viewStart -= io.MouseWheelH * v.fpp * W * 0.15; v.clampView(); }
    }

    // --- The ruler.
    dl->AddRectFilled(o, ImVec2(o.x + W, top), IM_COL32(32, 35, 41, 255));
    {
        const int rate = std::max(d.rate(), 1);
        const double unitsPerPx = m_units == 1 ? v.fpp : v.fpp / rate;
        const double step = niceStep(unitsPerPx * 90.0);
        const double minor = step / 5.0;
        const double u0 = v.viewStart * (m_units == 1 ? 1.0 : 1.0 / rate);
        const double u1 = (v.viewStart + v.fpp * W) * (m_units == 1 ? 1.0 : 1.0 / rate);
        const int decimals = step >= 1.0 ? 0 : std::min(6, static_cast<int>(std::ceil(-std::log10(step) - 1e-9)));
        for (double u = std::floor(u0 / minor) * minor; u <= u1; u += minor) {
            const double f = m_units == 1 ? u : u * rate;
            const float x = x0 + static_cast<float>((f - v.viewStart) / v.fpp);
            const bool major = std::abs(std::remainder(u, step)) < minor * 0.25;
            dl->AddLine(ImVec2(x, top - (major ? 9.0f : 4.0f)), ImVec2(x, top), kText);
            if (major) {
                dl->AddLine(ImVec2(x, top), ImVec2(x, o.y + height), kGrid);
                char b[48];
                if (m_units == 1) std::snprintf(b, sizeof b, "%lld", static_cast<long long>(std::llround(u)));
                else std::snprintf(b, sizeof b, "%s", clockOf(u, decimals).c_str());
                dl->AddText(ImVec2(x + 3.0f, o.y + 2.0f), kText, b);
            }
        }
    }

    // --- Loop and selection under the waveform.
    if (fs.hasLoop())
        dl->AddRectFilled(ImVec2(xls, top), ImVec2(xle, o.y + height), kLoop);
    if (v.hasSel()) {
        const float a = v.xOf(v.selA, x0), b = v.xOf(v.selB, x0);
        dl->AddRectFilled(ImVec2(a, top), ImVec2(b, o.y + height), kSel);
        dl->AddLine(ImVec2(a, top), ImVec2(a, o.y + height), kSelEdge);
        dl->AddLine(ImVec2(b, top), ImVec2(b, o.y + height), kSelEdge);
    }

    // --- The waveform, lane by lane.
    const auto& lods = d.lods();
    const float vz = m_vzoom;
    for (int c = 0; c < ch; ++c) {
        const float ly = top + c * (laneH + laneGap);
        const float mid = ly + laneH * 0.5f, amp = laneH * 0.5f * vz;
        if (c > 0) dl->AddLine(ImVec2(x0, ly - laneGap * 0.5f), ImVec2(x0 + W, ly - laneGap * 0.5f), kLaneLine);
        dl->AddLine(ImVec2(x0, mid), ImVec2(x0 + W, mid), kLaneLine);
        for (float g : {0.5f, -0.5f}) {   // -6 dB
            const float y = mid - g * amp;
            if (y > ly && y < ly + laneH) dl->AddLine(ImVec2(x0, y), ImVec2(x0 + W, y), kGrid);
        }
        auto Y = [&](float s) { return std::clamp(mid - s * amp, ly, ly + laneH); };
        if (v.fpp >= 1.0) {
            const wave::Lod* lod = nullptr;
            for (const wave::Lod& l : lods)
                if ((Frames(1) << l.shift) <= v.fpp) lod = &l;
            for (int px = 0; px < static_cast<int>(W); ++px) {
                const Frames f0 = static_cast<Frames>(v.viewStart + px * v.fpp);
                const Frames f1 = std::min(n, std::max(f0 + 1, static_cast<Frames>(v.viewStart + (px + 1) * v.fpp)));
                if (f0 >= n) break;
                float lo = 1e9f, hi = -1e9f, ms = 0.0f;
                int cnt = 0;
                if (lod) {
                    const Frames b0 = f0 >> lod->shift, b1 = std::min<Frames>((f1 - 1) >> lod->shift,
                                                                               static_cast<Frames>(lod->mn[c].size()) - 1);
                    for (Frames b = b0; b <= b1; ++b, ++cnt) {
                        lo = std::min(lo, lod->mn[c][b]); hi = std::max(hi, lod->mx[c][b]); ms += lod->ms[c][b];
                    }
                } else {
                    for (Frames f = f0; f < f1; ++f, ++cnt) {
                        const float s = d.sample(f, c);
                        lo = std::min(lo, s); hi = std::max(hi, s); ms += s * s;
                    }
                }
                if (!cnt) continue;
                const float rms = std::sqrt(ms / cnt);
                const float x = x0 + px + 0.5f;
                dl->AddLine(ImVec2(x, Y(hi)), ImVec2(x, Y(lo) + 1.0f), kPeak);
                dl->AddLine(ImVec2(x, Y(std::min(rms, hi))), ImVec2(x, Y(std::max(-rms, lo)) + 1.0f), kRms);
                if (hi >= 0.999f || lo <= -0.999f)
                    dl->AddLine(ImVec2(x, ly), ImVec2(x, ly + 3.0f), kClip, 1.0f);
            }
        } else {
            // Zoomed in past one sample a pixel: the samples themselves, joined,
            // with a dot on each once they are far enough apart to tell.
            const Frames f0 = std::max<Frames>(0, static_cast<Frames>(std::floor(v.viewStart)) - 1);
            const Frames f1 = std::min<Frames>(n, static_cast<Frames>(std::ceil(v.viewStart + v.fpp * W)) + 2);
            std::vector<ImVec2> pts;
            for (Frames f = f0; f < f1; ++f) pts.push_back(ImVec2(v.xOf(f, x0), Y(d.sample(f, c))));
            if (pts.size() >= 2)
                dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), kRms, ImDrawFlags_None, 1.5f);
            if (1.0 / v.fpp >= 6.0)
                for (const ImVec2& p : pts) {
                    dl->AddLine(ImVec2(p.x, mid), p, IM_COL32(80, 120, 160, 160));
                    dl->AddCircleFilled(p, 2.5f, kPeak);
                }
        }
        if (vz > 1.0f) {
            char b[24];
            std::snprintf(b, sizeof b, "x%.0f", vz);
            dl->AddText(ImVec2(x0 + 4.0f, ly + 2.0f), kText, b);
        }
        if (ch == 2) dl->AddText(ImVec2(x0 + W - 16.0f, ly + 2.0f), kText, c == 0 ? "L" : "R");
    }

    // --- Markers on top: loop points with flags, cursor, playhead.
    if (fs.hasLoop()) {
        for (int k = 0; k < 2; ++k) {
            const float x = k == 0 ? xls : xle;
            dl->AddLine(ImVec2(x, top - 2.0f), ImVec2(x, o.y + height), kLoopLine, 2.0f);
            const char* t = k == 0 ? "Loop" : "End";
            const ImVec2 ts = ImGui::CalcTextSize(t);
            const float fx = k == 0 ? x : x - ts.x - 8.0f;
            dl->AddRectFilled(ImVec2(fx, top - ts.y - 3.0f), ImVec2(fx + ts.x + 8.0f, top), kLoopLine, 2.0f);
            dl->AddText(ImVec2(fx + 4.0f, top - ts.y - 2.0f), IM_COL32(10, 30, 15, 255), t);
        }
    }
    {
        const float x = v.xOf(v.cursor, x0);
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + height), kCursor, 1.0f);
        dl->AddTriangleFilled(ImVec2(x - 5, o.y), ImVec2(x + 5, o.y), ImVec2(x, o.y + 7), kCursor);
    }
    if (playing() && m_playing == &v) {
        const float x = v.xOf(m_player->position(), x0);
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + height), kPlayhead, 2.0f);
    }
    if (hovered && m.y >= top && v.drag == 0) {
        dl->AddLine(ImVec2(m.x, top), ImVec2(m.x, o.y + height), IM_COL32(255, 255, 255, 50));
    }
    dl->PopClipRect();
    contextMenu(v);
}

void WaveEditor::contextMenu(View& v) {
    if (!ImGui::BeginPopup("##waveCtx")) return;
    wave::Document& d = v.doc;
    const Frames at = v.hover >= 0 ? v.hover : v.cursor;
    if (ImGui::MenuItem("Play from here")) { v.cursor = at; play(v, false); }
    ImGui::Separator();
    if (ImGui::MenuItem("Cut", "Ctrl+X", false, v.hasSel())) cutCopy(v, true);
    if (ImGui::MenuItem("Copy", "Ctrl+C", false, v.hasSel())) cutCopy(v, false);
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, m_clip.frames() > 0)) paste(v);
    if (ImGui::MenuItem("Delete", "Del", false, v.hasSel())) {
        const Frames a = v.selA, b = v.selB;
        edit(v, "Delete", [&](wave::Document& dd) { wave::erase(dd, a, b); });
        v.selB = v.selA;
    }
    if (ImGui::MenuItem("Silence", nullptr, false, v.hasSel())) {
        const Frames a = v.selA, b = v.selB;
        const unsigned mk = mask(v);
        edit(v, "Silence", [&](wave::Document& dd) { wave::silence(dd, a, b, mk); });
    }
    ImGui::Separator();
    processItems(v);
    ImGui::Separator();
    if (ImGui::MenuItem("Loop = selection", nullptr, false, v.hasSel())) {
        d.checkpoint("Loop from selection");
        d.data.loopStart = v.selA; d.data.loopEnd = v.selB; d.dirty = true;
    }
    if (ImGui::MenuItem("Loop starts here")) {
        d.checkpoint("Loop start");
        d.data.loopStart = at;
        if (d.data.loopEnd <= at) d.data.loopEnd = d.frames();
        d.dirty = true;
    }
    if (ImGui::MenuItem("Loop ends here", nullptr, false, at > 0)) {
        d.checkpoint("Loop end");
        if (d.data.loopStart < 0 || d.data.loopStart >= at) d.data.loopStart = 0;
        d.data.loopEnd = at;
        d.dirty = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Select all", "Ctrl+A")) { v.selA = 0; v.selB = d.frames(); }
    if (ImGui::MenuItem("Zoom to selection", "Z", false, v.hasSel())) v.show(v.selA, v.selB);
    if (ImGui::MenuItem("Whole sound", "F")) v.fit();
    ImGui::EndPopup();
}

// Gain, Normalize and Reverse for the Process menu and the waveform's right-click
// menu. Each does what its button under the waveform does: on the selection (or
// the whole sound), on the channels picked there, by the amount set there -- which
// the steppers here set as well.
void WaveEditor::processItems(View& v) {
    const char* chan = v.doc.channels() == 2 ? (m_chanMask == 1 ? ", left" : m_chanMask == 2 ? ", right" : "")
                                             : "";
    ImGui::TextDisabled("%s%s", v.hasSel() ? "The selection" : "The whole sound", chan);
    const float w = ui::stepperWidth("%+.1f dB") + ImGui::GetFontSize() * 2.0f;
    char label[64];
    ui::stepper("##procGain", m_gainDb, 0.5f, -48.0f, 48.0f, "%+.1f dB", w);
    std::snprintf(label, sizeof label, "Gain %+.1f dB", m_gainDb);
    if (ImGui::MenuItem(label)) applyGain(v);
    ui::stepper("##procNorm", m_normDb, 0.5f, -40.0f, 0.0f, "%.1f dB", w);
    std::snprintf(label, sizeof label, "Normalize to %.1f dB", m_normDb);
    if (ImGui::MenuItem(label)) applyNormalize(v);
    if (ImGui::MenuItem("Reverse")) applyReverse(v);
}

void WaveEditor::applyGain(View& v) {
    const Frames a = v.lo(), b = v.hi();
    const unsigned mk = mask(v);
    const float g = m_gainDb;
    edit(v, "Gain", [&](wave::Document& dd) { wave::gain(dd, a, b, g, mk); });
}

void WaveEditor::applyNormalize(View& v) {
    const Frames a = v.lo(), b = v.hi();
    const unsigned mk = mask(v);
    const float t = m_normDb;
    float applied = 0.0f;
    edit(v, "Normalize", [&](wave::Document& dd) { applied = wave::normalize(dd, a, b, t, mk); });
    char msg[96];
    std::snprintf(msg, sizeof msg, "Normalized: %+.2f dB.", applied);
    m_deps.status = msg;
}

void WaveEditor::applyReverse(View& v) {
    const Frames a = v.lo(), b = v.hi();
    const unsigned mk = mask(v);
    edit(v, "Reverse", [&](wave::Document& dd) { wave::reverse(dd, a, b, mk); });
}

// --- The tools under the waveform -------------------------------------------------------

void WaveEditor::toolColumns(View& v) {
    // A status line first: where the mouse is, what is selected -- and, on the
    // right, how the waveform is looked at.
    {
        const float lineX = ImGui::GetCursorPosX(), lineW = ImGui::GetContentRegionAvail().x;
        const float em = ImGui::GetFontSize();
        const wave::Document& d = v.doc;
        std::string s = d.channels() == 1 ? "Mono" : d.channels() == 2 ? "Stereo" : std::to_string(d.channels()) + " ch";
        char b[256];
        std::snprintf(b, sizeof b, "%s  %d Hz  %s  |  cursor %s", s.c_str(), d.rate(),
                      d.data.sourceBits ? (std::to_string(d.data.sourceBits) + (d.data.sourceFloat ? "-bit float" : "-bit")).c_str()
                                        : (d.path.empty() ? "new" : "compressed"),
                      fmtTime(v, v.cursor).c_str());
        ImGui::TextDisabled("%s", b);
        if (v.hasSel()) {
            ImGui::SameLine();
            ImGui::TextDisabled("|  selection %s - %s (%s)", fmtTime(v, v.selA).c_str(), fmtTime(v, v.selB).c_str(),
                                fmtTime(v, v.selB - v.selA).c_str());
        }
        if (v.hover >= 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("|  mouse %s  %+.3f (%.1f dB)", fmtTime(v, v.hover).c_str(), v.hoverValue,
                                wave::toDb(std::abs(v.hoverValue)));
        }
        const ImGuiStyle& st = ImGui::GetStyle();
        const float unitsW = em * 9.0f;
        const float rightW = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x +
                             ImGui::CalcTextSize("Snap").x + unitsW + ui::stepperWidth("%.0fx") +
                             2.0f * st.ItemSpacing.x + 4.0f;
        if (ImGui::GetCursorPosX() < lineX + lineW - rightW) ImGui::SameLine(lineX + lineW - rightW);
        ImGui::Checkbox("Snap", &m_snapZero);
        ImGui::SetItemTooltip("Clicks, selections and loop points land on zero crossings -- "
                              "no clicks where the sound is cut.");
        ImGui::SameLine();
        const char* units[] = {"Time", "Samples"};
        choice("##units", m_units, units, 2, unitsW);
        ImGui::SameLine();
        ui::stepper("##vzoom", m_vzoom, 1.0f, 1.0f, 16.0f, "%.0fx");
        ImGui::SetItemTooltip("Vertical zoom: look into the quiet parts.");
    }
    if (!ImGui::BeginTable("##waveTools", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                                                 ImGuiTableFlags_ScrollY,
                           ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupColumn("Loop", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    ImGui::TableSetupColumn("Edit", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Analysis", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    loopTools(v);
    ImGui::TableNextColumn();
    editTools(v);
    ImGui::TableNextColumn();
    analysis(v);
    ImGui::EndTable();
}

namespace {
// A position as a field: seconds or frames, whatever the units are; typed or
// stepped (never dragged).
bool frameField(const char* label, Frames& f, int units, int rate, Frames maxF) {
    bool changed = false;
    if (units == 1) {
        long long x = static_cast<long long>(f);
        const long long step = 1, fast = std::max(1, rate / 100);
        changed = ImGui::InputScalar(label, ImGuiDataType_S64, &x, &step, &fast, "%lld");
        f = static_cast<Frames>(x);
    } else {
        double s = static_cast<double>(f) / std::max(rate, 1);
        changed = ImGui::InputDouble(label, &s, 0.001, 0.01, "%.4f s");
        f = static_cast<Frames>(std::llround(s * rate));
    }
    f = std::clamp<Frames>(f, 0, maxF);
    return changed;
}
} // namespace

void WaveEditor::loopTools(View& v) {
    wave::Document& d = v.doc;
    auto& s = d.data;
    ui::sectionText("Loop points");
    const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (!s.hasLoop()) {
        ImGui::TextDisabled("None. A looping sound plays the whole file round.");
        ImGui::BeginDisabled(!v.hasSel());
        if (ImGui::Button("Loop = selection", ImVec2(bw, 0))) {
            d.checkpoint("Loop from selection");
            s.loopStart = v.selA; s.loopEnd = v.selB; d.dirty = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Loop the whole sound", ImVec2(bw, 0))) {
            d.checkpoint("Loop all");
            s.loopStart = 0; s.loopEnd = d.frames(); d.dirty = true;
        }
        ui::hint("Saved in the WAV (smpl chunk): a looping sound in the game then plays the "
                 "start once and repeats only the loop.");
        return;
    }
    Frames ls = s.loopStart, le = s.loopEnd;
    ImGui::SetNextItemWidth(-90.0f);
    if (frameField("Start##loop", ls, m_units, d.rate(), d.frames())) {
        d.checkpoint("Loop start");
        s.loopStart = std::min(ls, s.loopEnd - 1);
        d.dirty = true;
    }
    ImGui::SetNextItemWidth(-90.0f);
    if (frameField("End##loop", le, m_units, d.rate(), d.frames())) {
        d.checkpoint("Loop end");
        s.loopEnd = std::max(le, s.loopStart + 1);
        d.dirty = true;
    }
    ImGui::TextDisabled("Length %s", fmtTime(v, s.loopEnd - s.loopStart).c_str());
    if (ImGui::Button("Start = cursor", ImVec2(bw, 0)) && v.cursor < s.loopEnd) {
        d.checkpoint("Loop start");
        s.loopStart = v.cursor; d.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("End = cursor", ImVec2(bw, 0)) && v.cursor > s.loopStart) {
        d.checkpoint("Loop end");
        s.loopEnd = v.cursor; d.dirty = true;
    }
    ImGui::BeginDisabled(!v.hasSel());
    if (ImGui::Button("= selection", ImVec2(bw, 0))) {
        d.checkpoint("Loop from selection");
        s.loopStart = v.selA; s.loopEnd = v.selB; d.dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Snap to zero", ImVec2(bw, 0))) {
        d.checkpoint("Loop to zero crossings");
        s.loopStart = wave::nearestZeroCrossing(d, s.loopStart);
        s.loopEnd   = std::max(s.loopStart + 1, wave::nearestZeroCrossing(d, s.loopEnd));
        d.dirty = true;
    }
    ImGui::SetItemTooltip("Both points onto the nearest zero crossing: no click at the wrap.");
    if (ImGui::Button("Find best end", ImVec2(bw, 0))) {
        // Around where the end is now (a quarter of the loop either way), or in
        // the selection when one lies past the start.
        Frames from, to;
        if (v.hasSel() && v.selA > s.loopStart) { from = v.selA; to = v.selB; }
        else {
            const Frames r = std::max<Frames>((s.loopEnd - s.loopStart) / 4, d.rate() / 50);
            from = s.loopEnd - r; to = s.loopEnd + r;
        }
        float score = 0.0f;
        const Frames e = wave::bestLoopEnd(d, s.loopStart, from, to, 2048, &score);
        if (e > s.loopStart) {
            d.checkpoint("Best loop end");
            s.loopEnd = e; d.dirty = true;
            char b[96];
            std::snprintf(b, sizeof b, "Best end at %s, match %.0f%%.", fmtTime(v, e).c_str(), score * 100.0f);
            v.loopNote = b;
        } else {
            v.loopNote = "No room to search there.";
        }
    }
    ImGui::SetItemTooltip("Searches near the current end (or in the selection) for the place "
                          "where the sound goes on most like it does after the loop start.");
    ImGui::SameLine();
    if (ImGui::Button("Audition seam", ImVec2(bw, 0))) {
        // From a second and a half before the end, round the loop: the wrap,
        // over and over.
        m_loopMode = true;
        v.cursor = std::max(s.loopStart, s.loopEnd - static_cast<Frames>(d.rate() * 1.5));
        play(v, false);
    }
    ImGui::SetItemTooltip("Plays the last 1.5 s of the loop into its start, round and round.");
    ui::stepper("##xf", m_xfadeMs, 5.0f, 1.0f, 2000.0f, "%.0f ms", bw);
    ImGui::SameLine();
    if (ImGui::Button("Crossfade seam", ImVec2(bw, 0))) {
        const Frames len = static_cast<Frames>(m_xfadeMs * 0.001f * d.rate());
        d.checkpoint("Crossfade loop");
        if (wave::crossfadeLoop(d, len)) {
            m_deps.status = "Crossfaded the loop seam.";
        } else {
            d.undo();
            m_deps.status = "The crossfade needs that much sound before the loop start.";
        }
    }
    ImGui::SetItemTooltip("Blends the end of the loop into what comes before its start: the "
                          "wrap becomes seamless.");
    if (ImGui::Button("Zoom to loop", ImVec2(bw, 0))) v.show(s.loopStart, s.loopEnd);
    ImGui::SameLine();
    if (ImGui::Button("Remove loop", ImVec2(bw, 0))) {
        d.checkpoint("Remove loop");
        s.loopStart = s.loopEnd = -1; d.dirty = true;
    }
    if (!v.loopNote.empty()) ui::hint("%s", v.loopNote.c_str());
}

void WaveEditor::editTools(View& v) {
    wave::Document& d = v.doc;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float aw  = ImGui::GetContentRegionAvail().x;
    const float b3  = (aw - 2.0f * gap) / 3.0f, b2 = (aw - gap) * 0.5f;
    const Frames a = v.lo(), b = v.hi();
    const unsigned mk = mask(v);
    ui::sectionText(v.hasSel() ? "Edit the selection" : "Edit the whole sound");
    if (d.channels() == 2) {
        const char* chans[] = {"Both", "Left", "Right"};
        choice("##chan", m_chanMask, chans, 3);
    }
    ImGui::BeginDisabled(!v.hasSel());
    if (ImGui::Button("Cut", ImVec2(b3, 0))) cutCopy(v, true);
    ImGui::SameLine();
    if (ImGui::Button("Copy", ImVec2(b3, 0))) cutCopy(v, false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_clip.frames() <= 0);
    if (ImGui::Button("Paste", ImVec2(b3, 0))) paste(v);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!v.hasSel());
    if (ImGui::Button("Delete", ImVec2(b3, 0))) {
        edit(v, "Delete", [&](wave::Document& dd) { wave::erase(dd, a, b); });
        v.selB = v.selA;
    }
    ImGui::SameLine();
    if (ImGui::Button("Crop", ImVec2(b3, 0))) {
        edit(v, "Crop", [&](wave::Document& dd) { wave::crop(dd, a, b); });
        v.selA = 0; v.selB = d.frames(); v.cursor = 0;
        v.fit();
    }
    ImGui::SameLine();
    if (ImGui::Button("Silence", ImVec2(b3, 0)))
        edit(v, "Silence", [&](wave::Document& dd) { wave::silence(dd, a, b, mk); });
    ImGui::EndDisabled();

    ui::stepper("##ins", m_silenceMs, 50.0f, 1.0f, 60000.0f, "%.0f ms", b2);
    ImGui::SameLine();
    if (ImGui::Button("Insert silence", ImVec2(b2, 0))) {
        const Frames at = v.cursor, len = static_cast<Frames>(m_silenceMs * 0.001f * d.rate());
        edit(v, "Insert silence", [&](wave::Document& dd) { wave::insertSilence(dd, at, len); });
        v.selA = at; v.selB = at + len;
    }

    const char* curves[] = {"Lin", "Pow", "Exp", "Log", "S"};
    choice("##curve", m_fadeCurve, curves, 5);
    ImGui::SetItemTooltip("Fade curve: linear, equal power, exponential, logarithmic, S-curve");
    if (ImGui::Button("Fade in", ImVec2(b2, 0))) {
        const auto k = static_cast<wave::FadeCurve>(m_fadeCurve);
        edit(v, "Fade in", [&](wave::Document& dd) { wave::fade(dd, a, b, true, k, mk); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Fade out", ImVec2(b2, 0))) {
        const auto k = static_cast<wave::FadeCurve>(m_fadeCurve);
        edit(v, "Fade out", [&](wave::Document& dd) { wave::fade(dd, a, b, false, k, mk); });
    }

    ui::stepper("##gain", m_gainDb, 0.5f, -48.0f, 48.0f, "%+.1f dB", b2);
    ImGui::SameLine();
    if (ImGui::Button("Gain", ImVec2(b2, 0))) applyGain(v);
    ui::stepper("##norm", m_normDb, 0.5f, -40.0f, 0.0f, "%.1f dB", b2);
    ImGui::SameLine();
    if (ImGui::Button("Normalize", ImVec2(b2, 0))) applyNormalize(v);

    if (ImGui::Button("Reverse", ImVec2(b3, 0))) applyReverse(v);
    ImGui::SameLine();
    if (ImGui::Button("Invert", ImVec2(b3, 0)))
        edit(v, "Invert", [&](wave::Document& dd) { wave::invert(dd, a, b, mk); });
    ImGui::SameLine();
    if (ImGui::Button("Remove DC", ImVec2(b3, 0)))
        edit(v, "Remove DC", [&](wave::Document& dd) { wave::removeDc(dd, a, b, mk); });

    ui::stepper("##trim", m_trimDb, 3.0f, -96.0f, -12.0f, "%.0f dB", b2);
    ImGui::SameLine();
    if (ImGui::Button("Trim silence", ImVec2(b2, 0))) {
        const float t = m_trimDb;
        edit(v, "Trim silence", [&](wave::Document& dd) { wave::trimSilence(dd, t); });
        v.fit();
    }
    ImGui::SetItemTooltip("Cuts what is quieter than this off both ends.");

    ImGui::BeginDisabled(d.channels() == 1);
    if (ImGui::Button("To mono", ImVec2(b2, 0))) edit(v, "To mono", [](wave::Document& dd) { wave::toMono(dd); });
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(d.channels() != 1);
    if (ImGui::Button("To stereo", ImVec2(b2, 0))) edit(v, "To stereo", [](wave::Document& dd) { wave::toStereo(dd); });
    ImGui::EndDisabled();

    const char* rates[] = {"22k", "32k", "44.1k", "48k"};
    choice("##rate", m_rateChoice, rates, 4, b2);
    ImGui::SameLine();
    const int target = kRates[std::clamp(m_rateChoice, 0, 3)];
    ImGui::BeginDisabled(target == d.rate());
    if (ImGui::Button("Resample", ImVec2(b2, 0))) {
        edit(v, "Resample", [&](wave::Document& dd) { wave::resample(dd, target); });
        v.fit();
    }
    ImGui::EndDisabled();
}

void WaveEditor::analysis(View& v) {
    wave::Document& d = v.doc;
    const Frames a = v.lo(), b = v.hi();
    ui::sectionText(v.hasSel() ? "The selection" : "The whole sound");
    if (v.statsVer != d.version() || v.statsA != a || v.statsB != b) {
        v.stats = wave::stats(d, a, b);
        v.statsVer = d.version(); v.statsA = a; v.statsB = b;
    }
    const wave::Stats& st = v.stats;
    if (ImGui::BeginTable("##stats", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", fmtTime(v, b - a).c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled(d.channels() == 1 ? "Mono" : "Left");
        ImGui::TableNextColumn(); if (d.channels() > 1) ImGui::TextDisabled("Right");
        auto row = [&](const char* name, const float* val, const char* fmt) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
            for (int c = 0; c < std::min(d.channels(), 2); ++c) {
                ImGui::TableNextColumn();
                ImGui::Text(fmt, val[c]);
            }
        };
        row("Peak", st.peakDb, "%.1f dB");
        row("RMS", st.rmsDb, "%.1f dB");
        const float dc[2] = {st.dc[0] * 100.0f, st.dc[1] * 100.0f};
        row("DC", dc, "%+.2f %%");
        ImGui::EndTable();
    }
    if (st.clipped > 0) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%d samples at full scale (clipping).", st.clipped);
    else ImGui::TextDisabled("No clipping.");

    // The spectrum, 20 Hz to Nyquist on a log scale, -90..0 dB.
    if (v.specVer != d.version() || v.specA != a || v.specB != b) {
        v.spec = wave::spectrum(d, a, b, 96);
        v.specVer = d.version(); v.specA = a; v.specB = b;
    }
    const float W = ImGui::GetContentRegionAvail().x;
    const float H = ImGui::GetFontSize() * 6.0f;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##spec", ImVec2(W, H));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), kBg, 3.0f);
    const double nyq = d.rate() * 0.5;
    auto xOfHz = [&](double hz) { return o.x + static_cast<float>(std::log(hz / 20.0) / std::log(nyq / 20.0)) * W; };
    for (double hz : {100.0, 1000.0, 10000.0}) {
        if (hz >= nyq) continue;
        const float x = xOfHz(hz);
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + H), kGrid);
        dl->AddText(ImVec2(x + 2.0f, o.y + H - 15.0f), kText, hz >= 1000.0 ? (hz >= 10000.0 ? "10k" : "1k") : "100");
    }
    for (float db : {-30.0f, -60.0f}) {
        const float y = o.y + (-db / 90.0f) * H;
        dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + W, y), kGrid);
    }
    const int nb = static_cast<int>(v.spec.size());
    std::vector<ImVec2> pts;
    for (int i = 0; i < nb; ++i) {
        const float x = o.x + (i + 0.5f) / nb * W;
        const float y = o.y + std::clamp(-v.spec[i] / 90.0f, 0.0f, 1.0f) * H;
        pts.push_back(ImVec2(x, y));
    }
    for (std::size_t i = 0; i + 1 < pts.size(); ++i)
        dl->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, o.y + H), ImVec2(pts[i].x, o.y + H),
                          IM_COL32(80, 170, 235, 70));
    if (pts.size() >= 2) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), kRms, ImDrawFlags_None, 1.5f);
    if (ImGui::IsItemHovered()) {
        const float t = (ImGui::GetIO().MousePos.x - o.x) / W;
        const double hz = 20.0 * std::pow(nyq / 20.0, std::clamp(t, 0.0f, 1.0f));
        const int i = std::clamp(static_cast<int>(t * nb), 0, nb - 1);
        ImGui::SetTooltip("%.0f Hz: %.1f dB", hz, nb ? v.spec[i] : -120.0f);
    }
}

void WaveEditor::shortcuts(View& v) {
    ImGuiIO& io = ImGui::GetIO();
    auto key = [](ImGuiKey k, bool rep = false) { return ImGui::IsKeyPressed(k, rep); };
    wave::Document& d = v.doc;
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift;
    if (ctrl) {
        if (key(ImGuiKey_O)) openDialog();
        if (key(ImGuiKey_S)) save(v, shift);
        if (key(ImGuiKey_Z)) { if (shift) d.redo(); else d.undo(); v.clampAll(); v.clampView(); }
        if (key(ImGuiKey_Y)) { d.redo(); v.clampAll(); v.clampView(); }
        if (key(ImGuiKey_X)) cutCopy(v, true);
        if (key(ImGuiKey_C)) cutCopy(v, false);
        if (key(ImGuiKey_V)) paste(v);
        if (key(ImGuiKey_A)) { v.selA = 0; v.selB = d.frames(); }
        return;
    }
    if (key(ImGuiKey_Space)) {
        if (playing() && m_playing == &v) stop(); else play(v, false);
    }
    if (key(ImGuiKey_Delete) && v.hasSel()) {
        const Frames a = v.selA, b = v.selB;
        edit(v, "Delete", [&](wave::Document& dd) { wave::erase(dd, a, b); });
        v.selB = v.selA;
    }
    if (key(ImGuiKey_Home)) { v.cursor = 0; v.viewStart = 0.0; v.clampView(); }
    if (key(ImGuiKey_End)) { v.cursor = d.frames(); v.viewStart = static_cast<double>(d.frames()); v.clampView(); }
    const Frames nudge = std::max<Frames>(1, static_cast<Frames>(v.fpp * 10.0));
    for (int dir : {-1, 1}) {
        if (!key(dir < 0 ? ImGuiKey_LeftArrow : ImGuiKey_RightArrow, true)) continue;
        const Frames from = v.cursor;
        v.cursor = std::clamp<Frames>(v.cursor + dir * nudge, 0, d.frames());
        if (shift) {
            if (!v.hasSel()) v.anchor = from;
            v.selA = std::min(v.anchor, v.cursor);
            v.selB = std::max(v.anchor, v.cursor);
        } else {
            v.selA = v.selB = v.cursor;
        }
    }
    if (key(ImGuiKey_L)) m_loopMode = !m_loopMode;
    if (key(ImGuiKey_F)) v.fit();
    if (key(ImGuiKey_Z) && v.hasSel()) v.show(v.selA, v.selB);
    if (key(ImGuiKey_Equal) || key(ImGuiKey_KeypadAdd)) v.zoomAt(0.5, v.areaW * 0.5f);
    if (key(ImGuiKey_Minus) || key(ImGuiKey_KeypadSubtract)) v.zoomAt(2.0, v.areaW * 0.5f);
}

} // namespace waveui
