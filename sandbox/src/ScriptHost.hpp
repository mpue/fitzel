#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

// The host bridge exposed to Lua scripts as the global `game` table. The sandbox
// fills these callbacks and fields in before ticking scripts each frame; the
// ScriptSystem's C functions call through them. Entity creation/removal is
// deferred by the host (the tick loop is iterating the entity list), so
// game.spawn returns the new id immediately but the entity appears next frame.

// A request to create an entity at runtime (see game.spawn in Lua).
struct ScriptSpawn {
    int         type    = 3;          // EntityType (3 = Sphere)
    glm::vec3   pos{0.0f};
    glm::vec3   half{0.5f};           // half extents
    glm::vec3   rot{0.0f};            // Euler degrees
    glm::vec3   color{0.8f};
    glm::vec3   vel{0.0f};            // initial linear velocity (dynamic bodies)
    float       mass    = 1.0f;
    int         physics = 2;          // 0 none, 1 static, 2 dynamic
    std::string name;
    std::string script;               // Lua file under scripts/ ("" = none)
};

struct ScriptHost {
    // Input. `key` is a GLFW key code (see game.KEY_* constants); `button` is
    // 0=left, 1=right, 2=middle. *Pressed variants are true only on the frame
    // the key/button goes down.
    std::function<bool(int)> keyDown;
    std::function<bool(int)> keyPressed;
    std::function<bool(int)> mouseDown;
    std::function<bool(int)> mousePressed;

    // Player camera, refreshed each frame (Play mode).
    glm::vec3 camPos{0.0f};
    glm::vec3 camDir{0.0f, 0.0f, -1.0f};

    // Entities. spawn returns the new id immediately (creation deferred to the
    // end of the frame). getPos fills outPos and returns false for unknown ids.
    std::function<int(const ScriptSpawn&)>          spawn;
    std::function<void(int)>                        destroy;
    std::function<bool(int, glm::vec3&)>            getPos;
    std::function<void(int, glm::vec3)>            setPos;

    // Physics on a dynamic body (by entity id). No-ops on unknown ids.
    std::function<void(int, glm::vec3)> setVelocity;
    std::function<void(int, glm::vec3)> applyImpulse;

    // Play a one-shot sound file from the project's sounds/ folder.
    std::function<void(const std::string&)> playSound;

    // Host-side game state. Lua script environments are per-entity (isolated), so
    // shared state like the score lives here and is reached via game.addScore /
    // game.getScore / game.setHud.
    int         score = 0;
    std::string hud;
};
