#include "RetargetPanel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Blender.hpp"
#include "FolderDialog.hpp"
#include "UiStyle.hpp"

namespace fs = std::filesystem;
using namespace retarget;

namespace retargetui {

namespace {

const char* kSettings = "retarget.json";   // beside editor.json: the motion library folder

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string fileName(const std::string& p) { return fs::path(p).filename().string(); }

bool isMotionFile(const std::string& p) {
    const std::string e = lower(fs::path(p).extension().string());
    return e == ".glb" || e == ".gltf" || blender::canConvert(e);
}

glm::vec3 posOf(const glm::mat4& m) { return glm::vec3(m[3]); }

// `text` cut to `width` pixels with "..." -- a list row stays one line.
std::string fitted(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    std::string s = text;
    while (!s.empty() && ImGui::CalcTextSize((s + "...").c_str()).x > width) s.pop_back();
    return s + "...";
}

// The studio has no use for a 4K map on a figure 300 pixels tall: every colour
// map is halved until it fits 1024, the other maps are dropped. Done on the
// loader thread, so the upload is quick and the RAM stays small.
void shrinkForStudio(fitzel::ModelData& md) {
    struct Done { fitzel::SharedPixels px; int w, h; };
    std::map<const std::uint8_t*, Done> done;
    for (fitzel::ModelPrimitive& p : md.primitives) {
        p.normalPixels.reset();
        p.ormPixels.reset();
        p.emissionPixels.reset();
        if (p.texPixels.empty() || p.texWidth <= 0 || p.texHeight <= 0) continue;
        const auto it = done.find(p.texPixels.data());
        if (it != done.end()) {
            p.texPixels = it->second.px;
            p.texWidth = it->second.w;
            p.texHeight = it->second.h;
            continue;
        }
        const std::uint8_t* key = p.texPixels.data();
        int f = 1;
        while (p.texWidth / f > 1024 || p.texHeight / f > 1024) f *= 2;
        if (f > 1 && p.texPixels.size() >= static_cast<std::size_t>(p.texWidth) * p.texHeight * 4) {
            const int w = p.texWidth / f, h = p.texHeight / f;
            std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
            const std::uint8_t* in = p.texPixels.data();
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 4; ++c) {
                        unsigned sum = 0;
                        for (int dy = 0; dy < f; ++dy)
                            for (int dx = 0; dx < f; ++dx)
                                sum += in[(static_cast<std::size_t>(y * f + dy) * p.texWidth + (x * f + dx)) * 4 + c];
                        out[(static_cast<std::size_t>(y) * w + x) * 4 + c] = static_cast<std::uint8_t>(sum / (f * f));
                    }
            p.texPixels = fitzel::SharedPixels(std::move(out));
            p.texWidth = w;
            p.texHeight = h;
        }
        done[key] = {p.texPixels, p.texWidth, p.texHeight};
    }
}

// --- What the loader threads do ------------------------------------------------------

std::string studioCopyOf(const std::string& model) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "fitzel_retarget";
    fs::create_directories(dir, ec);
    return (dir / (fs::path(model).stem().string() + "-studio.glb")).generic_string();
}

RetargetTool::Loaded loadSource(const std::string& file) {
    RetargetTool::Loaded out;
    std::string glb = file;
    if (blender::needsBlender(file)) {
        const blender::Result r = blender::toGlb(file);
        if (!r.ok) { out.error = r.message; return out; }
        glb = r.glb;
        out.note = r.message;
    }
    auto rig = std::make_shared<Rig>(loadRig(glb));
    if (!rig->ok()) { out.error = rig->error; return out; }
    if (rig->motions.empty()) { out.error = fileName(file) + " has a skeleton but no animation."; return out; }
    out.rig = rig;
    auto md = std::make_shared<fitzel::ModelData>(fitzel::loadGltf(glb));
    shrinkForStudio(*md);
    out.model = md;
    if (out.note.empty()) out.note = "read directly";
    return out;
}

RetargetTool::Loaded loadCharacter(const std::string& model) {
    RetargetTool::Loaded out;
    const std::string name = fileName(model);
    loadRecipe(model, out.recipe);
    out.base = baseFor(model, out.recipe);
    out.info = inspect(out.base);
    if (!out.info.ok) { out.error = "Could not read " + name + "."; return out; }
    if (!out.info.binary) { out.error = name + " is a .gltf -- only a .glb can take new animations."; return out; }
    if (!out.info.skinned) { out.error = name + " has no skeleton."; return out; }
    auto rig = std::make_shared<Rig>(loadRig(out.base, false));
    if (!rig->ok()) { out.error = rig->error; return out; }
    out.rig = rig;
    // The studio shows the character as the bake will write it: repaired.
    std::string shown = out.base;
    if (out.info.needsRepair() && (out.recipe.repair.mergeSkins || out.recipe.repair.bindLoose)) {
        BoneMap tm = autoMap(*rig);
        applyNames(*rig, out.recipe.tgtMap, tm);
        const std::string copy = studioCopyOf(model);
        std::string err;
        if (writeModel(out.base, copy, {}, out.recipe.repair, tm, err)) shown = copy;
    }
    auto md = std::make_shared<fitzel::ModelData>(fitzel::loadGltf(shown));
    shrinkForStudio(*md);
    out.model = md;
    return out;
}

} // namespace

// --- Jobs ------------------------------------------------------------------------------

std::shared_ptr<RetargetTool::Job> RetargetTool::start(std::function<Loaded()> work) {
    auto job = std::make_shared<Job>();
    std::thread([job, work = std::move(work)] {
        Loaded r = work();
        std::lock_guard<std::mutex> lock(job->lock);
        job->result = std::move(r);
        job->done = true;
    }).detach();
    return job;
}

bool RetargetTool::take(const std::shared_ptr<Job>& job, Loaded& out) {
    if (!job) return false;
    std::lock_guard<std::mutex> lock(job->lock);
    if (!job->done) return false;
    out = std::move(job->result);
    return true;
}

// --- The tool --------------------------------------------------------------------------

RetargetTool::RetargetTool(Deps d) : m_d(std::move(d)) {
    std::ifstream f(kSettings);
    if (f) {
        const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_object()) m_library = j.value("library", std::string());
    }
    scanLibrary();
}

RetargetTool::~RetargetTool() {
    if (m_recipeDirty && !m_char.path.empty() && m_char.data.rig) saveRecipe(m_char.path, m_char.data.recipe);
}

std::string RetargetTool::projectDir() const {
    return m_d.currentProject.empty() ? std::string()
                                      : fs::path(m_d.currentProject).parent_path().generic_string();
}

void RetargetTool::scanModels() {
    m_modelsFor = projectDir();
    m_models.clear();
    if (m_modelsFor.empty()) return;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(m_modelsFor, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
        const fs::path& p = it->path();
        if (it->is_directory(ec)) {
            const std::string n = p.filename().string();
            if (!n.empty() && n[0] == '.') it.disable_recursion_pending();
            continue;
        }
        if (lower(p.extension().string()) != ".glb") continue;
        const ModelInfo info = inspect(p.generic_string());
        if (!info.skinned) continue;
        m_models.push_back({p.generic_string(), fs::relative(p, m_modelsFor, ec).generic_string()});
    }
    std::sort(m_models.begin(), m_models.end(), [](const ModelEntry& a, const ModelEntry& b) { return a.name < b.name; });
}

