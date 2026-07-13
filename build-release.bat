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
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
REM  Ninja explizit festnageln: ein von der VS-IDE erzeugter Cache kann sonst
REM  auf ein Ninja unter einer anderen (evtl. deinstallierten) VS-Installation
REM  zeigen -> "no such file or directory" beim Build.
set "NINJA=C:/Program Files/Microsoft Visual Studio/18/Insiders/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
REM  NICHT "CL" nennen: cl.exe liest die Umgebungsvariable CL und haengt sie an
REM  jede Kompiler-Zeile an. Daher VSCL.
set "VSCL=C:/Program Files/Microsoft Visual Studio/18/Insiders/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe"

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
if not exist "%NINJA%" (
    echo [Fehler] ninja.exe nicht gefunden:
    echo   %NINJA%
    exit /b 1
)

REM --- Git-Ownership-Schutz entschaerfen --------------------------------------
REM  Dieses Repo liegt auf einem exFAT-Laufwerk (D:), das keine Eigentuemer
REM  speichert. Git bricht dort den FetchContent-Clone (glfw, imgui, ...) mit
REM  "detected dubious ownership" ab. Wir markieren alle Verzeichnisse nur fuer
REM  die Laufzeit dieses Skripts als sicher (via Umgebung, kein Eingriff in die
REM  globale Git-Config; wird durch endlocal wieder verworfen).
set "GIT_CONFIG_COUNT=1"
set "GIT_CONFIG_KEY_0=safe.directory"
set "GIT_CONFIG_VALUE_0=*"

REM --- MSVC-Umgebung laden (setzt cl.exe, Linker, SDK-Pfade) ------------------
echo Lade VS18-Umgebung...
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [Fehler] vcvars64.bat konnte nicht geladen werden.
    exit /b 1
)

REM --- Konfigurieren, falls der Build-Baum fehlt oder veraltet ist -----------
REM  (VS18-Compiler explizit erzwingen, damit nicht versehentlich eine
REM   VS2022-cl gegen die VS18-STL gemischt wird -> STL1001.)
REM  Ein vorhandener Cache, der nicht auf VS18 Insiders zeigt (z.B. von der
REM  IDE gegen eine spaeter deinstallierte Community-Installation erzeugt),
REM  wird verworfen und frisch konfiguriert.
REM  Kriterium ist build.ninja (nur nach vollstaendig abgeschlossenem Configure
REM  vorhanden), nicht bloss die CMakeCache.txt - ein abgebrochener Configure
REM  hinterlaesst sonst einen halben Cache, der faelschlich als fertig gilt.
set "NEEDCONFIG=0"
if not exist "build\release\build.ninja" set "NEEDCONFIG=1"
if exist "build\release\CMakeCache.txt" (
    findstr /C:"18/Insiders/" "build\release\CMakeCache.txt" >nul 2>&1 || (
        echo Veralteter Build-Cache gefunden ^(zeigt nicht auf VS18 Insiders^) - wird neu erzeugt...
        rmdir /s /q "build\release"
        set "NEEDCONFIG=1"
    )
)

if "%NEEDCONFIG%"=="1" (
    echo Konfiguriere Release-Preset...
    "%CMAKE%" --preset release -DCMAKE_C_COMPILER="%VSCL%" -DCMAKE_CXX_COMPILER="%VSCL%" -DCMAKE_MAKE_PROGRAM="%NINJA%"
    if %errorlevel% neq 0 (
        echo [Fehler] CMake-Konfiguration fehlgeschlagen.
        exit /b 1
    )
)

REM --- glad-Codegenerierung braucht das Python-Modul jinja2 ------------------
REM  glad erzeugt den OpenGL-Loader zur Build-Zeit per Python. Fehlt jinja2,
REM  bricht der Build mit "ModuleNotFoundError: No module named 'jinja2'" ab.
REM  Wir pruefen genau das Python, das CMake beim Konfigurieren gefunden hat
REM  (_Python_EXECUTABLE im Cache) und installieren jinja2 nur bei Bedarf.
set "PYEXE="
for /f "tokens=2 delims==" %%P in ('findstr /B /C:"_Python_EXECUTABLE:INTERNAL=" "build\release\CMakeCache.txt" 2^>nul') do set "PYEXE=%%P"
if defined PYEXE set "PYEXE=%PYEXE:/=\%"
if defined PYEXE (
    "%PYEXE%" -c "import jinja2" >nul 2>&1 || (
        echo Python-Modul jinja2 fehlt ^(fuer glad-Codegenerierung^) - installiere...
        "%PYEXE%" -m pip install jinja2
        if errorlevel 1 (
            echo [Fehler] jinja2 konnte nicht installiert werden. Bitte manuell:
            echo   "%PYEXE%" -m pip install jinja2
            exit /b 1
        )
    )
)

REM --- Bauen -----------------------------------------------------------------
REM  Hinweis: laeuft das Spiel/der Editor noch, ist bin\sandbox.exe gesperrt
REM  und der Linker meldet LNK1104 -- dann zuerst die laufende Instanz schliessen.
echo Baue sandbox + player ^(Release^)...
"%CMAKE%" --build build\release --target sandbox player
if %errorlevel% neq 0 (
    echo [Fehler] Build fehlgeschlagen ^(laeuft die exe noch?^).
    exit /b 1
)

echo.
echo Fertig:  build\release\bin\sandbox.exe  ^(Editor^)
echo          build\release\bin\player.exe   ^(exportiertes Spiel^)
endlocal
