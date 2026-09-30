#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "TreeGen.hpp"
#include "TreePreview.hpp"

// The editor's "Tree generator" window (Planting menu): pick a species, turn a
// few botanical dials, watch the tree regrow in its own studio, save it as a
// .glb in the project's trees/ folder -- and, in the same click, make it a
// Vegetation species, so the forest is planted with it.
//
// Every control is a click, never a drag: presets are tiles, numbers are
// steppers, the studio turns by buttons (see parkinson-tauglicher Editor). A
// seed button rolls a new tree of the same kind.
//
// Owns its state and its actions, so main only constructs it and calls panel().
namespace treeui {

class TreeGenTool {
public:
    struct Deps {
        const std::string& currentProject;   // the open .fitzel ("" = none)
        std::string&       status;           // the editor's footer line
        // Make a just-saved tree (file name in the project) a Vegetation
        // species `height` metres tall -- VegetationSystem::adoptTreeModel.
        std::function<void(const std::string& file, const std::string& name, float height)> plant;
        // A tree was written into the project (every save): the Vegetation
        // pickers re-read their file lists (VegetationSystem::rescanTreeFiles).
        std::function<void()> saved;
    };
    explicit TreeGenTool(Deps d);

    void panel(bool& show);

private:
    struct Img { std::string name, path; };
    // One remembered texture choice (and its atlas grid) per kind of foliage,
    // so switching between an oak and a spruce does not lose either.
    struct LeafPick { std::string path; int cols = 1, rows = 1; };

    void applyPreset(int i);
    void regenerate();
    void reloadTextures();
    void scanImages();
    std::string projectDir() const;
    std::string treesDir() const;
    std::vector<std::string> savedTrees() const;
    void save(bool asSpecies);
    void open(const std::string& file);
    // A searchable pick from the image list. `emptyName` is what "" means for
    // this slot (the built-in texture, or no map at all).
    bool texturePicker(const char* id, const char* label, std::string& path,
                       const char* emptyName, const char* emptyTip);
    // The normal map that sits beside a colour map by the common naming
    // schemes (Bark001_Color -> Bark001_NormalGL, x_diff_4k -> x_nor_gl_4k,
    // x_Diffuse -> x_Normal ...), or "". Sets `dx` for a DirectX one.
    static std::string suggestNormal(const std::string& colourPath, bool& dx);
    void levelSection(int l, const char* title);

    Deps              m_d;
    treegen::Params   m_p;
    treegen::Mesh     m_mesh;
    TreePreview       m_studio;
    TreePreview::View m_view;
    bool   m_ready = false, m_studioOk = false;
    bool   m_dirty = true, m_texDirty = true, m_redraw = true;
    bool   m_turntable = false;
    std::uint32_t m_tex = 0;             // the studio picture (GL texture)
    double m_genMs = 0.0;
    int    m_preset = 0;
    std::string m_barkPath;              // "" = built-in
    std::string m_barkNormal;            // "" = no relief
    bool        m_barkNormalDX = false;  // green down: flipped for glTF on load/export
    LeafPick    m_broad, m_needle;       // "" path = built-in
    std::vector<Img> m_images;           // pickable textures (content + project)
    std::string m_scannedFor = "\x01";   // project the list was built for
    char   m_search[64] = {};
    char   m_name[64]   = {};
    std::string m_lastSaved;
};

} // namespace treeui