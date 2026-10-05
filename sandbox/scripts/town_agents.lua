-- Leute, die die Stadt bevölkern: jede Figur ist ein kleiner Agent, den ein
-- Sprachmodell auf diesem Rechner (Ollama, llm.*) steuert. Sie haben Namen,
-- Berufe und Wohnungen, entscheiden selbst, wohin sie als Nächstes gehen und was
-- sie dort tun, laufen über Gehwege und Überwege dorthin -- und wer einem
-- Bekannten (oder einem Fremden) über den Weg läuft, bleibt stehen und redet.
--
-- EINRICHTEN. Ein leeres Objekt (Empty) in die Szene, Add Component -> Script,
-- diese Datei. Ollama muss laufen und das Modell geladen haben:
--     ollama pull qwen3:8b
-- Dann Play. Die Leute erscheinen an Häusern in der Nähe der Kamera.
--
-- Taste H blendet die Liste aller Leute ein und aus (wer, wo, was).
--
-- CHAT. T öffnet ein Chatfenster (die Figur bleibt solange stehen). Geschrieben
-- wird an "Stadt" -- einen Erzähler, der weiß, wer gerade wo ist und was tut --
-- oder an eine Person: Tab wechselt, "@Erika ..." spricht Erika direkt an. Wer in
-- der Nähe steht, bleibt stehen, dreht sich zu dir und antwortet laut (auch in der
-- Sprechblase); wer weiter weg ist, antwortet wie per Handy. Bittest du jemanden,
-- irgendwohin zu gehen, tut er es vielleicht. Enter schickt, Esc schließt.
--
-- VERHALTEN FÜR ALLE. Der Parameter CONTEXT (im Inspector) gilt ab dem Start für
-- alle Leute. Im Chat setzt "/alle <Text>" ihn neu -- alle überlegen sofort, was
-- sie jetzt tun --, "/alle" zeigt ihn, "/alle aus" löscht ihn.
--
-- WO SIND SIE? Über jeder Person ein farbiger Pfeil mit Nummer, Name und
-- Entfernung; wer außerhalb des Bildes ist, hat einen Pfeil am Bildrand, der in
-- seine Richtung zeigt. M schaltet die Markierungen ab und wieder an.
--
-- FREIE KAMERA. K: fliegen mit WASD, Leertaste/Strg hoch/runter, Shift schneller,
-- Mausrad Grundtempo; umsehen mit den Pfeiltasten oder bei gehaltener rechter
-- Maustaste. 1..8 fliegt zu dieser Person und folgt ihr, 0 zu deiner Figur.
-- K oder Esc: zurück zu deiner Figur.
--
-- Ohne Ollama laufen die Leute trotzdem -- zu zufälligen Zielen, stumm -- und
-- oben links steht, warum.

AGENTS        = 6            -- wie viele Leute
MODEL         = "qwen3:8b"   -- Ollama-Modell (qwen3:4b geht auch, ist aber deutlich dümmer)
PREFABS_MEN   = "npc_mann,npc_alex"   -- Figuren für die Männer
PREFABS_WOMEN = "npc_frau"            -- ... und für die Frauen
SPAWN_RADIUS  = 250          -- Meter um die Kamera, in denen sie wohnen
WALK_SPEED    = 1.3          -- m/s
CLOCK_SPEED   = 1.0          -- Spielminuten pro Sekunde (1 = ein Tag in 24 min)
START_HOUR    = 8.0          -- Uhrzeit beim Start
TALK_RADIUS   = 3.5          -- Meter: so nah, und sie reden vielleicht
TALK_CHANCE   = 0.6          -- Anteil der Begegnungen, die zu einem Gespräch werden
BUBBLE_RANGE  = 45           -- Meter: weiter weg keine Sprechblasen
SHOW_LIST     = true         -- Liste oben links beim Start
LOG           = true         -- Entscheidungen und Gespräche auf die Konsole
CHAT_KEY      = "T"          -- öffnet das Chatfenster
MARKER_KEY    = "M"          -- Markierungen über den Leuten ein/aus
FREECAM_KEY   = "K"          -- freie Kamera ein/aus
FLY_SPEED     = 12           -- m/s der freien Kamera (Mausrad ändert, Shift x4)
PLAYER_NAME   = "Alex"       -- so heißt du für die Leute
NEAR_PLAYER   = 8            -- Meter: so nah, und man redet von Angesicht zu Angesicht
-- Was für ALLE gilt: eine Lage, ein Ereignis, eine Haltung -- steht in jedem Prompt.
-- Z. B. "Heute ist Stadtfest am Rathaus" oder "Alle sind Fremden gegenüber misstrauisch".
-- Im Spiel ändern: im Chat "/alle <Text>" (alle denken sofort neu nach), "/alle aus".
CONTEXT       = ""

-- --- Wer in der Stadt lebt ---------------------------------------------------
-- `work` ist eine Ortsart aus game.townPlaces ("" = ohne festen Arbeitsplatz),
-- `hours` die übliche Arbeitszeit (von, bis; volle Stunden).
local PERSONAS = {
    { name = "Erika", sex = "w",   age = 67, job = "Rentnerin",                      work = "",
      traits = "neugierig, redselig, kennt jeden Klatsch, liest gern Krimis aus der Bibliothek" },
    { name = "Jonas", sex = "m",   age = 34, job = "Lehrer an der Schule",            work = "school",
      hours = { 8, 14 }, traits = "gestresst, hilfsbereit, braucht dauernd Kaffee" },
    { name = "Mehmet", sex = "m",  age = 45, job = "Polizist",                        work = "police",
      hours = { 7, 15 }, traits = "ruhig, pflichtbewusst, joggt gern im Park" },
    { name = "Lena", sex = "w",    age = 28, job = "Programmiererin in einem Büro",   work = "office",
      hours = { 9, 17 }, traits = "introvertiert, ironisch, geht mittags gern spazieren" },
    { name = "Karl", sex = "m",    age = 52, job = "Mechaniker im Gewerbegebiet",     work = "works",
      hours = { 7, 16 }, traits = "brummig, aber herzlich, Fußballfan" },
    { name = "Sophie", sex = "w",  age = 19, job = "Studentin",                       work = "",
      traits = "fröhlich, chaotisch, immer zu spät, fährt viel Bus, lernt gern in der Bibliothek" },
    { name = "Anna", sex = "w",    age = 41, job = "Krankenpflegerin",                work = "hospital",
      hours = { 13, 21 }, traits = "müde von den Schichten, warmherzig, direkt" },
    { name = "Bernd", sex = "m",   age = 58, job = "Sachbearbeiter im Rathaus",       work = "townhall",
      hours = { 8, 16 }, traits = "pedantisch, liebt Vorschriften, heimlich ein Romantiker" },
}

