#!/usr/bin/env bash
# Builds everything and installs "Drums" for this user: a `drums` command and an app-launcher entry.
# Safe to re-run after pulling changes. Undo with ./uninstall.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$HOME/.local/bin"
APPS="$HOME/.local/share/applications"
ICONS="$HOME/.local/share/icons/hicolor/scalable/apps"

step() { printf '\n==> %s\n' "$*"; }

step "Drum kits"
if ! ls "$ROOT"/kits/*/kit.json >/dev/null 2>&1; then
  "$ROOT/kits/fetch-kits.sh"
else
  echo "already downloaded"
fi

step "Engine (C++)"
cd "$ROOT"
mise exec -- cmake -S engine -B engine/build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
mise exec -- cmake --build engine/build

step "UI (Electron)"
cd "$ROOT/ui"
[ -d node_modules ] || npm ci
npm run build

ELECTRON="$ROOT/ui/node_modules/electron/dist/electron"
[ -x "$ELECTRON" ] || { echo "Electron binary missing at $ELECTRON (try: cd ui && npm ci)" >&2; exit 1; }

step "Launcher"
mkdir -p "$BIN" "$APPS" "$ICONS"

cat >"$BIN/drums" <<EOF
#!/usr/bin/env bash
# Starts the Drums app (installed by $ROOT/install.sh). The UI starts and stops the engine itself.
PLATFORM=--ozone-platform-hint=auto
[ -n "\${WAYLAND_DISPLAY:-}" ] && PLATFORM=--ozone-platform=wayland
exec "$ELECTRON" "$ROOT/ui" "\$PLATFORM" --root "$ROOT" "\$@"
EOF
chmod +x "$BIN/drums"

install -m 644 "$ROOT/ui/resources/drums.svg" "$ICONS/drums.svg"

cat >"$APPS/drums.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Drums
GenericName=Drum practice
Comment=Play your e-drum through the PC with a metronome, notation and timing feedback
Exec=$BIN/drums
Icon=drums
Terminal=false
Categories=AudioVideo;Audio;Music;
Keywords=drums;e-drum;midi;metronome;practice;
StartupWMClass=drums-ui
EOF

command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q "$HOME/.local/share/icons/hicolor" >/dev/null 2>&1 || true

step "Done"
echo "Open \"Drums\" from the app launcher, or run: drums"
