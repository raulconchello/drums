#!/usr/bin/env bash
# Removes what install.sh added (launcher entry, icon, `drums` command). Leaves the project, kits and recordings.
set -euo pipefail
rm -f "$HOME/.local/bin/drums" \
      "$HOME/.local/share/applications/drums.desktop" \
      "$HOME/.local/share/icons/hicolor/scalable/apps/drums.svg"
command -v update-desktop-database >/dev/null && update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true
echo "Drums launcher removed."
