@echo off
REM ============================================================================
REM  Fitzel - alle Pruefungen auf einmal
REM
REM  "Ist noch alles gruen?" soll eine FRAGE sein, kein Ablauf. Die Harnesse
REM  einzeln von Hand zu starten heisst in der Praxis, dass irgendwann eins
REM  vergessen wird - und ein vergessener Harness ist genau so gut wie keiner.
REM
REM  Laeuft still, solange alles passt: eine Zeile pro Pruefung. Faellt eine
REM  durch, wird IHRE Ausgabe hier ausgegeben, damit man nicht erst nachsehen
REM  muss, wo das Log liegt.
REM
REM  Alle Harnesse loesen ihre Daten relativ zum Repo-Root auf (content\,
REM  images\, sandbox\assets\), also wird hierhin gewechselt. Aus einem anderen
REM  Verzeichnis gestartet melden sie sonst Fehler, die keine sind.
REM
REM    check-all.bat            die acht Pruefungen (ca. 4 s)
REM    check-all.bat --all      dazu audiocheck und die Bild-Werkzeuge
REM    check-all.bat --build    vorher build-release.bat
REM    check-all.bat --asan     gegen den ASan-Baum ^(build\asan, langsamer^)
REM
REM  Exit-Code 0 = alles gruen.
REM ============================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0"

set "BIN=build\release\bin"
set "OUT=build\checks"

set "WANT_ALL=0"
set "WANT_BUILD=0"
set "WANT_ASAN=0"
for %%A in (%*) do (
    if /i "%%~A"=="--all"   set "WANT_ALL=1"
    if /i "%%~A"=="--build" set "WANT_BUILD=1"
    if /i "%%~A"=="--asan"  set "WANT_ASAN=1"
)

REM  Der ASan-Baum ist ein eigener Build, kein Schalter am Release-Baum. Er
REM  faellt dort ab, wo der Fehler GEMACHT wurde, statt dort, wo der Allokator
REM  spaeter zurueckschlaegt - dafuer laeuft er zwei- bis dreimal so lang.
if "%WANT_ASAN%"=="1" (
    set "BIN=build\asan\bin"
    set "OUT=build\checks-asan"
)

if "%WANT_BUILD%"=="1" (
    call "%~dp0build-release.bat"
    if errorlevel 1 (
        echo.
        echo [Fehler] Build fehlgeschlagen - Pruefungen nicht gelaufen.
        exit /b 1
    )
    echo.
)

