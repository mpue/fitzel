// animcheck -- does a keyframe track hold the value it was given, and find its
// property again after a save?
//
// The animation system is one indirection away from everything it touches: a
// track does not hold a pointer to a float, it holds "entity 7, component
// `spin`, key `speed`, channel 0" and looks that up in the live scene every time
// it is played. That is what makes any inspector property animatable without
// registering it -- and it is also why nothing here fails loudly. A track that
// resolves to the wrong field writes a plausible number into a plausible place;
// a track that resolves to nothing does nothing at all, on a property that
// simply sits still. Both look, on screen, like an author who has not keyed
// anything yet.
//
// So the parts that cannot be seen are measured: that a value read out of a
// property comes back the same, that sampling between two keys lands where the
// arithmetic says, that a discrete property steps instead of ramping, that the
// ends of a clip clamp rather than run on, and that a clip written to JSON and
// read back binds to the same fields and samples identically.
//
// No GL, no window, no assets: this is arithmetic and a lookup.
//
//   build/release/bin/animcheck.exe
// Exits non-zero if any check fails.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../src/AnimSystem.hpp"
#include "../src/Component.hpp"
#include "../src/PropertyMeta.hpp"
#include "../src/SceneTypes.hpp"

namespace {

int g_fails = 0;

void fail(const char* what, const std::string& detail) {
    std::printf("[FAIL] %s: %s\n", what, detail.c_str());
    ++g_fails;
}
void pass(const char* what, const std::string& detail) {
    std::printf("  ok   %s -- %s\n", what, detail.c_str());
}
void check(bool ok, const char* what, const std::string& detail) {
    if (ok) pass(what, detail); else fail(what, detail);
}
void near(float got, float want, const char* what, float eps = 1e-4f) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "got %.5f, want %.5f", got, want);
    check(std::fabs(got - want) <= eps, what, buf);
}

// The property table entry for one key, so a check can talk about "Position"
// rather than about an index into a table.
const Property& entityProp(const char* key) {
    for (const Property& p : entityProperties())
        if (p.key == key) return p;
    std::printf("[FAIL] no entity property '%s'\n", key);
    ++g_fails;
    static Property dummy;
    return dummy;
}

const Property& compProp(const ComponentBase& c, const char* key) {
    for (const Property& p : c.props())
        if (p.key == key) return p;
    std::printf("[FAIL] no '%s' property on component '%s'\n", key, c.typeId());
    ++g_fails;
    static Property dummy;
    return dummy;
}

std::vector<Entity> makeScene() {
    std::vector<Entity> es;
    Entity box;
    box.type = EntityType::Box;
    box.id   = 7;
    box.name = "Box";
    box.localCenter = glm::vec3(0.0f);
    box.components.items.push_back(std::make_unique<SpinComponent>());
    es.push_back(std::move(box));
    return es;
}

} // namespace

