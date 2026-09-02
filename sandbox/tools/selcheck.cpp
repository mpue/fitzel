// selcheck -- does the selection keep its invariant, whatever is done to it?
//
// The editor's selection is two answers at once: the ACTIVE object (one, drives
// the Inspector and the gizmo) and the SET (many, what the batch operations act
// on). The set is empty for a plain single selection, so the dozens of paths
// that only ever care about one object can ignore it -- and that shortcut is
// exactly what makes the pair easy to get wrong. The invariant is: if the set is
// non-empty it contains the active object's id, and only ids that still exist.
//
// It used to be restored once per frame by a repair pass, because ~20 sites
// open-coded "select this entity" as a bare index assignment and none of them
// touched the set. Those sites call select() now, so the invariant holds at the
// point of change instead of being swept up afterwards -- but "holds at the
// point of change" is a claim about every method, and nothing in the editor
// fails visibly when it stops being true. A stale id in the set means Duplicate
// quietly copies an object that is no longer there, or Delete takes one the
// author never picked. That is not something you catch by looking at the screen.
//
// So every operation is played out here against a synthetic entity list and the
// invariant is checked after each one, including the awkward cases: toggling the
// active object away, a set that collapses back to one, and entities deleted out
// from under a selection that still names them.
//
// No GL, no window, no assets: this is bookkeeping about a list.
//
//   build/release/bin/selcheck.exe
// Exits non-zero if any check fails.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/Selection.hpp"

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
    ok ? pass(what, detail) : fail(what, detail);
}

// Entities 10, 20, 30, 40 -- ids deliberately unequal to their indices, so a
// method that confuses the two shows up instead of accidentally working.
std::vector<Entity> makeEntities() {
    std::vector<Entity> es;
    for (int i = 0; i < 4; ++i) {
        Entity e;
        e.id   = (i + 1) * 10;
        e.name = "e" + std::to_string(e.id);
        es.push_back(std::move(e));
    }
    return es;
}

std::string show(const Selection& s) {
    std::string out = "active=" + std::to_string(s.index()) +
                      " (id " + std::to_string(s.activeId()) + ") set={";
    for (std::size_t i = 0; i < s.multi().size(); ++i)
        out += (i ? "," : "") + std::to_string(s.multi()[i]);
    return out + "} count=" + std::to_string(s.count());
}

// The invariant itself, as one predicate: a non-empty set holds the active
// object and nothing that has gone away, and never just one item (that is a
// plain single selection, not a set of one).
bool invariantHolds(const Selection& s, const std::vector<Entity>& es) {
    if (s.multi().empty()) return true;
    if (s.multi().size() < 2) return false;
    if (std::find(s.multi().begin(), s.multi().end(), s.activeId()) == s.multi().end())
        return false;
    for (int id : s.multi())
        if (std::none_of(es.begin(), es.end(),
                         [&](const Entity& e) { return e.id == id; }))
            return false;
    return true;
}

} // namespace

