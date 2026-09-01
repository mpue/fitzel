#include "RoadPanel.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/asset/AssetTypes.hpp>

#include "UiStyle.hpp"

#include "RoadBridge.hpp"
#include "RoadDecal.hpp"
#include "RoadPrefab.hpp"
#include "RoadTunnel.hpp"
#include "RoadSide.hpp"
#include "RoadSystem.hpp"
#include "UiStyle.hpp"

namespace roadui {
namespace {

// Show a structural edit at once instead of leaving the viewport unchanged until
// the road is built.
//
// Adding a bridge or a loop only set a flag before: the geometry appeared on the
// next "Build road into terrain" and nothing happened in between, which reads
// exactly like the button not working -- and for a loop, which needs no terrain
// work at all, there was nothing to wait for in the first place. Re-lofting is
// the same pass a scene load runs, so it is a preview of what was just asked
// for, not a substitute for building: the Build prompt stays lit, because a
// bridge still needs its corridor cut and its abutments ramped.
void showAtOnce(const PanelState& s) {
    s.road().rebuildMesh();
    s.road().needsBuild = true;
}

// The bridge list + its style sliders. A bridge names the two control points the
// user picked; the road flies straight between them and the ground below is left
// alone. Returns true when something changed that the road must be rebuilt for.
bool bridgeSection(const PanelState& s) {
    bool rc = false;
    if (!ui::header("Bridges", ImGuiTreeNodeFlags_DefaultOpen)) return rc;

    const bool pair = s.sel >= 0 && s.sel2 >= 0 && s.sel != s.sel2;
    // Already bridged? Then offer to take it away again instead.
    int existing = -1;
    for (int i = 0; pair && i < static_cast<int>(s.road().bridges.size()); ++i)
        if ((s.road().bridges[i].a == s.sel && s.road().bridges[i].b == s.sel2) ||
            (s.road().bridges[i].a == s.sel2 && s.road().bridges[i].b == s.sel))
            existing = i;

    ImGui::BeginDisabled(!pair || existing >= 0);
    if (ImGui::Button("Create bridge", ImVec2(-1.0f, 0.0f))) {
        s.beginEdit();
        s.road().bridges.push_back({s.sel, s.sel2});
        s.endEdit("Create bridge");
        showAtOnce(s);
        rc = true;
    }
    ImGui::EndDisabled();
    if (!pair)
        ImGui::TextDisabled("Select a handle, then shift+click another");
    else if (existing >= 0)
        ImGui::TextDisabled("#%d \xE2\x86\x92 #%d is already a bridge", s.sel, s.sel2);

    // Scoped by name, not just by index. Every list in this window numbers its
    // rows from 0 and puts a "Remove" in each, so bridge row 0 and loop row 0
    // would otherwise be the SAME widget as far as ImGui is concerned: it reports
    // the clash as an ID conflict, and a click on one of them acts on the other.
    ImGui::PushID("bridges");
    for (int i = 0; i < static_cast<int>(s.road().bridges.size()); ++i) {
        ImGui::PushID(i);
        ImGui::Text("Bridge #%d \xE2\x86\x92 #%d", s.road().bridges[i].a,
                    s.road().bridges[i].b);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            s.beginEdit();
            s.road().bridges.erase(s.road().bridges.begin() + i);
            s.endEdit("Remove bridge");
            showAtOnce(s);
            rc = true;
            ImGui::PopID();
            break; // the list just shifted under us
        }
        ImGui::PopID();
    }
    ImGui::PopID(); // "bridges"
    if (s.road().bridges.empty()) ImGui::TextDisabled("No bridges");

    ImGui::Separator();
    rc |= roadbridge::panel(s.road().bridgeStyle);
    return rc;
}

// The tunnel list + its style sliders, and the mirror of bridgeSection above: a
// tunnel names the two control points the user picked, and the road holds its line
// through the hill between them instead of cutting it away.
bool tunnelSection(const PanelState& s) {
    bool rc = false;
    if (!ui::header("Tunnels")) return rc;

    const bool pair = s.sel >= 0 && s.sel2 >= 0 && s.sel != s.sel2;
    int existing = -1;
    for (int i = 0; pair && i < static_cast<int>(s.road().tunnels.size()); ++i)
        if ((s.road().tunnels[i].a == s.sel && s.road().tunnels[i].b == s.sel2) ||
            (s.road().tunnels[i].a == s.sel2 && s.road().tunnels[i].b == s.sel))
            existing = i;

    ImGui::BeginDisabled(!pair || existing >= 0);
    if (ImGui::Button("Create tunnel", ImVec2(-1.0f, 0.0f))) {
        s.beginEdit();
        s.road().tunnels.push_back({s.sel, s.sel2});
        s.endEdit("Create tunnel");
        showAtOnce(s);
        rc = true;
    }
    ImGui::EndDisabled();
    if (!pair)
        ImGui::TextDisabled("Select a handle, then shift+click another");
    else if (existing >= 0)
        ImGui::TextDisabled("#%d \xE2\x86\x92 #%d is already a tunnel", s.sel, s.sel2);

    ImGui::PushID("tunnels");   // own ID scope -- see the note in bridgeSection
    for (int i = 0; i < static_cast<int>(s.road().tunnels.size()); ++i) {
        ImGui::PushID(i);
        ImGui::Text("Tunnel #%d \xE2\x86\x92 #%d", s.road().tunnels[i].a,
                    s.road().tunnels[i].b);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            s.beginEdit();
            s.road().tunnels.erase(s.road().tunnels.begin() + i);
            s.endEdit("Remove tunnel");
            showAtOnce(s);
            rc = true;
            ImGui::PopID();
            break; // the list just shifted under us
        }
        ImGui::PopID();
    }
    ImGui::PopID(); // "tunnels"
    if (s.road().tunnels.empty()) ImGui::TextDisabled("No tunnels");

    ImGui::Separator();
    rc |= roadtunnel::panel(s.road().tunnelStyle);
    return rc;
}

