#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Material.hpp>

// Holes in a figure (game.wound): real ones, not pictures of them. A wound is a
// ball in the figure's REST pose; lit.frag cuts the skin away inside it, frays the
// edge, soaks what is round it and lets blood run down from it. The back of the
// skin on the far side of the body is what you see through the hole, as raw meat
// -- and through two holes in line, whatever stands behind.
//
// The rest pose because that is where a hole stays put: posed, the skin stretches
// and slides under any ball hung on a bone, and an arm passing in front would be
// cut as well. The rest position reaches the shader in the vertex's paint slot,
// which nothing else reads on a figure, and only on a figure that has wounds.
namespace wounds {

// Keep in step with kMaxWounds in lit.frag.
constexpr int kMax = 24;

struct Wound {
    glm::vec4 at{0.0f};   // rest pose x, y, z; radius in rest units
    float     born = 0.0f;
};
using List = std::vector<Wound>;

inline float clock() {
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration<float>(steady_clock::now() - start).count();
}

// Every figure's holes, by entity id.
class Book {
public:
    // A shot at `rest`, `radius` big. Into one already there it makes that one
    // bigger, up to `most`: what is hit again and again opens up.
    void add(int figure, const glm::vec3& rest, float radius, float most) {
        List& list = m_figures[figure];
        for (Wound& w : list) {
            const glm::vec3 c(w.at);
            if (glm::length(c - rest) > (w.at.w + radius) * 0.75f) continue;
            w.at.w = std::min(std::max(most, w.at.w), std::sqrt(w.at.w * w.at.w + radius * radius) * 1.05f);
            return;
        }
        Wound nw;
        nw.at = glm::vec4(rest, radius);
        nw.born = clock();
        if (static_cast<int>(list.size()) < kMax) {
            list.push_back(nw);
            return;
        }
        *std::min_element(list.begin(), list.end(),
                          [](const Wound& a, const Wound& b) { return a.born < b.born; }) = nw;
    }
    const List* of(int figure) const {
        const auto it = m_figures.find(figure);
        return it == m_figures.end() ? nullptr : &it->second;
    }
    void clear() { m_figures.clear(); }

private:
    std::unordered_map<int, List> m_figures;
};

// A material for something with these wounds: it cuts them, and shows its inside.
inline void dress(fitzel::Material& m, const List& list) {
    static const std::vector<std::string> at = [] {
        std::vector<std::string> v;
        for (int i = 0; i < kMax; ++i) v.push_back("uWound[" + std::to_string(i) + "]");
        return v;
    }();
    static const std::vector<std::string> age = [] {
        std::vector<std::string> v;
        for (int i = 0; i < kMax; ++i) v.push_back("uWoundAge[" + std::to_string(i) + "]");
        return v;
    }();
    const float now = clock();
    const std::size_t n = std::min<std::size_t>(list.size(), kMax);
    m.set("uWounds", static_cast<int>(n)).set("uDoubleSided", 1);
    for (std::size_t i = 0; i < n; ++i) {
        m.set(at[i], list[i].at);
        m.set(age[i], now - list[i].born);
    }
}

} // namespace wounds
