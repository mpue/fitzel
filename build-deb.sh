#!/usr/bin/env bash
# =============================================================================
#  Fitzel - Debian-Pakete bauen (.deb)
#
#  Das Gegenstueck zu build-installer.bat: dieselben Dateien, dieselben
#  Inhalte, aufgeteilt wie die Komponenten dort.
#
#    fitzel                 Editor und Player              -> /opt/fitzel
#    fitzel-content-base    Standard-Texturen (ca. 280 MB) -> /opt/fitzel/content
#    fitzel-content-more    weitere Texturen, HDRI, Modelle (ca. 1 GB)
#
#  fitzel empfiehlt content-base und schlaegt content-more vor; apt holt also
#  die Standard-Texturen mit, den Rest nur auf Wunsch. Ergebnis in build/deb/.
#
#  Die Programme suchen assets/, content/ und den Player neben sich, deshalb
#  liegt alles zusammen unter /opt/fitzel. Einstellungen, Fensterlayout und
#  Wiederherstellung schreibt der Editor nach ~/.local/share/fitzel, neue
#  Projekte nach ~/Dokumente/Fitzel (sandbox/src/UserDir.hpp) -- /opt bleibt
#  unangetastet.
#
#  Aufruf:   ./build-deb.sh                 alle drei Pakete
#            ./build-deb.sh --engine-only   nur fitzel (Inhalte aendern sich selten)
#
#  Installieren:   sudo apt install ./build/deb/fitzel_*.deb ./build/deb/fitzel-content-*.deb
#  Entfernen:      sudo apt remove fitzel fitzel-content-base fitzel-content-more
#
#  Baut vorher sandbox und player mit build-linux.sh, damit das Paket zum
#  Quelltext passt; ist alles aktuell, kostet das ein paar Sekunden.
#  Paketverwalter: DEBFULLNAME/DEBEMAIL, sonst git config user.name/email.
# =============================================================================
set -euo pipefail
shopt -s nullglob

cd "$(dirname "$0")"
BIN=build/linux/bin
OUT=build/deb
STAGE=$OUT/stage
CONTENT=content
OPT=opt/fitzel

ENGINE_ONLY=0
case ${1:-} in
    "")            ;;
    --engine-only) ENGINE_ONLY=1 ;;
    -h|--help)     sed -n '/^#  Aufruf:/,/^#  Entfernen:/p' "$0" | sed 's/^# \{0,2\}//'; exit 0 ;;
    *)             echo "Unbekannte Option: $1 (--engine-only, --help)" >&2; exit 1 ;;
esac

command -v dpkg-deb >/dev/null 2>&1 || { echo "dpkg-deb fehlt (Paket dpkg)." >&2; exit 1; }
command -v objdump  >/dev/null 2>&1 || { echo "objdump fehlt (Paket binutils)." >&2; exit 1; }

# --- Bauen ---------------------------------------------------------------------------
./build-linux.sh sandbox
./build-linux.sh player

# --- Version ---------------------------------------------------------------------------
# x.y.z und die Commit-Nummer aus dem Header, der in die Programme gebaut wurde:
# 0.6.0+424. Das "+N" laesst apt einen neueren Build derselben Version als
# Aktualisierung erkennen.
VH=build/linux/generated/fitzel/Version.hpp
ver_part() { sed -n "s/.*kVersion$1 *= *\([0-9]*\);.*/\1/p" "$VH"; }
VER="$(ver_part Major).$(ver_part Minor).$(ver_part Patch)+$(ver_part Build)-1"
[[ $VER =~ ^[0-9]+\.[0-9]+\.[0-9]+\+[0-9]+-1$ ]] ||
    { echo "Keine Version in $VH gefunden." >&2; exit 1; }
ARCH=$(dpkg --print-architecture)

NAME=${DEBFULLNAME:-$(git config user.name 2>/dev/null || true)}
MAIL=${DEBEMAIL:-$(git config user.email 2>/dev/null || true)}
[[ -n $NAME && -n $MAIL ]] ||
    { echo "Kein Paketverwalter: DEBFULLNAME und DEBEMAIL setzen." >&2; exit 1; }
