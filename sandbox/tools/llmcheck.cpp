// The language-model check: llm.* and json.* as a script sees them.
//
// A ScriptSystem is built as Play builds it and a real script runs in it:
//   * json.encode / json.decode round-trip a nested table (UTF-8 included) and
//     decode refuses broken JSON with nil and a reason;
//   * a request to a port nobody listens on comes back -- not a hang, not a
//     crash -- with ok = false and an error that says so;
//   * llm.chat does not block: the frame it is asked in returns at once;
//   * a request to Ollama (127.0.0.1:11434, the default model) comes back as
//     decoded JSON when format = "json". Without a running Ollama this part is
//     SKIPPED, not failed -- the machine may simply not have one.
//
//   build/release/bin/llmcheck.exe
// Exits non-zero on any failure.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "Component.hpp"
#include "ScriptHost.hpp"
#include "ScriptSystem.hpp"

namespace fs = std::filesystem;

int main() {
    const fs::path dir = fs::temp_directory_path() / "fitzel-llmcheck";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path scriptPath = dir / "llm.lua";
    {
        std::ofstream s(scriptPath, std::ios::binary);
        s << R"LUA(
            local live, dead
            function start(e)
                local v = json.decode(json.encode({ a = 1, b = { 1, 2, 3 }, c = "Straße" }))
                game.log("json\t" .. tostring(v.a == 1 and v.b[3] == 3 and v.c == "Straße"))
                local bad, why = json.decode("{nope")
                game.log("bad\t" .. tostring(bad == nil and why ~= nil))
                llm.setHost("127.0.0.1", 1)
                dead = llm.chat{ prompt = "hallo" }
                llm.setHost("127.0.0.1", 11434)
                live = llm.chat{
                    prompt = 'Antworte nur mit JSON: {"wort": "<ein deutsches Wort>"}',
                    format = "json", options = { temperature = 0 } }
                game.log("pending\t" .. llm.pending())
            end
            function update(e, dt, t)
                for _, a in ipairs(llm.poll()) do
                    if a.id == dead then
                        game.log("dead\t" .. tostring(a.ok) .. "\t" .. tostring(a.error))
                    elseif a.id == live then
                        local w = a.ok and type(a.data) == "table" and a.data.wort or nil
                        game.log("live\t" .. tostring(a.ok) .. "\t" .. tostring(w) .. "\t" ..
                                 tostring(a.error) .. "\t" .. string.format("%.0f", a.ms))
                    end
                end
            end
        )LUA";
    }

    std::vector<std::string> lines;
    ScriptHost host;
    host.log = [&](const std::string& line) { lines.push_back(line); };
    ScriptSystem scripts;
    scripts.setHost(&host);
    ScriptComponent sc;
    sc.file = "llm.lua";
    Entity e;
    e.id = 1;

    int failures = 0;
    auto has = [&](const std::string& prefix) -> const std::string* {
        for (const std::string& l : lines)
            if (l.rfind(prefix, 0) == 0) return &l;
        return nullptr;
    };

    const auto t0 = std::chrono::steady_clock::now();
    scripts.update(e, sc, scriptPath.generic_string(), 1.0f / 60.0f, 0.0f);
    const double firstMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();
    if (!scripts.lastError().empty()) {
        std::printf("FAIL the script did not run: %s\n", scripts.lastError().c_str());
        return 1;
    }
    // Two requests queued in start: the frame must not have waited for either.
    if (firstMs > 500.0) {
        std::printf("FAIL the first frame took %.0f ms -- llm.chat blocked\n", firstMs);
        ++failures;
    }
    for (int i = 0; i < 1800 && !(has("dead\t") && has("live\t")); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        scripts.update(e, sc, scriptPath.generic_string(), 0.05f, 0.05f * i);
    }
    fs::remove_all(dir, ec);

    auto expect = [&](bool ok, const char* what) {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++failures;
    };
    const std::string* j = has("json\t");
    expect(j && *j == "json\ttrue", "json round trip");
    const std::string* b = has("bad\t");
    expect(b && *b == "bad\ttrue", "json.decode refuses broken JSON");
    const std::string* p = has("pending\t");
    expect(p && *p != "pending\t0", "llm.pending counts the queued requests");
    const std::string* d = has("dead\t");
    expect(d && d->rfind("dead\tfalse\t", 0) == 0 && d->find("nil") == std::string::npos,
           "a dead server answers ok=false with an error");
    if (d) std::printf("       %s\n", d->c_str());

    const std::string* l = has("live\t");
    if (!l) {
        expect(false, "the Ollama request came back at all");
    } else if (l->rfind("live\tfalse\t", 0) == 0 &&
               l->find("no model server") != std::string::npos) {
        std::printf("  SKIP no Ollama on 127.0.0.1:11434\n");
    } else {
        expect(l->rfind("live\ttrue\t", 0) == 0 && l->rfind("live\ttrue\tnil\t", 0) != 0,
               "Ollama answered with decoded JSON");
        std::printf("       %s\n", l->c_str());
    }
    std::printf("first frame %.1f ms\n", firstMs);
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("llm check passed\n");
    return 0;
}