void RetargetTool::scanLibrary() {
    m_libraryFiles.clear();
    if (m_library.empty()) return;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(m_library, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(ec) && isMotionFile(it->path().string()))
            m_libraryFiles.push_back(it->path().generic_string());
    }
    std::sort(m_libraryFiles.begin(), m_libraryFiles.end(),
              [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
}

RetargetTool::Source* RetargetTool::sourceFor(const std::string& file) {
    if (file.empty()) return nullptr;
    auto it = m_sources.find(file);
    if (it != m_sources.end()) return it->second.get();
    auto s = std::make_unique<Source>();
    s->file = file;
    s->job = start([file] { return loadSource(file); });
    Source* raw = s.get();
    m_sources.emplace(file, std::move(s));
    return raw;
}

void RetargetTool::openCharacter(const std::string& modelRef) {
    const std::string model = modelRef;   // may be m_char.path itself, which is reset below
    if (model.empty()) return;
    if (m_recipeDirty && !m_char.path.empty() && m_char.data.rig) saveRecipe(m_char.path, m_char.data.recipe);
    m_recipeDirty = false;
    m_pending = false;
    m_char = Character{};
    m_char.path = fs::path(model).lexically_normal().generic_string();
    m_char.loading = true;
    m_char.job = start([path = m_char.path] { return loadCharacter(path); });
    m_sel = -1;
    m_audition.clear();
    m_tx = Transfer();
    m_txDirty = true;
    m_redraw = true;
    m_message.clear();
    if (m_studioOk) {
        m_studio.setModel(1, nullptr);
        m_studio.setModel(0, nullptr);
    }
    m_shown[0] = m_shown[1] = nullptr;
}

void RetargetTool::poll() {
    if (m_char.loading) {
        Loaded got;
        if (take(m_char.job, got)) {
            m_char.loading = false;
            m_char.job.reset();
            m_char.data = std::move(got);
            if (!m_char.data.error.empty()) {
                m_message = m_char.data.error;
                m_messageBad = true;
            } else {
                m_char.autoMap = autoMap(*m_char.data.rig);
                m_char.map = m_char.autoMap;
                applyNames(*m_char.data.rig, m_char.data.recipe.tgtMap, m_char.map);
                m_char.fileClips = inspect(m_char.path).clips;
                for (const ClipEntry& e : m_char.data.recipe.clips) sourceFor(sourceFile(m_char.path, e.source));
                if (!m_char.data.recipe.clips.empty()) selectClip(0);
            }
            m_txDirty = m_redraw = true;
        }
    }
    for (auto& [file, s] : m_sources) {
        if (!s->loading) continue;
        Loaded got;
        if (!take(s->job, got)) continue;
        s->loading = false;
        s->job.reset();
        s->data = std::move(got);
        if (file == currentFile()) m_txDirty = m_redraw = true;
    }
}

ClipEntry* RetargetTool::current() {
    std::vector<ClipEntry>& clips = m_char.data.recipe.clips;
    if (m_sel >= 0 && m_sel < static_cast<int>(clips.size())) return &clips[static_cast<std::size_t>(m_sel)];
    if (!m_audition.empty()) return &m_auditionEntry;
    return nullptr;
}

std::string RetargetTool::currentFile() {
    const ClipEntry* c = current();
    if (!c) return {};
    return m_sel >= 0 ? sourceFile(m_char.path, c->source) : m_audition;
}

void RetargetTool::selectClip(int i) {
    m_sel = i;
    m_audition.clear();
    m_time = 0.0f;
    m_txDirty = m_redraw = true;
    const ClipEntry* c = current();
    std::snprintf(m_nameBuf, sizeof m_nameBuf, "%s", c ? c->name.c_str() : "");
}

void RetargetTool::audition(const std::string& file, const std::string& take) {
    // An animation already on the character is just selected.
    const std::vector<ClipEntry>& clips = m_char.data.recipe.clips;
    for (std::size_t i = 0; i < clips.size(); ++i)
        if (sourceFile(m_char.path, clips[i].source) == file && (take.empty() || clips[i].take == take)) {
            selectClip(static_cast<int>(i));
            return;
        }
    m_sel = -1;
    m_audition = file;
    m_auditionEntry = ClipEntry{};
    m_auditionEntry.name = take.empty() ? clipNameFor(file) : clipNameForTake(take);
    m_auditionEntry.source = file;
    m_auditionEntry.take = take;
    m_time = 0.0f;
    m_txDirty = m_redraw = true;
    sourceFor(file);
}

void RetargetTool::addMotion(const std::string& file, const std::string& take, bool exactName) {
    if (!m_char.data.rig) {
        m_message = "Choose a character first -- the motions are added to it.";
        m_messageBad = true;
        return;
    }
    if (!isMotionFile(file)) {
        m_message = fileName(file) + " is not a motion file (glb, gltf, fbx, bvh, blend, dae, usd).";
        m_messageBad = true;
        return;
    }
    std::vector<ClipEntry>& clips = m_char.data.recipe.clips;
    ClipEntry e = file == m_audition && take == m_auditionEntry.take ? m_auditionEntry : ClipEntry{};
    e.take = take;
    // A clip copied from another character keeps its name exactly: an
    // animation graph asks for clips by name, and the one graph should play
    // on both characters. Motion files get a tidy name made of the file's.
    const std::string stem = take.empty() ? clipNameFor(file) : exactName ? take : clipNameForTake(take);
    std::string name = stem;
    for (int n = 2;; ++n) {
        bool taken = false;
        for (const ClipEntry& c : clips) taken = taken || c.name == name;
        if (!taken) break;
        name = stem + "_" + std::to_string(n);
    }
    e.name = name;
    e.source = sourceRef(m_char.path, projectDir(), file);
    clips.push_back(e);
    sourceFor(file);
    selectClip(static_cast<int>(clips.size()) - 1);
    recipeChanged();
    m_message = "Added " + name + ". Write to put it into " + fileName(m_char.path) + ".";
    m_messageBad = false;
}

void RetargetTool::addTakes(const std::string& file, const std::vector<std::string>& takes) {
    if (takes.empty()) return;
    for (const std::string& t : takes) addMotion(file, t, true);
    if (!m_messageBad) {
        m_message = "Added " + std::to_string(takes.size()) + " animation" + (takes.size() == 1 ? "" : "s") + " of " +
                    fileName(file) + ". Write to put them into " + fileName(m_char.path) + ".";
    }
}

void RetargetTool::copyFrom(const std::string& file) {
    m_copyFrom = file;
    m_copyPick.clear();
    m_showCharacters = true;
    sourceFor(file);
}

void RetargetTool::showAt(int clip, float seconds, int tab) {
    if (clip >= 0 && clip < static_cast<int>(m_char.data.recipe.clips.size())) selectClip(clip);
    m_time = std::max(0.0f, seconds);
    m_playing = false;
    m_tabRequest = tab;
    m_redraw = true;
}

bool RetargetTool::idle() const {
    if (m_char.loading) return false;
    for (const auto& [file, s] : m_sources)
        if (s->loading) return false;
    return true;
}

void RetargetTool::recipeChanged() {
    m_recipeDirty = true;
    m_pending = true;
    m_txDirty = true;
    m_redraw = true;
}

void RetargetTool::rebuildTransfer() {
    m_txDirty = false;
    m_tx = Transfer();
    m_txFor.clear();
    m_srcMap = emptyMap();
    ClipEntry* c = current();
    Source* s = c ? sourceFor(currentFile()) : nullptr;
    const fitzel::ModelData* left = s && !s->loading ? s->data.model.get() : nullptr;
    const fitzel::ModelData* right = m_char.data.model.get();
    if (m_studioOk) {
        if (m_shown[0] != left) { m_studio.setModel(0, left); m_shown[0] = left; }
        if (m_shown[1] != right) { m_studio.setModel(1, right); m_shown[1] = right; }
    }
    if (!c || !s || s->loading || !s->data.rig || !m_char.data.rig) return;
    m_srcMap = autoMap(*s->data.rig);
    applyNames(*s->data.rig, c->srcMap, m_srcMap);
    m_tx = Transfer(*s->data.rig, m_srcMap, *m_char.data.rig, m_char.map);
    m_txFor = currentFile();
}

void RetargetTool::write() {
    if (!m_char.data.rig) return;
    std::string msg;
    const auto src = [&](const ClipEntry& e) -> const Rig* {
        Source* s = sourceFor(sourceFile(m_char.path, e.source));
        return s && !s->loading ? s->data.rig.get() : nullptr;
    };
    const bool ok = bakeAll(m_char.path, m_char.data.recipe, src, msg);
    m_message = msg;
    m_messageBad = !ok;
    m_d.status = msg;
    if (ok) {
        m_pending = false;
        m_recipeDirty = false;
        m_char.fileClips = inspect(m_char.path).clips;
        m_char.data.base = baseFor(m_char.path, m_char.data.recipe);
    }
}
// --- The studio ----------------------------------------------------------------------------

namespace {

// The joint palette of `md` (loaded from the same file as `rig`) for node
// world matrices `W`; empty when the two do not line up.
std::vector<glm::mat4> paletteOf(const Rig& rig, const fitzel::ModelData& md, const std::vector<glm::mat4>& W) {
    if (md.skeleton.empty() || md.skeleton.size() != rig.skinJoints.size()) return {};
    std::vector<glm::mat4> pal(md.skeleton.size());
    for (std::size_t k = 0; k < pal.size(); ++k)
        pal[k] = W[static_cast<std::size_t>(rig.skinJoints[k])] * md.skeleton[k].inverseBind;
    return pal;
}

glm::vec4 groupColour(int group) {
    switch (group) {
        case GLeftArm: case GLeftLeg: case GLeftHand:    return {1.00f, 0.62f, 0.22f, 1.0f};
        case GRightArm: case GRightLeg: case GRightHand: return {0.30f, 0.76f, 1.00f, 1.0f};
        default:                                         return {0.96f, 0.96f, 1.00f, 1.0f};
    }
}

void addBones(std::vector<RetargetPreview::Segment>& out, const BoneMap& m, const BoneMap& other,
              const std::vector<glm::mat4>& W, const glm::mat4& place, float width, int hot) {
    for (int s = 0; s < SlotCount; ++s) {
        const int n = m[static_cast<std::size_t>(s)];
        if (n < 0 || n >= static_cast<int>(W.size())) continue;
        int p = slotInfo(s).parent;
        while (p >= 0 && m[static_cast<std::size_t>(p)] < 0) p = slotInfo(p).parent;
        if (p < 0) continue;
        RetargetPreview::Segment seg;
        seg.a = glm::vec3(place * glm::vec4(posOf(W[static_cast<std::size_t>(m[static_cast<std::size_t>(p)])]), 1.0f));
        seg.b = glm::vec3(place * glm::vec4(posOf(W[static_cast<std::size_t>(n)]), 1.0f));
        seg.color = groupColour(slotInfo(s).group);
        seg.width = s >= LThumb1 ? width * 0.6f : width;
        if (other[static_cast<std::size_t>(s)] < 0) seg.color = {0.55f, 0.55f, 0.58f, 0.8f};   // not carried over
        if (s == hot) { seg.color = {1.0f, 0.92f, 0.15f, 1.0f}; seg.width = width * 2.0f; }
        out.push_back(seg);
    }
}

// A mannequin for a motion file without a body: rounded limbs between the
// mapped joints, sized after a figure `H` tall (studio space).
std::vector<RetargetPreview::Capsule> mannequinOf(const BoneMap& m, const std::vector<glm::mat4>& W,
                                                  const glm::mat4& place, float H) {
    auto at = [&](int s, glm::vec3& p) {
        const int n = m[static_cast<std::size_t>(s)];
        if (n < 0 || n >= static_cast<int>(W.size())) return false;
        p = glm::vec3(place * glm::vec4(posOf(W[static_cast<std::size_t>(n)]), 1.0f));
        return true;
    };
    auto radius = [&](int s) {
        switch (s) {
            case Pelvis: return 0.06f;
            case Spine: return 0.07f;
            case Chest: return 0.075f;
            case UpperChest: return 0.08f;
            case Neck: case UpperNeck: case Head: return 0.03f;
            case LShoulder: case RShoulder: return 0.035f;
            case LUpperArm: case RUpperArm: case LLowerArm: case RLowerArm: return 0.03f;
            case LHand: case RHand: return 0.025f;
            case LUpperLeg: case RUpperLeg: return 0.05f;
            case LLowerLeg: case RLowerLeg: return 0.045f;
            case LFoot: case RFoot: return 0.035f;
            case LToes: case RToes: return 0.025f;
            default: return 0.008f;   // fingers
        }
    };
    std::vector<RetargetPreview::Capsule> caps;
    for (int s = 0; s < SlotCount; ++s) {
        glm::vec3 b;
        if (!at(s, b)) continue;
        int p = slotInfo(s).parent;
        glm::vec3 a;
        while (p >= 0 && !at(p, a)) p = slotInfo(p).parent;
        if (p < 0) continue;
        caps.push_back({a, b, radius(s) * H});
    }
    // The head: a ball over the top of the neck.
    glm::vec3 head, neck;
    if (at(Head, head)) {
        const bool haveNeck = at(UpperNeck, neck) || at(Neck, neck) || at(UpperChest, neck);
        const glm::vec3 up = haveNeck && glm::length(head - neck) > 1e-4f ? glm::normalize(head - neck)
                                                                          : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 c = head + up * (0.055f * H);
        caps.push_back({c, c, 0.065f * H});
    }
    return caps;
}

} // namespace

void RetargetTool::updatePose() {
    if (!m_studioOk) return;
    const Rig* tr = m_char.data.rig.get();
    const fitzel::ModelData* tmd = m_char.data.model.get();
    if (!tr || !tmd) { m_studio.setBones({}); return; }
    const float kD = 3.14159265f / 180.0f;
    const float height = std::max(tmd->height(), 0.5f);
    const glm::vec3 fT = facing(*tr, m_char.map);
    const glm::mat4 turnRot = glm::rotate(glm::mat4(1.0f), m_turn * kD - std::atan2(fT.x, fT.z), glm::vec3(0, 1, 0));
    const bool two = m_tx.valid();
    const float off = two ? 0.5f * height : 0.0f;
    const float width = 0.012f * height;
    std::vector<glm::mat4> tw, sw;
    std::vector<RetargetPreview::Segment> segs;
    if (two) {
        ClipEntry* c = current();
        Source* s = sourceFor(m_txFor);
        const Rig& sr = *s->data.rig;
        const int mi = motionIndex(sr, c->take);
        Options o;
        o.start = c->start;
        o.end = c->end;
        o.inPlace = c->inPlace;
        float a = 0.0f, b = 0.0f;
        clipRange(sr, mi, o, a, b);
        const float t = a + std::clamp(m_time, 0.0f, b - a);
        sr.sample(mi, t, sw);
        m_tx.pose(sw, m_tx.drift(sr, mi, o, t), tw);
        const glm::mat4& A = m_tx.facing();
        const int sh = m_tx.srcHipNode();
        glm::vec3 hold = glm::vec3(A * glm::vec4(posOf(m_hold ? sw[static_cast<std::size_t>(sh)]
                                                              : sr.nodes[static_cast<std::size_t>(sh)].bind), 1.0f));
        hold.y = 0.0f;
        const glm::mat4 place0 = glm::translate(glm::mat4(1.0f), glm::vec3(-off, 0.0f, 0.0f)) * turnRot *
                                 glm::scale(glm::mat4(1.0f), glm::vec3(m_tx.hipScale())) *
                                 glm::translate(glm::mat4(1.0f), -hold) * A;
        const fitzel::ModelData* smd = s->data.model.get();
        if (smd) m_studio.setPose(0, paletteOf(sr, *smd, sw), place0);
        bool body = false;
        if (smd)
            for (const fitzel::ModelPrimitive& prim : smd->primitives) body = body || !prim.skin.empty();
        if (!body) m_studio.setMannequin(0, mannequinOf(m_srcMap, sw, place0, height));
        addBones(segs, m_srcMap, m_char.map, sw, place0, width, m_hot);
    } else {
        tr->sample(-1, 0.0f, tw);
    }
    const int th = m_char.map[Hips];
    glm::vec3 hold(0.0f);
    if (th >= 0) hold = posOf(m_hold ? tw[static_cast<std::size_t>(th)] : tr->nodes[static_cast<std::size_t>(th)].bind);
    hold.y = 0.0f;
    const glm::mat4 place1 = glm::translate(glm::mat4(1.0f), glm::vec3(off, 0.0f, 0.0f)) * turnRot *
                             glm::translate(glm::mat4(1.0f), -hold);
    m_studio.setPose(1, paletteOf(*tr, *tmd, tw), place1);
    addBones(segs, m_char.map, two ? m_srcMap : m_char.map, tw, place1, width, m_hot);
    m_studio.setBones(std::move(segs));
}

bool RetargetTool::clipSpan(float& len, float& fps) {
    len = 0.0f;
    fps = 30.0f;
    if (!m_tx.valid()) return false;
    const ClipEntry* c = current();
    Source* s = sourceFor(m_txFor);
    if (!c || !s || !s->data.rig) return false;
    const int mi = motionIndex(*s->data.rig, c->take);
    if (mi < 0) return false;
    Options o;
    o.start = c->start;
    o.end = c->end;
    float a = 0.0f, b = 0.0f;
    clipRange(*s->data.rig, mi, o, a, b);
    len = b - a;
    fps = s->data.rig->motions[static_cast<std::size_t>(mi)].fps;
    return true;
}

void RetargetTool::drawStudio(float w, float h) {
    if (!m_studioOk) {
        ImGui::Dummy(ImVec2(w, 40.0f));
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "The studio's shader did not compile (retargetpreview.*).");
        return;
    }
    if (m_txDirty) rebuildTransfer();
    // Play: time runs over the clip and wraps.
    float len = 0.0f, fps = 30.0f;
    clipSpan(len, fps);
    if (m_playing && len > 0.0f) {
        m_time += ImGui::GetIO().DeltaTime * m_speed;
        if (m_time > len) m_time = std::fmod(m_time, len);
        m_redraw = true;
    }
    const fitzel::ModelData* tmd = m_char.data.model.get();
    const float height = tmd ? std::max(tmd->height(), 0.5f) : 1.7f;
    const int pw = static_cast<int>(w * 1.25f), ph = static_cast<int>(h * 1.25f);
    if (m_redraw || m_tex == 0 || pw != m_studio.width() || ph != m_studio.height()) {
        updatePose();
        RetargetPreview::View v;
        const bool two = m_tx.valid();
        const float spanW = (two ? 2.0f : 1.0f) * height + 0.4f;
        const float aspect = w / std::max(h, 1.0f);
        const float t15 = std::tan(15.0f * 3.14159265f / 180.0f);
        v.target = glm::vec3(0.0f, 0.5f * height, 0.0f);
        v.dist = std::max(0.62f * height / t15, 0.55f * spanW / (t15 * aspect)) * m_zoom;
        v.pitch = 8.0f;
        v.floorR = std::max(4.0f, spanW * 1.6f);
        v.bones = m_bones;
        m_tex = m_studio.render(pw, ph, v);
        m_redraw = false;
    }
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)m_tex, ImVec2(w, h), ImVec2(0, 1), ImVec2(1, 0));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto label = [&](float x, float y, const char* text, bool right) {
        const ImVec2 sz = ImGui::CalcTextSize(text);
        const float px = right ? x - sz.x - 8.0f : x + 8.0f;
        dl->AddRectFilled(ImVec2(px - 4, y - 2), ImVec2(px + sz.x + 4, y + sz.y + 2), IM_COL32(20, 22, 26, 150), 4.0f);
        dl->AddText(ImVec2(px, y), IM_COL32(240, 240, 245, 255), text);
    };
    if (!m_char.data.rig) {
        const char* msg = m_char.loading ? "Loading the character..."
                                         : "Choose a character above -- a rigged .glb from the project.";
        label(at.x + 4.0f, at.y + 8.0f, msg, false);
        return;
    }
    const std::string right = "Character: " + fileName(m_char.path);
    label(at.x + w - 4.0f, at.y + 8.0f, right.c_str(), true);
    const ClipEntry* c = current();
    if (!c) {
        label(at.x + 4.0f, at.y + 8.0f, "Add a motion on the left -- or click one in the library.", false);
        return;
    }
    Source* s = sourceFor(currentFile());
    std::string left = "Motion: " + fileName(currentFile());
    if (!c->take.empty() && m_char.data.rig) left += " - " + clipNameForTake(c->take);
    if (s && s->loading)
        left += blender::needsBlender(s->file) ? "  (Blender is converting it...)" : "  (loading...)";
    else if (s && !s->data.error.empty()) left += "  -- " + s->data.error;
    else if (!m_tx.valid() && !m_tx.problem().empty()) left += "  -- " + m_tx.problem();
    label(at.x + 4.0f, at.y + 8.0f, left.c_str(), false);
    if (m_tx.valid()) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%.2f / %.2f s   frame %d", m_time, len, static_cast<int>(std::lround(m_time * fps)));
        label(at.x + 4.0f, at.y + h - 26.0f, buf, false);
        if (std::abs(m_tx.hipScale() - 1.0f) > 0.02f) {
            std::snprintf(buf, sizeof buf, "motion shown at the character's size (x%.2f)", m_tx.hipScale());
            label(at.x + w - 4.0f, at.y + h - 26.0f, buf, true);
        }
    }
}

