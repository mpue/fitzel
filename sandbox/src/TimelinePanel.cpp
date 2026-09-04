#include "TimelinePanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "Component.hpp"
#include "PropertyMeta.hpp"
#include "SceneTypes.hpp"
#include "Selection.hpp"
#include "UiStyle.hpp"

namespace timelineui {

namespace {

// What is selected on the timeline. Kept here rather than in main because it is
// nobody else's business: no other panel acts on it and it does not belong to
// the scene. Both indices are re-validated every frame, so a scene load (or a
// deleted track) leaves them pointing at nothing rather than at the wrong key.
int g_selTrack = -1;
int g_selKey   = -1;

// The row metrics, in the CURRENT font's terms rather than in pixels.
//
// They were fixed pixel counts to begin with, and that is wrong here for a
// reason worth naming: the editor's font size is a comfort setting (View >
// Interface), so somebody who has turned the text up -- which is exactly the
// person this editor is for -- got rows shorter than their own line of text,
// with the buttons at the end of each one overlapping the row below. A timeline
// that becomes unreadable at the moment you enlarge the type is not an
// accessible timeline.
struct Metrics {
    float nameW, tailW, rowH, rulerH, keyR, grabPx;
};
Metrics metrics() {
    const float f = ImGui::GetFontSize();
    const float h = ImGui::GetFrameHeight();
    Metrics m;
    m.nameW  = f * 13.0f;                 // ~13 characters: "Position.x" and a margin
    m.tailW  = ImGui::CalcTextSize("Linear").x + ImGui::CalcTextSize("X").x +
               ImGui::GetStyle().FramePadding.x * 6.0f + f;
    m.rowH   = h + ImGui::GetStyle().ItemSpacing.y;
    m.rulerH = h + 4.0f;
    m.keyR   = std::max(5.0f, f * 0.42f);
    m.grabPx = m.keyR * 1.8f;
    return m;
}

ImU32 col(float r, float g, float b, float a = 1.0f) {
    return ImGui::GetColorU32(ImVec4(r, g, b, a));
}

void diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 fill, ImU32 line) {
    const ImVec2 p[4] = {{c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}};
    dl->AddQuadFilled(p[0], p[1], p[2], p[3], fill);
    dl->AddQuad(p[0], p[1], p[2], p[3], line, 1.5f);
}

// The end of the clip as the timeline draws it: never shorter than the last key,
// or keys would sit off the right edge with no way to reach them.
float clipEnd(const anim::Clip& c) {
    return std::max(1.0f, std::max(c.duration, c.lastKeyTime()));
}

const Entity* findEntity(const std::vector<Entity>& es, int id) {
    for (const Entity& e : es) if (e.id == id) return &e;
    return nullptr;
}

// "Position.x", "Range" -- what the row is animating, without the object's name.
std::string channelLabel(const std::vector<Entity>& es, const anim::Track& t) {
    const Entity* e = findEntity(es, t.entityId);
    const std::vector<Property>* props = e ? anim::propsFor(*e, t.comp) : nullptr;
    if (props)
        for (const Property& p : *props)
            if (p.key == t.key)
                return p.label + anim::channelSuffix(p.kind, t.index);
    return t.key;  // the field is gone (component detached): show the raw key
}

const char* interpName(anim::Interp i) {
    switch (i) {
        case anim::Interp::Smooth: return "Smooth";
        case anim::Interp::Step:   return "Step";
        default:                   return "Linear";
    }
}

// Move a key and keep the track sorted; returns where it ended up.
int moveKey(anim::Track& t, int idx, float newT) {
    if (idx < 0 || idx >= static_cast<int>(t.keys.size())) return -1;
    const anim::Key k{newT, t.keys[idx].v};
    t.keys.erase(t.keys.begin() + idx);
    int at = 0;
    while (at < static_cast<int>(t.keys.size()) && t.keys[at].t < newT) ++at;
    t.keys.insert(t.keys.begin() + at, k);
    return at;
}

// The nearest keyed time strictly before/after `from`, over every track. The
// transport's most useful pair of buttons: an author works key to key, and
// hunting for one by dragging is exactly the aiming this panel avoids.
float neighbourKeyTime(const anim::Clip& c, float from, bool forward) {
    float best = forward ? 1e9f : -1e9f;
    bool  any  = false;
    for (const anim::Track& t : c.tracks)
        for (const anim::Key& k : t.keys) {
            if (forward ? (k.t > from + 1e-3f) : (k.t < from - 1e-3f)) {
                if (forward ? (k.t < best) : (k.t > best)) { best = k.t; any = true; }
            }
        }
    return any ? best : from;
}

} // namespace

