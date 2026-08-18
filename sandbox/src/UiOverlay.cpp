#include "UiOverlay.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/asset/AssetTypes.hpp>
#include <fitzel/graphics/Texture.hpp>

using fitzel::AssetId;
using fitzel::AssetDatabase;

// ---------------------------------------------------------------------------
// Layout + drawing helpers (file-local). Offsets/sizes come in as pixels at
// kRefHeight; every call scales by (viewport height / kRefHeight) first.
// ---------------------------------------------------------------------------
namespace {

int colOf(UiAnchor a) { return static_cast<int>(a) % 3; } // 0 left, 1 centre, 2 right
int rowOf(UiAnchor a) { return static_cast<int>(a) / 3; } // 0 top,  1 middle, 2 bottom

ImU32 col32(const glm::vec4& c, float alpha = 1.0f) {
    auto b = [](float v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return IM_COL32(b(c.r), b(c.g), b(c.b), b(c.a * std::clamp(alpha, 0.0f, 1.0f)));
}

// Scale an already-packed colour's alpha (for the menu's fade-in).
ImU32 fade(ImU32 c, float a) {
    const float f = std::clamp(a, 0.0f, 1.0f);
    return (c & 0x00FFFFFFu) | (static_cast<ImU32>(((c >> 24) & 0xFF) * f) << 24);
}

// The menu's one accent, matching the focus ring the pad navigation already
// used -- and the racing HUD's gold, so the game speaks with one voice.
constexpr ImU32 kMenuAccent = IM_COL32(255, 215, 120, 255);

float easeOutCubic(float t) {
    const float u = 1.0f - std::clamp(t, 0.0f, 1.0f);
    return 1.0f - u * u * u;
}

// Screen-space top-left of an element's box. `box` is the already-scaled pixel
// size; `off` is the already-scaled pixel offset. The anchor picks which
// corner/edge the offset is measured inward from, so a layout reads the same at
// any resolution and from any corner.
ImVec2 topLeft(UiAnchor a, const ImVec2& vmin, const ImVec2& vsize,
               const ImVec2& off, const ImVec2& box) {
    const int c = colOf(a), r = rowOf(a);
    float x = 0.0f, y = 0.0f;
    if (c == 0)      x = vmin.x + off.x;                              // left:  inward +x
    else if (c == 1) x = vmin.x + vsize.x * 0.5f + off.x - box.x * 0.5f; // centre
    else             x = vmin.x + vsize.x - off.x - box.x;           // right: inward -x
    if (r == 0)      y = vmin.y + off.y;                              // top:   inward +y
    else if (r == 1) y = vmin.y + vsize.y * 0.5f + off.y - box.y * 0.5f; // middle
    else             y = vmin.y + vsize.y - off.y - box.y;           // bottom: inward -y
    return ImVec2(x, y);
}

// Base HUD text size for fontScale == 1: readable, scaled to the view, matching
// the existing Play HUD's feel.
float baseFontPx(const ImVec2& vsize) {
    return std::clamp(vsize.y * 0.03f, 16.0f, 40.0f);
}

// Text with a soft drop shadow, like the Play HUD's shadowText.
void shadowText(ImDrawList* dl, ImFont* font, float px, const ImVec2& p,
                ImU32 col, const char* s) {
    const float a = ((col >> 24) & 0xFF) / 255.0f;
    dl->AddText(font, px, ImVec2(p.x + 2.0f, p.y + 2.0f),
                fade(IM_COL32(0, 0, 0, 170), a), s);
    dl->AddText(font, px, p, col, s);
}

// Where an element lands on screen, without drawing it. Split out of drawVisual
// so a caller can know the rectangles BEFORE anything is drawn -- which is what
// lets the menu frame itself around its own contents, and what stops a lit
// button being drawn twice (once to measure, once to light).
void elementRect(const UiElement& e, const ImVec2& vmin, const ImVec2& vsize,
                 ImVec2& p0, ImVec2& p1) {
    const float scale = vsize.y / UiOverlay::kRefHeight;
    const ImVec2 off(e.offset.x * scale, e.offset.y * scale);
    if (e.type == UiElementType::Text) {
        const float px = std::max(6.0f, baseFontPx(vsize) * e.fontScale);
        const ImVec2 ts =
            ImGui::GetFont()->CalcTextSizeA(px, FLT_MAX, 0.0f, e.text.c_str());
        p0 = topLeft(e.anchor, vmin, vsize, off, ts);
        p1 = ImVec2(p0.x + ts.x, p0.y + ts.y);
        return;
    }
    const ImVec2 box(std::max(4.0f, e.size.x * scale), std::max(4.0f, e.size.y * scale));
    p0 = topLeft(e.anchor, vmin, vsize, off, box);
    p1 = ImVec2(p0.x + box.x, p0.y + box.y);
}

// Draw one element's visuals and return its screen rect (p0=top-left, p1=bottom-
// right). `tex` is the already-resolved image texture (kept alive by the caller;
// null = none). `hovered` lifts a button's background so a click reads pressable.
void drawVisual(ImDrawList* dl, const UiElement& e, const ImVec2& vmin,
                const ImVec2& vsize, const fitzel::Texture* tex, bool hovered,
                ImVec2& outP0, ImVec2& outP1, const ImVec2& shift = ImVec2(0, 0),
                float alpha = 1.0f) {
    ImFont* font = ImGui::GetFont();
    ImVec2 p0, p1;
    elementRect(e, vmin, vsize, p0, p1);
    p0.x += shift.x; p0.y += shift.y;
    p1.x += shift.x; p1.y += shift.y;
    outP0 = p0; outP1 = p1;

    if (e.type == UiElementType::Text) {
        const float px = std::max(6.0f, baseFontPx(vsize) * e.fontScale);
        shadowText(dl, font, px, p0, col32(e.color, alpha), e.text.c_str());
        return;
    }

    const ImVec2 box(p1.x - p0.x, p1.y - p0.y);

    if (e.type == UiElementType::Image) {
        // GL textures are bottom-up, so flip V (uv0.y=1, uv1.y=0) -- same as the
        // scene viewport image -- or the picture shows upside down.
        if (tex && tex->isValid())
            dl->AddImage((ImTextureID)(std::intptr_t)tex->id(), p0, p1,
                         ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f),
                         fade(IM_COL32_WHITE, alpha));
        // No placeholder in the runtime path -- an unset image simply draws
        // nothing; the authoring pass outlines it instead (see drawAuthoring).
        return;
    }

    // Button: background (image or colour), then a centred label.
    const float rounding = std::min(box.y * 0.25f, 10.0f);
    if (tex && tex->isValid()) {
        dl->AddImageRounded((ImTextureID)(std::intptr_t)tex->id(), p0, p1,
            ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f), fade(IM_COL32_WHITE, alpha),
            rounding); // flip V (GL bottom-up)
        if (hovered)
            dl->AddRectFilled(p0, p1, fade(IM_COL32(255, 255, 255, 40), alpha), rounding);
    } else {
        glm::vec4 bg = e.bgColor;
        if (hovered) bg = glm::vec4(glm::min(glm::vec3(bg) + 0.12f, glm::vec3(1.0f)), bg.a);
        dl->AddRectFilled(p0, p1, col32(bg, alpha), rounding);
        dl->AddRect(p0, p1, fade(IM_COL32(255, 255, 255, hovered ? 120 : 60), alpha),
                    rounding, 0, 1.5f);
    }
    if (!e.text.empty()) {
        const float px = std::max(6.0f, baseFontPx(vsize) * e.fontScale);
        const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, e.text.c_str());
        const ImVec2 tp(p0.x + (box.x - ts.x) * 0.5f, p0.y + (box.y - ts.y) * 0.5f);
        shadowText(dl, font, px, tp, col32(e.color, alpha), e.text.c_str());
    }
}