MAINTAINER="$NAME <$MAIL>"

# --- Hilfen -----------------------------------------------------------------------------
# put MODE SRC DEST: eine Datei ins Paket, DEST relativ zu dessen Wurzel.
put() { install -D -m "$1" -- "$2" "$ROOT/$3"; }
put_tree() {   # put_tree SRC DEST: rekursiv, Datei fuer Datei
    local f
    while IFS= read -r -d '' f; do put 644 "$f" "$2/${f#"$1"/}"; done \
        < <(find "$1" -type f -print0)
}
begin() {      # begin PAKET: frische Paketwurzel
    ROOT=$STAGE/$1
    rm -rf -- "$ROOT"
    mkdir -p -- "$ROOT/DEBIAN"
}
# finish PAKET ARCH KOMPRESSION BESCHREIBUNG [weitere control-Zeilen...]
# KOMPRESSION sind dpkg-deb-Optionen, z.B. "-Zxz" oder "-Zgzip -z1".
finish() {
    local pkg=$1 arch=$2 z=$3 desc=$4
    shift 4
    local size
    size=$(du -sk --exclude=DEBIAN -- "$ROOT" | cut -f1)
    {
        echo "Package: $pkg"
        echo "Version: $VER"
        echo "Architecture: $arch"
        echo "Maintainer: $MAINTAINER"
        echo "Installed-Size: $size"
        printf '%s\n' "$@"
        echo "Section: devel"
        echo "Priority: optional"
        echo "Homepage: https://github.com/mpue/fitzel"
        printf 'Description: %s\n' "$desc"
    } > "$ROOT/DEBIAN/control"
    # Ordner 755, Dateien 644, unabhaengig von der umask -- was ausfuehrbar
    # hingelegt wurde, bleibt es (755).
    find "$ROOT" -type d -exec chmod 755 {} +
    find "$ROOT" -type f ! -perm -u+x -exec chmod 644 {} +
    find "$ROOT" -type f -perm -u+x -exec chmod 755 {} +
    mkdir -p -- "$OUT"
    local deb=$OUT/${pkg}_${VER}_${arch}.deb
    # $z ungequotet: es sind ein oder zwei Optionen.
    # shellcheck disable=SC2086
    dpkg-deb --root-owner-group $z --build "$ROOT" "$deb" >/dev/null
    echo "  $deb  ($(du -h -- "$deb" | cut -f1))"
}

# --- Abhaengigkeiten ---------------------------------------------------------------------
# Gelinkt sind nur libc, libstdc++ und libgcc; GL, X11, Wayland und ALSA laden
# GLFW und miniaudio zur Laufzeit nach -- die stehen deshalb von Hand hier. Die
# Mindestversionen kommen aus den Symbolversionen, die die Programme brauchen.
max_symver() {  # max_symver PREFIX: hoechste PREFIX_x.y.z, die sandbox/player brauchen
    objdump -T "$BIN/sandbox" "$BIN/player" |
        grep -o "$1_[0-9][0-9.]*" | sed "s/^$1_//" | sort -uV | tail -1
}
GLIBC=$(max_symver GLIBC)
GLIBCXX=$(max_symver GLIBCXX)
# GLIBCXX_3.4.N -> GCC, das sie eingefuehrt hat (libstdc++6 traegt dessen Version).
case ${GLIBCXX##*.} in
    29) GCC=11 ;; 30) GCC=12 ;; 31) GCC=13 ;; 32) GCC=14 ;; 33) GCC=15 ;; 34) GCC=16 ;;
    *)  GCC=$(dpkg-query -W -f='${Version}' libstdc++6 2>/dev/null | cut -d- -f1) ;;
esac
DEPENDS="libc6 (>= $GLIBC), libstdc++6 (>= $GCC), libgcc-s1, libgl1, libegl1, \
libx11-6, libxcursor1, libxi6, libxinerama1, libxrandr2, libxkbcommon0, \
libwayland-client0, libwayland-cursor0, libwayland-egl1, libasound2t64 | libasound2"