// Vertical loops. Named by a pair of control points exactly like a bridge, and
// for the same reason: the loop is derived from the centreline on every build,
// so the only thing worth saving -- and the only thing worth editing -- is which
// stretch it applies to.
bool loopSection(const PanelState& s) {
    bool rc = false;

    // Which loop the current selection names -- worked out BEFORE the header,
    // because picking a loop's crown in the viewport selects its two points, and
    // landing on a collapsed section would make that click look like it missed.
    // Only on the frame the selection changes, so the section can still be shut.
    const bool pair = s.sel >= 0 && s.sel2 >= 0 && s.sel != s.sel2;
    int existing = -1;
    for (int i = 0; pair && i < static_cast<int>(s.road().loops.size()); ++i)
        if ((s.road().loops[i].a == s.sel && s.road().loops[i].b == s.sel2) ||
            (s.road().loops[i].a == s.sel2 && s.road().loops[i].b == s.sel))
            existing = i;
    static int lastPair[2] = {-2, -2};
    const bool picked = (s.sel != lastPair[0] || s.sel2 != lastPair[1]);
    lastPair[0] = s.sel; lastPair[1] = s.sel2;
    if (picked && existing >= 0) ImGui::SetNextItemOpen(true);

    if (!ui::header("Loops")) return rc;

    ui::hint("A loop curls the road up, over and back down. How far it travels\n"
             "while it turns is the distance between the two points; the radius\n"
             "sets how tall it stands. A long, low turn sways out sideways on the\n"
             "way up and back the other way coming down, so its two halves pass\n"
             "beside each other instead of through each other.\n"
             "In the viewport: click a loop's crown to select it.");

    ImGui::BeginDisabled(!pair || existing >= 0);
    if (ImGui::Button("Create loop", ImVec2(-1.0f, 0.0f))) {
        s.beginEdit();
        roadloop::Spec sp;
        sp.a = s.sel; sp.b = s.sel2;
        s.road().loops.push_back(sp);
        s.endEdit("Create loop");
        showAtOnce(s);
        rc = true;
    }
    ImGui::EndDisabled();
    if (!pair)
        ImGui::TextDisabled("Select a handle, then shift+click another");
    else if (existing >= 0)
        ImGui::TextDisabled("#%d \xE2\x86\x92 #%d already loops", s.sel, s.sel2);

    ImGui::PushID("loops");   // own ID scope -- see the note in bridgeSection
    for (int i = 0; i < static_cast<int>(s.road().loops.size()); ++i) {
        ImGui::PushID(i);
        // The row for the loop the viewport has selected is coloured and scrolled
        // to, so "I clicked that one" and "this is the radius I am about to
        // change" are the same statement.
        const bool here = (i == existing);
        if (here) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.24f, 1.0f));
            if (picked) ImGui::SetScrollHereY(0.5f);
        }
        ImGui::Text("Loop #%d \xE2\x86\x92 #%d", s.road().loops[i].a, s.road().loops[i].b);
        if (here) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            s.beginEdit();
            s.road().loops.erase(s.road().loops.begin() + i);
            s.endEdit("Remove loop");
            showAtOnce(s);
            ImGui::PopID();
            ImGui::PopID(); // "loops"
            return true; // the list just shifted under us
        }
        ImGui::SetNextItemWidth(-70.0f);
        if (ImGui::DragFloat("Radius", &s.road().loops[i].radius, 0.1f, 3.0f, 120.0f,
                             "%.1f m"))
            rc = true;
        if (ImGui::IsItemActivated())            s.beginEdit();
        if (ImGui::IsItemDeactivatedAfterEdit()) { s.endEdit("Loop radius"); showAtOnce(s); }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The turn stands twice this tall. Too small for the\n"
                              "stretch it is on and it stops turning over at all\n"
                              "-- a hump, not a loop. It also sets how fast you\n"
                              "have to arrive: holding the top needs v^2 >\n"
                              "gravity * radius, so a bigger loop is a faster one\n"
                              "to commit to.");
        ImGui::PopID();
    }
    ImGui::PopID(); // "loops"
    if (s.road().loops.empty()) ImGui::TextDisabled("No loops");
    return rc;
}

