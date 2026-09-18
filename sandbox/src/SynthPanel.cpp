#include "SynthPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include <fitzel/audio/Audio.hpp>
#include <fitzel/audio/Synth.hpp>
#include <fitzel/audio/SynthVoice.hpp>

#include "Pictogram.hpp"
#include "UiStyle.hpp"

namespace synthui {

using fitzel::synth::ModuleDef;
using fitzel::synth::ModuleType;
using fitzel::synth::Patch;
using fitzel::synth::PatchInput;
using fitzel::synth::Wire;

namespace {

constexpr int kScopeFrames = 256;   // one graph block: what a module last made

// --- what a module is made of --------------------------------------------------

struct ParamSpec {
    const char* key;
    const char* label;
    float       step, lo, hi;
    const char* fmt;
    float       def;      // what the engine uses when the patch omits it
    bool        log;      // a knob for this one moves in ratios, not in steps
};

const ParamSpec* paramsFor(ModuleType t, int& count) {
    static const ParamSpec osc[] = {
        {"freq", "Frequency", 5.0f, 0.0f, 20000.0f, "%.1f Hz", 220.0f, true},
        {"fine", "Detune", 0.5f, -200.0f, 200.0f, "%.1f Hz", 0.0f, false},
        {"pulseWidth", "Width", 0.05f, 0.01f, 0.99f, "%.2f", 0.5f, false},
        {"level", "Level", 0.05f, 0.0f, 4.0f, "%.2f x", 1.0f, false},
    };
    static const ParamSpec lfo[] = {
        {"rate", "Rate", 0.1f, 0.01f, 40.0f, "%.2f Hz", 2.0f, true},
        {"depth", "Depth", 0.5f, -10000.0f, 10000.0f, "%.2f", 1.0f, false},
        {"offset", "Centre", 0.5f, -10000.0f, 10000.0f, "%.2f", 0.0f, false},
    };
    static const ParamSpec note[] = {
        {"transpose", "Transpose", 1.0f, -48.0f, 48.0f, "%.0f st", 0.0f, false},
        {"fine", "Detune", 0.5f, -200.0f, 200.0f, "%.1f Hz", 0.0f, false},
    };
    static const ParamSpec filter[] = {
        {"cutoff", "Cutoff", 50.0f, 20.0f, 20000.0f, "%.0f Hz", 1000.0f, true},
        {"q", "Resonance", 0.1f, 0.05f, 20.0f, "%.2f Q", 0.707f, true},
    };
    static const ParamSpec adsr[] = {
        {"attack", "Attack", 0.01f, 0.0f, 10.0f, "%.3f s", 0.01f, false},
        {"decay", "Decay", 0.01f, 0.0f, 10.0f, "%.3f s", 0.1f, false},
        {"sustain", "Sustain", 0.05f, 0.0f, 1.0f, "%.2f", 0.7f, false},
        {"release", "Release", 0.01f, 0.0f, 10.0f, "%.3f s", 0.2f, false},
    };
    static const ParamSpec gain[] = {
        {"gain", "Gain", 0.05f, -4.0f, 4.0f, "%.2f x", 1.0f, false},
    };
    static const ParamSpec mix[] = {
        {"gain", "Gain", 0.05f, -4.0f, 4.0f, "%.2f x", 1.0f, false},
        {"level1", "In 1", 0.05f, 0.0f, 2.0f, "%.2f", 1.0f, false},
        {"level2", "In 2", 0.05f, 0.0f, 2.0f, "%.2f", 1.0f, false},
        {"level3", "In 3", 0.05f, 0.0f, 2.0f, "%.2f", 1.0f, false},
        {"level4", "In 4", 0.05f, 0.0f, 2.0f, "%.2f", 1.0f, false},
    };
    static const ParamSpec delay[] = {
        {"time", "Time", 0.01f, 0.001f, 2.0f, "%.3f s", 0.25f, false},
        {"feedback", "Feedback", 0.05f, 0.0f, 0.95f, "%.2f", 0.3f, false},
        {"mix", "Mix", 0.05f, 0.0f, 1.0f, "%.2f", 0.5f, false},
    };
    static const ParamSpec drive[] = {
        {"drive", "Drive", 0.25f, 0.0f, 50.0f, "%.2f x", 2.0f, true},
        {"mix", "Mix", 0.05f, 0.0f, 1.0f, "%.2f", 1.0f, false},
    };
    static const ParamSpec chorus[] = {
        {"rate", "Rate", 0.05f, 0.01f, 10.0f, "%.2f Hz", 0.6f, true},
        {"depth", "Depth", 0.0005f, 0.0f, 0.01f, "%.4f s", 0.002f, false},
        {"mix", "Mix", 0.05f, 0.0f, 1.0f, "%.2f", 0.5f, false},
    };
    static const ParamSpec reverb[] = {
        {"size", "Size", 0.05f, 0.0f, 1.0f, "%.2f", 0.5f, false},
        {"damp", "Damping", 0.05f, 0.0f, 0.95f, "%.2f", 0.4f, false},
        {"mix", "Mix", 0.05f, 0.0f, 1.0f, "%.2f", 0.3f, false},
    };
    static const ParamSpec map[] = {
        {"inMin", "From", 10.0f, -100000.0f, 100000.0f, "%.1f", 0.0f, false},
        {"inMax", "To", 10.0f, -100000.0f, 100000.0f, "%.1f", 1.0f, false},
        {"outMin", "Gives", 5.0f, -100000.0f, 100000.0f, "%.1f", 0.0f, false},
        {"outMax", "...up to", 5.0f, -100000.0f, 100000.0f, "%.1f", 1.0f, false},
        {"curve", "Curve", 0.1f, 0.1f, 6.0f, "%.2f", 1.0f, false},
    };
    static const ParamSpec param[] = {
        {"smooth", "Smoothing", 0.01f, 0.0f, 2.0f, "%.3f s", 0.02f, false},
    };
    static const ParamSpec konst[] = {
        {"value", "Value", 0.1f, -100000.0f, 100000.0f, "%.2f", 0.0f, false},
    };

    switch (t) {
    case ModuleType::Osc:    count = 4; return osc;
    case ModuleType::Lfo:    count = 3; return lfo;
    case ModuleType::Note:   count = 2; return note;
    case ModuleType::Filter: count = 2; return filter;
    case ModuleType::Adsr:   count = 4; return adsr;
    case ModuleType::Gain:
    case ModuleType::Out:    count = 1; return gain;
    case ModuleType::Mix:    count = 5; return mix;
    case ModuleType::Delay:  count = 3; return delay;
    case ModuleType::Drive:  count = 2; return drive;
    case ModuleType::Chorus: count = 3; return chorus;
    case ModuleType::Reverb: count = 3; return reverb;
    case ModuleType::Map:    count = 5; return map;
    case ModuleType::Param:  count = 1; return param;
    case ModuleType::Const:  count = 1; return konst;
    }
    count = 0;
    return nullptr;
}

const char* portName(ModuleType t, int port) {
    switch (t) {
    case ModuleType::Osc:    return port == 0 ? "freq" : "amp";
    case ModuleType::Lfo:    return "rate";
    case ModuleType::Note:   return "note";
    case ModuleType::Filter: return port == 0 ? "in" : "cutoff";
    case ModuleType::Adsr:   return "gate";
    case ModuleType::Gain:   return port == 0 ? "in" : "gain";
    case ModuleType::Delay:  return port == 0 ? "in" : "time";
    case ModuleType::Drive:  return port == 0 ? "in" : "drive";
    case ModuleType::Map:    return "value";
    case ModuleType::Mix:
        return port == 0 ? "in 1" : (port == 1 ? "in 2" : (port == 2 ? "in 3" : "in 4"));
    default: return "in";
    }
}

const char* modeLabel(ModuleType t) {
    switch (t) {
    case ModuleType::Osc:
    case ModuleType::Lfo:    return "Wave";
    case ModuleType::Filter: return "Mode";
    case ModuleType::Drive:  return "Shape";
    case ModuleType::Param:  return "Dial";
    default:                 return nullptr;
    }
}

// The colour a module is drawn in: what KIND of thing it is, at a glance. A
// patch is read by following the signal, and colour is the fastest way to see
// where sound is made, where it is shaped and where it leaves.
ImU32 moduleColour(ModuleType t) {
    switch (t) {
    case ModuleType::Osc:
    case ModuleType::Lfo:    return IM_COL32(196, 118, 40, 255);    // sources
    case ModuleType::Filter: return IM_COL32(52, 108, 176, 255);    // shaping
    case ModuleType::Adsr:   return IM_COL32(60, 140, 88, 255);     // time
    case ModuleType::Gain:
    case ModuleType::Mix:    return IM_COL32(48, 132, 132, 255);    // level
    case ModuleType::Delay:
    case ModuleType::Drive:
    case ModuleType::Chorus:
    case ModuleType::Reverb: return IM_COL32(120, 72, 168, 255);    // effects
    case ModuleType::Out:    return IM_COL32(172, 58, 58, 255);     // the way out
    default:                 return IM_COL32(88, 92, 104, 255);     // numbers
    }
}

std::string patchDirOf(const std::string& projectFile) {
    if (projectFile.empty()) return {};
    const std::filesystem::path folder = std::filesystem::path(projectFile).parent_path();
    return (folder / "content" / "patches").generic_string();
}

// A patch worth opening on: a note goes in, a sound comes out, and every wire
// that makes that work is already drawn. An empty canvas teaches nothing about
// how the pieces fit together.
Patch defaultPatch() {
    Patch p;
    p.name = "new patch";
    p.inputs.push_back({"pitch", 24.0f, 96.0f, 48.0f});
    p.inputs.push_back({"gate", 0.0f, 1.0f, 0.0f});

    auto mk = [](int id, ModuleType t, const char* mode, float x, float y) {
        ModuleDef m;
        m.id   = id;
        m.type = t;
        m.mode = mode;
        m.x    = x;
        m.y    = y;
        int              count = 0;
        const ParamSpec* specs = paramsFor(t, count);
        for (int k = 0; k < count; ++k) m.setParam(specs[k].key, specs[k].def);
        return m;
    };

    // No positions: the panel lays them out on the first frame, when it knows
    // how big a tile is.
    ModuleDef pitch = mk(1, ModuleType::Param, "pitch", 0.0f, 0.0f);
    pitch.setParam("smooth", 0.0f);
    ModuleDef note = mk(2, ModuleType::Note, "", 0.0f, 0.0f);
    ModuleDef osc  = mk(3, ModuleType::Osc, "saw", 0.0f, 0.0f);
    osc.setParam("freq", 0.0f);          // the pitch comes down the wire
    ModuleDef gate = mk(4, ModuleType::Param, "gate", 0.0f, 0.0f);
    gate.setParam("smooth", 0.0f);
    ModuleDef env = mk(5, ModuleType::Adsr, "", 0.0f, 0.0f);
    ModuleDef flt = mk(6, ModuleType::Filter, "lp", 0.0f, 0.0f);
    flt.setParam("cutoff", 2200.0f);
    ModuleDef vca = mk(7, ModuleType::Gain, "", 0.0f, 0.0f);
    ModuleDef out = mk(8, ModuleType::Out, "", 0.0f, 0.0f);
    out.setParam("gain", 0.8f);

    p.modules = {pitch, note, osc, gate, env, flt, vca, out};
    p.wires   = {{1, 2, 0}, {2, 3, 0}, {3, 6, 0}, {6, 7, 0},
                 {4, 5, 0}, {5, 7, 1}, {7, 8, 0}};
    return p;
}

// Lay a patch out left to right by how far each module is from the source side,
// for one that carries no positions (or when the user asks to tidy up). `em` is
// the UI font size, which is what the tiles are measured in: laying out in fixed
// pixels put them on top of each other on a large font.
void autoLayout(Patch& p, float em) {
    const std::size_t n = p.modules.size();
    std::vector<int>  depth(n, 0);
    for (std::size_t pass = 0; pass < n; ++pass)
        for (const Wire& w : p.wires) {
            std::size_t from = n, to = n;
            for (std::size_t i = 0; i < n; ++i) {
                if (p.modules[i].id == w.from) from = i;
                if (p.modules[i].id == w.to)   to   = i;
            }
            if (from < n && to < n) depth[to] = std::max(depth[to], depth[from] + 1);
        }

    std::vector<int> used;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t d = static_cast<std::size_t>(depth[i]);
        if (used.size() <= d) used.resize(d + 1, 0);
        p.modules[i].x = em * 0.8f + static_cast<float>(d) * (em * 12.0f);
        p.modules[i].y = em * 1.2f + static_cast<float>(used[d]) * (em * 11.0f);
        ++used[d];
    }
}

// --- controls ------------------------------------------------------------------

// A knob. Drag it, roll the wheel over it, or double-click to type the number --
// three ways in, because this editor has a standing rule about not depending on
// a steady hand and a knob alone would break it. The wheel is the one that needs
// no aim at all: park the pointer on it and turn.
bool knob(const char* label, float& v, const ParamSpec& sp, float size) {
    ImGui::PushID(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##k", ImVec2(size, size));
    const bool active  = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    bool       changed = false;

    const float range = sp.hi - sp.lo;
    if (active) {
        const float d = -ImGui::GetIO().MouseDelta.y;   // up is more
        if (d != 0.0f) {
            v += d * range / (ImGui::GetIO().KeyShift ? 2000.0f : 200.0f);
            changed = true;
        }
    }
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        v += ImGui::GetIO().MouseWheel * sp.step * (ImGui::GetIO().KeyShift ? 0.1f : 1.0f);
        changed = true;
    }
    if (changed) v = std::clamp(v, sp.lo, sp.hi);

    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        ImGui::OpenPopup("##type");
    if (ImGui::BeginPopup("##type")) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputFloat("##v", &v, 0.0f, 0.0f, "%.4f",
                              ImGuiInputTextFlags_EnterReturnsTrue)) {
            v       = std::clamp(v, sp.lo, sp.hi);
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // The dial: a ring, a filled arc for where it stands, and a pointer.
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    const float  r = size * 0.5f - 2.0f;
    float        t = range != 0.0f ? (v - sp.lo) / range : 0.0f;
    if (sp.log) {
        // A logarithmic parameter has its useful half squeezed into the first
        // few per cent of a linear dial -- a cutoff sweep would be unusable.
        const float lo = std::max(sp.lo, 0.0001f);
        const float x  = std::max(v, lo);
        t = std::log(x / lo) / std::log(std::max(sp.hi, lo * 1.001f) / lo);
    }
    t = std::clamp(t, 0.0f, 1.0f);
    const float a0 = 2.4f, a1 = 2.4f + 4.4f;      // the gap sits at the bottom
    const float a  = a0 + (a1 - a0) * t;
    dl->PathArcTo(c, r, a0, a1, 32);
    dl->PathStroke(IM_COL32(60, 62, 70, 255), 0, 3.0f);
    dl->PathArcTo(c, r, a0, a, 32);
    dl->PathStroke(IM_COL32(230, 168, 60, 255), 0, 3.0f);
    dl->AddLine(ImVec2(c.x + std::cos(a) * r * 0.35f, c.y + std::sin(a) * r * 0.35f),
                ImVec2(c.x + std::cos(a) * r * 0.95f, c.y + std::sin(a) * r * 0.95f),
                IM_COL32(235, 235, 240, 255), 2.0f);
    if (hovered || active) dl->AddCircle(c, r + 1.5f, IM_COL32(255, 200, 90, 160), 24, 1.5f);

    char buf[48];
    std::snprintf(buf, sizeof buf, sp.fmt, v);
    ImGui::SetItemTooltip("%s\n%s\ndrag, wheel, or double-click to type", sp.label, buf);
    ImGui::PopID();
    return changed;
}

} // namespace

