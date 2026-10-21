#include "HoldPosePanel.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "UiStyle.hpp"

namespace fs = std::filesystem;

namespace holdposeui {

namespace {

const float kDeg = 3.14159265f / 180.0f;
const float kMoveSteps[] = {0.001f, 0.005f, 0.01f, 0.02f};      // metres
const char* kMoveNames[] = {"1 mm", "5 mm", "1 cm", "2 cm"};
const float kTurnSteps[] = {1.0f, 5.0f, 15.0f};                  // degrees
const char* kTurnNames[] = {"1 deg", "5 deg", "15 deg"};
const ImVec4 kSideCol[2] = {ImVec4(0.35f, 0.62f, 1.0f, 1.0f), ImVec4(1.0f, 0.42f, 0.32f, 1.0f)};
const char* kSideName[2] = {"Left hand", "Right hand"};

std::string fileName(const std::string& p) { return fs::path(p).filename().string(); }

template <class T>
bool ready(const std::future<T>& f) {
    return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// A copy kept once, the first time this window writes over a file.
void backupOnce(const std::string& file) {
    std::error_code ec;
    const fs::path b = fs::path(file + ".before-holdpose");
    if (!fs::exists(b, ec) && fs::exists(fs::path(file), ec)) fs::copy_file(fs::path(file), b, ec);
}

// Two buttons, - and +, for an amount that has no number worth showing.
int pair(const char* id, float w) {
    int r = 0;
    ImGui::PushID(id);
    if (ImGui::Button("-", ImVec2(w, 0.0f))) r = -1;
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button("+", ImVec2(w, 0.0f))) r = 1;
    ImGui::PopID();
    return r;
}

} // namespace

HoldPoseTool::HoldPoseTool(Deps d) : m_d(d) {}
HoldPoseTool::~HoldPoseTool() = default;

std::string HoldPoseTool::projectDir() const {
    if (!m_d.currentProject.empty()) return fs::path(m_d.currentProject).parent_path().generic_string();
    if (!m_armsPath.empty()) return fs::path(m_armsPath).parent_path().parent_path().generic_string();
    return {};
}

std::string HoldPoseTool::holdsFile() const {
    return m_recipe.holds.empty() ? std::string() : holdpose::resolve(m_armsPath, m_recipe.holds);
}

bool HoldPoseTool::idle() const {
    return !m_armsJob.valid() && !m_gunJob.valid() && !m_wantPose;
}

// --- Loading -----------------------------------------------------------------------------

void HoldPoseTool::scanModels() {
    m_modelsFor = projectDir();
    m_models.clear();
    if (m_modelsFor.empty()) return;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(fs::path(m_modelsFor), fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        const std::string n = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (n == "build" || n == ".git" || n == "node_modules" || (!n.empty() && n[0] == '.')) it.disable_recursion_pending();
            continue;
        }
        if (it->path().extension() != ".glb") continue;
        ModelEntry e;
        e.path = it->path().generic_string();
        e.name = holdpose::relative(m_modelsFor + "/x", e.path);
        e.skinned = retarget::inspect(e.path).skinned;
        m_models.push_back(e);
    }
    std::sort(m_models.begin(), m_models.end(), [](const ModelEntry& a, const ModelEntry& b) { return a.name < b.name; });
}

void HoldPoseTool::openArms(const std::string& model) {
    m_armsPath = fs::path(model).generic_string();
    m_arms.reset();
    m_rig.reset();
    m_recipe = holdpose::Recipe{};
    holdpose::loadRecipe(m_armsPath, m_recipe);
    m_infoError.clear();
    m_info = holdpose::HoldsInfo{};
    if (!holdsFile().empty()) holdpose::readHoldsInfo(holdsFile(), m_info, m_infoError);
    const std::string path = m_armsPath;
    m_armsJob = std::async(std::launch::async, [path] {
        ArmsLoad r;
        auto md = std::make_shared<fitzel::ModelData>(fitzel::loadGltf(path));
        if (md->empty()) { r.error = "Cannot read " + fileName(path); return r; }
        shrinkForStudio(*md);
        r.model = md;
        r.rig = std::make_shared<retarget::Rig>(retarget::loadRig(path, false));
        return r;
    });
    if (m_pose.empty() || !m_wantPose) {
        m_wantName = m_pose;
        m_wantPose = true;
    }
    m_message.clear();
    m_redraw = true;
}

void HoldPoseTool::loadGun(const std::string& file) {
    if (file == m_gunPath && (m_gun || m_gunJob.valid())) return;
    m_gunPath = file;
    m_gun.reset();
    if (file.empty()) return;
    m_gunJobPath = file;
    m_gunJob = std::async(std::launch::async, [file] {
        auto md = std::make_shared<fitzel::ModelData>(fitzel::loadGltf(file));
        shrinkForStudio(*md);
        return md;
    });
}