if not exist "%BIN%\shadercheck.exe" (
    echo [Fehler] Keine gebauten Harnesse in %BIN%.
    if "%WANT_ASAN%"=="1" (
        echo          Erst den ASan-Baum bauen - siehe build-asan.bat.
    ) else (
        echo          Erst build-release.bat laufen lassen ^(oder check-all.bat --build^).
    )
    exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"

REM  Woran gemessen wurde. Gruene Pruefungen gegen einen alten Build sind die
REM  eine Antwort, die schlimmer ist als gar keine.
for %%F in ("%BIN%\sandbox.exe") do echo Build vom %%~tF
if "%WANT_ASAN%"=="1" echo AddressSanitizer aktiv - jeder Heap-Zugriff wird geprueft.
echo.

set /a FAILED=0
set /a RAN=0

REM --- Die Pruefungen: Exit-Code hat eine Bedeutung -------------------------
call :run shadercheck
call :run citycheck
REM  housecheck legt jede Haus-Vorlage aus und baut sie: Raeume im Grundriss und
REM  erreichbar, Oeffnungen in Waenden, Treppe in der Diele, Wohnflaeche nach
REM  WoFlV, gueltige Meshes, Parameter ueberleben Speichern/Laden.
call :run housecheck
REM  towncheck legt jede Stadt-Vorlage aus und baut sie: nichts Festes auf der
REM  Fahrbahn, keine Ueberlappungen, Bruecken ueber Fluesse, Hang, Speichern/Laden.
call :run towncheck
REM  rivercheck schreibt zwar Bilder, ist aber trotzdem eine Pruefung: die
REM  Wasserlaeufe schneiden ins Hoehenfeld, und ein kriechendes Bett oder eine
REM  nicht zurueckgegebene Rinne sieht man an keinem Bild.
call :run rivercheck "%OUT%" "sandbox\assets\shaders"
REM  roadcheck misst, dass eine Szene wirklich MEHRERE Strassen haelt: jede
REM  schneidet ihren Korridor ins gemeinsame Hoehenfeld, jede ist Boden, jede
REM  steht in der Szenendatei -- und eine geloeschte Strasse wird nicht
REM  freigegeben, weil die Undo-Historie noch auf sie zeigt.
call :run roadcheck "sandbox\assets\shaders"
REM  levelcheck wuerfelt ganze Rennszenen: fuenfhundert Kurse ueber den ganzen
REM  Reglerraum, und misst an jedem, was man sonst nur durch Fahren faende --
REM  eine Kurve, die enger ist als das Fahrzeug halten kann, ein Checkpoint,
REM  den die Gegner nie zaehlen (oder nie erreichen koennen), eine gespiegelte
REM  Querneigung, ein Bauwerk auf Kontrollpunkten, die es nicht gibt, und ein
REM  Seed, der zwar durchgereicht, aber nirgends benutzt wird.
call :run levelcheck
REM  sculptcheck misst die Zusage des Zieh-Werkzeugs: eine Geste, die
REM  ueberschiesst und zurueckkommt, hinterlaesst die Hoehe, bei der sie
REM  aufgehoert hat -- und nicht die Summe aller Zuckungen.
call :run sculptcheck
REM  selcheck prueft die Invariante der Auswahl: das aktive Objekt und die
REM  Menge muessen zusammenpassen. Bricht das, kopiert Duplizieren still ein
REM  Objekt, das der Autor nie angeklickt hat -- am Bildschirm sieht man nichts.
call :run selcheck
REM  animcheck prueft die Keyframe-Spuren. Eine Spur sucht ihre Eigenschaft bei
REM  jedem Abspielen ueber Id und Name neu; greift sie daneben, schreibt sie eine
REM  plausible Zahl an eine plausible Stelle, und findet sie nichts, passiert gar
REM  nichts -- beides sieht am Bildschirm aus wie "noch nichts keyframed".
call :run animcheck
REM  graphcheck prueft die Animations-Zustandsmaschine. Ein FSM faellt aus,
REM  indem es SITZENBLEIBT: ein Trigger, der nicht geloescht wird, jagt durch
REM  jeden Pfeil; eine Exit-Zeit gegen die falsche Laenge haelt eine Tuer fuer
REM  immer zu. Beides sieht aus wie ein halb gezeichneter Graph.
call :run graphcheck
call :run autosavecheck "%OUT%\autosave"
call :run meshpaintcheck
REM  bridgecheck baut jede Bruecken-Vorlage ueber einem Tal: Flaechen richtig
REM  gewunden, Stuetzen bis in den Boden, Deck-Kollision steigt mit der Fahrbahn.
call :run bridgecheck
REM  modelcheck misst die beiden Modellier-Werkzeuge, die still danebengehen:
REM  ein Loop Cut, der die Seite vergisst, von der er misst, schneidet einen
REM  Zickzack statt einer Linie -- und ein Face-Material ist ein Array neben
REM  einem anderen, also landet es Schnitte spaeter auf der falschen Flaeche.
call :run modelcheck
REM  modifiercheck misst den Modifier-Stack: jedes Ergebnis geschlossen und nach
REM  aussen gewunden, Material und Farbgewichte mitgenommen, die Handrechnungen
REM  (Catmull-Clark-Wuerfelecke auf 5/18, hohle Box 1 - 0.8^3) exakt -- ein Stack,
REM  der Flaechen verdreht, sieht von vorn oft noch richtig aus.
call :run modifiercheck
REM  proccheck misst die prozeduralen Graphen: jeder Knotentyp geschlossen und
REM  nach aussen gewunden, Flaechenzahlen von Hand gerechnet, die Regeln des
REM  Graphen (keine Schleifen, Loeschen schliesst die Kette, Einfuegen spleisst)
REM  und die Vorlagen ganz gekocht. procpanelcheck bedient das Procedural-Fenster
REM  mit echten ImGui-Klicks: Stepper, getippter Wert, Add, Verdrahten, Undo.
call :run proccheck
call :run procpanelcheck
REM  camtexcheck: was eine Kamera sieht, auf einem Material (Monitor, Spiegel) --
REM  pro Kamera und Groesse gezeichnet, hoechstens 30-mal pro Sekunde, GL-Zustand
REM  zurueck, die Verknuepfung in der Datei.
call :run camtexcheck
REM  ikcheck: inverse Kinematik -- Glieder an vier Rig-Benennungen gefunden, Zwei-
REM  Knochen-Kette erreicht ihr Ziel bei gleichen Knochenlaengen, Fuesse auf Stufen,
REM  in Loecher und auf Haenge, Haende an einen Punkt.
call :run ikcheck
REM  decalcheck: Decals aus den Flaechen darunter geschnitten -- Grundflaeche,
REM  UVs, Abstand, Neigungsgrenze, Kante, Empfaenger aus echten Objekten, Gelaende.
call :run decalcheck
REM  swingcheck: Haengendes schaukelt -- Pendelperiode, Daempfung, Stoss, Winkel-
REM  grenze, Verdrehen bei seitlichem Treffer, der Aufhaengepunkt bleibt.
call :run swingcheck
REM  retargetcheck misst das Retargeting: die automatische Knochen-Zuordnung an
REM  Mixamo-, Unreal- und BVH-Skeletten, dann (wo die Testdateien und Blender da
REM  sind) eine echte FBX-Bewegung auf eine Daz-Figur -- jeder Knochen muss dahin
REM  zeigen, wohin der Knochen der Bewegung zeigt, und Rezept und Original muessen
REM  ein Neuschreiben ueberstehen, ohne dass sich etwas anhaeuft.
call :run retargetcheck
REM  synthcheck rendert den modularen Synth OFFLINE und misst ihn. Klang faellt
REM  aus, ohne zu scheitern: ein Filter, der 6 dB daneben liegt, eine Huellkurve
REM  in Samples statt Sekunden, ein Delay, dessen Interpolation nie laeuft --
REM  alles spielt vergnuegt weiter. Dazu die zwei Invarianten, die man erst spaet
REM  hoert: Blockgroesse aendert den Klang nicht, und nichts verlaesst den Ausgang,
REM  das nicht endlich und im Bereich ist.
call :run synthcheck
REM  musiccheck misst die Band-Analyse eines Rhythmusspiels an einem synthetischen
REM  120-BPM-Track (Bass-Flux auf jeder Kick, Hoehen auf jeder Hat) -- und mit
REM  einem Audiogeraet die Songuhr: Start, Tempo 1.00x, Pause, Vorlauf.
call :run musiccheck
REM  synthplaycheck spielt die Kette, die ein Spiel benutzt: Patch und Song als
REM  Dateien in einem Wegwerf-Projekt, eine Synth-Komponente, die sie nennt, und
REM  ein echtes Lua-Skript, das synth.noteOn / synth.playMidi ruft. Jedes Glied
REM  ist ein Name, der zu einem Namen passen muss. Braucht ein Audiogeraet.
call :run synthplaycheck
REM  luadoccheck haelt die Lua-Referenz vollstaendig: jede Funktion und
REM  Konstante in game / synth / music muss in docs\lua-scripting.md stehen
REM  (die Help / Lua API zeigt), und jeder dort gezeigte Aufruf muss noch
REM  existieren. Eine gebundene Funktion ohne Zeile findet niemand.
call :run luadoccheck
REM  importcheck misst, ob ein als viele Objekte gebautes Modell auch als
REM  viele Objekte ankommt. Der Fehler hat kein Symptom: ein Import, der die
REM  Struktur verliert, zeichnet dasselbe Bild -- er ist nur EIN Objekt statt
REM  zwanzig, und das merkt man erst beim Anklicken eines Rades.
call :run importcheck
call :run softcheck
call :run collidecheck
call :run streamcheck
call :run shotcheck
call :run pathcheck "%OUT%"
REM  gpucheck haelt den GPU-Pfadtracer gegen den CPU-Tracer: dieselbe Szene,
REM  derselbe Beschleuniger, zwei Renderer. Auf einer Maschine ohne OpenGL 4.3
REM  meldet es sich als uebersprungen und faellt NICHT durch -- dort rechnet
REM  eben die CPU, und das ist kein Fehler im Code.
call :run gpucheck "%OUT%"
REM  tracecheck faehrt die Zustandsmaschine der Pathtracing-Vorschau mit einer
REM  Uhr, die der Test selbst stellt: startet sie, wartet sie vorher, hoert sie
REM  auf, faengt sie bei Kamerabewegung neu an. Nichts davon sieht man einem
REM  Standbild an -- eine Vorschau, die nie anlaeuft, und eine, die noch rechnet,
REM  sehen in der ersten Sekunde gleich aus.
call :run tracecheck
call :run capturecheck "%OUT%"
call :run iconcheck

REM --- Werkzeuge: laut, langsam oder ohne Urteil ----------------------------
REM  audiocheck spielt 16 Sekunden hoerbar Ton ab; skycheck und fogcheck
REM  schreiben Bilder und koennen per Konstruktion nicht durchfallen ("It is
REM  not a test. It is a way of SEEING"). Nichts davon beantwortet "ist alles
REM  gruen", also stehen sie hinter --all.
if "%WANT_ALL%"=="1" (
    echo.
    call :run audiocheck
    call :run skycheck  "%OUT%"
    call :run fogcheck  "%OUT%"
    REM  mixercheck zeichnet das Mixer-Panel und schreibt ein Bild davon. Wie
    REM  skycheck kann es nicht durchfallen: ein Panel ist etwas, das man
    REM  ANSIEHT -- ob die Fader-Kappe auf dem richtigen Dezibel sitzt, sagt
    REM  keine Zahl.
    call :run mixercheck "%OUT%\mixer.png"
)

echo.
if %FAILED% GTR 0 (
    echo %FAILED% von %RAN% Pruefungen fehlgeschlagen.
    exit /b 1
)
echo Alles gruen - %RAN% Pruefungen.
if "%WANT_ALL%"=="0" echo Nicht gelaufen: audiocheck ^(16 s Ton^), skycheck, fogcheck, mixercheck ^(Bilder^) - mit --all.
exit /b 0

REM --- eine Pruefung -------------------------------------------------------
REM  %1 = Name, %2 %3 = Argumente. Die Ausgabe geht ins Log und wird nur bei
REM  einem Fehlschlag gezeigt: gruen soll leise sein.
:run
set "NAME=%~1"
set "PAD=%NAME%                "
set /a RAN+=1
<nul set /p "=  !PAD:~0,17!"
"%BIN%\%NAME%.exe" %2 %3 > "%OUT%\%NAME%.log" 2>&1
REM  NICHT `if errorlevel 1`: das heisst "groesser oder gleich 1" und ist
REM  damit FALSCH fuer negative Codes. Ein Prozess, der gar nicht erst
REM  startet, liefert 0xC0000135 (DLL nicht gefunden) = -1073741515, und
REM  der Test darauf sagt "ok". Genau einmal passiert: acht Harnesse im
REM  ASan-Baum ohne ihre Laufzeit-DLL, achtmal gruen gemeldet.
set "RC=!ERRORLEVEL!"
if not "!RC!"=="0" (
    echo FEHLGESCHLAGEN
    echo.
    REM  Ein leeres Log heisst, dass der Prozess nicht bis zur ersten
    REM  Ausgabe gekommen ist - dann ist der Code die einzige Spur.
    REM  -1073741515 = 0xC0000135, eine DLL fehlt.
    for %%S in ("%OUT%\%NAME%.log") do (
        if %%~zS EQU 0 (
            echo    keine Ausgabe - Exit-Code !RC!
        ) else (
            type "%OUT%\%NAME%.log"
        )
    )
    echo.
    set /a FAILED+=1
) else (
    echo ok
)
exit /b 0
