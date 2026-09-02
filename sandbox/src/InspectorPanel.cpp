#include "InspectorPanel.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include "CameraSystem.hpp"
#include "Command.hpp"
#include "Component.hpp"
#include "Document.hpp"
#include "GliderTool.hpp"
#include "ModelLibrary.hpp"
#include "MultiShot.hpp"
#include "ParticleSystem.hpp"
#include "PrefabSystem.hpp"
#include "PropertyMeta.hpp"
#include "RaceGrid.hpp"
#include "RoadSet.hpp"
#include "ScriptSystem.hpp"
#include <fitzel/world/Terrain.hpp>
#include "UiStyle.hpp"
#include "VehicleTool.hpp"

namespace inspectorui {

void drawPanel(const PanelState& s) {
    if (!ImGui::Begin("Inspector")) { ImGui::End(); return; }

    if (s.sel.valid()) {
        Entity& b = s.entities[s.sel.index()];
        // Undo transaction: snapshot this entity's subtree before any
        // widget below mutates it (committed at the block's end).
        const std::vector<int> inspFrameIds   = s.collectSubtreeIds(b.id);
        std::vector<Entity>    inspFrameStart = s.snapshotEntities(inspFrameIds);
        // Set true when the Camera branch below sets the Main Camera via
        // setMainCamera(): that pushes its own multi-camera undo step, so
        // the per-entity edit wrapper must not also log this frame.
        bool                   mainCamJustSet = false;
        ui::sectionText(entityTypeName(b.type));

        // Auto-generated fields: the property table (PropertyMeta.hpp)
        // declares each field once -> the right widget, range and
        // visibility fall out here. Adding a field is a table entry.
        for (const Property& pr : entityProperties()) {
            if (!(pr.typeMask & typeBit(b.type))) continue;
            if (pr.visible && !pr.visible(&b)) continue;
            // Children follow center/rotation edits automatically
            // (resolveHierarchy); no per-field side effects left.
            drawProperty(pr, &b);
        }

        if (b.type == EntityType::Sun) {
            ImGui::SliderFloat("Time of day", &s.timeOfDay, 0.0f, 24.0f, "%.1f h");
            ImGui::SameLine();
            ImGui::Checkbox("Pause", &s.timePaused);
            ImGui::TextDisabled("The sun drives the sky and casts shadows.");
        } else {
            // --- Bespoke fields (enumerate project state) ------------
            if (auto* mdl = b.components.get<ModelComponent>()) {
                LoadedModel* lm = s.models.byId(mdl->modelId);
                ImGui::Text("Model: %s", lm ? lm->name.c_str() : "(missing)");
            }
            ImGui::Text("Parent: %s",
                        b.parent < 0 ? "(root)" : ("id " + std::to_string(b.parent)).c_str());
            if (ImGui::Button("Drop to ground"))
            {
                const glm::mat4 pw = s.parentWorldMat(b);
                s.setWorld(b, glm::vec3(b.center.x,
                    s.streamer.heightAt(b.center.x, b.center.z) + b.half.y,
                    b.center.z), b.rotation, b.parent >= 0 ? &pw : nullptr);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(b.parent < 0);
            if (ImGui::Button("Unparent")) { b.parent = -1; s.rebaseLocal(b, nullptr); }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Delete##insp")) s.deleteEntity(s.sel.index());
        }
        // Components: optional attached capabilities. Each renders from
        // its own metadata; add/remove is open via the type registry.
        // (Re-fetch: Delete##insp above may have cleared the selection.)
        if (s.sel.valid()) {
            Entity& be = s.entities[s.sel.index()];
            ui::sectionText("Components");
            for (std::size_t ci = 0; ci < be.components.items.size(); ++ci) {
                ComponentBase* c = be.components.items[ci].get();
                ImGui::PushID(static_cast<int>(ci));
                // Each component is a collapsible card: a semibold
                // header bar you can fold away (its background makes the
                // component's extent obvious), with a right-aligned X to
                // detach it. Folding keeps a busy inspector readable.
                bool addable = true;
                for (const auto& t : components::registry())
                    if (t.typeId == c->typeId()) { addable = t.addable; break; }
                const bool open = ui::header(c->displayName(),
                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
                // X (detach) sitting on the right of the header bar.
                // Engine-managed components (Sun) can't be removed.
                bool remove = false;
                if (addable) {
                    const float xW = ImGui::GetFrameHeight();
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - xW);
                    remove = ImGui::SmallButton("X");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Remove this component");
                }
                if (open) {
                    ImGui::Indent();
                if (auto* sc = dynamic_cast<ScriptComponent*>(c)) {
                    // Bespoke picker: enumerate the project's .lua files.
                    std::vector<std::string> luaFiles = s.listScripts();
                    const std::string cur = sc->file.empty() ? "(none)" : sc->file;
                    ImGui::SetNextItemWidth(-60.0f);
                    if (ImGui::BeginCombo("##scriptfile", cur.c_str())) {
                        if (ImGui::Selectable("(none)", sc->file.empty())) sc->file.clear();
                        for (const std::string& f : luaFiles)
                            if (ImGui::Selectable(f.c_str(), sc->file == f)) sc->file = f;
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(sc->file.empty());
                    if (ImGui::Button("Edit##scr")) s.openScript(sc->file);
                    ImGui::EndDisabled();
                    const bool scriptMissing = !sc->file.empty() &&
                        std::find(luaFiles.begin(), luaFiles.end(), sc->file) == luaFiles.end();
                    if (scriptMissing)
                        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f),
                            "Missing: s.scripts/%s", sc->file.c_str());
                    else if (!s.scripts.lastError().empty())
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                                           "Script error: %s", s.scripts.lastError().c_str());
                    // --- Exported parameters: the script's module-level
                    // globals (see ScriptParam). Each is a persisted,
                    // editable field whose value overrides the script's
                    // default when Play starts.
                    if (!sc->file.empty() && !scriptMissing) {
                        const ScriptParamScan& scan = s.scanScriptParams(sc->file);
                        if (!scan.ok) {
                            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f),
                                "Parameters unavailable: %s", scan.err.c_str());
                        } else if (!scan.defs.empty()) {
                            ImGui::Separator();
                            ImGui::TextDisabled("Script parameters");
                            // Reconcile the stored overrides with the
                            // current globals: keep a matching value, seed
                            // a new one from its default, drop the gone.
                            // (Idempotent, so it doesn't churn undo.)
                            std::vector<ScriptParam> merged;
                            merged.reserve(scan.defs.size());
                            for (const ScriptParam& def : scan.defs) {
                                const ScriptParam* prev = nullptr;
                                for (const ScriptParam& sp : sc->params)
                                    if (sp.sameShape(def)) { prev = &sp; break; }
                                merged.push_back(prev ? *prev : def);
                            }
                            sc->params = std::move(merged);
                            for (ScriptParam& sp : sc->params) {
                                ImGui::PushID(sp.name.c_str());
                                switch (sp.type) {
                                    case ScriptParam::Type::Number: {
                                        float v = static_cast<float>(sp.num);
                                        if (ImGui::DragFloat(sp.name.c_str(), &v, 0.1f))
                                            sp.num = v;
                                        break;
                                    }
                                    case ScriptParam::Type::Bool:
                                        ImGui::Checkbox(sp.name.c_str(), &sp.b);
                                        break;
                                    case ScriptParam::Type::String: {
                                        char buf[128];
                                        std::snprintf(buf, sizeof(buf), "%s", sp.str.c_str());
                                        if (ImGui::InputText(sp.name.c_str(), buf, sizeof(buf)))
                                            sp.str = buf;
                                        break;
                                    }
                                    case ScriptParam::Type::Vec3:
                                        ImGui::DragFloat3(sp.name.c_str(), &sp.vec.x, 0.1f);
                                        break;
                                    case ScriptParam::Type::Color:
                                        ImGui::ColorEdit3(sp.name.c_str(), &sp.vec.x);
                                        break;
                                }
                                ImGui::PopID();
                            }
                            if (ImGui::SmallButton("Reset to defaults"))
                                sc->params = scan.defs;
                        }
                    }
                } else if (auto* mc = dynamic_cast<MaterialComponent*>(c)) {
                    // Bespoke picker: pick from the material library.
                    const int mi = s.document.materialIndex(mc->material);
                    // "##pick": the component header above is also
                    // labelled "Material" -> same ID stack, same hash.
                    if (ImGui::BeginCombo("Material##pick", s.materials[mi].name.c_str())) {
                        // A search box at the top of the popup, focused
                        // as it opens: on a library of dozens, typing
                        // three letters beats scrolling to the right
                        // row. One shared buffer across the pickers is
                        // enough -- only one popup is open at a time --
                        // and it starts empty every time so a popup
                        // never opens already hiding most of the list.
                        if (ImGui::IsWindowAppearing()) {
                            s.matPickFilter[0] = 0;
                            ImGui::SetKeyboardFocusHere();
                        }
                        ui::searchBox("##matpickf", s.matPickFilter,
                                      s.matPickFilterCap);
                        for (int i = 0; i < static_cast<int>(s.materials.size()); ++i) {
                            if (!ui::icontains(s.materials[i].name.c_str(),
                                               s.matPickFilter)) continue;
                            const bool sel = (i == mi);
                            if (ImGui::Selectable(s.materials[i].name.c_str(), sel)) {
                                mc->material = s.materials[i].assetId;
                                s.matSel = i;
                            }
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Edit##mat")) { s.matSel = mi; s.showMaterials = true; }
                } else if (auto* mdl = dynamic_cast<ModelComponent*>(c)) {
                    // Scale drives the pick box (half) for the model.
                    if (ImGui::SliderFloat("Scale", &mdl->scale, 0.05f, 20.0f, "%.2f"))
                        if (LoadedModel* lm = s.models.byId(mdl->modelId))
                            be.half = s.modelHalf(*lm, mdl->scale);
                    // Which material each part of the model uses, and a
                    // picker to point it somewhere else. An imported
                    // model brings its own materials and nothing in the
                    // editor ever said so: the Materials panel lists
                    // them among all the others with no clue which mesh
                    // they belong to, and there was no way to put a part
                    // on a material of your own at all.
                    //
                    // Per MODEL, not per instance: primMaterialId lives
                    // on the LoadedModel, so this repoints every entity
                    // instancing it. Said out loud below rather than
                    // discovered by editing one lamppost and watching
                    // forty others change.
                    if (LoadedModel* lm = s.models.byId(mdl->modelId)) {
                        const int nParts =
                            static_cast<int>(lm->primMaterialId.size());
                        char hdr[64];
                        std::snprintf(hdr, sizeof(hdr),
                                      "Materials (%d)###modelmats", nParts);
                        if (nParts > 0 && ImGui::CollapsingHeader(hdr)) {
                            ImGui::TextDisabled(
                                "Applies to every instance of this model.");
                            for (int pi = 0; pi < nParts; ++pi) {
                                const int pm = s.document.materialIndex(
                                    lm->primMaterialId[pi]);
                                char lbl[48], btn[32];
                                std::snprintf(lbl, sizeof(lbl), "Part %d##pm%d",
                                              pi + 1, pi);
                                std::snprintf(btn, sizeof(btn), "Edit##pmb%d", pi);
                                ImGui::SetNextItemWidth(-72.0f);
                                if (ImGui::BeginCombo(
                                        lbl, pm >= 0 ? s.materials[pm].name.c_str()
                                                     : "(missing)")) {
                                    if (ImGui::IsWindowAppearing()) {
                                        s.matPickFilter[0] = 0;
                                        ImGui::SetKeyboardFocusHere();
                                    }
                                    ui::searchBox("##pmpickf", s.matPickFilter,
                                                  s.matPickFilterCap);
                                    for (int i = 0;
                                         i < static_cast<int>(s.materials.size());
                                         ++i) {
                                        if (!ui::icontains(
                                                s.materials[i].name.c_str(),
                                                s.matPickFilter)) continue;
                                        const bool sel = (i == pm);
                                        // Names repeat across a library, and
                                        // two Selectables sharing a label are
                                        // one widget to ImGui -- hence the ##.
                                        char it[160];
                                        std::snprintf(it, sizeof(it), "%s##pm%d_%d",
                                                      s.materials[i].name.c_str(),
                                                      pi, i);
                                        if (ImGui::Selectable(it, sel))
                                            lm->primMaterialId[pi] =
                                                s.materials[i].assetId;
                                        if (sel) ImGui::SetItemDefaultFocus();
                                    }
                                    ImGui::EndCombo();
                                }
                                ImGui::SameLine();
                                if (ImGui::SmallButton(btn) && pm >= 0) {
                                    s.matSel = pm; s.showMaterials = true;
                                }
                            }
                        }
                    }
                } else if (auto* col = dynamic_cast<CollectibleComponent*>(c)) {
                    // Points + radius from metadata; Sound is a picker
                    // over the Sound assets (chosen, not typed).
                    for (const Property& pr : col->props())
                        if (pr.key != "sound") drawProperty(pr, col);
                    s.soundPickerCombo("Sound", col->sound);
                } else if (auto* mp = dynamic_cast<MissilePickupComponent*>(c)) {
                    // Same deal as the Collectible: rounds, radius and
                    // respawn from metadata, the pickup cue chosen from
                    // the Sound assets rather than typed.
                    for (const Property& pr : mp->props())
                        if (pr.key != "sound") drawProperty(pr, mp);
                    s.soundPickerCombo("Sound", mp->sound);
                } else if (auto* pa = dynamic_cast<ParticleComponent*>(c)) {
                    // Everything from metadata except the sprite, which
                    // gets a Texture picker instead of a raw filename.
                    for (const Property& pr : pa->props())
                        if (pr.key != "sprite") drawProperty(pr, pa);
                    s.texturePickerCombo("Sprite", pa->sprite);
                    // Where the speed glow gets its speed from. Shown
                    // because the link is invisible otherwise: the
                    // craft is usually an ANCESTOR, and an emitter
                    // parented to the wrong thing would just never
                    // flare, with nothing on screen to say why.
                    if (pa->speedGlowMin != pa->speedGlowMax) {
                        float top = 0.0f;
                        std::string src;
                        if (ParticleSystem::speedSource(s.entities, be, top, src))
                            ImGui::TextDisabled("Speed from %s (top %.0f m/s)",
                                                src.c_str(), top);
                        else
                            ImGui::TextColored(
                                ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                                "No Glider or Opponent here or above -- "
                                "the speed glow does nothing.");
                    }
                    if (ImGui::Button("Restart burst")) {
                        s.particles.restart(be.id);
                        pa->playing = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%d live / %d emitters",
                                        s.particles.liveCount(),
                                        s.particles.emitterCount());
                } else if (auto* bp = dynamic_cast<BoostPadComponent*>(c)) {
                    // Speed/accel/direction from metadata; the punch SFX
                    // is a Sound picker (with volume/pitch already drawn
                    // as sliders above), plus a Preview to audition it.
                    for (const Property& pr : bp->props())
                        if (pr.key != "sound") drawProperty(pr, bp);
                    s.soundPickerCombo("Punch sound", bp->sound);
                    ImGui::BeginDisabled(bp->sound.empty());
                    if (ImGui::Button("Preview punch")) s.playBoostPunch(*bp);
                    ImGui::EndDisabled();
                } else if (auto* fl = dynamic_cast<FinishLineComponent*>(c)) {
                    // Laps + gate size from metadata; the three start
                    // samples are Sound pickers with one Preview each,
                    // so the sequence can be auditioned while placing it.
                    for (const Property& pr : fl->props())
                        if (pr.key != "soundReady" && pr.key != "soundSet" &&
                            pr.key != "soundGo")
                            drawProperty(pr, fl);
                    auto startCue = [&](const char* label, const char* btn,
                                        std::string& field) {
                        s.soundPickerCombo(label, field);
                        ImGui::BeginDisabled(field.empty());
                        if (ImGui::Button(btn)) s.playCue(field, fl->soundGain, 1.0f);
                        ImGui::EndDisabled();
                    };
                    startCue("Ready sound", "Preview##ready", fl->soundReady);
                    startCue("Set sound",   "Preview##set",   fl->soundSet);
                    startCue("Go sound",    "Preview##go",    fl->soundGo);
                    // Session type, the roster of who is in it, and the
                    // grid (see RaceGrid). Folded into THIS branch
                    // rather than added as another `else if` further
                    // down -- the chain stops at the first match, so a
                    // second branch for the same component type would
                    // never run.
                    racegrid::inspector(*fl, s.entities, s.roads.active());
                } else if (auto* cp = dynamic_cast<CheckpointComponent*>(c)) {
                    // Gate size from metadata; the pass SFX is a picker
                    // with volume/pitch sliders and a Preview, like the
                    // boost pad's punch.
                    for (const Property& pr : cp->props())
                        if (pr.key != "sound") drawProperty(pr, cp);
                    s.soundPickerCombo("Gate sound", cp->sound);
                    ImGui::BeginDisabled(cp->sound.empty());
                    if (ImGui::Button("Preview gate"))
                        s.playCue(cp->sound, cp->soundGain, cp->soundPitch);
                    ImGui::EndDisabled();
                } else if (auto* tr = dynamic_cast<TriggerComponent*>(c)) {
                    // Radius/once/message from metadata; Sound is a picker.
                    for (const Property& pr : tr->props())
                        if (pr.key != "sound") drawProperty(pr, tr);
                    s.soundPickerCombo("Sound", tr->sound);
                } else if (auto* stc = dynamic_cast<SceneTriggerComponent*>(c)) {
                    // Radius/once from metadata; Scene is a picker over the
                    // project's other scenes (chosen, not typed).
                    for (const Property& pr : stc->props())
                        if (pr.key != "scene") drawProperty(pr, stc);
                    const std::string folder =
                        std::filesystem::path(s.currentProject).parent_path().string();
                    const std::string label = stc->scene.empty() ? "(none)" : stc->scene;
                    if (ImGui::BeginCombo("Scene", label.c_str())) {
                        if (ImGui::Selectable("(none)", stc->scene.empty()))
                            stc->scene.clear();
                        for (const auto& [n, path] : s.listScenesIn(folder)) {
                            (void)path;
                            if (ImGui::Selectable(n.c_str(), stc->scene == n))
                                stc->scene = n;
                        }
                        ImGui::EndCombo();
                    }
                } else if (auto* sr = dynamic_cast<ShowroomComponent*>(c)) {
                    // Stage + camera from metadata; the three cues are
                    // Sound pickers with a Preview each, so the picker's
                    // feel can be tuned while it is being built.
                    for (const Property& pr : sr->props())
                        if (pr.key != "soundMove" && pr.key != "soundSelect" &&
                            pr.key != "soundStart")
                            drawProperty(pr, sr);
                    auto shCue = [&](const char* label, const char* btn,
                                     std::string& field) {
                        s.soundPickerCombo(label, field);
                        ImGui::BeginDisabled(field.empty());
                        if (ImGui::Button(btn)) s.playCue(field, sr->soundGain, 1.0f);
                        ImGui::EndDisabled();
                    };
                    shCue("Move sound",   "Preview##shmove", sr->soundMove);
                    shCue("Select sound", "Preview##shsel",  sr->soundSelect);
                    shCue("Start sound",  "Preview##shgo",   sr->soundStart);
                } else if (auto* tec = dynamic_cast<TrackEntryComponent*>(c)) {
                    // Everything from metadata except the scene, which is
                    // a picker over the project's scenes (chosen, not
                    // typed) -- same rule as the Scene Trigger's.
                    for (const Property& pr : tec->props())
                        if (pr.key != "scene") drawProperty(pr, tec);
                    const std::string tfolder =
                        std::filesystem::path(s.currentProject).parent_path().string();
                    const std::string tlabel =
                        tec->scene.empty() ? "(none)" : tec->scene;
                    if (ImGui::BeginCombo("Scene", tlabel.c_str())) {
                        if (ImGui::Selectable("(none)", tec->scene.empty()))
                            tec->scene.clear();
                        for (const auto& [n, path] : s.listScenesIn(tfolder)) {
                            (void)path;
                            if (ImGui::Selectable(n.c_str(), tec->scene == n))
                                tec->scene = n;
                        }
                        ImGui::EndCombo();
                    }
                } else if (auto* sw = dynamic_cast<SpawnerComponent*>(c)) {
                    // Prefab is a picker over the project's prefabs
                    // (chosen, not typed); picking one hides the
                    // primitive "Spawns" enum -- a prefab brings its
                    // own shape. Everything else from metadata.
                    const std::string pdir = s.currentProject.empty()
                        ? std::string()
                        : prefab::prefabsDirIn(
                              std::filesystem::path(s.currentProject)
                                  .parent_path().generic_string());
                    const auto prefabs = prefab::list(pdir);
                    const char* kNoPrefab = "(none - spawn a solid)";
                    const std::string plabel =
                        sw->prefab.empty() ? kNoPrefab : sw->prefab;
                    if (ImGui::BeginCombo("Prefab", plabel.c_str())) {
                        if (ImGui::Selectable(kNoPrefab, sw->prefab.empty()))
                            sw->prefab.clear();
                        for (const auto& [pn, ppath] : prefabs) {
                            (void)ppath;
                            if (ImGui::Selectable(pn.c_str(), sw->prefab == pn))
                                sw->prefab = pn;
                        }
                        ImGui::EndCombo();
                    }
                    // A renamed/deleted prefab would silently spawn
                    // nothing, so say so where it's authored.
                    if (!sw->prefab.empty() &&
                        std::none_of(prefabs.begin(), prefabs.end(),
                                     [&](const auto& np) {
                                         return np.first == sw->prefab;
                                     }))
                        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f),
                                           "Missing: %s", sw->prefab.c_str());
                    for (const Property& pr : sw->props()) {
                        if (pr.key == "prefab") continue;
                        if (pr.visible && !pr.visible(sw)) continue;
                        drawProperty(pr, sw);
                    }
                    if (!sw->prefab.empty())
                        ImGui::TextDisabled("Launch speed only moves the\n"
                                            "instance if its root has a\n"
                                            "dynamic Physics component.");
                } else if (auto* ts = dynamic_cast<TriggerSoundComponent*>(c)) {
                    // Radius/volume/loop/once from metadata; Sound picker.
                    for (const Property& pr : ts->props()) drawProperty(pr, ts);
                    s.soundPickerCombo("Sound", ts->sound);
                } else if (auto* as = dynamic_cast<AudioSourceComponent*>(c)) {
                    // Volume/loop/play-on-start/spatial from metadata;
                    // Sound is a picker over the project's Sound assets.
                    for (const Property& pr : as->props())
                        if (pr.key != "sound") drawProperty(pr, as);
                    s.soundPickerCombo("Sound", as->sound);
                    // Editor preview: audition the sound right here
                    // without entering Play (uses the same voice path).
                    ImGui::BeginDisabled(as->sound.empty());
                    if (ImGui::Button("Preview")) s.startAudioSource(be.id);
                    ImGui::SameLine();
                    if (ImGui::Button("Stop##audiosrc")) s.stopAudioSource(be.id);
                    ImGui::EndDisabled();
                } else if (auto* cam = dynamic_cast<CameraComponent*>(c)) {
                    // FOV from metadata; the Main Camera button marks this
                    // the view Play and the exported game start from,
                    // clearing the flag on every other camera. (The raw
                    // activeOnStart bool is hidden: set it here so exactly
                    // one camera can ever be the main one.)
                    for (const Property& pr : cam->props())
                        if (pr.key != "activeOnStart") drawProperty(pr, cam);
                    if (cam->activeOnStart) {
                        ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f),
                                           "* Main Camera");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Clear")) {
                            s.setMainCamera(-1); mainCamJustSet = true;
                        }
                    } else if (ImGui::Button("Set as Main Camera")) {
                        s.setMainCamera(be.id); mainCamJustSet = true;
                    }
                    // --- Multishot -------------------------------
                    // The two things the property metadata cannot
                    // carry: WHAT is being shot (an entity
                    // reference) and WHICH MOVES are in the rotation
                    // (a bool per shot, which as thirteen inspector
                    // rows would bury every number above it).
                    if (cam->mode == CameraComponent::Multishot) {
                        ImGui::Separator();
                        const Entity* subj = s.document.find(
                            cam->shotTarget >= 0 ? cam->shotTarget : be.parent);
                        const std::string slabel =
                            subj ? subj->name
                                 : (cam->shotTarget < 0 ? "(parent -- none)"
                                                        : "(missing)");
                        if (ImGui::BeginCombo("Subject", slabel.c_str())) {
                            if (ImGui::Selectable("(parent)", cam->shotTarget < 0))
                                cam->shotTarget = -1;
                            for (const Entity& se : s.entities) {
                                if (se.id == be.id) continue;   // not itself
                                if (ImGui::Selectable(se.name.c_str(),
                                                      cam->shotTarget == se.id))
                                    cam->shotTarget = se.id;
                            }
                            ImGui::EndCombo();
                        }
                        if (!subj)
                            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                                               "Pick an object to shoot.");