echo "Baue Pakete fuer Fitzel $VER ($ARCH)"
echo "  braucht glibc >= $GLIBC, libstdc++ von GCC >= $GCC"
rm -rf -- "$STAGE"

# =========================================================================================
#  fitzel
# =========================================================================================
begin fitzel
put 755 "$BIN/sandbox"                "$OPT/sandbox"
put 755 "$BIN/player"                 "$OPT/player"
put 644 docs/third-party-licenses.md  "$OPT/third-party-licenses.md"
put 644 docs/lua-scripting.md         "$OPT/lua-scripting.md"
put 644 README.md                     "$OPT/README.md"
# Shader und Skripte aus dem Repository, nicht aus bin/: der Build kopiert nur
# HINEIN, ein im Repo geloeschtes Skript lebt dort weiter. Die Gamepad-
# Zuordnungen kommen aus dem Build -- FetchContent holt sie, im Repo sind sie nicht.
put_tree sandbox/assets "$OPT/assets"
put 644 images/splash.png                  "$OPT/assets/splash.png"
put 644 "$BIN/assets/gamecontrollerdb.txt" "$OPT/assets/gamecontrollerdb.txt"
for f in sandbox/scripts/*.lua; do put 644 "$f" "$OPT/scripts/${f##*/}"; done
# Hier und nicht in den Inhaltspaketen: zwei Pakete duerfen keine Datei teilen.
put 644 installer/CONTENT-LICENSES.txt "$OPT/content/CONTENT-LICENSES.txt"

# Ein Skript, kein Symlink: das Programm stellt sein Arbeitsverzeichnis ueber
# argv[0] auf den eigenen Ordner (Startup.cpp). Ueber einen Symlink im PATH
# hiesse argv[0] nur "fitzel", und assets/ wuerde im aktuellen Ordner gesucht.
install -d "$ROOT/usr/bin"
printf '#!/bin/sh\nexec /%s/sandbox "$@"\n' "$OPT" > "$ROOT/usr/bin/fitzel"
chmod 755 "$ROOT/usr/bin/fitzel"

install -d "$ROOT/usr/share/applications"
cat > "$ROOT/usr/share/applications/fitzel.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Fitzel
GenericName=Game Engine
GenericName[de]=Spiele-Engine
Comment=Game engine and editor for things that move through a landscape
Comment[de]=Spiele-Engine und Editor fuer Dinge, die sich durch eine Landschaft bewegen
Exec=fitzel
Icon=fitzel
Terminal=false
Categories=Development;IDE;
Keywords=game;engine;editor;3d;
EOF

# fitzel.ico enthaelt jede Groesse schon als PNG; die werden nur herausgeschnitten.
# Icon-Cache und Menue aktualisiert dpkg selbst (Trigger von hicolor und
# desktop-file-utils), dafuer braucht das Paket keine Skripte.
python3 - images/fitzel.ico "$ROOT/usr/share/icons/hicolor" <<'EOF'
import os, struct, sys
data = open(sys.argv[1], 'rb').read()
for i in range(struct.unpack_from('<H', data, 4)[0]):
    w, _, _, _, _, _, size, off = struct.unpack_from('<BBBBHHII', data, 6 + 16 * i)
    png = data[off:off + size]
    if png.startswith(b'\x89PNG'):
        d = f'{sys.argv[2]}/{w or 256}x{w or 256}/apps'
        os.makedirs(d, exist_ok=True)
        open(f'{d}/fitzel.png', 'wb').write(png)
EOF

{
    echo "Fitzel -- $(git config --get remote.origin.url 2>/dev/null || echo https://github.com/mpue/fitzel)"
    echo
    cat LICENSE
    echo
    echo "Third-party components and their licences: /$OPT/third-party-licenses.md"
    echo "Content shipped in the fitzel-content-* packages: /$OPT/content/CONTENT-LICENSES.txt"
} > "$STAGE/copyright"
put 644 "$STAGE/copyright" usr/share/doc/fitzel/copyright

