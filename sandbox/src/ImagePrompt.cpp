// The Image editor's "New image from prompt" window: a sentence, a size, a
// seed -- and ComfyUI (ComfyUi.hpp) paints the picture, which opens in a tab of
// its own or lands on a layer of the open picture. The window is not modal:
// the editor stays usable while the server works, and the window shows how
// far it got and the server's preview. The rest of the editor is
// ImageEditPanel.cpp.
#include "ImageEditPanel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>

#include <imgui.h>

#include "ComfyUi.hpp"
#include "FolderDialog.hpp"
#include "UiStyle.hpp"
#include "fitzel/rhi/Device.hpp"
#include "fitzel/ui/ImGuiTexture.hpp"

namespace imageui {

namespace {

float em() { return ImGui::GetFontSize(); }

bool bigButton(const char* label, const char* tip = nullptr, float w = 0.0f) {
    const bool r = ImGui::Button(label, ImVec2(w, ImGui::GetFrameHeight() + em() * 0.45f));
    if (tip) ImGui::SetItemTooltip("%s", tip);
    return r;
}

bool toggle(const char* label, bool on, float w = 0.0f, const char* tip = nullptr) {
    if (on) {
        const ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, c);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
    }
    const bool r = bigButton(label, tip, w);
    if (on) ImGui::PopStyleColor(2);
    return r;
}

float across(int n) {
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    return std::floor((ImGui::GetContentRegionAvail().x - sp * float(n - 1)) / float(n));
}

void copyTo(char* buf, std::size_t cap, const std::string& s) {
    std::snprintf(buf, cap, "%s", s.c_str());
}

std::string trimmed(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// A file name from the prompt's first words: "Rostige Metallplatte, Nieten"
// -> "rostige_metallplatte_nieten".
std::string nameFor(const std::string& prompt) {
    static const struct { const char* utf8; const char* ascii; } kFold[] = {
        {"\xC3\xA4", "ae"}, {"\xC3\xB6", "oe"}, {"\xC3\xBC", "ue"}, {"\xC3\x84", "ae"},
        {"\xC3\x96", "oe"}, {"\xC3\x9C", "ue"}, {"\xC3\x9F", "ss"}};
    std::string out;
    int words = 0;
    bool gap = false;
    for (std::size_t i = 0; i < prompt.size() && words < 6 && out.size() < 48; ++i) {
        const unsigned char c = static_cast<unsigned char>(prompt[i]);
        std::string piece;
        if (std::isalnum(c)) {
            piece = char(std::tolower(c));
        } else if (c >= 0x80) {
            for (const auto& f : kFold)
                if (prompt.compare(i, 2, f.utf8) == 0) { piece = f.ascii; ++i; break; }
        }
        if (piece.empty()) {
            gap = !out.empty();
            continue;
        }
        if (gap) {
            if (++words >= 6) break;
            out += '_';
            gap = false;
        }
        out += piece;
    }
    return out.empty() ? std::string("prompt") : out;
}

struct SizePreset { const char* label; int w, h; const char* tip; };
const SizePreset kSizes[] = {
    {"512", 512, 512, nullptr},
    {"768", 768, 768, nullptr},
    {"1024", 1024, 1024, "What Z-Image Turbo is made for -- the sharpest."},
    {"1536", 1536, 1536, "Slower; details can repeat."},
    {"1344 x 768", 1344, 768, "Wide, 16:9"},
    {"768 x 1344", 768, 1344, "Tall, 9:16"},
    {"1152 x 896", 1152, 896, "4:3"},
    {"896 x 1152", 896, 1152, "3:4"},
};

} // namespace

const comfy::Workflow& ImageEditor::workflow() {
    const std::string& file = comfy::client().settings.workflow;
    if (!m_workflow || m_workflowFor != file) {
        m_workflowFor = file;
        m_workflowErr.clear();
        auto wf = std::make_shared<comfy::Workflow>();
        if (file.empty() || !comfy::loadWorkflow(file, *wf, m_workflowErr)) *wf = comfy::builtinWorkflow();
        m_workflow = wf;
    }
    return *m_workflow;
}

void ImageEditor::generate(const std::string& prompt) {
    if (m_job) return;
    comfy::Client& cc = comfy::client();
    comfy::Settings& s = cc.settings;
    const std::string p = trimmed(prompt);
    if (p.empty()) {
        m_promptMsg = "Write what the picture should show first.";
        m_promptFailed = true;
        return;
    }
    s.prompt = p;
    const comfy::Workflow& wf = workflow();
    comfy::Request r;
    r.prompt = p;
    r.width  = s.width;
    r.height = s.height;
    r.steps  = wf.steps.empty() ? 0 : s.steps;
    r.count  = wf.count.empty() ? 1 : s.count;
    r.seed   = s.randomSeed ? comfy::randomSeed() : s.seed;
    cc.save();
    View* v = cur();
    m_jobTarget = m_promptAsLayer && v ? v->id : -1;
    m_jobPrompt = p;
    m_job = cc.generate(wf, r);
    m_promptMsg.clear();
    m_promptFailed = false;
    m_previewVer = 0;
}

void ImageEditor::showPreview(const std::vector<std::uint8_t>& px, int w, int h) {
    if (m_previewTex) m_retired.push_back(m_previewTex);
    fitzel::rhi::TextureDesc td;
    td.format = fitzel::rhi::Format::RGBA8;
    td.width = w;
    td.height = h;
    td.sampler = fitzel::rhi::SamplerDesc::linearClamp();
    m_previewTex = fitzel::rhi::device().createTexture(td, px.data(), fitzel::rhi::Format::RGBA8);
    m_previewW = w;
    m_previewH = h;
}

void ImageEditor::takeGenerated() {
    if (m_probe && m_probe->finished()) {
        std::lock_guard<std::mutex> g(m_probe->lock);
        m_probeFailed = !m_probe->ok;
        m_probeMsg = m_probe->ok ? m_probe->info : m_probe->error;
        m_probe.reset();
    }
    if (!m_job) return;
    comfy::Image pv;
    {
        std::lock_guard<std::mutex> g(m_job->lock);
        if (m_job->previewVersion != m_previewVer && !m_job->preview.px.empty()) {
            pv = m_job->preview;
            m_previewVer = m_job->previewVersion;
        }
    }
    if (!pv.px.empty()) showPreview(pv.px, pv.w, pv.h);
    if (!m_job->finished()) return;

    // Layers go onto their picture when nothing is half done on it.
    View* target = nullptr;
    for (auto& v : m_views)
        if (v->id == m_jobTarget) target = v.get();
    if (target && target == cur() && (m_pane != Pane::None || m_xf.on || m_crop.on || m_lasso.on || m_text.on || m_painting))
        return;

    std::vector<comfy::Image> images;
    std::string err;
    double secs = 0.0;
    long long seed = 0;
    {
        std::lock_guard<std::mutex> g(m_job->lock);
        images = std::move(m_job->images);
        err = m_job->ok ? std::string() : m_job->error;
        secs = m_job->seconds;
        seed = m_job->seed;
    }
    m_job.reset();
    if (err.empty() && images.empty()) err = "ComfyUI sent no picture back.";
    if (!err.empty()) {
        m_promptFailed = true;
        m_promptMsg = err == "cancelled" ? std::string("Cancelled.") : err;
        m_d.status = "New image from prompt: " + m_promptMsg;
        return;
    }
    comfy::client().rememberServer();
    comfy::client().save();
    m_lastSeed = seed;
    showPreview(images.front().px, images.front().w, images.front().h);
    const std::string base = nameFor(m_jobPrompt);
    const bool many = images.size() > 1;
    for (std::size_t i = 0; i < images.size(); ++i) {
        comfy::Image& im = images[i];
        const std::string name = many ? base + "_" + std::to_string(i + 1) : base;
        if (target) {
            if (target == cur()) cancelGesture(*target);
            target->doc.checkpoint("New image from prompt");
            target->doc.insertLayer(name, im.px, im.w, im.h, (target->doc.width() - im.w) / 2,
                                    (target->doc.height() - im.h) / 2);
        } else {
            auto nv = std::make_unique<View>();
            nv->doc.adopt(im.w, im.h, std::move(im.px), "Background");
            nv->doc.setName(name);
            nv->doc.markUnsaved();
            nv->id = m_nextId++;
            m_views.push_back(std::move(nv));
            m_cur = int(m_views.size()) - 1;
            m_selectTab = m_cur;
        }
    }
    char msg[160];
    std::snprintf(msg, sizeof msg, "%s in %.1f s, seed %lld.", many ? "Made the pictures" : "Made the picture", secs, seed);
    m_promptFailed = false;
    m_promptMsg = msg;
    m_d.status = std::string(msg) + (target ? " On layers of " + target->doc.title() + "." : " Not saved yet -- Ctrl+S.");
}

void ImageEditor::promptWindow() {
    takeGenerated();
    if (!m_promptShow) return;
    comfy::Client& cc = comfy::client();
    comfy::Settings& s = cc.settings;
    if (!m_promptInit) {
        copyTo(m_promptBuf, sizeof m_promptBuf, s.prompt);
        copyTo(m_urlBuf, sizeof m_urlBuf, s.url);
        copyTo(m_startBuf, sizeof m_startBuf, s.startCommand);
        m_promptInit = true;
    }
    // The first time: beside the editor if the screen has room there, else
    // over its right-hand side.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(em() * 32.0f, std::min(em() * 46.0f, vp->WorkSize.y - em() * 4.0f));
    float x = m_winPos.x + m_winSize.x + em() * 0.5f;
    if (x + size.x > vp->WorkPos.x + vp->WorkSize.x) x = m_winPos.x + m_winSize.x - size.x - em() * 2.0f;
    ImGui::SetNextWindowPos(ImVec2(std::max(vp->WorkPos.x, x), m_winPos.y + em() * 3.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    if (m_promptFocus) ImGui::SetNextWindowFocus();
    if (!ImGui::Begin("New image from prompt###imgprompt", &m_promptShow)) {
        ImGui::End();
        return;
    }
    const comfy::Workflow& wf = workflow();
    const bool busy = m_job != nullptr;

    ui::hint("What the picture shows: the thing, its material, the light, the style.");
    if (m_promptFocus) {
        ImGui::SetKeyboardFocusHere();
        m_promptFocus = false;
    }
    bool go = ImGui::InputTextMultiline("##prompt", m_promptBuf, sizeof m_promptBuf, ImVec2(-1.0f, em() * 5.5f),
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_WordWrap);
    ImGui::SetItemTooltip("Ctrl+Enter paints it.");

    ui::sectionText("Size");
    for (int i = 0; i < int(std::size(kSizes)); ++i) {
        if (i % 4) ImGui::SameLine();
        if (toggle(kSizes[i].label, s.width == kSizes[i].w && s.height == kSizes[i].h, across(4 - i % 4), kSizes[i].tip)) {
            s.width = kSizes[i].w;
            s.height = kSizes[i].h;
        }
    }
    float fw = float(s.width), fh = float(s.height);
    const float sw = em() * 9.0f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Width");
    ImGui::SameLine();
    if (ui::stepper("pw", fw, 64.0f, 64.0f, 4096.0f, "%.0f px", sw)) s.width = int(fw);
    ImGui::SameLine();
    ImGui::TextUnformatted("Height");
    ImGui::SameLine();
    if (ui::stepper("ph", fh, 64.0f, 64.0f, 4096.0f, "%.0f px", sw)) s.height = int(fh);
    if (View* v = cur()) {
        char like[64];
        std::snprintf(like, sizeof like, "Like the open picture (%d x %d)", v->doc.width(), v->doc.height());
        if (bigButton(like, "Its size, rounded to what the model takes (multiples of 16).", -1.0f)) {
            s.width = std::clamp((v->doc.width() + 8) / 16 * 16, 64, 4096);
            s.height = std::clamp((v->doc.height() + 8) / 16 * 16, 64, 4096);
        }
    }

    ui::sectionText("Painting");
    if (!wf.count.empty()) {
        float n = float(s.count);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Pictures");
        ImGui::SameLine();
        if (ui::stepper("pn", n, 1.0f, 1.0f, 4.0f, "%.0f", sw)) s.count = int(n);
        ImGui::SetItemTooltip("Several at once, each its own tab -- one seed, different pictures.");
    }
    if (!wf.steps.empty()) {
        float st = float(s.steps > 0 ? s.steps : wf.defSteps);
        if (!wf.count.empty()) ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Steps");
        ImGui::SameLine();
        if (ui::stepper("pst", st, 1.0f, 1.0f, 100.0f, "%.0f", sw)) s.steps = int(st) == wf.defSteps ? 0 : int(st);
        ImGui::SetItemTooltip("The workflow's own: %d. More is slower, rarely better for a turbo model.", wf.defSteps);
    }
    if (!wf.seed.empty()) {
        ImGui::Checkbox("A new seed each time", &s.randomSeed);
        ImGui::SetItemTooltip("Off: the same seed and prompt paint the same picture again -- change a word and see what it does.");
        if (!s.randomSeed) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(em() * 10.0f);
            ImGui::InputScalar("##seed", ImGuiDataType_S64, &s.seed);
            ImGui::SetItemTooltip("The seed");
        }
        if (m_lastSeed >= 0 && (s.randomSeed || s.seed != m_lastSeed)) {
            ImGui::SameLine();
            char keep[64];
            std::snprintf(keep, sizeof keep, "Keep %lld", m_lastSeed);
            if (ImGui::Button(keep)) { s.randomSeed = false; s.seed = m_lastSeed; }
            ImGui::SetItemTooltip("The last picture's seed: paint it again with another prompt.");
        }
    }
    ImGui::BeginDisabled(cur() == nullptr);
    bool asLayer = m_promptAsLayer && cur();
    if (ImGui::Checkbox("Onto the open picture, as a layer", &asLayer)) m_promptAsLayer = asLayer;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Off: every picture opens in a tab of its own.");

    ImGui::Spacing();
    if (!busy) {
        go = bigButton("Generate", "Ctrl+Enter", -1.0f) || go;
        if (go) generate(m_promptBuf);
    } else {
        std::string stage;
        int step = 0, steps = 0;
        {
            std::lock_guard<std::mutex> g(m_job->lock);
            stage = m_job->stage;
            step = m_job->step;
            steps = m_job->steps;
        }
        char overlay[256];
        if (steps > 0) std::snprintf(overlay, sizeof overlay, "%s  %d / %d", stage.c_str(), step, steps);
        else std::snprintf(overlay, sizeof overlay, "%s", stage.c_str());
        const float cw = em() * 6.0f;
        const float h = ImGui::GetFrameHeight() + em() * 0.45f;
        // Without a step count the bar sweeps: something is happening, how much is not known.
        const float frac = steps > 0 ? float(step) / float(steps) : -float(ImGui::GetTime());
        ImGui::ProgressBar(frac, ImVec2(ImGui::GetContentRegionAvail().x - cw - ImGui::GetStyle().ItemSpacing.x, h), overlay);
        ImGui::SameLine();
        if (bigButton("Cancel", "Takes the prompt off ComfyUI's queue, or stops it there.", cw)) cc.cancel(m_job);
    }
    if (!m_promptMsg.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        if (m_promptFailed) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", m_promptMsg.c_str());
        else ui::hint("%s", m_promptMsg.c_str());
        ImGui::PopTextWrapPos();
    }
    if (m_previewTex && m_previewW > 0 && m_previewH > 0) {
        // What the window has left above its two headers, and not less than a thumbnail.
        const float headers = (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y) * 2.0f + em();
        const float maxW = ImGui::GetContentRegionAvail().x;
        const float maxH = std::max(em() * 8.0f, ImGui::GetContentRegionAvail().y - headers);
        const float k = std::min(maxW / float(m_previewW), maxH / float(m_previewH));
        ImGui::Image(fitzel::imTexture(m_previewTex), ImVec2(float(m_previewW) * k, float(m_previewH) * k));
        ImGui::SetItemTooltip(busy ? "ComfyUI's preview while it paints" : "The last picture made");
    }

    ImGui::Spacing();
    if (ui::header("Server")) promptServer();
    if (ui::header("Workflow")) promptWorkflow();
    // The keyboard is ours while this window has it, as it is the editor
    // window's: Delete here must not delete something in the scene.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) m_focusFrame = ImGui::GetFrameCount();
    ImGui::End();
}

void ImageEditor::promptServer() {
    comfy::Client& cc = comfy::client();
    comfy::Settings& s = cc.settings;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - em() * 9.5f);
    if (ImGui::InputTextWithHint("##url", "http://127.0.0.1:8188", m_urlBuf, sizeof m_urlBuf)) {
        s.url = trimmed(m_urlBuf);
        m_probeMsg.clear();
    }
    ImGui::SetItemTooltip("Where ComfyUI listens -- this machine, or one on the network started with --listen.");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_probe != nullptr);
    if (ImGui::Button("Test", ImVec2(em() * 4.5f, 0.0f))) {
        s.url = trimmed(m_urlBuf);
        m_probeMsg = "Asking...";
        m_probeFailed = false;
        m_probe = cc.probe(workflow());
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("What the server is, and whether it has the model files the workflow loads.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##recent", "Used", ImGuiComboFlags_HeightRegular)) {
        if (s.recent.empty()) ui::hint("none yet");
        for (const std::string& r : s.recent)
            if (ImGui::Selectable(r.c_str(), r == s.url)) {
                s.url = r;
                copyTo(m_urlBuf, sizeof m_urlBuf, r);
                m_probeMsg.clear();
            }
        ImGui::EndCombo();
    }
    if (!m_probeMsg.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        if (m_probeFailed) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", m_probeMsg.c_str());
        else ui::hint("%s", m_probeMsg.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Start command");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##start", "systemctl start comfyui", m_startBuf, sizeof m_startBuf))
        s.startCommand = trimmed(m_startBuf);
    ui::hint("Run when the server does not answer; Generate then waits for it. Empty: just say so.");
}