void HoldPoseTool::poll() {
    if (ready(m_armsJob)) {
        ArmsLoad r = m_armsJob.get();
        if (!r.error.empty()) {
            m_message = r.error;
            m_messageBad = true;
        } else {
            m_arms = r.model;
            m_rig = r.rig;
            glm::vec3 size;
            holdpose::bounds(*m_arms, m_armsCentre, size);
            if (!m_poser.init(*m_arms)) {
                m_message = m_poser.problem();
                m_messageBad = true;
            }
        }
        m_shown[1] = nullptr;
        m_redraw = true;
    }
    if (ready(m_gunJob)) {
        ModelPtr g = m_gunJob.get();
        if (m_gunJobPath == m_gunPath) {
            m_gun = g && !g->empty() ? g : nullptr;
            if (m_gun) {
                glm::vec3 size;
                holdpose::bounds(*m_gun, m_gunCentre, size);
            } else {
                m_message = "Cannot read the weapon " + fileName(m_gunPath);
                m_messageBad = true;
            }
        }
        m_shown[0] = nullptr;
        m_redraw = true;
    }
    if (m_wantPose && m_arms && !m_armsJob.valid()) {
        m_wantPose = false;
        const std::vector<std::string> names = poseNames();
        std::string pick = m_wantName;
        if (std::find(names.begin(), names.end(), pick) == names.end()) pick = names.empty() ? "" : names.front();
        selectPose(pick);
    }
}

std::vector<std::string> HoldPoseTool::poseNames() const {
    std::set<std::string> s;
    for (const auto& [name, w] : m_recipe.poses) s.insert(name);
    if (m_arms)
        for (const fitzel::AnimationClip& c : m_arms->animations)
            if (c.name.rfind("hold_", 0) == 0 && c.name.size() > 5) s.insert(c.name.substr(5));
    return {s.begin(), s.end()};
}

void HoldPoseTool::selectPose(const std::string& name) {
    m_pose = name;
    const auto it = m_recipe.poses.find(name);
    m_weapon = it != m_recipe.poses.end() ? it->second : holdpose::Weapon{};
    m_weaponDirty = false;
    loadGun(m_weapon.model.empty() ? std::string() : holdpose::resolve(m_armsPath, m_weapon.model));
    startPose();
}

void HoldPoseTool::startPose() {
    m_undo.clear();
    m_dirty = false;
    m_placing = false;
    m_base.clear();
    if (!m_arms || m_pose.empty()) { m_redraw = true; return; }
    int clip = -1;
    for (std::size_t i = 0; i < m_arms->animations.size(); ++i)
        if (m_arms->animations[i].name == "hold_" + m_pose) clip = static_cast<int>(i);
    m_base = m_poser.basePose(*m_arms, clip);
    // Where the arms sit: the table's entry; a new pose starts where another one is.
    m_havePlacement = false;
    std::string err;
    holdpose::Placement p;
    if (!holdsFile().empty()) {
        m_havePlacement = holdpose::readPlacement(holdsFile(), m_pose, p, err);
        if (!m_havePlacement)
            for (const std::string& other : poseNames())
                if (other != m_pose && holdpose::readPlacement(holdsFile(), other, p, err)) break;
    }
    m_edit = holdpose::Edit{};
    m_edit.place = p;
    for (int side = 0; side < 2; ++side) {
        m_edit.hands[static_cast<std::size_t>(side)] = m_poser.handOf(m_base, side, armsM(), weaponFrame(m_weapon));
        if (!m_poser.hasArm(side)) m_edit.hands[static_cast<std::size_t>(side)].on = false;
    }
    if (clip < 0) {
        m_message = "No clip hold_" + m_pose + " yet: the arms stand in their rest pose until it is written.";
        m_messageBad = false;
    }
    m_redraw = true;
}

// --- Edits -----------------------------------------------------------------------------------

void HoldPoseTool::change() {
    m_undo.push_back(m_edit);
    if (m_undo.size() > 300) m_undo.erase(m_undo.begin());
    m_dirty = true;
    m_redraw = true;
}

void HoldPoseTool::undo() {
    if (m_undo.empty()) return;
    m_edit = m_undo.back();
    m_undo.pop_back();
    m_dirty = !m_undo.empty();
    m_redraw = true;
}

void HoldPoseTool::moveHand(int side, const glm::vec3& d) {
    change();
    m_edit.hands[static_cast<std::size_t>(side)].pos += d;
}

void HoldPoseTool::nudge(int side, const glm::vec3& d) { moveHand(side & 1, d); }

// A turn about one of the weapon's axes, round the middle of the palm: the hand
// rolls on its grip instead of swinging round the wrist.
void HoldPoseTool::turnHand(int side, const glm::vec3& axis, float deg) {
    change();
    holdpose::Hand& h = m_edit.hands[static_cast<std::size_t>(side)];
    glm::vec3 pivot = h.pos, facing;
    if (!m_G.empty() && m_poser.palm(m_G, side, pivot, facing))
        pivot = glm::vec3(glm::inverse(weaponM()) * armsM() * glm::vec4(pivot, 1.0f));
    const glm::quat q = glm::angleAxis(deg * kDeg, axis);
    h.rot = glm::normalize(q * h.rot);
    h.pos = pivot + q * (h.pos - pivot);
}

void HoldPoseTool::turnArms(const glm::vec3& axis, float deg) {
    change();
    m_edit.place.r = glm::mat3(glm::angleAxis(deg * kDeg, axis)) * m_edit.place.r;
}