struct Panel::State {
    Patch       patch = defaultPatch();
    std::string file;
    std::string status;
    bool        statusBad = false;

    fitzel::SynthVoice   voice;
    fitzel::synth::Graph scope;
    bool                 scopeOk     = false;
    bool                 needsBuild  = true;
    bool                 needsParams = false;
    float                volume      = 0.7f;

    int selected     = -1;   // module id shown in the inspector
    int wiringModule = -1;   // an input waiting for a source...
    int wiringPort   = -1;
    int wiringFrom   = -1;   // ...or an output waiting for a destination
    int dragModule   = -1;

    bool needsLayout = true; // positions still to be worked out (see autoLayout)
    int  heldNote    = -1;   // the key the mouse is holding down
    ImVec2 addAt{0.0f, 0.0f}; // where the canvas was right-clicked
    bool hold     = false;   // keep the gate open after letting go

    std::vector<float>       scopeBuf;
    std::vector<std::string> files;
    char                     nameBuf[64] = {};

    // Undo, one whole patch per step. A patch is a few kilobytes of plain data,
    // so keeping copies is simpler and more reliable than describing each edit
    // -- and an editor where a wrong click cannot be taken back is one people
    // stop trying things in.
    std::vector<Patch> undo;
    std::vector<Patch> redo;