void fireAction(const UiElement& e, const UiActionSink& sink) {
    switch (e.action) {
        case UiActionKind::None: break;
        case UiActionKind::LoadScene:
            if (sink.loadScene && !e.actionStr.empty()) sink.loadScene(e.actionStr);
            break;
        case UiActionKind::ShowMessage:
            if (sink.showMessage) sink.showMessage(e.actionStr);
            break;
        case UiActionKind::AddScore:
            if (sink.addScore) sink.addScore(e.actionNum);
            break;
        case UiActionKind::PlaySound:
            if (sink.playSound && !e.actionStr.empty()) sink.playSound(e.actionStr);
            break;
        case UiActionKind::Quit:
            if (sink.quit) sink.quit();
            break;
        case UiActionKind::Resume:
            if (sink.resume) sink.resume();
            break;
        case UiActionKind::Restart:
            if (sink.restart) sink.restart();
            break;
        case UiActionKind::Graphics:
            if (sink.graphics) sink.graphics();
            break;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// UiElement equality (undo trivial-check compares whole lists).
// ---------------------------------------------------------------------------
bool UiElement::operator==(const UiElement& o) const {
    return type == o.type && name == o.name && visible == o.visible &&
           anchor == o.anchor && offset == o.offset && size == o.size &&
           text == o.text && fontScale == o.fontScale && color == o.color &&
           bgColor == o.bgColor && image == o.image && action == o.action &&
           actionStr == o.actionStr && actionNum == o.actionNum;
}

// ---------------------------------------------------------------------------
// Serialization (scene "settings" object, under "uiOverlay").
// ---------------------------------------------------------------------------
namespace {
const char* elementTypeName(UiElementType t) {
    switch (t) {
        case UiElementType::Text:   return "text";
        case UiElementType::Button: return "button";
        case UiElementType::Image:  return "image";
    }
    return "text";
}
UiElementType elementType(const std::string& s) {
    if (s == "button") return UiElementType::Button;
    if (s == "image")  return UiElementType::Image;
    return UiElementType::Text;
}
const char* actionName(UiActionKind a) {
    switch (a) {
        case UiActionKind::None:        return "none";
        case UiActionKind::LoadScene:   return "loadScene";
        case UiActionKind::ShowMessage: return "showMessage";
        case UiActionKind::AddScore:    return "addScore";
        case UiActionKind::PlaySound:   return "playSound";
        case UiActionKind::Quit:        return "quit";
        case UiActionKind::Resume:      return "resume";
        case UiActionKind::Restart:     return "restart";
        case UiActionKind::Graphics:    return "graphics";
    }
    return "none";
}
UiActionKind actionKind(const std::string& s) {
    if (s == "loadScene")   return UiActionKind::LoadScene;
    if (s == "showMessage") return UiActionKind::ShowMessage;
    if (s == "addScore")    return UiActionKind::AddScore;
    if (s == "playSound")   return UiActionKind::PlaySound;
    if (s == "quit")        return UiActionKind::Quit;
    if (s == "resume")      return UiActionKind::Resume;
    if (s == "restart")     return UiActionKind::Restart;
    if (s == "graphics")    return UiActionKind::Graphics;
    return UiActionKind::None;
}
} // namespace

void UiOverlay::save(nlohmann::json& settings) const {
    if (m_elements.empty()) return; // keep clean scenes free of an empty key
    // Menu settings ride in their own key so the element array keeps its shape
    // (scenes written before menu mode existed still load unchanged).
    settings["uiOverlayMenu"] = {
        {"menu", m_menuMode},
        {"toggleKey", m_toggleKey},
    };
    nlohmann::json arr = nlohmann::json::array();
    for (const UiElement& e : m_elements) {
        nlohmann::json j = {
            {"type", elementTypeName(e.type)},
            {"name", e.name},
            {"visible", e.visible},
            {"anchor", static_cast<int>(e.anchor)},
            {"offset", {e.offset.x, e.offset.y}},
            {"size", {e.size.x, e.size.y}},
            {"text", e.text},
            {"fontScale", e.fontScale},
            {"color", {e.color.r, e.color.g, e.color.b, e.color.a}},
            {"bgColor", {e.bgColor.r, e.bgColor.g, e.bgColor.b, e.bgColor.a}},
            {"action", actionName(e.action)},
            {"actionStr", e.actionStr},
            {"actionNum", e.actionNum},
        };
        if (e.image.valid()) j["image"] = e.image.toString();
        arr.push_back(std::move(j));
    }
    settings["uiOverlay"] = std::move(arr);
}

void UiOverlay::load(const nlohmann::json& settings) {
    m_elements.clear();
    m_texCache.clear(); // drop the previous scene's texture handles
    m_menuMode  = false;        // scenes without the key are plain HUDs
    m_toggleKey = kKeyEscape;
    if (settings.contains("uiOverlayMenu") && settings["uiOverlayMenu"].is_object()) {
        const nlohmann::json& o = settings["uiOverlayMenu"];
        m_menuMode  = o.value("menu", false);
        m_toggleKey = o.value("toggleKey", static_cast<int>(kKeyEscape));
    }
    resetRuntime(); // a loaded menu starts closed, a HUD starts shown
    if (!settings.contains("uiOverlay") || !settings["uiOverlay"].is_array()) return;
    for (const nlohmann::json& j : settings["uiOverlay"]) {
        UiElement e;
        e.type    = elementType(j.value("type", std::string("text")));
        e.name    = j.value("name", std::string("Element"));
        e.visible = j.value("visible", true);
        e.anchor  = static_cast<UiAnchor>(std::clamp(j.value("anchor", 0), 0, 8));
        if (j.contains("offset") && j["offset"].is_array() && j["offset"].size() == 2)
            e.offset = glm::vec2(j["offset"][0].get<float>(), j["offset"][1].get<float>());
        if (j.contains("size") && j["size"].is_array() && j["size"].size() == 2)
            e.size = glm::vec2(j["size"][0].get<float>(), j["size"][1].get<float>());
        e.text      = j.value("text", std::string());
        e.fontScale = j.value("fontScale", 1.0f);
        if (j.contains("color") && j["color"].is_array() && j["color"].size() == 4)
            e.color = glm::vec4(j["color"][0].get<float>(), j["color"][1].get<float>(),
                                j["color"][2].get<float>(), j["color"][3].get<float>());
        if (j.contains("bgColor") && j["bgColor"].is_array() && j["bgColor"].size() == 4)
            e.bgColor = glm::vec4(j["bgColor"][0].get<float>(), j["bgColor"][1].get<float>(),
                                  j["bgColor"][2].get<float>(), j["bgColor"][3].get<float>());
        if (j.contains("image") && j["image"].is_string())
            e.image = AssetId::fromString(j["image"].get<std::string>());
        e.action    = actionKind(j.value("action", std::string("none")));
        e.actionStr = j.value("actionStr", std::string());
        e.actionNum = j.value("actionNum", 10.0f);
        m_elements.push_back(std::move(e));
    }
}

// ---------------------------------------------------------------------------
// Runtime + authoring rendering.
// ---------------------------------------------------------------------------
fitzel::Texture* UiOverlay::resolve(AssetDatabase& assets, const AssetId& id) {
    if (!id.valid()) return nullptr;
    auto it = m_texCache.find(id);
    if (it == m_texCache.end())
        it = m_texCache.emplace(id, assets.loadTexture(id)).first;
    const std::shared_ptr<fitzel::Texture>& t = it->second;
    return (t && t->isValid()) ? t.get() : nullptr;
}

// --- Focus (keyboard / gamepad navigation) ----------------------------------
// A button is navigable when it is visible and actually does something; a
// decorative button (action None) would be a dead stop in the cycle.
bool UiOverlay::navigable(const UiElement& e) {
    return e.type == UiElementType::Button && e.visible &&
           e.action != UiActionKind::None;
}

bool UiOverlay::anyButtons() const {
    for (const UiElement& e : m_elements)
        if (navigable(e)) return true;
    return false;
}

// Start on the first navigable button so something is always highlighted -- a
// menu that comes up with nothing selected leaves the pad with nothing to do.
// "First" is the first in reading order, i.e. the same order moveFocus walks:
// the element list is authoring order and says nothing about the layout, so
// picking from it would highlight an arbitrary entry.
void UiOverlay::focusFirst() {
    m_focus = -1;
    for (int i = 0; i < static_cast<int>(m_elements.size()); ++i) {
        if (!navigable(m_elements[i])) continue;
        if (m_focus < 0) { m_focus = i; continue; }
        const UiElement& best = m_elements[m_focus];
        const UiElement& cand = m_elements[i];
        const bool sameRow = std::abs(cand.offset.y - best.offset.y) <= 1.0f;
        if (( sameRow && cand.offset.x < best.offset.x) ||
            (!sameRow && cand.offset.y < best.offset.y))
            m_focus = i;
    }
}

void UiOverlay::resetRuntime() {
    m_runtimeVisible = !m_menuMode;
    m_openT = m_menuMode ? 0.0f : 1.0f;
    focusFirst();
}

void UiOverlay::setRuntimeVisible(bool v) {
    // Opening replays the entry animation, so a menu always comes up the same
    // way rather than snapping in on every re-open but the first.
    if (v && !m_runtimeVisible) { focusFirst(); m_openT = 0.0f; }
    m_runtimeVisible = v;
}

void UiOverlay::moveFocus(int dir) {
    const int n = static_cast<int>(m_elements.size());
    if (n == 0 || dir == 0) return;
    // Reading order, so "down" walks a stacked menu the way it looks: sort the
    // navigable buttons by their authored offset (top to bottom, then left to
    // right) rather than by their order in the element list, which is authoring
    // order and has nothing to do with the layout.
    std::vector<int> order;
    for (int i = 0; i < n; ++i)
        if (navigable(m_elements[i])) order.push_back(i);
    if (order.empty()) { m_focus = -1; return; }
    std::sort(order.begin(), order.end(), [this](int a, int b) {
        const UiElement& ea = m_elements[a];
        const UiElement& eb = m_elements[b];
        if (std::abs(ea.offset.y - eb.offset.y) > 1.0f) return ea.offset.y < eb.offset.y;
        return ea.offset.x < eb.offset.x;
    });
    int at = 0;
    for (int i = 0; i < static_cast<int>(order.size()); ++i)
        if (order[i] == m_focus) { at = i; break; }
    const int cnt = static_cast<int>(order.size());
    m_focus = order[((at + dir) % cnt + cnt) % cnt]; // wrap both ways
}

bool UiOverlay::activateFocus(const UiActionSink& sink) {
    if (m_focus < 0 || m_focus >= static_cast<int>(m_elements.size())) return false;
    const UiElement& e = m_elements[m_focus];
    if (!navigable(e)) return false;
    fireAction(e, sink);
    return true;
}

void UiOverlay::drawRuntime(ImDrawList* dl, const glm::vec2& vmin, const glm::vec2& vsize,
                            AssetDatabase& assets, const UiActionSink& sink) {
    if (!m_runtimeVisible) return; // a closed menu draws nothing and eats no clicks
    const ImVec2 vm(vmin.x, vmin.y), vs(vsize.x, vsize.y);
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool moved   = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    const float S = std::clamp(vs.y / 900.0f, 0.55f, 2.2f);

    // --- Menu mode: give it a stage ----------------------------------------
    // A HUD overlay is drawn bare, as it always was -- it belongs ON the game. A
    // MENU is a different thing: it stops the game being the thing you are
    // looking at, and until now it was drawn just as bare, which left authored
    // buttons floating unreadably over whatever happened to be on screen. The
    // chrome below is generated, not authored: the author still only places
    // buttons, and gets a menu that frames itself around them.
    float t = 1.0f, alpha = 1.0f, slide = 0.0f;
    if (m_menuMode) {
        m_openT = std::min(1.0f, m_openT + io.DeltaTime * 4.5f);
        t     = easeOutCubic(m_openT);
        alpha = t;
        slide = (1.0f - t) * 26.0f * S;   // the block settles up into place

        // Scrim: the game recedes. Darkest at the edges so the middle -- where a
        // centred menu sits -- keeps some of the scene showing through.
        dl->AddRectFilled(ImVec2(vm.x, vm.y), ImVec2(vm.x + vs.x, vm.y + vs.y),
                          fade(IM_COL32(4, 6, 11, 200), alpha));
        const float band = vs.y * 0.34f;
        dl->AddRectFilledMultiColor(ImVec2(vm.x, vm.y), ImVec2(vm.x + vs.x, vm.y + band),
                                    fade(IM_COL32(2, 3, 6, 170), alpha),
                                    fade(IM_COL32(2, 3, 6, 170), alpha),
                                    fade(IM_COL32(2, 3, 6, 0), alpha),
                                    fade(IM_COL32(2, 3, 6, 0), alpha));
        dl->AddRectFilledMultiColor(ImVec2(vm.x, vm.y + vs.y - band),
                                    ImVec2(vm.x + vs.x, vm.y + vs.y),
                                    fade(IM_COL32(2, 3, 6, 0), alpha),
                                    fade(IM_COL32(2, 3, 6, 0), alpha),
                                    fade(IM_COL32(2, 3, 6, 170), alpha),
                                    fade(IM_COL32(2, 3, 6, 170), alpha));

        // The frame, fitted to whatever the author placed. Skipped when the
        // elements sprawl across most of the screen -- a full-bleed menu is a
        // deliberate layout, and boxing it would only add a pointless border.
        ImVec2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
        int counted = 0;
        for (const UiElement& e : m_elements) {
            if (!e.visible) continue;
            ImVec2 a, b;
            elementRect(e, vm, vs, a, b);
            lo.x = std::min(lo.x, a.x); lo.y = std::min(lo.y, a.y);
            hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y);
            ++counted;
        }
        if (counted > 0 &&
            (hi.x - lo.x) < vs.x * 0.86f && (hi.y - lo.y) < vs.y * 0.86f) {
            const float padX = 34.0f * S, padY = 30.0f * S;
            const ImVec2 a(lo.x - padX, lo.y - padY + slide);
            const ImVec2 b(hi.x + padX, hi.y + padY + slide);
            const float r = 10.0f * S;
            // Outer glow, then the panel, then a hairline and an accent spine.
            for (int i = 4; i >= 1; --i)
                dl->AddRect(ImVec2(a.x - i * 2.0f * S, a.y - i * 2.0f * S),
                            ImVec2(b.x + i * 2.0f * S, b.y + i * 2.0f * S),
                            fade(kMenuAccent, 0.05f * alpha / i),
                            r + i * 2.0f * S, 0, 2.0f * S);
            dl->AddRectFilled(a, b, fade(IM_COL32(9, 12, 19, 236), alpha), r);
            dl->AddRect(a, b, fade(IM_COL32(255, 255, 255, 34), alpha), r, 0, 1.0f);
            dl->AddRectFilled(a, ImVec2(a.x + 3.0f * S, b.y),
                              fade(kMenuAccent, 0.9f * alpha), r,
                              ImDrawFlags_RoundCornersLeft);
            // Corner ticks: cheap, and they make a plain rectangle read as a
            // deliberate frame rather than a default one.
            const float tick = 16.0f * S, th = 2.0f * S;
            const ImU32 tc = fade(kMenuAccent, 0.55f * alpha);
            dl->AddLine(ImVec2(b.x - tick, a.y), ImVec2(b.x, a.y), tc, th);
            dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x, a.y + tick), tc, th);
            dl->AddLine(ImVec2(b.x - tick, b.y), ImVec2(b.x, b.y), tc, th);
            dl->AddLine(ImVec2(b.x, b.y - tick), ImVec2(b.x, b.y), tc, th);

            // Control hint under the frame. The close half is only claimed when
            // the menu really is on Escape -- the toggle key is authored, and a
            // hint that names the wrong key is worse than none.
            ImFont* font = ImGui::GetFont();
            const float hs = 13.0f * S;
            const char* hint = (m_toggleKey == kKeyEscape)
                                   ? "ENTER / (A)  SELECT       ESC / MENU  CLOSE"
                                   : "ENTER / (A)  SELECT";
            const float hw = font->CalcTextSizeA(hs, FLT_MAX, 0.0f, hint).x;
            shadowText(dl, font, hs,
                       ImVec2((a.x + b.x) * 0.5f - hw * 0.5f, b.y + 12.0f * S),
                       fade(IM_COL32(150, 162, 182, 255), alpha), hint);
        }
    }

    const ImVec2 shift(0.0f, slide);
    const float  pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.2f);

    for (int i = 0; i < static_cast<int>(m_elements.size()); ++i) {
        const UiElement& e = m_elements[i];
        if (!e.visible) continue;
        const fitzel::Texture* tex = resolve(assets, e.image);
        ImVec2 p0, p1;
        if (e.type == UiElementType::Button) {
            // Hit-test off the placement, not off a throwaway draw: lighting a
            // button used to draw it twice, which double-blended its background.
            elementRect(e, vm, vs, p0, p1);
            p0.y += slide; p1.y += slide;
            const bool hovered = mouse.x >= p0.x && mouse.x <= p1.x &&
                                 mouse.y >= p0.y && mouse.y <= p1.y;
            // Moving the mouse onto a button takes the focus with it, so the
            // highlight always agrees with what a click would hit.
            if (hovered && moved && navigable(e)) m_focus = i;
            const bool lit = hovered || i == m_focus;
            ImVec2 q0, q1;
            drawVisual(dl, e, vm, vs, tex, lit, q0, q1, shift, alpha);
            if (i == m_focus) {
                // Focus ring: a bright outline just outside the button, so the
                // selection is unmistakable on a pad even on a busy background
                // (the hover lift alone is too subtle from across the room). In
                // menu mode it breathes and grows a caret, because there the
                // focus is the only thing telling you what a button press does.
                const float grow = m_menuMode ? 3.0f + pulse * 2.5f * S : 3.0f;
                const float r = std::min((p1.y - p0.y) * 0.25f, 10.0f) + grow;
                if (m_menuMode) {
                    dl->AddRectFilled(p0, p1, fade(kMenuAccent, 0.14f * alpha),
                                      std::min((p1.y - p0.y) * 0.25f, 10.0f));
                    const float ch = (p1.y - p0.y) * 0.22f;
                    const float cx = p0.x - 14.0f * S, cy = (p0.y + p1.y) * 0.5f;
                    dl->AddTriangleFilled(ImVec2(cx - ch * 0.6f, cy - ch),
                                          ImVec2(cx + ch * 0.6f, cy),
                                          ImVec2(cx - ch * 0.6f, cy + ch),
                                          fade(kMenuAccent, alpha));
                }
                dl->AddRect(ImVec2(p0.x - grow, p0.y - grow),
                            ImVec2(p1.x + grow, p1.y + grow),
                            fade(kMenuAccent, alpha), r, 0, 3.0f);
            }
            if (hovered && clicked) fireAction(e, sink);
        } else {
            drawVisual(dl, e, vm, vs, tex, false, p0, p1, shift, alpha);
        }
    }
}