glm::mat4 HoldPoseTool::weaponM() const { return holdpose::weaponFrame(m_weapon); }

glm::mat4 HoldPoseTool::armsM() const {
    return holdpose::armsMatrix(weaponM(), m_edit.place, m_info, m_armsCentre);
}

bool HoldPoseTool::placeHandAt(float px, float py) {
    if (!m_gun) return false;
    glm::vec3 o, d, hit, n;
    m_studio.ray(px, py, o, d);
    float dist = 0.0f;
    if (!holdpose::pick(*m_gun, holdpose::weaponMatrix(m_weapon, m_gunCentre), o, d, dist, hit, n)) return false;
    glm::vec3 c, facing;
    if (m_G.empty() || !m_poser.palm(m_G, m_side, c, facing)) return false;
    // The palm comes to lie on the surface: its middle a little off it, along the
    // surface's normal (the hand keeps its turn).
    const glm::mat4 A = armsM();
    const glm::vec3 palmView = glm::vec3(A * glm::vec4(c, 1.0f));
    const glm::vec3 want = hit + n * 0.03f;
    const glm::vec3 inWeapon = glm::vec3(glm::inverse(weaponM()) * glm::vec4(want - palmView, 0.0f));
    moveHand(m_side, inWeapon);
    return true;
}

bool HoldPoseTool::drawGizmo(float x, float y, float w, float h) {
    m_gizmoShown = false;
    if (m_gizmoOp == 0 || m_placing || m_G.empty() || !m_poser.hasArm(m_side)) {
        m_gizmoDrag = false;
        return false;
    }
    holdpose::Hand& hand = m_edit.hands[static_cast<std::size_t>(m_side)];
    if (!hand.on) return false;
    const glm::mat4 W = weaponM(), Wi = glm::inverse(W), A = armsM();
    // The gizmo sits on the middle of the palm -- of the hand at its target,
    // so it stays on the target even where the arm cannot reach.
    glm::vec3 pc, facing;
    if (!m_poser.palm(m_G, m_side, pc, facing)) return false;
    const glm::mat4 wristNow = Wi * A * m_G[static_cast<std::size_t>(m_poser.wrist(m_side))];
    const glm::vec3 palmInWrist = glm::vec3(glm::inverse(wristNow) * Wi * A * glm::vec4(pc, 1.0f));
    const glm::mat4 target = glm::translate(glm::mat4(1.0f), hand.pos) * glm::mat4_cast(hand.rot);
    const glm::vec3 pivot = glm::vec3(target * glm::vec4(palmInWrist, 1.0f));
    glm::mat4 M = W * glm::translate(glm::mat4(1.0f), pivot);
    if (m_gizmoHandAxes) M = M * glm::mat4_cast(hand.rot);
    const glm::mat4 before = M;

    ImGuizmo::PushID("holdpose");
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(x, y, w, h);
    // Larger than the scene viewport's: a hand is small in the picture.
    ImGuizmo::SetGizmoSizeClipSpace(0.2f);
    const bool snapHeld = ImGui::GetIO().KeyCtrl;
    const float step = m_gizmoOp == 1 ? kMoveSteps[m_moveStep] : kTurnSteps[m_turnStep];
    const float snap[3] = {step, step, step};
    ImGuizmo::Manipulate(glm::value_ptr(m_studio.viewMatrix()), glm::value_ptr(m_studio.projMatrix()),
                         m_gizmoOp == 1 ? ImGuizmo::TRANSLATE : ImGuizmo::ROTATE, ImGuizmo::LOCAL,
                         glm::value_ptr(M), nullptr, snapHeld ? snap : nullptr);
    const bool using3d = ImGuizmo::IsUsing();
    const bool busy = using3d || ImGuizmo::IsOver();
    ImGuizmo::SetGizmoSizeClipSpace(0.1f);   // ImGuizmo's own, for the scene viewport
    ImGuizmo::PopID();
    const glm::vec4 c = m_studio.projMatrix() * m_studio.viewMatrix() * glm::vec4(glm::vec3(before[3]), 1.0f);
    if (c.w > 1e-6f) {
        m_gizmoAt = glm::vec2(x + (c.x / c.w * 0.5f + 0.5f) * w, y + (0.5f - c.y / c.w * 0.5f) * h);
        m_gizmoShown = true;
    }
    if (using3d && !m_gizmoDrag) {
        change();                    // one undo step for the whole drag
        m_gizmoDrag = true;
    }
    if (!using3d) m_gizmoDrag = false;
    if (using3d && M != before) {
        // What the drag did, in the weapon's frame, done to the wrist's target.
        const glm::mat4 d = Wi * M * glm::inverse(before) * W;
        const glm::mat4 moved = d * target;
        hand.pos = glm::vec3(moved[3]);
        glm::mat3 r(moved);
        for (int k = 0; k < 3; ++k) r[k] = glm::normalize(r[k]);
        hand.rot = glm::normalize(glm::quat_cast(r));
        m_dirty = true;
        m_redraw = true;
    }
    return busy;
}

