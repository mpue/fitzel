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
REM
REM  Exit-Code 0 = alles gruen.
REM ============================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0"

set "BIN=build\release\bin"
set "OUT=build\checks"

set "WANT_ALL=0"
set "WANT_BUILD=0"
for %%A in (%*) do (
    if /i "%%~A"=="--all"   set "WANT_ALL=1"
    if /i "%%~A"=="--build" set "WANT_BUILD=1"
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
    echo          Erst build-release.bat laufen lassen ^(oder check-all.bat --build^).
    exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"

REM  Woran gemessen wurde. Gruene Pruefungen gegen einen alten Build sind die
REM  eine Antwort, die schlimmer ist als gar keine.
for %%F in ("%BIN%\sandbox.exe") do echo Build vom %%~tF
echo.

set /a FAILED=0
set /a RAN=0

REM --- Die Pruefungen: Exit-Code hat eine Bedeutung -------------------------
call :run shadercheck
call :run citycheck
call :run autosavecheck "%OUT%\autosave"
call :run meshpaintcheck
call :run shotcheck
call :run pathcheck "%OUT%"
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
)

echo.
if %FAILED% GTR 0 (
    echo %FAILED% von %RAN% Pruefungen fehlgeschlagen.
    exit /b 1
)
echo Alles gruen - %RAN% Pruefungen.
if "%WANT_ALL%"=="0" echo Nicht gelaufen: audiocheck ^(16 s Ton^), skycheck, fogcheck ^(Bilder^) - mit --all.
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
if errorlevel 1 (
    echo FEHLGESCHLAGEN
    echo.
    type "%OUT%\%NAME%.log"
    echo.
    set /a FAILED+=1
) else (
    echo ok
)
exit /b 0
