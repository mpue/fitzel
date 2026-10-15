#!/usr/bin/env bash
# =============================================================================
#  Fitzel - Menueeintrag fuer den Editor aus dem Build-Ordner
#
#  Fuer den Editor, wie ihn build-linux.sh baut (build/linux/bin/sandbox), ohne
#  das .deb: legt fitzel.desktop und das Icon fuer den angemeldeten Benutzer an
#  (~/.local/share). Damit steht Fitzel im Anwendungsmenue, und das laufende
#  Fenster bekommt sein Icon in Dock und Alt+Tab. Unter Wayland findet der
#  Desktop das Icon nur ueber eine .desktop-Datei, deren Name zur app_id des
#  Fensters passt -- "fitzel", siehe WindowConfig::appId in main.cpp.
#
#  Ist das .deb ebenfalls installiert, gilt dieser Eintrag (gleicher Name,
#  ~/.local vor /usr): das Menue startet dann den Build aus dem Repository.
#
#  Aufruf:   ./install-desktop-entry.sh            anlegen
#            ./install-desktop-entry.sh --remove   wieder entfernen
#
#  Nach dem Verschieben des Repositorys einfach erneut aufrufen.
# =============================================================================
set -euo pipefail

cd "$(dirname "$0")"
REPO=$(pwd)
BIN=$REPO/build/linux/bin
DATA=${XDG_DATA_HOME:-$HOME/.local/share}
APPS=$DATA/applications
ICONS=$DATA/icons/hicolor
SIZES="16 24 32 48 64 128 256"   # die Groessen in images/fitzel.ico

refresh() {
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q "$APPS" || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t "$ICONS" 2>/dev/null || true
    fi
}

case ${1:-} in
    "") ;;
    --remove)
        rm -f "$APPS/fitzel.desktop"
        for s in $SIZES; do rm -f "$ICONS/${s}x${s}/apps/fitzel.png"; done
        refresh
        echo "Fitzel ist aus dem Anwendungsmenue entfernt."
        exit 0 ;;
    -h|--help) sed -n '/^#  Aufruf:/,/^#  Nach dem/p' "$0" | sed 's/^# \{0,2\}//'; exit 0 ;;
    *) echo "Unbekannte Option: $1 (--remove, --help)" >&2; exit 1 ;;
esac

[[ -x $BIN/sandbox ]] ||
    { echo "Kein Editor in $BIN -- erst ./build-linux.sh aufrufen." >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 fehlt." >&2; exit 1; }

# Jede Groesse aus fitzel.ico, wie im .deb (build-deb.sh): die Eintraege sind
# schon PNGs und werden nur herausgeschnitten.
python3 - images/fitzel.ico "$ICONS" <<'EOF'
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

# Exec nimmt den Pfad in Anfuehrungszeichen, mit \ " ` $ maskiert -- und jeden
# Backslash fuer die Schluesseldatei selbst noch einmal.
EXEC=$(printf '%s' "$BIN/sandbox" | sed -e 's/[\\"`$]/\\&/g' -e 's/\\/\\\\/g')
mkdir -p "$APPS"
cat > "$APPS/fitzel.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Fitzel
GenericName=Game Engine
GenericName[de]=Spiele-Engine
Comment=Game engine and editor for things that move through a landscape
Comment[de]=Spiele-Engine und Editor fuer Dinge, die sich durch eine Landschaft bewegen
Exec="$EXEC"
Path=$BIN
Icon=fitzel
StartupWMClass=fitzel
Terminal=false
Categories=Development;IDE;
Keywords=game;engine;editor;3d;
EOF
refresh
echo "Fitzel steht im Anwendungsmenue (startet $BIN/sandbox)."
echo "Ein schon laufender Editor bekommt das Icon nach einem Neustart."