int main() {
    std::printf("selcheck -- the selection's invariant\n\n");

    // --- A plain click selects exactly one thing ---------------------------
    {
        auto es = makeEntities();
        Selection s(es);
        check(!s.valid() && s.count() == 0 && s.ids().empty(),
              "empty to start", show(s));

        s.select(30);
        const std::vector<int> want30{30};
        check(s.index() == 2 && s.activeId() == 30 && s.multi().empty() &&
              s.count() == 1 && s.ids() == want30,
              "select(id) resolves the id to its index", show(s));

        // The point of the whole exercise: selecting one thing drops the set,
        // at the moment of the click and not a frame later.
        s.toggle(10);
        s.select(40);
        check(s.multi().empty() && s.activeId() == 40,
              "select() drops an existing set", show(s));
    }

    // --- Ctrl+click builds and unbuilds a set ------------------------------
    {
        auto es = makeEntities();
        Selection s(es);
        s.select(10);
        s.toggle(20);
        check(s.count() == 2 && s.contains(10) && s.contains(20) && s.activeId() == 20,
              "toggle seeds the set from the active object", show(s));
        check(invariantHolds(s, es), "invariant after toggle", show(s));

        s.toggle(30);
        check(s.count() == 3 && s.activeId() == 30, "toggle adds", show(s));

        // Toggling the ACTIVE object away has to promote a survivor, or the
        // Inspector is left pointing at something no longer selected.
        s.toggle(30);
        check(s.count() == 2 && s.contains(s.activeId()) && s.activeId() != 30,
              "toggling the active object away promotes a survivor", show(s));
        check(invariantHolds(s, es), "invariant after removing the active", show(s));

        // Down to one: that is a plain single selection, not a set of one.
        s.toggle(20);
        check(s.multi().empty() && s.count() == 1 && s.activeId() == 10,
              "a set of one collapses to a single selection", show(s));
        check(invariantHolds(s, es), "invariant after collapse", show(s));
    }

    // --- Box-select adds a batch, keeping what was there --------------------
    {
        auto es = makeEntities();
        Selection s(es);
        s.select(10);
        s.addMany({20, 30});
        check(s.count() == 3 && s.contains(10) && s.contains(30) && s.activeId() == 30,
              "addMany keeps the current selection", show(s));
        check(invariantHolds(s, es), "invariant after addMany", show(s));

        // A box dragged over the same objects twice must not list them twice:
        // Duplicate would make two copies of each.
        s.addMany({20, 30});
        check(s.multi().size() == 3, "addMany does not duplicate ids", show(s));

        s.addMany({});
        check(s.count() == 3, "an empty box changes nothing", show(s));
    }

    // --- Entities deleted out from under the selection ----------------------
    {
        auto es = makeEntities();
        Selection s(es);

        // Erase 20 and 30, leaving the active object (10) alive, then rebuild a
        // set that still names them -- what a delete elsewhere leaves behind.
        es.erase(std::remove_if(es.begin(), es.end(),
                                [](const Entity& e) { return e.id == 20 || e.id == 30; }),
                 es.end());
        s.select(10);
        s.addMany({20, 30});
        // Adding objects that no longer exist must not take down the ones that
        // do: 10 was selected before the call and is still here.
        check(s.activeId() == 10 && s.valid(),
              "a stale id in addMany does not clear the selection", show(s));
        s.normalize();
        check(invariantHolds(s, es), "normalize drops ids whose entities are gone", show(s));
        check(!s.contains(20) && !s.contains(30), "the dead ids are really gone", show(s));
        check(s.activeId() == 10, "the surviving selection is kept", show(s));

        // The active object itself going away clears everything: there is no
        // sensible survivor to promote to when the anchor is what was deleted.
        auto es2 = makeEntities();
        Selection t(es2);
        t.select(10);
        t.toggle(20);
        es2.clear();
        t.normalize();
        check(t.multi().empty() && t.index() == -1 && !t.valid(),
              "an emptied scene empties the selection, index included", show(t));
    }

    // --- The index/id split -------------------------------------------------
    {
        auto es = makeEntities();
        Selection s(es);
        s.select(40);
        const int before = s.index();
        // Reordering moves the index but not the id. index() is a snapshot;
        // activeId() is the thing that survives -- which is why the SET holds
        // ids and only the active object is kept as an index.
        std::reverse(es.begin(), es.end());
        check(s.index() == before && es[s.index()].id != 40,
              "index() is stale after a reorder (by design)", show(s));
        s.select(40);
        check(es[s.index()].id == 40, "select() re-resolves it", show(s));
    }

    // --- Nonsense in, nothing out -------------------------------------------
    {
        auto es = makeEntities();
        Selection s(es);
        s.select(999);
        check(!s.valid() && s.count() == 0,
              "selecting an unknown id selects nothing", show(s));
        s.toggle(-1);
        check(s.count() == 0, "toggling -1 does nothing", show(s));
        s.select(10);
        s.clear();
        check(!s.valid() && s.multi().empty() && s.ids().empty(),
              "clear() empties both", show(s));
    }

    std::printf(g_fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fails);
    return g_fails ? 1 : 0;
}