// Side objects: a list of placement rules (guard rails, curbs, posts) repeated
// along the road from a model asset. Each rule is derived, not authored -- so the
// panel edits a handful of numbers, not hundreds of objects. Edits go on the same
// undo timeline as the rest of the road (the shape snapshot now carries them), and
// a live change re-derives the instances at once (rebuildSideObjects is cheap).
void sideSection(const PanelState& s) {
    if (!ui::header("Side objects", ImGuiTreeNodeFlags_DefaultOpen)) return;

    // Add a rule pre-seeded for its kind. Empty model = it places nothing until
    // one is picked, so adding then choosing reads naturally.
    auto add = [&](roadside::Kind k) {
        s.beginEdit();
        s.road().sideLines.push_back(roadside::preset(k));
        s.endEdit("Add side object");
    };
    if (ImGui::Button("+ Guard rail")) add(roadside::Kind::GuardRail);
    ImGui::SameLine();
    if (ImGui::Button("+ Curb")) add(roadside::Kind::Curb);
    ImGui::SameLine();
    if (ImGui::Button("+ Post")) add(roadside::Kind::Post);
    ImGui::SameLine();
    if (ImGui::Button("+ On road")) add(roadside::Kind::OnRoad);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Objects ON the carriageway, every N metres -- cones,\n"
                          "blocks, arrows. Offset is measured from the middle of\n"
                          "the road, and they stand on the asphalt (or the bridge\n"
                          "deck), not on the ground beneath it.");

    if (s.road().sideLines.empty()) {
        ImGui::TextDisabled("None. Add a rail, curb or posts.");
        return;
    }
    if (s.road().verts() == 0)
        ImGui::TextDisabled("Build the road to see them placed.");

    bool changed = false; // re-derive instances this frame (live preview)

    ImGui::PushID("sideobjects");   // own ID scope -- see the note in bridgeSection
    for (int i = 0; i < static_cast<int>(s.road().sideLines.size()); ++i) {
        roadside::Line& L = s.road().sideLines[i];
        ImGui::PushID(i);
        ImGui::Separator();

        // Enable + kind label + delete on one line.
        if (ImGui::Checkbox("##en", &L.enabled)) {
            L.enabled = !L.enabled;               // snapshot the pre-toggle state
            s.beginEdit();
            L.enabled = !L.enabled;
            s.endEdit("Toggle side object");
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(roadside::kindName(L.kind));
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
        if (ImGui::SmallButton("X")) {
            s.beginEdit();
            s.road().sideLines.erase(s.road().sideLines.begin() + i);
            s.endEdit("Remove side object");
            ImGui::PopID();
            changed = true;
            break; // the list shifted under us
        }

        // Model picker: every Model asset the database knows (engine + project),
        // whether or not it has been imported yet -- the build imports it on
        // demand. Stores the asset's relative path, which resolves back portably.
        const std::string preview = L.model.empty() ? "(pick a model)" : L.model;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##model", preview.c_str())) {
            int shown = 0;
            for (const fitzel::AssetId& id : s.assetDb.allAssets()) {
                const auto* e = s.assetDb.entry(id);
                if (!e || e->type != fitzel::AssetType::Model) continue;
                ++shown;
                const std::string ref   = e->relPath;
                const std::string label = e->absPath.filename().string();
                const bool sel = (ref == L.model);
                if (ImGui::Selectable((label + "##" + id.toString()).c_str(), sel)) {
                    const std::string old = L.model;
                    L.model = old;                 // snapshot pre-change
                    s.beginEdit();
                    L.model = ref;
                    s.endEdit("Side object model");
                    changed = true;
                }
                if (ImGui::IsItemHovered() && e->relPath != label)
                    ImGui::SetTooltip("%s", e->relPath.c_str());
                if (sel) ImGui::SetItemDefaultFocus();
            }
            if (shown == 0)
                ImGui::TextDisabled("No model assets found (drop a .glb in Assets)");
            ImGui::EndCombo();
        }

        // Side. Combo gives no before-value, so snapshot around it by hand.
        {
            const int old = L.side;
            const char* items = "Left\0Right\0Both\0Alternate\0";
            if (ImGui::Combo("Side", &L.side, items)) {
                const int nw = L.side;
                L.side = old; s.beginEdit(); L.side = nw; s.endEdit("Side object side");
                changed = true;
            }
        }

        // On the road vs. beside it. A toggle rather than a fixed property of the
        // Kind, so a rail can be pulled onto the carriageway (and a cone line
        // pushed off it) without deleting the rule and re-picking its model.
        if (ImGui::Checkbox("On the road", &L.onRoad)) {
            L.onRoad = !L.onRoad;
            s.beginEdit(); L.onRoad = !L.onRoad; s.endEdit("Side on road");
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("On: stands on the carriageway. Offset is measured from\n"
                              "the middle of the road and the height comes from the\n"
                              "road surface (bridge decks and banking included).\n"
                              "Off: stands beside the road. Offset is measured beyond\n"
                              "the edge and the height comes from the ground.");

        // Metric knobs. DragFloat so a value is typeable (Parkinson-friendly) --
        // click to type, drag to sweep. Bracket the whole drag for undo.
        auto drag = [&](const char* label, float* v, float speed, float lo, float hi,
                        const char* fmt, const char* undoLabel) {
            if (ImGui::DragFloat(label, v, speed, lo, hi, fmt)) changed = true;
            if (ImGui::IsItemActivated())            s.beginEdit();
            if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit(undoLabel);
        };
        drag("Offset",  &L.offset,  0.02f, 0.0f, 20.0f, "%.2f m", "Side offset");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(L.onRoad
                ? "Distance from the middle of the road: 0 = dead centre, half\n"
                  "the road width = out at the edge. Which way it goes is the\n"
                  "Side above; Both mirrors it into two tracks."
                : "Distance beyond the road edge (added to the half-width).");
        if (L.onRoad) {
            ImGui::SameLine();
            ImGui::TextDisabled("(edge at %.2f m)", s.road().width * 0.5f);
        }
        drag("Spacing", &L.spacing, 0.05f, 0.25f, 60.0f, "%.2f m", "Side spacing");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Metres between copies. For a continuous rail/curb,\n"
                              "match this to the model's length.");
        drag("Rotation", &L.yaw, 1.0f, -180.0f, 180.0f, "%+.0f deg", "Side rotation");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Extra turn about the upright axis, on top of the road's\n"
                              "heading: 0 = the model faces along the road, 90 = across\n"
                              "it. The whole run turns together.");
        drag("Scale",   &L.scale,   0.01f, 0.05f, 20.0f, "%.2f x", "Side scale");
        drag("Height",  &L.lift,    0.02f, -5.0f, 10.0f, "%+.2f m", "Side height");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(L.onRoad
                ? "Raise the model above the road surface it stands on\n"
                  "(negative sinks it into the asphalt)."
                : "Raise the model above the ground it stands on\n"
                  "(a guard-rail beam rides up on its posts).");

        // Face-toward-road only matters for a one-sided profile on Both/Alternate,
        // and means nothing in the middle of the carriageway -- so it is hidden
        // there rather than left as a switch that turns a cone backwards.
        if (!L.onRoad) {
            if (ImGui::Checkbox("Face the road", &L.faceRoad)) {
                L.faceRoad = !L.faceRoad;
                s.beginEdit(); L.faceRoad = !L.faceRoad; s.endEdit("Side facing");
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Flip the far side 180 so a one-sided model (rail\n"
                                  "beam, curb face) keeps its front toward the road.");
        }

        // Knockable: a dynamic body in Play, so the car sends it flying. Off = a
        // solid barrier (rails/curbs). Its mass tunes how easily it shifts.
        if (ImGui::Checkbox("Knockable", &L.knockable)) {
            L.knockable = !L.knockable;
            s.beginEdit(); L.knockable = !L.knockable; s.endEdit("Side knockable");
            changed = true; // the batch caches knockable/mass; refresh it
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("In Play this object is a loose, dynamic body -- a\n"
                              "vehicle bowls it over and it flies off (posts,\n"
                              "bollards). Off = a fixed barrier that stops a car.");
        if (L.knockable) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::DragFloat("Mass", &L.mass, 0.1f, 0.1f, 500.0f, "%.1f kg"))
                changed = true;
            if (ImGui::IsItemActivated())            s.beginEdit();
            if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit("Side mass");
        }
        ImGui::PopID();
    }
    ImGui::PopID(); // "sideobjects"

    if (changed) s.road().rebuildSideObjects();
}

