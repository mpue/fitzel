-- chess.lua -- Schach gegen eine eingebaute Engine, mit der Maus gespielt.
--
-- An die Wurzel des Schachbretts haengen, unter der die Figuren als Kinder
-- liegen (im Chess-Projekt: "chessboard"). Beim Start von Play liest das Skript
-- das Brett aus der Szene: die beiden Koenige legen fest, wo die Felder liegen
-- und welche Seite Weiss ist; jede Figur wird an ihrem Namen erkannt
-- (Pawn/Knight/Bishop/Rook/Queen/King, auch Bauer/Springer/Laeufer/Turm/Dame/
-- Koenig). Fehlende Figuren -- etwa sieben der acht Bauern -- werden aus einer
-- vorhandenen derselben Farbe geklont.
--
-- Bedienung, alles per Klick, nichts muss gezogen werden:
--   Klick auf eine eigene Figur waehlt sie, Klick auf ein markiertes Feld zieht.
--   Rechte Maustaste halten und bewegen oder Pfeiltasten: Blick drehen, W/S zoomen.
--   N neues Spiel, Z oder Backspace Zug zurueck, T Seite tauschen.
--
-- Die Engine: Alpha-Beta mit Ruhesuche, Figur-Feld-Tabellen, kleines
-- Eroeffnungsbuch. Ihre Staerke kommt aus der Suchtiefe und einem Schuss Zufall;
-- mit den Standardwerten spielt sie grob auf 1200-1400 Elo (geschaetzt, nicht
-- gegen bewertete Gegner gemessen). Sie rechnet in Scheiben von ~10 ms pro Frame,
-- das Bild laeuft waehrenddessen weiter.

-- Inspector-Felder --------------------------------------------------------------
menschSpieltWeiss = true   -- false: du fuehrst Schwarz, die Engine eroeffnet
suchtiefe         = 3      -- Halbzuege voller Breite: 2 schwach, 3 Standard, 4 stark
zufall            = 20     -- Rauschen auf die Zugbewertung, in Hundertstel Bauern
patzer            = 0.07   -- Anteil der Zuege, in denen sie einen schwaecheren waehlt
denkzeit          = 0.8    -- Sekunden, die sie mindestens ueberlegt
maxDenkzeit       = 6      -- Sekunden, nach denen sie spaetestens zieht
zugKlang          = "move.wav" -- erklingt, wenn die gezogene Figur aufsetzt ("" = still)

local PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING = 1, 2, 3, 4, 5, 6

