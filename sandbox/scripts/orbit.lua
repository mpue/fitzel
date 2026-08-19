-- Lässt ein Objekt auf einer Kreisbahn laufen.
-- Inspector > Script: "orbit.lua", dann Play.
--
-- WO DER MITTELPUNKT LIEGT: dort, wo das Objekt beim Start von Play steht. Du
-- stellst es also auf die Mitte des Kreises, nicht auf die Bahn -- beim Start
-- springt es einmal auf den Radius hinaus und läuft von da los. Das ist die
-- Variante, die sich im Editor vorhersagen lässt: der Kreis ist da, wo du das
-- Objekt hingestellt hast.
--
-- Der Transform ist LOKAL (siehe docs/lua-scripting.md §2). Hängt das Objekt an
-- einem Parent, kreist es also im Raum des Parents und wandert mit ihm mit --
-- praktisch für eine Bahn, die selbst noch bewegt wird.

-- --- Inspector-Felder (ohne `local` = editierbar, pro Objekt gespeichert) ----

radius     = 8.0    -- Radius der Bahn in Metern
-- Bahngeschwindigkeit in Metern pro Sekunde, NICHT Grad pro Sekunde. Das ist der
-- Unterschied, der beim Ändern des Radius auffällt: bei m/s bleibt das Tempo
-- gleich und die Runde dauert länger, bei °/s wird das Objekt mit dem Radius
-- immer schneller. Für °/s statt m/s: unten in update() das `/ r` streichen und
-- `math.rad(speed)` nehmen.
speed      = 6.0
startAngle = 0.0    -- Startwinkel in Grad. Mehrere Objekte auf derselben Bahn
                    -- verteilst du damit gleichmässig (2 Objekte: 0 und 180).
clockwise  = false  -- Drehrichtung (von oben gesehen)
faceForward = true  -- Nase in Fahrtrichtung drehen (setzt e.ry). Gierwinkel 0
                    -- zeigt entlang +Z; ein Modell, das in seiner eigenen Datei
                    -- nach -Z schaut, fährt damit rückwärts -- dann startAngle
                    -- lassen und im Inspector die Y-Rotation des Modells um 180
                    -- drehen, oder faceForward aus.

-- --- Privater Zustand (mit `local` -> nicht im Inspector) --------------------

local cx, cy, cz   -- Mittelpunkt, beim Start eingefangen
local ang          -- aktueller Winkel im Bogenmass

function start(e)
    cx, cy, cz = e.x, e.y, e.z
    ang = math.rad(startAngle)
end

function update(e, dt, t)
    -- Radius 0 wäre eine Division durch null in der Winkelgeschwindigkeit, und
    -- ein negativer Radius dreht die Bahn nur um -- beides fängt der Betrag ab.
    local r = math.max(math.abs(radius), 0.001)
    local w = speed / r                 -- Bahn- -> Winkelgeschwindigkeit (rad/s)
    if clockwise then w = -w end

    -- Der Winkel wird aufsummiert und nicht aus `t` gerechnet: so ändert eine
    -- Tempoänderung im Inspector (oder zur Laufzeit) das Tempo, statt das Objekt
    -- an eine andere Stelle der Bahn zu setzen.
    ang = ang + w * dt

    e.x = cx + math.cos(ang) * r
    e.z = cz + math.sin(ang) * r
    e.y = cy

    if faceForward then
        -- Tangente an den Kreis, also die Ableitung der Position nach dem
        -- Winkel, mit der Drehrichtung im Vorzeichen.
        local s = (w >= 0.0) and 1.0 or -1.0
        local fx, fz = -math.sin(ang) * s, math.cos(ang) * s
        -- Gierwinkel 0 zeigt in fitzel entlang +Z und wächst Richtung +X.
        e.ry = math.deg(math.atan(fx, fz))
    end
end
