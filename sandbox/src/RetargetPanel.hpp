#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include <fitzel/world/Model.hpp>

#include "Retarget.hpp"
#include "RetargetPreview.hpp"

// The editor's "Retarget animations" window (Assets menu): give a character the
// motions of another skeleton.
//
//   * pick the character (a rigged .glb in the project, or the selected one),
//   * add motions -- a file dialog, files dropped on the window, a click in
//     the motion library (a folder of FBX/BVH/glb files, auditioned by a click
//     before they are added), or ticked in the list of another character's
//     clips (Characters tab: copy what one character has to another),
//   * watch them side by side in the studio: the motion's own figure on the
//     left, the character playing it on the right, bones drawn over both,
//   * fix a bone the automatic map got wrong (one dropdown per body part),
//     trim a clip, take its travel out,
//   * write them all into the character's .glb in one click.
//
// FBX, BVH and .blend files go through Blender in the background (Blender.hpp),
// glTF is read directly. What a character's clips are made of is kept in a
// recipe next to the model (Retarget.hpp), so the window opens where it was
// left and every clip can be redone.
//
// Every control is a click, never a drag (see parkinson-tauglicher Editor):
// the studio turns by buttons, times are steppers, bones are picked from lists.
// Owns its state and its actions, so main only constructs it and calls panel().
namespace retargetui {

class RetargetTool {
public:
    struct Deps {
        const std::string&        currentProject;   // the open .fitzel ("" = none)
        std::string&              status;           // the editor's footer line
        std::vector<std::string>* drops = nullptr;  // files dropped on the editor this frame
        const float*              dropX = nullptr;  // ...and where (screen pixels)
        const float*              dropY = nullptr;
        std::function<std::string()> selectedModel; // the selected entity's model file ("" = none)
    };
    explicit RetargetTool(Deps d);
    ~RetargetTool();
    RetargetTool(const RetargetTool&)            = delete;
    RetargetTool& operator=(const RetargetTool&) = delete;

    void panel(bool& show);
    // Show this character (an absolute path to its .glb).
    void openCharacter(const std::string& model);
    // Add a motion file to the character, as a drop or the + button would --
    // `take` picks one animation of a file that has several ("" = the main one).
    void addMotion(const std::string& file, const std::string& take = {});
    // Add several animations of one file (another character's clips) at once.
    void addTakes(const std::string& file, const std::vector<std::string>& takes);
    // The file the Characters tab copies from (for retargetpanelcheck).
    void copyFrom(const std::string& file);
    // Hold clip `clip` (-1: keep) at `seconds` and bring tab `tab` (0 clip,
    // 1 bones, 2 character; -1: keep) to the front -- for retargetpanelcheck.
    void showAt(int clip, float seconds, int tab);
    // Nothing loading any more (character and motions).
    bool idle() const;
    // The window's size the first time it opens.
    void setFirstSize(float w, float h) { m_firstW = w; m_firstH = h; }
    // Turn the figures on the floor (degrees; 0 = facing the camera).
    void setTurn(float deg) { m_turn = deg; m_redraw = true; }

    // What a loader thread hands back.
    struct Loaded {
        std::string                        error, note;
        std::shared_ptr<retarget::Rig>     rig;
        std::shared_ptr<fitzel::ModelData> model;    // for the studio (textures shrunk)
        retarget::Recipe                   recipe;   // characters: their recipe
        std::string                        base;     // characters: the file bakes start from
        retarget::ModelInfo                info;     // characters: what their file holds
    };

private:
    // A loader thread's mailbox. The thread is detached and holds its own
    // reference, so closing the window (or the editor) never waits on a
    // Blender that is still converting.
    struct Job {
        std::mutex lock;
        bool       done = false;
        Loaded     result;
    };
    static std::shared_ptr<Job> start(std::function<Loaded()> work);
    static bool take(const std::shared_ptr<Job>& job, Loaded& out);

    struct Source {
        std::string          file;
        bool                 loading = true;
        std::shared_ptr<Job> job;
        Loaded               data;
    };
    struct Character {
        std::string          path;
        bool                 loading = false;
        std::shared_ptr<Job> job;
        Loaded               data;
        retarget::BoneMap   autoMap = retarget::emptyMap();
        retarget::BoneMap   map = retarget::emptyMap();
        std::vector<std::string> fileClips;   // what the .glb holds right now
    };

    void poll();
    void drawTopBar();
    void drawClipList(float height);
    void drawLibrary();
    void drawCharacters();
    void drawStudio(float width, float height);
    void drawTransport();
    void drawClipTab();
    void drawBonesTab();
    void drawCharacterTab();
    void drawWriteBar();
    bool boneCombo(const char* id, const retarget::Rig& rig, int& node, bool source);

    void scanModels();
    void scanLibrary();
    void selectClip(int i);
    void audition(const std::string& file, const std::string& take = {});
    Source* sourceFor(const std::string& file);   // starts loading it if new
    retarget::ClipEntry* current();               // the selected clip, or the audition
    std::string currentFile();                    // its motion file (absolute)
    void recipeChanged();
    void mapsChanged() { m_txDirty = true; m_redraw = true; }
    void rebuildTransfer();
    void updatePose();
    bool clipSpan(float& len, float& fps);        // the shown clip's length and rate
    void write();
    std::string projectDir() const;

    Deps                     m_d;
    Character                m_char;
    std::map<std::string, std::unique_ptr<Source>> m_sources;   // by absolute file
    int                      m_sel = -1;           // recipe clip shown (-1: none / audition)
    std::string              m_audition;           // a library file shown before it is added
    retarget::ClipEntry      m_auditionEntry;
    bool                     m_pending = false;    // recipe edited since the last write
    bool                     m_recipeDirty = false;

    // The studio.
    RetargetPreview          m_studio;
    bool                     m_studioReady = false, m_studioOk = false;
    std::uint32_t            m_tex = 0;
    bool                     m_redraw = true;
    const fitzel::ModelData* m_shown[2] = {nullptr, nullptr};
    float                    m_time = 0.0f;        // seconds into the clip
    bool                     m_playing = true;
    float                    m_speed = 1.0f;
    float                    m_turn = 20.0f;       // the figures' turn on the floor (deg)
    float                    m_zoom = 1.0f;
    bool                     m_bones = true;
    bool                     m_hold = true;        // keep the figures over their spots
    int                      m_hot = -1;           // slot under the mouse in the bones tab
    retarget::Transfer       m_tx;
    retarget::BoneMap        m_srcMap = retarget::emptyMap();
    bool                     m_txDirty = true;
    std::string              m_txFor;              // the source file m_tx was built for

    // Lists.
    struct ModelEntry { std::string path, name; };
    std::vector<ModelEntry>  m_models;
    std::string              m_modelsFor = "\x01";
    std::string              m_library;            // motion library folder
    std::vector<std::string> m_libraryFiles;
    char                     m_libSearch[64] = {};
    std::string              m_copyFrom;           // the character the Characters tab copies from
    std::vector<std::string> m_copyPick;           // ...the animations ticked there
    bool                     m_showCharacters = false;   // bring the Characters tab to the front
    char                     m_boneSearch[64] = {};
    char                     m_nameBuf[64] = {};
    int                      m_tabRequest = -1;    // a tab to bring to the front next frame
    float                    m_firstW = 1200.0f, m_firstH = 860.0f;
    std::string              m_message;            // the last write / problem, under the button
    bool                     m_messageBad = false;
};

} // namespace retargetui