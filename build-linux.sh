#!/usr/bin/env bash
# =============================================================================
#  Fitzel - Linux-Build im Docker-Container (Ninja, Release)
#
#  Baut das Image aus docker/linux/Dockerfile und darin alle Ziele. Der
#  Build-Baum liegt in einem Docker-Volume (fitzel-linux-build), nicht unter
#  build/: Kompilieren ueber den Windows-Bind-Mount ist um ein Vielfaches
#  langsamer, und so bleibt auch der Abhaengigkeits-Download zwischen zwei
#  Laeufen erhalten. Die fertigen Programme landen in build/linux/bin.
#
#  Aufruf (Git Bash):   ./build-linux.sh            alle Ziele
#                       ./build-linux.sh sandbox    nur eines
# =============================================================================
set -euo pipefail

cd "$(dirname "$0")"
IMAGE=fitzel-linux-build
VOLUME=fitzel-linux-build

docker build -t "$IMAGE" docker/linux

# MSYS_NO_PATHCONV: sonst schreibt Git Bash /src und /build in Windows-Pfade um.
MSYS_NO_PATHCONV=1 docker run --rm \
    -v "$(pwd -W 2>/dev/null || pwd):/src" \
    -v "$VOLUME:/build" \
    "$IMAGE" bash -c '
        set -e
        cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release
        # Jobs nach Speicher, nicht nach Kernen: mit 32 Jobs in 16 GB toetet der
        # OOM-Killer cc1plus (StreetSign.cpp, main.cpp brauchen je mehrere GB)
        # und reisst den Container mit. Rund 1,5 GB pro Job.
        JOBS=$(awk "/MemTotal/{j=int(\$2/1572864); print (j<1)?1:j}" /proc/meminfo)
        # -k 0: weiterbauen, damit ein Lauf alle Fehler zeigt, nicht nur den ersten.
        cmake --build /build -j "$JOBS" -- -k 0 '"${1:-}"'
        mkdir -p /src/build/linux
        cp -r /build/bin /src/build/linux/
    '