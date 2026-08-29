#pragma once

#include <functional>
#include <string>

// Crash recovery for the editor.
//
// Every few minutes the open scene is written to a snapshot outside the project
// -- scene, settings and the material library in one self-contained file (see
// projectio::saveSceneWithMaterials). The snapshot is deleted when the user saves
// and when the editor shuts down cleanly, so a snapshot that is still lying there
// at startup means exactly one thing: the last session ended without shutting
// down. That is the whole detection mechanism; there is no separate crash flag to
// get out of sync with the file it describes.
//
// It deliberately does NOT touch the project folder. Autosave must never turn
// into "the editor saved for me": unsaved is a state the user chose, and quitting
// without saving has to still mean the project on disk is unchanged. The snapshot
// is an offer made after a crash, not a save.
namespace autosave {

// A snapshot left behind by a session that never shut down. Read from the small
// sidecar written beside it, so this stays instant even when the snapshot itself
// is a large scene.
struct Snapshot {
    std::string file;          // the snapshot scene file ("" = nothing pending)
    std::string scenePath;     // the scene it stands in for
    std::string projectFolder; // the project that scene belongs to
    std::string projectName;   // display only
    std::string sceneName;     // display only
    std::string writtenAt;     // local time, "2026-08-28 14:32"
    std::string app;           // the build that wrote it
    int         ageMinutes = 0;// how long ago, at the moment it was read

    bool valid() const { return !file.empty(); }
};

// The snapshot pending in `dir`, or an empty one. Also returns empty when the
// snapshot's project or scene has meanwhile disappeared -- there would be nothing
// to restore into.
Snapshot pending(const std::string& dir);

// Delete whatever is in `dir`. Safe to call when there is nothing.
void discard(const std::string& dir);

// The periodic writer. One per editor.
class Autosave {
public:
    explicit Autosave(std::string dir = "recovery") : m_dir(std::move(dir)) {}

    void  setEnabled(bool on)          { m_enabled = on; }
    bool  enabled() const              { return m_enabled; }
    void  setIntervalMinutes(float m)  { m_interval = m > 0.25f ? m : 0.25f; }
    float intervalMinutes() const      { return m_interval; }
    const std::string& dir() const     { return m_dir; }

    // Call once per editor frame.
    //
    // `now` is the app clock in seconds. `editable` is false whenever the
    // document is not a faithful picture of the user's work -- play mode (where
    // gameplay mutates entities that a Stop will roll back) and a scene load in
    // flight (where the entity list is half-built). `scenePath` is the open scene
    // ("" = no project, nothing to snapshot). `revision` is the document's edit
    // counter (CommandStack::revision).
    //
    // `write` serializes the scene to the path it is handed and returns whether
    // that worked; it is only ever called with a temporary path, which is then
    // moved into place, so a crash DURING a snapshot cannot leave a truncated one
    // behind to be offered at the next start.
    void tick(double now, bool editable, const std::string& scenePath,
              unsigned revision,
              const std::function<bool(const std::string&)>& write);

    // The user saved, or closed the project: what is on disk is now at least as
    // new as the snapshot, so the snapshot has nothing left to offer.
    void clear();

    // Note an edit that does not pass through the undo history. Not required --
    // the periodic fallback in tick() catches those anyway, just later.
    void touch() { m_touched = true; }

    // "Autosave 14:32" for the File menu, or "" before the first snapshot. Just
    // enough for the user to see the thing is alive, which is most of its value.
    const std::string& status() const { return m_status; }

private:
    std::string m_dir;
    bool        m_enabled  = true;
    float       m_interval = 3.0f;   // minutes between snapshots
    double      m_nextDue  = -1.0;   // app time of the next attempt (-1 = unset)
    double      m_lastWrite = -1.0;  // app time of the last snapshot written
    unsigned    m_lastRev  = 0;
    bool        m_haveRev  = false;
    bool        m_touched  = false;
    std::string m_status;
};

} // namespace autosave

#ifndef FITZEL_PLAYER
namespace autosave {

// What the user chose in the recovery dialog. `None` while it is still open.
enum class Choice { None, Restore, Discard };

// The startup dialog offering `s` back. Call it every frame while a snapshot is
// pending (it opens the popup itself on the first call) and act on the answer.
Choice drawRecoveryModal(const Snapshot& s);

} // namespace autosave
#endif
