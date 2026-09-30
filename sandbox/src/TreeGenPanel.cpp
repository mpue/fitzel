#include "TreeGenPanel.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>

#include <imgui.h>

#include <fitzel/asset/Vfs.hpp>
#include <fitzel/graphics/Texture.hpp>

#include "TreeGlb.hpp"
#include "UiStyle.hpp"

namespace fs = std::filesystem;

namespace treeui {

namespace {

constexpr float kLabelW = 158.0f;

// One labelled amount: the caption on the left, a stepper (big minus, big
// plus, no drag) filling the rest of the row.
bool row(const char* label, float& v, float step, float lo, float hi, const char* fmt,
         const char* tip = nullptr) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tip) ImGui::SetItemTooltip("%s", tip);
    ImGui::SameLine(kLabelW);
    const bool changed = ui::stepper("##v", v, step, lo, hi, fmt,
                                     std::max(ImGui::GetContentRegionAvail().x, 120.0f));
    if (tip) ImGui::SetItemTooltip("%s", tip);
    ImGui::PopID();
    return changed;
}

bool rowInt(const char* label, int& v, int step, int lo, int hi, const char* tip = nullptr) {
    float f = static_cast<float>(v);
    if (!row(label, f, static_cast<float>(step), static_cast<float>(lo), static_cast<float>(hi),
             "%.0f", tip))
        return false;
    v = static_cast<int>(std::lround(f));
    return true;
}

bool rowCheck(const char* label, bool& v, const char* tip = nullptr) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tip) ImGui::SetItemTooltip("%s", tip);
    ImGui::SameLine(kLabelW);
    const bool changed = ImGui::Checkbox("##v", &v);
    if (tip) ImGui::SetItemTooltip("%s", tip);
    ImGui::PopID();
    return changed;
}

bool isImage(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ".png" || e == ".jpg" || e == ".jpeg";
}

// A file name made of the tree's name: letters, digits, - and _.
std::string fileStem(const char* name) {
    std::string s;
    for (const char* c = name; *c; ++c) {
        const unsigned char u = static_cast<unsigned char>(*c);
        if (std::isalnum(u) || u == '-' || u == '_') s.push_back(static_cast<char>(u));
        else if (u == ' ' && !s.empty() && s.back() != '_') s.push_back('_');
    }
    while (!s.empty() && s.back() == '_') s.pop_back();
    return s.empty() ? std::string("tree") : s;
}

std::string thousands(std::size_t n) {
    char b[32];
    if (n >= 1000) std::snprintf(b, sizeof b, "%zu,%03zu", n / 1000, n % 1000);
    else std::snprintf(b, sizeof b, "%zu", n);
    return b;
}

fitzel::Texture studioTexture(const treeglb::Image& im, const std::string& path) {
    if (!im.rgba.empty()) return fitzel::Texture::fromPixels(im.rgba.data(), im.w, im.h, 4);
    // Unflipped: the tree's UVs follow glTF, top row first.
    return fitzel::Texture::fromFile(path, false);
}

} // namespace

TreeGenTool::TreeGenTool(Deps d) : m_d(d) {
    // The bark and leaf sheet the editor ships with, where they are: a first
    // tree should look like a tree, not like the built-in stand-ins.
    const std::string content = FITZEL_CONTENT_DIR;
    const std::string bark = content + "/models/Bark001_2K-JPG_Color.jpg";
    const std::string leaf = content + "/models/LeafSet024_1K-JPG_Color-LeafSet024_1K-JPG_Opacity.png";
    if (fitzel::vfs::exists(bark)) m_barkPath = bark;
    if (fitzel::vfs::exists(leaf)) m_broad = {leaf, 3, 3};
    m_barkNormal = suggestNormal(m_barkPath, m_barkNormalDX);
    applyPreset(0);
}

