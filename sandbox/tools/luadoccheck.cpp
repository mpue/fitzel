// The Lua reference check: does docs/lua-scripting.md name everything a script
// can call -- and nothing it cannot?
//
// The reference is the only manual there is (Help -> Lua API shows it in the
// editor), and it goes stale the quiet way: a new game.* function is bound,
// works, and simply never gets a line, so nobody finds it. Twice already a whole
// family went missing (the animation-graph calls, then all of music.*).
//
// So this asks the real VM, not the source: a ScriptSystem is built exactly as
// Play builds it, a script walks the `game`, `synth` and `music` tables, and
// every function must appear in the reference as `table.name(` (or, called with
// a table, `table.name{`), every constant by its name. The other way round,
// every call the reference shows must still exist -- a documented call that is
// gone is worse than a missing one.
//
//   build/release/bin/luadoccheck.exe [docs/lua-scripting.md]
// Run from the repo root (check-all.bat does). Exits non-zero on any gap.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Component.hpp"
#include "ScriptHost.hpp"
#include "ScriptSystem.hpp"

namespace fs = std::filesystem;

namespace {

// KEY_A..KEY_Z, KEY_0..KEY_9, KEY_F1..KEY_F12 are bound in loops and documented
// as ranges ("KEY_A ... KEY_Z"); a line per letter would help nobody.
bool isRangeKey(const std::string& n) {
    static const std::regex re("KEY_([A-Z0-9]|F[0-9]+)");
    return std::regex_match(n, re);
}

} // namespace

int main(int argc, char** argv) {
    const fs::path docPath = argc > 1 ? fs::path(argv[1]) : fs::path("docs/lua-scripting.md");
    std::ifstream in(docPath, std::ios::binary);
    if (!in) {
        std::printf("FAIL cannot read %s (run from the repo root)\n",
                    docPath.generic_string().c_str());
        return 1;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string doc = ss.str();

    // A script that reports every name in the three API tables through game.log.
    const fs::path dir = fs::temp_directory_path() / "fitzel-luadoccheck";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path scriptPath = dir / "list.lua";
    {
        std::ofstream s(scriptPath, std::ios::binary);
        s << R"(
            function start(e)
                for _, t in ipairs({ "game", "synth", "music" }) do
                    for k, v in pairs(_G[t]) do
                        game.log(t .. "\t" .. k .. "\t" .. type(v))
                    end
                end
            end
        )";
    }

    struct Name { std::string table, name; bool fn; };
    std::vector<Name> bound;
    ScriptHost host;
    host.log = [&](const std::string& line) {
        const size_t a = line.find('\t'), b = line.rfind('\t');
        if (a == std::string::npos || a == b) return;
        bound.push_back({line.substr(0, a), line.substr(a + 1, b - a - 1),
                         line.substr(b + 1) == "function"});
    };
    ScriptSystem scripts;
    scripts.setHost(&host);
    ScriptComponent sc;
    sc.file = "list.lua";
    Entity e;
    e.id = 1;
    scripts.update(e, sc, scriptPath.generic_string(), 1.0f / 60.0f, 0.0f);
    fs::remove_all(dir, ec);

    if (!scripts.lastError().empty() || bound.empty()) {
        std::printf("FAIL the listing script did not run: %s\n", scripts.lastError().c_str());
        return 1;
    }

    int failures = 0, fns = 0, consts = 0;
    std::set<std::string> boundFns;
    for (const Name& n : bound) {
        if (n.fn) {
            ++fns;
            boundFns.insert(n.table + "." + n.name);
            // `game.spawn(` or, with a table argument, `game.spawn{`.
            const std::string qn = n.table + "." + n.name;
            if (doc.find(qn + "(") == std::string::npos &&
                doc.find(qn + "{") == std::string::npos) {
                std::printf("  FAIL %s.%s is bound but not in the reference\n",
                            n.table.c_str(), n.name.c_str());
                ++failures;
            }
        } else {
            ++consts;
            if (isRangeKey(n.name)) continue;
            if (doc.find(n.name) == std::string::npos) {
                std::printf("  FAIL constant %s.%s is bound but not in the reference\n",
                            n.table.c_str(), n.name.c_str());
                ++failures;
            }
        }
    }

    // The other way: a call the reference shows must still be there.
    static const std::regex call(R"(\b(game|synth|music)\.([A-Za-z_][A-Za-z0-9_]*)[({])");
    std::set<std::string> documented;
    for (auto it = std::sregex_iterator(doc.begin(), doc.end(), call);
         it != std::sregex_iterator(); ++it)
        documented.insert((*it)[1].str() + "." + (*it)[2].str());
    for (const std::string& d : documented)
        if (!boundFns.count(d)) {
            std::printf("  FAIL %s is in the reference but no longer bound\n", d.c_str());
            ++failures;
        }

    std::printf("%d functions, %d constants bound; %zu calls in the reference\n",
                fns, consts, documented.size());
    if (failures) {
        std::printf("%d gap(s) -- update docs/lua-scripting.md\n", failures);
        return 1;
    }
    std::printf("reference complete\n");
    return 0;
}