                        // What is on screen right now, and the speed
                        // the shot list is choosing against -- the two
                        // numbers that explain why it picked what it
                        // picked. Without them a rotation that leans
                        // on travelling shots looks like a bug rather
                        // than a parked car.
                        if (auto* dir = s.cams.director(be.id)) {
                            ImGui::Text("Now: %s  %.1fs left  (%.0f km/h)",
                                        multishot::shotName(dir->shot()),
                                        dir->remaining(), dir->speed() * 3.6f);
                            if (ImGui::Button("Cut")) dir->cut();
                            ImGui::SameLine();
                        } else {
                            ImGui::TextDisabled("Not running -- preview or play.");
                        }
                        // Preview drives the viewport from this camera
                        // without entering Play, which is the only way
                        // to judge a shot: framing is not a number, it
                        // is a picture.
                        if (s.activeCam == be.id) {
                            if (ImGui::Button("Stop preview")) s.activeCam = -1;
                        } else if (ImGui::Button("Preview")) {
                            s.activeCam = be.id;
                        }

                        if (ImGui::TreeNodeEx("Shots",
                                              ImGuiTreeNodeFlags_DefaultOpen)) {
                            if (ImGui::SmallButton("All"))
                                for (bool& u : cam->shots.use) u = true;
                            ImGui::SameLine();
                            if (ImGui::SmallButton("None"))
                                for (bool& u : cam->shots.use) u = false;
                            ImGui::SameLine();
                            ImGui::TextDisabled("(? per shot)");
                            for (int i = 0; i < multishot::ShotCount; ++i) {
                                ImGui::PushID(i);
                                ImGui::Checkbox(multishot::shotName(i),
                                                &cam->shots.use[i]);
                                if (ImGui::IsItemHovered())
                                    ImGui::SetTooltip("%s", multishot::shotHint(i));
                                // Audition one move now, instead of
                                // waiting for the rotation to offer it.
                                if (auto* dir = s.cams.director(be.id)) {
                                    ImGui::SameLine(ImGui::GetWindowWidth() - 40.0f);
                                    if (ImGui::SmallButton(">")) dir->play(i);
                                    if (ImGui::IsItemHovered())
                                        ImGui::SetTooltip("Play this shot now");
                                }
                                ImGui::PopID();
                            }
                            ImGui::TreePop();
                        }
                    }
                } else if (auto* cs = dynamic_cast<CameraSwitcherComponent*>(c)) {
                    // Radius from metadata; Target is a picker over the
                    // scene's Camera entities (plus the player view).
                    for (const Property& pr : cs->props()) drawProperty(pr, cs);
                    const Entity* cur = s.document.find(cs->target);
                    const std::string label = cur ? cur->name : "(Player view)";
                    if (ImGui::BeginCombo("Target", label.c_str())) {
                        if (ImGui::Selectable("(Player view)", cs->target < 0))
                            cs->target = -1;
                        for (const Entity& ce : s.entities)
                            if (ce.components.get<CameraComponent>())
                                if (ImGui::Selectable(ce.name.c_str(), cs->target == ce.id))
                                    cs->target = ce.id;
                        ImGui::EndCombo();
                    }
                } else if (auto* an = dynamic_cast<AnimationComponent*>(c)) {
                    // Speed/playing/loop from metadata; Clip is a picker
                    // over the model's animation clip names.
                    for (const Property& pr : an->props()) drawProperty(pr, an);
                    const auto* mc = be.components.get<ModelComponent>();
                    LoadedModel* lm = mc ? s.models.byId(mc->modelId) : nullptr;
                    if (lm && lm->animData && !lm->animData->animations.empty()) {
                        const auto& clips = lm->animData->animations;
                        an->clip = glm::clamp(an->clip, 0,
                                              static_cast<int>(clips.size()) - 1);
                        if (ImGui::BeginCombo("Clip", clips[an->clip].name.c_str())) {
                            for (int i = 0; i < static_cast<int>(clips.size()); ++i)
                                if (ImGui::Selectable(clips[i].name.c_str(), an->clip == i))
                                    { an->clip = i; an->time = 0.0f; }
                            ImGui::EndCombo();
                        }
                    } else {
                        ImGui::TextDisabled("No animated model on this entity.");
                    }
                } else if (auto* at = dynamic_cast<AnimationTriggerComponent*>(c)) {
                    // Radius/once from metadata; Target = a picker over
                    // the scene's entities that have an Animation.
                    for (const Property& pr : at->props()) drawProperty(pr, at);
                    const Entity* cur = s.document.find(at->target);
                    const std::string label = cur ? cur->name : "(none)";
                    if (ImGui::BeginCombo("Target", label.c_str())) {
                        if (ImGui::Selectable("(none)", at->target < 0))
                            at->target = -1;
                        for (const Entity& te : s.entities)
                            if (te.components.get<AnimationComponent>())
                                if (ImGui::Selectable(te.name.c_str(), at->target == te.id))
                                    at->target = te.id;
                        ImGui::EndCombo();
                    }
                } else if (auto* dop = dynamic_cast<DoorOpenerComponent*>(c)) {
                    // Radius/stayOpen from metadata; Target = a Door
                    // entity ((self) for the door this is attached to).
                    for (const Property& pr : dop->props()) drawProperty(pr, dop);
                    const Entity* cur = s.document.find(dop->target);
                    const std::string label = cur ? cur->name : "(self)";
                    if (ImGui::BeginCombo("Target door", label.c_str())) {
                        if (ImGui::Selectable("(self)", dop->target < 0))
                            dop->target = -1;
                        for (const Entity& te : s.entities)
                            if (te.components.get<DoorComponent>())
                                if (ImGui::Selectable(te.name.c_str(), dop->target == te.id))
                                    dop->target = te.id;
                        ImGui::EndCombo();
                    }
                } else if (auto* vh = dynamic_cast<VehicleComponent*>(c)) {
                    // Props + wheel-slot pickers + re-detect
                    // (see VehicleTool).
                    vehicleui::inspector(*vh, be, s.document);
                } else if (auto* gl = dynamic_cast<GliderComponent*>(c)) {
                    // Grouped flight tuning + drive hint (see GliderTool).
                    // The crash/alarm samples get the same Sound picker
                    // every other sound field in the editor uses.
                    gliderui::inspector(*gl, be, s.document,
                                        [&](const char* lbl, std::string& f) {
                                            s.soundPickerCombo(lbl, f);
                                        });
                } else {
                    for (const Property& pr : c->props()) drawProperty(pr, c);
                }
                    ImGui::Unindent();
                } // if (open): collapsed cards render just their header
                ImGui::PopID();
                ImGui::Spacing();        // gap between cards
                if (remove) {
                    be.components.items.erase(be.components.items.begin() + ci);
                    break;
                }
            }
            if (ImGui::Button("Add Component")) ImGui::OpenPopup("addcomp");
            if (ImGui::BeginPopup("addcomp")) {
                for (const components::TypeInfo& t : components::registry())
                    if (t.addable && ImGui::Selectable(t.displayName.c_str()))
                        be.components.items.push_back(t.make());
                ImGui::EndPopup();
            }
        }
        // Commit the inspector interaction as one undoable step. Begin
        // when a field is first touched, commit when nothing is active.
        // (Re-check selection: Delete##insp above may have cleared it.)
        if (s.sel.valid()) {
            const int  selId      = s.entities[s.sel.index()].id;
            const bool inspActive = ImGui::IsAnyItemActive();
            if (mainCamJustSet) {
                // setMainCamera() already pushed its own undo step (over
                // all cameras); don't let this wrapper log a duplicate.
                s.inspEditId = -1;
            } else if (inspActive && s.inspEditId != selId) {
                s.inspEditId     = selId;
                s.inspEditIds    = inspFrameIds;
                s.inspEditBefore = inspFrameStart;
            } else if (!inspActive && s.inspEditId == selId) {
                s.inspEditId = -1;
                auto cmd = std::make_unique<ModifyEntitiesCmd>(
                    s.inspEditBefore, s.snapshotEntities(s.inspEditIds));
                if (!cmd->trivial()) s.history.pushApplied(std::move(cmd));
            }
        }
    } else {
        ImGui::TextDisabled("Select an object in the Hierarchy or viewport.");
    }
    ui::sectionText("New block defaults");
    ImGui::SliderFloat3("Size", &s.entityNewHalf.x, 0.25f, 12.0f, "%.2f m");
    if (ImGui::Button("Materials...")) s.showMaterials = true;
    ImGui::SameLine();
    if (ImGui::Button("Models...")) s.showModels = true;
    ImGui::TextDisabled("Walk into blocks in FPS mode (F).");
    ImGui::End();
}

} // namespace inspectorui