void ImageEditor::promptWorkflow() {
    comfy::Settings& s = comfy::client().settings;
    const comfy::Workflow& wf = workflow();
    ImGui::TextUnformatted(wf.name.c_str());
    if (!s.workflow.empty()) ui::hint("%s", s.workflow.c_str());
    if (!m_workflowErr.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s -- the built-in one paints instead.", m_workflowErr.c_str());
        ImGui::PopTextWrapPos();
    }
    if (toggle("Built in", s.workflow.empty(), across(2), "ComfyUI's Z-Image Turbo template: 8 steps, about a second a picture on a big card.")) {
        s.workflow.clear();
        m_probeMsg.clear();
    }
    ImGui::SameLine();
    if (bigButton("From a file...", "A workflow saved in ComfyUI with File > Export (API).", across(1))) {
        std::string out;
        if (ed::pickFile(out, s.workflow.empty() ? startDir() : std::filesystem::path(s.workflow).parent_path().string(),
                         "ComfyUI workflow (API)", "*.json")) {
            s.workflow = out;
            m_probeMsg.clear();
        }
    }
    // Where the window's values go, so a workflow of one's own can be checked.
    auto where = [](const std::vector<comfy::Slot>& slots) {
        std::string t;
        for (const comfy::Slot& sl : slots) t += (t.empty() ? "" : ", ") + sl.node + "." + sl.input;
        return t.empty() ? std::string("--") : t;
    };
    ImGui::PushTextWrapPos(0.0f);
    ui::hint("Prompt -> %s   Size -> %s   Seed -> %s   Steps -> %s   Pictures -> %s", where(wf.prompt).c_str(),
             where(wf.width).c_str(), where(wf.seed).c_str(), where(wf.steps).c_str(), where(wf.count).c_str());
    ImGui::PopTextWrapPos();
}

} // namespace imageui
