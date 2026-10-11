// treefellcheck -- trees felled one at a time (game.clearTrees, a tank rolling
// through a wood) must leave the forest exactly as a full regeneration with the
// same clearings would, and right away: TreeField::configure filters the
// resident tiles in place when all that changed is new discs. Streams a field,
// fells a handful of trunks, and compares against a field built from scratch
// with those discs. Exits non-zero on any difference.
//   build/linux/bin/treefellcheck
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "../src/TreeField.hpp"

namespace {

using Tree = std::array<float, 5>;

std::vector<Tree> all(const TreeField& f) {
    std::vector<std::vector<float>> per(1);
    f.gather(glm::vec2(0.0f), -1.0f, per);
    std::vector<Tree> out;
    for (std::size_t k = 0; k + 5 <= per[0].size(); k += 5)
        out.push_back({per[0][k], per[0][k + 1], per[0][k + 2], per[0][k + 3], per[0][k + 4]});
    std::sort(out.begin(), out.end());
    return out;
}

// Stream until nothing is pending and a few updates in a row change nothing.
void settle(TreeField& f, glm::vec2 eye) {
    int quiet = 0;
    for (int n = 0; n < 4000 && quiet < 5; ++n) {
        const bool changed = f.update(eye);
        quiet = (!changed && f.pendingTiles() == 0) ? quiet + 1 : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace

int main() {
    TreeField::Inputs in;
    in.eco.enabled = true;
    in.eco.cover   = 0.9f;
    in.species.push_back({1.0f, 9.0f, false});
    const glm::vec2 eye(40.0f, -25.0f);

    TreeField a;
    a.radius = 300.0f;
    a.configure(in);
    settle(a, eye);
    const std::vector<Tree> before = all(a);
    if (before.size() < 100) {
        std::printf("FAIL: only %zu trees to fell from\n", before.size());
        return 1;
    }

    // Fell every 37th tree, a few per "frame", as a game would.
    TreeField::Inputs cut = in;
    int felled = 0, failures = 0;
    for (std::size_t k = 0; k < before.size(); k += 37) {
        cut.wet.push_back({before[k][0], before[k][2], 0.6f});
        ++felled;
        if (felled % 4 == 0) {
            a.configure(cut);
            if (!a.update(eye)) { std::printf("FAIL: felling did not report a change\n"); ++failures; }
        }
    }
    a.configure(cut);
    a.update(eye);
    const std::vector<Tree> now = all(a);   // before any tile is rebuilt

    TreeField b;
    b.radius = a.radius;
    b.configure(cut);
    settle(b, eye);
    const std::vector<Tree> ref = all(b);

    settle(a, eye);
    const std::vector<Tree> later = all(a);

    std::printf("trees %zu, felled %d, left %zu (reference %zu, after streaming %zu)\n",
                before.size(), felled, now.size(), ref.size(), later.size());
    if (now != ref)   { std::printf("FAIL: felled in place != regenerated\n"); ++failures; }
    if (later != ref) { std::printf("FAIL: streaming on changed the felled field\n"); ++failures; }
    if (before.size() - now.size() < static_cast<std::size_t>(felled)) {
        std::printf("FAIL: fewer trees gone than felled\n"); ++failures;
    }

    // A real change (the cover) still regrows everything.
    TreeField::Inputs sparse = cut;
    sparse.eco.cover = 0.3f;
    a.configure(sparse);
    settle(a, eye);
    TreeField c;
    c.radius = a.radius;
    c.configure(sparse);
    settle(c, eye);
    if (all(a) != all(c)) { std::printf("FAIL: a full change did not regrow the field\n"); ++failures; }

    std::printf(failures ? "treefellcheck: %d failure(s)\n" : "treefellcheck: OK\n", failures);
    return failures ? 1 : 0;
}
