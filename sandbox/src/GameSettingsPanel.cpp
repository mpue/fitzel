#include "GameSettingsPanel.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include <imgui.h>

#include "FolderDialog.hpp"
#include "InstallerBuild.hpp"
#include "UiStyle.hpp"

namespace game {

namespace {

bool listed(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Copy a picked image into the project (the export bundles the project folder
// whole) and return the bare file name to store in the settings. Empty if the
// copy failed.
std::string adoptImage(const std::string& picked, const std::string& projectFolder) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string base = fs::path(picked).filename().string();
    const fs::path    dst  = fs::path(projectFolder) / base;
    if (fs::weakly_canonical(picked, ec) != fs::weakly_canonical(dst, ec))
        fs::copy_file(picked, dst, fs::copy_options::overwrite_existing, ec);
    return ec ? std::string{} : base;
}

// The loading screen section, preview first.
//
// The preview is drawn by the SAME painter the game uses, into a box instead of
// a window -- so what is being configured is the thing itself, not a description
// of it. That matters more here than anywhere else in this dialog: the screen it
// configures is one nobody can see while they are making it, since by the time
// it shows up the editor is gone.
void drawLoadingSection(game::Settings& s, const std::string& projectFolder) {
    if (!ImGui::CollapsingHeader("Loading screen", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    // Lives across frames: it owns the decoded background, and re-decoding a 4K
    // image every frame would turn this dialog into a slideshow.
    static loadingscreen::Screen preview;
    static float                 demo = 0.0f;
    demo += ImGui::GetIO().DeltaTime * 0.35f;
    if (demo > 1.15f) demo = 0.0f;

    preview.setProjectFolder(projectFolder);
    preview.setStyle(s.loading);

    const float  w  = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 sz(w, w * 9.0f / 16.0f);
    preview.drawInto(ImGui::GetWindowDrawList(), p0, sz, std::min(demo, 1.0f),
                     "Building entities...");
    ImGui::GetWindowDrawList()->AddRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y),
                                        ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::Dummy(sz);
    ui::hint("Live preview -- the same drawing the game does, at 16:9.");
    ImGui::Spacing();

    loadingscreen::Style& L = s.loading;

    // --- Background ----------------------------------------------------------
    ui::sectionText("Background");
    ImGui::TextUnformatted(L.background.empty() ? "(the splash image)"
                                                : L.background.c_str());
    if (ImGui::Button("Browse...##loadbg")) {
        std::string picked;
        if (ed::pickFile(picked, projectFolder, "Images",
                         "*.png;*.jpg;*.jpeg;*.bmp;*.tga")) {
            const std::string base = adoptImage(picked, projectFolder);
            if (!base.empty()) L.background = base;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Use splash##loadbg")) L.background.clear();
    ui::hint("Blank uses the splash image, so one picture can do both jobs.");

    int fit = static_cast<int>(L.fit);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##fit", &fit, "Fit inside\0Fill the screen\0Stretch\0"))
        L.fit = static_cast<loadingscreen::Fit>(fit);
    ui::hint("Fit inside letterboxes and never enlarges past the image's own\n"
             "size; Fill crops the overflow; Stretch ignores the aspect.");
    ImGui::ColorEdit3("Frame colour", &L.back.x, ImGuiColorEditFlags_NoInputs);
    ImGui::Spacing();

    // --- Headline ------------------------------------------------------------
    ui::sectionText("Headline");
    // The text buffer is seeded from the setting whenever the two have drifted
    // apart and nothing is being typed -- not just on the frame the modal opens.
    // With this section collapsed on that frame, an "opening" seed never happens,
    // and an unseeded buffer would write its stale text back over the setting.
    static char titleBuf[96];
    if (!ImGui::IsAnyItemActive() && L.title != titleBuf)
        std::snprintf(titleBuf, sizeof titleBuf, "%s", L.title.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##loadTitle", "(no headline)", titleBuf,
                             sizeof titleBuf);
    L.title = titleBuf;
    ImGui::SliderFloat("Size##title", &L.titleScale, 1.0f, 6.0f, "%.1fx");
    ImGui::ColorEdit3("Text colour", &L.textColor.x, ImGuiColorEditFlags_NoInputs);
    ImGui::Spacing();

    // --- Progress bar --------------------------------------------------------
    ui::sectionText("Progress bar");
    int at = static_cast<int>(L.barAt);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##barAt", &at, "Bottom\0Middle\0Top\0"))
        L.barAt = static_cast<loadingscreen::BarAt>(at);
    ImGui::SliderFloat("Distance##bar", &L.barInset, 0.0f, 240.0f, "%.0f px");
    // Percent as an int: a slider showing "0.55" for "just over half the
    // screen" is a number nobody is thinking in.
    int widthPct = static_cast<int>(L.barWidth * 100.0f + 0.5f);
    if (ImGui::SliderInt("Width##bar", &widthPct, 15, 100, "%d%%"))
        L.barWidth = widthPct / 100.0f;
    ImGui::SliderFloat("Height##bar", &L.barHeight, 4.0f, 48.0f, "%.0f px");
    ImGui::SliderFloat("Rounding##bar", &L.barRound, 0.0f, 24.0f, "%.0f px");
    ImGui::ColorEdit3("Fill##bar", &L.barColor.x, ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    ImGui::ColorEdit3("Track##bar", &L.barBack.x, ImGuiColorEditFlags_NoInputs);
    ImGui::Spacing();

    // --- What else is written ------------------------------------------------
    ui::sectionText("Show");
    ImGui::Checkbox("Step name", &L.showLabel);
    ImGui::SameLine();
    ImGui::Checkbox("Percent", &L.showPercent);
    ImGui::SameLine();
    ImGui::Checkbox("Version", &L.showVersion);
    ui::hint("The step name is what the loader is doing right now; the version\n"
             "is small and dim, and worth its space on a bug report screenshot.");
    ImGui::Spacing();
}

// The installer section: what gets built next to the export, and who it says the
// game is from.
//
// Whether an Inno Setup compiler exists is checked HERE, while the switch is
// being flipped -- not at export time. Exporting a real game runs for minutes,
// and "you needed a tool you do not have" is not an answer anybody should have
// to wait that long to receive.
void drawInstallerSection(game::Settings& s) {
    if (!ImGui::CollapsingHeader("Installer"))
        return;

    // Finding the compiler walks the registry and the disk, so the answer is
    // taken once and kept -- this runs on every frame the section is open.
    static const bool        haveIscc = installer::available();
    static const std::string isccPath = installer::compilerPath();

    ImGui::Checkbox("Build a setup.exe", &s.makeInstaller);
    ui::hint("Compiles everything in the export folder into one installer that\n"
             "copies the game into place and adds it to the Start menu. Slow --\n"
             "it compresses the whole game -- so leave it off while testing.");

    if (!haveIscc) {
        ImGui::TextDisabled("Inno Setup was not found on this machine.");
        ui::hint("The installer is compiled by Inno Setup (free, innosetup.com).\n"
                 "Without it the export still works; only the setup is skipped.");
    } else if (s.makeInstaller) {
        ImGui::TextDisabled("Using %s", isccPath.c_str());
    }

    if (!s.makeInstaller) { ImGui::Spacing(); return; }

    // Buffers, like the exe name above: re-seeded whenever they have drifted
    // from the settings and nothing is being typed, so a section that was
    // collapsed on the frame the modal opened cannot later write its stale text
    // back over saved values.
    static char product[96], version[32], publisher[96];
    if (!ImGui::IsAnyItemActive()) {
        if (s.productName != product)
            std::snprintf(product, sizeof product, "%s", s.productName.c_str());
        if (s.version != version)
            std::snprintf(version, sizeof version, "%s", s.version.c_str());
        if (s.publisher != publisher)
            std::snprintf(publisher, sizeof publisher, "%s", s.publisher.c_str());
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##product", "(the executable name)", product, sizeof product);
    s.productName = product;
    ui::hint("The name in the setup wizard, in the Start menu and in the list of\n"
             "installed programs. Blank uses the executable name.");

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##version", "1.0.0", version, sizeof version);
    s.version = version;
    ui::hint("Shown next to the game in the installed-programs list. Raising it\n"
             "makes the next setup replace this one instead of installing a\n"
             "second copy beside it.");

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##publisher", "(publisher, optional)", publisher,
                             sizeof publisher);
    s.publisher = publisher;

    ImGui::Spacing();
}

} // namespace

bool drawSettingsModal(const char* popupId, Settings& s,
                       const std::vector<std::string>& scenes,
                       const std::string& projectFolder) {
    namespace fs = std::filesystem;
    bool applied = false;

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(720.0f, 900.0f));
    // What the settings looked like when the dialog opened, so a dismissal can
    // put them back. Without this, Cancel (and Esc, and a click outside) keeps
    // every edit in memory and merely skips the write -- which leaves the dialog
    // and game.json disagreeing, and the export reads the FILE. That is a
    // setting you watched yourself make and that never reached the exe.
    static Settings    opened;
    static bool        wasOpen = false;
    static bool        saved   = false;
    static std::string iconErr;

    if (!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (wasOpen && !saved) s = opened;
        wasOpen = false;
        saved   = false;
        return false;
    }

    // The single free-text field (exe name) is mirrored into a char buffer, synced
    // from `s` on the frame the modal opens so re-opening always shows saved state.
    static char exeBuf[64];
    if (ImGui::IsWindowAppearing()) {
        std::snprintf(exeBuf, sizeof exeBuf, "%s", s.exeName.c_str());
        opened = s;
        iconErr.clear();
    }
    wasOpen = true;

    // --- Executable name -----------------------------------------------------
    ui::sectionText("Executable");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##exeName", "game (project name)", exeBuf, sizeof exeBuf);
    s.exeName = exeBuf;
    ui::hint("Base name of the exported .exe. Blank uses the project name.");
    ImGui::Spacing();

    // --- Splash image --------------------------------------------------------
    ui::sectionText("Splash screen");
    ImGui::TextUnformatted(s.splash.empty() ? "(engine default)" : s.splash.c_str());
    if (ImGui::Button("Browse...")) {
        std::string picked;
        if (ed::pickFile(picked, projectFolder, "Images",
                         "*.png;*.jpg;*.jpeg;*.bmp;*.tga")) {
            // Copy the chosen image into the project so the export bundles it; store
            // just the file name (project-relative) in the settings.
            std::error_code ec;
            const std::string base = fs::path(picked).filename().string();
            const fs::path dst = fs::path(projectFolder) / base;
            if (fs::weakly_canonical(picked, ec) != fs::weakly_canonical(dst, ec))
                fs::copy_file(picked, dst, fs::copy_options::overwrite_existing, ec);
            if (!ec) s.splash = base;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Use default")) s.splash.clear();
    ui::hint("Shown while the game loads. PNG/JPG; blank uses the engine splash.");
    ImGui::Spacing();

    // --- Application icon ----------------------------------------------------
    ui::sectionText("Icon");
    const ImVec4 kWarn(1.0f, 0.45f, 0.35f, 1.0f);
    if (s.icon.empty()) {
        ImGui::TextUnformatted("(engine default)");
    } else {
        ImGui::TextUnformatted(s.icon.c_str());
        // Whether the picture is actually THERE, not merely named. A settings
        // file pointing at a file that has since been renamed or deleted looks
        // exactly like a configured icon here, and today only the export finds
        // out -- one dialog too late to do anything about it.
        std::error_code iec;
        if (!fs::exists(fs::path(projectFolder) / s.icon, iec)) {
            ImGui::SameLine();
            ImGui::TextColored(kWarn, "-- file missing from the project!");
        }
    }
    if (ImGui::Button("Browse...##icon")) {
        iconErr.clear();
        std::string picked;
        if (ed::pickFile(picked, projectFolder, "Images",
                         "*.png;*.jpg;*.jpeg;*.bmp;*.tga")) {
            const std::string base = adoptImage(picked, projectFolder);
            // A failed copy used to leave the field exactly as it was, which
            // reads as "the click did nothing" -- and the next thing to mention
            // it at all is an exported exe wearing the Windows default.
            if (!base.empty()) s.icon = base;
            else iconErr = "could not copy that file into the project folder";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Use default##icon")) { s.icon.clear(); iconErr.clear(); }
    if (!iconErr.empty()) ImGui::TextColored(kWarn, "%s", iconErr.c_str());
    ui::hint("A square PNG, ideally 256x256 or bigger. The export scales it to\n"
             "every size Windows asks for and writes it into the .exe itself --\n"
             "there is no .ico to make, and the setup gets the same picture.\n"
             "It only reaches an export once this dialog is closed with Save.");
    ImGui::Spacing();

    drawLoadingSection(s, projectFolder);

    // --- Start scene ---------------------------------------------------------
    ui::sectionText("Start scene");
    const char* startLabel =
        s.startScene.empty() ? "(default scene)" : s.startScene.c_str();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##startScene", startLabel)) {
        if (ImGui::Selectable("(default scene)", s.startScene.empty()))
            s.startScene.clear();
        for (const std::string& stem : scenes)
            if (ImGui::Selectable(stem.c_str(), s.startScene == stem))
                s.startScene = stem;
        ImGui::EndCombo();
    }
    ui::hint("Scene the game boots into. Default is the project's main scene.");
    ImGui::Spacing();

    // --- Start mode ----------------------------------------------------------
    // Directly under the start scene, because the two are one decision: WHICH
    // level opens, and what the player is standing in when it does.
    ui::sectionText("Start as");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##startMode", startModeName(s.startMode))) {
        for (int i = 0; i <= static_cast<int>(StartMode::Multishot); ++i) {
            const StartMode m = static_cast<StartMode>(i);
            if (ImGui::Selectable(startModeName(m), s.startMode == m))
                s.startMode = m;
        }
        ImGui::EndCombo();
    }
    ui::hint("What the player is on the first frame.\n"
             "On foot spawns the walking player at the Player Start.\n"
             "Behind the wheel / Flying take over the scene's vehicle or glider.\n"
             "The two watching modes hand the frame to a camera instead of to a\n"
             "player -- an attract screen or a title card. Main Camera uses the\n"
             "one marked as such; Multishot uses the first multishot camera in\n"
             "the scene, which is the one that cuts its own shots.\n"
             "A mode the scene cannot provide falls back to on foot.");
    ImGui::Spacing();

    // --- Export scenes -------------------------------------------------------
    ui::sectionText("Scenes to export");
    bool all = s.exportScenes.empty();
    if (ImGui::Checkbox("All scenes", &all)) {
        if (all) s.exportScenes.clear();
        else     s.exportScenes = scenes; // switch to an explicit list, all selected
    }
    if (!all) {
        ImGui::Indent();
        for (const std::string& stem : scenes) {
            bool inc = listed(s.exportScenes, stem);
            if (ImGui::Checkbox(stem.c_str(), &inc)) {
                if (inc) s.exportScenes.push_back(stem);
                else s.exportScenes.erase(
                         std::remove(s.exportScenes.begin(), s.exportScenes.end(), stem),
                         s.exportScenes.end());
            }
        }
        ImGui::Unindent();
        ui::hint("The start scene is always bundled, even if unchecked.");
    }

    ImGui::Spacing();
    ui::sectionText("Assets");
    ImGui::Checkbox("Export only used assets", &s.trimAssets);
    ui::hint("Ship only the models/textures the project references instead of the\n"
             "whole content library. Sounds and prefabs are always kept, and .lua\n"
             "scripts are scanned for asset names. Off = copy everything (safe).");

    ImGui::Checkbox("Pack content into an encrypted archive", &s.packContent);
    ui::hint("Bundle content/, project/ and assets/ into one encrypted game.fpak\n"
             "next to the exe, so the models, textures and sounds are not lying\n"
             "in open folders. Off ships loose files (useful to debug an export).");

    ImGui::Spacing();
    drawInstallerSection(s);

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button("Save", ImVec2(120.0f, 0.0f))) {
        applied = true;
        saved   = true;   // keeps the dismissal above from undoing the edits
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
        ImGui::CloseCurrentPopup();   // `opened` is restored on the next frame

    ImGui::EndPopup();
    return applied;
}

} // namespace game