void UiOverlay::drawAuthoring(ImDrawList* dl, const glm::vec2& vmin, const glm::vec2& vsize,
                              AssetDatabase& assets, int selected) {
    const ImVec2 vm(vmin.x, vmin.y), vs(vsize.x, vsize.y);
    for (int i = 0; i < static_cast<int>(m_elements.size()); ++i) {
        const UiElement& e = m_elements[i];
        ImVec2 p0, p1;
        if (e.visible) {
            drawVisual(dl, e, vm, vs, resolve(assets, e.image), false, p0, p1);
        } else {
            // Still show hidden elements faintly while authoring so they can be
            // found and re-enabled.
            const float scale = vs.y / kRefHeight;
            const ImVec2 box(std::max(4.0f, e.size.x * scale), std::max(4.0f, e.size.y * scale));
            p0 = topLeft(e.anchor, vm, vs, ImVec2(e.offset.x * scale, e.offset.y * scale), box);
            p1 = ImVec2(p0.x + box.x, p0.y + box.y);
        }
        if (e.type == UiElementType::Image && !(e.image.valid())) {
            // An image with no asset: outline the box so it can be placed/sized.
            dl->AddRect(p0, p1, IM_COL32(255, 120, 255, 160), 0, 0, 1.5f);
            dl->AddLine(p0, p1, IM_COL32(255, 120, 255, 120), 1.0f);
        }
        if (i == selected) {
            const ImU32 sel = IM_COL32(120, 200, 255, 230);
            dl->AddRect(ImVec2(p0.x - 3, p0.y - 3), ImVec2(p1.x + 3, p1.y + 3), sel, 0, 0, 2.0f);
            // Corner ticks for a clearer selection read.
            const float t = 8.0f;
            dl->AddLine(ImVec2(p0.x - 3, p0.y - 3), ImVec2(p0.x - 3 + t, p0.y - 3), sel, 2.0f);
            dl->AddLine(ImVec2(p0.x - 3, p0.y - 3), ImVec2(p0.x - 3, p0.y - 3 + t), sel, 2.0f);
        }
    }
}