std::vector<glm::vec3> HoldPoseTool::viewJoints() {
    solve();
    std::vector<glm::vec3> out;
    const glm::mat4 A = armsM();
    for (const glm::mat4& g : m_G) out.push_back(glm::vec3(A * g[3]));
    return out;
}

void HoldPoseTool::solve() {
    if (!m_arms || m_base.empty()) return;
    m_poser.solve(m_base, m_edit, armsM(), weaponM(), m_G, m_short);
}

// --- Writing -------------------------------------------------------------------------------------

bool HoldPoseTool::write(std::string& err) {
    if (!m_arms || !m_rig || m_pose.empty() || m_G.empty()) {
        err = "Nothing to write: open arms and a pose first.";
        return false;
    }
    solve();
    // The pose into the arms' clip (the other clips stay as they are).
    const retarget::Clip clip = holdpose::clipOf("hold_" + m_pose, *m_arms, *m_rig, m_G);
    retarget::Repair keep;
    keep.mergeSkins = false;
    keep.bindLoose = false;
    backupOnce(m_armsPath);
    if (!retarget::writeModel(m_armsPath, m_armsPath, {clip}, keep, retarget::emptyMap(), err)) return false;
    // Where the arms sit, into the game's table.
    if (!holdsFile().empty()) {
        backupOnce(holdsFile());
        if (!holdpose::writePlacement(holdsFile(), m_pose, m_edit.place, err)) return false;
    }
    m_recipe.poses[m_pose] = m_weapon;
    holdpose::saveRecipe(m_armsPath, m_recipe);
    // The written pose is the new start: read the model again.
    const std::string pose = m_pose;
    openArms(m_armsPath);
    m_wantName = pose;
    m_wantPose = true;
    return true;
}

// --- Drawing ------------------------------------------------------------------------------------

void HoldPoseTool::setView(int v) {
    m_view = std::clamp(v, 0, 6);
    switch (m_view) {
        case 1: m_yaw = -90.0f; m_pitch = 5.0f; break;
        case 2: m_yaw = 90.0f; m_pitch = 5.0f; break;
        case 3: m_yaw = 0.0f; m_pitch = 89.0f; break;
        case 4: m_yaw = 0.0f; m_pitch = -89.0f; break;
        case 5: m_yaw = 180.0f; m_pitch = 5.0f; break;
        case 6: m_yaw = 135.0f; m_pitch = 20.0f; break;
        default: break;
    }
    m_redraw = true;
}

void HoldPoseTool::drawTopBar() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Arms");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300.0f);
    const std::string shown = m_armsPath.empty() ? std::string("Choose rigged arms...") : fileName(m_armsPath);
    if (ImGui::BeginCombo("##arms", shown.c_str())) {
        bool any = false;
        for (const ModelEntry& e : m_models) {
            if (!e.skinned) continue;
            any = true;
            if (ImGui::Selectable(e.name.c_str(), e.path == m_armsPath)) openArms(e.path);
        }
        if (!any) ImGui::TextDisabled("No rigged .glb in the project");
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Selected object") && m_d.selectedModel) {
        const std::string s = m_d.selectedModel();
        if (!s.empty()) openArms(s);
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) scanModels();

    ImGui::SameLine(0.0f, 24.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Pose");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo("##pose", m_pose.empty() ? "-" : m_pose.c_str())) {
        for (const std::string& n : poseNames())
            if (ImGui::Selectable(n.c_str(), n == m_pose)) selectPose(n);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("New pose...")) ImGui::OpenPopup("##newpose");
    if (ImGui::BeginPopup("##newpose")) {
        ImGui::TextUnformatted("Name (the clip becomes hold_<name>):");
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText("##nn", m_newName, sizeof m_newName);
        if (ImGui::Button("Create from the current pose") && m_newName[0]) {
            // The new pose starts as a copy of the one shown, on the same weapon.
            const holdpose::Weapon w = m_weapon;
            const holdpose::Edit e = m_edit;
            const std::vector<glm::mat4> base = m_G.empty() ? m_base : m_G;
            m_pose = m_newName;
            m_recipe.poses[m_pose] = w;
            m_weapon = w;
            m_base = base;
            m_edit = e;
            for (auto& h : m_edit.hands) h.curl = {}, h.swivel = 0.0f;
            m_undo.clear();
            m_dirty = true;
            m_havePlacement = false;
            m_redraw = true;
            m_newName[0] = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void HoldPoseTool::drawViewButtons() {
    static const char* names[] = {"Eye", "Left", "Right", "Top", "Below", "Front", "3/4"};
    for (int i = 0; i < 7; ++i) {
        if (i) ImGui::SameLine(0.0f, 4.0f);
        const bool on = m_view == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(names[i])) setView(i);
        if (on) ImGui::PopStyleColor();
    }
    ImGui::SameLine(0.0f, 16.0f);
    const bool orbit = m_view != 0;
    ImGui::BeginDisabled(!orbit);
    if (ImGui::ArrowButton("##l", ImGuiDir_Left)) { m_yaw -= 15.0f; m_redraw = true; }
    ImGui::SameLine(0.0f, 2.0f);
    if (ImGui::ArrowButton("##r", ImGuiDir_Right)) { m_yaw += 15.0f; m_redraw = true; }
    ImGui::SameLine(0.0f, 2.0f);
    if (ImGui::ArrowButton("##u", ImGuiDir_Up)) { m_pitch = std::min(89.0f, m_pitch + 15.0f); m_redraw = true; }
    ImGui::SameLine(0.0f, 2.0f);
    if (ImGui::ArrowButton("##d", ImGuiDir_Down)) { m_pitch = std::max(-89.0f, m_pitch - 15.0f); m_redraw = true; }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::Button("Zoom -")) { if (orbit) m_dist *= 1.25f; else m_eyeZoom = std::max(1.0f, m_eyeZoom / 1.25f); m_redraw = true; }
    ImGui::SameLine(0.0f, 2.0f);
    if (ImGui::Button("Zoom +")) { if (orbit) m_dist = std::max(0.15f, m_dist / 1.25f); else m_eyeZoom = std::min(6.0f, m_eyeZoom * 1.25f); m_redraw = true; }
    ImGui::SameLine(0.0f, 16.0f);
    if (ImGui::Checkbox("See-through weapon", &m_xrayGun)) m_redraw = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("See-through arms", &m_xrayArms)) m_redraw = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Palm marks", &m_markers)) m_redraw = true;
}

