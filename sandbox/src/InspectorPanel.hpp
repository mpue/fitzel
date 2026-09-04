#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "AnimSystem.hpp"  // the clip a property is keyed into
#include "Component.hpp"   // ScriptParam
#include "Selection.hpp"
#include "SceneTypes.hpp"  // Entity, MaterialDef, LoadedModel

class BoostPadComponent;
class CommandStack;
class Document;
class ModelLibrary;
class ParticleSystem;
class RoadSet;
class ScriptSystem;
namespace camerasys { class CameraSystem; }
namespace fitzel { class TerrainStreamer; }

// The editor's "Inspector" panel: everything about the one selected entity --
// its metadata-driven fields (PropertyMeta.hpp), then its components as
// collapsible cards, then the defaults a new block is created with.
//
// Most component cards render straight from their own property metadata; the
// ones listed by hand in the .cpp are the fields metadata cannot carry -- an
// entity reference, a pick from the project's assets, a preview button.
//
// Editor-only, and it has no `show` flag: Inspector is always open, like
// Hierarchy.
namespace inspectorui {

// A script's exported parameters (its module-level globals), as scanned from the
// .lua on disk. main caches one of these per file and re-scans when the file
// changes; the Script card renders `defs` as editable fields. `ok`/`err` report a
// parse/run failure -- the fields are then unknown, not empty-by-choice.
struct ScriptParamScan {
    std::filesystem::file_time_type mtime{};
    std::vector<ScriptParam>        defs;
    bool                            ok = false;
    std::string                     err;
};

// What the panel touches in main. References rather than a back-pointer to the
// editor, so the panel can be read on its own and can't quietly reach for more
// than this list -- the same shape roadui::PanelState uses. It is a long list
// because the inspector is where every component type surfaces; each entry is
// one card's bespoke field, not a new coupling.
struct PanelState {
    std::vector<Entity>&      entities;
    Document&                 document;
    CommandStack&             history;
    std::vector<MaterialDef>& materials;
    const Selection&          sel;

    // --- Subsystems a card reads or drives ----------------------------------
    ModelLibrary&             models;      // Model + Animation cards
    ParticleSystem&           particles;   // the Particle card's live counters
    ScriptSystem&             scripts;     // lastError(), shown on the Script card
    camerasys::CameraSystem&  cams;        // the Multishot director's readout
    RoadSet&                  roads;       // the race grid is laid out along one
    fitzel::TerrainStreamer&  streamer;    // "Drop to ground"

    // --- The Sun's two scene-wide fields ------------------------------------
    // The sun is not an ordinary entity: its transform IS the time of day, so
    // its inspector edits those instead of a position.
    float& timeOfDay;
    bool&  timePaused;

    // --- Transform helpers (main owns the scene graph) -----------------------
    std::function<glm::mat4(const Entity&)> parentWorldMat;
    std::function<void(Entity&, const glm::vec3&, const glm::vec3&,
                       const glm::mat4*)>   setWorld;
    std::function<void(Entity&, const glm::mat4*)>          rebaseLocal;
    std::function<glm::vec3(const LoadedModel&, float)>     modelHalf;
    std::function<void(int)>                                deleteEntity;
    std::function<void(int)>                                setMainCamera;

    // --- Undo: the inspector commits one interaction as one step -------------
    // The snapshot pair lives in main because it has to survive between frames
    // (a drag is many frames of one edit).
    std::function<std::vector<int>(int)>                       collectSubtreeIds;
    std::function<std::vector<Entity>(const std::vector<int>&)> snapshotEntities;
    int&                 inspEditId;
    std::vector<int>&    inspEditIds;
    std::vector<Entity>& inspEditBefore;

    // --- Asset pickers (they enumerate project state, not metadata) ----------
    std::function<void(const char*, std::string&)> soundPickerCombo;
    std::function<void(const char*, std::string&)> texturePickerCombo;
    std::function<std::vector<std::string>()>      listScripts;
    std::function<void(const std::string&)>        openScript;
    std::function<const ScriptParamScan&(const std::string&)> scanScriptParams;
    // The project's other scenes, for the Scene Trigger / Track Entry pickers.
    const std::string&                             currentProject;
    std::function<std::vector<std::pair<std::string, std::string>>(
        const std::string&)>                       listScenesIn;

    // --- Auditioning a sound where it is authored ---------------------------
    std::function<void(const std::string&, float, float)> playCue;
    std::function<void(const BoostPadComponent&)>         playBoostPunch;
    std::function<void(int)>                              startAudioSource;
    std::function<void(int)>                              stopAudioSource;

    // --- Cross-panel handoffs ------------------------------------------------
    // "Edit" next to a material picker jumps to the Materials panel with that
    // row selected; the two buttons at the bottom open the sibling panels.
    int&        matSel;
    char*       matPickFilter;   // one shared buffer: one popup open at a time
    std::size_t matPickFilterCap;
    bool&       showMaterials;
    bool&       showModels;

    // Multishot's viewport preview: drive the view from this camera without
    // entering Play. main owns which camera the viewport follows.
    int&        activeCam;

    // New-block defaults, edited at the bottom of the panel.
    glm::vec3&  entityNewHalf;

    // --- Keyframing ----------------------------------------------------------
    // Every field the panel draws from property metadata gets a key diamond
    // beside it, because the Inspector is WHERE a property is set: making the
    // author find the same field again in another panel to record the value they
    // just dialled in is the step that stops people animating anything. The
    // panel needs the clip to write into, the playhead to write at, and whether
    // an ordinary edit should record itself.
    std::vector<anim::Clip>& clips;    // the scene's animations (never empty)
    int&          editClip;            // the one the Timeline is on
    anim::Player& animPlayer;
    bool&         autoKey;
};

void drawPanel(const PanelState& s);

} // namespace inspectorui