local KIND_DE = {
    home = "Wohnhaus", flat = "Wohnblock", office = "Bürohaus", works = "Gewerbe",
    park = "Park", stop = "Bushaltestelle", townhall = "Rathaus", school = "Schule",
    kindergarten = "Kindergarten", church = "Kirche", police = "Polizei",
    firestation = "Feuerwache", hospital = "Krankenhaus", library = "Bibliothek",
    museum = "Museum", theatre = "Theater", pool = "Schwimmbad", petrol = "Tankstelle",
    station = "Bahnhof", industry = "Gewerbegebiet", powerplant = "Kraftwerk",
    landfill = "Wertstoffhof",
}
-- Orte, die man besucht (nicht: fremde Wohnhäuser).
local PUBLIC = {
    park = true, stop = true, townhall = true, school = true, kindergarten = true,
    church = true, police = true, hospital = true, library = true, museum = true,
    theatre = true, pool = true, petrol = true, station = true, industry = true,
}
-- Orte, an denen man drinnen ist: wer dort länger bleibt, geht hinein (die
-- Figur verschwindet) und kommt danach wieder heraus.
local INDOOR = {
    home = true, flat = true, office = true, works = true, townhall = true, school = true,
    kindergarten = true, church = true, police = true, hospital = true, library = true,
    museum = true, theatre = true, pool = true, station = true, industry = true,
}
local INSIDE_AFTER = 12  -- Spielminuten: wer kürzer bleibt, wartet vor der Tür
-- Wie schnell die Gehanimation eines Prefabs bei walkSpeed = 1 läuft (m/s).
local CLIP_SPEED = { npc_alex = 1.12, npc_mann = 1.4, npc_frau = 1.4 }
-- Eine Farbe pro Person: Markierung, Liste, Pfeil am Bildrand.
local AGENT_COLORS = {
    { 1.00, 0.42, 0.36 }, { 0.36, 0.72, 1.00 }, { 0.45, 0.88, 0.45 }, { 1.00, 0.80, 0.25 },
    { 0.82, 0.50, 1.00 }, { 0.30, 0.90, 0.85 }, { 1.00, 0.55, 0.80 }, { 0.85, 0.85, 0.85 },
}
local function colorOf(a) return AGENT_COLORS[(a.i - 1) % #AGENT_COLORS + 1] end

local _agents = {}       -- die Leute (siehe spawnAgents)
local _places = {}       -- alle Orte der Stadt
local _byName = {}       -- name -> Ort
local _requests = {}     -- llm-id -> { kind = "plan"|"talk", ... }
local _clock              -- Spielzeit in Minuten seit Mitternacht
local _llmError           -- letzter Fehler vom Modell (oben links)
local _llmOk = false      -- schon einmal eine Antwort bekommen
local _talks = {}         -- laufende Gespräche
local _showList
local _pairSeen = {}      -- "i:j" -> Zeit der letzten Begegnung
local _now = 0            -- Sekunden seit Play
local goInside, comeOut   -- (unten, bei Bewegen)
local _context = ""       -- was für alle gilt (CONTEXT, im Spiel per "/alle")

-- Der Satz, mit dem die Lage für alle in einen Prompt kommt ("" = keine).
local function contextNote()
    if _context == "" then return "" end
    return "\nWas gerade für alle in der Stadt gilt (richte dich unbedingt danach): " ..
           _context .. "\n"
end

-- --- Kleinkram ----------------------------------------------------------------

local function log(...)
    if LOG then game.log("[agenten]", ...) end
end

local function clamp(x, a, b) return math.max(a, math.min(b, x)) end
local function wrap(a) return (a + 180) % 360 - 180 end
local function dist2(ax, az, bx, bz) local dx, dz = bx - ax, bz - az; return dx * dx + dz * dz end

local function clockText()
    local m = math.floor(_clock) % (24 * 60)
    return string.format("%02d:%02d", m // 60, m % 60)
end

local function split(s)
    local out = {}
    for w in string.gmatch(s, "[^,%s]+") do out[#out + 1] = w end
    return out
end

local function shuffle(t)
    for i = #t, 2, -1 do
        local j = math.random(i)
        t[i], t[j] = t[j], t[i]
    end
    return t
end

local function remember(a, text)
    table.insert(a.memory, 1, clockText() .. " " .. text)
    while #a.memory > 6 do table.remove(a.memory) end
end

-- Text in Zeilen von höchstens `n` Zeichen (UTF-8-sicher genug: an Leerzeichen).
local function wrapText(s, n)
    local lines, line = {}, ""
    for w in string.gmatch(s, "%S+") do
        if line == "" then line = w
        elseif utf8.len(line .. " " .. w) and utf8.len(line .. " " .. w) <= n then line = line .. " " .. w
        else lines[#lines + 1] = line; line = w end
    end
    if line ~= "" then lines[#lines + 1] = line end
    return lines
end

-- --- Die Stadt -----------------------------------------------------------------

local function loadPlaces()
    _places = game.townPlaces()
    _byName = {}
    for _, p in ipairs(_places) do _byName[p.name] = p end
    log(#_places .. " Orte in der Stadt")
end

local function nearestOfKind(kind, x, z, except)
    local best, bd
    for _, p in ipairs(_places) do
        if p.kind == kind and p ~= except then
            local d = dist2(x, z, p.x, p.z)
            if not bd or d < bd then best, bd = p, d end
        end
    end
    return best
end

local function placeLabel(a, p)
    local what = KIND_DE[p.kind] or p.kind
    if p == a.home then return p.name .. " (dein Zuhause)" end
    if p == a.work then return p.name .. " (deine Arbeit)" end
    for _, o in ipairs(_agents) do
        if o ~= a and o.home == p then return p.name .. " (Zuhause von " .. o.p.name .. ")" end
    end
    if p.name:find(what, 1, true) then return p.name end
    return p.name .. " (" .. what .. ")"
end

-- Die Ziele, zwischen denen eine Figur wählt: Zuhause, Arbeit, die Wohnungen der
-- anderen, und die öffentlichen Orte in der Nähe -- höchstens 12.
local function hour() return (_clock / 60) % 24 end
local function night() local h = hour(); return h >= 22 or h < 6 end
local function workTime(a)
    local h = a.p.hours
    return a.work and h and hour() >= h[1] and hour() < h[2]
end

local function candidates(a)
    local list, seen = {}, {}
    -- Zweimal hintereinander am selben Ort geblieben: jetzt woanders hin (ein
    -- kleines Modell dreht sich sonst im Kreis). Nachts zu Hause darf man bleiben.
    if a.here and (a.stayCount or 0) >= 2 and not (night() and a.here == a.home) and
        not (workTime(a) and a.here == a.work) then
        seen[a.here] = true
    end
    local function add(p) if p and not seen[p] then seen[p] = true; list[#list + 1] = p end end
    add(a.home)
    add(a.work)
    -- Was die Lage für alle nennt ("Stadtfest im Stadtpark", "alle zum Rathaus"),
    -- muss man auch wählen können -- egal wie weit weg. Bis zu drei Orte.
    if _context ~= "" then
        local ctx, named = _context:lower(), 0
        for _, p in ipairs(_places) do
            if named >= 3 then break end
            if PUBLIC[p.kind] then
                local kind = (KIND_DE[p.kind] or ""):lower()
                if ctx:find(p.name:lower(), 1, true) or (kind ~= "" and ctx:find(kind, 1, true)) then
                    if not seen[p] then named = named + 1 end
                    add(p)
                end
            end
        end
    end
    local near = {}
    for _, p in ipairs(_places) do
        if PUBLIC[p.kind] then near[#near + 1] = { p = p, d = dist2(a.x, a.z, p.x, p.z) } end
    end
    table.sort(near, function(u, v) return u.d < v.d end)
    for i = 1, math.min(7, #near) do add(near[i].p) end
    local others = {}
    for _, o in ipairs(_agents) do if o ~= a then others[#others + 1] = o end end
    shuffle(others)
    for i = 1, math.min(2, #others) do add(others[i].home) end
    -- Noch etwas weiter weg, damit nicht immer dieselben drei Orte drankommen.
    for i = 8, math.min(12, #near) do
        if #list >= 12 then break end
        if math.random() < 0.5 then add(near[i].p) end
    end
    return list
end

-- --- Die Leute -------------------------------------------------------------------

-- Prefabs, die es in diesem Projekt nicht gibt (oben links gemeldet).
local _missing = {}

-- Eine Figur für Person Nummer `idx`: eins der eigenen Prefabs (`own`, reihum),
-- sonst irgendeins aus `prefabs`, sonst eine farbige Säule als Ersatz -- damit die
-- Leute auch in einem Projekt ohne die npc_*-Prefabs auftauchen. Gibt id, das
-- Prefab ("" = Ersatz) und die Höhe zurück, auf der der Ursprung steht.
local function spawnFigure(prefabs, own, i, home, idx, name)
    local yaw = math.random(0, 359)
    -- Reihum durch die eigenen (zwei Männer nicht im selben Modell), dann der Rest.
    local order = {}
    for k = 0, #own - 1 do order[#order + 1] = own[(i - 1 + k) % #own + 1] end
    for k = #own + 1, #prefabs do order[#order + 1] = prefabs[k] end
    for _, pf in ipairs(order) do
        if not _missing[pf] then
            local id = game.spawnPrefab(pf, home.x, home.y, home.z, yaw)
            if id and id > 0 then return id, pf, home.y end
            _missing[pf] = true
            game.log("[agenten] Prefab fehlt im Projekt: " .. pf)
        end
    end
    local c = AGENT_COLORS[(idx - 1) % #AGENT_COLORS + 1]
    local id = game.spawn{ type = game.CYLINDER, x = home.x, y = home.y + 0.9, z = home.z,
                           sx = 0.28, sy = 0.9, sz = 0.28, r = c[1], g = c[2], b = c[3],
                           physics = game.PHYSICS_NONE, name = "Agent " .. name }
    return id, "", home.y + 0.9
end

local function spawnAgents()
    local cx, _, cz = game.cameraPos()
    local homes = {}
    for _, p in ipairs(_places) do
        if p.kind == "home" or p.kind == "flat" then
            homes[#homes + 1] = { p = p, d = dist2(cx, cz, p.x, p.z) }
        end
    end
    if #homes == 0 then return end
    table.sort(homes, function(u, v) return u.d < v.d end)
    -- Im Umkreis, sonst die nächsten (die Kamera steht vielleicht vor der Stadt).
    local pool = {}
    for _, h in ipairs(homes) do
        if h.d <= SPAWN_RADIUS * SPAWN_RADIUS or #pool < AGENTS * 3 then pool[#pool + 1] = h.p end
        if #pool >= math.max(AGENTS * 4, 40) then break end
    end
    shuffle(pool)
    local men, women = split(PREFABS_MEN), split(PREFABS_WOMEN)
    local personas = shuffle({ table.unpack(PERSONAS) })
    for i = 1, math.min(AGENTS, #personas, #pool) do
        local home = pool[i]
        local persona = personas[i]
        -- Erst eine Figur des eigenen Geschlechts, dann irgendeine.
        local own, other = men, women
        if persona.sex == "w" then own, other = women, men end
        local list = { table.unpack(own) }
        for _, pf in ipairs(other) do list[#list + 1] = pf end
        local id, prefab, spawnY = spawnFigure(list, own, i, home, #_agents + 1, persona.name)
        if id and id > 0 then
            local a = {
                -- i: Platz in _agents (Farbe, Taste für die freie Kamera)
                i = #_agents + 1, id = id, p = persona, prefab = prefab, home = home,
                x = home.x, y = home.y, z = home.z, yaw = 0, spawnY = spawnY,
                state = "start", memory = {}, met = {}, here = home,
                nextTalk = 0, bubble = nil,
            }
            a.work = persona.work ~= "" and nearestOfKind(persona.work, home.x, home.z) or nil
            _agents[#_agents + 1] = a
            log(persona.name .. " wohnt in " .. home.name ..
                (a.work and (", arbeitet in " .. a.work.name) or "") ..
                " (" .. (prefab ~= "" and prefab or "Ersatzsäule") .. ")")
        end
    end
end

local function say(a, text, secs, italic)
    if not text then return end
    text = tostring(text):gsub("[\240-\247][\128-\191][\128-\191][\128-\191]", "")   -- keine Emojis
    if text == "" then return end
    a.bubble = { text = text, untilT = _now + (secs or math.max(3.5, utf8.len(text) * 0.07)),
                 italic = italic }
end

-- Einen Weg dorthin, und los.
local function goTo(a, place)
    local path = game.townPath(a.x, a.z, place.x, place.z)
    if not path or #path == 0 then
        log(a.p.name .. ": kein Weg nach " .. place.name)
        return false
    end
    a.path, a.step = path, 1
    a.target = place
    a.state = "walk"
    a.stuck, a.lastX, a.lastZ = 0, a.x, a.z
    return true
end

-- --- Was als Nächstes? Das Modell entscheidet ---------------------------------

local PLAN_SCHEMA_BASE = {
    type = "object",
    properties = {
        place    = { type = "string" },
        activity = { type = "string" },
        minutes  = { type = "integer" },
        say      = { type = "string" },
    },
    required = { "place", "activity", "minutes", "say" },
}

local function fallbackPlan(a)
    local c = candidates(a)
    local p = c[math.random(#c)]
    a.plan = { place = p, activity = "schaut sich um", minutes = math.random(5, 25) }
    if a.inside then comeOut(a) end
    if not goTo(a, p) then a.state = "do"; a.doUntil = _now + 10 end
end

local function askPlan(a)
    local list = candidates(a)
    local names, lines = {}, {}
    for _, p in ipairs(list) do
        names[#names + 1] = p.name
        lines[#lines + 1] = "- " .. placeLabel(a, p)
    end
    local schema = json.decode(json.encode(PLAN_SCHEMA_BASE))
    schema.properties.place.enum = names
    local here = a.here and placeLabel(a, a.here) or (game.streetAt(a.x, a.z) or "unterwegs")
    local mem = #a.memory > 0 and table.concat(a.memory, "\n") or "(noch nichts)"
    local day = ""
    if a.work and a.p.hours then
        day = string.format("Du arbeitest normalerweise von %d bis %d Uhr (%s). ",
                            a.p.hours[1], a.p.hours[2], a.work.name)
    end
    local nudge = ""
    if workTime(a) and a.here ~= a.work then
        nudge = "Eigentlich müsstest du jetzt bei der Arbeit sein. "
    elseif night() and a.here ~= a.home then
        nudge = "Es ist spät, langsam nach Hause und schlafen. "
    end
    if a.plan and a.plan.activity ~= "" then
        nudge = nudge .. "Eben hast du: " .. a.plan.activity .. " -- jetzt etwas anderes. "
    end
    local system = string.format(
        "Du bist %s, %d Jahre alt, %s in einer deutschen Kleinstadt. Du bist %s. " ..
        "Du wohnst in %s. %sDu lebst ganz normal deinen Alltag. " ..
        "Antworte nur mit dem verlangten JSON, auf Deutsch, knapp und natürlich.",
        a.p.name, a.p.age, a.p.job, a.p.traits, a.home.name, day)
    -- Uhrzeit, Hinweise und die Lage für alle vorn -- außerhalb des Formats, ein
    -- "%" darin (aus dem Modell oder aus CONTEXT) ist dort kein Platzhalter.
    local prompt = "Es ist " .. clockText() .. " Uhr. " .. nudge .. contextNote() .. string.format(
        "Du bist gerade: %s.\nWas zuletzt war:\n%s\n\n" ..
        "Mögliche Ziele:\n%s\n\n" ..
        "Wohin gehst du als Nächstes und was tust du dort? " ..
        "place = genau ein Name aus der Liste (ohne Klammerzusatz), " ..
        "activity = was du dort tust, passend zu genau diesem Ort und zu dir " ..
        "(3-8 Wörter, dritte Person, ohne deinen Namen), " ..
        "minutes = wie lange (5-240; bei der Arbeit eher lange), " ..
        "say = ein kurzer Satz, den du vor dich hin sagst oder denkst (höchstens 12 Wörter). " ..
        "Passe es an die Uhrzeit an (morgens zur Arbeit, mittags Pause, abends nach Hause " ..
        "oder ausgehen, nachts schlafen). Mach nicht dasselbe wie eben.",
        here, mem, table.concat(lines, "\n"))
    local id = llm.chat{
        model = MODEL, system = system, prompt = prompt, format = schema,
        options = { temperature = 0.9, num_predict = 160 },
    }
    _requests[id] = { kind = "plan", agent = a, sent = _now }
    a.state = "think"
    a.thinkSince = _now
end

local function onPlan(a, ans)
    local d = ans.ok and type(ans.data) == "table" and ans.data or nil
    local place = d and _byName[d.place]
    if not place then
        if ans.ok then log(a.p.name .. ": unbrauchbare Antwort: " .. tostring(ans.text)) end
        fallbackPlan(a)
        return
    end
    local minutes = clamp(math.floor(tonumber(d.minutes) or 15), 3, 240)
    a.plan = { place = place, activity = tostring(d.activity or ""), minutes = minutes }
    log(string.format("%s %s: -> %s, %s (%d min) \"%s\" [%.1f s]", clockText(), a.p.name,
                      place.name, a.plan.activity, minutes, tostring(d.say), ans.ms / 1000))
    say(a, d.say)
    a.stayCount = place == a.here and (a.stayCount or 0) + 1 or 0
    if place == a.here or dist2(a.x, a.z, place.x, place.z) < 4 then
        -- Bleibt, wo es ist: drinnen, wenn es länger dauert und ein Haus ist.
        a.state = "do"
        a.doUntil = _now + minutes / CLOCK_SPEED
        remember(a, a.plan.activity .. " (" .. place.name .. ")")
        local stay = INDOOR[place.kind] and minutes >= INSIDE_AFTER
        if stay and not a.inside then goInside(a) elseif not stay and a.inside then comeOut(a) end
        return
    end
    if a.inside then comeOut(a) end
    if not goTo(a, place) then
        a.state = "do"
        a.doUntil = _now + 8
    end
end

-- --- Gespräche --------------------------------------------------------------------

local function describe(a)
    if a.state == "walk" and a.plan then return "ist unterwegs zu " .. a.plan.place.name ..
        " (will dort: " .. a.plan.activity .. ")" end
    if a.plan then return a.plan.activity .. " (" .. a.plan.place.name .. ")" end
    return "steht herum"
end

local function startTalk(a, b)
    local talk = { a = a, b = b, lines = nil, line = 0, lineUntil = 0, started = _now }
    for _, x in ipairs({ a, b }) do
        x.resume = x.state
        x.state = "talk"
        x.talk = talk
        x.nextTalk = _now + 90
    end
    local function who(x, other)
        local know = x.met[other.p.name]
        return string.format("%s (%d, %s; %s). %s gerade: %s. Kennt %s %s.", x.p.name, x.p.age,
                             x.p.job, x.p.traits, x.p.name, describe(x), other.p.name,
                             know and ("schon (" .. know .. "x getroffen)") or "noch nicht")
    end
    local mem = {}
    for _, x in ipairs({ a, b }) do
        for i = 1, math.min(2, #x.memory) do mem[#mem + 1] = x.p.name .. ": " .. x.memory[i] end
    end
    local schema = {
        type = "object",
        properties = { lines = { type = "array", maxItems = 4, items = {
            type = "object",
            properties = { speaker = { type = "string", enum = { a.p.name, b.p.name } },
                           text = { type = "string" } },
            required = { "speaker", "text" } } } },
        required = { "lines" },
    }
    local prompt = string.format(
        "Zwei Leute begegnen sich um %s Uhr auf der Straße (%s) in einer deutschen Kleinstadt.\n" ..
        "%s\n%s\nWas sie zuletzt erlebt haben:\n%s\n\n" ..
        "Schreibe ihr kurzes Gespräch: 2 bis 4 Zeilen, abwechselnd, jede Zeile höchstens " ..
        "15 Wörter, natürlich und umgangssprachlich, passend zu Charakter und Situation. " ..
        "Wer sich nicht kennt, grüßt nur kurz oder stellt sich vor.",
        clockText(), game.streetAt(a.x, a.z) or "Gehweg", who(a, b), who(b, a),
        #mem > 0 and table.concat(mem, "\n") or "(nichts Besonderes)") .. contextNote()
    local id = llm.chat{
        model = MODEL, prompt = prompt, format = schema,
        system = "Du schreibst kurze, glaubwürdige Alltagsdialoge auf Deutsch. Nur JSON.",
        options = { temperature = 1.0, num_predict = 260 },
    }
    _requests[id] = { kind = "talk", talk = talk, sent = _now }
    _talks[#_talks + 1] = talk
    log(clockText() .. " " .. a.p.name .. " trifft " .. b.p.name)
end

local function endTalk(talk)
    for _, x in ipairs({ talk.a, talk.b }) do
        local other = x == talk.a and talk.b or talk.a
        x.met[other.p.name] = (x.met[other.p.name] or 0) + 1
        local last = talk.lines and talk.lines[#talk.lines]
        remember(x, "hat mit " .. other.p.name .. " geredet" ..
                    (last and (" (" .. last.speaker .. ": \"" .. last.text .. "\")") or ""))
        x.talk = nil
        x.state = (x.resume == "walk" or x.resume == "do") and x.resume or "plan"
    end
    for i, t in ipairs(_talks) do if t == talk then table.remove(_talks, i) break end end
end

local function onTalk(talk, ans)
    local d = ans.ok and type(ans.data) == "table" and ans.data or nil
    local lines = {}
    if d and type(d.lines) == "table" then
        for _, l in ipairs(d.lines) do
            if type(l) == "table" and type(l.text) == "string" and l.text ~= "" then
                lines[#lines + 1] = { speaker = tostring(l.speaker), text = l.text }
            end
        end
    end
    if #lines == 0 then
        lines = { { speaker = talk.a.p.name, text = "Hallo!" }, { speaker = talk.b.p.name, text = "Tag!" } }
    end
    talk.lines, talk.line, talk.lineUntil = lines, 0, _now
    for _, l in ipairs(lines) do log("   " .. l.speaker .. ": " .. l.text) end
end

local function tickTalk(talk)
    if not talk.lines then
        -- Kein Modell, keine Antwort nach 25 s: auseinandergehen.
        if _now - talk.started > 25 then endTalk(talk) end
        return
    end
    if _now < talk.lineUntil then return end
    talk.line = talk.line + 1
    local l = talk.lines[talk.line]
    if not l then endTalk(talk) return end
    local who = l.speaker == talk.b.p.name and talk.b or talk.a
    local secs = math.max(2.6, utf8.len(l.text) * 0.075)
    say(who, l.text, secs)
    talk.lineUntil = _now + secs + 0.3
end

-- Wer einander nahe kommt, redet vielleicht.
local function lookForMeetings()
    for i = 1, #_agents do
        local a = _agents[i]
        for j = i + 1, #_agents do
            local b = _agents[j]
            local key = i .. ":" .. j
            local near = dist2(a.x, a.z, b.x, b.z) < TALK_RADIUS * TALK_RADIUS
            if near and not _pairSeen[key] then
                _pairSeen[key] = _now
                local free = function(x)
                    return (x.state == "walk" or x.state == "do") and not x.inside and
                           _now > x.nextTalk and not (x.listenUntil and _now < x.listenUntil)
                end
                if free(a) and free(b) and math.random() < TALK_CHANCE then startTalk(a, b) end
            elseif not near and _pairSeen[key] and dist2(a.x, a.z, b.x, b.z) > 64 then
                _pairSeen[key] = nil   -- wieder auseinander: die nächste Begegnung zählt neu
            end
        end
    end
end

-- --- Bewegen -----------------------------------------------------------------------

local function face(a, tx, tz, dt, speed)
    local dx, dz = tx - a.x, tz - a.z
    if dx * dx + dz * dz < 1e-4 then return 0 end
    local want = math.deg(math.atan(dx, dz))
    local diff = wrap(want - a.yaw)
    a.yaw = wrap(a.yaw + clamp(diff, -(speed or 360) * dt, (speed or 360) * dt))
    return diff
end

-- Die Figur um (vx, vz) m/s bewegen -- durch die Physik, wie walker.lua.
local function move(a, vx, vz, dt)
    local fx, fy, fz, _, terrain = game.moveCharacter(a.id, vx, vz, dt)
    if not fx then return end
    if terrain then fy = game.terrainHeight(fx, fz) end
    if not a.footOff then a.footOff = a.spawnY - fy end
    a.x, a.y, a.z = fx, fy, fz
end

local function place(a)
    game.setPos(a.id, a.x, a.y + (a.footOff or 0), a.z)
    game.setRot(a.id, 0, a.yaw, 0)
end

local function animate(a, walking, speed)
    game.animBool(a.id, "walking", walking)
    local clip = CLIP_SPEED[a.prefab] or 1.4
    game.animNumber(a.id, "walkSpeed", walking and (speed / clip) or 1)
end

-- Hinein: unsichtbar, ohne Kapsel (sonst stünde sie unsichtbar im Weg).
goInside = function(a)
    a.inside = true
    a.bubble = nil
    game.removeCharacter(a.id)
    game.setActive(a.id, false)
end

comeOut = function(a)
    a.inside = false
    game.setActive(a.id, true)
    if a.here then a.yaw = math.deg(math.atan(a.x - a.here.ax, a.z - a.here.az)) end
end

local function tickWalk(a, dt)
    local wp = a.path[a.step]
    if not wp then
        -- Angekommen.
        a.here = a.target
        a.path = nil
        a.state = "do"
        local minutes = a.plan and a.plan.minutes or 10
        a.doUntil = _now + minutes / CLOCK_SPEED
        if a.plan then remember(a, a.plan.activity .. " (" .. a.target.name .. ")") end
        if INDOOR[a.target.kind] and minutes >= INSIDE_AFTER then
            goInside(a)
        else
            if a.plan and a.plan.activity ~= "" then say(a, "(" .. a.plan.activity .. ")", 5, true) end
            if math.random() < 0.5 then game.animTrigger(a.id, "bored") end   -- wer es kann, schaut sich um
        end
        return false
    end
    local dx, dz = wp.x - a.x, wp.z - a.z
    local d = math.sqrt(dx * dx + dz * dz)
    local reach = a.step == #a.path and 0.4 or 0.9
    if d < reach then a.step = a.step + 1 return tickWalk(a, dt) end
    local diff = face(a, wp.x, wp.z, dt, 300)
    local speed = WALK_SPEED * clamp(math.cos(math.rad(diff)), 0.15, 1)
    if a.step == #a.path then speed = math.min(speed, d * 1.5 + 0.3) end
    move(a, dx / d * speed, dz / d * speed, dt)
    -- Steckt fest (an einer Laterne, an einer anderen Figur): erst den Punkt
    -- auslassen, dann hinüberheben.
    a.stuck = a.stuck + dt
    if a.stuck > 2 then
        local moved = math.sqrt(dist2(a.lastX, a.lastZ, a.x, a.z))
        if moved < 0.5 then
            a.stuckCount = (a.stuckCount or 0) + 1
            if a.stuckCount >= 3 then
                a.x, a.z, a.y = wp.x, wp.z, wp.y
                game.removeCharacter(a.id)   -- die Kapsel neu, wo die Figur jetzt steht
                place(a)
                a.stuckCount = 0
            end
            a.step = a.step + 1
        else
            a.stuckCount = 0
        end
        a.stuck, a.lastX, a.lastZ = 0, a.x, a.z
    end
    return true
end

-- --- Zeichnen -----------------------------------------------------------------------

local function bubble(a, camX, camY, camZ)
    if a.inside then return end
    local b = a.bubble
    local thinking = a.state == "think" and _now - (a.thinkSince or 0) > 0.6
    local talkWait = a.state == "talk" and a.talk and not a.talk.lines
    if (not b or _now > b.untilT) and not thinking and not talkWait then return end
    local d = math.sqrt(dist2(camX, camZ, a.x, a.z) + (camY - a.y) ^ 2)
    if d > BUBBLE_RANGE then return end
    local hx, hy = game.worldToHud(a.x, a.y + 2.15, a.z)   -- a.y: die Füße
    if not hx then return end
    local alpha = clamp((BUBBLE_RANGE - d) / 10, 0, 1)
    local size = clamp(30 - d * 0.25, 18, 28)
    local text = (b and _now <= b.untilT) and b.text or "…"
    local lines = wrapText(text, 34)
    local lh = size * 1.2
    local w = 0
    for _, l in ipairs(lines) do w = math.max(w, game.hudTextSize(l, size) or 0) end
    local nameSize = size * 0.7
    local nw = game.hudTextSize(a.p.name, nameSize, true) or 0
    w = math.max(w, nw) + 26
    local h = #lines * lh + nameSize * 1.25 + 16
    local x, y = hx - w * 0.5, hy - h - 14
    local italic = b and b.italic
    local br, bg, bb = 0.98, 0.98, 0.96
    if italic then br, bg, bb = 0.9, 0.92, 0.97 end
    game.hudRect(x, y, w, h, br, bg, bb, 0.92 * alpha, 12)
    game.hudTri(hx - 9, y + h - 0.5, hx + 9, y + h - 0.5, hx, y + h + 12, br, bg, bb, 0.92 * alpha)
    game.hudText(x + 13, y + 7, a.p.name, nameSize, 0.35, 0.42, 0.62, alpha, 0, true)
    local ty = y + 8 + nameSize * 1.25
    local c = italic and 0.38 or 0.1
    for _, l in ipairs(lines) do
        game.hudText(x + 13, ty, l, size, c, c, c + 0.02, alpha, 0, false)
        ty = ty + lh
    end
end

local function stateText(a)
    if a.inside then
        local what = a.state == "think" and "überlegt …" or (a.plan and a.plan.activity or "")
        return what .. " (drinnen, " .. (a.here and a.here.name or "?") .. ")"
    end
    if a.state == "think" then return "überlegt …" end
    if a.state == "walk" then return "→ " .. a.target.name .. (a.plan and (": " .. a.plan.activity) or "") end
    if a.state == "do" then return (a.plan and a.plan.activity or "wartet") .. " @ " .. (a.here and a.here.name or "?") end
    if a.state == "talk" then
        local o = a.talk and (a.talk.a == a and a.talk.b or a.talk.a)
        return "redet mit " .. (o and o.p.name or "?")
    end
    return a.state
end

local function panel()
    local x, y = 24, 24
    local rows = { string.format("Stadt  %s Uhr   (%d Leute, %s)", clockText(), #_agents, MODEL) }
    if not _llmOk then
        if _llmError then rows[#rows + 1] = "Modell: " .. _llmError
        elseif llm.pending() > 0 then rows[#rows + 1] = "Modell lädt … (beim ersten Mal ~30 s)" end
    end
    local missing = {}
    for pf in pairs(_missing) do missing[#missing + 1] = pf end
    if #missing > 0 then
        table.sort(missing)
        rows[#rows + 1] = "Prefab fehlt im Projekt: " .. table.concat(missing, ", ") ..
                          " -> Säulen als Ersatz"
    end
    if #_agents == 0 then rows[#rows + 1] = "Keine Leute: keine Wohnhäuser in der Stadt gefunden" end
    if _context ~= "" then
        local c = _context
        if utf8.len(c) and utf8.len(c) > 70 then c = c:sub(1, utf8.offset(c, 70) - 1) .. " …" end
        rows[#rows + 1] = "Für alle: " .. c
    end
    if _showList then
        for _, a in ipairs(_agents) do
            rows[#rows + 1] = string.format("%s (%d)  %s", a.p.name, a.p.age, stateText(a))
        end
    end
    local w = 0
    local first = #rows - (_showList and #_agents or 0)   -- Zeilen vor der Personenliste
    for i, r in ipairs(rows) do
        w = math.max(w, (game.hudTextSize(r, i == 1 and 20 or 18, i == 1) or 0) + (i > first and 30 or 0))
    end
    game.hudRect(x - 10, y - 8, w + 20, #rows * 26 + 12, 0.05, 0.06, 0.08, 0.62, 8)
    for i, r in ipairs(rows) do
        local tx = x
        if i > first then
            -- Farbe und Nummer der Person (die Taste für die freie Kamera).
            local a = _agents[i - first]
            local c = colorOf(a)
            game.hudRect(x, y + (i - 1) * 26 + 2, 20, 20, c[1], c[2], c[3], a.inside and 0.5 or 1, 4)
            game.hudText(x + 10, y + (i - 1) * 26 + 3, tostring(a.i), 14, 0.05, 0.05, 0.05, 1, 0.5, true)
            tx = x + 30
        end
        game.hudText(tx, y + (i - 1) * 26, r, i == 1 and 20 or 18, 0.95, 0.95, 0.97, 0.95, 0, i == 1)
    end
end

-- --- Chat mit der Stadt und ihren Leuten -----------------------------------------------

local _chat = {
    open = false, text = "", target = 0,   -- 0 = die Stadt (Erzähler), sonst _agents[i]
    log = {},                               -- { who=, text=, kind="me"|"town"|"agent"|"info", t= }
    town = {},                              -- Verlauf mit dem Erzähler (messages)
    waiting = nil,                          -- llm-id, auf die gewartet wird
    drain = false, backT = 0,
}

-- Wo deine Figur steht: die Figur, die sich in shared.figures meldet (der
-- Controller), sonst die Kamera.
local function playerId()
    if shared and shared.figures then
        for id in pairs(shared.figures) do
            local mine = false
            for _, a in ipairs(_agents) do if a.id == id then mine = true break end end
            if not mine and game.getPos(id) then return id end
        end
    end
end

local function playerPos()
    local id = playerId()
    if id then return game.getPos(id) end
    return game.cameraPos()
end

-- Emojis und andere Zeichen jenseits von U+FFFF hat die HUD-Schrift nicht.
local function printable(s)
    return (tostring(s):gsub("[\240-\247][\128-\191][\128-\191][\128-\191]", ""):gsub("%s+$", ""))
end

local function chatLog(kind, who, text)
    text = printable(text)
    _chat.log[#_chat.log + 1] = { kind = kind, who = who, text = text, t = _now }
    while #_chat.log > 60 do table.remove(_chat.log, 1) end
end

local function targetName()
    local a = _agents[_chat.target]
    return a and a.p.name or "Stadt"
end

local function distTo(a)
    local px, _, pz = playerPos()
    return math.sqrt(dist2(px, pz, a.x, a.z))
end

-- Was die Stadt gerade weiß: jede Person, wo sie ist, was sie tut, wie weit weg.
local function townStatus()
    local rows = {}
    for _, a in ipairs(_agents) do
        local street = game.streetAt(a.x, a.z) or "?"
        local where = a.inside and a.here and (a.here.name .. ", drinnen")
                      or (a.state == "walk" and a.target and ("unterwegs zu " .. a.target.name))
                      or (a.here and a.here.name) or street
        rows[#rows + 1] = string.format(
            "- %s (%d, %s, wohnt %s): %s. Ort: %s; Straße: %s; %d m von %s entfernt.%s",
            a.p.name, a.p.age, a.p.job, a.home.name, stateText(a), where, street,
            math.floor(distTo(a)), PLAYER_NAME,
            a.memory[1] and (" Zuletzt: " .. a.memory[1]) or "")
    end
    return table.concat(rows, "\n")
end

local function askTown(text)
    local system = string.format(
        "Du bist der Erzähler einer deutschen Kleinstadt in einem Spiel. Es ist %s Uhr. " ..
        "Du weißt genau, wo jede Person gerade ist und was sie tut -- aber nur aus dieser " ..
        "Liste, die stimmt:\n%s\n\n" ..
        "Der Spieler heißt %s. Beantworte seine Fragen kurz (1-3 Sätze; fragt er nach " ..
        "allen, eine kurze Zeile pro Person), auf Deutsch, freundlich und konkret (Namen, " ..
        "Orte, Straßen, Entfernungen). Erfinde keine Personen und keine Orte; was nicht " ..
        "in der Liste steht, weißt du nicht.",
        clockText(), townStatus(), PLAYER_NAME) .. contextNote()
    local msgs = {}
    for i = math.max(1, #_chat.town - 7), #_chat.town do msgs[#msgs + 1] = _chat.town[i] end
    msgs[#msgs + 1] = { role = "user", content = text }
    _chat.town[#_chat.town + 1] = { role = "user", content = text }
    -- Mit Schema: ohne denkt das kleine Modell sonst auf Englisch laut nach.
    local schema = { type = "object", properties = { answer = { type = "string" } },
                     required = { "answer" } }
    local id = llm.chat{ model = MODEL, system = system, messages = msgs, priority = true,
                         format = schema, options = { temperature = 0.4, num_predict = 600 } }
    _requests[id] = { kind = "chat", target = 0 }
    _chat.waiting = id
end

local function askAgent(a, text)
    local d = distTo(a)
    local near = d <= NEAR_PLAYER and not a.inside
    local list = candidates(a)
    local names = { "" }
    local lines = {}
    for _, p in ipairs(list) do
        names[#names + 1] = p.name
        lines[#lines + 1] = "- " .. placeLabel(a, p)
    end
    local how = near and (PLAYER_NAME .. " steht direkt vor dir und spricht dich an.")
                or string.format("%s ist %d m entfernt und schreibt dir eine Nachricht aufs Handy.",
                                 PLAYER_NAME, math.floor(d))
    local system = string.format(
        "Du bist %s, %d Jahre alt, %s in einer deutschen Kleinstadt. Du bist %s. " ..
        "Du wohnst in %s. Es ist %s Uhr. Gerade: %s. %s\n" ..
        "Was zuletzt war:\n%s\n\n" ..
        "Antworte in deiner Rolle, kurz (1-2 Sätze), umgangssprachlich, auf Deutsch, als " ..
        "reply. Bittet %s dich, irgendwohin zu gehen oder mitzukommen, und du willst das, " ..
        "setze go auf genau einen dieser Orte, sonst go = \"\":\n%s",
        a.p.name, a.p.age, a.p.job, a.p.traits, a.home.name, clockText(), describe(a), how,
        #a.memory > 0 and table.concat(a.memory, "\n") or "(nichts Besonderes)",
        PLAYER_NAME, table.concat(lines, "\n")) .. contextNote()
    a.chat = a.chat or {}
    local msgs = {}
    for i = math.max(1, #a.chat - 7), #a.chat do msgs[#msgs + 1] = a.chat[i] end
    msgs[#msgs + 1] = { role = "user", content = text }
    a.chat[#a.chat + 1] = { role = "user", content = text }
    local schema = {
        type = "object",
        properties = { reply = { type = "string" }, go = { type = "string", enum = names } },
        required = { "reply", "go" },
    }
    local id = llm.chat{ model = MODEL, system = system, messages = msgs, format = schema,
                         priority = true, options = { temperature = 0.8, num_predict = 180 } }
    _requests[id] = { kind = "chat", target = a.i, agent = a, near = near, said = text }
    _chat.waiting = id
    -- Wer vor dir steht, bleibt stehen und hört zu.
    if near then a.listenUntil = _now + 20 end
end

local function onChat(r, ans)
    if _chat.waiting == ans.id then _chat.waiting = nil end
    if not ans.ok then
        chatLog("info", "", "(keine Antwort: " .. tostring(ans.error) .. ")")
        return
    end
    if r.target == 0 then
        local answer = type(ans.data) == "table" and ans.data.answer
        if not answer then
            log("Chat Stadt (kein JSON, " .. tostring(ans.tokens) .. " Token): " ..
                ans.text:sub(1, 300):gsub("\n", "|"))
            -- Abgeschnitten: was bis dahin dastand.
            answer = ans.text:match('"answer"%s*:%s*"(.-)"') or ans.text:match('"answer"%s*:%s*"(.*)$')
            answer = answer and (answer:gsub("\\n", "\n"):gsub('\\"', '"') .. " …")
                     or "Das weiß ich gerade nicht."
        end
        answer = tostring(answer)
        chatLog("town", "Stadt", answer)
        log("Chat Stadt: " .. answer)
        _chat.town[#_chat.town + 1] = { role = "assistant", content = answer }
        return
    end
    local a = r.agent
    local d = type(ans.data) == "table" and ans.data or {}
    local reply = tostring(d.reply or ans.text)
    chatLog("agent", a.p.name, reply)
    log("Chat " .. a.p.name .. ": " .. reply .. (d.go and d.go ~= "" and ("  [go " .. d.go .. "]") or ""))
    a.chat[#a.chat + 1] = { role = "assistant", content = json.encode(d) }
    remember(a, PLAYER_NAME .. " sagte: \"" .. r.said .. "\" -- du: \"" .. reply .. "\"")
    if r.near then
        say(a, reply)
        a.listenUntil = _now + math.max(3, utf8.len(reply) * 0.07)
    end
    local place = d.go and d.go ~= "" and _byName[d.go]
    if place and a.state ~= "talk" then
        a.plan = { place = place, activity = "trifft sich dort, wie mit " .. PLAYER_NAME ..
                   " besprochen", minutes = 30 }
        if a.inside then comeOut(a) end
        if goTo(a, place) then
            chatLog("info", "", "(" .. a.p.name .. " geht zu " .. place.name .. ")")
            log(a.p.name .. " geht auf " .. PLAYER_NAME .. "s Bitte zu " .. place.name)
        end
    end
end

-- "/alle ...": die Lage für alle setzen, zeigen oder löschen.
local function setContext(text)
    if text == "" then
        chatLog("info", "", _context ~= "" and ("Für alle gilt: " .. _context)
                                           or "Für alle gilt gerade nichts Besonderes.")
        return
    end
    if text == "aus" or text == "-" then
        _context = ""
        chatLog("info", "", "Für alle gilt wieder nichts Besonderes.")
    else
        _context = text
        chatLog("info", "", "Für alle gilt jetzt: " .. text)
    end
    log("Für alle: " .. (_context ~= "" and _context or "(nichts)"))
    -- Alle überlegen neu, was sie jetzt tun (wer gerade redet, danach).
    for _, a in ipairs(_agents) do
        if a.state == "do" or a.state == "walk" then
            a.state = "plan"
            a.path = nil
        elseif a.state == "talk" then
            a.resume = "plan"
        end
    end
end

local function sendChat()
    local text = _chat.text:gsub("^%s+", ""):gsub("%s+$", "")
    _chat.text = ""
    if text == "" then return end
    local cmd, rest = text:match("^/(%S+)%s*(.*)$")
    if cmd and cmd:lower() == "alle" then
        setContext(rest)
        return
    end
    -- "@Name rest": an diese Person.
    local who, rest = text:match("^@(%S+)%s*(.*)$")
    if who then
        local low = who:lower()
        _chat.target = 0
        if low ~= "stadt" then
            for i, a in ipairs(_agents) do
                if a.p.name:lower():sub(1, #low) == low then _chat.target = i break end
            end
        end
        text = rest
        if text == "" then return end
    end
    chatLog("me", PLAYER_NAME .. " → " .. targetName(), text)
    log("Chat " .. PLAYER_NAME .. " -> " .. targetName() .. ": " .. text)
    local a = _agents[_chat.target]
    if a then askAgent(a, text) else askTown(text) end
end

local function utf8Pop(s)
    local n = utf8.len(s)
    if not n or n == 0 then return "" end
    return s:sub(1, utf8.offset(s, n) - 1)
end

local function tickChat(dt)
    local key = game["KEY_" .. CHAT_KEY:upper()]
    if not _chat.open then
        if key and game.keyPressed(key) then
            _chat.open, _chat.drain = true, true
        end
        return
    end
    game.captureInput()   -- die Figur läuft nicht los, während man tippt
    local typed = game.textInput()
    if _chat.drain then
        _chat.drain = false   -- das T, das den Chat geöffnet hat
    elseif typed ~= "" then
        _chat.text = _chat.text .. typed
    end
    if game.keyPressed(game.KEY_ESCAPE) then _chat.open = false return end
    if game.keyPressed(game.KEY_TAB) then _chat.target = (_chat.target + 1) % (#_agents + 1) end
    if game.keyPressed(game.KEY_ENTER) then sendChat() end
    -- Backspace: einmal, und gehalten nach einer Weile immer weiter.
    if game.keyPressed(game.KEY_BACKSPACE) then
        _chat.text = utf8Pop(_chat.text)
        _chat.backT = -0.45
    elseif game.keyDown(game.KEY_BACKSPACE) then
        _chat.backT = _chat.backT + dt
        while _chat.backT > 0.04 do _chat.text = utf8Pop(_chat.text); _chat.backT = _chat.backT - 0.04 end
    end
end

local CHAT_COLORS = {
    me = { 0.62, 0.8, 1.0 }, town = { 1.0, 0.86, 0.5 }, agent = { 0.95, 0.95, 0.97 },
    info = { 0.6, 0.62, 0.66 },
}

local function drawChat()
    local W = game.hudSize()
    local x, w = 24, math.min(860, W - 48)
    local size, lh = 20, 26
    -- Geschlossen: nur die letzten Nachrichten, ein paar Sekunden lang.
    local recent = {}
    for _, m in ipairs(_chat.log) do
        if _chat.open or _now - m.t < 15 then recent[#recent + 1] = m end
    end
    local rows = {}
    for _, m in ipairs(recent) do
        local head = m.who ~= "" and (m.who .. ": ") or ""
        for para in string.gmatch(head .. m.text, "[^\n]+") do
            for _, l in ipairs(wrapText(para, 72)) do
                rows[#rows + 1] = { text = l, c = CHAT_COLORS[m.kind] or CHAT_COLORS.info }
            end
        end
    end
    local maxRows = _chat.open and 12 or 4
    local first = math.max(1, #rows - maxRows + 1)
    local shown = #rows - first + 1
    if not _chat.open and shown == 0 then
        game.hudText(x, 1080 - 44, CHAT_KEY:upper() .. ": Chat mit der Stadt", 18, 1, 1, 1, 0.55, 0, false)
        return
    end
    local extra = _chat.open and 2 or 0
    local h = (shown + extra) * lh + 18
    local y = 1080 - 24 - h
    game.hudRect(x - 10, y, w, h, 0.05, 0.06, 0.08, _chat.open and 0.82 or 0.5, 10)
    local ty = y + 9
    if _chat.open then
        local waiting = _chat.waiting and ("   " .. targetName() .. " schreibt …") or ""
        game.hudText(x, ty, "an: " .. targetName() ..
                     "   (Tab wechselt, @Name, /alle Text = für alle, Enter schickt, Esc schließt)" ..
                     waiting, 17, 0.7, 0.72, 0.78, 1, 0, false)
        ty = ty + lh
    end
    for i = first, #rows do
        local r = rows[i]
        game.hudText(x, ty, r.text, size, r.c[1], r.c[2], r.c[3], 1, 0, false)
        ty = ty + lh
    end
    if _chat.open then
        local cursor = (math.floor(_now * 2) % 2 == 0) and "▌" or " "
        local line = "> " .. _chat.text
        local lines = wrapText(line, 72)
        game.hudText(x, ty, (lines[#lines] or "> ") .. cursor, size, 1, 1, 1, 1, 0, true)
    end
end

-- --- Wo sind sie? Markierungen --------------------------------------------------------

local _markers = true

-- Die Blickrichtung als Achsen: vorne, rechts, oben (für die Pfeile am Rand).
local function cameraAxes()
    local fx, fy, fz = game.cameraDir()
    local rx, rz = -fz, fx
    local rl = math.sqrt(rx * rx + rz * rz)
    if rl < 1e-4 then rx, rz, rl = 1, 0, 1 end
    rx, rz = rx / rl, rz / rl
    -- oben = rechts x vorne
    local ux, uy, uz = -rz * fy, rz * fx - rx * fz, rx * fy
    return fx, fy, fz, rx, rz, ux, uy, uz
end

local function marker(a, cx, cy, cz, W, axes)
    local c = colorOf(a)
    local ay = a.y   -- die Füße
    local d = math.sqrt(dist2(cx, cz, a.x, a.z) + (cy - ay) ^ 2)
    local alpha = a.inside and 0.55 or 1
    local label = string.format("%d %s  %dm%s", a.i, a.p.name, math.floor(d + 0.5),
                                a.inside and "  (drinnen)" or "")
    local hx, hy = game.worldToHud(a.x, ay + 2.35, a.z)
    local onScreen = hx and hx > 30 and hx < W - 30 and hy > 30 and hy < 1080 - 30
    if onScreen then
        -- Nah, mit Sprechblase: die nennt den Namen schon -- nur der Pfeil.
        local bubbleUp = d < BUBBLE_RANGE and a.bubble and _now <= a.bubble.untilT and not a.inside
        local s = clamp(18 - d * 0.02, 10, 18)
        local tipY = hy
        if bubbleUp then return end
        game.hudTri(hx - s * 0.7, tipY - s * 1.3, hx + s * 0.7, tipY - s * 1.3, hx, tipY,
                    c[1], c[2], c[3], 0.95 * alpha)
        local tw = game.hudTextSize(label, 16, true) or 80
        game.hudRect(hx - tw * 0.5 - 6, tipY - s * 1.3 - 26, tw + 12, 22, 0.05, 0.06, 0.08, 0.6 * alpha, 6)
        game.hudText(hx, tipY - s * 1.3 - 24, label, 16, c[1], c[2], c[3], alpha, 0.5, true)
        return
    end
    -- Außerhalb des Bildes: ein Pfeil am Rand, der zu ihr zeigt.
    local fx, fy, fz, rx, rz, ux, uy, uz = table.unpack(axes)
    local vx, vy, vz = a.x - cx, ay + 1 - cy, a.z - cz
    local sx = vx * rx + vz * rz
    local sy = vx * ux + vy * uy + vz * uz
    if hx and (vx * fx + vy * fy + vz * fz) > 0 then
        sx, sy = hx - W * 0.5, 540 - hy   -- vor der Kamera: genau aus dem Bildpunkt
    end
    local ang = math.atan(sy, sx)
    local ex, ey = math.cos(ang), -math.sin(ang)
    local mx, my = W * 0.5 - 46, 540 - 46
    local k = math.min(mx / math.max(math.abs(ex), 1e-4), my / math.max(math.abs(ey), 1e-4))
    local px, py = W * 0.5 + ex * k, 540 + ey * k
    local s = 16
    local nx, ny = -ey, ex
    game.hudTri(px + ex * s, py + ey * s, px - ex * s * 0.6 + nx * s * 0.8, py - ey * s * 0.6 + ny * s * 0.8,
                px - ex * s * 0.6 - nx * s * 0.8, py - ey * s * 0.6 - ny * s * 0.8,
                c[1], c[2], c[3], 0.9 * alpha)
    local lx = clamp(px - ex * 34, 60, W - 60)
    local ly = clamp(py - ey * 34 - 9, 20, 1080 - 40)
    game.hudText(lx, ly, label, 15, c[1], c[2], c[3], 0.95 * alpha, 0.5, true)
end

local function drawMarkers()
    if not _markers then return end
    local W = game.hudSize()
    local cx, cy, cz = game.cameraPos()
    local axes = { cameraAxes() }
    for _, a in ipairs(_agents) do
        if a.state ~= "start" then marker(a, cx, cy, cz, W, axes) end
    end
end

-- --- Freie Kamera ----------------------------------------------------------------------

local _cam = { on = false, x = 0, y = 0, z = 0, yaw = 0, pitch = 0, speed = FLY_SPEED,
               follow = nil, mx = nil, my = nil, home = nil }

-- Die Kamera deiner Figur (ein Kind-Objekt mit "Camera" im Namen, sonst das erste
-- leere Kind), um nach dem Fliegen dorthin zurückzuschalten.
local function playerCamera()
    local pid = playerId()
    if not pid then return nil end
    local empty
    for _, c in ipairs(game.children(pid) or {}) do
        local n = (game.getName(c) or ""):lower()
        if n:find("camera") or n:find("kamera") then return c end
        local info = game.entityInfo(c)
        if not empty and info and info.type == game.EMPTY then empty = c end
    end
    return empty
end

local function camDir()
    local cp = math.cos(math.rad(_cam.pitch))
    return math.sin(math.rad(_cam.yaw)) * cp, math.sin(math.rad(_cam.pitch)),
           math.cos(math.rad(_cam.yaw)) * cp
end

local function freeCamOn()
    _cam.on = true
    _cam.home = playerCamera()
    _cam.x, _cam.y, _cam.z = game.cameraPos()
    local dx, dy, dz = game.cameraDir()
    _cam.yaw = math.deg(math.atan(dx, dz))
    _cam.pitch = math.deg(math.asin(clamp(dy, -1, 1)))
    _cam.follow, _cam.mx = nil, nil
    game.setCamera(-1)        -- das Skript führt die Kamera
    game.showCursor(true)     -- für die rechte Maustaste
end

local function freeCamOff()
    _cam.on = false
    game.showCursor(false)
    game.setCamera(_cam.home or -1)
end

local function held(...)
    for _, k in ipairs({ ... }) do if game.keyDown(k) then return true end end
    return false
end

local function tickFreeCam(dt)
    local key = game["KEY_" .. FREECAM_KEY:upper()]
    if not _cam.on then
        -- Einschalten, und erst im nächsten Frame fliegen: dieselbe Taste ist in
        -- diesem Frame noch "gedrückt" und schaltete sie gleich wieder aus.
        if not _chat.open and key and game.keyPressed(key) then freeCamOn() end
        return
    end
    game.captureInput()   -- deine Figur bleibt stehen, die Tasten gehören der Kamera
    if not _chat.open then
        if game.keyPressed(key) or game.keyPressed(game.KEY_ESCAPE) then freeCamOff() return end
        -- Ziffern: zu dieser Person fliegen und ihr folgen; 0 zu deiner Figur.
        for n = 0, math.min(9, #_agents) do
            if game.keyPressed(game["KEY_" .. n]) then
                if n == 0 then
                    local x, y, z = playerPos()
                    _cam.follow = nil
                    _cam.goal = { x = x, y = y, z = z }
                else
                    _cam.follow, _cam.goal = _agents[n], nil
                end
            end
        end
        -- Umsehen: Pfeiltasten, oder rechte Maustaste gehalten und ziehen.
        local turn = 70 * dt
        if held(game.KEY_LEFT)  then _cam.yaw = _cam.yaw + turn end
        if held(game.KEY_RIGHT) then _cam.yaw = _cam.yaw - turn end
        if held(game.KEY_UP)    then _cam.pitch = _cam.pitch + turn end
        if held(game.KEY_DOWN)  then _cam.pitch = _cam.pitch - turn end
        local mx, my = game.mousePos()
        if game.mouseDown(game.MOUSE_RIGHT) and mx and _cam.mx then
            _cam.yaw = _cam.yaw - (mx - _cam.mx) * 0.18
            _cam.pitch = _cam.pitch - (my - _cam.my) * 0.18
        end
        _cam.mx, _cam.my = mx, my
        _cam.pitch = clamp(_cam.pitch, -89, 89)
        local wheel = game.mouseWheel()
        if wheel ~= 0 then _cam.speed = clamp(_cam.speed * (1.25 ^ wheel), 1, 400) end
        -- Fliegen: in Blickrichtung, quer dazu, hoch und runter.
        local fx, fy, fz = camDir()
        local rx, rz = -fz, fx
        local rl = math.sqrt(rx * rx + rz * rz)
        if rl > 1e-4 then rx, rz = rx / rl, rz / rl end
        local ahead = (held(game.KEY_W) and 1 or 0) - (held(game.KEY_S) and 1 or 0)
        local side = (held(game.KEY_D) and 1 or 0) - (held(game.KEY_A) and 1 or 0)
        local up = (held(game.KEY_SPACE, game.KEY_E) and 1 or 0) -
                   (held(game.KEY_LCTRL, game.KEY_Q) and 1 or 0)
        if ahead ~= 0 or side ~= 0 or up ~= 0 then _cam.follow, _cam.goal = nil, nil end
        local v = _cam.speed * (held(game.KEY_LSHIFT, game.KEY_RSHIFT) and 4 or 1) * dt
        -- (rx, rz) = vorne x oben: rechts auf dem Boden (wie in walker.lua).
        _cam.x = _cam.x + (fx * ahead + rx * side) * v
        _cam.y = _cam.y + (fy * ahead + up) * v
        _cam.z = _cam.z + (fz * ahead + rz * side) * v
    end
    -- Folgen: schräg hinter und über der Person, weich nachgezogen.
    local tx, ty, tz
    if _cam.follow then
        local a = _cam.follow
        tx, ty, tz = a.x, a.y + 1.2, a.z
        local back = math.rad(a.yaw)
        local wx, wy, wz = tx - math.sin(back) * 9, ty + 5, tz - math.cos(back) * 9
        local k = 1 - math.exp(-2.5 * dt)
        _cam.x, _cam.y, _cam.z = _cam.x + (wx - _cam.x) * k, _cam.y + (wy - _cam.y) * k,
                                 _cam.z + (wz - _cam.z) * k
    elseif _cam.goal then
        local g = _cam.goal
        tx, ty, tz = g.x, g.y + 1.2, g.z
        local k = 1 - math.exp(-2.5 * dt)
        _cam.x, _cam.y, _cam.z = _cam.x + (tx - 8 - _cam.x) * k, _cam.y + (ty + 5 - _cam.y) * k,
                                 _cam.z + (tz - 8 - _cam.z) * k
    end
    if tx then
        -- Hinsehen: Gier und Neigung aus der Richtung zum Ziel.
        local dx, dy, dz = tx - _cam.x, ty - _cam.y, tz - _cam.z
        local h = math.sqrt(dx * dx + dz * dz)
        _cam.yaw = math.deg(math.atan(dx, dz))
        _cam.pitch = math.deg(math.atan(dy, math.max(h, 1e-3)))
    end
    -- Nicht unter den Boden.
    local gy = game.terrainHeight(_cam.x, _cam.z)
    if gy and _cam.y < gy + 0.5 then _cam.y = gy + 0.5 end
    local fx, fy, fz = camDir()
    game.setCameraPos(_cam.x, _cam.y, _cam.z)
    game.setCameraDir(fx, fy, fz)
end

local function drawFreeCamHint()
    if not _cam.on or _chat.open then return end
    local W = game.hudSize()
    local who = _cam.follow and ("folgt " .. _cam.follow.p.name .. "   ") or ""
    local t = string.format("%sFreie Kamera  %.0f m/s   WASD fliegen · Leertaste/Strg hoch/runter · " ..
                            "Shift schnell · Mausrad Tempo · Pfeile/rechte Maus umsehen · " ..
                            "1-%d Person · 0 du · %s/Esc zurück",
                            who, _cam.speed, math.max(1, #_agents), FREECAM_KEY:upper())
    local tw = game.hudTextSize(t, 17) or 600
    game.hudRect(W * 0.5 - tw * 0.5 - 12, 1080 - 64, tw + 24, 30, 0.05, 0.06, 0.08, 0.7, 8)
    game.hudText(W * 0.5, 1080 - 60, t, 17, 0.95, 0.95, 0.97, 1, 0.5, false)
end

-- --- Jeder Frame ---------------------------------------------------------------------

function start(self)
    math.randomseed(os.time())
    llm.setModel(MODEL)
    _clock = START_HOUR * 60
    _showList = SHOW_LIST
    _context = tostring(CONTEXT or ""):gsub("^%s+", ""):gsub("%s+$", "")
    loadPlaces()
    if #_places == 0 then
        game.log("[agenten] Keine Stadt in der Szene (game.townPlaces ist leer).")
        return
    end
    spawnAgents()
end

function update(self, dt, t)
    _now = t
    if #_places == 0 then
        game.setHud("town_agents: keine Stadt in dieser Szene")
        return
    end
    _clock = _clock + dt * CLOCK_SPEED
    -- Kamera vor Chat: Esc im offenen Chat schließt nur den Chat.
    tickFreeCam(dt)
    tickChat(dt)
    if not _chat.open then
        if game.keyPressed(game.KEY_H) then _showList = not _showList end
        local mk = game["KEY_" .. MARKER_KEY:upper()]
        if mk and game.keyPressed(mk) then _markers = not _markers end
    end

    -- Antworten des Modells.
    for _, ans in ipairs(llm.poll()) do
        local r = _requests[ans.id]
        _requests[ans.id] = nil
        if ans.ok then _llmOk, _llmError = true, nil
        else _llmError = ans.error; log("Modell: " .. tostring(ans.error)) end
        if r and r.kind == "plan" and r.agent.state == "think" then onPlan(r.agent, ans)
        elseif r and r.kind == "talk" then onTalk(r.talk, ans)
        elseif r and r.kind == "chat" then onChat(r, ans) end
    end

    for _, a in ipairs(_agents) do
        if a.state == "start" then
            -- Die Figur erscheint einen Frame nach spawnPrefab. Ein Modell wird um
            -- die Mitte seines Kastens gezeichnet, nicht um die Füße: der Ursprung
            -- steht eine halbe Höhe über dem Boden (sonst steckt sie halb darin).
            if game.getPos(a.id) then
                local _, hy = game.getScale(a.id)
                a.footOff = hy or 0.9
                a.state = "plan"
            end
        end
        if a.state == "plan" then
            if _llmError and not _llmOk then fallbackPlan(a) else askPlan(a) end
        end
        local walking = false
        local listening = a.listenUntil and _now < a.listenUntil and not a.inside and
                          a.state ~= "start" and a.state ~= "talk"
        if listening then
            -- Hört dir zu: stehen bleiben, dich ansehen.
            move(a, 0, 0, dt)
            local px, _, pz = playerPos()
            face(a, px, pz, dt, 300)
        elseif a.inside then
            -- Drinnen: nur die Uhr. Herauskommen erst, wenn das Modell sagt, wohin.
            if a.state == "do" and _now > a.doUntil then a.state = "plan" end
            if a.state == "think" and _now - a.thinkSince > 120 then fallbackPlan(a) end
        elseif a.state == "walk" then
            walking = tickWalk(a, dt)
        elseif a.state ~= "start" then
            move(a, 0, 0, dt)   -- stehen bleiben, aber auf dem Boden
            if a.state == "do" then
                if a.here then face(a, a.here.ax, a.here.az, dt, 120) end
                if _now > a.doUntil then a.state = "plan" end
            elseif a.state == "talk" and a.talk then
                local o = a.talk.a == a and a.talk.b or a.talk.a
                face(a, o.x, o.z, dt, 240)
            elseif a.state == "think" and _now - a.thinkSince > 120 then
                fallbackPlan(a)   -- das Modell hat diese Anfrage verloren
            end
        end
        if a.state ~= "start" and not a.inside then
            place(a)
            animate(a, walking, WALK_SPEED)
        end
    end

    for i = #_talks, 1, -1 do tickTalk(_talks[i]) end
    lookForMeetings()

    local cx, cy, cz = game.cameraPos()
    for _, a in ipairs(_agents) do bubble(a, cx, cy, cz) end
    drawMarkers()
    panel()
    drawChat()
    drawFreeCamHint()
end