void RetargetTool::drawTransport() {
    const float bh = 30.0f;
    float len = 0.0f, fps = 30.0f;
    clipSpan(len, fps);
    auto btn = [&](const char* label, float wd, const char* tip) {
        const bool hit = ImGui::Button(label, ImVec2(wd, bh));
        ImGui::SetItemTooltip("%s", tip);
        ImGui::SameLine();
        return hit;
    };
    ImGui::BeginDisabled(len <= 0.0f);
    if (btn("|<", 36.0f, "To the start.")) { m_time = 0.0f; m_redraw = true; }
    if (btn("<", 36.0f, "One frame back.")) { m_playing = false; m_time = std::max(0.0f, m_time - 1.0f / fps); m_redraw = true; }
    if (btn(m_playing ? "Pause" : "Play", 70.0f, "Play or hold the motion.")) m_playing = !m_playing;
    if (btn(">", 36.0f, "One frame on.")) { m_playing = false; m_time = std::min(len, m_time + 1.0f / fps); m_redraw = true; }
    if (btn(">|", 36.0f, "To the end.")) { m_playing = false; m_time = len; m_redraw = true; }
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - 2.0f * 52.0f - 2.0f * sp));
    if (ImGui::SliderFloat("##time", &m_time, 0.0f, std::max(len, 0.001f), "%.2f s")) m_redraw = true;
    ImGui::SetItemTooltip("Click anywhere on the bar to jump there.");
    ImGui::SameLine();
    for (const float speed : {0.5f, 1.0f}) {
        const bool on = std::abs(m_speed - speed) < 0.01f;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(speed < 1.0f ? "Slow" : "1x", ImVec2(52.0f, bh))) m_speed = speed;
        ImGui::SetItemTooltip(speed < 1.0f ? "Half speed." : "Real speed.");
        if (on) ImGui::PopStyleColor();
        if (speed < 1.0f) ImGui::SameLine();
    }
    ImGui::EndDisabled();

    // The view, by buttons.
    if (btn("Turn <", 64.0f, "Turn both figures to the left.")) { m_turn -= 45.0f; m_redraw = true; }
    if (btn("Turn >", 64.0f, "Turn both figures to the right.")) { m_turn += 45.0f; m_redraw = true; }
    if (btn("Front", 56.0f, "Both face you.")) { m_turn = 0.0f; m_redraw = true; }
    if (btn("Side", 56.0f, "Both seen from their left side.")) { m_turn = 90.0f; m_redraw = true; }
    if (btn("Closer", 60.0f, "Step closer.")) { m_zoom = std::max(0.35f, m_zoom * 0.8f); m_redraw = true; }
    if (btn("Farther", 64.0f, "Step back.")) { m_zoom = std::min(2.5f, m_zoom / 0.8f); m_redraw = true; }
    if (ImGui::Checkbox("Bones", &m_bones)) m_redraw = true;
    ImGui::SetItemTooltip("Draw the mapped bones over both figures: orange left, blue right, grey = only on one side.");
    ImGui::SameLine();
    if (ImGui::Checkbox("Hold in place", &m_hold)) m_redraw = true;
    ImGui::SetItemTooltip("Keep each figure over its spot even when the motion walks off\n"
                          "(only here -- the clip keeps its travel unless 'In place' is set).");
}
// --- The window ---------------------------------------------------------------------------

