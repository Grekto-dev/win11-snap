#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
EXT_UUID="win11-snap@pedro"

echo "==> Disabling $EXT_UUID in Cinnamon..."
python3 - << 'EOF' || true
from gi.repository import Gio
UUID = "win11-snap@pedro"
gs = Gio.Settings.new("org.cinnamon")
cur = [x for x in gs.get_strv("enabled-extensions") if x != UUID]
gs.set_strv("enabled-extensions", cur)
Gio.Settings.sync()
EOF
sleep 0.4

echo "==> Removing extension link, binary, and GTK theme..."
make uninstall

BACKUP_FILE="$HOME/.config/win11-snap/cinnamon_backup.ini"
if [ -f "$BACKUP_FILE" ]; then
    echo "==> Restoring original Cinnamon keybindings, edge-tiling, and GTK theme..."
    python3 - "$BACKUP_FILE" << 'EOF' || true
import sys, configparser
from gi.repository import Gio
cp = configparser.ConfigParser()
cp.read(sys.argv[1])
if cp.has_section("backup"):
    b = cp["backup"]
    muffin = Gio.Settings.new("org.cinnamon.muffin")
    if "edge_tiling" in b:
        muffin.set_boolean("edge-tiling", b["edge_tiling"].lower() == "true")
    wm = Gio.Settings.new("org.cinnamon.desktop.keybindings.wm")
    for k in ["push-tile-left", "push-tile-right", "push-tile-up", "push-tile-down"]:
        if k in b:
            vals = [x.strip() for x in b[k].split(",") if x.strip()]
            wm.set_strv(k, vals)
    if "gtk_theme" in b and b["gtk_theme"].strip():
        iface = Gio.Settings.new("org.cinnamon.desktop.interface")
        if iface.get_string("gtk-theme") == "Mint-Y-Dark-Aqua-Win11":
            iface.set_string("gtk-theme", b["gtk_theme"].strip())
    Gio.Settings.sync()
EOF
fi

if [ "${1:-}" = "--purge" ]; then
    echo "==> Purging user configuration (~/.config/win11-snap and Cinnamon spices JSON)..."
    rm -rf "$HOME/.config/win11-snap"
    rm -rf "$HOME/.config/cinnamon/spices/$EXT_UUID"
else
    echo "==> User config preserved at ~/.config/win11-snap/config.ini (pass --purge to remove)."
fi