bool keyDiamond(const char* id, bool animated, bool keyedHere) {
    const float h = ImGui::GetFrameHeight();
    ImGui::PushID(id);
    // A whole frame-height square to hit, not the diamond inside it: the mark is
    // small because it sits in a column of fields, the TARGET is not.
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##key", ImVec2(h, h));
    const bool hot = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = keyedHere ? col(1.0f, 1.0f, 1.0f)
                     : animated  ? col(0.95f, 0.76f, 0.32f)
                                 : col(1.0f, 1.0f, 1.0f, hot ? 0.55f : 0.26f);
    diamond(dl, ImVec2(at.x + h * 0.5f, at.y + h * 0.5f), metrics().keyR,
            fill, col(0.0f, 0.0f, 0.0f, animated || keyedHere ? 0.75f : 0.0f));
    if (hot)
        ImGui::SetTooltip(keyedHere ? "Remove the key at the playhead"
                                    : "Key this property at the playhead");
    ImGui::PopID();
    return clicked;
}

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    ImGui::SetNextWindowSize(ImVec2(820.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Timeline", &s.show)) { ImGui::End(); return; }

    // THE TIMELINE DOES NOT TOUCH THE SCENE UNLESS IT IS ASKED TO. Having the
    // panel open used to be the preview, on the theory that an animation editor
    // showing the un-animated pose is lying about what the clip does. It is --
    // and it is the far smaller problem. With the clip applied every frame, a
    // property it owns cannot be dragged: the edit is overwritten before it is
    // drawn, so the object springs back to the keyed pose and nothing on screen
    // says why. Worse, closing the panel handed back the values captured when it
    // opened, undoing a move the author had made in between.
    //
    // So Preview is a switch, and it starts off. Off, the scene is exactly what
    // was authored and every property edits normally -- the playhead only says
    // where the next key will land. On (and while Play runs), the clip drives
    // what it owns and the scene is put back when the switch goes off, the panel
    // closes, or Play begins.

    if (s.clips.empty()) s.clips.push_back(anim::Clip{});
    s.editClip = std::clamp(s.editClip, 0, static_cast<int>(s.clips.size()) - 1);
    anim::Clip&   c = s.clips[s.editClip];
    anim::Player& p = s.player;
    const float   end = clipEnd(c);
    const float   step = 1.0f / (c.fps > 0.1f ? c.fps : 10.0f);

    // MOVING THE PLAYHEAD IS THE REQUEST TO SEE IT. Preview being a switch you
    // had to find first made the whole panel look broken: keys went in, the
    // playhead moved over them, and the scene sat there. Every control that sets
    // a time goes through this, so scrubbing, stepping, jumping to a key and
    // Play all pose the scene without anyone hunting for a checkbox -- which
    // stays, as the way to turn it back OFF and get the authored scene back.
    auto goTo = [&](float t) {
        p.time = std::clamp(t, 0.0f, clipEnd(c));
        if (!p.preview && !c.empty()) anim::beginPreview(c, s.entities, p);
    };

    // Keep the selection honest before anything reads it.
    if (g_selTrack >= static_cast<int>(c.tracks.size())) { g_selTrack = -1; g_selKey = -1; }
    if (g_selTrack >= 0 && g_selKey >= static_cast<int>(c.tracks[g_selTrack].keys.size()))
        g_selKey = -1;

    // --- Which animation --------------------------------------------------
    // First, because it renames everything under it: the transport, the rows and
    // the diamonds in the Inspector all belong to the clip named here.
    const float pickW = ImGui::GetFontSize() * 10.0f;
    ImGui::SetNextItemWidth(pickW);
    if (ImGui::BeginCombo("##clip", c.name.c_str())) {
        for (int i = 0; i < static_cast<int>(s.clips.size()); ++i) {
            const bool on = (i == s.editClip);
            if (ImGui::Selectable(s.clips[i].name.c_str(), on)) {
                if (i != s.editClip) {
                    // Leaving a clip ends its preview: the scene goes back before
                    // the next clip is allowed to pose it, or the two would take
                    // turns overwriting each other's properties.
                    if (p.preview) anim::endPreview(c, s.entities, p);
                    s.editClip = i;
                    p.time = 0.0f;
                }
            }
            if (on) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    {   // Renaming in place. The name is how a clip is referred to from outside
        // the panel, so it is edited where it is read rather than in a dialog.
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", c.name.c_str());
        ImGui::SetNextItemWidth(pickW);
        if (ImGui::InputText("Name##clipname", buf, sizeof(buf))) {
            c.name = buf;
            s.markDirty();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        if (p.preview) anim::endPreview(c, s.entities, p);
        anim::Clip fresh;
        fresh.name = "Animation " + std::to_string(s.clips.size() + 1);
        s.clips.push_back(std::move(fresh));
        s.editClip = static_cast<int>(s.clips.size()) - 1;
        p.time = 0.0f;
        g_selTrack = g_selKey = -1;
        s.markDirty();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(s.clips.size() < 2);
    if (ImGui::Button("Delete clip")) {
        if (p.preview) anim::endPreview(c, s.entities, p);
        s.clips.erase(s.clips.begin() + s.editClip);
        s.editClip = std::clamp(s.editClip, 0, static_cast<int>(s.clips.size()) - 1);
        g_selTrack = g_selKey = -1;
        s.markDirty();
        ImGui::EndDisabled();
        ImGui::End();
        return;   // `c` above is dangling now; next frame draws the survivor
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    // --- Transport ----------------------------------------------------------
    // Big buttons in a fixed order, all of them one click: nothing here needs a
    // press-move-release to operate.
    // Every fixed width below is in font terms for the same reason the rows are:
    // the editor's type size is a comfort setting, and a transport whose labels
    // spill out of their buttons when you turn it up is not one.
    const float  em = ImGui::GetFontSize();
    const ImVec2 tb(em * 2.4f, 0.0f);
    if (ImGui::Button("|<", tb)) goTo(0.0f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to the start");
    ImGui::SameLine();
    if (ImGui::Button("<<", tb)) goTo(neighbourKeyTime(c, p.time, false));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous key");
    ImGui::SameLine();
    if (ImGui::Button("<", tb)) goTo(p.time - step);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("One frame back");
    ImGui::SameLine();
    if (p.playing) {
        if (ImGui::Button("Pause", ImVec2(em * 4.5f, 0.0f))) p.playing = false;
    } else {
        ImGui::BeginDisabled(c.empty());
        if (ImGui::Button("Play", ImVec2(em * 4.5f, 0.0f))) {
            // Play needs the preview on to be visible at all, so it turns it on
            // rather than being greyed out until you find the switch yourself.
            if (!p.preview) anim::beginPreview(c, s.entities, p);
            p.playing = true;
            if (p.time >= end - 1e-3f) p.time = 0.0f;   // at the end: from the top
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button(">", tb)) goTo(p.time + step);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("One frame on");
    ImGui::SameLine();
    if (ImGui::Button(">>", tb)) goTo(neighbourKeyTime(c, p.time, true));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next key");
    ImGui::SameLine();
    if (ImGui::Button(">|", tb)) goTo(end);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to the end");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 8.0f);   // the -/+ step buttons live inside this
    float t = p.time;
    if (ImGui::InputFloat("s##time", &t, step, step * 5.0f, "%.2f"))
        goTo(anim::snap(c, t));
    ImGui::SameLine();
    ui::hint("of %.2f s", end);

    // --- Authoring ----------------------------------------------------------
    bool preview = p.preview;
    if (ImGui::Checkbox("Preview", &preview)) {
        if (preview) anim::beginPreview(c, s.entities, p);
        else         anim::endPreview(c, s.entities, p);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Show the scene as the clip poses it.\n"
            "While this is on, an animated property is driven by the timeline\n"
            "and cannot be edited by hand -- unless Auto-key is on, which\n"
            "records the edit instead. Turning it off puts the scene back.");
    ImGui::SameLine();
    ImGui::Checkbox("Auto-key", &s.autoKey);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Editing a property that is already animated records the new\n"
            "value as a key at the playhead. Properties that are not\n"
            "animated are untouched -- the diamond in the Inspector\n"
            "starts one.");
    ImGui::SameLine();
    // Re-key everything already animated on the selected object. The diamonds in
    // the Inspector add the FIRST key of a property; this is the one you press
    // for every pose after that, and it cannot miss a channel.
    const int selId = s.sel.valid() ? s.entities[s.sel.index()].id : -1;
    int selTracks = 0;
    for (const anim::Track& tr : c.tracks) if (tr.entityId == selId) ++selTracks;
    ImGui::BeginDisabled(selTracks == 0);
    if (ImGui::Button("Key object")) {
        for (anim::Track& tr : c.tracks) {
            if (tr.entityId != selId) continue;
            const anim::Bound b = anim::bind(tr, s.entities);
            if (!b) continue;
            const float kt = anim::snap(c, p.time);
            const float v  = anim::readValue(*b.prop, b.owner, tr.index);
            const int   at = tr.keyAt(kt);
            if (at >= 0) tr.keys[at].v = v;
            else {
                int ins = 0;
                while (ins < static_cast<int>(tr.keys.size()) && tr.keys[ins].t < kt) ++ins;
                tr.keys.insert(tr.keys.begin() + ins, anim::Key{kt, v});
            }
        }
        s.markDirty();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Key every animated property of the selected object at this time");

    ImGui::SetNextItemWidth(em * 3.6f);
    if (ImGui::DragFloat("Length", &c.duration, 0.25f, 0.5f, 3600.0f, "%.1f s")) s.markDirty();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 3.6f);
    if (ImGui::DragFloat("Grid", &c.fps, 0.5f, 1.0f, 120.0f, "%.0f fps")) s.markDirty();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every key snaps to this grid, however roughly you aim");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 3.6f);
    if (ImGui::DragFloat("Speed", &c.speed, 0.02f, 0.05f, 8.0f, "%.2fx")) s.markDirty();
    ImGui::SameLine();
    if (ImGui::Checkbox("Loop", &c.loop)) s.markDirty();
    ImGui::SameLine();
    if (ImGui::Checkbox("Play on start", &c.playOnStart)) s.markDirty();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Run this animation the moment the game starts");

    if (p.preview) {
        int live = 0;
        for (const anim::Track& tr : c.tracks)
            if (anim::bind(tr, s.entities)) ++live;
        ui::hint("Preview on -- the clip is driving %d of %d tracks at %.2f s.",
                 live, static_cast<int>(c.tracks.size()), p.time);
    } else if (!c.tracks.empty()) {
        ui::hint("Preview off -- the scene shows the values you authored.");
    }

    ImGui::Separator();

    if (c.tracks.empty()) {
        ui::hint("Nothing is animated yet.");
        ui::hint("Select an object, then press the diamond next to a property in the "
                 "Inspector to key it here.");
        ui::hint("The timeline leaves the scene alone until Preview is ticked.");
        ImGui::End();
        return;
    }

    // --- The rows -----------------------------------------------------------
    // Everything below is drawn at ABSOLUTE screen positions -- a grid of rows
    // and a draw list, not a stack of widgets -- so the block's height has to be
    // claimed up front with one Dummy. Without it Dear ImGui measures the window
    // by wherever the last item happened to be placed, which is not the bottom of
    // the block: the content came out short, a scrollbar appeared, the scroll
    // shifted the screen positions the rows were drawn at, and the rows vanished
    // off the top while the controls under them slid into the gap. Reserve
    // first, draw second.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Metrics m = metrics();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  fullW  = ImGui::GetContentRegionAvail().x;

    int headings = 0, lastGroup = -0x7fffffff;
    for (const anim::Track& tr : c.tracks)
        if (tr.entityId != lastGroup) { ++headings; lastGroup = tr.entityId; }
    const float blockH = m.rulerH +
                         m.rowH * static_cast<float>(c.tracks.size() + headings);
    ImGui::Dummy(ImVec2(fullW, blockH));
    const float  x0     = origin.x + m.nameW;
    const float  x1     = origin.x + std::max(m.nameW + ImGui::GetFontSize() * 4.0f, fullW - m.tailW);
    const float  spanX  = std::max(1.0f, x1 - x0);

    auto timeToX = [&](float tt) { return x0 + (tt / end) * spanX; };
    auto xToTime = [&](float xx) { return std::clamp((xx - x0) / spanX * end, 0.0f, end); };

    // Ruler. Clicking or dragging anywhere on it moves the playhead, snapped --
    // a whole-width target for the one thing you reach for most.
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##ruler", ImVec2(fullW, m.rulerH));
    const bool rulerActive = ImGui::IsItemActive();
    if (rulerActive)
        goTo(anim::snap(c, xToTime(ImGui::GetIO().MousePos.x)));

    const ImU32 cLine  = col(1.0f, 1.0f, 1.0f, 0.16f);
    const ImU32 cText  = col(1.0f, 1.0f, 1.0f, 0.55f);
    const float rulerY = origin.y;
    // A label every second while they fit, every five when they do not.
    const int   secStep = (spanX / std::max(end, 1.0f)) < ImGui::GetFontSize() * 2.2f ? 5 : 1;
    for (int sec = 0; sec <= static_cast<int>(end); sec += secStep) {
        const float x = timeToX(static_cast<float>(sec));
        dl->AddLine(ImVec2(x, rulerY + m.rulerH * 0.6f), ImVec2(x, rulerY + m.rulerH), cLine);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", sec);
        dl->AddText(ImVec2(x + 3.0f, rulerY + 2.0f), cText, buf);
    }

    const float rowsTop = origin.y + m.rulerH;
    float       rowY    = rowsTop;
    int         lastEntity = -0x7fffffff;

    for (int i = 0; i < static_cast<int>(c.tracks.size()); ++i) {
        anim::Track& tr = c.tracks[i];

        // A heading row whenever the object changes, so a scene with four
        // animated objects reads as four groups rather than one long list.
        // (The tracks are kept in creation order; grouping is by adjacency,
        // which is what that order gives for anything keyed one object at a
        // time.)
        if (tr.entityId != lastEntity) {
            lastEntity = tr.entityId;
            const Entity* e = findEntity(s.entities, tr.entityId);
            ImGui::SetCursorScreenPos(ImVec2(origin.x, rowY));
            if (e) ui::title("%s", e->name.empty() ? entityTypeName(e->type) : e->name.c_str());
            else   ui::title("(deleted object %d)", tr.entityId);
            rowY += m.rowH;
        }

        const bool rowSel = (g_selTrack == i);
        if (rowSel)
            dl->AddRectFilled(ImVec2(origin.x, rowY), ImVec2(x1, rowY + m.rowH),
                              col(1.0f, 1.0f, 1.0f, 0.06f));

        // The name is a button: selecting a row is how you say which track the
        // buttons at the bottom act on.
        ImGui::SetCursorScreenPos(ImVec2(origin.x + ImGui::GetFontSize() * 0.8f, rowY));
        ImGui::PushID(i);
        const std::string label = channelLabel(s.entities, tr) +
                                  (tr.comp.empty() ? "" : "  (" + tr.comp + ")");
        if (ImGui::Selectable(label.c_str(), rowSel, 0,
                              ImVec2(m.nameW - ImGui::GetFontSize(), m.rowH)))
            { g_selTrack = i; g_selKey = -1; }

        // The row's timeline strip: one wide target that catches both a click on
        // a key and a drag of one.
        ImGui::SetCursorScreenPos(ImVec2(x0, rowY));
        ImGui::InvisibleButton("##strip", ImVec2(spanX, m.rowH));
        const bool stripHeld = ImGui::IsItemActive();
        if (ImGui::IsItemActivated()) {
            // Press: take the nearest key within reach, if there is one.
            const float mx = ImGui::GetIO().MousePos.x;
            int   best = -1;
            float bestD = m.grabPx;
            for (int k = 0; k < static_cast<int>(tr.keys.size()); ++k) {
                const float d = std::fabs(timeToX(tr.keys[k].t) - mx);
                if (d <= bestD) { bestD = d; best = k; }
            }
            g_selTrack = i;
            g_selKey   = best;
            // Landing on a key also parks the playhead there: the nudge buttons
            // below then act on the key you are looking at, and the viewport
            // shows the pose it makes.
            if (best >= 0) goTo(tr.keys[best].t);
        } else if (stripHeld && g_selTrack == i && g_selKey >= 0) {
            const float nt = anim::snap(c, xToTime(ImGui::GetIO().MousePos.x));
            if (std::fabs(nt - tr.keys[g_selKey].t) > 1e-4f) {
                g_selKey = moveKey(tr, g_selKey, nt);
                p.time   = nt;
                s.markDirty();
            }
        }

        // The row's own line, then its keys on top of it.
        dl->AddLine(ImVec2(x0, rowY + m.rowH * 0.5f), ImVec2(x1, rowY + m.rowH * 0.5f),
                    col(1.0f, 1.0f, 1.0f, 0.10f));
        const bool live = static_cast<bool>(anim::bind(tr, s.entities));
        for (int k = 0; k < static_cast<int>(tr.keys.size()); ++k) {
            const bool isSel = (g_selTrack == i && g_selKey == k);
            const ImU32 fill = !live ? col(0.55f, 0.55f, 0.55f)
                             : isSel ? col(1.0f, 1.0f, 1.0f)
                                     : col(0.95f, 0.76f, 0.32f);
            diamond(dl, ImVec2(timeToX(tr.keys[k].t), rowY + m.rowH * 0.5f), m.keyR,
                    fill, col(0.0f, 0.0f, 0.0f, 0.75f));
        }

        // Two buttons at the right: how the row interpolates (one click cycles
        // it, which is a bigger target than a combo and never opens a popup you
        // have to aim into), and detaching the row entirely.
        ImGui::SetCursorScreenPos(ImVec2(x1 + ImGui::GetStyle().ItemSpacing.x, rowY));
        const bool discrete = [&] {
            const anim::Bound b = anim::bind(tr, s.entities);
            return b && anim::isDiscrete(b.prop->kind);
        }();
        ImGui::BeginDisabled(discrete);   // a bool has no in-between to shape
        if (ImGui::SmallButton(interpName(tr.interp))) {
            tr.interp = tr.interp == anim::Interp::Linear ? anim::Interp::Smooth
                      : tr.interp == anim::Interp::Smooth ? anim::Interp::Step
                                                          : anim::Interp::Linear;
            s.markDirty();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        bool killRow = ImGui::SmallButton("X");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop animating this");
        ImGui::PopID();

        rowY += m.rowH;

        if (killRow) {
            c.tracks.erase(c.tracks.begin() + i);
            g_selTrack = -1; g_selKey = -1;
            s.markDirty();
            break;   // the indices below have shifted; next frame draws the rest
        }
    }

    // The playhead, over everything.
    const float px = timeToX(p.time);
    dl->AddLine(ImVec2(px, rulerY), ImVec2(px, rowY), col(1.0f, 0.35f, 0.30f, 0.95f), 2.0f);
    dl->AddTriangleFilled(ImVec2(px - m.keyR, rulerY), ImVec2(px + m.keyR, rulerY),
                          ImVec2(px, rulerY + m.keyR * 1.4f), col(1.0f, 0.35f, 0.30f, 0.95f));

    // Back to the flow, below what was reserved -- not below wherever the last
    // row was drawn, which is the same thing only when nothing was deleted.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + blockH));
    ImGui::Separator();

    // --- What is selected ---------------------------------------------------
    // The key's time and value as numbers, and a frame either way as buttons.
    // This is the half of the panel that makes the dragging optional.
    if (g_selTrack >= 0 && g_selKey >= 0) {
        anim::Track& tr = c.tracks[g_selTrack];
        anim::Key&   k  = tr.keys[g_selKey];
        ui::hint("Key %d of %d on %s", g_selKey + 1, static_cast<int>(tr.keys.size()),
                 channelLabel(s.entities, tr).c_str());

        if (ImGui::Button("<##keyback", ImVec2(em * 2.2f, 0.0f))) {
            g_selKey = moveKey(tr, g_selKey, std::max(0.0f, k.t - step));
            p.time   = c.tracks[g_selTrack].keys[g_selKey].t;
            s.markDirty();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 8.0f);
        float kt = k.t;
        if (ImGui::InputFloat("s##keyt", &kt, step, step * 5.0f, "%.2f")) {
            g_selKey = moveKey(tr, g_selKey, std::clamp(anim::snap(c, kt), 0.0f, end));
            p.time   = c.tracks[g_selTrack].keys[g_selKey].t;
            s.markDirty();
        }
        ImGui::SameLine();
        if (ImGui::Button(">##keyfwd", ImVec2(em * 2.2f, 0.0f))) {
            g_selKey = moveKey(tr, g_selKey, std::min(end, k.t + step));
            p.time   = c.tracks[g_selTrack].keys[g_selKey].t;
            s.markDirty();
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 6.0f);
        // Re-fetch: a move above may have shuffled the key to another index.
        anim::Key& kv = c.tracks[g_selTrack].keys[g_selKey];
        if (ImGui::DragFloat("Value", &kv.v, 0.05f)) s.markDirty();

        ImGui::SameLine();
        if (ImGui::Button("Delete key")) {
            tr.keys.erase(tr.keys.begin() + g_selKey);
            if (tr.keys.empty()) {
                c.tracks.erase(c.tracks.begin() + g_selTrack);
                g_selTrack = -1;
            }
            g_selKey = -1;
            s.markDirty();
        }
    } else if (g_selTrack >= 0) {
        anim::Track& tr = c.tracks[g_selTrack];
        ui::hint("%s -- no key selected", channelLabel(s.entities, tr).c_str());
        // Adding a key to the row you are on, at the playhead, from what the
        // property actually reads right now.
        const anim::Bound b = anim::bind(tr, s.entities);
        ImGui::BeginDisabled(!b);
        if (ImGui::Button("Add key here")) {
            const float kt = anim::snap(c, p.time);
            const float v  = anim::readValue(*b.prop, b.owner, tr.index);
            const int   at = tr.keyAt(kt);
            if (at >= 0) tr.keys[at].v = v;
            else {
                int ins = 0;
                while (ins < static_cast<int>(tr.keys.size()) && tr.keys[ins].t < kt) ++ins;
                tr.keys.insert(tr.keys.begin() + ins, anim::Key{kt, v});
                g_selKey = ins;
            }
            s.markDirty();
        }
        ImGui::EndDisabled();
    } else {
        ui::hint("Click a key to move it a frame at a time, or a row to add one.");
    }

    ImGui::End();
}

} // namespace timelineui
