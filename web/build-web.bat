@echo off
REM ============================================================================
REM  Fitzel - Browser-Player (WebAssembly, Emscripten)
REM
REM  Baut nur den Player fuer den Browser: build\web\bin\player.js + player.wasm.
REM  Den Editor gibt es im Browser nicht. Der Web-Export des Editors
REM  ("File > Export for Web...") nimmt diese beiden Dateien und legt sie neben
REM  das gepackte Spiel (game.fpak) und die Seite (web\index.html).
REM
REM  Voraussetzung: emsdk unter %USERPROFILE%\emsdk (oder EMSDK setzen).
REM  cmake und ninja kommen wie bei build-release.bat aus der VS-Installation.
REM
REM  Aufruf:  web\build-web.bat          (Release)
REM           web\build-web.bat debug    (mit Assertions, ohne Optimierung)
REM ============================================================================

setlocal

if "%EMSDK%"=="" set "EMSDK=%USERPROFILE%\emsdk"
set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=C:/Program Files/Microsoft Visual Studio/18/Insiders/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"

cd /d "%~dp0.."

if not exist "%EMSDK%\emsdk_env.bat" (
    echo [Fehler] emsdk nicht gefunden: %EMSDK%
    echo   EMSDK auf das emsdk-Verzeichnis setzen.
    exit /b 1
)
if not exist "%CMAKE%" (
    echo [Fehler] cmake.exe nicht gefunden: %CMAKE%
    exit /b 1
)

REM Die emsdk-Umgebung von Hand statt per emsdk_env.bat: das erkennt eine
REM aufrufende Git-Bash und schreibt dann Bash-Exports statt cmd-Variablen.
REM emcc findet seine Konfiguration (%EMSDK%\.emscripten) selbst.
for /d %%D in ("%EMSDK%\python\*") do set "EMSDK_PYTHON=%%D\python.exe"
for /d %%D in ("%EMSDK%\node\*") do set "EMSDK_NODE=%%D\bin\node.exe"
set "PATH=%EMSDK%;%EMSDK%\upstream\emscripten;%PATH%"

REM Das Repo liegt auf exFAT ohne Besitzer-Info; git verweigert sonst die
REM FetchContent-Klone ("dubious ownership"). Nur fuer diesen Lauf.
set GIT_CONFIG_COUNT=1
set GIT_CONFIG_KEY_0=safe.directory
set GIT_CONFIG_VALUE_0=*

set "BUILD_TYPE=Release"
set "BUILD_DIR=build\web"
if /i "%1"=="debug" (
    set "BUILD_TYPE=Debug"
    set "BUILD_DIR=build\web-debug"
)

if not exist "%BUILD_DIR%\build.ninja" (
    call emcmake "%CMAKE%" -S . -B "%BUILD_DIR%" -G Ninja ^
        -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
        "-DCMAKE_MAKE_PROGRAM=%NINJA%"
    if errorlevel 1 exit /b 1
)

"%CMAKE%" --build "%BUILD_DIR%" --target player %FITZEL_WEB_BUILD_ARGS%
if errorlevel 1 exit /b 1

REM Neben jeden Editor-Build legen, den es gibt: der Web-Export sucht den
REM Player in <Editor-Ordner>\web\ (ProjectIO.cpp, exportGame).
for %%B in (release gt default) do (
    if exist "build\%%B\bin\" (
        if not exist "build\%%B\bin\web" mkdir "build\%%B\bin\web"
        copy /y "%BUILD_DIR%\bin\player.js"   "build\%%B\bin\web\" >nul
        copy /y "%BUILD_DIR%\bin\player.wasm" "build\%%B\bin\web\" >nul
        copy /y "web\index.html"     "build\%%B\bin\web\" >nul
        copy /y "web\fitzel-coi.js"  "build\%%B\bin\web\" >nul
        copy /y "web\serve.py"       "build\%%B\bin\web\" >nul
        echo Web-Player nach build\%%B\bin\web kopiert.
    )
)

echo.
echo Gebaut: %BUILD_DIR%\bin\player.js + player.wasm
endlocal