void HoldPoseTool::drawStudio(float w, float h) {
    if (!m_studioOk) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "The studio's shader did not compile (retargetpreview.*).");
        return;
    }
    if (m_shown[0] != m_gun.get()) { m_studio.setModel(0, m_gun.get()); m_shown[0] = m_gun.get(); m_redraw = true; }
    if (m_shown[1] != m_arms.get()) { m_studio.setModel(1, m_arms.get()); m_shown[1] = m_arms.get(); m_redraw = true; }
    const int pw = static_cast<int>(w * 1.25f), ph = static_cast<int>(h * 1.25f);
    if (m_redraw || m_tex == 0 || pw != m_studio.width() || ph != m_studio.height()) {
        solve();
        const glm::mat4 W = weaponM(), A = armsM();
        if (m_arms && !m_G.empty()) m_studio.setPose(1, ik::palette(m_G, m_arms->skeleton), A);
        if (m_gun) m_studio.setPose(0, {}, holdpose::weaponMatrix(m_weapon, m_gunCentre));
        std::vector<RetargetPreview::Segment> marks;
        if (m_markers && !m_G.empty())
            for (int side = 0; side < 2; ++side) {
                glm::vec3 c, f;
                if (!m_poser.hasArm(side) || !m_poser.palm(m_G, side, c, f)) continue;
                const glm::vec3 p = glm::vec3(A * glm::vec4(c, 1.0f));
                const ImVec4 k = kSideCol[side];
                RetargetPreview::Segment s;
                s.a = s.b = p;
                s.color = glm::vec4(k.x, k.y, k.z, side == m_side ? 1.0f : 0.55f);
                s.width = side == m_side ? 0.010f : 0.006f;
                marks.push_back(s);
            }
        m_studio.setBones(marks);
        RetargetPreview::View v;
        v.free = true;
        v.bones = true;
        v.floor = false;
        v.nearZ = 0.01f;
        v.farZ = 40.0f;
        v.alpha[0] = m_xrayGun ? 0.3f : 1.0f;
        v.alpha[1] = m_xrayArms ? 0.35f : 1.0f;
        if (m_view == 0) {
            // The player's eye, at the game's field of view (zoom narrows it).
            v.eye = glm::vec3(0.0f);
            v.at = glm::vec3(0.0f, 0.0f, -1.0f);
            const float t = std::tan(0.5f * m_recipe.fov * kDeg) / m_eyeZoom;
            v.fov = 2.0f * std::atan(t) / kDeg;
            if (m_eyeZoom > 1.0f) v.at = glm::normalize(glm::vec3(W[3]));   // zoomed: onto the weapon
        } else {
            const glm::vec3 target = glm::vec3(W[3]);
            const float cp = std::cos(m_pitch * kDeg), sp = std::sin(m_pitch * kDeg);
            v.eye = target + m_dist * glm::vec3(cp * std::sin(m_yaw * kDeg), sp, cp * std::cos(m_yaw * kDeg));
            v.at = target;
            v.up = std::abs(m_pitch) > 80.0f ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            v.fov = 40.0f;
        }
        m_tex = m_studio.render(pw, ph, v);
        m_redraw = false;
    }
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)m_tex, ImVec2(w, h), ImVec2(0, 1), ImVec2(1, 0));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto label = [&](float x, float y, const char* text, ImU32 col) {
        const ImVec2 sz = ImGui::CalcTextSize(text);
        dl->AddRectFilled(ImVec2(x - 4, y - 2), ImVec2(x + sz.x + 4, y + sz.y + 2), IM_COL32(20, 22, 26, 170), 4.0f);
        dl->AddText(ImVec2(x, y), col, text);
    };
    float ly = at.y + 8.0f;
    if (!m_arms) {
        label(at.x + 8.0f, ly, m_armsJob.valid() ? "Loading the arms..." : "Choose rigged arms above.", IM_COL32(240, 240, 245, 255));
        return;
    }
    if (m_gunJob.valid()) { label(at.x + 8.0f, ly, "Loading the weapon...", IM_COL32(240, 240, 245, 255)); ly += 24.0f; }
    else if (!m_gun) { label(at.x + 8.0f, ly, "No weapon model: choose one on the Weapon tab.", IM_COL32(255, 200, 120, 255)); ly += 24.0f; }
    for (int side = 0; side < 2; ++side)
        if (m_edit.hands[static_cast<std::size_t>(side)].on && m_short[side] > 0.005f) {
            char b[96];
            std::snprintf(b, sizeof b, "%s cannot reach: %.1f cm short -- move the shoulders closer (Arms tab)",
                          kSideName[side], m_short[side] * 100.0f);
            label(at.x + 8.0f, ly, b, IM_COL32(255, 170, 120, 255));
            ly += 24.0f;
        }
    if (m_placing) label(at.x + 8.0f, at.y + h - 30.0f, "Click the weapon where the palm should lie (Esc: cancel)",
                         IM_COL32(255, 230, 140, 255));
    const bool gizmoBusy = drawGizmo(at.x, at.y, w, h);
    if (hovered && !gizmoBusy && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec2 m = ImGui::GetMousePos();
        const float px = (m.x - at.x) * static_cast<float>(pw) / w, py = (m.y - at.y) * static_cast<float>(ph) / h;
        // A click on a palm mark picks that hand.
        int picked = -1;
        for (int side = 0; side < 2 && !m_G.empty(); ++side) {
            glm::vec3 c, f;
            if (!m_poser.hasArm(side) || !m_poser.palm(m_G, side, c, f)) continue;
            const glm::vec2 s = m_studio.project(glm::vec3(armsM() * glm::vec4(c, 1.0f)));
            if (glm::length(s - glm::vec2(px, py)) < 18.0f * static_cast<float>(pw) / w) picked = side;
        }
        if (picked >= 0 && !m_placing) {
            m_side = picked;
            m_redraw = true;
        } else if (m_placing) {
            if (placeHandAt(px, py)) m_placing = false;
        }
    }
    if (m_placing && ImGui::IsKeyPressed(ImGuiKey_Escape)) m_placing = false;
}