-- ===========================================================================
-- Die Engine
-- ===========================================================================
-- 0x88-Brett: Feld = Reihe * 16 + Linie (a1 = 0, h1 = 7, a8 = 112). Figuren als
-- Zahlen, Weiss positiv, Schwarz negativ. Ein Zug ist eine Zahl:
-- von | nach << 7 | Umwandlung << 14 | Art << 17 (Art: 1 Doppelschritt,
-- 2 en passant, 3 Rochade).
local function newEngine()
    local E = {}

    local VALUE = {100, 320, 330, 500, 900, 0}
    local ORDERV = {1, 3, 3, 5, 9, 12}
    local PHASE = {0, 1, 1, 2, 4, 0}
    local KN   = {33, 31, 18, 14, -14, -18, -31, -33}
    local KD   = {1, -1, 16, -16, 17, 15, -15, -17}
    local DIAG = {17, 15, -15, -17}
    local ORTH = {1, -1, 16, -16}

    -- Figur-Feld-Tabellen (Tomasz Michniewski), Reihe 8 zuerst, aus Weiss' Sicht.
    local PST = {
        [PAWN] = {
             0,  0,  0,  0,  0,  0,  0,  0,
            50, 50, 50, 50, 50, 50, 50, 50,
            10, 10, 20, 30, 30, 20, 10, 10,
             5,  5, 10, 25, 25, 10,  5,  5,
             0,  0,  0, 20, 20,  0,  0,  0,
             5, -5,-10,  0,  0,-10, -5,  5,
             5, 10, 10,-20,-20, 10, 10,  5,
             0,  0,  0,  0,  0,  0,  0,  0 },
        [KNIGHT] = {
            -50,-40,-30,-30,-30,-30,-40,-50,
            -40,-20,  0,  0,  0,  0,-20,-40,
            -30,  0, 10, 15, 15, 10,  0,-30,
            -30,  5, 15, 20, 20, 15,  5,-30,
            -30,  0, 15, 20, 20, 15,  0,-30,
            -30,  5, 10, 15, 15, 10,  5,-30,
            -40,-20,  0,  5,  5,  0,-20,-40,
            -50,-40,-30,-30,-30,-30,-40,-50 },
        [BISHOP] = {
            -20,-10,-10,-10,-10,-10,-10,-20,
            -10,  0,  0,  0,  0,  0,  0,-10,
            -10,  0,  5, 10, 10,  5,  0,-10,
            -10,  5,  5, 10, 10,  5,  5,-10,
            -10,  0, 10, 10, 10, 10,  0,-10,
            -10, 10, 10, 10, 10, 10, 10,-10,
            -10,  5,  0,  0,  0,  0,  5,-10,
            -20,-10,-10,-10,-10,-10,-10,-20 },
        [ROOK] = {
              0,  0,  0,  0,  0,  0,  0,  0,
              5, 10, 10, 10, 10, 10, 10,  5,
             -5,  0,  0,  0,  0,  0,  0, -5,
             -5,  0,  0,  0,  0,  0,  0, -5,
             -5,  0,  0,  0,  0,  0,  0, -5,
             -5,  0,  0,  0,  0,  0,  0, -5,
             -5,  0,  0,  0,  0,  0,  0, -5,
              0,  0,  0,  5,  5,  0,  0,  0 },
        [QUEEN] = {
            -20,-10,-10, -5, -5,-10,-10,-20,
            -10,  0,  0,  0,  0,  0,  0,-10,
            -10,  0,  5,  5,  5,  5,  0,-10,
             -5,  0,  5,  5,  5,  5,  0, -5,
              0,  0,  5,  5,  5,  5,  0, -5,
            -10,  5,  5,  5,  5,  5,  0,-10,
            -10,  0,  5,  0,  0,  0,  0,-10,
            -20,-10,-10, -5, -5,-10,-10,-20 },
        [KING] = {
            -30,-40,-40,-50,-50,-40,-40,-30,
            -30,-40,-40,-50,-50,-40,-40,-30,
            -30,-40,-40,-50,-50,-40,-40,-30,
            -30,-40,-40,-50,-50,-40,-40,-30,
            -20,-30,-30,-40,-40,-30,-30,-20,
            -10,-20,-20,-20,-20,-20,-20,-10,
             20, 20,  0,  0,  0,  0, 20, 20,
             20, 30, 10,  0,  0, 10, 30, 20 },
    }
    -- Im Endspiel: Bauern nach vorn, der Koenig in die Mitte.
    local EG_PAWN = {
         0,  0,  0,  0,  0,  0,  0,  0,
        80, 80, 80, 80, 80, 80, 80, 80,
        50, 50, 50, 50, 50, 50, 50, 50,
        30, 30, 30, 30, 30, 30, 30, 30,
        15, 15, 15, 15, 15, 15, 15, 15,
         5,  5,  5,  5,  5,  5,  5,  5,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0 }
    local EG_KING = {
        -50,-40,-30,-20,-20,-30,-40,-50,
        -30,-20,-10,  0,  0,-10,-20,-30,
        -30,-10, 20, 30, 30, 20,-10,-30,
        -30,-10, 30, 40, 40, 30,-10,-30,
        -30,-10, 30, 40, 40, 30,-10,-30,
        -30,-10, 20, 30, 30, 20,-10,-30,
        -30,-30,  0,  0,  0,  0,-30,-30,
        -50,-30,-30,-30,-30,-30,-30,-50 }

    -- Vorzeichenbehaftet und mit Materialwert: MG[figur][feld], figur -6..6.
    local MG, EG = {}, {}
    for t = 1, 6 do
        local mgW, egW, mgB, egB = {}, {}, {}, {}
        local egt = (t == PAWN and EG_PAWN) or (t == KING and EG_KING) or PST[t]
        for r = 0, 7 do
            for f = 0, 7 do
                local sq = r * 16 + f
                local iw = (7 - r) * 8 + f + 1
                local ib = r * 8 + f + 1
                mgW[sq] = VALUE[t] + PST[t][iw];  egW[sq] = VALUE[t] + egt[iw]
                mgB[sq] = -(VALUE[t] + PST[t][ib]); egB[sq] = -(VALUE[t] + egt[ib])
            end
        end
        MG[t], EG[t], MG[-t], EG[-t] = mgW, egW, mgB, egB
    end

    -- Zobrist-Schluessel aus einem festen xorshift64 (nicht aus math.random, damit
    -- die Engine dessen Folge nicht verschiebt).
    local rs = 0x2545F4914F6CDD1D
    local function rnd()
        rs = rs ~ (rs << 13); rs = rs ~ (rs >> 7); rs = rs ~ (rs << 17)
        return rs
    end
    local Z = {}
    for p = -6, 6 do
        if p ~= 0 then
            local t = {}
            for sq = 0, 127 do t[sq] = rnd() end
            Z[p] = t
        end
    end
    local ZSIDE = rnd()
    local ZC, ZEP = {}, {}
    for i = 0, 15 do ZC[i] = rnd() end
    for f = 0, 7 do ZEP[f] = rnd() end

    -- Rochaderechte: 1 = Weiss kurz, 2 = Weiss lang, 4 = Schwarz kurz, 8 = Schwarz lang.
    local CMASK = {}
    for sq = 0, 127 do CMASK[sq] = 15 end
    CMASK[0] = 13; CMASK[7] = 14; CMASK[4] = 12
    CMASK[112] = 7; CMASK[119] = 11; CMASK[116] = 3

    -- Stellung ------------------------------------------------------------------
    local board = {}
    for i = 0, 127 do board[i] = 0 end
    local side, castle, ep, half = 1, 0, -1, 0
    local kingSq = {[1] = 4, [-1] = 116}
    local hash = 0
    local mg, eg, phase = 0, 0, 0
    local count = {}
    for p = -6, 6 do count[p] = 0 end
    local hMove, hCap, hCastle, hEp, hHalf, hHash = {}, {}, {}, {}, {}, {}
    local hply = 0

    local function put(p, sq)
        board[sq] = p
        mg = mg + MG[p][sq]; eg = eg + EG[p][sq]
        phase = phase + PHASE[p > 0 and p or -p]
        count[p] = count[p] + 1
        hash = hash ~ Z[p][sq]
        if p == KING then kingSq[1] = sq elseif p == -KING then kingSq[-1] = sq end
    end
    local function take(sq)
        local p = board[sq]
        board[sq] = 0
        mg = mg - MG[p][sq]; eg = eg - EG[p][sq]
        phase = phase - PHASE[p > 0 and p or -p]
        count[p] = count[p] - 1
        hash = hash ~ Z[p][sq]
        return p
    end

    local function attacked(sq, by)
        local s
        local pawn = PAWN * by
        if by == 1 then
            s = sq - 15; if (s & 0x88) == 0 and board[s] == pawn then return true end
            s = sq - 17; if (s & 0x88) == 0 and board[s] == pawn then return true end
        else
            s = sq + 15; if (s & 0x88) == 0 and board[s] == pawn then return true end
            s = sq + 17; if (s & 0x88) == 0 and board[s] == pawn then return true end
        end
        local n, k = KNIGHT * by, KING * by
        for i = 1, 8 do
            s = sq + KN[i]
            if (s & 0x88) == 0 and board[s] == n then return true end
            s = sq + KD[i]
            if (s & 0x88) == 0 and board[s] == k then return true end
        end
        local b, r, q = BISHOP * by, ROOK * by, QUEEN * by
        for i = 1, 4 do
            local d = DIAG[i]
            s = sq + d
            while (s & 0x88) == 0 do
                local x = board[s]
                if x ~= 0 then
                    if x == b or x == q then return true end
                    break
                end
                s = s + d
            end
            d = ORTH[i]
            s = sq + d
            while (s & 0x88) == 0 do
                local x = board[s]
                if x ~= 0 then
                    if x == r or x == q then return true end
                    break
                end
                s = s + d
            end
        end
        return false
    end

    local function makeMove(m)
        local from, to = m & 127, (m >> 7) & 127
        local promo, flag = (m >> 14) & 7, m >> 17
        hply = hply + 1
        hMove[hply] = m; hCastle[hply] = castle; hEp[hply] = ep
        hHalf[hply] = half; hHash[hply] = hash
        if ep >= 0 then hash = hash ~ ZEP[ep & 7] end
        hash = hash ~ ZC[castle]
        local us = side
        local cap = 0
        if flag == 2 then cap = take(to - 16 * us)
        elseif board[to] ~= 0 then cap = take(to) end
        hCap[hply] = cap
        local p = take(from)
        if promo ~= 0 then put(promo * us, to) else put(p, to) end
        if flag == 3 then
            if to > from then put(take(from + 3), from + 1)
            else put(take(from - 4), from - 1) end
        end
        castle = castle & CMASK[from] & CMASK[to]
        hash = hash ~ ZC[castle]
        ep = -1
        if flag == 1 then
            -- nur ein Feld, auf dem ein gegnerischer Bauer wirklich schlagen kann
            local enemy = -PAWN * us
            local l, r = to - 1, to + 1
            if ((l & 0x88) == 0 and board[l] == enemy) or ((r & 0x88) == 0 and board[r] == enemy) then
                ep = from + 16 * us
                hash = hash ~ ZEP[ep & 7]
            end
        end
        if p == PAWN * us or cap ~= 0 then half = 0 else half = half + 1 end
        side = -us
        hash = hash ~ ZSIDE
    end

    local function unmakeMove()
        local m = hMove[hply]
        local from, to = m & 127, (m >> 7) & 127
        local promo, flag = (m >> 14) & 7, m >> 17
        local us = -side
        side = us
        if flag == 3 then
            if to > from then put(take(from + 1), from + 3)
            else put(take(from - 1), from - 4) end
        end
        local p = take(to)
        if promo ~= 0 then p = PAWN * us end
        put(p, from)
        local cap = hCap[hply]
        if cap ~= 0 then
            if flag == 2 then put(cap, to - 16 * us) else put(cap, to) end
        end
        castle = hCastle[hply]; ep = hEp[hply]; half = hHalf[hply]; hash = hHash[hply]
        hply = hply - 1
    end
    -- Pseudolegale Zuege in `list` (ab Index 1); nur Schlagzuege und Umwandlungen
    -- in die Dame, wenn `capsOnly`. Liefert die Anzahl.
    local function genMoves(list, capsOnly)
        local n = 0
        local us = side
        local fwd = 16 * us
        local promoRank = us == 1 and 7 or 0
        local startRank = us == 1 and 1 or 6
        for sq = 0, 119 do
            if (sq & 0x88) == 0 then
                local pc = board[sq]
                if pc ~= 0 and (pc > 0) == (us > 0) then
                    local t = pc * us
                    if t == PAWN then
                        local to = sq + fwd
                        if board[to] == 0 then
                            if (to >> 4) == promoRank then
                                n = n + 1; list[n] = sq | (to << 7) | (QUEEN << 14)
                                if not capsOnly then
                                    n = n + 1; list[n] = sq | (to << 7) | (ROOK << 14)
                                    n = n + 1; list[n] = sq | (to << 7) | (BISHOP << 14)
                                    n = n + 1; list[n] = sq | (to << 7) | (KNIGHT << 14)
                                end
                            elseif not capsOnly then
                                n = n + 1; list[n] = sq | (to << 7)
                                if (sq >> 4) == startRank and board[to + fwd] == 0 then
                                    n = n + 1; list[n] = sq | ((to + fwd) << 7) | (1 << 17)
                                end
                            end
                        end
                        for k = -1, 1, 2 do
                            local tc = to + k
                            if (tc & 0x88) == 0 then
                                local x = board[tc]
                                if x ~= 0 and (x > 0) ~= (us > 0) then
                                    if (tc >> 4) == promoRank then
                                        n = n + 1; list[n] = sq | (tc << 7) | (QUEEN << 14)
                                        if not capsOnly then
                                            n = n + 1; list[n] = sq | (tc << 7) | (ROOK << 14)
                                            n = n + 1; list[n] = sq | (tc << 7) | (BISHOP << 14)
                                            n = n + 1; list[n] = sq | (tc << 7) | (KNIGHT << 14)
                                        end
                                    else
                                        n = n + 1; list[n] = sq | (tc << 7)
                                    end
                                elseif tc == ep then
                                    n = n + 1; list[n] = sq | (tc << 7) | (2 << 17)
                                end
                            end
                        end
                    elseif t == KNIGHT or t == KING then
                        local dirs = t == KNIGHT and KN or KD
                        for i = 1, 8 do
                            local to = sq + dirs[i]
                            if (to & 0x88) == 0 then
                                local x = board[to]
                                if x == 0 then
                                    if not capsOnly then n = n + 1; list[n] = sq | (to << 7) end
                                elseif (x > 0) ~= (us > 0) then
                                    n = n + 1; list[n] = sq | (to << 7)
                                end
                            end
                        end
                        if t == KING and not capsOnly then
                            if us == 1 then
                                if (castle & 1) ~= 0 and board[5] == 0 and board[6] == 0 and board[7] == ROOK
                                   and not attacked(4, -1) and not attacked(5, -1) and not attacked(6, -1) then
                                    n = n + 1; list[n] = 4 | (6 << 7) | (3 << 17)
                                end
                                if (castle & 2) ~= 0 and board[3] == 0 and board[2] == 0 and board[1] == 0
                                   and board[0] == ROOK
                                   and not attacked(4, -1) and not attacked(3, -1) and not attacked(2, -1) then
                                    n = n + 1; list[n] = 4 | (2 << 7) | (3 << 17)
                                end
                            else
                                if (castle & 4) ~= 0 and board[117] == 0 and board[118] == 0 and board[119] == -ROOK
                                   and not attacked(116, 1) and not attacked(117, 1) and not attacked(118, 1) then
                                    n = n + 1; list[n] = 116 | (118 << 7) | (3 << 17)
                                end
                                if (castle & 8) ~= 0 and board[115] == 0 and board[114] == 0 and board[113] == 0
                                   and board[112] == -ROOK
                                   and not attacked(116, 1) and not attacked(115, 1) and not attacked(114, 1) then
                                    n = n + 1; list[n] = 116 | (114 << 7) | (3 << 17)
                                end
                            end
                        end
                    else
                        local dirs = (t == BISHOP and DIAG) or (t == ROOK and ORTH) or KD
                        for i = 1, #dirs do
                            local d = dirs[i]
                            local to = sq + d
                            while (to & 0x88) == 0 do
                                local x = board[to]
                                if x == 0 then
                                    if not capsOnly then n = n + 1; list[n] = sq | (to << 7) end
                                else
                                    if (x > 0) ~= (us > 0) then n = n + 1; list[n] = sq | (to << 7) end
                                    break
                                end
                                to = to + d
                            end
                        end
                    end
                end
            end
        end
        return n
    end

    local function inCheck() return attacked(kingSq[side], -side) end

    local function insufficient()
        if count[PAWN] + count[-PAWN] + count[ROOK] + count[-ROOK] + count[QUEEN] + count[-QUEEN] > 0 then
            return false
        end
        return count[KNIGHT] + count[BISHOP] + count[-KNIGHT] + count[-BISHOP] <= 1
    end

    -- Wie oft die jetzige Stellung schon vorkam (gleiche Seite am Zug, seit dem
    -- letzten Schlag- oder Bauernzug).
    local function repetitions()
        local c = 0
        local j = hply - 2
        while j >= 0 and j >= hply - half do
            if hHash[j + 1] == hash then c = c + 1 end
            j = j - 2
        end
        return c
    end

    local function centerDist(sq)
        local f, r = sq & 7, sq >> 4
        return math.max(3 - f, f - 4) + math.max(3 - r, r - 4)
    end
    -- Mattfuehrung: den blanken Koenig an den Rand druecken, den eigenen heranholen.
    local function mopUp(wk, lk)
        local df = math.abs((wk & 7) - (lk & 7))
        local dr = math.abs((wk >> 4) - (lk >> 4))
        return 10 * centerDist(lk) + 4 * (14 - df - dr)
    end

    local function evaluate()
        local p = phase
        if p > 24 then p = 24 end
        local s = (mg * p + eg * (24 - p)) // 24
        if count[BISHOP] >= 2 then s = s + 30 end
        if count[-BISHOP] >= 2 then s = s - 30 end
        local mW = count[KNIGHT] * 320 + count[BISHOP] * 330 + count[ROOK] * 500 + count[QUEEN] * 900
        local mB = count[-KNIGHT] * 320 + count[-BISHOP] * 330 + count[-ROOK] * 500 + count[-QUEEN] * 900
        if count[-PAWN] == 0 and mW >= mB + 400 then s = s + mopUp(kingSq[1], kingSq[-1]) end
        if count[PAWN] == 0 and mB >= mW + 400 then s = s - mopUp(kingSq[-1], kingSq[1]) end
        return s * side
    end

    -- Suche ---------------------------------------------------------------------
    local INF, MATE = 1000000, 100000
    local EXACT, LOWER, UPPER = 0, 1, 2
    local nodes, stopped, deadline = 0, false, 0
    local tt, ttScore = {}, {}
    local killers1, killers2 = {}, {}
    local histT = {}
    local moveBuf, scoreBuf = {}, {}
    for i = 0, 70 do moveBuf[i] = {}; scoreBuf[i] = {}; killers1[i] = 0; killers2[i] = 0 end

    local function tick()
        nodes = nodes + 1
        if (nodes & 255) == 0 then
            if os.clock() > deadline then stopped = true end
            if coroutine.isyieldable() then coroutine.yield() end
        end
    end

    local function scoreMoves(list, n, scores, ply, ttMove)
        for i = 1, n do
            local m = list[i]
            local s
            if m == ttMove then
                s = 1000000
            else
                local to = (m >> 7) & 127
                local x = board[to]
                if x ~= 0 or (m >> 17) == 2 then
                    local victim = x ~= 0 and (x > 0 and x or -x) or PAWN
                    local a = board[m & 127]
                    s = 100000 + ORDERV[victim] * 16 - ORDERV[a > 0 and a or -a]
                elseif ((m >> 14) & 7) ~= 0 then
                    s = 90000
                elseif m == killers1[ply] then
                    s = 80000
                elseif m == killers2[ply] then
                    s = 70000
                else
                    s = histT[(board[m & 127] + 6) * 128 + to] or 0
                end
            end
            scores[i] = s
        end
    end

    local function pickNext(list, scores, i, n)
        local best, bi = scores[i], i
        for j = i + 1, n do
            if scores[j] > best then best = scores[j]; bi = j end
        end
        if bi ~= i then
            list[i], list[bi] = list[bi], list[i]
            scores[i], scores[bi] = scores[bi], scores[i]
        end
        return list[i]
    end

    -- Fail-soft wie die Suche: was sie liefert, ist eine echte Bewertung oder eine
    -- Schranke, die jenseits von alpha/beta liegt -- nie alpha selbst. Die Wurzel
    -- (think) unterscheidet so gescheiterte Kandidaten von knapp gleich guten.
    local function quiesce(alpha, beta, ply)
        tick()
        if stopped then return 0 end
        local stand = evaluate()
        if stand >= beta then return stand end
        if stand > alpha then alpha = stand end
        if ply >= 64 then return stand end
        local best = stand
        local list, scores = moveBuf[ply], scoreBuf[ply]
        local n = genMoves(list, true)
        scoreMoves(list, n, scores, ply, 0)
        for i = 1, n do
            local m = pickNext(list, scores, i, n)
            local x = board[(m >> 7) & 127]
            local gain = x ~= 0 and VALUE[x > 0 and x or -x] or 100
            if ((m >> 14) & 7) ~= 0 then gain = gain + 800 end
            if stand + gain + 200 >= alpha then
                makeMove(m)
                if attacked(kingSq[-side], side) then
                    unmakeMove()
                else
                    local sc = -quiesce(-beta, -alpha, ply + 1)
                    unmakeMove()
                    if stopped then return 0 end
                    if sc > best then
                        best = sc
                        if sc >= beta then return sc end
                        if sc > alpha then alpha = sc end
                    end
                end
            end
        end
        return best
    end

    local function search(depth, alpha, beta, ply)
        tick()
        if stopped then return 0 end
        if ply > 0 and (half >= 100 or insufficient() or repetitions() > 0) then return 0 end
        local check = attacked(kingSq[side], -side)
        if check and ply < 16 then depth = depth + 1 end
        if depth <= 0 then return quiesce(alpha, beta, ply) end
        local key = hash
        local info = tt[key]
        local ttMove = 0
        if info then
            ttMove = info & 0xFFFFF
            if ply > 0 and ((info >> 20) & 63) >= depth then
                local s = ttScore[key]
                if s > MATE - 500 then s = s - ply elseif s < 500 - MATE then s = s + ply end
                local f = info >> 26
                if f == EXACT or (f == LOWER and s >= beta) or (f == UPPER and s <= alpha) then
                    return s
                end
            end
        end
        local list, scores = moveBuf[ply], scoreBuf[ply]
        local n = genMoves(list, false)
        scoreMoves(list, n, scores, ply, ttMove)
        local alpha0 = alpha
        local best, bestMove, legal = -INF, 0, 0
        for i = 1, n do
            local m = pickNext(list, scores, i, n)
            makeMove(m)
            if attacked(kingSq[-side], side) then
                unmakeMove()
            else
                legal = legal + 1
                local sc = -search(depth - 1, -beta, -alpha, ply + 1)
                unmakeMove()
                if stopped then return 0 end
                if sc > best then
                    best, bestMove = sc, m
                    if sc > alpha then
                        alpha = sc
                        if sc >= beta then
                            local to = (m >> 7) & 127
                            if board[to] == 0 and (m >> 17) ~= 2 and ((m >> 14) & 7) == 0 then
                                if killers1[ply] ~= m then
                                    killers2[ply] = killers1[ply]; killers1[ply] = m
                                end
                                local hk = (board[m & 127] + 6) * 128 + to
                                histT[hk] = (histT[hk] or 0) + depth * depth
                            end
                            break
                        end
                    end
                end
            end
        end
        if legal == 0 then return check and (ply - MATE) or 0 end
        local f = (best >= beta and LOWER) or (best > alpha0 and EXACT) or UPPER
        local stored = best
        if stored > MATE - 500 then stored = stored + ply elseif stored < 500 - MATE then stored = stored - ply end
        tt[key] = bestMove | (depth << 20) | (f << 26)
        ttScore[key] = stored
        return best
    end
    -- Oeffentlich ---------------------------------------------------------------
    local STARTFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

    local function sqName(sq) return string.char(97 + (sq & 7)) .. tostring((sq >> 4) + 1) end
    local function sqFromName(s)
        local f, r = s:byte(1) - 97, s:byte(2) - 49
        if f < 0 or f > 7 or r < 0 or r > 7 then return -1 end
        return r * 16 + f
    end
    E.sqName = sqName

    function E.setFen(fen)
        for i = 0, 127 do board[i] = 0 end
        for p = -6, 6 do count[p] = 0 end
        mg, eg, phase, hash, hply = 0, 0, 0, 0, 0
        local parts = {}
        for w in fen:gmatch("%S+") do parts[#parts + 1] = w end
        local map = {p = PAWN, n = KNIGHT, b = BISHOP, r = ROOK, q = QUEEN, k = KING}
        local rank, file = 7, 0
        for ch in parts[1]:gmatch(".") do
            if ch == "/" then
                rank, file = rank - 1, 0
            elseif ch:match("%d") then
                file = file + tonumber(ch)
            else
                local t = map[ch:lower()]
                if t then put(ch == ch:upper() and t or -t, rank * 16 + file) end
                file = file + 1
            end
        end
        side = parts[2] == "b" and -1 or 1
        castle = 0
        local c = parts[3] or "-"
        if c:find("K") then castle = castle | 1 end
        if c:find("Q") then castle = castle | 2 end
        if c:find("k") then castle = castle | 4 end
        if c:find("q") then castle = castle | 8 end
        ep = -1
        if parts[4] and parts[4] ~= "-" then ep = sqFromName(parts[4]) end
        half = tonumber(parts[5] or "0") or 0
        hash = hash ~ ZC[castle]
        if ep >= 0 then hash = hash ~ ZEP[ep & 7] end
        if side == -1 then hash = hash ~ ZSIDE end
    end

    function E.reset() E.setFen(STARTFEN) end
    function E.pieceAt(sq) return board[sq] end
    function E.sideToMove() return side end
    function E.kingSquare(c) return kingSq[c] end
    E.inCheck = inCheck
    function E.make(m) makeMove(m) end
    function E.unmake() unmakeMove() end
    function E.from(m) return m & 127 end
    function E.to(m) return (m >> 7) & 127 end
    function E.promo(m) return (m >> 14) & 7 end
    function E.flag(m) return m >> 17 end
    function E.nodes() return nodes end
    function E.evaluate() return evaluate() end

    function E.legalMoves()
        local list = {}
        local n = genMoves(list, false)
        local out = {}
        for i = 1, n do
            local m = list[i]
            makeMove(m)
            if not attacked(kingSq[-side], side) then out[#out + 1] = m end
            unmakeMove()
        end
        return out
    end

    -- "play" (und die legalen Zuege), "mate", "stalemate", "fifty",
    -- "repetition" oder "material".
    function E.status()
        local legal = E.legalMoves()
        if #legal == 0 then return inCheck() and "mate" or "stalemate" end
        if half >= 100 then return "fifty" end
        if repetitions() >= 2 then return "repetition" end
        if insufficient() then return "material" end
        return "play", legal
    end

    function E.perft(d)
        if d == 0 then return 1 end
        local list = {}
        local n = genMoves(list, false)
        local total = 0
        for i = 1, n do
            makeMove(list[i])
            if not attacked(kingSq[-side], side) then total = total + E.perft(d - 1) end
            unmakeMove()
        end
        return total
    end

    local PROMO_CH = {[KNIGHT] = "n", [BISHOP] = "b", [ROOK] = "r", [QUEEN] = "q"}
    function E.uci(m)
        local p = (m >> 14) & 7
        return sqName(m & 127) .. sqName((m >> 7) & 127) .. (p ~= 0 and PROMO_CH[p] or "")
    end
    function E.parseUci(s)
        for _, m in ipairs(E.legalMoves()) do
            if E.uci(m) == s then return m end
        end
        return nil
    end

    -- Kurze algebraische Notation mit deutschen Figuren (S L T D K), vor dem Zug.
    local LETTER = {"", "S", "L", "T", "D", "K"}
    function E.san(m, legal)
        local from, to = m & 127, (m >> 7) & 127
        local promo, flag = (m >> 14) & 7, m >> 17
        local p = board[from]
        local t = p > 0 and p or -p
        local s
        if flag == 3 then
            s = to > from and "0-0" or "0-0-0"
        else
            local capture = board[to] ~= 0 or flag == 2
            if t == PAWN then
                s = (capture and (string.char(97 + (from & 7)) .. "x") or "") .. sqName(to)
                if promo ~= 0 then s = s .. "=" .. LETTER[promo] end
            else
                local other, sameFile, sameRank = false, false, false
                for _, o in ipairs(legal or E.legalMoves()) do
                    local of = o & 127
                    if o ~= m and ((o >> 7) & 127) == to and of ~= from and board[of] == p then
                        other = true
                        if (of & 7) == (from & 7) then sameFile = true end
                        if (of >> 4) == (from >> 4) then sameRank = true end
                    end
                end
                local dis = ""
                if other then
                    if not sameFile then dis = string.char(97 + (from & 7))
                    elseif not sameRank then dis = tostring((from >> 4) + 1)
                    else dis = sqName(from) end
                end
                s = LETTER[t] .. dis .. (capture and "x" or "") .. sqName(to)
            end
        end
        makeMove(m)
        if inCheck() then s = s .. (#E.legalMoves() == 0 and "#" or "+") end
        unmakeMove()
        return s
    end

    -- Den Zug fuer die Seite am Zug waehlen. o.depth: Suchtiefe; o.margin: wie weit
    -- unter dem besten Zug die Engine ueberhaupt noch waehlt; o.noise: Rauschen auf
    -- die Bewertung; o.slip: Anteil der Zuege, in denen sie einen der schwaecheren
    -- Kandidaten nimmt; o.maxTime: Sekunden. Liefert Zug und Bewertung (Zentibauern
    -- aus Sicht der Seite am Zug). In einer Coroutine gibt sie alle 256 Knoten ab.
    function E.think(o)
        local depthMax = o.depth or 3
        local noise = o.noise or 0
        local margin = math.max(o.margin or 0, 2 * noise)
        local slip = o.slip or 0
        local rand = o.random or math.random
        nodes, stopped = 0, false
        deadline = os.clock() + (o.maxTime or 5)
        tt, ttScore, histT = {}, {}, {}
        for i = 0, 70 do killers1[i] = 0; killers2[i] = 0 end
        local order = E.legalMoves()
        if #order == 0 then return nil, 0 end
        if #order == 1 then return order[1], 0 end
        local done, doneBest
        for depth = 1, depthMax do
            local res, best = {}, -INF
            for _, m in ipairs(order) do
                -- Fenster knapp unter (bester - margin): wer darin bleibt, hat eine
                -- genaue Bewertung; wer scheitert, landet sicher darunter.
                makeMove(m)
                local sc = -search(depth - 1, -INF, margin - best + 1, 1)
                unmakeMove()
                if stopped then break end
                res[m] = sc
                if sc > best then best = sc end
            end
            if stopped then break end
            done, doneBest = res, best
            table.sort(order, function(a, b) return res[a] > res[b] end)
            if best > MATE - 500 then break end
        end
        if not done then return order[1], 0 end
        if doneBest > MATE - 500 or doneBest < 500 - MATE then return order[1], doneBest end
        local cands = {}
        for _, m in ipairs(order) do
            if done[m] >= doneBest - margin then cands[#cands + 1] = m end
        end
        if slip > 0 and #cands > 1 and rand() < slip then
            local m = cands[2 + math.floor(rand() * (#cands - 1))]
            return m, done[m]
        end
        local pick, pickS = order[1], -INF
        for _, m in ipairs(cands) do
            local s = done[m] + (rand() * 2 - 1) * noise
            if s > pickS then pick, pickS = m, s end
        end
        return pick, done[pick]
    end

    E.reset()
    return E
end

-- ===========================================================================
-- Eroeffnungsbuch: ein paar gaengige Anfaenge, damit die ersten Zuege nach
-- Schach aussehen und nicht nach Figur-Feld-Tabelle.
-- ===========================================================================
local BOOK_LINES = {
    "e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4 g8f6 e1g1 f8e7",
    "e2e4 e7e5 g1f3 b8c6 f1c4 f8c5 c2c3 g8f6 d2d3 d7d6",
    "e2e4 e7e5 g1f3 b8c6 f1c4 g8f6 d2d3 f8e7",
    "e2e4 e7e5 g1f3 b8c6 d2d4 e5d4 f3d4 g8f6 b1c3 f8b4",
    "e2e4 e7e5 b1c3 g8f6 g1f3 b8c6",
    "e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3 a7a6",
    "e2e4 c7c5 g1f3 b8c6 d2d4 c5d4 f3d4 g8f6 b1c3 e7e5",
    "e2e4 c7c5 b1c3 b8c6 g2g3 g7g6 f1g2 f8g7",
    "e2e4 e7e6 d2d4 d7d5 b1c3 g8f6 c1g5 f8e7",
    "e2e4 e7e6 d2d4 d7d5 e4e5 c7c5 c2c3 b8c6",
    "e2e4 c7c6 d2d4 d7d5 b1c3 d5e4 c3e4 c8f5 e4g3 f5g6",
    "e2e4 d7d5 e4d5 d8d5 b1c3 d5a5 d2d4 g8f6",
    "d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 c1g5 f8e7 e2e3 e8g8",
    "d2d4 d7d5 c2c4 c7c6 g1f3 g8f6 b1c3 d5c4",
    "d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 c7c5",
    "d2d4 g8f6 c2c4 e7e6 g1f3 d7d5 b1c3 f8e7",
    "d2d4 g8f6 c2c4 e7e6 b1c3 f8b4 e2e3 e8g8",
    "d2d4 g8f6 c2c4 g7g6 b1c3 f8g7 e2e4 d7d6 g1f3 e8g8",
    "c2c4 e7e5 b1c3 g8f6 g1f3 b8c6 g2g3 d7d5",
    "c2c4 g8f6 b1c3 e7e6 g1f3 d7d5",
    "g1f3 d7d5 d2d4 g8f6 c2c4 e7e6",
    "g1f3 g8f6 c2c4 g7g6 b1c3 f8g7",
}
local BOOK = {}
for i, line in ipairs(BOOK_LINES) do
    local l = {}
    for w in line:gmatch("%S+") do l[#l + 1] = w end
    BOOK[i] = l
end
-- ===========================================================================
-- Das Spiel am Brett
-- ===========================================================================
local E                    -- die Engine
local failMsg              -- das Brett liess sich nicht lesen
local clock = 0            -- Spielzeit seit Play, Sekunden

-- Brett in Weltkoordinaten: Mitte von a1, ein Linien- und ein Reihenschritt,
-- Hoehe der Spielflaeche (top) und der Figurenfuesse (base), Boden neben dem
-- Brett, Brettrand in Feldern ab der Mitte.
local G = { ax = 0, az = 0, fx = 0, fz = 0.05, rx = 0.05, rz = 0, s = 0.05,
            top = 0, base = 0, ground = 0, edge = 5, yaw = 0 }

local objs = {}            -- Figuren: {id, t, c, hx, hy, hz, sq, slot, x, y, z, active, anim}
local tmpl = {}            -- [c * 10 + t] = Vorlage zum Klonen
local mk = {}              -- Markierungen (Spawn-IDs)
local mkShown = {}         -- id -> sichtbar?
local mkFresh = 0          -- Frames, in denen die Schalter noch nachgereicht werden

local human = 1            -- die Farbe des Menschen
local phase = "setup"      -- "human", "engine", "anim", "promo", "over"
local selected             -- Feld der gewaehlten Figur
local legalCache           -- legale Zuege, solange der Mensch am Zug ist
local gameMoves, sanList = {}, {}
local lastFrom, lastTo
local resultText
local thinkCo, thinkMove, thinkStart = nil, nil, 0
local promoMoves           -- die Umwandlungszuege, die auf eine Wahl warten
local hoverSq
local mouse = { x = 0, y = 0, over = false, lx = 0, ly = 0 }
local cam = { side = 0, sideGoal = 0, orbit = 0, pitch = 48, dist = 0.8, fov = 40 }
-- Was HUD und Markierungen zeigen, festgehalten, solange die Stellung stimmt:
-- waehrend die Engine rechnet, steht ihr Brett mitten in der Suche.
local view = { checkSq = nil, balance = 0 }

local START = {8, 2, 2, 2, 1, 1}
local BACK = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK}
local SHOW_ORDER = {QUEEN, ROOK, BISHOP, KNIGHT, PAWN}
local NAMES = {
    {PAWN,   {"pawn", "bauer"}},
    {KNIGHT, {"knight", "springer", "horse"}},
    {BISHOP, {"bishop", "laeufer", "l\195\164ufer"}},
    {ROOK,   {"rook", "turm"}},
    {QUEEN,  {"queen", "dame"}},
    {KING,   {"king", "koenig", "k\195\182nig"}},
}
local PIECE_DE = {"Bauer", "Springer", "L\195\164ufer", "Turm", "Dame", "K\195\182nig"}

local function squareXZ(file, rank)
    return G.ax + G.fx * file + G.rx * rank, G.az + G.fz * file + G.rz * rank
end

local function kindOf(name)
    for _, k in ipairs(NAMES) do
        for _, w in ipairs(k[2]) do
            if name:find(w, 1, true) then return k[1] end
        end
    end
    return nil
end

local function descendants(id, out)
    for _, c in ipairs(game.children(id) or {}) do
        out[#out + 1] = c
        descendants(c, out)
    end
    return out
end

-- Das Brett aus der Szene lesen. Liefert eine Fehlermeldung oder nil.
local function readBoard(rootIds)
    local found, surface = {}, nil
    for _, root in ipairs(rootIds) do
        for _, id in ipairs(descendants(root, {})) do
            local label = game.getName(id) or ""
            local name = label:lower()
            local x, y, z = game.getPos(id)
            local hx, hy, hz = game.getScale(id)
            if x and hx and game.isActive(id) ~= false then
                local t = kindOf(name)
                local rec = { id = id, t = t, name = name, label = label,
                              x = x, y = y, z = z, hx = hx, hy = hy, hz = hz }
                if t then
                    found[#found + 1] = rec
                elseif name:find("board", 1, true) or name:find("brett", 1, true) then
                    surface = rec
                end
            end
        end
        if #found > 0 then break end
    end

    local kings = {}
    for _, p in ipairs(found) do if p.t == KING then kings[#kings + 1] = p end end
    if #kings < 2 then
        return "Auf dem Brett fehlen die K\195\182nige (Objekte mit \"King\" im Namen)."
    end
    local kw, kb
    for _, k in ipairs(kings) do
        local n = k.name
        if not kw and (n:match("w%d*$") or n:find("white", 1, true) or n:find("weiss", 1, true)) then kw = k
        elseif not kb and (n:match("b%d*$") or n:find("black", 1, true) or n:find("schwarz", 1, true)) then kb = k end
    end
    if not kw or not kb or kw == kb then kw, kb = kings[1], kings[2] end

    local rx, rz = (kb.x - kw.x) / 7, (kb.z - kw.z) / 7
    local s = math.sqrt(rx * rx + rz * rz)
    if s < 1e-4 then return "Die beiden K\195\182nige stehen auf demselben Feld." end
    local fx, fz = -rz, rx
    -- Die weisse Dame steht auf d, links vom Koenig: sonst die Linien umdrehen.
    for _, p in ipairs(found) do
        if p.t == QUEEN then
            local along = ((p.x - kw.x) * rx + (p.z - kw.z) * rz) / (s * s)
            if along < 3.5 then
                if (p.x - kw.x) * fx + (p.z - kw.z) * fz > 0 then fx, fz = -fx, -fz end
                break
            end
        end
    end
    G.s, G.rx, G.rz, G.fx, G.fz = s, rx, rz, fx, fz
    G.ax, G.az = kw.x - 4 * fx, kw.z - 4 * fz
    G.yaw = math.deg(math.atan(-rz, rx))

    -- Wo die Figuren stehen, und wie gross jede Sorte ist: die kleinste Figur
    -- einer Sorte ist die Vorlage (eine versehentlich aufgeblaehte ist nur groesser).
    local bases = {}
    for _, p in ipairs(found) do
        local dx, dz = p.x - G.ax, p.z - G.az
        p.file = math.floor((dx * fx + dz * fz) / (s * s) + 0.5)
        p.rank = math.floor((dx * rx + dz * rz) / (s * s) + 0.5)
        p.c = p.rank <= 3 and 1 or -1
        p.vol = p.hx * p.hy * p.hz
        bases[#bases + 1] = p.y - p.hy
    end
    kw.c, kb.c = 1, -1
    for _, p in ipairs(found) do
        local key = p.c * 10 + p.t
        if not tmpl[key] or p.vol < tmpl[key].vol then tmpl[key] = p end
    end
    for _, p in ipairs(found) do
        local tp = tmpl[p.c * 10 + p.t]
        if p.vol > tp.vol * 1.6 then
            p.hx, p.hy, p.hz = tp.hx, tp.hy, tp.hz
            game.setScale(p.id, tp.hx, tp.hy, tp.hz)
        end
    end
    table.sort(bases)
    if surface then
        G.top = surface.y + surface.hy
        G.ground = surface.y - surface.hy
        G.edge = math.max(surface.hx, surface.hz) / s
    else
        G.top = bases[1]
        G.ground = G.top
        G.edge = 4.6
    end
    -- Die Figuren stehen, wo der Autor sie hingestellt hat: auf der Hoehe, auf
    -- der die meisten stehen (die Bounding Box des Bretts kann einen Rand haben).
    G.base = bases[math.max(1, (#bases + 1) // 2)]
    if G.base > G.top + 0.3 * s or G.base < G.top - 0.3 * s then G.base = G.top end

    -- Die Grundstellung: jede Sorte auf ihre Felder, die naechste Figur zuerst.
    local used = {}
    for c = 1, -1, -2 do
        for t = 1, 6 do
            local want = {}
            local back, pawns = c == 1 and 0 or 7, c == 1 and 1 or 6
            for f = 0, 7 do
                if t == PAWN then want[#want + 1] = {f, pawns}
                elseif BACK[f + 1] == t then want[#want + 1] = {f, back} end
            end
            for _, w in ipairs(want) do
                local best, bd = nil, math.huge
                for _, p in ipairs(found) do
                    if not used[p] and p.c == c and p.t == t then
                        local d = (p.file - w[1]) ^ 2 + (p.rank - w[2]) ^ 2
                        if d < bd then best, bd = p, d end
                    end
                end
                local o
                if best then
                    used[best] = true
                    o = { id = best.id, t = t, c = c, hx = best.hx, hy = best.hy, hz = best.hz,
                          x = best.x, y = best.y, z = best.z }
                else
                    local tp = tmpl[c * 10 + t] or tmpl[-c * 10 + t]
                    if not tp then
                        return "Keine Vorlage f\195\188r " .. PIECE_DE[t] .. " gefunden."
                    end
                    o = { id = game.clone(tp.id, tp.label .. " (Klon)"), t = t, c = c,
                          hx = tp.hx, hy = tp.hy, hz = tp.hz, x = tp.x, y = tp.y, z = tp.z, fresh = 4 }
                end
                o.active = true
                objs[#objs + 1] = o
            end
        end
    end
    for _, p in ipairs(found) do
        if not used[p] then game.setActive(p.id, false) end
    end
    return nil
end

-- Markierungen: flache, halb durchsichtige Flaechen auf den Feldern.
local function makeMarkers()
    local function mat(name, r, g, b, a, glow)
        return game.createMaterial{ name = name, color = {r, g, b}, opacity = a,
                                    alphaMode = game.ALPHA_BLEND, roughness = 0.9,
                                    emission = {r, g, b}, emissionStrength = glow or 0.35 }
    end
    local function plane(m)
        local id = game.spawn{ type = game.PLANE, x = G.ax, y = -50, z = G.az, ry = G.yaw,
                               size = 0.02, physics = game.PHYSICS_NONE, material = m,
                               name = "Schach-Markierung" }
        mkShown[id] = true
        return id
    end
    local mLast  = mat("Schach letzter Zug", 0.95, 0.78, 0.25, 0.42)
    local mSel   = mat("Schach Auswahl", 0.25, 0.85, 0.35, 0.55)
    local mHover = mat("Schach Zeiger", 0.9, 0.95, 1.0, 0.22)
    local mDot   = mat("Schach Ziel", 0.2, 0.75, 0.3, 0.75, 0.6)
    local mCap   = mat("Schach Schlagen", 0.95, 0.35, 0.2, 0.45)
    local mCheck = mat("Schach Schach", 1.0, 0.15, 0.1, 0.55, 0.8)
    mk.last1, mk.last2 = plane(mLast), plane(mLast)
    mk.check = plane(mCheck)
    mk.sel = plane(mSel)
    mk.hover = plane(mHover)
    mk.dots, mk.caps = {}, {}
    for i = 1, 28 do mk.dots[i] = plane(mDot) end
    for i = 1, 8 do mk.caps[i] = plane(mCap) end
    -- Gespawnt wird erst am Ende des Frames: bis dahin laufen setActive/setPos ins
    -- Leere, also die Schalter ein paar Frames lang nachreichen.
    mkFresh = 3
end

local function marker(id, sq, size, lift)
    if not id then return end
    local on = sq ~= nil
    if on then
        local x, z = squareXZ(sq & 7, sq >> 4)
        local h = size * G.s * 0.5
        game.setScale(id, h, 0.001, h)
        game.setPos(id, x, G.top + lift * 0.001, z)
    end
    if mkShown[id] ~= on or mkFresh > 0 then
        mkShown[id] = on
        game.setActive(id, on)
    end
end

-- Figuren -----------------------------------------------------------------------
local function objOn(sq)
    for _, o in ipairs(objs) do
        if o.sq == sq then return o end
    end
    return nil
end

local function setShown(o, on)
    if o.active ~= on then
        o.active = on
        game.setActive(o.id, on)
    end
end

-- Eine neue Figur fuer eine Umwandlung: eine Kopie aus dem eigenen Satz.
local function newObj(c, t)
    local tp = tmpl[c * 10 + t] or tmpl[-c * 10 + t]
    if not tp then return nil end
    local id = game.clone(tp.id, tp.label .. " (Umwandlung)")
    if not id or id == 0 then return nil end
    local o = { id = id, t = t, c = c, hx = tp.hx, hy = tp.hy, hz = tp.hz,
                x = tp.x, y = tp.y, z = tp.z, active = false, fresh = 4 }
    objs[#objs + 1] = o
    return o
end

local function lostCounts(c)
    local on = {0, 0, 0, 0, 0, 0}
    for r = 0, 7 do
        for f = 0, 7 do
            local p = E.pieceAt(r * 16 + f) * c
            if p > 0 then on[p] = on[p] + 1 end
        end
    end
    local out, promos = {0, 0, 0, 0, 0, 0}, 0
    for t = 2, 5 do
        if on[t] > START[t] then promos = promos + on[t] - START[t]
        else out[t] = START[t] - on[t] end
    end
    out[PAWN] = math.max(0, 8 - on[PAWN] - promos)
    return out
end

local function slotXZ(c, slot)
    local col, row = slot // 8, slot % 8
    local gap = G.edge - 3.5 + 0.8
    local file = c == 1 and (-gap - col * 0.9) or (7 + gap + col * 0.9)
    local rank = c == 1 and row or (7 - row)
    return squareXZ(file, rank)
end

local function animateTo(o, x, y, z, animate)
    local dx, dy, dz = x - o.x, y - o.y, z - o.z
    local dist = math.sqrt(dx * dx + dy * dy + dz * dz)
    if dist < 1e-5 then return end
    if not animate then
        o.x, o.y, o.z, o.anim = x, y, z, nil
        return
    end
    local n = dist / G.s
    o.anim = { x0 = o.x, y0 = o.y, z0 = o.z, x1 = x, y1 = y, z1 = z, t = 0,
               dur = math.min(0.28 + 0.05 * n, 0.65), lift = G.s * math.min(0.5 + 0.2 * n, 2.2) }
end

-- Die Figuren nach der Stellung der Engine ausrichten: was auf seinem Feld
-- bleibt, bleibt; leere Soll-Felder holen sich eine freie Figur ihrer Sorte (oder
-- eine neue, bei einer Umwandlung); was vom Brett ist, steht neben dem Brett,
-- soweit es geschlagen wurde.
local function syncPieces(animate)
    local taken = {}
    for _, o in ipairs(objs) do
        if o.sq and E.pieceAt(o.sq) == o.c * o.t and not taken[o.sq] then
            taken[o.sq] = o
        else
            o.sq = nil
        end
    end
    for r = 0, 7 do
        for f = 0, 7 do
            local sq = r * 16 + f
            local p = E.pieceAt(sq)
            if p ~= 0 and not taken[sq] then
                local c = p > 0 and 1 or -1
                local t = p * c
                local tx, tz = squareXZ(f, r)
                local pick, pd = nil, math.huge
                for _, o in ipairs(objs) do
                    if not o.sq and o.c == c and o.t == t then
                        local d = (o.x - tx) ^ 2 + (o.z - tz) ^ 2 + (o.active and 0 or 100)
                        if d < pd then pick, pd = o, d end
                    end
                end
                pick = pick or newObj(c, t)
                if pick then
                    pick.sq = sq
                    taken[sq] = pick
                end
            end
        end
    end
    for _, o in ipairs(objs) do o.slot = nil end
    for c = 1, -1, -2 do
        local lost = lostCounts(c)
        local slot = 0
        for _, t in ipairs(SHOW_ORDER) do
            local need = lost[t]
            for _, o in ipairs(objs) do
                if need > 0 and o.c == c and o.t == t and not o.sq then
                    o.slot = slot
                    slot, need = slot + 1, need - 1
                end
            end
        end
    end
    for _, o in ipairs(objs) do
        local x, y, z
        if o.sq then
            x, z = squareXZ(o.sq & 7, o.sq >> 4)
            y = G.base + o.hy
        elseif o.slot then
            x, z = slotXZ(o.c, o.slot)
            y = G.ground + o.hy
        end
        if x then
            if not o.active then
                -- erscheint: von oben herab auf seinen Platz
                o.x, o.y, o.z = x, y + 2.5 * G.s, z
                setShown(o, true)
            end
            animateTo(o, x, y, z, animate)
        else
            o.anim = nil
            setShown(o, false)
        end
    end
end

local function stepPieces(dt)
    local busy = false
    for _, o in ipairs(objs) do
        local a = o.anim
        if a then
            a.t = a.t + dt
            local u = math.min(a.t / a.dur, 1)
            local k = u * u * (3 - 2 * u)
            o.x = a.x0 + (a.x1 - a.x0) * k
            o.z = a.z0 + (a.z1 - a.z0) * k
            o.y = a.y0 + (a.y1 - a.y0) * k + a.lift * 4 * u * (1 - u)
            if u >= 1 then
                if a.sound then game.playSound(zugKlang) end
                o.anim = nil
            else
                busy = true
            end
        end
        -- Ein Klon entsteht erst am Ende des Frames: seinen Schalter ein paar
        -- Frames lang nachreichen (die Vorlage kann ausgeschaltet gewesen sein).
        if o.fresh then
            game.setActive(o.id, o.active)
            o.fresh = o.fresh > 1 and o.fresh - 1 or nil
        end
        if o.active then game.setPos(o.id, o.x, o.y, o.z) end
    end
    return busy
end
-- Zeigen und Klicken -------------------------------------------------------------
-- Die Figur, die der Strahl zuerst trifft. Jede Figur ist dafuer eine Saeule,
-- die sich nach oben verjuengt (Fuss = Bounding Box, Spitze gut halb so breit):
-- die Boxen selbst sind breiter als die Figuren, und mit ihnen nahm der hohe
-- Koenig auf e1 den Klick fuer den Bauern dahinter. Abgetastet wird in
-- Millimeterschritten von der hoechsten Figur bis aufs Brett.
local function pieceOnRay(ox, oy, oz, dx, dy, dz)
    if dy > -1e-4 then return nil end
    local top = G.base
    for _, o in ipairs(objs) do
        if o.sq and o.active and o.y + o.hy > top then top = o.y + o.hy end
    end
    local t0 = math.max(0, (top - oy) / dy)
    local t1 = (G.base - oy) / dy
    local step = 0.001
    local t = t0
    while t <= t1 do
        local x, y, z = ox + dx * t, oy + dy * t, oz + dz * t
        for _, o in ipairs(objs) do
            if o.sq and o.active then
                local lo, hi = o.y - o.hy, o.y + o.hy
                if y >= lo and y <= hi then
                    local r = math.min(o.hx, o.hz) * (1 - 0.45 * (y - lo) / (hi - lo))
                    local px, pz = x - o.x, z - o.z
                    if px * px + pz * pz <= r * r then return o.sq end
                end
            end
        end
        t = t + step
    end
    return nil
end

-- Das Feld auf einem Strahl: eine getroffene Figur zuerst (ihr Kopf ragt ueber
-- das Feld dahinter), sonst der Punkt auf der Spielflaeche.
local function squareOnRay(ox, oy, oz, dx, dy, dz)
    local hit = pieceOnRay(ox, oy, oz, dx, dy, dz)
    if hit then return hit end
    if math.abs(dy) < 1e-6 then return nil end
    local t = (G.top - oy) / dy
    if t <= 0 then return nil end
    local hx, hz = ox + dx * t - G.ax, oz + dz * t - G.az
    local s2 = G.s * G.s
    local f = math.floor((hx * G.fx + hz * G.fz) / s2 + 0.5)
    local r = math.floor((hx * G.rx + hz * G.rz) / s2 + 0.5)
    if f < 0 or f > 7 or r < 0 or r > 7 then return nil end
    return r * 16 + f
end

local function squareUnderMouse()
    if not mouse.over then return nil end
    local ox, oy, oz, dx, dy, dz = game.mouseRay()
    if not ox then return nil end
    return squareOnRay(ox, oy, oz, dx, dy, dz)
end

-- Spielablauf ------------------------------------------------------------------
local function materialBalance()
    local s = 0
    local V = {1, 3, 3, 5, 9, 0}
    for r = 0, 7 do
        for f = 0, 7 do
            local p = E.pieceAt(r * 16 + f)
            if p > 0 then s = s + V[p] elseif p < 0 then s = s - V[-p] end
        end
    end
    return s * human
end

local function snapshot()
    view.checkSq = E.inCheck() and E.kingSquare(E.sideToMove()) or nil
    view.balance = materialBalance()
end

local function replay(n)
    E.reset()
    for i = 1, n do E.make(gameMoves[i]) end
    for i = #gameMoves, n + 1, -1 do
        gameMoves[i] = nil
        sanList[i] = nil
    end
    local lm = gameMoves[n]
    if lm then lastFrom, lastTo = E.from(lm), E.to(lm) else lastFrom, lastTo = nil, nil end
end

local function stopThinking()
    if thinkCo then
        thinkCo = nil
        replay(#gameMoves)   -- die Suche liess ihre Zuege auf dem Brett stehen
    end
    thinkMove = nil
end

local function playMove(m)
    local from, to, flag = E.from(m), E.to(m), E.flag(m)
    local san = E.san(m, legalCache)
    local us = E.sideToMove()
    local victim = objOn(flag == 2 and (to - 16 * us) or to)
    if victim then victim.sq = nil end
    local mover = objOn(from)
    if mover then mover.sq = to end
    if flag == 3 then
        local rf, rt = from - 4, from - 1
        if to > from then rf, rt = from + 3, from + 1 end
        local rook = objOn(rf)
        if rook then rook.sq = rt end
    end
    E.make(m)
    gameMoves[#gameMoves + 1] = m
    sanList[#sanList + 1] = san
    lastFrom, lastTo = from, to
    selected, legalCache, promoMoves = nil, nil, nil
    syncPieces(true)
    -- Der Klang gehoert zum Aufsetzen, nicht zum Klick: er haengt an der
    -- Bewegung der Figur, die jetzt auf dem Zielfeld steht (bei einer
    -- Umwandlung die neue).
    local landed = objOn(to)
    if landed and landed.anim and zugKlang ~= "" then
        landed.anim.sound = true
    end
    snapshot()
    phase = "anim"
end

local function bookMove()
    local played = {}
    for i, m in ipairs(gameMoves) do played[i] = E.uci(m) end
    local options = {}
    for _, line in ipairs(BOOK) do
        local ok = #line > #played
        for i = 1, #played do
            if not ok or line[i] ~= played[i] then ok = false; break end
        end
        if ok then options[#options + 1] = line[#played + 1] end
    end
    while #options > 0 do
        local i = math.random(#options)
        local m = E.parseUci(options[i])
        if m then return m end
        table.remove(options, i)
    end
    return nil
end

local function startEngine()
    phase = "engine"
    thinkStart = clock
    thinkMove = bookMove()
    if thinkMove then return end
    local opts = { depth = math.max(1, math.floor(suchtiefe)), noise = zufall, slip = patzer,
                   margin = 80, maxTime = maxDenkzeit }
    thinkCo = coroutine.create(function() return E.think(opts) end)
end

local function stepEngine()
    if thinkCo then
        local t0 = os.clock()
        repeat
            local ok, res = coroutine.resume(thinkCo)
            if not ok then
                game.log("chess.lua: Engine-Fehler: " .. tostring(res))
                stopThinking()
                local legal = E.legalMoves()
                thinkMove = legal[math.random(#legal)]
            elseif coroutine.status(thinkCo) == "dead" then
                thinkCo, thinkMove = nil, res
            end
        until not thinkCo or os.clock() - t0 > 0.010
    end
    if not thinkCo and thinkMove and clock - thinkStart >= denkzeit then
        local m = thinkMove
        thinkMove = nil
        playMove(m)
    end
end

local RESULT = {
    stalemate  = "Patt \226\128\147 Remis.",
    fifty      = "Remis \226\128\147 50 Z\195\188ge ohne Schlagen oder Bauernzug.",
    repetition = "Remis \226\128\147 dreimal dieselbe Stellung.",
    material   = "Remis \226\128\147 zu wenig Material zum Mattsetzen.",
}

local function nextTurn()
    local st, legal = E.status()
    if st ~= "play" then
        phase = "over"
        if st == "mate" then
            resultText = (-E.sideToMove() == human) and "Schachmatt \226\128\147 du gewinnst!"
                                                    or "Schachmatt \226\128\147 die Engine gewinnt."
        else
            resultText = RESULT[st]
        end
        return
    end
    if E.sideToMove() == human then
        phase = "human"
        legalCache = legal
    else
        startEngine()
    end
end

local function newGame()
    stopThinking()
    replay(0)
    selected, promoMoves, resultText, legalCache = nil, nil, nil, nil
    syncPieces(true)
    snapshot()
    phase = "anim"
end

local function takeBack()
    if #gameMoves == 0 then return end
    stopThinking()
    local n = #gameMoves - 1
    -- zurueck bis der Mensch am Zug ist: die Antwort der Engine geht mit
    local toMove = (n % 2 == 0) and 1 or -1
    if n > 0 and toMove ~= human then n = n - 1 end
    replay(n)
    selected, promoMoves, resultText, legalCache = nil, nil, nil, nil
    syncPieces(true)
    snapshot()
    phase = "anim"
end

local function swapSides()
    human = -human
    cam.sideGoal = human == 1 and 0 or 180
    if phase == "over" then return end
    stopThinking()
    snapshot()
    selected, promoMoves, legalCache = nil, nil, nil
    phase = "anim"
end

local function choosePromo(t)
    if not promoMoves then return end
    for _, m in ipairs(promoMoves) do
        if E.promo(m) == t then playMove(m); return end
    end
end

local function clickBoard(sq)
    if phase ~= "human" or not sq then
        selected = nil
        return
    end
    if selected then
        local choices = {}
        for _, m in ipairs(legalCache) do
            if E.from(m) == selected and E.to(m) == sq then choices[#choices + 1] = m end
        end
        if #choices == 1 then
            playMove(choices[1])
            return
        elseif #choices > 1 then
            promoMoves = choices
            selected = nil
            phase = "promo"
            return
        end
    end
    local p = E.pieceAt(sq)
    if p ~= 0 and (p > 0) == (human > 0) and sq ~= selected then
        selected = sq
    else
        selected = nil
    end
end

-- Kamera ---------------------------------------------------------------------------
local function updateCamera(dt)
    local k = math.min(1, dt * 7)
    cam.side = cam.side + (cam.sideGoal - cam.side) * k
    local a = math.atan(G.rz, G.rx) + math.rad(cam.side + cam.orbit)
    -- Der Blick zielt etwas vor die Brettmitte: so passt die nahe Kante noch
    -- ueber die Knoepfe, und hinten bleibt Luft unter der Zugliste.
    local cx, cz = squareXZ(3.5, 3.5)
    cx, cz = cx - math.cos(a) * 0.6 * G.s, cz - math.sin(a) * 0.6 * G.s
    local cy = G.top
    local p = math.rad(cam.pitch)
    local d = cam.dist
    local px = cx - math.cos(a) * math.cos(p) * d
    local pz = cz - math.sin(a) * math.cos(p) * d
    local py = cy + math.sin(p) * d
    game.setCameraPos(px, py, pz)
    game.setCameraDir(cx - px, cy - py, cz - pz)
    game.setCameraFov(cam.fov)
end

-- HUD --------------------------------------------------------------------------------
local function inside(b)
    return mouse.over and mouse.x >= b.x and mouse.x <= b.x + b.w and mouse.y >= b.y and mouse.y <= b.y + b.h
end

local function buttons()
    local list = {}
    local bw, bh, gap = 260, 72, 18
    local y = 1080 - 40 - bh
    list[#list + 1] = { x = 40, y = y, w = bw, h = bh, label = "Neues Spiel", act = newGame, on = true }
    list[#list + 1] = { x = 40 + bw + gap, y = y, w = bw, h = bh, label = "Zug zur\195\188ck",
                        act = takeBack, on = #gameMoves > 0 and phase ~= "anim" }
    list[#list + 1] = { x = 40 + 2 * (bw + gap), y = y, w = bw, h = bh, label = "Seite tauschen",
                        act = swapSides, on = phase ~= "promo" and phase ~= "anim" }
    if phase == "promo" then
        local W = game.hudSize()
        local s, g2 = 190, 22
        local x0 = (W - (4 * s + 3 * g2)) / 2
        for i, t in ipairs({QUEEN, ROOK, BISHOP, KNIGHT}) do
            list[#list + 1] = { x = x0 + (i - 1) * (s + g2), y = 430, w = s, h = s,
                                label = PIECE_DE[t], act = function() choosePromo(t) end, on = true, big = true }
        end
    end
    return list
end

local function drawButton(b)
    local hot = b.on and inside(b)
    local a = b.on and 1 or 0.45
    if hot then
        game.hudRect(b.x, b.y, b.w, b.h, 0.93, 0.8, 0.45, 0.96, 14)
    else
        game.hudRect(b.x, b.y, b.w, b.h, 0.09, 0.09, 0.11, 0.84, 14)
    end
    game.hudFrame(b.x, b.y, b.w, b.h, 0.93, 0.8, 0.45, 0.7 * a, 2, 14)
    local size = b.big and 34 or 30
    local _, th = game.hudTextSize(b.label, size, true)
    local c = hot and 0.08 or 0.95
    game.hudText(b.x + b.w / 2, b.y + (b.h - th) / 2, b.label, size, c, c, c, a, 0.5, true)
end

local function estimatedElo()
    local base = ({650, 1000, 1350, 1650, 1850})[math.max(1, math.min(5, math.floor(suchtiefe)))]
    return math.floor((base - 900 * patzer - 1.5 * zufall) / 50 + 0.5) * 50
end

local function drawHud()
    local W = game.hudSize()
    -- Stand
    local status
    if phase == "human" then
        status = view.checkSq and "Du bist am Zug \226\128\147 Schach!" or "Du bist am Zug."
    elseif phase == "promo" then
        status = "Umwandeln in \226\128\166"
    elseif phase == "engine" then
        status = "Die Engine \195\188berlegt" .. string.rep(".", 1 + math.floor(clock * 3) % 3)
    elseif phase == "over" then
        status = resultText or ""
    else
        status = ""
    end
    local bal = view.balance
    local info = "Du spielst " .. (human == 1 and "Wei\195\159" or "Schwarz") ..
                 "  \194\183  Engine ~" .. estimatedElo() .. " Elo (gesch\195\164tzt)"
    if bal ~= 0 then info = info .. "  \194\183  Material " .. (bal > 0 and "+" or "") .. bal end
    local pw = math.max(600, game.hudTextSize(info, 24) + 56, game.hudTextSize(status, 30) + 56)
    game.hudRect(40, 40, pw, 170, 0.07, 0.07, 0.09, 0.78, 16)
    game.hudText(64, 56, "Schach", 44, 0.96, 0.86, 0.55, 1, 0, true)
    game.hudText(64, 112, status, 30, 1, 1, 1, 1, 0, false)
    game.hudText(64, 158, info, 24, 0.75, 0.75, 0.78, 1, 0, false)

    -- Zugliste
    local rows = math.ceil(#sanList / 2)
    local first = math.max(1, rows - 15)
    local lw = 330
    local lx = W - 40 - lw
    local lh = 76 + 36 * math.min(rows, 16)
    game.hudRect(lx, 40, lw, math.max(lh, 110), 0.07, 0.07, 0.09, 0.78, 16)
    game.hudText(lx + 24, 56, "Z\195\188ge", 32, 0.96, 0.86, 0.55, 1, 0, true)
    for r = first, rows do
        local y = 104 + (r - first) * 36
        game.hudText(lx + 24, y, r .. ".", 26, 0.6, 0.6, 0.65, 1, 0, false)
        game.hudText(lx + 90, y, sanList[2 * r - 1] or "", 26, 1, 1, 1, 1, 0, false)
        game.hudText(lx + 210, y, sanList[2 * r] or "", 26, 1, 1, 1, 1, 0, false)
    end
    if rows == 0 then game.hudText(lx + 24, 104, "\226\128\147", 26, 0.6, 0.6, 0.65, 1, 0, false) end

    -- Ergebnis und Umwandlung
    if phase == "over" and resultText then
        local tw = game.hudTextSize(resultText, 46, true)
        local bw = tw + 120
        game.hudRect((W - bw) / 2, 250, bw, 110, 0.07, 0.07, 0.09, 0.88, 18)
        game.hudFrame((W - bw) / 2, 250, bw, 110, 0.96, 0.86, 0.55, 0.9, 3, 18)
        game.hudText(W / 2, 278, resultText, 46, 0.96, 0.86, 0.55, 1, 0.5, true)
    end
    if phase == "promo" then
        game.hudRect(W / 2 - 520, 360, 1040, 290, 0.05, 0.05, 0.07, 0.8, 18)
        game.hudText(W / 2, 374, "Der Bauer wird zur \226\128\166", 34, 1, 1, 1, 1, 0.5, true)
    end

    for _, b in ipairs(buttons()) do drawButton(b) end
    -- Bedienhinweise: rechts unten, wenn neben den Knoepfen Platz ist, sonst darueber.
    local h1 = "Klick: Figur w\195\164hlen, Klick: Ziel  \194\183  rechte Maustaste oder Pfeile: Blick  \194\183  W/S: Zoom"
    local h2 = "N neues Spiel  \194\183  R\195\188cktaste zur\195\188ck  \194\183  T Seite tauschen"
    local hw = game.hudTextSize(h1, 22)
    if W - 40 - hw > 40 + 3 * 260 + 2 * 18 + 40 then
        game.hudText(W - 40, 1080 - 40 - 62, h1, 22, 0.85, 0.85, 0.88, 0.9, 1, false)
        game.hudText(W - 40, 1080 - 40 - 30, h2, 22, 0.85, 0.85, 0.88, 0.9, 1, false)
    else
        game.hudText(40, 1080 - 40 - 72 - 74, h1, 22, 0.85, 0.85, 0.88, 0.9, 0, false)
        game.hudText(40, 1080 - 40 - 72 - 44, h2, 22, 0.85, 0.85, 0.88, 0.9, 0, false)
    end
end

local function updateMarkers()
    marker(mk.last1, lastFrom, 0.96, 1)
    marker(mk.last2, lastTo, 0.96, 1)
    marker(mk.check, phase ~= "anim" and view.checkSq or nil, 0.96, 2)
    marker(mk.sel, selected, 0.96, 3)
    marker(mk.hover, (phase == "human" and hoverSq ~= selected) and hoverSq or nil, 0.9, 4)
    local nd, nc = 0, 0
    if selected and legalCache then
        local seen = {}
        for _, m in ipairs(legalCache) do
            local to = E.to(m)
            if E.from(m) == selected and not seen[to] then
                seen[to] = true
                if E.pieceAt(to) ~= 0 or E.flag(m) == 2 then
                    nc = nc + 1
                    marker(mk.caps[nc], to, 0.96, 5)
                else
                    nd = nd + 1
                    marker(mk.dots[nd], to, 0.3, 5)
                end
            end
        end
    end
    for i = nd + 1, #mk.dots do marker(mk.dots[i], nil) end
    for i = nc + 1, #mk.caps do marker(mk.caps[i], nil) end
    if mkFresh > 0 then mkFresh = mkFresh - 1 end
end

-- ===========================================================================
function start(e)
    game.showCursor(true)
    game.setCrosshair(false)
    E = newEngine()
    local roots = {e.id}
    for _, n in ipairs({"chessboard", "Schachbrett"}) do
        local id = game.find(n)
        if id then roots[#roots + 1] = id end
    end
    failMsg = readBoard(roots)
    if failMsg then
        game.log("chess.lua: " .. failMsg)
        return
    end
    makeMarkers()
    human = menschSpieltWeiss and 1 or -1
    cam.side = human == 1 and 0 or 180
    cam.sideGoal = cam.side
    cam.dist = 16 * G.s
    syncPieces(false)
    snapshot()
    phase = "anim"
end

function update(e, dt, t)
    game.setHud("")
    if dt > 0.1 then dt = 0.1 end
    clock = clock + dt
    if failMsg then
        local W = game.hudSize()
        game.hudRect(W / 2 - 560, 60, 1120, 120, 0.1, 0.05, 0.05, 0.85, 16)
        game.hudText(W / 2, 100, failMsg, 32, 1, 0.8, 0.7, 1, 0.5, true)
        game.rest()
        return
    end

    -- Eingabe: jeden Frame abfragen, sonst zaehlen die Flanken nicht.
    local mx, my, over = game.mousePos()
    mouse.lx, mouse.ly = mouse.x, mouse.y
    mouse.x, mouse.y, mouse.over = mx, my, over
    local click = game.mousePressed(game.MOUSE_LEFT)
    local rightDown = game.mouseDown(game.MOUSE_RIGHT)
    local kNew = game.keyPressed(game.KEY_N)
    -- Z und Y: GLFW benennt Tasten nach der US-Belegung, auf einer deutschen
    -- Tastatur liegt das Z dort, wo sie Y heisst.
    local kBack = game.keyPressed(game.KEY_BACKSPACE)
    local kZ = game.keyPressed(game.KEY_Z)
    local kY = game.keyPressed(game.KEY_Y)
    local kSwap = game.keyPressed(game.KEY_T)

    if rightDown and over then
        cam.orbit = cam.orbit + (mouse.x - mouse.lx) * 0.25
        cam.pitch = math.max(18, math.min(88, cam.pitch + (mouse.y - mouse.ly) * 0.15))
    end
    if game.keyDown(game.KEY_LEFT) then cam.orbit = cam.orbit - 70 * dt end
    if game.keyDown(game.KEY_RIGHT) then cam.orbit = cam.orbit + 70 * dt end
    if game.keyDown(game.KEY_UP) then cam.pitch = math.min(88, cam.pitch + 40 * dt) end
    if game.keyDown(game.KEY_DOWN) then cam.pitch = math.max(18, cam.pitch - 40 * dt) end
    if game.keyDown(game.KEY_W) then cam.dist = math.max(8 * G.s, cam.dist * (1 - 0.9 * dt)) end
    if game.keyDown(game.KEY_S) then cam.dist = math.min(30 * G.s, cam.dist * (1 + 0.9 * dt)) end

    if kNew then newGame()
    elseif (kBack or kZ or kY) and phase ~= "anim" then takeBack()
    elseif kSwap and phase ~= "promo" and phase ~= "anim" then swapSides() end

    hoverSq = squareUnderMouse()
    if click then
        local hit = false
        for _, b in ipairs(buttons()) do
            if inside(b) then
                hit = true
                if b.on then b.act() end
                break
            end
        end
        if not hit and phase == "promo" then
            phase, promoMoves = "human", nil
        elseif not hit then
            clickBoard(hoverSq)
        end
    end

    if phase == "engine" then stepEngine() end
    local busy = stepPieces(dt)
    if phase == "anim" and not busy then nextTurn() end

    updateCamera(dt)
    updateMarkers()
    drawHud()

    -- Steht alles still (der Mensch ist dran, nichts gleitet, keine Drehtaste
    -- gehalten), wartet die Engine auf die naechste Eingabe, statt dasselbe Bild
    -- in voller Bildrate zu zeichnen. Eine Mausbewegung weckt sie sofort; die
    -- Tasten muessen hier stehen, weil eine gehaltene Taste nur einmal meldet.
    -- Waehrend die Engine rechnet, bleibt die volle Rate: ihre 10-ms-Scheiben
    -- kaemen sonst nur zehnmal pro Sekunde dran.
    local turning = game.keyDown(game.KEY_LEFT) or game.keyDown(game.KEY_RIGHT) or
                    game.keyDown(game.KEY_UP) or game.keyDown(game.KEY_DOWN) or
                    game.keyDown(game.KEY_W) or game.keyDown(game.KEY_S)
    if (phase == "human" or phase == "promo" or phase == "over") and not busy
       and not turning and math.abs(cam.sideGoal - cam.side) < 0.01 then
        game.rest()
    end
end

-- Fuer Tests ohne Fenster: die Engine von aussen erreichbar. Der Host ignoriert
-- den Rueckgabewert eines Skripts.
return { newEngine = newEngine }