// Prefabs along the road: the same placement rule as a side object, but what it
// leaves behind is REAL entities -- one prefab instance per station, in a single
// undoable step. That is the whole difference, and the section says so: a prefab
// is an entity subtree, and its scripts, lights and physics only mean anything as
// scene objects, so this stamps once instead of deriving. The placements stay put
// when the road later moves.
// Decals: images laid on the carriageway (see RoadDecal.hpp). Same shape as the
// side objects above -- a list of rules, each derived into geometry rather than
// authored -- and edited the same way, so the two read alike. A live change
// re-derives the patches at once; rebuildDecals is a few hundred vertices.
//
// Everything here is a number you can type. That is not a style choice: landing a
// start line ON the start line by dragging a gizmo across a viewport needs a
// steadier hand than this editor is allowed to assume, and "62 metres along, 0
// across" is a placement anybody can hit.
void decalSection(const PanelState& s) {
    if (!ui::header("Decals", ImGuiTreeNodeFlags_DefaultOpen)) return;

    auto add = [&](roaddecal::Blend b) {
        s.beginEdit();
        roaddecal::Decal d = roaddecal::preset(b);
        // Drop it where the road can actually show it: the middle of what is
        // built, rather than at metre zero on top of whatever is already there.
        d.dist = s.road().roadLength() * 0.5f;
        s.road().decals.push_back(d);
        s.endEdit("Add decal");
        s.road().rebuildDecals();
    };
    if (ImGui::Button("+ Marking")) add(roaddecal::Blend::Cutout);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A painted marking -- arrow, number, start box, logo.\n"
                          "The image's transparent parts are cut away, so it sits\n"
                          "in the ordinary opaque pass and costs nothing extra.");
    ImGui::SameLine();
    if (ImGui::Button("+ Stain")) add(roaddecal::Blend::Blend);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A translucent patch -- oil, scorch, wet smear, soft\n"
                          "shadow. The image is blended over the tarmac, so the\n"
                          "asphalt still shows through it.");
    ImGui::SameLine();
    if (ImGui::Button("+ Patch")) add(roaddecal::Blend::Opaque);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A solid surface swap -- concrete apron, repaved\n"
                          "stretch, full start grid. The whole rectangle is\n"
                          "painted; the image's alpha is ignored.");

    if (s.road().decals.empty()) {
        ImGui::TextDisabled("None. Add a marking, stain or patch.");
        return;
    }
    const float roadLen = s.road().roadLength();
    if (s.road().verts() == 0 || roadLen <= 0.0f)
        ImGui::TextDisabled("Build the road to see them placed.");
    else
        ImGui::TextDisabled("Road is %.0f m long.", roadLen);

    bool changed = false; // re-derive the patches this frame (live preview)

    ImGui::PushID("decals");        // own ID scope -- see the note in bridgeSection
    for (int i = 0; i < static_cast<int>(s.road().decals.size()); ++i) {
        roaddecal::Decal& d = s.road().decals[i];
        ImGui::PushID(i);
        ImGui::Separator();

        // Enable + what it is + delete, on one line. The image's own file name is
        // the label once there is one: with half a dozen decals on a track, "the
        // one called startgrid.png" is how the author thinks of them.
        if (ImGui::Checkbox("##en", &d.enabled)) {
            d.enabled = !d.enabled;               // snapshot the pre-toggle state
            s.beginEdit();
            d.enabled = !d.enabled;
            s.endEdit("Toggle decal");
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(d.texture.empty() ? roaddecal::blendName(d.blend)
                                                 : d.texture.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
        if (ImGui::SmallButton("X")) {
            s.beginEdit();
            s.road().decals.erase(s.road().decals.begin() + i);
            s.endEdit("Remove decal");
            ImGui::PopID();
            changed = true;
            break; // the list shifted under us
        }

        // The image: the WIDE list -- every image the scan could decode, minus the
        // normal maps -- because a decal is any picture at all rather than a
        // tileable surface. Same list, and the same reasoning, as the glow map.
        const std::string preview = d.texture.empty() ? "(pick an image)" : d.texture;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##image", preview.c_str())) {
            for (const std::string& f : s.road().emisFiles)
                if (ImGui::Selectable(f.c_str(), d.texture == f)) {
                    const std::string old = d.texture;
                    d.texture = old;               // snapshot pre-change
                    s.beginEdit();
                    d.texture = f;
                    s.endEdit("Decal image");
                    changed = true;
                }
            if (s.road().emisFiles.empty())
                ImGui::TextDisabled("No images found (drop a .png in the project)");
            ImGui::EndCombo();
        }

        // Mode. Combo gives no before-value, so snapshot around it by hand.
        {
            int mode = static_cast<int>(d.blend);
            const char* items = "Cut out\0Blended\0Opaque\0";
            if (ImGui::Combo("Mode", &mode, items)) {
                const auto nw = static_cast<roaddecal::Blend>(mode);
                s.beginEdit();
                d.blend = nw;
                s.endEdit("Decal mode");
                changed = true;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What the image's transparency means.\n"
                              "Cut out: painted or not, nothing in between --\n"
                              "  drawn with the opaque scene, never sorts wrongly.\n"
                              "Blended: the tarmac shows through -- the only mode\n"
                              "  where Opacity does anything, and the only one\n"
                              "  that pays for back-to-front sorting.\n"
                              "Opaque: the whole rectangle is painted.");

        // Every knob is a DragFloat: click to type an exact value, drag to sweep.
        // The whole drag is one undo step (see the side-object section).
        auto drag = [&](const char* label, float* v, float speed, float lo, float hi,
                        const char* fmt, const char* undoLabel) {
            if (ImGui::DragFloat(label, v, speed, lo, hi, fmt)) changed = true;
            if (ImGui::IsItemActivated())            s.beginEdit();
            if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit(undoLabel);
        };

        drag("Along",  &d.dist,   0.25f, 0.0f, std::max(roadLen, 1.0f), "%.1f m",
             "Decal position");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Metres from the start of the road to the middle of\n"
                              "the decal, measured along the centreline.");
        drag("Across", &d.offset, 0.05f, -40.0f, 40.0f, "%+.2f m", "Decal offset");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Metres right of the middle of the road (negative =\n"
                              "left). The carriageway edge is at %.2f m.",
                              s.road().width * 0.5f);
        drag("Length", &d.length, 0.05f, 0.1f, 200.0f, "%.2f m", "Decal length");
        drag("Width",  &d.width,  0.05f, 0.1f, 200.0f, "%.2f m", "Decal width");
        drag("Turn",   &d.spin,   1.0f, -180.0f, 180.0f, "%+.0f deg", "Decal turn");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Turn the image on the road: 0 = the top of the\n"
                              "picture points the way the road goes, 180 = it\n"
                              "faces oncoming traffic, 90 = it lies across.\n"
                              "The patch stays glued to the surface either way.");

        // Repeats: an int, because "three start boxes" is a count, not a sweep.
        if (ImGui::DragInt("Copies", &d.repeat, 0.1f, 1, 512)) changed = true;
        if (ImGui::IsItemActivated())            s.beginEdit();
        if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit("Decal copies");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How many, each one further along the road. A dashed\n"
                              "centre line, a run of chevrons, a column of start\n"
                              "boxes -- one rule instead of twenty decals.");
        if (d.repeat > 1) {
            drag("Every", &d.spacing, 0.1f, 0.0f, 200.0f, "%.2f m", "Decal spacing");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Metres between copies, centre to centre.\n"
                                  "0 = one length, i.e. butt-jointed.");
        }

        // One knob per mode, and only the one that does anything there: an
        // Opacity slider on a cut-out decal is a control that moves and changes
        // nothing, which is worse than no control at all.
        if (d.blend == roaddecal::Blend::Blend) {
            drag("Opacity", &d.opacity, 0.01f, 0.0f, 1.0f, "%.2f", "Decal opacity");
        } else if (d.blend == roaddecal::Blend::Cutout) {
            drag("Cut at", &d.cutoff, 0.01f, 0.0f, 1.0f, "%.2f", "Decal cutoff");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Image alpha below this is thrown away. Raise it\n"
                                  "if a soft-edged image leaves a halo, lower it if\n"
                                  "thin strokes break up.");
        }

        if (ImGui::ColorEdit3("Tint", &d.tint.x, ImGuiColorEditFlags_NoInputs))
            changed = true;
        if (ImGui::IsItemActivated())            s.beginEdit();
        if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit("Decal tint");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Multiplies the image. White leaves it alone; this is\n"
                              "how one white arrow becomes a whole set of coloured\n"
                              "ones without a second file.");

        drag("Glow", &d.glowStrength, 0.02f, 0.0f, 8.0f, "%.2f", "Decal glow");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Light the image's own painted texels after the scene\n"
                              "is lit, so the marking stays bright at night -- what\n"
                              "makes a boost pad read as a lit strip. 0 = ordinary\n"
                              "paint.");
        if (d.glowStrength > 0.0f) {
            if (ImGui::ColorEdit3("Glow colour", &d.glow.x,
                                  ImGuiColorEditFlags_NoInputs))
                changed = true;
            if (ImGui::IsItemActivated())            s.beginEdit();
            if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit("Decal glow colour");
        }

        drag("Lift", &d.lift, 0.005f, 0.0f, 0.5f, "%.3f m", "Decal lift");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Height above the asphalt. Raise it if the decal\n"
                              "flickers against the road at a distance; too much\n"
                              "and it reads as a sticker hovering over the track.");

        ImGui::PopID();
    }
    ImGui::PopID();

    if (changed) s.road().rebuildDecals();
}

