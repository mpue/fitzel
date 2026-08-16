#include "LoadingScreen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <glad/gl.h>
#include <imgui.h>

#include <fitzel/Version.hpp>
#include <fitzel/core/Window.hpp>
#include <fitzel/ui/Gui.hpp>

namespace loadingscreen {

namespace {

ImU32 col(const glm::vec3& c, float a = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, a));
}

// Where the background image lands inside `pos`..`size` for a given fit. Cover
// and Stretch fill the frame (Cover keeps the aspect and lets the overflow be
// clipped); Contain shrinks to fit but never blows a small image up past its
// native size, which only ever makes it blurry.
void fitRect(Fit fit, const ImVec2& pos, const ImVec2& size, float iw, float ih,
             ImVec2& outMin, ImVec2& outMax) {
    if (iw <= 0.0f || ih <= 0.0f) { outMin = pos; outMax = pos; return; }
    float w = size.x, h = size.y;
    if (fit == Fit::Contain) {
        const float s = std::min({1.0f, size.x / iw, size.y / ih});
        w = iw * s; h = ih * s;
    } else if (fit == Fit::Cover) {
        const float s = std::max(size.x / iw, size.y / ih);
        w = iw * s; h = ih * s;
    }
    outMin = ImVec2(pos.x + (size.x - w) * 0.5f, pos.y + (size.y - h) * 0.5f);
    outMax = ImVec2(outMin.x + w, outMin.y + h);
}

} // namespace

void Screen::setProjectFolder(std::string folder) {
    if (folder == m_projectFolder) return;
    m_projectFolder = std::move(folder);
    m_bgLoaded.clear();
    m_bgTried = false;
}

void Screen::ensureBackground() {
    // Configured image, else the engine splash -- so a project that only ever set
    // a splash still gets a picture behind its bar.
    std::string want;
    if (!m_style.background.empty()) {
        want = m_projectFolder.empty() ? m_style.background
                                       : m_projectFolder + "/" + m_style.background;
    } else {
        want = "assets/splash.png";
    }
    if (want == m_bgLoaded && (m_bg.isValid() || m_bgTried)) return;

    m_bg      = fitzel::Texture::fromFile(want, /*flipVertically=*/false);
    m_bgLoaded = want;
    m_bgTried  = true; // a miss is remembered: no decode attempt per frame
}

void Screen::release() {
    m_bg      = fitzel::Texture{};
    m_bgLoaded.clear();
    m_bgTried = false; // forget the miss too, so the next draw really retries
}

void Screen::drawInto(ImDrawList* dl, const ImVec2& pos, const ImVec2& size,
                      float progress, const std::string& label) {
    if (!dl || size.x <= 1.0f || size.y <= 1.0f) return;
    ensureBackground();

    const ImVec2 br(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, br, col(m_style.back));

    if (m_bg.isValid()) {
        ImVec2 a, b;
        fitRect(m_style.fit, pos, size, static_cast<float>(m_bg.width()),
                static_cast<float>(m_bg.height()), a, b);
        // Clipped, because Cover deliberately overflows the frame -- and in the
        // settings preview the frame is a small box in the middle of a dialog.
        dl->PushClipRect(pos, br, true);
        dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(m_bg.id())), a, b);
        dl->PopClipRect();
    }

    ImFont*     font = ImGui::GetFont();
    const float base = ImGui::GetFontSize();
    // The preview is a fraction of the real frame, so text and bar are scaled by
    // its height. Without this, a 1440p screen's layout previewed at 200px would
    // be all text and no picture, and every choice would be made against a lie.
    const float k = std::clamp(size.y / 720.0f, 0.35f, 3.0f);

    if (!m_style.title.empty()) {
        const float sz = base * m_style.titleScale * k;
        const ImVec2 ts =
            font->CalcTextSizeA(sz, FLT_MAX, 0.0f, m_style.title.c_str());
        dl->AddText(font, sz,
                    ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + 40.0f * k),
                    col(m_style.textColor), m_style.title.c_str());
    }

    // --- The bar -------------------------------------------------------------
    const float barW = std::clamp(m_style.barWidth, 0.1f, 1.0f) * size.x;
    const float barH = std::max(2.0f, m_style.barHeight * k);
    const float inset = m_style.barInset * k;
    float barY = pos.y + size.y - inset - barH;          // Bottom
    if (m_style.barAt == BarAt::Center) barY = pos.y + (size.y - barH) * 0.5f;
    else if (m_style.barAt == BarAt::Top) barY = pos.y + inset;
    const float barX = pos.x + (size.x - barW) * 0.5f;

    const float round = std::min(m_style.barRound * k, barH * 0.5f);
    const ImVec2 t0(barX, barY), t1(barX + barW, barY + barH);
    dl->AddRectFilled(t0, t1, col(m_style.barBack, 0.85f), round);
    const float frac = std::clamp(progress, 0.0f, 1.0f);
    if (frac > 0.0f) {
        // At least a visible sliver: a bar that reads 0 for the first second
        // looks stuck rather than starting.
        const float w = std::max(frac * barW, barH);
        dl->AddRectFilled(t0, ImVec2(barX + std::min(w, barW), barY + barH),
                          col(m_style.barColor), round);
    }

    // --- Label row, sitting on top of the bar --------------------------------
    const float lineSz = base * k;
    const float lineY  = barY - lineSz - 8.0f * k;
    if (m_style.showLabel && !label.empty()) {
        dl->AddText(font, lineSz, ImVec2(barX, lineY), col(m_style.textColor),
                    label.c_str());
    }
    if (m_style.showPercent) {
        char pct[16];
        std::snprintf(pct, sizeof pct, "%d%%", static_cast<int>(frac * 100.0f + 0.5f));
        const ImVec2 ps = font->CalcTextSizeA(lineSz, FLT_MAX, 0.0f, pct);
        dl->AddText(font, lineSz, ImVec2(barX + barW - ps.x, lineY),
                    col(m_style.textColor), pct);
    }
    if (m_style.showVersion) {
        const float vs = lineSz * 0.85f;
        const ImVec2 vsz =
            font->CalcTextSizeA(vs, FLT_MAX, 0.0f, fitzel::kVersionFull);
        dl->AddText(font, vs,
                    ImVec2(barX + barW - vsz.x, barY + barH + 6.0f * k),
                    col(m_style.textColor, 0.45f), fitzel::kVersionFull);
    }
}

void Screen::frame(fitzel::Window& window, fitzel::Gui& gui, float progress,
                   const std::string& label) {
    // Pumping events matters as much as the picture: without it Windows marks
    // the window unresponsive and paints its own white rectangle over it, which
    // is exactly the frozen frame this screen exists to replace.
    window.pollEvents();
    int w = 0, h = 0;
    window.framebufferSize(w, h);
    if (w <= 0 || h <= 0) return; // minimised: nothing to draw into
    glViewport(0, 0, w, h);
    glClearColor(m_style.back.r, m_style.back.g, m_style.back.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    gui.beginFrame();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    drawInto(ImGui::GetBackgroundDrawList(), vp->Pos, vp->Size, progress, label);
    gui.endFrame();
    window.swapBuffers();
}

} // namespace loadingscreen
