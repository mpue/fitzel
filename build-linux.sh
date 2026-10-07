#!/usr/bin/env bash
# =============================================================================
#  Fitzel - nativer Linux-Build (Ninja, Release)
#
#  Baut direkt mit der Toolchain des Systems, ohne Docker. Der Build-Baum
#  liegt unter build/linux, die fertigen Programme in build/linux/bin.
#  Compiler ueber CC/CXX waehlbar (z.B. CC=gcc-12 CXX=g++-12 ./build-linux.sh);
#  gebraucht wird ein Compiler mit brauchbarem C++20 (GCC >= 12, Clang >= 16).
#
#  Abhaengigkeiten (Ubuntu/Debian):
#    sudo apt install git cmake ninja-build pkg-config g++ \
#        python3 python3-jinja2 \
#        libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
#        libxkbcommon-dev libwayland-dev wayland-protocols \
#        libgl1-mesa-dev libasound2-dev
#
#  Aufruf:   ./build-linux.sh            alle Ziele
#            ./build-linux.sh sandbox    nur eines
# =============================================================================
set -euo pipefail

cd "$(dirname "$0")"
BUILD_DIR=build/linux

missing=()
for tool in git cmake ninja pkg-config python3 "${CXX:-c++}"; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
python3 -c 'import jinja2' >/dev/null 2>&1 || missing+=("python3-jinja2")
if ((${#missing[@]})); then
    echo "Fehlt: ${missing[*]} - siehe Paketliste im Kopf von $0" >&2
    exit 1
fi

cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release

# Jobs nach Speicher, nicht nach Kernen: mit 32 Jobs in 16 GB toetet der
# OOM-Killer cc1plus (StreetSign.cpp, main.cpp brauchen je mehrere GB).
# Rund 1,5 GB pro Job, hoechstens so viele wie Kerne.
JOBS=$(awk '/MemTotal/{j=int($2/1572864); print (j<1)?1:j}' /proc/meminfo)
CORES=$(nproc)
((JOBS > CORES)) && JOBS=$CORES

# -k 0: weiterbauen, damit ein Lauf alle Fehler zeigt, nicht nur den ersten.
cmake --build "$BUILD_DIR" -j "$JOBS" ${1:+--target "$1"} -- -k 0