void prefabSection(const PanelState& s) {
    if (!ui::header("Prefabs along the road")) return;
    roadprefab::Settings& c = s.prefabCfg;

    // Prefab picker. The folder is only scanned while the combo is open.
    const std::string preview = c.name.empty() ? "(pick a prefab)" : c.name;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##prefab", preview.c_str())) {
        const auto items = s.listPrefabs ? s.listPrefabs()
                                         : std::vector<std::pair<std::string, std::string>>();
        for (const auto& it : items) {
            const bool sel = (it.second == c.path);
            if (ImGui::Selectable((it.first + "##" + it.second).c_str(), sel)) {
                c.path = it.second;
                c.name = it.first;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        if (items.empty())
            ImGui::TextDisabled("(no prefabs in this project yet)");
        ImGui::EndCombo();
    }

    ImGui::Checkbox("On the road", &c.onRoad);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: stands on the carriageway, Offset measured from the\n"
                          "middle of the road, height taken from the road surface.\n"
                          "Off: stands beside the road, Offset measured beyond the\n"
                          "edge, height taken from the ground.");
    {
        const char* items = "Left\0Right\0Both\0Alternate\0";
        ImGui::Combo("Side##prefab", &c.side, items);
    }
    ImGui::DragFloat("Offset##prefab",  &c.offset,  0.02f, 0.0f, 20.0f, "%.2f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(c.onRoad
            ? "Distance from the middle of the road (the edge is at %.2f m)."
            : "Distance beyond the road edge.", s.road().width * 0.5f);
    ImGui::DragFloat("Spacing##prefab", &c.spacing, 0.05f, 0.5f, 200.0f, "%.2f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Metres between copies. Watch the count below before\n"
                          "you place -- a tight spacing on a long road is a lot\n"
                          "of objects.");
    ImGui::DragFloat("Rotation##prefab", &c.yaw, 1.0f, -180.0f, 180.0f, "%+.0f deg");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Extra turn about the upright axis, on top of the road's\n"
                          "heading: 0 = along the road, 90 = across it.");
    ImGui::DragFloat("Scale##prefab",   &c.scale,   0.01f, 0.05f, 20.0f, "%.2f x");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uniform scale for the whole prefab: child offsets, box\n"
                          "sizes and model scales all grow together.");
    ImGui::DragFloat("Height##prefab",  &c.lift,    0.02f, -5.0f, 20.0f, "%+.2f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Raise the prefab's root above the surface it lands on.\n"
                          "A prefab has no model base to sit on -- its own layout\n"
                          "decides that -- so this is how you settle it.");

    // What the button would do, before it does it: the real placement walk, so
    // the count cannot disagree with the result.
    const int n = static_cast<int>(s.road().placeLine(roadprefab::asLine(c)).size());
    const bool ready = !c.path.empty() && n > 0;
    if (s.road().verts() == 0)
        ImGui::TextDisabled("Build the road first -- placements follow a built centreline.");
    else if (c.path.empty())
        ImGui::TextDisabled("Pick a prefab to place.");
    else
        ImGui::Text("Places %d %s", n, n == 1 ? "copy" : "copies");

    ImGui::BeginDisabled(!ready || !s.placePrefabs);
    if (ImGui::Button("Place along the road", ImVec2(-1.0f, 0.0f))) s.placePrefabs();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Instantiates the prefab at every station as real,\n"
                          "selectable objects, grouped under \"Road prefabs\" and\n"
                          "undoable in one step. Unlike the side objects above they\n"
                          "do NOT follow the road afterwards -- move the road and\n"
                          "they stay where they were put.");
}