int main() {
    std::printf("animcheck\n");

    // --- Reading and writing a property as a number -------------------------
    {
        std::vector<Entity> es = makeScene();
        const Property& pos = entityProp("center");
        es[0].localCenter = glm::vec3(1.0f, 2.0f, 3.0f);
        near(anim::readValue(pos, &es[0], 1), 2.0f, "read Vec3 channel y");
        anim::writeValue(pos, &es[0], 2, -4.0f);
        near(es[0].localCenter.z, -4.0f, "write Vec3 channel z");
        check(anim::componentCount(pos.kind) == 3, "a Vec3 is three channels",
              std::to_string(anim::componentCount(pos.kind)));

        const Property& act = entityProp("active");
        es[0].active = false;
        near(anim::readValue(act, &es[0], 0), 0.0f, "read a bool as 0");
        anim::writeValue(act, &es[0], 0, 1.0f);
        check(es[0].active, "write a bool from 1", es[0].active ? "true" : "false");
        check(anim::isDiscrete(act.kind), "a bool is discrete", "");
        check(!anim::isAnimatable(entityProp("name").kind), "text cannot be keyed", "");
    }

    // --- Keying from the live value, and sampling between keys --------------
    {
        std::vector<Entity> es = makeScene();
        anim::Clip c;
        c.fps = 10.0f;
        const Property& pos = entityProp("center");

        es[0].localCenter = glm::vec3(0.0f, 0.0f, 0.0f);
        anim::keyProperty(c, 7, "", pos, &es[0], 0.0f);
        es[0].localCenter = glm::vec3(10.0f, 0.0f, 0.0f);
        anim::keyProperty(c, 7, "", pos, &es[0], 2.0f);

        check(static_cast<int>(c.tracks.size()) == 3,
              "keying a Vec3 makes three tracks",
              std::to_string(c.tracks.size()));
        check(anim::isAnimated(c, 7, "", pos), "the property reads as animated", "");
        check(anim::isKeyedAt(c, 7, "", pos, 2.0f), "and as keyed at 2s", "");
        check(!anim::isKeyedAt(c, 7, "", pos, 1.0f), "but not at 1s", "");

        // Halfway between the two keys is halfway between the two values.
        anim::apply(c, es, 1.0f);
        near(es[0].localCenter.x, 5.0f, "linear sample at the midpoint");

        // ...and before/after the clip it CLAMPS rather than extrapolating. An
        // object still drifting after the last key is the failure this rules out.
        anim::apply(c, es, -3.0f);
        near(es[0].localCenter.x, 0.0f, "before the first key: held");
        anim::apply(c, es, 99.0f);
        near(es[0].localCenter.x, 10.0f, "after the last key: held");

        // A key written where one already sits replaces it rather than stacking
        // a second one at the same moment (which would make sampling depend on
        // insertion order).
        es[0].localCenter.x = 3.0f;
        anim::keyProperty(c, 7, "", pos, &es[0], 2.0f);
        const anim::Track* tx = anim::findTrack(c, 7, "", "center", 0);
        check(tx && tx->keys.size() == 2, "re-keying the same time replaces",
              tx ? std::to_string(tx->keys.size()) : "no track");
        anim::apply(c, es, 2.0f);
        near(es[0].localCenter.x, 3.0f, "and the new value is what plays");
    }

    // --- Snapping -----------------------------------------------------------
    {
        anim::Clip c;
        c.fps = 10.0f;
        near(anim::snap(c, 0.2971f), 0.3f, "a shaky 0.2971 lands on 0.3");
        near(anim::snap(c, -5.0f), 0.0f, "and nothing lands before zero");
        c.fps = 4.0f;
        near(anim::snap(c, 0.6f), 0.5f, "a coarser grid rounds further");
    }

    // --- Discrete properties step -------------------------------------------
    {
        std::vector<Entity> es = makeScene();
        anim::Clip c;
        const Property& act = entityProp("active");
        es[0].active = false;
        anim::keyProperty(c, 7, "", act, &es[0], 0.0f);
        es[0].active = true;
        anim::keyProperty(c, 7, "", act, &es[0], 1.0f);

        const anim::Track* t = anim::findTrack(c, 7, "", "active", 0);
        check(t && t->interp == anim::Interp::Step, "a bool track is created stepping",
              t ? "step" : "no track");
        anim::apply(c, es, 0.9f);
        check(!es[0].active, "still off just before the key", es[0].active ? "on" : "off");
        anim::apply(c, es, 1.0f);
        check(es[0].active, "on at the key", es[0].active ? "on" : "off");
    }

    // --- A component's property, through its type id ------------------------
    {
        std::vector<Entity> es = makeScene();
        auto* spin = es[0].components.get<SpinComponent>();
        anim::Clip c;
        const Property& sp = compProp(*spin, "speed");

        spin->speed = 0.0f;
        anim::keyProperty(c, 7, "spin", sp, spin, 0.0f);
        spin->speed = 180.0f;
        anim::keyProperty(c, 7, "spin", sp, spin, 4.0f);
        anim::apply(c, es, 2.0f);
        near(es[0].components.get<SpinComponent>()->speed, 90.0f,
             "a component field animates through its type id");

        // A track whose component is not there resolves to nothing and is
        // skipped -- not a crash, and not a write into the entity instead.
        anim::Track ghost;
        ghost.entityId = 7; ghost.comp = "nosuch"; ghost.key = "speed";
        ghost.keys.push_back({0.0f, 1.0f});
        c.tracks.push_back(ghost);
        check(!anim::bind(c.tracks.back(), es), "a missing component binds to nothing", "");
        anim::apply(c, es, 2.0f);   // must not crash or write anywhere
        pass("applying a clip with an unresolvable track", "no write, no crash");
        c.tracks.pop_back();
    }

    // --- The preview gives the scene back ------------------------------------
    {
        std::vector<Entity> es = makeScene();
        anim::Clip c;
        const Property& pos = entityProp("center");
        es[0].localCenter = glm::vec3(0.0f, 5.0f, 0.0f);
        anim::keyProperty(c, 7, "", pos, &es[0], 0.0f);
        es[0].localCenter = glm::vec3(0.0f, 25.0f, 0.0f);
        anim::keyProperty(c, 7, "", pos, &es[0], 2.0f);

        // What the author last typed is where the object stands now.
        es[0].localCenter = glm::vec3(0.0f, 5.0f, 0.0f);
        anim::Player p;
        anim::beginPreview(c, es, p);
        anim::apply(c, es, 2.0f);
        near(es[0].localCenter.y, 25.0f, "the preview poses the scene");
        anim::endPreview(c, es, p);
        near(es[0].localCenter.y, 5.0f, "and stopping puts it back");
        check(!p.preview && !p.playing, "and leaves no preview running", "");
    }

    // --- Round trip through JSON --------------------------------------------
    {
        std::vector<Entity> es = makeScene();
        auto* spin = es[0].components.get<SpinComponent>();
        anim::Clip c;
        c.duration = 6.5f; c.fps = 25.0f; c.speed = 1.5f;
        c.loop = true; c.playOnStart = true;

        const Property& pos = entityProp("center");
        es[0].localCenter = glm::vec3(0.0f);
        anim::keyProperty(c, 7, "", pos, &es[0], 0.0f);
        es[0].localCenter = glm::vec3(8.0f, 0.0f, 0.0f);
        anim::keyProperty(c, 7, "", pos, &es[0], 4.0f);
        const Property& sp = compProp(*spin, "speed");
        spin->speed = 30.0f;
        anim::keyProperty(c, 7, "spin", sp, spin, 1.0f);
        anim::track(c, 7, "", pos, 0).interp = anim::Interp::Smooth;

        c.name = "Door opens";
        nlohmann::json j;
        std::vector<anim::Clip> lib{c};
        anim::save(j, lib);
        std::vector<anim::Clip> loaded;
        anim::load(j, loaded);
        check(loaded.size() == 1, "the clip list survives the save",
              std::to_string(loaded.size()));
        if (loaded.empty()) loaded.push_back(anim::Clip{});
        const anim::Clip& back = loaded[0];
        check(back.name == "Door opens", "and so does its name", back.name);
        check(anim::findClip(loaded, "Door opens") == 0, "which is how it is found",
              std::to_string(anim::findClip(loaded, "Door opens")));

        check(back.tracks.size() == c.tracks.size(), "every track survives the save",
              std::to_string(back.tracks.size()) + " of " + std::to_string(c.tracks.size()));
        near(back.duration, 6.5f, "the clip's length survives");
        near(back.fps, 25.0f, "its grid survives");
        near(back.speed, 1.5f, "its speed survives");
        check(back.loop && back.playOnStart, "and both of its flags", "");
        const anim::Track* bx = anim::findTrack(back, 7, "", "center", 0);
        check(bx && bx->interp == anim::Interp::Smooth, "as does a track's curve", "");

        // The real question: does the reloaded clip still find the same fields?
        std::vector<Entity> es2 = makeScene();
        anim::apply(back, es2, 2.0f);
        near(es2[0].localCenter.x, 4.0f, "the reloaded clip drives the same property");
        anim::apply(back, es2, 1.0f);
        near(es2[0].components.get<SpinComponent>()->speed, 30.0f,
             "and the same component field");
    }

    // --- A scene with no animation at all ------------------------------------
    {
        nlohmann::json j;                 // no "anim" key: every scene saved so far
        std::vector<anim::Clip> lib{anim::Clip{}};
        anim::load(j, lib);
        check(lib.empty(), "a scene with no animation loads no clips",
              std::to_string(lib.size()));
        std::vector<Entity> es = makeScene();
        anim::apply(anim::Clip{}, es, 1.0f);
        pass("applying an empty clip", "no write, no crash");

        // ...and the shape written BEFORE clips had names: one clip inline under
        // "anim". The first scenes anyone animated are in that form, and they
        // have to keep their work.
        nlohmann::json old;
        old["anim"] = nlohmann::json{
            {"duration", 4.0f},
            {"tracks", nlohmann::json::array({
                nlohmann::json{{"e", 7}, {"c", ""}, {"k", "center"}, {"i", 0},
                               {"p", 0}, {"keys", "0 0 2 6"}}})}};
        std::vector<anim::Clip> migrated;
        anim::load(old, migrated);
        check(migrated.size() == 1, "a pre-clips scene loads as one clip",
              std::to_string(migrated.size()));
        if (!migrated.empty()) {
            check(migrated[0].name == "Animation", "under a default name",
                  migrated[0].name);
            std::vector<Entity> es2 = makeScene();
            anim::apply(migrated[0], es2, 1.0f);
            near(es2[0].localCenter.x, 3.0f, "and it still drives its property");
        }
    }

    std::printf(g_fails ? "\n%d check(s) failed\n" : "\nall checks passed\n", g_fails);
    return g_fails ? 1 : 0;
}