void RetargetTool::drawTopBar() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Character");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300.0f);
    const std::string shown = m_char.path.empty() ? std::string("Choose a character...") : fileName(m_char.path);
    if (ImGui::BeginCombo("##character", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (m_models.empty())
            ui::hint(projectDir().empty() ? "Open a project first." : "No rigged .glb in this project.");
        for (const ModelEntry& m : m_models)
            if (ImGui::Selectable(m.name.c_str(), m.path == m_char.path)) openCharacter(m.path);
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Every .glb with a skeleton in the open project.");
    ImGui::SameLine();
    if (ImGui::Button("Use selected", ImVec2(0.0f, 0.0f))) {
        const std::string p = m_d.selectedModel ? m_d.selectedModel() : std::string();
        if (p.empty()) {
            m_message = "Select an object with a character model in the scene first.";
            m_messageBad = true;
        } else if (!inspect(p).skinned) {
            m_message = fileName(p) + " has no skeleton.";
            m_messageBad = true;
        } else {
            openCharacter(p);
        }
    }
    ImGui::SetItemTooltip("Take the model of the object selected in the scene.");
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) { scanModels(); scanLibrary(); }
    ImGui::SetItemTooltip("Look for characters and library motions again.");

    // Blender, on the right: only needed for FBX, BVH and .blend.
    const blender::Install& bl = blender::find();
    const std::string blText = bl.exe.empty() ? std::string("Blender: not found") : "Blender " + bl.version;
    const float bw = ImGui::CalcTextSize(blText.c_str()).x + (bl.exe.empty() ? 80.0f : 70.0f);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - bw));
    ImGui::AlignTextToFramePadding();
    if (bl.exe.empty()) ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.3f, 1.0f), "%s", blText.c_str());
    else ui::hint("%s", blText.c_str());
    ImGui::SetItemTooltip("%s", bl.exe.empty()
                                    ? "Needed only for FBX, BVH and .blend motions (glTF works without).\n"
                                      "Install it from blender.org, or point to blender.exe."
                                    : (bl.exe + (bl.chosen ? "\n(chosen by hand)" : "\n(found automatically)")).c_str());
    ImGui::SameLine();
    if (ImGui::Button(bl.exe.empty() ? "Locate..." : "Change")) {
        std::string exe;
        if (ed::pickFile(exe, bl.exe.empty() ? std::string("C:/Program Files") : fs::path(bl.exe).parent_path().string(),
                         "Blender", "blender.exe;blender"))
            blender::choose(exe);
    }
}