finish fitzel "$ARCH" -Zxz \
    "game engine and editor for things that move through a landscape
 Fitzel is a game engine with its own editor: terrain, roads, rivers,
 vegetation, towns, weather, vehicles and Lua scripting, and a player that
 runs the finished game. The editor exports games for Linux, Windows and
 the web." \
    "Depends: $DEPENDS" \
    "Recommends: fitzel-content-base, zenity | kdialog" \
    "Suggests: fitzel-content-more, ffmpeg"

((ENGINE_ONLY)) && { rm -rf -- "$STAGE"; echo "Fertig."; exit 0; }

# =========================================================================================
#  Inhalte -- wie in installer/fitzel.iss
# =========================================================================================
# Jedes Muster nimmt die .meta des Assets mit, die die ID haelt, ueber die
# Projekte es finden. Pro Textursatz nur, was die Engine liest: Farbe und
# OpenGL-Normal-Map. Fehlt etwas, wird nicht gepackt: ein halbes Inhaltspaket
# waere schlimmer als keins.
missing=()
put_glob() {   # put_glob ZIELORDNER MUSTER...
    local dest=$1 pat f any
    shift
    for pat in "$@"; do
        any=0
        for f in $CONTENT/$pat; do put 644 "$f" "$OPT/$dest/${f##*/}"; any=1; done
        ((any)) || missing+=("$CONTENT/$pat")
    done
}
tex() { put_glob content/textures "textures/$1_diff*_4k.*" "textures/$1_nor_gl_4k.$2*"; }
content_copyright() {
    put 644 installer/CONTENT-LICENSES.txt "usr/share/doc/$1/copyright"
}
check_missing() {
    ((${#missing[@]})) || return 0
    echo "Fehlt in $CONTENT/ - $1 wird nicht gebaut (oder --engine-only):" >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 1
}
# JPG, PNG und EXR werden nicht kleiner; gzip -1 nimmt die .meta mit und haelt
# den Bau kurz.
CONTENT_Z="-Zgzip -z1"

begin fitzel-content-base
# Terrain-Schichten und die Standard-Oberflaeche der Strasse.
tex coast_sand_01    png
tex aerial_rocks_01  png
tex rocky_terrain_02 png
tex snow_02          png
tex asphalt_02       png
put_glob content/textures "textures/road_puddles.png*" "textures/road_wet_grain.png*"
check_missing fitzel-content-base
content_copyright fitzel-content-base
finish fitzel-content-base all "$CONTENT_Z" \
    "default textures for the Fitzel game engine
 The terrain layers (sand, rock, snow) and road surfaces a new Fitzel
 project starts with, and the noise behind wet roads. CC0, from
 Poly Haven, or made for Fitzel." \
    "Enhances: fitzel"

begin fitzel-content-more
tex asphalt_04             png
tex brown_mud_leaves_01    png
tex concrete_block_wall_03 png
tex cracked_concrete_02    png
tex forest_leaves_02       png
tex gray_rocks             png
tex marble_cliff_03        exr
tex marble_cliff_04        png
tex moon_flat_macro_02     png
tex rocky_trail            exr
put_glob content/textures "textures/cowboy_town_hall_4k.exr*"
put_glob content/models   "models/boulder_01_4k.glb*" "models/Camera_01_4k.glb*" \
                          "models/Bark001_2K-JPG_*"   "models/LeafSet024_1K-JPG_*"
check_missing fitzel-content-more
content_copyright fitzel-content-more
finish fitzel-content-more all "$CONTENT_Z" \
    "more textures, an HDRI and models for the Fitzel game engine
 Further ground, rock and concrete textures, an HDRI sky and two models
 (a boulder and a camera), plus bark and leaf sets for the tree
 generator. CC0, from Poly Haven and ambientCG." \
    "Enhances: fitzel"

rm -rf -- "$STAGE"
echo "Fertig. Installieren:"
echo "  sudo apt install ./$OUT/fitzel_${VER}_$ARCH.deb ./$OUT/fitzel-content-base_${VER}_all.deb"
