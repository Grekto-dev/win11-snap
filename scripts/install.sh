#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
EXT_UUID="win11-snap@pedro"

echo "==> Building win11-snapd..."
make

echo "==> Installing $EXT_UUID extension, GTK theme, and cleaning up legacy service/shortcuts..."
make install

echo "==> Enabling $EXT_UUID in Cinnamon..."
python3 - << 'EOF'
from gi.repository import Gio
UUID = "win11-snap@pedro"
gs = Gio.Settings.new("org.cinnamon")
cur = list(gs.get_strv("enabled-extensions"))
if UUID not in cur:
    cur.append(UUID)
    gs.set_strv("enabled-extensions", cur)
    Gio.Settings.sync()
EOF

# Ask Cinnamon to load/reload the extension immediately
gdbus call --session --dest org.Cinnamon --object-path /org/Cinnamon \
    --method org.Cinnamon.ReloadXlet "$EXT_UUID" "EXTENSION" >/dev/null 2>&1 || true

sleep 0.6
echo "==> Installation complete! Configure in System Settings -> Extensions -> Win11 Snap (or run: win11-snapd --settings)"
ps -C win11-snapd -o pid,rss,cmd || true