// The road.txt scratch save/load. Predates scenes carrying roads and is kept as a
// quick way to move a spline between scenes by hand.
void scratchFileSection(const PanelState& s) {
    ImGui::Separator();
    if (ImGui::Button("Save")) {
        std::ofstream f("road.txt");
        // "x z height" per line -- an older two-column road.txt still loads, its
        // points just come back sitting on the terrain.
        for (int i = 0; i < static_cast<int>(s.road().roadPts.size()); ++i)
            f << s.road().roadPts[i].x << ' ' << s.road().roadPts[i].y << ' '
              << s.road().liftOf(i) << '\n';
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        std::ifstream f("road.txt");
        if (f) {
            s.road().clearPoints();
            std::string line;
            while (std::getline(f, line)) {
                std::istringstream ls(line);
                glm::vec2 p;
                float h = 0.0f;
                if (!(ls >> p.x >> p.y)) continue;
                ls >> h; // absent in the old format -> stays 0
                s.road().insertPoint(static_cast<int>(s.road().roadPts.size()), p, h);
            }
            s.sel = -1;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(road.txt)");
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Roads", &s.show)) {
        // --- The roads in this scene -----------------------------------------
        // A scene holds as many roads as the author draws (see RoadSet), the way
        // it holds as many watercourses. This picker says which one every control
        // below edits; the checkbox beside each row hides it without deleting it.
        //
        // The row checkbox is deliberately NOT bracketed for undo, like the "Show
        // roads" toggle it replaces: it changes what you can see, not what the
        // scene is.
        {
            RoadSet& rs = s.roads;
            ui::sectionText("Roads");
            const float rows = static_cast<float>(rs.count() < 3 ? 3 : rs.count());
            if (ImGui::BeginChild("roadlist",
                                  ImVec2(0.0f, ImGui::GetFrameHeightWithSpacing() *
                                                   (rows > 6.0f ? 6.0f : rows) + 6.0f),
                                  true)) {
                for (int i = 0; i < rs.count(); ++i) {
                    RoadSystem& r = rs.at(i);
                    ImGui::PushID(i);
                    ImGui::Checkbox("##on", &r.enabled);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Draw this road (and drive on it).");
                    ImGui::SameLine();
                    char row[160];
                    std::snprintf(row, sizeof(row), "%s  (%d pts%s)",
                                  r.name.c_str(),
                                  static_cast<int>(r.roadPts.size()),
                                  r.needsBuild ? ", not built" : "");
                    if (ImGui::Selectable(row, i == rs.selected()) && s.selectRoad)
                        s.selectRoad(i);
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            if (ImGui::Button("Add road") && s.addRoad) s.addRoad();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A second carriageway with its own width, surface,\n"
                                  "rails and city -- drawn and built on its own.");
            ImGui::SameLine();
            // The last road cannot go: the viewport handles, this panel and the
            // whole editor are written against "the road being edited" (see
            // RoadSet). Clearing its points is what emptying the scene looks like.
            ImGui::BeginDisabled(rs.count() < 2);
            if (ImGui::Button("Delete road") && s.deleteRoad)
                s.deleteRoad(rs.selected());
            ImGui::EndDisabled();
            if (rs.count() < 2 && ImGui::IsItemHovered())
                ImGui::SetTooltip("The scene always has one road. Use Clear below\n"
                                  "to empty this one.");
            ImGui::SameLine();
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s", s.road().name.c_str());
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputText("##roadname", buf, sizeof(buf)))
                    s.road().name = buf;
                if (ImGui::IsItemActivated())            s.beginEdit();
                if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit("Rename road");
            }
        }

        if (ImGui::Checkbox("Edit mode", &s.editMode) && s.editMode) s.grabLMB();
        if (s.editMode) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                               "Click ground = add | drag handle = move | Del = delete");
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                               "Ctrl+drag a handle = raise/lower it");
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                               "Shift+click a second handle = pick a bridge");
            ImGui::TextDisabled("With an end handle selected, clicks extend from");
            ImGui::TextDisabled("that end -- which is how you draw under a bridge.");
            ImGui::TextDisabled("Arrows nudge (Shift = coarse), PgUp/PgDn height,");
            ImGui::TextDisabled("Esc deselects, Ctrl+Z undoes.");
        } else {
            ImGui::TextDisabled("Enable edit mode to place and drag handles");
        }
        ImGui::Text("Points: %d", static_cast<int>(s.road().roadPts.size()));
        ImGui::SameLine();
        if (s.sel >= 0 && s.sel2 >= 0)
            ImGui::Text("| selected #%d \xE2\x86\x92 #%d", s.sel, s.sel2);
        else if (s.sel >= 0) ImGui::Text("| selected #%d", s.sel);
        else                 ImGui::TextDisabled("| none selected");

        // --- Selected point: its height above the graded ground ---------------
        // Editing it here rather than only by Ctrl+drag is what makes a level
        // crossing or a fixed embankment height reproducible.
        if (s.sel >= 0 && s.sel < static_cast<int>(s.road().roadPts.size())) {
            float lift = s.road().liftOf(s.sel);
            ImGui::SetNextItemWidth(-90.0f);
            if (ImGui::DragFloat("Height##pt", &lift, 0.05f, -50.0f, 50.0f, "%+.2f m"))
                s.road().setLift(s.sel, lift);
            // Bracket the whole drag: activation is the frame the grab starts,
            // before any value change, so the snapshot is the pre-edit height.
            if (ImGui::IsItemActivated())            s.beginEdit();
            if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit("Point height");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Height of point #%d above the ground it would\n"
                                  "otherwise follow. The terrain is graded along:\n"
                                  "up builds an embankment, down cuts a trench.\n"
                                  "Use a bridge to span instead of filling.", s.sel);
            ImGui::SameLine();
            ImGui::BeginDisabled(lift == 0.0f);
            if (ImGui::SmallButton("Ground")) {
                s.beginEdit();
                s.road().setLift(s.sel, 0.0f);
                s.endEdit("Point to ground");
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Put the point back on the terrain (height 0).");

            // --- ...and its cross-fall ----------------------------------------
            // Banking is a per-point value ramped along the road exactly like the
            // height, so a corner is banked by setting its apex and letting the
            // entry and exit ease into it -- not by tilting a whole stretch.
            float bank = s.road().bankOf(s.sel);
            ImGui::SetNextItemWidth(-90.0f);
            if (ImGui::DragFloat("Tilt##pt", &bank, 0.2f, -60.0f, 60.0f, "%+.1f deg"))
                s.road().setBank(s.sel, bank);
            if (ImGui::IsItemActivated())            s.beginEdit();
            if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit("Point tilt");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Cross-fall at point #%d: the carriageway rolls\n"
                                  "about its centreline. Positive drops the RIGHT\n"
                                  "edge, which banks a right-hand corner into the\n"
                                  "turn. The corridor is graded along with the\n"
                                  "tilt, so the ground follows the low edge instead\n"
                                  "of poking through it.\n\n"
                                  "Capped at 60 deg: past that a road is asking to\n"
                                  "be a loop, which this ribbon cannot be.", s.sel);
            ImGui::SameLine();
            ImGui::BeginDisabled(bank == 0.0f);
            if (ImGui::SmallButton("Level")) {
                s.beginEdit();
                s.road().setBank(s.sel, 0.0f);
                s.endEdit("Point level");
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Take the cross-fall back out (tilt 0).");
        }

        // --- Build: commit the previewed spline into the terrain --------------
        // Editing only updates the preview; Build grades the road into the ground
        // (flush + smooth) and lofts the drivable mesh + collider.
        ImGui::Separator();
        if (s.road().needsBuild)
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.30f, 1.0f),
                               "Preview \xE2\x80\x93 not built yet");
        else if (s.road().verts() > 0)
            ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.0f),
                               "Built \xE2\x80\x93 embedded in terrain");
        else
            ImGui::TextDisabled("No road built");
        ImGui::BeginDisabled(s.road().roadPts.size() < 2);
        if (ImGui::Button(s.road().needsBuild ? "Build road into terrain" : "Rebuild road",
                          ImVec2(-1.0f, 0.0f)))
            s.build();
        ImGui::EndDisabled();
        // One button, every road: the corridors share the height field, and one
        // cut against ground another road had already graded is a crossing that
        // moves every time either half is touched (see RoadSet::buildAll). Said
        // out loud only when there is a second road waiting, so a scene with one
        // reads exactly as it used to.
        if (s.roads.count() > 1 && s.roads.anyNeedsBuild())
            ui::hint("Builds all %d roads.", s.roads.count());
        ImGui::Separator();

        // Everything that changes the road's shape or the corridor it grades feeds
        // `rc`, and `rc` is what marks it dirty -- so a tunable can't quietly take
        // effect on some frames and not others.
        bool rc = false;
        if (ImGui::Checkbox("Closed loop", &s.road().closed)) {
            // Toggled already; bracket around the value it had before.
            s.road().closed = !s.road().closed;
            s.beginEdit();
            s.road().closed = !s.road().closed;
            s.endEdit("Closed loop");
            rc = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Join the last control point back to the first\n"
                              "(needs at least 3 points).");
        rc |= ImGui::SliderFloat("Width", &s.road().width, 1.0f, 20.0f, "%.1f m");

        // Raised edges: a wall along both sides, part of the carriageway rather
        // than scenery beside it. Live-previewed like the rest of the section, so
        // the two sliders can be dialled in against the craft they are for.
        const bool edgeChanged =
            ImGui::SliderFloat("Edge height", &s.road().edgeWidth, 0.0f, 6.0f, "%.2f m") |
            ImGui::SliderFloat("Edge angle", &s.road().edgeAngle, 0.0f, 90.0f, "%.0f deg");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = flat (no edge at all), 90 = a vertical wall.\n"
                              "In between it is a bank a craft can ride up and be\n"
                              "turned back down by.");
        if (edgeChanged) {
            rc = true;
            showAtOnce(s);   // the section changed: show it, don't wait for Build
        }
        if (s.road().edgeWidth > 0.001f)
            ui::hint("The road is %.1f m wide across everything, and the lip\n"
                     "stands %.2f m proud of the carriageway.",
                     s.road().surfaceHalf() * 2.0f, s.road().edgeRise());

        rc |= ImGui::SliderFloat("Smoothing", &s.road().grade, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How much the road grade is flattened along its\n"
                              "length (higher = smoother/steadier slope).");
        rc |= ImGui::SliderFloat("Shoulder", &s.road().shoulder, 0.0f, 12.0f, "%.1f m");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Width of the terrain blend beyond the road edge\n"
                              "that eases back into the natural ground.");
        rc |= ImGui::SliderFloat("Texture tile", &s.road().texTile, 2.0f, 24.0f, "%.1f m");

        ImGui::Separator();
        rc |= bridgeSection(s);
        rc |= tunnelSection(s);

        ImGui::Separator();
        rc |= loopSection(s);

        if (rc) s.road().needsBuild = true;

        // Side objects manage their own (cheap) re-derive and undo, independent of
        // the terrain-grading rebuild `rc` tracks -- so they stay out of it.
        ImGui::Separator();
        sideSection(s);

        // Decals re-derive themselves the same way, and are equally out of `rc`.
        ImGui::Separator();
        decalSection(s);

        ImGui::Separator();
        prefabSection(s);

        // Edge fade is a pure shader effect (alpha taper at the ribbon edges) -- it
        // needs no rebuild, so it's deliberately kept out of `rc`.
        ImGui::SliderFloat("Edge fade", &s.road().fadeWidth, 0.0f, s.road().width * 0.5f,
                           "%.1f m");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fade the road to transparent over this many metres\n"
                              "at each edge, blending it into the terrain (0 = off).");

        // Also a pure shader effect -- no rebuild, so out of `rc` like the fade.
        ImGui::SliderFloat("Rain rings", &s.road().rainRings, 0.0f, 3.0f, "%.2fx");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How hard rain drops dent the wet road (0 = off).\n"
                              "Only shows while it is actually raining AND the\n"
                              "surface is wet -- turn the storm up to judge it.");

        // The road's own wetness, weather or no weather. Same shader path as the
        // rain's, so what this shows at 1.0 is exactly what a downpour shows.
        ImGui::SliderFloat("Wetness", &s.road().wetness, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How wet the carriageway looks with NO rain: darker\n"
                              "tarmac and a sharper sheen, the higher it goes.\n"
                              "Rain adds on top (the wetter of the two wins), so\n"
                              "this is a floor, not an override. 0 = weather only.");

        ImGui::SliderFloat("Wet reflection", &s.road().wetReflect, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How much the wet road mirrors its surroundings.\n"
                              "0 = only the sun's sheen (and no cubemap capture,\n"
                              "so it costs nothing); above that the scene around\n"
                              "the road is reflected -- and the frame pays for a\n"
                              "probe render whenever the road is wet, rain included.");

        // Where that wetness stands. Any greyscale image works as the mask, so
        // the picker is the wide list (the same one the glow map uses).
        const std::string wetLabel = s.road().wetMap.empty() ? "(none)" : s.road().wetMap;
        if (ImGui::BeginCombo("Wet map", wetLabel.c_str())) {
            if (ImGui::Selectable("(none)", s.road().wetMap.empty()))
                s.road().setWetMap(std::string());
            for (const std::string& f : s.road().emisFiles)
                if (ImGui::Selectable(f.c_str(), s.road().wetMap == f))
                    s.road().setWetMap(f);
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Greyscale mask for the wetness: black stays dry,\n"
                              "white gets the full sheen. road_puddles.png is a\n"
                              "seamless one to start from (road_wet_grain.png is\n"
                              "the finer variant). Without one the whole road is\n"
                              "equally wet, which reflects like plastic.");
        // Both are pure shader parameters -- no rebuild, like the fade above.
        ImGui::BeginDisabled(s.road().wetMap.empty());
        ImGui::SliderFloat("Puddle size", &s.road().wetTile, 3.0f, 150.0f, "%.0f m");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Metres of road one tile of the map covers.\n"
                              "Small = frequent patches, large = long wet and\n"
                              "dry stretches.");
        ImGui::SliderFloat("Puddle depth", &s.road().wetVar, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How much of the map to believe. 0 = even sheen\n"
                              "(as if there were no map), 1 = the dry parts of\n"
                              "the mask go fully matte.");
        ImGui::SliderFloat("Puddle edge", &s.road().wetShore, 0.02f, 0.60f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How sharp the shoreline is. The map is read as a\n"
                              "height field that the rain fills up, so a small\n"
                              "value gives puddles a visible rim (they read as\n"
                              "water) and a large one fades them out into damp\n"
                              "tarmac. The wetter it gets, the further they grow.");
        ImGui::SliderFloat("Puddle stretch", &s.road().wetStretch, 1.0f, 4.0f, "%.1fx");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How far the patches are drawn out along the drive.\n"
                              "1 = round. Real puddles run downhill and get\n"
                              "dragged by the traffic, so they are longer than\n"
                              "they are wide.");
        ImGui::EndDisabled();

        // Surface texture picker (any diffuse texture in textures/). Picking one
        // brings its matching normal map along, so the common case needs no second
        // trip to the combo below -- but the combo is there to override or clear it.
        if (!s.road().texFiles.empty() &&
            ImGui::BeginCombo("Surface", s.road().texFiles[s.road().texSel].c_str())) {
            for (int i = 0; i < static_cast<int>(s.road().texFiles.size()); ++i) {
                const bool sel = (i == s.road().texSel);
                if (ImGui::Selectable(s.road().texFiles[i].c_str(), sel)) {
                    s.road().setSurface(s.road().texFiles[i]);
                    s.road().texSel = i;
                    const std::string n = s.road().normalFor(s.road().texFiles[i]);
                    s.road().setNormal(n); // "" clears it: a pack without one stays flat
                    s.road().normSel = -1;
                    for (int k = 0; k < static_cast<int>(s.road().normFiles.size()); ++k)
                        if (s.road().normFiles[k] == n) s.road().normSel = k;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // Normal map: the asphalt's grain. Without one the ribbon is lit by its
        // geometry normal alone and reads as painted on, especially under a low sun.
        const char* curN = (s.road().normSel >= 0 &&
                            s.road().normSel < static_cast<int>(s.road().normFiles.size()))
                               ? s.road().normFiles[s.road().normSel].c_str()
                               : "(none)";
        if (ImGui::BeginCombo("Normal", curN)) {
            if (ImGui::Selectable("(none)", s.road().normSel < 0)) {
                s.road().setNormal(std::string());
                s.road().normSel = -1;
            }
            for (int i = 0; i < static_cast<int>(s.road().normFiles.size()); ++i) {
                const bool sel = (i == s.road().normSel);
                if (ImGui::Selectable(s.road().normFiles[i].c_str(), sel)) {
                    s.road().setNormal(s.road().normFiles[i]);
                    s.road().normSel = i;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Follows the surface by name when you pick one\n"
                              "(asphalt_02_diff_4k -> asphalt_02_nor_gl_4k).\n"
                              "Strength is the global \"Normal strength\" in Terrain.");

        // Glow: self-illumination on top of the lit surface, so the track stays
        // bright at night. Pure shader state -- no rebuild, like the fade above.
        ui::sectionText("Glow");
        ImGui::SliderFloat("Glow strength", &s.road().emissionStrength, 0.0f, 6.0f,
                           "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = off. Emission is added after lighting, so the\n"
                              "road keeps glowing at night and through fog.\n"
                              "Push past 1 for a hard neon look.");
        ImGui::BeginDisabled(s.road().emissionStrength <= 0.0f);
        ImGui::ColorEdit3("Glow colour", &s.road().emission.x);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Multiplies the glow map. With no map the whole\n"
                              "carriageway glows in this colour.");

        const char* curE = (s.road().emisSel >= 0 &&
                            s.road().emisSel < static_cast<int>(s.road().emisFiles.size()))
                               ? s.road().emisFiles[s.road().emisSel].c_str()
                               : "(whole road)";
        if (ImGui::BeginCombo("Glow map", curE)) {
            if (ImGui::Selectable("(whole road)", s.road().emisSel < 0))
                s.road().setEmission(std::string());
            for (int i = 0; i < static_cast<int>(s.road().emisFiles.size()); ++i) {
                const bool sel = (i == s.road().emisSel);
                if (ImGui::Selectable(s.road().emisFiles[i].c_str(), sel)) {
                    s.road().setEmission(s.road().emisFiles[i]);
                    s.road().emisSel = i;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Only the map's lit texels glow -- centre lines,\n"
                              "edge strips, light channels. The image spans the\n"
                              "road width exactly once, left to right, whatever\n"
                              "the surface tiling is: paint a stripe down the\n"
                              "middle of the image and it lands on the centre line.");
        ImGui::BeginDisabled(s.road().emisSel < 0);
        ImGui::SliderFloat("Glow repeat", &s.road().emissionTile, 1.0f, 200.0f,
                           "%.0f m");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Metres of road per repeat of the glow map along\n"
                              "the drive. Small values make chevrons/dashes.");
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        ImGui::BeginDisabled(s.sel < 0 || s.sel >= static_cast<int>(s.road().roadPts.size()));
        if (ImGui::Button("Delete selected")) s.removePoint(s.sel);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(s.road().roadPts.empty());
        // No "Undo point" button here: it only ever dropped the *last* point,
        // which is not what you did when you just moved or raised one. Road edits
        // are on the editor's real undo stack now (Ctrl+Z / Edit menu).
        if (ImGui::Button("Clear")) {
            s.beginEdit();
            s.road().clearPoints();
            s.road().bridges.clear();
            s.road().tunnels.clear();
            s.sel = s.sel2 = -1;
            s.endEdit("Clear road");
        }
        ImGui::EndDisabled();

        scratchFileSection(s);
    }
    ImGui::End();
}

} // namespace roadui