void HoldPoseTool::drawHandsTab() {
    for (int side = 0; side < 2; ++side) {
        if (side) ImGui::SameLine();
        ImGui::BeginDisabled(!m_poser.hasArm(side));
        const bool on = m_side == side;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? kSideCol[side] : ImGui::GetStyleColorVec4(ImGuiCol_Button));
        if (on) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.06f, 1.0f));
        if (ImGui::Button(kSideName[side], ImVec2(130.0f, 0.0f))) { m_side = side; m_redraw = true; }
        ImGui::PopStyleColor(on ? 2 : 1);
        ImGui::EndDisabled();
    }
    holdpose::Hand& h = m_edit.hands[static_cast<std::size_t>(m_side)];
    // The gizmo on the hand in the studio: what it does, and along which axes.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Gizmo");
    static const char* ops[] = {"Off", "Move", "Turn"};
    for (int i = 0; i < 3; ++i) {
        ImGui::SameLine(i == 0 ? 70.0f : 0.0f, 4.0f);
        const bool on = m_gizmoOp == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(ops[i])) m_gizmoOp = i;
        if (on) ImGui::PopStyleColor();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Axes");
    static const char* axes[] = {"Weapon", "Hand"};
    for (int i = 0; i < 2; ++i) {
        ImGui::SameLine(i == 0 ? 70.0f : 0.0f, 4.0f);
        const bool on = m_gizmoHandAxes == (i == 1);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(axes[i])) m_gizmoHandAxes = i == 1;
        if (on) ImGui::PopStyleColor();
    }
    ui::hint("Drag the gizmo in the studio; Ctrl: in the steps below.");
    bool reach = h.on;
    if (ImGui::Checkbox("On the weapon (the arm reaches to it)", &reach)) { change(); h.on = reach; }
    ImGui::Spacing();
    if (ImGui::Button(m_placing ? "Click the weapon now..." : "Place by a click on the weapon", ImVec2(-1.0f, 0.0f)))
        m_placing = !m_placing;

    ui::sectionText("Move (along the weapon)");
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("Step##mv", &m_moveStep, kMoveNames, 4);
    const float ms = kMoveSteps[m_moveStep];
    struct Ax { const char* name; glm::vec3 dir; };
    const Ax moves[] = {{"Right / left", {1, 0, 0}}, {"Up / down", {0, 1, 0}}, {"Back / ahead", {0, 0, 1}}};
    const float sw = ui::stepperWidth("%.1f cm");
    for (const Ax& a : moves) {
        ImGui::PushID(a.name);
        float cm = glm::dot(h.pos, a.dir) * 100.0f;
        const float was = cm;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(a.name);
        ImGui::SameLine(120.0f);
        if (ui::stepper("##v", cm, ms * 100.0f, -200.0f, 200.0f, "%.1f cm", sw)) moveHand(m_side, a.dir * ((cm - was) / 100.0f));
        ImGui::PopID();
    }

    ui::sectionText("Turn (round the palm)");
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("Step##tn", &m_turnStep, kTurnNames, 3);
    const float ts = kTurnSteps[m_turnStep];
    const Ax turns[] = {{"Roll (barrel axis)", {0, 0, 1}}, {"Tilt (side axis)", {1, 0, 0}}, {"Swing (up axis)", {0, 1, 0}}};
    for (const Ax& a : turns) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(a.name);
        ImGui::SameLine(140.0f);
        if (const int r = pair(a.name, 44.0f)) turnHand(m_side, a.dir, ts * static_cast<float>(r));
    }

    ui::sectionText("Elbow");
    {
        float sv = h.swivel;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Swing out / in");
        ImGui::SameLine(140.0f);
        if (ui::stepper("##sw", sv, 5.0f, -180.0f, 180.0f, "%.0f deg", sw)) { change(); h.swivel = sv; }
    }

    ui::sectionText("Fingers (on top of the clip)");
    for (int f = 0; f < holdpose::FingerCount; ++f) {
        ImGui::PushID(f);
        float c = h.curl[static_cast<std::size_t>(f)];
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(holdpose::fingerName(f));
        ImGui::SameLine(140.0f);
        if (ui::stepper("##c", c, 5.0f, -90.0f, 120.0f, "%.0f deg", sw)) { change(); h.curl[static_cast<std::size_t>(f)] = c; }
        ImGui::PopID();
    }
    if (ImGui::Button("Open all 5 deg")) { change(); for (float& c : h.curl) c = std::max(-90.0f, c - 5.0f); }
    ImGui::SameLine();
    if (ImGui::Button("Close all 5 deg")) { change(); for (float& c : h.curl) c = std::min(120.0f, c + 5.0f); }
    ImGui::Spacing();
    if (ImGui::Button("Back to the clip's hand", ImVec2(-1.0f, 0.0f)) && !m_base.empty()) {
        change();
        h = m_poser.handOf(m_base, m_side, armsM(), weaponM());
    }
}