std::string TreeGenTool::suggestNormal(const std::string& colourPath, bool& dx) {
    dx = false;
    if (colourPath.empty()) return {};
    const fs::path p(colourPath);
    const std::string stem = p.stem().string();
    // What the colour map is called -> what its normal map is called, by the
    // sites bark textures come from (ambientCG, Poly Haven, Quixel, ...).
    static const std::pair<const char*, const char*> kSwap[] = {
        {"_Color", "_NormalGL"}, {"_Color", "_Normal"}, {"_Color", "_NormalDX"},
        {"Color", "NormalGL"},   {"Color", "Normal"},
        {"_diff_", "_nor_gl_"},  {"_diff_", "_nor_dx_"}, {"_diff", "_nor_gl"},
        {"_Diffuse", "_Normal"}, {"Diffuse", "Normal"},
        {"_BaseColor", "_Normal"}, {"_basecolor", "_normal"},
        {"_Albedo", "_Normal"},  {"_albedo", "_normal"},
        {"_col", "_nrm"},        {"_col", "_normal"},
    };
    for (const auto& [from, to] : kSwap) {
        const std::size_t at = stem.find(from);
        if (at == std::string::npos) continue;
        std::string cand = stem;
        cand.replace(at, std::strlen(from), to);
        for (const char* ext : {".png", ".jpg", ".jpeg"}) {
            const std::string f = (p.parent_path() / (cand + ext)).generic_string();
            if (fitzel::vfs::exists(f)) {
                std::string low = cand;
                std::transform(low.begin(), low.end(), low.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                dx = low.find("dx") != std::string::npos;
                return f;
            }
        }
    }
    return {};
}

std::string TreeGenTool::projectDir() const {
    if (m_d.currentProject.empty()) return {};
    return fs::path(m_d.currentProject).parent_path().generic_string();
}

std::string TreeGenTool::treesDir() const {
    const std::string p = projectDir();
    return p.empty() ? std::string() : p + "/trees";
}

void TreeGenTool::applyPreset(int i) {
    const auto& list = treegen::presets();
    if (i < 0 || i >= static_cast<int>(list.size())) return;
    m_preset = i;
    const float detail = m_p.detail, density = m_p.leafDensity, relief = m_p.barkNormalStrength;
    m_p = list[static_cast<std::size_t>(i)].params;
    // The budget and the bark's relief are the author's, not the species'.
    if (m_ready) { m_p.detail = detail; m_p.leafDensity = density; m_p.barkNormalStrength = relief; }
    std::snprintf(m_name, sizeof m_name, "%s", m_p.name.c_str());
    m_dirty = m_texDirty = true;
}

void TreeGenTool::reloadTextures() {
    m_texDirty = false;
    const bool needles = m_p.leaves.alongTwig;
    LeafPick& lp = needles ? m_needle : m_broad;
    treeglb::Image bark = m_barkPath.empty() ? treeglb::Image{} : treeglb::readImage(m_barkPath);
    if (!bark.valid()) { bark = treeglb::builtinBark(); m_barkPath.clear(); }
    treeglb::Image leaf = lp.path.empty() ? treeglb::Image{} : treeglb::readImage(lp.path);
    if (!leaf.valid()) {
        leaf = needles ? treeglb::builtinNeedles() : treeglb::builtinLeaf();
        lp = LeafPick{};
    }
    // The relief, in glTF's convention whatever it came in.
    treeglb::Image nrm = m_barkNormal.empty() ? treeglb::Image{} : treeglb::readImage(m_barkNormal);
    if (!nrm.valid()) m_barkNormal.clear();
    else if (m_barkNormalDX) nrm = treeglb::flipGreen(nrm);
    m_p.barkTexture = m_barkPath;
    m_p.barkNormal = m_barkNormal;
    m_p.barkNormalDX = m_barkNormalDX;
    m_p.leafTexture = lp.path;
    m_p.leaves.cols = std::max(1, lp.cols);
    m_p.leaves.rows = std::max(1, lp.rows);
    m_p.barkAspect  = static_cast<float>(bark.h) / static_cast<float>(std::max(bark.w, 1));
    m_p.leaves.aspect = (static_cast<float>(leaf.w) / static_cast<float>(m_p.leaves.cols)) /
                        std::max(static_cast<float>(leaf.h) / static_cast<float>(m_p.leaves.rows), 1.0f);
    if (m_studioOk)
        m_studio.setTextures(studioTexture(bark, m_barkPath), studioTexture(leaf, lp.path),
                             nrm.valid() ? studioTexture(nrm, m_barkNormal) : fitzel::Texture{});
    m_dirty = true;
}

void TreeGenTool::regenerate() {
    if (m_texDirty) reloadTextures();
    const auto t0 = std::chrono::steady_clock::now();
    m_mesh = treegen::generate(m_p);
    m_genMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (m_studioOk) m_studio.setMesh(m_mesh);
    m_dirty = false;
    m_redraw = true;
}

void TreeGenTool::scanImages() {
    m_scannedFor = projectDir();
    m_images.clear();
    auto add = [&](const std::string& dir) {
        if (dir.empty() || !fitzel::vfs::isDirectory(dir)) return;
        for (const std::string& f : fitzel::vfs::listFiles(dir, true))
            if (isImage(f)) m_images.push_back({fs::path(f).filename().string(), f});
    };
    add(FITZEL_CONTENT_DIR);
    add(m_scannedFor);
    std::sort(m_images.begin(), m_images.end(), [](const Img& a, const Img& b) {
        return a.name != b.name ? a.name < b.name : a.path < b.path;
    });
    m_images.erase(std::unique(m_images.begin(), m_images.end(),
                               [](const Img& a, const Img& b) { return a.path == b.path; }),
                   m_images.end());
}

std::vector<std::string> TreeGenTool::savedTrees() const {
    std::vector<std::string> out;
    const std::string dir = treesDir();
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".glb") out.push_back(e.path().generic_string());
    std::sort(out.begin(), out.end());
    return out;
}