void RetargetTool::drawClipList(float height) {
    ui::sectionText("Animations");
    std::vector<ClipEntry>& clips = m_char.data.recipe.clips;
    ImGui::BeginChild("##clips", ImVec2(0.0f, height), ImGuiChildFlags_Borders);
    if (!m_char.data.rig) ui::hint("No character yet.");
    else if (clips.empty()) ui::hint("None yet: add a motion below,\nor drop files on this window.");
    const float lineH = ImGui::GetTextLineHeight();
    for (int i = 0; i < static_cast<int>(clips.size()); ++i) {
        const ClipEntry& c = clips[static_cast<std::size_t>(i)];
        const std::string file = sourceFile(m_char.path, c.source);
        Source* s = sourceFor(file);
        if (!s) continue;
        const bool inFile = std::find(m_char.fileClips.begin(), m_char.fileClips.end(), c.name) != m_char.fileClips.end();
        std::string state;
        if (!c.take.empty() && clipNameForTake(c.take) != c.name) state = "(" + clipNameForTake(c.take) + ")";
        if (s->loading) state = blender::needsBlender(file) ? "converting..." : "loading...";
        else if (!s->data.error.empty()) state = "not readable";
        else if (!inFile) state = "not written yet";
        const float rowW = ImGui::GetContentRegionAvail().x - 12.0f;
        const std::string sub = state.empty() ? fitted(fileName(file), rowW)
                                              : fitted(fileName(file), rowW - ImGui::CalcTextSize(state.c_str()).x - 16.0f) +
                                                    "  " + state;
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorPos();
        if (ImGui::Selectable("##row", m_sel == i, ImGuiSelectableFlags_None, ImVec2(0.0f, lineH * 2.0f + 6.0f)))
            selectClip(i);
        if (!s->data.error.empty()) ImGui::SetItemTooltip("%s", s->data.error.c_str());
        ImGui::SetCursorPos(ImVec2(p.x + 6.0f, p.y + 2.0f));
        const std::string title = fitted(c.name, rowW);
        if (!s->data.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", title.c_str());
        else ImGui::TextUnformatted(title.c_str());
        ImGui::SetCursorPos(ImVec2(p.x + 6.0f, p.y + 2.0f + lineH));
        ui::hint("%s", sub.c_str());
        ImGui::SetCursorPos(ImVec2(p.x, p.y + lineH * 2.0f + 6.0f + ImGui::GetStyle().ItemSpacing.y));
        ImGui::PopID();
    }
    // What the character came with -- kept by every write.
    std::vector<std::string> originals;
    for (const std::string& n : m_char.data.info.clips) {
        bool mine = false;
        for (const ClipEntry& c : clips) mine = mine || c.name == n;
        if (!mine) originals.push_back(n);
    }
    if (!originals.empty()) {
        ImGui::Spacing();
        ui::hint("Already in the character:");
        const float w = ImGui::GetContentRegionAvail().x - 16.0f;
        for (const std::string& n : originals) {
            ui::hint("   %s", fitted(n, w).c_str());
            ImGui::SetItemTooltip("%s", n.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(!m_char.data.rig);
    if (ImGui::Button("+ Add motion file...", ImVec2(-1.0f, 34.0f))) {
        std::string file;
        const std::string from = m_library.empty() ? projectDir() : m_library;
        if (ed::pickFile(file, from, "Motions", "*.fbx;*.glb;*.gltf;*.bvh;*.blend;*.dae")) addMotion(file);
    }
    ImGui::SetItemTooltip("FBX (Mixamo, ActorCore ...), glTF, BVH or a .blend -- or drop files on this window.");
    ImGui::EndDisabled();
}

void RetargetTool::drawLibrary() {
    if (m_library.empty()) ui::hint("A folder of motion files, to pick from.");
    else ui::hint("%s", m_library.c_str());
    if (ImGui::Button(m_library.empty() ? "Choose folder..." : "Other folder...", ImVec2(-1.0f, 0.0f))) {
        std::string dir;
        if (ed::pickFolder(dir, m_library)) {
            m_library = dir;
            scanLibrary();
            nlohmann::json j = {{"library", m_library}};
            std::ofstream f(kSettings, std::ios::trunc);
            f << j.dump(2) << "\n";
        }
    }
    if (m_libraryFiles.empty()) return;
    ui::searchBox("##libsearch", m_libSearch, sizeof m_libSearch, "Search motions...");
    ImGui::BeginChild("##lib", ImVec2(0.0f, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_Borders);
    const std::vector<ClipEntry>& clips = m_char.data.recipe.clips;
    int shownCount = 0;
    for (const std::string& f : m_libraryFiles) {
        const std::string name = fs::path(f).stem().string();
        if (!ui::icontains(name.c_str(), m_libSearch)) continue;
        ++shownCount;
        bool added = false;
        for (const ClipEntry& c : clips) added = added || sourceFile(m_char.path, c.source) == f;
        ImGui::PushID(f.c_str());
        const float addW = 30.0f;
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::Selectable(name.c_str(), m_audition == f, ImGuiSelectableFlags_None,
                              ImVec2(ImGui::GetContentRegionAvail().x - addW - 4.0f, 0.0f)))
            audition(f);
        ImGui::SetItemTooltip("%s\nClick to watch it on the character first.", f.c_str());
        ImGui::SameLine(ImGui::GetContentRegionMax().x - addW);
        ImGui::BeginDisabled(added || !m_char.data.rig);
        if (ImGui::Button(added ? "ok" : "+", ImVec2(addW, 0.0f))) addMotion(f);
        ImGui::SetItemTooltip(added ? "Already added." : "Add it to the character.");
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (shownCount == 0) ui::hint("Nothing matches.");
    ImGui::EndChild();
}
void RetargetTool::drawCharacters() {
    ui::hint("Copy animations another character already has.");
    const std::string shown = m_copyFrom.empty() ? std::string("Choose a character...") : fileName(m_copyFrom);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##copyfrom", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
        int others = 0;
        for (const ModelEntry& m : m_models) {
            if (m.path == m_char.path) continue;
            ++others;
            if (ImGui::Selectable(m.name.c_str(), m.path == m_copyFrom)) copyFrom(m.path);
        }
        if (others == 0) ui::hint(projectDir().empty() ? "Open a project first." : "No other rigged .glb in this project.");
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Every other .glb with a skeleton in the open project.");
    if (ImGui::Button("Other file...", ImVec2(-1.0f, 0.0f))) {
        std::string file;
        const std::string from = m_copyFrom.empty() ? projectDir() : fs::path(m_copyFrom).parent_path().string();
        if (ed::pickFile(file, from, "Characters", "*.glb;*.gltf;*.fbx;*.blend")) copyFrom(file);
    }
    ImGui::SetItemTooltip("A character from anywhere: glTF, FBX or .blend.");
    if (m_copyFrom.empty()) return;
    Source* s = sourceFor(m_copyFrom);
    if (s->loading) {
        ui::hint(blender::needsBlender(m_copyFrom) ? "Blender is converting it..." : "Loading...");
        return;
    }
    if (!s->data.error.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", s->data.error.c_str());
        ImGui::PopTextWrapPos();
        return;
    }
    const Rig& r = *s->data.rig;
    // Only animations that move the skeleton (a file from Blender also has one
    // per mesh, for its shape keys).
    std::vector<int> takes;
    for (std::size_t i = 0; i < r.motions.size(); ++i) {
        int joints = 0;
        for (const Motion::Channel& c : r.motions[i].channels)
            joints += c.node >= 0 && r.nodes[static_cast<std::size_t>(c.node)].joint ? 1 : 0;
        if (joints > 0) takes.push_back(static_cast<int>(i));
    }
    if (takes.empty()) { ui::hint("It has no animations."); return; }
    const std::vector<ClipEntry>& clips = m_char.data.recipe.clips;
    auto added = [&](const std::string& take) {
        for (const ClipEntry& c : clips)
            if (c.take == take && sourceFile(m_char.path, c.source) == m_copyFrom) return true;
        return false;
    };
    auto picked = [&](const std::string& take) {
        return std::find(m_copyPick.begin(), m_copyPick.end(), take) != m_copyPick.end();
    };
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("All", ImVec2(half, 0.0f))) {
        m_copyPick.clear();
        for (int i : takes)
            if (!added(r.motions[static_cast<std::size_t>(i)].name)) m_copyPick.push_back(r.motions[static_cast<std::size_t>(i)].name);
    }
    ImGui::SameLine();
    if (ImGui::Button("None", ImVec2(half, 0.0f))) m_copyPick.clear();
    const float listH = std::max(80.0f, ImGui::GetContentRegionAvail().y - 44.0f);
    ImGui::BeginChild("##takes", ImVec2(0.0f, listH), ImGuiChildFlags_Borders);
    for (int i : takes) {
        const Motion& mo = r.motions[static_cast<std::size_t>(i)];
        const bool done = added(mo.name);
        bool on = done || picked(mo.name);
        ImGui::PushID(i);
        ImGui::BeginDisabled(done);
        if (ImGui::Checkbox("##pick", &on)) {
            if (on) m_copyPick.push_back(mo.name);
            else m_copyPick.erase(std::remove(m_copyPick.begin(), m_copyPick.end(), mo.name), m_copyPick.end());
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        char len[32];
        if (mo.duration > 0.0f) std::snprintf(len, sizeof len, "%.1f s", mo.duration);
        else std::snprintf(len, sizeof len, "pose");
        const float lenW = ImGui::CalcTextSize(len).x;
        const std::string label = fitted(clipNameForTake(mo.name), ImGui::GetContentRegionAvail().x - lenW - 16.0f);
        const bool watching = m_audition == m_copyFrom && m_auditionEntry.take == mo.name;
        if (ImGui::Selectable(label.c_str(), watching)) audition(m_copyFrom, mo.name);
        ImGui::SetItemTooltip("%s%s\nClick to watch it on your character.", mo.name.c_str(),
                              done ? "\n(already added)" : "");
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - lenW - 4.0f);
        ui::hint("%s", len);
        ImGui::PopID();
    }
    ImGui::EndChild();
    int count = 0;
    for (const std::string& t : m_copyPick) count += added(t) ? 0 : 1;
    char label[64];
    std::snprintf(label, sizeof label, count ? "Add %d animation%s" : "Tick the animations to add", count,
                  count == 1 ? "" : "s");
    ImGui::BeginDisabled(count == 0 || !m_char.data.rig);
    if (ImGui::Button(label, ImVec2(-1.0f, 34.0f))) {
        std::vector<std::string> todo;
        for (int i : takes) {   // in the file's order
            const std::string& n = r.motions[static_cast<std::size_t>(i)].name;
            if (picked(n) && !added(n)) todo.push_back(n);
        }
        m_copyPick.clear();
        addTakes(m_copyFrom, todo);
    }
    ImGui::EndDisabled();
}

// --- The tabs under the studio ---------------------------------------------------------------

namespace {
constexpr float kLabelW = 130.0f;

void rowLabel(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(kLabelW);
}
} // namespace

void RetargetTool::drawClipTab() {
    ClipEntry* c = current();
    if (!c) { ui::hint("Pick an animation on the left -- or a motion in the library to watch it first."); return; }
    const std::string file = currentFile();
    Source* s = sourceFor(file);
    const bool audition = m_sel < 0;
    if (audition) {
        ImGui::TextWrapped("Watching %s -- it is not on the character yet.", fileName(file).c_str());
        ImGui::BeginDisabled(!m_char.data.rig);
        if (ImGui::Button("+ Add it to the character", ImVec2(-1.0f, 34.0f))) {
            addMotion(file, c->take);
            ImGui::EndDisabled();
            return;
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
    } else {
        rowLabel("Name");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##name", m_nameBuf, sizeof m_nameBuf);
        ImGui::SetItemTooltip("What the clip is called in the model -- the name an Animation or a graph state plays.");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            // Any name: it has to match what an animation graph asks for,
            // dashes and bars included. Only blanks at the ends go.
            std::string want = m_nameBuf;
            while (!want.empty() && std::isspace(static_cast<unsigned char>(want.back()))) want.pop_back();
            while (!want.empty() && std::isspace(static_cast<unsigned char>(want.front()))) want.erase(want.begin());
            if (want.empty()) want = c->name;
            bool taken = false;
            for (const ClipEntry& o : m_char.data.recipe.clips) taken = taken || (&o != c && o.name == want);
            if (taken) {
                m_message = "There is already an animation called " + want + ".";
                m_messageBad = true;
            } else if (want != c->name) {
                c->name = want;
                recipeChanged();
            }
            std::snprintf(m_nameBuf, sizeof m_nameBuf, "%s", c->name.c_str());
        }
        for (const std::string& n : m_char.data.info.clips)
            if (n == c->name) ui::hint("Replaces the clip of that name the character came with.");
    }
    rowLabel("Motion file");
    ImGui::TextUnformatted(fileName(file).c_str());
    ImGui::SetItemTooltip("%s", file.c_str());
    if (s->loading) {
        ImGui::SameLine();
        ui::hint(blender::needsBlender(file) ? "Blender is converting it..." : "loading...");
        return;
    }
    if (!s->data.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", s->data.error.c_str());
        if (!audition && ImGui::Button("Remove from the character", ImVec2(0.0f, 30.0f))) {
            m_char.data.recipe.clips.erase(m_char.data.recipe.clips.begin() + m_sel);
            selectClip(-1);
            recipeChanged();
        }
        return;
    }
    ImGui::SameLine();
    ui::hint("(%s)", s->data.note.c_str());
    const Rig& sr = *s->data.rig;
    int mi = motionIndex(sr, c->take);
    if (sr.motions.size() > 1) {
        rowLabel("Take");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##take", sr.motions[static_cast<std::size_t>(mi)].name.c_str())) {
            for (std::size_t i = 0; i < sr.motions.size(); ++i)
                if (ImGui::Selectable(sr.motions[i].name.c_str(), static_cast<int>(i) == mi)) {
                    c->take = sr.motions[i].name;
                    c->start = c->end = 0.0f;
                    m_time = 0.0f;
                    if (audition) m_txDirty = m_redraw = true; else recipeChanged();
                }
            ImGui::EndCombo();
        }
        mi = motionIndex(sr, c->take);
    }
    const Motion& mo = sr.motions[static_cast<std::size_t>(mi)];
    Options o;
    o.start = c->start;
    o.end = c->end;
    float a = 0.0f, b = 0.0f;
    clipRange(sr, mi, o, a, b);
    bool edited = false;
    const float frame = 1.0f / mo.fps;
    const float stepW = std::min(220.0f, ImGui::GetContentRegionAvail().x - kLabelW);
    rowLabel("Starts at");
    if (ui::stepper("##start", a, frame * 5.0f, 0.0f, std::max(0.0f, b - frame), "%.2f s", stepW)) {
        c->start = a;
        c->end = b;
        edited = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Here##s", ImVec2(60.0f, 0.0f))) {
        const float now = a + m_time;
        c->start = std::min(now, b - frame);
        c->end = b;
        m_time = 0.0f;
        edited = true;
    }
    ImGui::SetItemTooltip("Start the clip at the frame the studio shows now.");
    rowLabel("Ends at");
    if (ui::stepper("##end", b, frame * 5.0f, a + frame, mo.duration, "%.2f s", stepW)) {
        c->start = a;
        c->end = b;
        edited = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Here##e", ImVec2(60.0f, 0.0f))) {
        c->start = a;
        c->end = std::max(a + m_time, a + frame);
        edited = true;
    }
    ImGui::SetItemTooltip("End the clip at the frame the studio shows now.");
    ImGui::SameLine();
    if (ImGui::Button("All", ImVec2(44.0f, 0.0f))) {
        c->start = c->end = 0.0f;
        edited = true;
    }
    ImGui::SetItemTooltip("The whole motion again.");
    clipRange(sr, mi, Options{c->start, c->end, c->inPlace}, a, b);
    ImGui::SetCursorPosX(kLabelW);
    ui::hint("%.2f s, %d frames at %.0f fps (the motion is %.2f s)", b - a,
             static_cast<int>(std::lround((b - a) * mo.fps)) + 1, mo.fps, mo.duration);
    rowLabel("Travel");
    if (ImGui::RadioButton("Moves on (as recorded)", !c->inPlace)) { c->inPlace = false; edited = true; }
    ImGui::SetItemTooltip("The hips travel as in the recording -- for root motion, or a scene that is played once.");
    ImGui::SameLine();
    if (ImGui::RadioButton("In place", c->inPlace)) { c->inPlace = true; edited = true; }
    ImGui::SetItemTooltip("The forward travel is taken out (sway and bob stay): the game moves the character,\n"
                          "as a walk or run cycle for a controller wants it.");
    if (edited) {
        m_time = std::clamp(m_time, 0.0f, b - a);
        if (audition) m_txDirty = m_redraw = true;
        else recipeChanged();
    }
    if (!audition) {
        ImGui::Spacing();
        if (ImGui::Button("Remove from the character", ImVec2(0.0f, 30.0f))) {
            const std::string name = c->name;
            m_char.data.recipe.clips.erase(m_char.data.recipe.clips.begin() + m_sel);
            selectClip(m_char.data.recipe.clips.empty() ? -1 : std::max(0, m_sel - 1));
            recipeChanged();
            m_message = "Removed " + name + " -- write again to take it out of the file too.";
            m_messageBad = false;
        }
    }
}

bool RetargetTool::boneCombo(const char* id, const Rig& rig, int& node, bool source) {
    (void)source;
    bool changed = false;
    const char* shown = node >= 0 && node < static_cast<int>(rig.nodes.size())
                            ? rig.nodes[static_cast<std::size_t>(node)].name.c_str()
                            : "(none)";
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo(id, shown, ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) {
            m_boneSearch[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ui::searchBox("##bonesearch", m_boneSearch, sizeof m_boneSearch, "Search bones...");
        if (ImGui::Selectable("(none) -- leave this part out", node < 0)) { node = -1; changed = true; }
        for (int i : rig.order) {
            const Node& n = rig.nodes[static_cast<std::size_t>(i)];
            if (!n.joint || !ui::icontains(n.name.c_str(), m_boneSearch)) continue;
            int depth = 0;
            for (int p = n.parent; p >= 0 && depth < 12; p = rig.nodes[static_cast<std::size_t>(p)].parent) ++depth;
            const std::string label = std::string(static_cast<std::size_t>(m_boneSearch[0] ? 0 : depth), ' ') + n.name;
            ImGui::PushID(i);
            if (ImGui::Selectable(label.c_str(), node == i)) { node = i; changed = true; }
            if (node == i && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void RetargetTool::drawBonesTab() {
    const Rig* tr = m_char.data.rig.get();
    if (!tr) { ui::hint("No character yet."); return; }
    ClipEntry* c = current();
    Source* s = c ? sourceFor(currentFile()) : nullptr;
    const Rig* sr = s && !s->loading && s->data.rig ? s->data.rig.get() : nullptr;
    int both = 0;
    for (int k = 0; k < SlotCount; ++k)
        both += m_srcMap[static_cast<std::size_t>(k)] >= 0 && m_char.map[static_cast<std::size_t>(k)] >= 0 ? 1 : 0;
    if (sr && m_tx.copiedBones() > 0)
        ui::hint("%d of %d body parts carried over, and %d more bones by name (same rig). Point at a row to see "
                 "the bone in both figures.", both, SlotCount, m_tx.copiedBones());
    else if (sr) ui::hint("%d of %d body parts carried over. Point at a row to see the bone in both figures.", both, SlotCount);
    else ui::hint("The character's bones. Pick a motion to see both sides.");
    ImGui::BeginDisabled(!sr || !c || c->srcMap.empty());
    if (ImGui::Button("Motion: automatic")) { c->srcMap.clear(); if (m_sel >= 0) recipeChanged(); else mapsChanged(); }
    ImGui::SetItemTooltip("Forget the hand-picked bones of this motion.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_char.data.recipe.tgtMap.empty());
    if (ImGui::Button("Character: automatic")) {
        m_char.data.recipe.tgtMap.clear();
        m_char.map = m_char.autoMap;
        recipeChanged();
    }
    ImGui::SetItemTooltip("Forget the hand-picked bones of the character.");
    ImGui::EndDisabled();

    int hot = -1;
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("##map", 3, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Body part", ImGuiTableColumnFlags_WidthFixed, 140.0f);
    ImGui::TableSetupColumn("Bone in the motion", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Bone in the character", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (int g = 0; g < GroupCount; ++g) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const bool hands = g == GLeftHand || g == GRightHand;
        const bool open = ImGui::TreeNodeEx(groupName(g), ImGuiTreeNodeFlags_SpanAllColumns |
                                                              (hands ? 0 : ImGuiTreeNodeFlags_DefaultOpen));
        if (!open) continue;
        for (int k = 0; k < SlotCount; ++k) {
            if (slotInfo(k).group != g) continue;
            ImGui::PushID(k);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const int sn = m_srcMap[static_cast<std::size_t>(k)], tn = m_char.map[static_cast<std::size_t>(k)];
            const bool half = sr && ((sn >= 0) != (tn >= 0));
            ImGui::AlignTextToFramePadding();
            if (half) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s  (!)", slotInfo(k).label);
            else ImGui::TextUnformatted(slotInfo(k).label);
            if (half) ImGui::SetItemTooltip("Found on one side only: this part keeps the character's own pose.");
            if (ImGui::IsItemHovered()) hot = k;
            ImGui::TableSetColumnIndex(1);
            if (sr) {
                int n = sn;
                if (boneCombo("##src", *sr, n, true)) {
                    c->srcMap[slotInfo(k).key] = n >= 0 ? sr->nodes[static_cast<std::size_t>(n)].name : std::string();
                    if (m_sel >= 0) recipeChanged(); else mapsChanged();
                }
                if (ImGui::IsItemHovered()) hot = k;
            } else {
                ui::hint("-");
            }
            ImGui::TableSetColumnIndex(2);
            int n = tn;
            if (boneCombo("##tgt", *tr, n, false)) {
                m_char.data.recipe.tgtMap[slotInfo(k).key] = n >= 0 ? tr->nodes[static_cast<std::size_t>(n)].name : std::string();
                m_char.map[static_cast<std::size_t>(k)] = n;
                recipeChanged();
            }
            if (ImGui::IsItemHovered()) hot = k;
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::EndTable();
    if (hot != m_hot) { m_hot = hot; m_redraw = true; }
}

void RetargetTool::drawCharacterTab() {
    if (!m_char.data.rig) { ui::hint(m_char.loading ? "Loading..." : "No character yet."); return; }
    std::error_code ec;
    const std::string orig = originalPath(m_char.path);
    ui::title("%s", fileName(m_char.path).c_str());
    ui::hint("%s", m_char.path.c_str());
    if (fs::exists(fs::path(orig), ec))
        ui::hint("Its original is kept as %s -- every write starts from it, so nothing piles up.", fileName(orig).c_str());
    else
        ui::hint("The first write keeps its original as %s.", fileName(orig).c_str());
    ui::hint("%d bones; %d of %d body parts found.", m_char.data.rig->jointCount(), mappedCount(m_char.map), SlotCount);
    ui::sectionText("Repairs");
    const ModelInfo& info = m_char.data.info;
    Repair& rp = m_char.data.recipe.repair;
    if (!info.needsRepair()) ui::hint("Nothing to repair: one skin, and every mesh is on it.");
    bool reload = false;
    if (info.extraSkins > 0) {
        if (ImGui::Checkbox("Put the clothes on the body's skeleton", &rp.mergeSkins)) reload = true;
        ImGui::SetItemTooltip("%d mesh(es) are skinned to a copy of the skeleton of their own (a Daz export does\n"
                              "that for clothes). The engine plays the first skin only, so they would stand still.",
                              info.extraSkins);
    }
    if (!info.looseMeshes.empty()) {
        if (ImGui::Checkbox("Bind loose meshes to the nearest bone", &rp.bindLoose)) reload = true;
        ImGui::SetItemTooltip("Meshes without a skin (hair, eyelashes ...) would stay where they are while the body\n"
                              "moves. Each is bound whole to the bone most of it is closest to (hair: the head).");
        for (const std::string& n : info.looseMeshes) ui::hint("    %s", n.c_str());
    }
    if (reload) {
        recipeChanged();
        openCharacter(m_char.path);
    }
}

void RetargetTool::drawWriteBar() {
    const std::vector<ClipEntry>& clips = m_char.data.recipe.clips;
    bool loading = false;
    for (const ClipEntry& c : clips) {
        Source* s = sourceFor(sourceFile(m_char.path, c.source));
        loading = loading || s->loading;
    }
    char label[200];
    if (!m_char.data.rig) std::snprintf(label, sizeof label, "Write into the character");
    else if (clips.empty()) std::snprintf(label, sizeof label, "Add a motion first");
    else if (loading) std::snprintf(label, sizeof label, "Waiting for the motions to load...");
    else
        std::snprintf(label, sizeof label, "Write %d animation%s into %s", static_cast<int>(clips.size()),
                      clips.size() == 1 ? "" : "s", fileName(m_char.path).c_str());
    ImGui::BeginDisabled(!m_char.data.rig || clips.empty() || loading);
    if (m_pending) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(label, ImVec2(380.0f, 40.0f))) write();
    if (m_pending) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Retarget every animation in the list and save them into the model, with its original\n"
                          "clips. The scene picks the new clips up by itself.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    if (!m_message.empty())
        ImGui::TextColored(m_messageBad ? ImVec4(1.0f, 0.55f, 0.45f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s",
                           m_message.c_str());
    else if (m_pending)
        ui::hint("Changes not yet in the file.");
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
}

void RetargetTool::panel(bool& show) {
    ImGui::SetNextWindowSize(ImVec2(m_firstW, m_firstH), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Retarget animations", &show)) { ImGui::End(); return; }
    if (!m_studioReady) {
        m_studioReady = true;
        m_studioOk = m_studio.init();
    }
    if (m_modelsFor != projectDir()) scanModels();
    poll();
    // Files dropped onto this window become animations of the character.
    if (m_d.drops && !m_d.drops->empty() && m_d.dropX && m_d.dropY) {
        const ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
        if (*m_d.dropX >= p.x && *m_d.dropX <= p.x + sz.x && *m_d.dropY >= p.y && *m_d.dropY <= p.y + sz.y) {
            const std::vector<std::string> files = *m_d.drops;
            m_d.drops->clear();
            for (const std::string& f : files) addMotion(fs::path(f).generic_string());
        }
    }
    drawTopBar();
    ImGui::Separator();
    const float barH = 54.0f;
    const float avail = std::max(200.0f, ImGui::GetContentRegionAvail().y - barH);
    ImGui::BeginChild("##left", ImVec2(290.0f, avail));
    drawClipList(std::max(130.0f, avail * 0.36f));
    ImGui::Spacing();
    // Where new animations come from: a folder of motion files, or the clips
    // another character already has.
    if (ImGui::BeginTabBar("##from")) {
        if (ImGui::BeginTabItem("Motion library")) { drawLibrary(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Characters", nullptr, m_showCharacters ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawCharacters();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        m_showCharacters = false;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##right", ImVec2(0.0f, avail));
    const float rw = ImGui::GetContentRegionAvail().x;
    drawStudio(rw, std::clamp(avail * 0.5f, 220.0f, rw * 0.6f));
    drawTransport();
    if (ImGui::BeginTabBar("##tabs")) {
        auto pick = [&](int t) { return m_tabRequest == t ? ImGuiTabItemFlags_SetSelected : 0; };
        if (ImGui::BeginTabItem("Clip", nullptr, pick(0))) { drawClipTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Bones", nullptr, pick(1))) { drawBonesTab(); ImGui::EndTabItem(); }
        else if (m_hot >= 0) { m_hot = -1; m_redraw = true; }
        if (ImGui::BeginTabItem("Character", nullptr, pick(2))) { drawCharacterTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
        m_tabRequest = -1;
    }
    ImGui::EndChild();
    ImGui::Separator();
    drawWriteBar();
    ImGui::End();
    if (m_recipeDirty && m_char.data.rig && !m_char.loading) {
        saveRecipe(m_char.path, m_char.data.recipe);
        m_recipeDirty = false;
    }
}

} // namespace retargetui