#ifndef FITZEL_PLAYER
// ---------------------------------------------------------------------------
// Editor panel.
// ---------------------------------------------------------------------------
namespace {
const char* kAnchorLabels[9] = {
    "Top-Left", "Top-Center", "Top-Right",
    "Mid-Left", "Center",     "Mid-Right",
    "Bottom-Left", "Bottom-Center", "Bottom-Right",
};
const char* kTypeLabels[3]   = {"Text", "Button", "Image"};
const char* kActionLabels[9] = {"None", "Load scene", "Show message",
                                "Add score", "Play sound", "Quit",
                                "Resume", "Restart level", "Graphics settings"};

// A combo that assigns one of `items` (or "(none)") to a string field.
void stringCombo(const char* label, std::string& field,
                 const std::vector<std::string>& items) {
    const std::string cur = field.empty() ? "(none)" : field;
    if (ImGui::BeginCombo(label, cur.c_str())) {
        if (ImGui::Selectable("(none)", field.empty())) field.clear();
        for (const std::string& s : items)
            if (ImGui::Selectable(s.c_str(), field == s)) field = s;
        ImGui::EndCombo();
    }
    if (!field.empty() && std::find(items.begin(), items.end(), field) == items.end())
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f), "Missing: %s", field.c_str());
}
} // namespace

