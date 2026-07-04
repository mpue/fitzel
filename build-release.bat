@echo off
REM ============================================================================
REM  Fitzel - Release-Build (Ninja, CMAKE_BUILD_TYPE=Release)
REM
REM  cmake und cl sind auf diesem Rechner nicht in der PATH-Variable. Dieses
REM  Skript laedt die VS18-Toolchain (vcvars64) und baut den sandbox-Target
REM  in build\release. Ergebnis: build\release\bin\sandbox.exe
REM
REM  Aufruf einfach per Doppelklick oder in der Konsole:  build-release.bat
REM ============================================================================

setlocal

REM --- Werkzeuge (bundled VS18) ----------------------------------------------
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
REM  NICHT "CL" nennen: cl.exe liest die Umgebungsvariable CL und haengt sie an
REM  jede Kompiler-Zeile an. Daher VSCL.
set "VSCL=C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe"

REM --- Ins Verzeichnis dieses Skripts wechseln (= Repo-Root) ------------------
cd /d "%~dp0"

if not exist "%VCVARS%" (
    echo [Fehler] vcvars64.bat nicht gefunden:
    echo   %VCVARS%
    exit /b 1
)
if not exist "%CMAKE%" (
    echo [Fehler] cmake.exe nicht gefunden:
    echo   %CMAKE%
    exit /b 1
)

REM --- MSVC-Umgebung laden (setzt cl.exe, Linker, SDK-Pfade) ------------------
echo Lade VS18-Umgebung...
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [Fehler] vcvars64.bat konnte nicht geladen werden.
    exit /b 1
)

REM --- Konfigurieren, falls der Build-Baum noch nicht existiert ---------------
REM  (VS18-Compiler explizit erzwingen, damit nicht versehentlich eine
REM   VS2022-cl gegen die VS18-STL gemischt wird -> STL1001.)
if not exist "build\release\CMakeCache.txt" (
    echo Konfiguriere Release-Preset ^(erstmalig^)...
    "%CMAKE%" --preset release -DCMAKE_C_COMPILER="%VSCL%" -DCMAKE_CXX_COMPILER="%VSCL%"
    if %errorlevel% neq 0 (
        echo [Fehler] CMake-Konfiguration fehlgeschlagen.
        exit /b 1
    )
)

REM --- Bauen -----------------------------------------------------------------
REM  Hinweis: laeuft das Spiel/der Editor noch, ist bin\sandbox.exe gesperrt
REM  und der Linker meldet LNK1104 -- dann zuerst die laufende Instanz schliessen.
echo Baue sandbox ^(Release^)...
"%CMAKE%" --build build\release --target sandbox
if %errorlevel% neq 0 (
    echo [Fehler] Build fehlgeschlagen ^(laeuft die exe noch?^).
    exit /b 1
)

echo.
echo Fertig:  build\release\bin\sandbox.exe
endlocal