bool TreeGenTool::texturePicker(const char* id, const char* label, std::string& path,
                                const char* emptyName, const char* emptyTip) {
    bool changed = false;
    ImGui::PushID(id);
    const std::string shown = path.empty() ? std::string(emptyName) : fs::path(path).filename().string();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(kLabelW);
    if (ImGui::Button(shown.c_str(), ImVec2(-1.0f, 0.0f))) {
        m_search[0] = '\0';
        ImGui::OpenPopup("##pick");
    }
    ImGui::SetItemTooltip("%s", path.empty() ? emptyTip : path.c_str());
    if (ImGui::BeginPopup("##pick")) {
        ui::searchBox("##search", m_search, sizeof m_search, "Search images...");
        ImGui::BeginChild("##list", ImVec2(420.0f, 340.0f), ImGuiChildFlags_Borders);
        const float rowH = ImGui::GetTextLineHeight() + 8.0f;
        if (ImGui::Selectable(emptyName, path.empty(), 0, ImVec2(0.0f, rowH))) {
            path.clear();
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        std::vector<int> hits;
        for (int i = 0; i < static_cast<int>(m_images.size()); ++i)
            if (m_search[0] == '\0' || ui::icontains(m_images[static_cast<std::size_t>(i)].path.c_str(), m_search))
                hits.push_back(i);
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(hits.size()), rowH + ImGui::GetStyle().ItemSpacing.y);
        while (clip.Step())
            for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
                const Img& im = m_images[static_cast<std::size_t>(hits[static_cast<std::size_t>(k)])];
                ImGui::PushID(k);
                if (ImGui::Selectable(im.name.c_str(), im.path == path, 0, ImVec2(0.0f, rowH))) {
                    path = im.path;
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SetItemTooltip("%s", im.path.c_str());
                ImGui::PopID();
            }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

void TreeGenTool::levelSection(int l, const char* title) {
    treegen::Level& L = m_p.level[l];
    if (!ui::header(title)) return;
    ImGui::PushID(l);
    bool d = false;
    d |= rowInt("Count", L.count, 1, 0, 400, "How many grow on a parent of full length.");
    d |= row(l == 1 ? "Crown base" : "Start", L.start, 0.02f, 0.0f, 0.98f, "%.2f",
             l == 1 ? "Height of the lowest limb, as a fraction of the tree."
                    : "Where along the parent they begin (0 = its base).");
    d |= row("End", L.end, 0.02f, 0.02f, 1.0f, "%.2f", "Where along the parent they stop.");
    d |= row("Length", L.length, 0.02f, 0.02f, 1.5f, "%.2f",
             l == 1 ? "The longest limb, as a fraction of the tree height (the crown shape shortens the rest)."
                    : "As a fraction of the parent; nearer the parent's tip they are shorter.");
    d |= row("Length variation", L.lengthVar, 0.05f, 0.0f, 0.9f, "%.2f");
    d |= row("Angle at base", L.angle, 5.0f, 0.0f, 180.0f, "%.0f deg", "From the parent, where it starts.");
    d |= row("Angle at tip", L.angleTip, 5.0f, 0.0f, 180.0f, "%.0f deg", "From the parent, near its tip.");
    d |= row("Angle variation", L.angleVar, 1.0f, 0.0f, 45.0f, "%.0f deg");
    d |= rowInt("Per node", L.whorl, 1, 1, 8, "1 = alternate / spiral, 2 = opposite pairs, 3+ = whorls (conifers).");
    d |= row("Turn per node", L.rotate, 5.0f, 0.0f, 360.0f, "%.1f deg",
             "137.5 = the golden angle (spiral), 180 = two rows, 90 = crossed pairs, 0 = all in one plane.");
    d |= row("Curve", L.curve, 5.0f, -180.0f, 180.0f, "%.0f deg", "Bend over the length: + up, - arching down.");
    d |= row("Gnarl", L.gnarl, 0.05f, 0.0f, 2.0f, "%.2f", "Smooth wandering of the wood.");
    d |= row("Gravity", L.gravity, 0.1f, -3.0f, 8.0f, "%.1f", "+ droops (weeping), - rises towards the light.");
    d |= row("Taper", L.taper, 0.05f, 0.0f, 0.95f, "%.2f", "Extra thinning towards the tip.");
    d |= rowInt("Forks", L.forks, 1, 0, 3, "Each fork splits every piece in two.");
    if (L.forks > 0) d |= row("Fork angle", L.forkAngle, 5.0f, 0.0f, 90.0f, "%.0f deg");
    d |= rowInt("Segments", L.segments, 1, 2, 48, "Resolution along a stem of full length.");
    d |= rowInt("Sides", L.sides, 1, 3, 32, "Most vertices round a stem (thin ones use fewer).");
    ImGui::PopID();
    if (d) m_dirty = true;
}

void TreeGenTool::save(bool asSpecies) {
    const std::string dir = treesDir();
    if (dir.empty()) { m_d.status = "Open a project first: trees are saved in its trees/ folder."; return; }
    if (m_dirty || m_texDirty) regenerate();
    std::error_code ec;
    fs::create_directories(dir, ec);
    m_p.name = m_name;
    const std::string file = fileStem(m_name) + ".glb";
    const std::string path = dir + "/" + file;
    const LeafPick& lp = m_p.leaves.alongTwig ? m_needle : m_broad;
    treeglb::Image bark = m_barkPath.empty() ? treeglb::builtinBark() : treeglb::readImage(m_barkPath);
    treeglb::Image leaf = lp.path.empty() ? (m_p.leaves.alongTwig ? treeglb::builtinNeedles() : treeglb::builtinLeaf())
                                          : treeglb::readImage(lp.path);
    treeglb::Image nrm = m_barkNormal.empty() ? treeglb::Image{} : treeglb::readImage(m_barkNormal);
    if (nrm.valid() && m_barkNormalDX) nrm = treeglb::flipGreen(nrm);
    std::string err;
    if (!treeglb::writeGlb(path, m_mesh, m_p, bark, leaf, nrm, &err)) {
        m_d.status = "Could not save the tree: " + err;
        return;
    }
    m_lastSaved = path;
    if (m_d.saved) m_d.saved();
    if (asSpecies && m_d.plant) {
        m_d.plant(file, m_p.name, m_mesh.hi.y - m_mesh.lo.y);
        m_d.status = "Saved trees/" + file + " and planted it as the species '" + m_p.name + "'.";
    } else {
        m_d.status = "Saved trees/" + file + ".";
    }
}

void TreeGenTool::open(const std::string& file) {
    treegen::Params p;
    if (!treeglb::readParams(file, p)) {
        m_d.status = "Not a generated tree: " + fs::path(file).filename().string();
        return;
    }
    m_p = p;
    m_preset = -1;
    std::snprintf(m_name, sizeof m_name, "%s", m_p.name.c_str());
    // Its textures, where they still are; otherwise the current picks stay.
    if (!p.barkTexture.empty() && fitzel::vfs::exists(p.barkTexture)) m_barkPath = p.barkTexture;
    if (p.barkNormal.empty() || fitzel::vfs::exists(p.barkNormal)) {
        m_barkNormal = p.barkNormal;
        m_barkNormalDX = p.barkNormalDX;
    }
    if (!p.leafTexture.empty() && fitzel::vfs::exists(p.leafTexture))
        (p.leaves.alongTwig ? m_needle : m_broad) = LeafPick{p.leafTexture, p.leaves.cols, p.leaves.rows};
    m_dirty = m_texDirty = true;
    m_d.status = "Opened " + fs::path(file).filename().string() + ".";
}
void TreeGenTool::panel(bool& show) {
    ImGui::SetNextWindowSize(ImVec2(480.0f, 940.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Tree generator", &show)) { ImGui::End(); return; }
    if (!m_ready) {
        m_ready = true;
        m_studioOk = m_studio.init();
        m_texDirty = m_dirty = true;
    }
    if (m_scannedFor != projectDir()) scanImages();
    if (m_dirty || m_texDirty) regenerate();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float sp = ImGui::GetStyle().ItemSpacing.x;

    // --- The studio ------------------------------------------------------------
    {
        const float w = avail, h = std::min(avail * 1.1f, 560.0f);
        if (m_turntable) { m_view.yaw += 20.0f * ImGui::GetIO().DeltaTime; m_redraw = true; }
        if (m_studioOk) {
            // Drawn at 1.5x and shown smaller: the leaves are alpha-tested and
            // the studio has no MSAA of its own.
            const int pw = static_cast<int>(w * 1.5f), ph = static_cast<int>(h * 1.5f);
            if (m_redraw || m_tex == 0 || pw != m_studio.width() || ph != m_studio.height()) {
                m_tex = m_studio.render(pw, ph, m_view);
                m_redraw = false;
            }
            ImGui::Image((ImTextureID)(intptr_t)m_tex, ImVec2(w, h), ImVec2(0, 1), ImVec2(1, 0));
        } else {
            ImGui::Dummy(ImVec2(w, 60.0f));
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "The preview shaders did not compile.");
        }
        // The view, by buttons: turn, tilt, distance, sun.
        const float bw = (avail - 5.0f * sp) / 6.0f;
        auto btn = [&](const char* label, const char* tip) {
            const bool hit = ImGui::Button(label, ImVec2(bw, 30.0f));
            ImGui::SetItemTooltip("%s", tip);
            return hit;
        };
        if (btn("Turn <", "Walk round the tree to the left.")) { m_view.yaw -= 30.0f; m_redraw = true; }
        ImGui::SameLine();
        if (btn("Turn >", "Walk round the tree to the right.")) { m_view.yaw += 30.0f; m_redraw = true; }
        ImGui::SameLine();
        if (btn("Higher", "Look from higher up.")) { m_view.pitch = std::min(m_view.pitch + 12.0f, 85.0f); m_redraw = true; }
        ImGui::SameLine();
        if (btn("Lower", "Look from lower down.")) { m_view.pitch = std::max(m_view.pitch - 12.0f, -20.0f); m_redraw = true; }
        ImGui::SameLine();
        if (btn("Closer", "Step closer.")) { m_view.zoom = std::max(m_view.zoom * 0.75f, 0.12f); m_redraw = true; }
        ImGui::SameLine();
        if (btn("Farther", "Step back.")) { m_view.zoom = std::min(m_view.zoom / 0.75f, 3.0f); m_redraw = true; }
        if (btn("Sun <", "Move the sun round.")) { m_view.sunYaw -= 30.0f; m_redraw = true; }
        ImGui::SameLine();
        if (btn("Sun >", "Move the sun round.")) { m_view.sunYaw += 30.0f; m_redraw = true; }
        ImGui::SameLine();
        if (btn("Sun up", "Raise the sun.")) { m_view.sunPitch = std::min(m_view.sunPitch + 12.0f, 85.0f); m_redraw = true; }
        ImGui::SameLine();
        if (btn("Sun low", "Lower the sun.")) { m_view.sunPitch = std::max(m_view.sunPitch - 12.0f, 4.0f); m_redraw = true; }
        ImGui::SameLine();
        if (btn("Crown", "Look at the crown / back at the whole tree.")) {
            m_view.lift = m_view.lift > 0.1f ? 0.0f : 0.25f;
            m_redraw = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Spin", &m_turntable)) m_redraw = true;
        ImGui::SetItemTooltip("Turn the tree slowly, like a turntable.");
        ui::hint("%s bark + %s leaf triangles  |  %d stems, %d leaves  |  %.2f m  |  %.0f ms",
                 thousands(m_mesh.barkTris()).c_str(), thousands(m_mesh.leafTris()).c_str(),
                 m_mesh.stems, m_mesh.leafCards, m_mesh.hi.y - m_mesh.lo.y, m_genMs);
    }

    // --- The species -------------------------------------------------------------
    ui::sectionText("Species");
    {
        const auto& list = treegen::presets();
        const float tw = (avail - 2.0f * sp) / 3.0f;
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            if (i % 3) ImGui::SameLine();
            const bool on = i == m_preset;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(list[static_cast<std::size_t>(i)].name, ImVec2(tw, 40.0f))) applyPreset(i);
            if (on) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", list[static_cast<std::size_t>(i)].hint);
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine(kLabelW);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##name", m_name, sizeof m_name)) m_p.name = m_name;
    {
        float seed = static_cast<float>(m_p.seed);
        if (row("Seed", seed, 1.0f, 0.0f, 999999.0f, "%.0f", "Same seed, same tree.")) {
            m_p.seed = static_cast<std::uint32_t>(seed);
            m_dirty = true;
        }
        if (ImGui::Button("New variant", ImVec2(-1.0f, 34.0f))) {
            std::random_device entropy;
            m_p.seed = entropy() % 1000000u;
            m_dirty = true;
        }
        ImGui::SetItemTooltip("Another tree of the same kind (a new seed).");
    }

    // --- Textures -------------------------------------------------------------------
    // First after the species: your own bark and leaves are what make the tree yours.
    if (ui::header("Textures", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (texturePicker("bark", "Bark texture", m_barkPath, "Built-in",
                          "Bark drawn by the generator. Pick your own image.")) {
            // Its normal map comes along, where it lies beside it by name.
            m_barkNormal = suggestNormal(m_barkPath, m_barkNormalDX);
            m_texDirty = true;
        }
        if (row("Bark tile width", m_p.barkTile, 0.05f, 0.1f, 5.0f, "%.2f m",
                "How wide one repeat of the bark image is on the trunk."))
            m_dirty = true;
        if (texturePicker("bnrm", "Bark normal map", m_barkNormal, "None",
                          "No relief. Pick the bark's normal map (NormalGL, _nor_gl_ ...): "
                          "the furrows then catch the light.")) {
            std::string low = fs::path(m_barkNormal).stem().string();
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            m_barkNormalDX = low.find("dx") != std::string::npos;
            m_texDirty = true;
        }
        if (!m_barkNormal.empty()) {
            if (row("Relief", m_p.barkNormalStrength, 0.1f, 0.0f, 3.0f, "%.1f",
                    "How strongly the normal map tilts the light (saved in the .glb)."))
                m_redraw = true;
            if (m_studioOk) m_studio.setReliefStrength(m_p.barkNormalStrength);
            if (rowCheck("DirectX map", m_barkNormalDX,
                         "Green points down (NormalDX, _nor_dx_): flipped to glTF's convention "
                         "for the preview and in the saved file. Found from the name; tick it if "
                         "the furrows look lit from below."))
                m_texDirty = true;
        }
        LeafPick& lp = m_p.leaves.alongTwig ? m_needle : m_broad;
        if (texturePicker("leaf", "Leaf texture", lp.path, "Built-in",
                          "A leaf drawn by the generator. Pick your own: PNG with alpha."))
            m_texDirty = true;
        if (rowInt("Atlas columns", lp.cols, 1, 1, 8,
                   "The leaf image holds a grid of leaves: each card takes one at random."))
            m_texDirty = true;
        if (rowInt("Atlas rows", lp.rows, 1, 1, 8)) m_texDirty = true;
        ui::hint("Leaves stand on the bottom edge of their cell, stalk down.");
    }

    bool d = false;
    // --- Trunk ----------------------------------------------------------------------
    if (ui::header("Trunk")) {
        treegen::Level& T = m_p.level[0];
        d |= row("Height", m_p.height, 0.5f, 0.5f, 80.0f, "%.1f m", "The whole tree, top to foot.");
        d |= row("Height variation", m_p.heightVar, 0.05f, 0.0f, 0.5f, "%.2f", "How much a new seed may differ.");
        d |= row("Trunk radius", m_p.radius, 0.02f, 0.02f, 3.0f, "%.2f m", "Above the root flare.");
        float mm = m_p.twigRadius * 1000.0f;
        if (row("Twig radius", mm, 0.5f, 1.0f, 30.0f, "%.1f mm",
                "Every twig tip. The wood in between follows the pipe model: a stem is as thick as all it carries.")) {
            m_p.twigRadius = mm / 1000.0f;
            d = true;
        }
        d |= row("Lean", m_p.lean, 1.0f, 0.0f, 40.0f, "%.0f deg");
        d |= row("Root flare", m_p.flare, 0.05f, 0.0f, 3.0f, "%.2f", "Extra girth at the ground.");
        d |= row("Flare height", m_p.flareHeight, 0.1f, 0.1f, 5.0f, "%.1f m");
        d |= rowInt("Root lobes", m_p.buttress, 1, 0, 12, "Buttresses round the foot (0 = round).");
        d |= row("Bark relief", m_p.relief, 0.01f, 0.0f, 0.3f, "%.2f", "Ridges in the trunk's silhouette.");
        d |= rowInt("Forks", T.forks, 1, 0, 4, "The trunk splits into limbs: 1 = two, 2 = four.");
        if (T.forks > 0) {
            d |= row("Fork from", m_p.forkStart, 0.02f, 0.0f, 0.95f, "%.2f", "Fraction of the height.");
            d |= row("Fork to", m_p.forkEnd, 0.02f, 0.0f, 0.98f, "%.2f");
            d |= row("Fork angle", T.forkAngle, 5.0f, 0.0f, 90.0f, "%.0f deg");
        }
        d |= row("Gnarl", T.gnarl, 0.05f, 0.0f, 2.0f, "%.2f");
        d |= row("Gravity", T.gravity, 0.1f, -3.0f, 3.0f, "%.1f", "- keeps it upright, + lets it sag.");
        d |= row("Taper", T.taper, 0.05f, 0.0f, 0.95f, "%.2f");
        d |= rowInt("Segments", T.segments, 1, 4, 64);
        d |= rowInt("Sides", T.sides, 1, 5, 48);
    }
    // --- Crown ----------------------------------------------------------------------
    if (ui::header("Crown")) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Shape");
        ImGui::SetItemTooltip("How long the limbs are by their height in the crown.");
        ImGui::SameLine(kLabelW);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##shape", treegen::shapeName(m_p.shape))) {
            for (int s = 0; s < static_cast<int>(treegen::Shape::Count); ++s) {
                const auto sh = static_cast<treegen::Shape>(s);
                if (ImGui::Selectable(treegen::shapeName(sh), sh == m_p.shape,
                                      0, ImVec2(0.0f, ImGui::GetTextLineHeight() + 8.0f))) {
                    m_p.shape = sh;
                    d = true;
                }
            }
            ImGui::EndCombo();
        }
        d |= rowInt("Branch levels", m_p.levels, 1, 1, 3, "Limbs, branches, twigs.");
    }
    if (d) m_dirty = true;
    levelSection(1, "Limbs");
    if (m_p.levels >= 2) levelSection(2, "Branches");
    if (m_p.levels >= 3) levelSection(3, "Twigs");

    // --- Leaves ---------------------------------------------------------------------
    d = false;
    if (ui::header("Leaves")) {
        treegen::Leaves& L = m_p.leaves;
        d |= rowCheck("Leaves", L.enabled);
        d |= row("Clusters per twig", L.perStem, 1.0f, 0.0f, 80.0f, "%.0f");
        d |= rowInt("Leaves per cluster", L.cluster, 1, 1, 12,
                    "A rosette round the twig -- or, for sprays, crossed cards (a bottle brush).");
        d |= row("Size", L.size, 0.01f, 0.01f, 3.0f, "%.2f m", "One card, stalk to tip.");
        d |= row("Size variation", L.sizeVar, 0.05f, 0.0f, 0.9f, "%.2f");
        d |= row("Start on twig", L.start, 0.05f, 0.0f, 0.95f, "%.2f", "0 = all along the twig, 0.7 = tufts at the tips.");
        if (!L.alongTwig) d |= row("Angle", L.angle, 5.0f, 0.0f, 120.0f, "%.0f deg", "Leaf from the twig.");
        d |= row("Face the sky", L.up, 0.05f, 0.0f, 1.0f, "%.2f", "0 = every way, 1 = blades turned up to the light.");
        d |= row("Droop", L.droop, 0.05f, 0.0f, 2.0f, "%.2f");
        d |= row("Soft shading", L.bend, 0.05f, 0.0f, 1.0f, "%.2f",
                 "Bends the leaf normals out of the crown, so it shades as one soft volume instead of confetti.");
        d |= row("Hollow crown", L.hollow, 0.05f, 0.0f, 0.9f, "%.2f",
                 "Leaves off the crown's shaded core -- a real crown is a shell.");
        d |= row("Fold", L.fold, 0.05f, 0.0f, 1.5f, "%.2f", "Folds each leaf along its midrib (4 triangles instead of 2).");
        if (rowCheck("Sprays along twig", L.alongTwig,
                     "For needle sprays and twig cards: the card lies along the twig, stalk end first.")) {
            d = true;
            m_texDirty = true;
        }
        d |= rowCheck("Also on branches", L.onParent, "Leaves on the outer part of the next-coarser level too.");
    }
    // --- Budget ---------------------------------------------------------------------
    if (ui::header("Budget")) {
        d |= row("Detail", m_p.detail, 0.1f, 0.25f, 2.0f, "%.2f", "Rings, sides and segments of the wood.");
        d |= row("Leaf density", m_p.leafDensity, 0.1f, 0.1f, 2.0f, "%.2f", "Fewer leaves grow larger to cover the same.");
        ui::hint("Vegetation derives the far levels, shadows and impostors from this mesh itself.");
    }
    if (d) m_dirty = true;

    // --- Save -------------------------------------------------------------------------
    ImGui::Separator();
    const bool project = !projectDir().empty();
    ImGui::BeginDisabled(!project || m_mesh.empty());
    if (ImGui::Button("Save tree", ImVec2((avail - sp) * 0.4f, 38.0f))) save(false);
    ImGui::SetItemTooltip("Write trees/%s.glb in the project.", fileStem(m_name).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Save + plant in Vegetation", ImVec2(-1.0f, 38.0f))) save(true);
    ImGui::SetItemTooltip("Save, and make it a Vegetation species (or reload the species that already uses this file).");
    ImGui::EndDisabled();
    if (!project) ui::hint("Open or create a project: trees are saved in its trees/ folder.");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##open", "Open a saved tree...")) {
        const std::vector<std::string> saved = savedTrees();
        if (saved.empty()) ImGui::TextDisabled("No trees in this project yet.");
        for (const std::string& f : saved)
            if (ImGui::Selectable(fs::path(f).stem().string().c_str(), f == m_lastSaved,
                                  0, ImVec2(0.0f, ImGui::GetTextLineHeight() + 8.0f)))
                open(f);
        ImGui::EndCombo();
    }
    if (!m_lastSaved.empty()) ui::hint("Last saved: %s", m_lastSaved.c_str());
    ImGui::End();
}

} // namespace treeui