void UiOverlay::drawEditorPanel(bool* open, int& sel, AssetDatabase& assets,
                                const std::vector<std::string>& scenes,
                                const std::vector<std::string>& sounds,
                                const std::function<std::string(const std::string&)>&
                                    copyToScene) {
    if (!ImGui::Begin("UI Overlay", open)) { ImGui::End(); return; }

    ImGui::TextDisabled("Screen-space 2D drawn over the scene while playing.");

    // --- HUD or menu -------------------------------------------------------
    // A menu overlay is the scene's pause/start screen: hidden until its key is
    // pressed, and while it is open the game frees the mouse and stops taking
    // gameplay input.
    {
        bool menu = m_menuMode;
        if (ImGui::Checkbox("Menu (hidden until the key below)", &menu))
            setMenuMode(menu);
        if (m_menuMode) {
            // A short list of keys that never clash with movement or the editor's
            // own shortcuts. GLFW key codes.
            struct KeyOpt { const char* name; int key; };
            static const KeyOpt kOpts[] = {
                {"Escape", kKeyEscape}, {"Tab", 258}, {"F1", 290},
                {"M", 77}, {"P", 80},
            };
            int cur = 0;
            for (int i = 0; i < IM_ARRAYSIZE(kOpts); ++i)
                if (kOpts[i].key == m_toggleKey) cur = i;
            if (ImGui::BeginCombo("Opens with", kOpts[cur].name)) {
                for (int i = 0; i < IM_ARRAYSIZE(kOpts); ++i)
                    if (ImGui::Selectable(kOpts[i].name, i == cur))
                        m_toggleKey = kOpts[i].key;
                ImGui::EndCombo();
            }
            if (m_toggleKey == kKeyEscape)
                ImGui::TextDisabled("Escape opens/closes this menu instead of\n"
                                    "quitting -- give it a button with the Quit\n"
                                    "action so the game can still be left.");
        }
    }

    // --- Copy this overlay into another scene ------------------------------
    // Writes the elements + the menu settings straight into the target scene
    // file, replacing whatever overlay it had. The scene stays closed; nothing
    // about the one being edited changes.
    if (copyToScene && !scenes.empty()) {
        ImGui::SetNextItemWidth(190.0f);
        if (ImGui::BeginCombo("##copyTarget",
                              m_copyTarget.empty() ? "(pick a scene)"
                                                   : m_copyTarget.c_str())) {
            for (const std::string& s : scenes)
                if (ImGui::Selectable(s.c_str(), s == m_copyTarget)) {
                    m_copyTarget = s;
                    m_copyStatus.clear();
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(m_copyTarget.empty() || m_elements.empty());
        if (ImGui::Button("Copy to scene")) m_copyStatus = copyToScene(m_copyTarget);
        ImGui::EndDisabled();
        if (m_elements.empty()) ImGui::TextDisabled("(nothing to copy yet)");
        else if (!m_copyStatus.empty())
            ImGui::TextDisabled("%s", m_copyStatus.c_str());
    }

    ImGui::Separator();

    // --- Add elements ------------------------------------------------------
    auto addElement = [&](UiElementType t, const char* defName) {
        UiElement e;
        e.type = t;
        e.name = defName;
        if (t == UiElementType::Button) { e.text = "Button"; e.action = UiActionKind::None; }
        else if (t == UiElementType::Image) { e.text.clear(); }
        m_elements.push_back(std::move(e));
        sel = static_cast<int>(m_elements.size()) - 1;
    };
    if (ImGui::Button("+ Text"))   addElement(UiElementType::Text, "Text");
    ImGui::SameLine();
    if (ImGui::Button("+ Button")) addElement(UiElementType::Button, "Button");
    ImGui::SameLine();
    if (ImGui::Button("+ Image"))  addElement(UiElementType::Image, "Image");

    ImGui::Separator();

    // --- Element list ------------------------------------------------------
    const int n = static_cast<int>(m_elements.size());
    if (sel >= n) sel = n - 1;
    ImGui::BeginChild("uiList", ImVec2(0, 160), true);
    for (int i = 0; i < n; ++i) {
        UiElement& e = m_elements[i];
        ImGui::PushID(i);
        bool vis = e.visible;
        if (ImGui::Checkbox("##vis", &vis)) e.visible = vis;
        ImGui::SameLine();
        char lbl[160];
        std::snprintf(lbl, sizeof(lbl), "%s  [%s]",
                      e.name.c_str(), kTypeLabels[static_cast<int>(e.type)]);
        if (ImGui::Selectable(lbl, sel == i)) sel = i;
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Reorder / duplicate / delete for the selection.
    ImGui::BeginDisabled(sel < 0 || sel >= n);
    if (ImGui::Button("Up") && sel > 0) { std::swap(m_elements[sel], m_elements[sel - 1]); --sel; }
    ImGui::SameLine();
    if (ImGui::Button("Down") && sel >= 0 && sel < n - 1) { std::swap(m_elements[sel], m_elements[sel + 1]); ++sel; }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate") && sel >= 0) {
        UiElement dup = m_elements[sel];
        dup.name += " copy";
        dup.offset += glm::vec2(20.0f, 20.0f);
        m_elements.insert(m_elements.begin() + sel + 1, dup);
        ++sel;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && sel >= 0) {
        m_elements.erase(m_elements.begin() + sel);
        if (sel >= static_cast<int>(m_elements.size())) sel = static_cast<int>(m_elements.size()) - 1;
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    // --- Property editor ---------------------------------------------------
    if (sel < 0 || sel >= static_cast<int>(m_elements.size())) {
        ImGui::TextDisabled("Select or add an element.");
        ImGui::End();
        return;
    }
    UiElement& e = m_elements[sel];

    char nameBuf[128];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", e.name.c_str());
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) e.name = nameBuf;

    int ti = static_cast<int>(e.type);
    if (ImGui::Combo("Type", &ti, kTypeLabels, 3)) e.type = static_cast<UiElementType>(ti);

    ImGui::Checkbox("Visible", &e.visible);

    int ai = static_cast<int>(e.anchor);
    if (ImGui::Combo("Anchor", &ai, kAnchorLabels, 9)) e.anchor = static_cast<UiAnchor>(ai);
    ImGui::DragFloat2("Offset (px)", &e.offset.x, 1.0f, -10000.0f, 10000.0f, "%.0f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pixels inward from the anchor, at a 1080-tall view.\n"
                          "Scales with the actual view height.");

    if (e.type != UiElementType::Text) {
        ImGui::DragFloat2("Size (px)", &e.size.x, 1.0f, 4.0f, 10000.0f, "%.0f");
    }

    // Text label (Text + Button).
    if (e.type == UiElementType::Text || e.type == UiElementType::Button) {
        char txt[512];
        std::snprintf(txt, sizeof(txt), "%s", e.text.c_str());
        if (ImGui::InputText("Text", txt, sizeof(txt))) e.text = txt;
        ImGui::SliderFloat("Font scale", &e.fontScale, 0.25f, 5.0f, "%.2f x");
        ImGui::ColorEdit4("Text colour", &e.color.x);
    }

    // Image slot (Image + Button icon). Drop a Texture from the Assets browser.
    if (e.type == UiElementType::Image || e.type == UiElementType::Button) {
        std::string slot = "(none)";
        if (e.image.valid()) {
            const AssetDatabase::Entry* te = assets.entry(e.image);
            slot = te ? te->relPath : e.image.toString();
        }
        ImGui::Text("Image:");
        ImGui::SameLine();
        ImGui::Button((slot + "##uiimg").c_str(), ImVec2(-60.0f, 0.0f));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ASSET_GUID")) {
                const AssetId gid = AssetId::fromString(std::string(
                    static_cast<const char*>(pl->Data), pl->DataSize));
                if (assets.typeForId(gid) == fitzel::AssetType::Texture) e.image = gid;
            }
            ImGui::EndDragDropTarget();
        }
        if (e.image.valid()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##uiimg")) e.image = {};
        }
        ImGui::TextDisabled("Drag a texture here from the Assets panel.");
    }

    // Button-only: background + click action.
    if (e.type == UiElementType::Button) {
        ImGui::ColorEdit4("Background", &e.bgColor.x);
        ImGui::TextDisabled("(background shows only when no image is set)");
        ImGui::Separator();
        int aci = static_cast<int>(e.action);
        if (ImGui::Combo("On click", &aci, kActionLabels, IM_ARRAYSIZE(kActionLabels)))
            e.action = static_cast<UiActionKind>(aci);
        switch (e.action) {
            case UiActionKind::LoadScene:   stringCombo("Scene", e.actionStr, scenes); break;
            case UiActionKind::PlaySound:   stringCombo("Sound", e.actionStr, sounds); break;
            case UiActionKind::ShowMessage: {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "%s", e.actionStr.c_str());
                if (ImGui::InputText("Message", msg, sizeof(msg))) e.actionStr = msg;
                break;
            }
            case UiActionKind::AddScore:
                ImGui::DragFloat("Points", &e.actionNum, 1.0f, -100000.0f, 100000.0f, "%.0f");
                break;
            case UiActionKind::Resume:
                ImGui::TextDisabled("(closes this menu -- same as pressing its key)");
                break;
            case UiActionKind::Restart:
                ImGui::TextDisabled("(replays the level from how it stood when Play\n"
                                    " began; unsaved editor edits are kept)");
                break;
            case UiActionKind::None:
            case UiActionKind::Quit:
                break;
        }
    }

    ImGui::End();
}
#endif // FITZEL_PLAYER
