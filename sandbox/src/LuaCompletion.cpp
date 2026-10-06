#include "LuaCompletion.hpp"

#include <cctype>
#include <cstddef>

#include <TextEditor.h>

namespace luacomplete {

// Top-level identifiers: Lua keywords + the stdlib bits scripts use + the script
// lifecycle functions and the `e` entity fields. Offered when the word being
// typed is not a `game.` member.
const Completion kTopLevel[] = {
    {"function", "def"}, {"local", "scope"}, {"return", ""}, {"end", ""},
    {"then", ""}, {"else", ""}, {"elseif", ""}, {"for", ""}, {"while", ""},
    {"repeat", ""}, {"until", ""}, {"break", ""}, {"true", ""}, {"false", ""},
    {"nil", ""}, {"and", ""}, {"or", ""}, {"not", ""}, {"in", ""},
    {"start", "start(e)  -- called once on spawn"},
    {"update", "update(e, dt, t)  -- called each frame"},
    {"game", "engine API table"},
    {"synth", "Synth components: notes, dials, MIDI songs"},
    {"shared", "one table every script sees (scripts that work together)"},
    {"print", "print(...)"}, {"pairs", "pairs(t)"}, {"ipairs", "ipairs(t)"},
    {"tostring", "tostring(v)"}, {"tonumber", "tonumber(v)"}, {"type", "type(v)"},
    {"math", "math.*"}, {"string", "string.*"}, {"table", "table.*"},
};

// Members of the `game` table (functions + constants), offered after "game.".
// Signatures mirror ScriptSystem.cpp's C bindings.
const Completion kGameMembers[] = {
    {"keyDown", "keyDown(KEY) -> bool  (held)"},
    {"keyPressed", "keyPressed(KEY) -> bool  (this frame)"},
    {"mouseDown", "mouseDown(btn) -> bool"},
    {"mousePressed", "mousePressed(btn) -> bool"},
    {"mousePos", "mousePos() -> x, y, over  -- 1080-high HUD canvas"},
    {"mouseRay", "mouseRay() -> ox, oy, oz, dx, dy, dz  (world ray under the pointer)"},
    {"showCursor", "showCursor(on)  -- free the pointer in Play"},
    {"captureInput", "captureInput() -> held  -- every frame a menu is open: other scripts see no keys, Esc is yours"},
    {"cameraPos", "cameraPos() -> x, y, z"},
    {"cameraDir", "cameraDir() -> x, y, z"},
    {"spawn", "spawn{type=,x=,y=,z=,...} -> id"},
    {"destroy", "destroy(id)"},
    {"clone", "clone(id [, name]) -> id  (object + children, deferred)"},
    {"getPos", "getPos(id) -> x, y, z"},
    {"setPos", "setPos(id, x, y, z)"},
    {"setVelocity", "setVelocity(id, x, y, z)"},
    {"applyImpulse", "applyImpulse(id, x, y, z [, px, py, pz])  -- [where it struck: a hanging thing swings]"},
    {"moveCharacter", "moveCharacter(id, vx, vz [, dt]) -> x, y, z, onGround, onTerrain, turn  (a capsule through the world; turn = radians a tram it rides in turned it)"},
    {"removeCharacter", "removeCharacter(id)  -- drop the figure's capsule"},
    {"spawnVehicle", "spawnVehicle(id) -> bool  (the object's Vehicle parked in the physics world)"},
    {"driveVehicle", "driveVehicle(id) -> bool  (the player's controls go to that car)"},
    {"leaveVehicle", "leaveVehicle()  -- controls back, the car brakes where it is"},
    {"drivenVehicle", "drivenVehicle() -> id, speed, steer, throttle | nil"},
    {"groundHeight", "groundHeight(x, y, z [, maxDist]) -> y  (what lies below: road, deck, drawn terrain)"},
    {"castRay", "castRay(ox, oy, oz, dx, dy, dz [, maxDist]) -> x, y, z, nx, ny, nz, id, dist  (bodies + drawn terrain; id -1 = the world)"},
    {"orbitFrame", "orbitFrame(weight, dist, side, up, fov)  -- this frame: aim over the shoulder"},
    {"emit", "emit(id)  -- replay the object's Particle burst where it is"},
    {"toWorld", "toWorld(id, x, y, z) -> wx, wy, wz  (a point in the object's own frame)"},
    {"reach", "reach(id, \"left\"|\"right\"|\"leftFoot\"|\"rightFoot\", x, y, z [, weight])  -- this frame: a hand or foot to a point (IK)"},
    {"decal", "decal(x, y, z, nx, ny, nz [, size, material, spin]) -> bool  -- a bullet hole / image where it hit"},
    {"shatter", "shatter(id, x, y, z, dx, dy, dz [, strength]) -> bool  -- the glass there breaks into shards"},
    {"restart", "restart()  -- Play again from how the scene stood when Play began"},
    {"collectibles", "collectibles() -> { id, ... }  (active objects with a Collectible)"},
    {"collectible", "collectible(id) -> {item, icon, category, description, count, inventory, radius, ...}"},
    {"bonePos", "bonePos(id, bone) -> x, y, z, rx, ry, rz  (world, as last drawn)"},
    {"bones", "bones(id) -> { names }"},
    {"attach", "attach(obj, figure, bone [, x, y, z [, rx, ry, rz]]) -> ok  (follows the bone; no numbers = stays put)"},
    {"detach", "detach(obj)"},
    {"playSound", "playSound(name)"},
    {"addScore", "addScore(n)"}, {"getScore", "getScore() -> n"},
    {"setHud", "setHud(text)"},
    {"hudRect", "hudRect(x, y, w, h, r, g, b, a, rounding)  -- 1080-high canvas"},
    {"hudGradient", "hudGradient(x, y, w, h, r, g, b, a, r2, g2, b2, a2)"},
    {"hudFrame", "hudFrame(x, y, w, h, r, g, b, a, thickness, rounding)"},
    {"hudLine", "hudLine(x1, y1, x2, y2, r, g, b, a, thickness)"},
    {"hudCircle", "hudCircle(x, y, radius, r, g, b, a, thickness)  -- no thickness = filled"},
    {"hudTri", "hudTri(x1, y1, x2, y2, x3, y3, r, g, b, a)"},
    {"hudText", "hudText(x, y, text, size, r, g, b, a, align, bold)"},
    {"hudTextSize", "hudTextSize(text, size, bold) -> w, h"},
    {"hudSize", "hudSize() -> w, h  (h is always 1080)"},
    {"hudImage", "hudImage(tex, x, y, w, h [, r, g, b, a [, u0, v0, u1, v1]])  -- a Texture asset"},
    {"imageSize", "imageSize(tex) -> w, h, u0, v0, u1, v1  (the visible part)"},
    {"worldToHud", "worldToHud(x, y, z) -> hx, hy  (nil behind the eye)"},
    {"setCrosshair", "setCrosshair(on)"},
    {"rest", "rest([fps])  -- nothing moves this frame: wait for input, at most fps (10) frames/s"},
    {"setCameraFov", "setCameraFov(degrees)"},
    {"timeOfDay", "timeOfDay() -> hours 0..24  (the scene's clock)"},
    {"setTimeOfDay", "setTimeOfDay(hours)  (sun, light and street lamps follow)"},
    {"dayLength", "dayLength() -> real seconds per day, 0 = the clock stands"},
    {"setDayLength", "setDayLength(seconds)  (0 stops the clock, nil = the scene's own)"},
    {"streetLamps", "streetLamps() -> lit, mode (\"auto\" | \"on\" | \"off\")"},
    {"setStreetLamps", "setStreetLamps(\"on\" | \"off\" | \"auto\")  (the towns' street lamps)"},
    {"setFocus", "setFocus(near, far)  -- depth of field in metres; setFocus() = the view's own"},
    {"BOX", "type 0"}, {"RAMP", "type 1"}, {"CYLINDER", "type 2"}, {"SPHERE", "type 3"},
    {"EMPTY", "type 7"}, {"PLANE", "type 8"},
    {"MOUSE_LEFT", "0"}, {"MOUSE_RIGHT", "1"}, {"MOUSE_MIDDLE", "2"},
    {"KEY_SPACE", "32"}, {"KEY_ENTER", "257"}, {"KEY_ESCAPE", "256"},
    {"KEY_LSHIFT", "340"}, {"KEY_LCTRL", "341"},
    {"KEY_LEFT", "263"}, {"KEY_RIGHT", "262"}, {"KEY_UP", "265"}, {"KEY_DOWN", "264"},
    {"KEY_W", "87"}, {"KEY_A", "65"}, {"KEY_S", "83"}, {"KEY_D", "68"},
};

// Members of the `synth` table, offered after "synth.". `id` is an object with a
// Synth component; a note is a number (60) or a name ("C4", "F#3").
const Completion kSynthMembers[] = {
    {"play", "play(id) -> ok  -- start it, and its song if it has one"},
    {"stop", "stop(id)"},
    {"noteOn", "noteOn(id, note, velocity) -> ok  -- note 60 or \"C4\", velocity 0..1"},
    {"noteOff", "noteOff(id, note)  -- noteOff(id) lets every note go"},
    {"set", "set(id, dial, value) -> ok  -- a dial of the patch"},
    {"playMidi", "playMidi(id, file, loop) -> ok  -- file under content/midi/"},
    {"stopMidi", "stopMidi(id)"},
    {"isPlaying", "isPlaying(id) -> bool  -- is its song playing"},
    {"setTempo", "setTempo(id, scale)  -- 1 = as written"},
    {"note", "note(\"A4\") -> 69"},
    {"lastError", "lastError() -> text  -- why the last call said no"},
};

// New-script templates, offered in the "New Script" dialog. An "empty component"
// is just the two lifecycle stubs; the documented one lists the entity fields
// and the game API as a starting reference.
const char* kTemplateEmpty =
    "-- %s : entity component (runs in Play)\n\n"
    "function start(e)\n"
    "end\n\n"
    "function update(e, dt, t)\n"
    "end\n";

const char* kTemplateDocumented =
    "-- %s : entity component (runs in Play)\n"
    "-- e fields: x/y/z pos, rx/ry/rz rot(deg), sx/sy/sz half-size, name, id\n"
    "--           (mutate them to move/rotate/scale this entity)\n"
    "-- API: game.keyDown/keyPressed(KEY_*), game.mouseDown/mousePressed(MOUSE_*),\n"
    "--      game.spawn{...}, game.destroy(id), game.setPos/getPos(id,...),\n"
    "--      game.setVelocity/applyImpulse(id,...), game.playSound(name),\n"
    "--      game.addScore(n)/getScore(), game.setHud(text), game.cameraPos/Dir()\n\n"
    "function start(e)\n"
    "    -- called once when the entity enters Play\n"
    "end\n\n"
    "function update(e, dt, t)\n"
    "    -- dt = seconds since last frame, t = seconds since Play started\n"
    "end\n";

void refreshCompletion(TextEditor& ed, Completions& c) {
    c.items.clear();
    const auto        cur  = ed.GetCursorPosition();
    const std::string line = ed.GetCurrentLineText();
    const int         tab  = ed.GetTabSize();
    // Map the tab-expanded cursor column back to a byte index in the line.
    int idx = 0, col = 0;
    while (idx < static_cast<int>(line.size()) && col < cur.mColumn) {
        col += (line[idx] == '\t') ? (tab - (col % tab)) : 1;
        ++idx;
    }
    auto isIdent = [](char ch){
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; };
    int start = idx;
    while (start > 0 && isIdent(line[start - 1])) --start;
    c.prefix = line.substr(start, idx - start);
    // "game." member context: a '.' right before the word, and the token before
    // the dot is exactly "game".
    c.gameMember  = false;
    c.synthMember = false;
    if (start > 0 && line[start - 1] == '.') {
        int ws = start - 1;
        while (ws > 0 && isIdent(line[ws - 1])) --ws;
        const std::string owner = line.substr(ws, (start - 1) - ws);
        c.gameMember  = owner == "game";
        c.synthMember = owner == "synth";
    }
    if (c.prefix.empty() && !c.gameMember && !c.synthMember) {
        c.open = false; c.manualClose = false; return;
    }
    // Esc keeps the popup closed until the prefix actually changes.
    if (c.manualClose) {
        if (c.prefix == c.closedPrefix) { c.open = false; return; }
        c.manualClose = false;
    }
    auto lower = [](std::string s){
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s; };
    const std::string pfx = lower(c.prefix);
    auto consider = [&](const Completion* arr, std::size_t count){
        for (std::size_t i = 0; i < count; ++i)
            if (lower(arr[i].text).rfind(pfx, 0) == 0) c.items.push_back(arr[i]);
    };
    if (c.gameMember)
        consider(kGameMembers, sizeof(kGameMembers) / sizeof(kGameMembers[0]));
    else if (c.synthMember)
        consider(kSynthMembers, sizeof(kSynthMembers) / sizeof(kSynthMembers[0]));
    else
        consider(kTopLevel, sizeof(kTopLevel) / sizeof(kTopLevel[0]));
    // Nothing useful to offer (no match, or the sole match is already typed).
    if (c.items.empty() || (c.items.size() == 1 && lower(c.items[0].text) == pfx)) {
        c.open = false; return;
    }
    if (c.sel >= static_cast<int>(c.items.size())) c.sel = 0;
    c.open = true;
}

} // namespace luacomplete
