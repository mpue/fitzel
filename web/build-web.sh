#!/usr/bin/env bash
# ============================================================================
#  Fitzel - Browser-Player (WebAssembly, Emscripten), unter Linux
#
#  Wie web/build-web.bat: baut nur den Player fuer den Browser
#  (build/web/bin/player.js + player.wasm) und legt ihn mit der Seite neben
#  jeden Editor-Build, den es gibt (build/<name>/bin/web/) -- dort sucht ihn
#  der Web-Export ("File > Export for Web...", oder sandbox --export-web).
#
#  Voraussetzung: emsdk unter ~/emsdk (oder EMSDK setzen), cmake, ninja.
#
#  Aufruf:  web/build-web.sh          (Release)
#           web/build-web.sh debug    (mit Assertions, ohne Optimierung)
# ============================================================================
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
EMSDK="${EMSDK:-$HOME/emsdk}"
[[ -f "$EMSDK/emsdk_env.sh" ]] || { echo "[Fehler] emsdk nicht gefunden: $EMSDK (EMSDK setzen)" >&2; exit 1; }
# shellcheck disable=SC1091
source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1

BUILD_TYPE=Release
BUILD_DIR=build/web
if [[ "${1:-}" == "debug" ]]; then
    BUILD_TYPE=Debug
    BUILD_DIR=build/web-debug
fi

# Ein eigener Build-Ordner mit eigenen _deps: die des Desktop-Builds teilen
# hiesse dessen Objekte ueberschreiben.
if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    emcmake cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi
cmake --build "$BUILD_DIR" --target player ${FITZEL_WEB_BUILD_ARGS:-}

for b in build/*/; do
    b="${b%/}"
    [[ "$b" == "$BUILD_DIR" || ! -d "$b/bin" || ! -x "$b/bin/sandbox" ]] && continue
    mkdir -p "$b/bin/web"
    cp -f "$BUILD_DIR/bin/player.js" "$BUILD_DIR/bin/player.wasm" \
          web/index.html web/fitzel-coi.js web/serve.py "$b/bin/web/"
    echo "Web-Player nach $b/bin/web kopiert."
done

echo
echo "Gebaut: $BUILD_DIR/bin/player.js + player.wasm"