void HoldPoseTool::drawArmsTab() {
    ui::hint("Where the arms' model sits on this weapon: moving it moves the shoulders --\n"
             "the hands stay on their grips. Written into the game's Lua table.");
    if (!m_havePlacement && !holdsFile().empty())
        ui::hint("The table has no entry for this pose yet: it gets one when written.");
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("Step##amv", &m_moveStep, kMoveNames, 4);
    const float ms = kMoveSteps[m_moveStep];
    const float sw = ui::stepperWidth("%.1f cm");
    const char* names[] = {"Right / left", "Up / down", "Back / ahead"};
    for (int k = 0; k < 3; ++k) {
        ImGui::PushID(k);
        float cm = m_edit.place.t[k] * 100.0f;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(names[k]);
        ImGui::SameLine(120.0f);
        if (ui::stepper("##t", cm, ms * 100.0f, -500.0f, 500.0f, "%.1f cm", sw)) { change(); m_edit.place.t[k] = cm / 100.0f; }
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("Step##atn", &m_turnStep, kTurnNames, 3);
    const float ts = kTurnSteps[m_turnStep];
    struct Ax { const char* name; glm::vec3 dir; };
    const Ax turns[] = {{"Pitch", {1, 0, 0}}, {"Yaw", {0, 1, 0}}, {"Roll", {0, 0, 1}}};
    for (const Ax& a : turns) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(a.name);
        ImGui::SameLine(140.0f);
        if (const int r = pair(a.name, 44.0f)) turnArms(a.dir, ts * static_cast<float>(r));
    }
    ImGui::Spacing();
    ui::sectionText("Table");
    const std::string hf = holdsFile();
    ImGui::TextWrapped("%s", hf.empty() ? "No Lua table set (Weapon tab)." : holdpose::relative(projectDir() + "/x", hf).c_str());
    if (!m_infoError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", m_infoError.c_str());
    else if (!hf.empty())
        ui::hint("Arms scale %.3f, centre %s", static_cast<double>(m_info.scale), m_info.haveCentre ? "from the table" : "the model's own");
}

void HoldPoseTool::drawWeaponTab() {
    ui::hint("How the game holds this weapon in front of the eye. It must match the game\n"
             "(DARKNESS: off / aim / scale in player.lua) -- here it only places the view.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Model");
    ImGui::SameLine(120.0f);
    ImGui::SetNextItemWidth(-1.0f);
    const std::string cur = m_weapon.model.empty() ? std::string("-") : m_weapon.model;
    if (ImGui::BeginCombo("##gun", cur.c_str())) {
        for (const ModelEntry& e : m_models) {
            if (e.skinned) continue;
            if (ImGui::Selectable(e.name.c_str(), holdpose::resolve(m_armsPath, m_weapon.model) == e.path)) {
                m_weapon.model = holdpose::relative(m_armsPath, e.path);
                m_weaponDirty = true;
                loadGun(e.path);
                m_redraw = true;
            }
        }
        ImGui::EndCombo();
    }
    const float sw = ui::stepperWidth("%.3f");
    auto row3 = [&](const char* name, glm::vec3& v, float step, float lo, float hi, const char* f) {
        ImGui::PushID(name);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(name);
        for (int k = 0; k < 3; ++k) {
            ImGui::SameLine(k == 0 ? 120.0f : 0.0f);
            ImGui::PushID(k);
            if (ui::stepper("##s", v[k], step, lo, hi, f, sw)) { m_weaponDirty = true; m_redraw = true; }
            ImGui::PopID();
        }
        ImGui::PopID();
    };
    row3("Off (m)", m_weapon.off, 0.005f, -2.0f, 2.0f, "%.3f");
    row3("Aim (deg)", m_weapon.aim, 1.0f, -180.0f, 180.0f, "%.1f");
    float s = m_weapon.scale.x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Scale");
    ImGui::SameLine(120.0f);
    if (ui::stepper("##sc", s, 0.01f, 0.01f, 10.0f, "%.2f", sw)) { m_weapon.scale = glm::vec3(s); m_weaponDirty = true; m_redraw = true; }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Eye FOV");
    ImGui::SameLine(120.0f);
    if (ui::stepper("##fov", m_recipe.fov, 1.0f, 20.0f, 120.0f, "%.0f deg", sw)) { m_weaponDirty = true; m_redraw = true; }

    ui::sectionText("Lua table");
    ImGui::TextWrapped("%s", m_recipe.holds.empty() ? "(none)" : m_recipe.holds.c_str());
    if (ImGui::BeginCombo("##holds", "Choose the .lua table...")) {
        std::error_code ec;
        const std::string root = projectDir();
        if (!root.empty())
            for (fs::recursive_directory_iterator it(fs::path(root), fs::directory_options::skip_permission_denied, ec), end;
                 it != end; it.increment(ec)) {
                if (ec) break;
                if (it->is_directory(ec)) {
                    const std::string n = it->path().filename().string();
                    if (n == "build" || (!n.empty() && n[0] == '.')) it.disable_recursion_pending();
                    continue;
                }
                if (it->path().extension() != ".lua") continue;
                const std::string p = it->path().generic_string();
                if (ImGui::Selectable(holdpose::relative(root + "/x", p).c_str())) {
                    m_recipe.holds = holdpose::relative(m_armsPath, p);
                    m_infoError.clear();
                    holdpose::readHoldsInfo(p, m_info, m_infoError);
                    m_weaponDirty = true;
                    startPose();
                }
            }
        ImGui::EndCombo();
    }
}

void HoldPoseTool::drawWriteBar() {
    ImGui::BeginDisabled(m_undo.empty());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_dirty && m_undo.empty());
    if (ImGui::Button("Back to what is written")) startPose();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 24.0f);
    const bool can = m_arms && !m_pose.empty() && !m_G.empty();
    ImGui::BeginDisabled(!can);
    const std::string label = "Write hold_" + (m_pose.empty() ? std::string("...") : m_pose);
    if (ImGui::Button(label.c_str(), ImVec2(220.0f, 0.0f))) {
        std::string err;
        if (write(err)) {
            m_message = "Written: hold_" + m_pose + " into " + fileName(m_armsPath) +
                        (holdsFile().empty() ? std::string() : " and its place into " + fileName(holdsFile()));
            m_messageBad = false;
            m_d.status = m_message;
        } else {
            m_message = err;
            m_messageBad = true;
        }
    }
    ImGui::EndDisabled();
    if (m_dirty || m_weaponDirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "changed");
    }
    if (!m_message.empty()) {
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextColored(m_messageBad ? ImVec4(1.0f, 0.55f, 0.45f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s",
                           m_message.c_str());
    }
}

