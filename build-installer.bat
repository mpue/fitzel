@echo off
REM ============================================================================
REM  Fitzel - Installer bauen (Inno Setup 7)
REM
REM  Packt build\release\bin und die freien Inhalte aus content\ in
REM  build\installer\Fitzel-<Version>-Setup.exe. Vorher build-release.bat
REM  laufen lassen: gepackt wird, was dort liegt.
REM
REM  Die Version kommt aus CMakeLists.txt (project VERSION), der einen Stelle,
REM  an der sie steht. Was genau hineinkommt, steht in installer\fitzel.iss.
REM ============================================================================

setlocal
cd /d "%~dp0"

set "ISCC=C:\Program Files\Inno Setup 7\ISCC.exe"
if not exist "%ISCC%" (
    echo [Fehler] ISCC.exe nicht gefunden:
    echo   %ISCC%
    exit /b 1
)

if not exist "build\release\bin\sandbox.exe" (
    echo [Fehler] build\release\bin\sandbox.exe fehlt - zuerst build-release.bat.
    exit /b 1
)
if not exist "build\release\bin\player.exe" (
    echo [Fehler] build\release\bin\player.exe fehlt - zuerst build-release.bat.
    exit /b 1
)

REM --- Ist die exe neuer als die Version? ------------------------------------
REM  Die Version steckt beim Bauen in der exe. Ist sandbox.exe aelter als
REM  CMakeLists.txt, meldet sich der Editor im neuen Installer mit der alten
REM  Nummer -- beim ersten 0.5.0-Installer wegen einer offenen Editor-Instanz
REM  (LNK1104) beinahe passiert.
powershell -NoProfile -Command "if ((Get-Item CMakeLists.txt).LastWriteTime -gt (Get-Item build\release\bin\sandbox.exe).LastWriteTime) { exit 1 }"
if errorlevel 1 (
    echo [Fehler] build\release\bin\sandbox.exe ist aelter als CMakeLists.txt -
    echo          zuerst build-release.bat ^(und dafuer den Editor schliessen^).
    exit /b 1
)

REM --- Version aus CMakeLists.txt ---------------------------------------------
REM  Die Zeile "    VERSION 0.5.0" im project()-Block; cmake_minimum_required
REM  steht mit "(VERSION" am Zeilenanfang und faellt deshalb heraus.
set "VER="
for /f "tokens=2" %%V in ('findstr /R /C:"^ *VERSION [0-9]" CMakeLists.txt') do if not defined VER set "VER=%%V"
if not defined VER (
    echo [Fehler] Keine Version in CMakeLists.txt gefunden.
    exit /b 1
)

REM --- MSVC-Laufzeit (msvcp140, vcruntime140) aus der VS18-Installation -------
REM  Der Build linkt die Laufzeit dynamisch; der Installer legt die DLLs neben
REM  die exe, damit das Programm auch ohne VC++-Redistributable startet.
set "CRT="
for /d %%D in ("C:\Program Files\Microsoft Visual Studio\18\*") do (
    for /d %%R in ("%%D\VC\Redist\MSVC\*") do (
        for /d %%C in ("%%R\x64\Microsoft.VC*.CRT") do set "CRT=%%C"
    )
)
if not defined CRT (
    echo [Fehler] MSVC-Redist-Ordner ^(x64\Microsoft.VC*.CRT^) nicht gefunden.
    exit /b 1
)

echo Baue Installer fuer Fitzel %VER%
echo   Laufzeit: %CRT%
"%ISCC%" /Qp "/DAppVersion=%VER%" "/DCrtDir=%CRT%" installer\fitzel.iss
if errorlevel 1 (
    echo [Fehler] Inno Setup ist fehlgeschlagen.
    exit /b 1
)

echo.
echo Fertig:  build\installer\Fitzel-%VER%-Setup.exe
endlocal