    void mark() {                        // call BEFORE changing the patch
        undo.push_back(patch);
        if (undo.size() > 64) undo.erase(undo.begin());
        redo.clear();
    }
};

Panel::Panel() : m_state(std::make_unique<State>()) {
    std::snprintf(m_state->nameBuf, sizeof m_state->nameBuf, "%s",
                  m_state->patch.name.c_str());
    m_state->scopeBuf.resize(kScopeFrames, 0.0f);
}

Panel::~Panel() = default;

void Panel::draw(bool& show, fitzel::Audio& audio, const std::string& projectFile) {
    if (!show) return;
    State& st = *m_state;

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 62.0f,
                                    ImGui::GetFontSize() * 40.0f),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Synth", &show)) {
        ImGui::End();
        return;
    }

    const float em = ImGui::GetFontSize();
    if (st.needsLayout) {
        autoLayout(st.patch, em);
        st.needsLayout = false;
    }
    // What a press asked for, run after the last widget: adding or deleting a
    // module renumbers the list the loops below are walking.
    std::function<void()> pending;
    const std::string     dir = patchDirOf(projectFile);

    // =====================================================================
    // The bar: the file, the sound, and what it is doing
    // =====================================================================
    ImGui::SetNextItemWidth(em * 9.0f);
    if (ImGui::InputText("##name", st.nameBuf, sizeof st.nameBuf))
        st.patch.name = st.nameBuf;
    ImGui::SameLine();
    if (picto::button("new", picto::Icon::New,
                      "New patch\nStarts from a small playable voice")) {
        pending = [&st] {
            st.mark();
            st.patch = defaultPatch();
            st.file.clear();
            std::snprintf(st.nameBuf, sizeof st.nameBuf, "%s", st.patch.name.c_str());
            st.needsBuild  = true;
            st.needsLayout = true;
            st.selected    = -1;
        };
    }
    ImGui::SameLine();
    if (picto::button("save", picto::Icon::Save,
                      dir.empty() ? "Save\n(open a project first: patches live in it)"
                                  : "Save\ninto the project's content/patches",
                      !dir.empty())) {
        pending = [&st, dir] {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::string stem = st.patch.name.empty() ? std::string("patch") : st.patch.name;
            for (char& c : stem)
                if (c == '/' || c == '\\' || c == ':' || c == '"') c = '_';
            const std::string path = dir + "/" + stem + ".json";
            st.statusBad = !st.patch.save(path);
            st.status    = st.statusBad ? "could not write " + path : "saved " + stem + ".json";
            if (!st.statusBad) st.file = path;
        };
    }
    ImGui::SameLine();
    if (picto::button("open", picto::Icon::Open,
                      dir.empty() ? "Open a patch\n(open a project first)"
                                  : "Open a patch from this project",
                      !dir.empty())) {
        st.files.clear();
        std::error_code ec;
        if (std::filesystem::is_directory(dir, ec))
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
                if (e.is_regular_file() && e.path().extension() == ".json")
                    st.files.push_back(e.path().generic_string());
        ImGui::OpenPopup("##openpatch");
    }
    if (ImGui::BeginPopup("##openpatch")) {
        if (st.files.empty()) ImGui::TextDisabled("no patches in this project yet");
        for (const std::string& f : st.files)
            if (ImGui::Selectable(
                    std::filesystem::path(f).filename().generic_string().c_str())) {
                pending = [&st, f] {
                    Patch       loaded;
                    std::string err;
                    if (Patch::load(f, loaded, &err)) {
                        st.mark();
                        st.patch = std::move(loaded);
                        st.file  = f;
                        std::snprintf(st.nameBuf, sizeof st.nameBuf, "%s",
                                      st.patch.name.c_str());
                        bool placed = false;
                        for (const ModuleDef& m : st.patch.modules)
                            placed = placed || m.x != 0.0f || m.y != 0.0f;
                        st.needsLayout = !placed;
                        st.needsBuild = true;
                        st.status     = "loaded " + st.patch.name;
                        st.statusBad  = false;
                    } else {
                        st.status    = err;
                        st.statusBad = true;
                    }
                };
            }
        ImGui::EndPopup();
    }

    ImGui::SameLine(0.0f, em);
    if (picto::button("undo", picto::Icon::Undo, "Undo", !st.undo.empty())) {
        pending = [&st] {
            st.redo.push_back(st.patch);
            st.patch = st.undo.back();
            st.undo.pop_back();
            st.needsBuild = true;
        };
    }
    ImGui::SameLine();
    if (picto::button("redo", picto::Icon::Redo, "Redo", !st.redo.empty())) {
        pending = [&st] {
            st.undo.push_back(st.patch);
            st.patch = st.redo.back();
            st.redo.pop_back();
            st.needsBuild = true;
        };
    }
    ImGui::SameLine();
    if (picto::button("tidy", picto::Icon::Tidy,
                      "Tidy up\nLays the modules out left to right, by signal flow")) {
        pending = [&st, em] {
            st.mark();
            autoLayout(st.patch, em);
        };
    }

    ImGui::SameLine(0.0f, em);
    const bool playing = st.voice.isPlaying();
    if (picto::button("listen", playing ? picto::Icon::Stop : picto::Icon::Play,
                      playing ? "Stop" : "Listen\nPlays the patch as it is now",
                      true, playing)) {
        if (playing) {
            st.voice.stop();
        } else {
            std::string          err;
            fitzel::SynthVoice v = fitzel::SynthVoice::fromPatch(audio, st.patch, &err);
            if (v.isValid()) {
                // The old voice goes only once the new one exists -- see
                // Audio.hpp on what tearing a playing voice down costs.
                st.voice = std::move(v);
                st.voice.setVolume(st.volume);
                for (const PatchInput& in : st.patch.inputs)
                    st.voice.setInput(in.name, in.value);
                st.voice.play();
                st.status    = "playing";
                st.statusBad = false;
            } else {
                st.status    = err;
                st.statusBad = true;
            }
        }
    }
    // Volume: a loudspeaker, and the number as a percentage -- the one unit of
    // loudness that needs no explaining.
    ImGui::SameLine(0.0f, em * 0.6f);
    {
        const float  ps = picto::size();
        const ImVec2 p  = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(ps * 0.8f, ps));
        picto::draw(ImGui::GetWindowDrawList(), picto::Icon::Speaker,
                    ImVec2(p.x + ps * 0.4f, p.y + ps * 0.5f), ps * 0.3f,
                    ImGui::GetColorU32(ImGuiCol_Text));
        ImGui::SetItemTooltip("Volume of the preview");
    }
    ImGui::SameLine();
    float volPct = st.volume * 100.0f;
    if (ui::stepper("vol", volPct, 5.0f, 0.0f, 100.0f, "%.0f %%", em * 6.0f)) {
        st.volume = volPct / 100.0f;
        st.voice.setVolume(st.volume);
    }

    // The output, and a meter that measures what came BACK rather than what was
    // asked for (the mixer's own meters do the latter; see MixerPanel.hpp).
    {
        // The preview graph runs EVERY frame, playing or not: its buffers are
        // what the little scope in each tile draws, and freezing them the
        // moment the sound starts would blank the display exactly when there is
        // something to watch. The output line prefers the voice, because that
        // is the signal actually leaving the mixer.
        int n = 0;
        if (st.scopeOk) {
            st.scope.process(st.scopeBuf.data(), kScopeFrames);
            n = kScopeFrames;
        }
        if (playing) n = st.voice.peek(st.scopeBuf.data(), kScopeFrames);
        float pk = 0.0f;
        for (int i = 0; i < n; ++i)
            pk = std::max(pk, std::fabs(st.scopeBuf[static_cast<std::size_t>(i)]));
        const float span = std::max(0.1f, pk * 1.15f);
        ImGui::SameLine(0.0f, em);
        ImGui::PlotLines("##out", st.scopeBuf.data(), n, 0, nullptr, -span, span,
                         ImVec2(em * 9.0f, em * 1.6f));
        ImGui::SameLine();
        ImGui::ProgressBar(std::min(1.0f, pk), ImVec2(em * 4.0f, em * 1.0f), "");
        if (!st.status.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(st.statusBad ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f)
                                            : ImVec4(0.55f, 0.8f, 0.55f, 1.0f),
                               "%s", st.status.c_str());
        }
    }
    ImGui::Separator();

    // =====================================================================
    // The canvas
    // =====================================================================
    const float inspectorW = em * 15.0f;
    const float keysH      = em * 6.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.095f, 0.11f, 1.0f));
    ImGui::BeginChild("##canvas", ImVec2(-inspectorW - em, -keysH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        ImDrawList*  dl     = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();   // where patch (0,0) sits
        const float  tileW  = em * 9.0f;

        // Header, then a row per input, then a row per setting shown, then the
        // scope. Stacked rather than overlaid: the first version let the
        // parameter lines run into the waveform, which made both unreadable.
        const int kShownParams = 3;
        auto tileRows = [&](const ModuleDef& m) {
            int count = 0;
            paramsFor(m.type, count);
            return fitzel::synth::inputCount(m.type) + std::min(count, kShownParams);
        };
        auto tileHeight = [&](const ModuleDef& m) {
            return em * 2.1f + static_cast<float>(tileRows(m)) * em * 1.15f + em * 2.9f;
        };
        auto tilePos    = [&](const ModuleDef& m) {
            return ImVec2(origin.x + m.x, origin.y + m.y);
        };
        auto inPortPos  = [&](const ModuleDef& m, int port) {
            const ImVec2 p = tilePos(m);
            return ImVec2(p.x, p.y + em * 2.1f + em * 1.15f * (static_cast<float>(port) + 0.5f));
        };
        auto outPortPos = [&](const ModuleDef& m) {
            const ImVec2 p = tilePos(m);
            return ImVec2(p.x + tileW, p.y + em * 2.1f + em * 0.6f);
        };

        // The background: somewhere to scroll to, and somewhere to right-click.
        // Submitted FIRST and allowed to be overlapped, so every tile and port
        // after it takes the pointer before it does. (It used to go in last, on
        // the theory that it would stay out of the way -- instead it sat on top
        // of everything and nothing on the canvas could be pressed.)
        ImGui::SetCursorScreenPos(origin);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##bg", ImVec2(1800.0f, 1200.0f));
        if (ImGui::IsItemClicked()) {
            st.wiringModule = -1;
            st.wiringPort   = -1;
            st.wiringFrom   = -1;
            st.selected     = -1;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            // Where the click was, kept: by the time a module is picked from the
            // menu the pointer is over the menu, not over the canvas.
            st.addAt = ImVec2(ImGui::GetIO().MousePos.x - origin.x,
                              ImGui::GetIO().MousePos.y - origin.y);
            ImGui::OpenPopup("##addmod");
        }

        // --- the cables, under the tiles ---------------------------------
        for (const Wire& w : st.patch.wires) {
            const ModuleDef* a = st.patch.find(w.from);
            const ModuleDef* b = st.patch.find(w.to);
            if (!a || !b) continue;
            const ImVec2 p0  = outPortPos(*a);
            const ImVec2 p1  = inPortPos(*b, w.port);
            const float  d   = std::max(40.0f, std::fabs(p1.x - p0.x) * 0.5f);
            const bool   lit = !st.scopeOk || st.scope.reaches(b->id);
            dl->AddBezierCubic(p0, ImVec2(p0.x + d, p0.y), ImVec2(p1.x - d, p1.y), p1,
                               IM_COL32(12, 12, 15, 220), 5.0f);
            dl->AddBezierCubic(p0, ImVec2(p0.x + d, p0.y), ImVec2(p1.x - d, p1.y), p1,
                               lit ? IM_COL32(220, 190, 120, 230)
                                   : IM_COL32(110, 112, 120, 140),
                               2.5f);
        }

        // --- the tiles ----------------------------------------------------
        for (std::size_t mi = 0; mi < st.patch.modules.size(); ++mi) {
            ModuleDef&   m     = st.patch.modules[mi];
            const ImVec2 p0    = tilePos(m);
            const float  th    = tileHeight(m);
            const ImVec2 p1(p0.x + tileW, p0.y + th);
            const bool   sel   = st.selected == m.id;
            const bool   heard = !st.scopeOk || st.scope.reaches(m.id);
            const ImU32  col   = moduleColour(m.type);
            const int    alpha = heard ? 255 : 115;

            ImGui::PushID(m.id);
            // The body takes the click (select, drag); the ports are allowed to
            // sit on top of it and take theirs.
            ImGui::SetCursorScreenPos(p0);
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##tile", ImVec2(tileW, th));
            if (ImGui::IsItemActivated()) st.selected = m.id;
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                if (st.dragModule != m.id) {
                    st.mark();                     // one undo step for the whole drag
                    st.dragModule = m.id;
                }
                m.x += ImGui::GetIO().MouseDelta.x;
                m.y += ImGui::GetIO().MouseDelta.y;
            }
            if (st.dragModule == m.id && !ImGui::IsItemActive()) st.dragModule = -1;
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("##tilemenu");

            dl->AddRectFilled(p0, p1, IM_COL32(30, 31, 36, alpha), 6.0f);
            dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + em * 1.7f),
                              IM_COL32(static_cast<int>((col >> IM_COL32_R_SHIFT) & 0xFF),
                                       static_cast<int>((col >> IM_COL32_G_SHIFT) & 0xFF),
                                       static_cast<int>((col >> IM_COL32_B_SHIFT) & 0xFF),
                                       alpha),
                              6.0f, ImDrawFlags_RoundCornersTop);
            dl->AddRect(p0, p1, sel ? IM_COL32(255, 200, 90, 255) : IM_COL32(12, 12, 15, 220),
                        6.0f, 0, sel ? 2.5f : 1.5f);

            char head[80];
            std::snprintf(head, sizeof head, "%s%s%s",
                          m.name.empty() ? fitzel::synth::typeName(m.type) : m.name.c_str(),
                          m.mode.empty() ? "" : "  ", m.mode.c_str());
            dl->AddText(ImVec2(p0.x + 7.0f, p0.y + em * 0.3f),
                        IM_COL32(250, 250, 252, alpha), head);
            if (!heard)
                dl->AddText(ImVec2(p1.x - em * 2.4f, p0.y + em * 0.3f),
                            IM_COL32(255, 205, 205, 200), "mute");

            // Inputs, each a port you can press.
            const int ports = fitzel::synth::inputCount(m.type);
            for (int p = 0; p < ports; ++p) {
                const ImVec2 pp = inPortPos(m, p);
                int          from = -1;
                for (const Wire& w : st.patch.wires)
                    if (w.to == m.id && w.port == p) from = w.from;

                ImGui::SetCursorScreenPos(ImVec2(pp.x - em * 0.55f, pp.y - em * 0.55f));
                ImGui::PushID(p);
                ImGui::SetNextItemAllowOverlap();
                ImGui::InvisibleButton("##in", ImVec2(em * 1.1f, em * 1.1f));
                const bool hot = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked()) {
                    if (st.wiringFrom >= 0 && st.wiringFrom != m.id) {
                        const int to = m.id, port = p, src = st.wiringFrom;
                        pending = [&st, to, port, src] {
                            st.mark();
                            auto& w = st.patch.wires;
                            w.erase(std::remove_if(w.begin(), w.end(),
                                                   [&](const Wire& x) {
                                                       return x.to == to && x.port == port;
                                                   }),
                                    w.end());
                            w.push_back({src, to, port});
                            st.wiringFrom = -1;
                            st.needsBuild = true;
                        };
                    } else if (from >= 0) {
                        // A plugged input: pressing it pulls the cable out, which
                        // is the same gesture as pulling a real one.
                        const int to = m.id, port = p;
                        pending = [&st, to, port] {
                            st.mark();
                            auto& w = st.patch.wires;
                            w.erase(std::remove_if(w.begin(), w.end(),
                                                   [&](const Wire& x) {
                                                       return x.to == to && x.port == port;
                                                   }),
                                    w.end());
                            st.needsBuild = true;
                        };
                    } else {
                        st.wiringModule = m.id;
                        st.wiringPort   = p;
                        st.wiringFrom   = -1;
                    }
                }
                ImGui::PopID();

                const bool  waiting = (st.wiringModule == m.id && st.wiringPort == p);
                const ImU32 pc      = waiting  ? IM_COL32(255, 190, 70, 255)
                                      : from >= 0 ? IM_COL32(220, 190, 120, 255)
                                                  : IM_COL32(130, 134, 145, 255);
                dl->AddCircleFilled(pp, (hot || waiting) ? em * 0.42f : em * 0.3f, pc, 16);
                dl->AddCircle(pp, em * 0.42f, IM_COL32(12, 12, 15, 220), 16, 1.5f);
                dl->AddText(ImVec2(pp.x + em * 0.6f, pp.y - em * 0.5f),
                            IM_COL32(200, 203, 212, alpha), portName(m.type, p));
                if (hot)
                    ImGui::SetTooltip("%s -- press this, then press an output\n"
                                      "(pressing a plugged input pulls the cable out)",
                                      portName(m.type, p));
            }

            // The one output.
            if (m.type != ModuleType::Out) {
                const ImVec2 op = outPortPos(m);
                ImGui::SetCursorScreenPos(ImVec2(op.x - em * 0.55f, op.y - em * 0.55f));
                ImGui::SetNextItemAllowOverlap();
                ImGui::InvisibleButton("##out", ImVec2(em * 1.1f, em * 1.1f));
                const bool hot = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked()) {
                    if (st.wiringModule >= 0 && st.wiringModule != m.id) {
                        const int src = m.id;
                        pending = [&st, src] {
                            st.mark();
                            const int to = st.wiringModule, port = st.wiringPort;
                            auto&     w  = st.patch.wires;
                            w.erase(std::remove_if(w.begin(), w.end(),
                                                   [&](const Wire& x) {
                                                       return x.to == to && x.port == port;
                                                   }),
                                    w.end());
                            w.push_back({src, to, port});
                            st.wiringModule = -1;
                            st.wiringPort   = -1;
                            st.needsBuild   = true;
                        };
                    } else {
                        st.wiringFrom   = (st.wiringFrom == m.id) ? -1 : m.id;
                        st.wiringModule = -1;
                    }
                }
                const bool waiting = st.wiringFrom == m.id;
                dl->AddCircleFilled(op, (hot || waiting) ? em * 0.42f : em * 0.3f,
                                    waiting ? IM_COL32(255, 190, 70, 255)
                                            : IM_COL32(220, 190, 120, 255),
                                    16);
                dl->AddCircle(op, em * 0.42f, IM_COL32(12, 12, 15, 220), 16, 1.5f);
                if (hot) ImGui::SetTooltip("output -- press this, then press an input");
            }

            // What it is set to...
            int              count = 0;
            const ParamSpec* specs = paramsFor(m.type, count);
            float            ty = p0.y + em * 2.1f + static_cast<float>(ports) * em * 1.15f;
            for (int k = 0; k < std::min(count, kShownParams); ++k) {
                char val[32], line[64];
                std::snprintf(val, sizeof val, specs[k].fmt, m.param(specs[k].key, specs[k].def));
                std::snprintf(line, sizeof line, "%s %s", specs[k].label, val);
                dl->AddText(ImVec2(p0.x + 7.0f, ty), IM_COL32(168, 172, 182, alpha), line);
                ty += em * 1.15f;
            }

            // ...and what it is making. The fastest answer there is to "why can
            // I not hear this": follow the tiles until one of them is flat.
            if (st.scopeOk) {
                int          n   = 0;
                const float* buf = st.scope.buffer(m.id, n);
                if (buf && n > 1) {
                    const ImVec2 s0(p0.x + 6.0f, p1.y - em * 2.3f);
                    const ImVec2 s1(p1.x - 6.0f, p1.y - em * 0.4f);
                    dl->AddRectFilled(s0, s1, IM_COL32(16, 17, 20, 230), 3.0f);
                    float lo = buf[0], hi = buf[0];
                    for (int i = 1; i < n; ++i) {
                        lo = std::min(lo, buf[i]);
                        hi = std::max(hi, buf[i]);
                    }
                    const float span = std::max(1e-6f, (hi - lo) * 1.15f);
                    const float mid  = (hi + lo) * 0.5f;
                    ImVec2      prev;
                    for (int i = 0; i < n; i += 2) {
                        const float  fx = static_cast<float>(i) / static_cast<float>(n - 1);
                        const ImVec2 q(s0.x + fx * (s1.x - s0.x),
                                       (s0.y + s1.y) * 0.5f -
                                           (buf[i] - mid) / span * (s1.y - s0.y));
                        if (i > 0) dl->AddLine(prev, q, IM_COL32(120, 220, 190, alpha), 1.2f);
                        prev = q;
                    }
                }
            }

            if (ImGui::BeginPopup("##tilemenu")) {
                ImGui::TextDisabled("%s #%d", fitzel::synth::typeName(m.type), m.id);
                ImGui::Separator();
                if (ImGui::Selectable("Duplicate")) {
                    const ModuleDef src = m;
                    pending = [&st, src] {
                        st.mark();
                        ModuleDef copy = src;
                        copy.id = st.patch.nextId();
                        copy.x += 28.0f;
                        copy.y += 28.0f;
                        st.selected = copy.id;
                        st.patch.modules.push_back(std::move(copy));
                        st.needsBuild = true;
                    };
                }
                if (m.type != ModuleType::Out && ImGui::Selectable("Delete")) {
                    const int id = m.id;
                    pending = [&st, id] {
                        st.mark();
                        auto& mods = st.patch.modules;
                        mods.erase(std::remove_if(mods.begin(), mods.end(),
                                                  [id](const ModuleDef& x) { return x.id == id; }),
                                   mods.end());
                        auto& w = st.patch.wires;
                        w.erase(std::remove_if(w.begin(), w.end(),
                                               [id](const Wire& x) {
                                                   return x.from == id || x.to == id;
                                               }),
                                w.end());
                        if (st.selected == id) st.selected = -1;
                        st.needsBuild = true;
                    };
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        // The cable being made, following the pointer.
        if (st.wiringModule >= 0 || st.wiringFrom >= 0) {
            const ImVec2 mouse  = ImGui::GetIO().MousePos;
            ImVec2       anchor = mouse;
            if (st.wiringFrom >= 0) {
                if (const ModuleDef* a = st.patch.find(st.wiringFrom)) anchor = outPortPos(*a);
            } else if (const ModuleDef* b = st.patch.find(st.wiringModule)) {
                anchor = inPortPos(*b, st.wiringPort);
            }
            const float d = std::max(40.0f, std::fabs(mouse.x - anchor.x) * 0.5f);
            dl->AddBezierCubic(anchor, ImVec2(anchor.x + d, anchor.y),
                               ImVec2(mouse.x - d, mouse.y), mouse,
                               IM_COL32(255, 190, 70, 230), 2.5f);
        }

        if (ImGui::BeginPopup("##addmod")) {
            ImGui::TextDisabled("Add a module");
            ImGui::Separator();
            static const ModuleType kAddable[] = {
                ModuleType::Osc,    ModuleType::Lfo,    ModuleType::Note,
                ModuleType::Filter, ModuleType::Adsr,   ModuleType::Gain,
                ModuleType::Mix,    ModuleType::Delay,  ModuleType::Drive,
                ModuleType::Chorus, ModuleType::Reverb, ModuleType::Map,
                ModuleType::Param,  ModuleType::Const,
            };
            for (ModuleType t : kAddable)
                if (ImGui::Selectable(fitzel::synth::typeName(t))) {
                    pending = [&st, t] {
                        st.mark();
                        ModuleDef m;
                        m.id   = st.patch.nextId();
                        m.type = t;
                        m.x    = st.addAt.x;
                        m.y    = st.addAt.y;
                        if (t == ModuleType::Osc || t == ModuleType::Lfo) m.mode = "sine";
                        if (t == ModuleType::Filter) m.mode = "lp";
                        if (t == ModuleType::Drive)  m.mode = "soft";
                        if (t == ModuleType::Param && !st.patch.inputs.empty())
                            m.mode = st.patch.inputs.front().name;
                        int              count = 0;
                        const ParamSpec* specs = paramsFor(t, count);
                        for (int k = 0; k < count; ++k) m.setParam(specs[k].key, specs[k].def);
                        st.selected = m.id;
                        st.patch.modules.push_back(std::move(m));
                        st.needsBuild = true;
                    };
                }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // =====================================================================
    // The inspector
    // =====================================================================
    ImGui::SameLine();
    ImGui::BeginChild("##inspector", ImVec2(inspectorW, -keysH), ImGuiChildFlags_Borders);
    {
        ModuleDef* m = st.selected >= 0 ? st.patch.find(st.selected) : nullptr;
        if (!m) {
            ui::hint("Press a module to set it up here.\n\n"
                     "Right-click the canvas to add one.\n"
                     "Press an input, then an output, to\n"
                     "wire them together -- no dragging.");
        } else {
            ImGui::TextUnformatted(fitzel::synth::typeName(m->type));
            ImGui::SameLine();
            ImGui::TextDisabled("#%d", m->id);

            char nb[48];
            std::snprintf(nb, sizeof nb, "%s", m->name.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputTextWithHint("##mname", "name (optional)", nb, sizeof nb))
                m->name = nb;

            if (const char* ml = modeLabel(m->type)) {
                ImGui::SetNextItemWidth(em * 7.0f);
                if (m->type == ModuleType::Param) {
                    if (ImGui::BeginCombo(ml, m->mode.empty() ? "(none)" : m->mode.c_str())) {
                        for (const PatchInput& in : st.patch.inputs)
                            if (ImGui::Selectable(in.name.c_str(), in.name == m->mode)) {
                                st.mark();
                                m->mode       = in.name;
                                st.needsBuild = true;
                            }
                        ImGui::EndCombo();
                    }
                } else {
                    // Not a drop-down of names: the shapes themselves, side by
                    // side. "saw" means nothing until you have seen one; the
                    // picture of a saw wave is the saw wave. And a row of buttons
                    // is one press, where a list is two and a precise aim.
                    struct Choice { const char* mode; picto::Icon icon; const char* tip; };
                    static const Choice waves[] = {
                        {"sine", picto::Icon::WaveSine, "Sine -- pure and soft"},
                        {"saw", picto::Icon::WaveSaw, "Saw -- bright and buzzy"},
                        {"pulse", picto::Icon::WavePulse, "Pulse -- hollow, reedy"},
                        {"noise", picto::Icon::WaveNoise, "Noise -- hiss, wind, surf"},
                    };
                    static const Choice filts[] = {
                        {"lp", picto::Icon::LowPass, "Low-pass -- keeps the lows, dulls the top"},
                        {"hp", picto::Icon::HighPass, "High-pass -- keeps the highs, thins it out"},
                        {"bp", picto::Icon::BandPass, "Band-pass -- keeps a band around the cutoff"},
                    };
                    static const Choice shapes[] = {
                        {"soft", picto::Icon::ShapeSoft, "Soft -- warm saturation"},
                        {"hard", picto::Icon::ShapeHard, "Hard -- clipped flat"},
                        {"atan", picto::Icon::ShapeAtan, "Round -- gentle all the way"},
                        {"fold", picto::Icon::ShapeFold, "Fold -- folds back on itself, metallic"},
                    };
                    const Choice* opts = (m->type == ModuleType::Filter) ? filts
                                         : (m->type == ModuleType::Drive) ? shapes
                                                                          : waves;
                    const int     cnt  = (m->type == ModuleType::Filter) ? 3 : 4;
                    const std::string cur = m->mode.empty() ? opts[0].mode : m->mode;
                    for (int k = 0; k < cnt; ++k) {
                        if (k > 0) ImGui::SameLine();
                        if (picto::button(opts[k].mode, opts[k].icon, opts[k].tip, true,
                                          cur == opts[k].mode)) {
                            st.mark();
                            m->mode       = opts[k].mode;
                            st.needsBuild = true;
                        }
                    }
                }
            }
            ImGui::Separator();

            int              count = 0;
            const ParamSpec* specs = paramsFor(m->type, count);
            for (int k = 0; k < count; ++k) {
                const ParamSpec& sp = specs[k];
                float            v  = m->param(sp.key, sp.def);
                ImGui::PushID(k);
                if (knob(sp.key, v, sp, em * 2.6f)) {
                    m->setParam(sp.key, v);
                    st.needsParams = true;
                }
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextDisabled("%s", sp.label);
                ImGui::SetNextItemWidth(inspectorW - em * 5.0f);
                if (ImGui::InputFloat("##num", &v, sp.step, sp.step * 10.0f, sp.fmt)) {
                    m->setParam(sp.key, std::clamp(v, sp.lo, sp.hi));
                    st.needsParams = true;
                }
                ImGui::EndGroup();
                ImGui::PopID();
            }
        }

        // The dials the game turns belong to the patch, not to any one module.
        ImGui::SeparatorText("Dials");
        for (std::size_t i = 0; i < st.patch.inputs.size(); ++i) {
            PatchInput& in = st.patch.inputs[i];
            ImGui::PushID(static_cast<int>(1000 + i));
            char nb[48];
            std::snprintf(nb, sizeof nb, "%s", in.name.c_str());
            ImGui::SetNextItemWidth(em * 4.5f);
            if (ImGui::InputText("##dn", nb, sizeof nb)) {
                in.name       = nb;
                st.needsBuild = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-(ImGui::GetFrameHeight() + em * 0.5f));
            if (ImGui::SliderFloat("##dv", &in.value, in.min, in.max)) {
                st.voice.setInput(in.name, in.value);
                if (st.scopeOk) st.scope.setInput(in.name, in.value);
            }
            ImGui::SameLine();
            if (picto::button("del", picto::Icon::Trash, "Remove this dial", true, false,
                              ImGui::GetFrameHeight())) {
                const std::size_t idx = i;
                pending = [&st, idx] {
                    st.mark();
                    st.patch.inputs.erase(st.patch.inputs.begin() + static_cast<long>(idx));
                    st.needsBuild = true;
                };
            }
            ImGui::PopID();
        }
        if (picto::button("adddial", picto::Icon::AddDial,
                          "Add a dial\nA number the game can turn while the sound plays")) {
            pending = [&st] {
                st.mark();
                st.patch.inputs.push_back(
                    {"dial" + std::to_string(st.patch.inputs.size() + 1), 0.0f, 1.0f, 0.0f});
                st.needsBuild = true;
            };
        }
    }
    ImGui::EndChild();

    // =====================================================================
    // The keyboard: play the thing
    // =====================================================================
    {
        const int pitchIdx = st.scopeOk ? st.scope.inputIndex("pitch") : -1;
        const int gateIdx  = st.scopeOk ? st.scope.inputIndex("gate") : -1;
        ImGui::BeginChild("##keys", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
        if (pitchIdx < 0 || gateIdx < 0) {
            ui::hint("A patch is playable once it has a dial called \"pitch\" and one "
                     "called \"gate\": pitch through a Note module into an oscillator, "
                     "gate into an envelope.");
            if (picto::button("addkeys", picto::Icon::Keys,
                              "Make it playable\nAdds the \"pitch\" and \"gate\" dials")) {
                pending = [&st] {
                    st.mark();
                    bool hasP = false, hasG = false;
                    for (const PatchInput& in : st.patch.inputs) {
                        hasP = hasP || in.name == "pitch";
                        hasG = hasG || in.name == "gate";
                    }
                    if (!hasP) st.patch.inputs.push_back({"pitch", 24.0f, 96.0f, 48.0f});
                    if (!hasG) st.patch.inputs.push_back({"gate", 0.0f, 1.0f, 0.0f});
                    st.needsBuild = true;
                };
            }
        } else {
            if (picto::button("hold", st.hold ? picto::Icon::Lock : picto::Icon::LockOpen,
                              st.hold ? "Hold is on -- the note keeps sounding after\n"
                                        "you let go. Press to release it."
                                      : "Hold -- keep a note sounding after you let go",
                              true, st.hold)) {
                st.hold = !st.hold;
                // Letting go of Hold lets go of the note, or it would ring on
                // with nothing on screen saying why.
                if (!st.hold && st.heldNote >= 0 && gateIdx >= 0) {
                    st.heldNote = -1;
                    st.voice.setInput(gateIdx, 0.0f);
                    st.scope.setInput(gateIdx, 0.0f);
                    for (PatchInput& in : st.patch.inputs)
                        if (in.name == "gate") in.value = 0.0f;
                }
            }
            ImGui::SameLine(0.0f, em * 0.6f);
            const ImVec2 kp = ImGui::GetCursorScreenPos();
            const float  kh = std::max(em * 2.2f, ImGui::GetContentRegionAvail().y - 4.0f);
            const float  kw = em * 1.5f;
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            const int    firstNote = 48;   // C3, two octaves
            static const bool kBlack[12] = {false, true, false, true, false, false,
                                            true,  false, true, false, true, false};

            int pressed = -1;
            // Whites first, blacks over them, so the narrow keys win the press.
            for (int pass = 0; pass < 2; ++pass) {
                int white = 0;
                for (int n = firstNote; n < firstNote + 24; ++n) {
                    const bool black = kBlack[(n - firstNote) % 12];
                    if (!black) ++white;
                    if (black != (pass == 1)) continue;
                    const float x = kp.x + (black ? static_cast<float>(white) * kw - kw * 0.3f
                                                  : static_cast<float>(white - 1) * kw);
                    const ImVec2 a(x, kp.y);
                    const ImVec2 b(x + (black ? kw * 0.6f : kw - 2.0f),
                                   kp.y + (black ? kh * 0.6f : kh));
                    ImGui::SetCursorScreenPos(a);
                    ImGui::PushID(n);
                    ImGui::SetNextItemAllowOverlap();
                    ImGui::InvisibleButton("##key", ImVec2(b.x - a.x, b.y - a.y));
                    const bool hot  = ImGui::IsItemHovered();
                    const bool down = ImGui::IsItemActive();
                    ImGui::PopID();
                    if (down) pressed = n;
                    const bool on = (st.heldNote == n);

                    ImU32 fill = black ? IM_COL32(28, 28, 32, 255) : IM_COL32(232, 232, 236, 255);
                    if (on)       fill = IM_COL32(230, 168, 60, 255);
                    else if (hot) fill = black ? IM_COL32(62, 62, 70, 255)
                                               : IM_COL32(250, 240, 214, 255);
                    dl->AddRectFilled(a, b, fill, 2.0f);
                    dl->AddRect(a, b, IM_COL32(12, 12, 15, 220), 2.0f);
                }
            }

            // One note at a time: this is a preview, and the patch has exactly
            // one voice to give.
            // The dials move with the keys. Playing a note IS turning "pitch"
            // and "gate", and a panel that plays a note while its own dials sit
            // still is telling two different stories about one patch.
            auto setDial = [&](const char* name, int idx, float v) {
                st.voice.setInput(idx, v);
                st.scope.setInput(idx, v);
                for (PatchInput& in : st.patch.inputs)
                    if (in.name == name) in.value = v;
            };
            if (pressed >= 0 && pressed != st.heldNote) {
                st.heldNote = pressed;
                setDial("pitch", pitchIdx, static_cast<float>(pressed));
                setDial("gate", gateIdx, 1.0f);
            } else if (pressed < 0 && st.heldNote >= 0 && !st.hold &&
                       !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                st.heldNote = -1;
                setDial("gate", gateIdx, 0.0f);
            }
        }
        ImGui::EndChild();
    }

    ImGui::End();

    if (pending) pending();

    // =====================================================================
    // Keep the preview in step with the patch
    // =====================================================================
    if (st.needsBuild) {
        std::string err;
        st.scopeOk = st.scope.compile(st.patch, 48000.0, kScopeFrames, &err);
        for (const PatchInput& in : st.patch.inputs) st.scope.setInput(in.name, in.value);
        st.needsBuild  = false;
        st.needsParams = false;
        if (!st.scopeOk) {
            st.status    = err;
            st.statusBad = true;
        } else if (st.statusBad) {
            st.status    = "patch is sound again";
            st.statusBad = false;
        }
        // The shape changed, so the playing voice is a patch that no longer
        // exists -- rebuilt, but only if the new one compiles. Half-way through
        // an edit a patch is briefly nonsense (a cable out, a loop mid-reroute),
        // and falling silent at every one of those would punish the normal way
        // of working. The old sound carries on and the error says what is wrong.
        if (st.voice.isPlaying() && st.scopeOk) {
            fitzel::SynthVoice v = fitzel::SynthVoice::fromPatch(audio, st.patch, nullptr);
            if (v.isValid()) {
                st.voice = std::move(v);
                st.voice.setVolume(st.volume);
                for (const PatchInput& in : st.patch.inputs)
                    st.voice.setInput(in.name, in.value);
                st.voice.play();
            }
        }
    } else if (st.needsParams) {
        // Numbers only: the voice and the scope both take them without a
        // rebuild, which is what makes turning a knob while it plays smooth.
        st.scope.updateParams(st.patch);
        st.voice.updateParams(st.patch);
        st.needsParams = false;
    }
}

} // namespace synthui