void HoldPoseTool::panel(bool& show) {
    ImGui::SetNextWindowSize(ImVec2(m_firstW, m_firstH), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Hold Pose Studio", &show)) { ImGui::End(); return; }
    if (!m_studioReady) {
        m_studioReady = true;
        m_studioOk = m_studio.init();
    }
    if (m_modelsFor != projectDir()) scanModels();
    if (m_armsPath.empty()) {
        // Arms that already have a recipe open by themselves.
        for (const ModelEntry& e : m_models) {
            std::error_code ec;
            if (e.skinned && fs::exists(fs::path(holdpose::recipePath(e.path)), ec)) { openArms(e.path); break; }
        }
    }
    poll();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_Z))
        undo();
    drawTopBar();
    ImGui::Separator();
    const float barH = 40.0f;
    const float avail = std::max(240.0f, ImGui::GetContentRegionAvail().y - barH);
    const float sideW = 380.0f;
    ImGui::BeginChild("##studio", ImVec2(std::max(200.0f, ImGui::GetContentRegionAvail().x - sideW - 8.0f), avail));
    const float sw = ImGui::GetContentRegionAvail().x;
    const float viewH = ImGui::GetFrameHeightWithSpacing() * 1.1f;
    drawStudio(sw, std::max(160.0f, ImGui::GetContentRegionAvail().y - viewH));
    drawViewButtons();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##side", ImVec2(0.0f, avail), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Hands")) { drawHandsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Arms")) { drawArmsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Weapon")) { drawWeaponTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::Separator();
    drawWriteBar();
    ImGui::End();
}

} // namespace holdposeui
