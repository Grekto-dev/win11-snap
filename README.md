# Win11 Snap (`win11-snap@pedro`) — Ultralight Windows 11 Snap Layouts & Window Buttons for Cinnamon (X11)

A lightweight **Cinnamon Extension + native C11 X11 daemon (`win11-snapd`) + GTK3/GTK4 theme (`Mint-Y-Dark-Aqua-Win11`)** that brings authentic Windows 11 window snapping, Snap Layouts, Top Snap Bar, automatic Snap Assist, customizable Windows 11 keyboard shortcuts, and Windows 11 caption buttons to **Linux Mint (Cinnamon / Muffin / X11)** with near-zero resource usage.

---

## Features

1. **Cinnamon Extension (`win11-snap@pedro`):**
   - Automatically starts, reloads (`SIGUSR1`), and stops the native `win11-snapd` daemon when the extension is enabled or disabled in **System Settings → Extensions**.
   - Cleanly hands off Cinnamon's `edge-tiling` and `push-tile-*` shortcuts (backing up the user's original settings to `~/.config/win11-snap/cinnamon_backup.ini`) and restores native Cinnamon behavior immediately when disabled or uninstalled.
   - **Integrated Extension Settings UI:** click the **Configure (gear)** button on **Win11 Snap** inside Cinnamon Extensions Settings (or run `win11-snapd --settings`) to configure multitasking behavior, keyboard shortcuts with real-time system conflict detection, and language (**English** default / **Português (BR)**).

2. **Ultralight Native C11 Daemon (`win11-snapd`):**
   - Built directly on **Xlib + XInput2 (XI 2.2) + XRandR + XRender + Cairo** (no resident GTK/GLib toolkit in the daemon process).
   - **0.0% Idle CPU & < 1 MB PSS:** blocks in `poll(..., -1)` with `0` CPU ticks when idle and ~`959 kB` PSS (~`2.59 MB` PSS after full Snap Assist usage via server-side XRender thumbnail downscaling and `malloc_trim`).
   - **Maximize Button Hover & `Super + Z` Snap Layouts Popup:** hovering over any window's Maximize button (`450 ms`) or pressing `Super + Z` opens a 32-bit ARGB popup with **6 grid layouts** (`50/50`, `70/30`, `3 Columns`, `50% + 2 Quadrants`, `4 Quadrants`, `25/50/25`).
   - **Drag-to-Snap & Top Snap Bar:** dragging a window to screen edges or corners displays the frosted **Acrylic Phantom Zone Preview** (corners → quadrants; sides → halves); dragging to the top center opens the Windows 11 **Top Snap Bar** with all 6 layouts.
   - **Automatic Snap Assist:** after snapping a window (via popup, drag-and-drop, or keyboard shortcut), remaining empty zones display live window thumbnails on an Acrylic backdrop with a `#60CDFF` focus ring, full keyboard navigation (**Arrow keys + `Enter`**, `1..6`, `Esc`), mouse click selection, and per-card `×` close button.

3. **Windows 11 Titlebar Buttons (`Mint-Y-Dark-Aqua-Win11` & Custom CSD Overlay):**
   - Pixel-aligned `46×32 px` caption buttons with crisp `10×10` (`1 px` stroke) symbolic icons and `#C42B1C` Close hover for Muffin SSD, GTK3 CSD, and GTK4 windows.
   - **4-Layer Custom CSD Support** for frameless/custom-titlebar apps (Electron, VS Code, Antigravity, Chrome/Chromium, Qt):
     1. `_WIN11_MAX_BUTTON(CARDINAL[4])` X11 window property (`x, y, w, h`)
     2. Per-`WM_CLASS` profiles (`[profile:<wm_class>]` in `config.ini`)
     3. Interactive 2-click calibrator (`win11-snapd --learn-window`) & automatic `[custom_csd]` fallback
     4. Native X11 child-window Windows 11 caption button overlay (`decorate_custom_csd = true`)

---

## Default Windows 11 Keyboard Shortcuts

All shortcuts are pre-filled with Windows 11 defaults and can be customized or cleared in the extension settings (with automatic conflict blocking against Cinnamon WM, media keys, custom shortcuts, and active X11 grabs):

| Action | Default Shortcut |
| :--- | :--- |
| Open / toggle Snap Layouts popup | `Super + Z` |
| Snap window Left / move quadrant left | `Super + Left` |
| Snap window Right / move quadrant right | `Super + Right` |
| Maximize / snap to upper quadrant | `Super + Up` |
| Restore / minimize / snap to lower quadrant | `Super + Down` |
| Snap window to Top Half | `Super + Alt + Up` |
| Snap window to Bottom Half | `Super + Alt + Down` |

---

## Installation

### Dependencies (Linux Mint / Ubuntu / Debian)
```bash
sudo apt install build-essential pkg-config libx11-dev libxi-dev libxrandr-dev libxrender-dev libcairo2-dev python3-gi gir1.2-gtk-3.0
```

### Build & Install Extension
```bash
./scripts/install.sh
```
This compiles `win11-snapd`, installs the `Mint-Y-Dark-Aqua-Win11` theme to `~/.themes/`, links the extension to `~/.local/share/cinnamon/extensions/win11-snap@pedro`, and enables it in Cinnamon.

### Uninstall
```bash
./scripts/uninstall.sh          # Restores original Cinnamon shortcuts, edge-tiling, and GTK theme
./scripts/uninstall.sh --purge  # Also removes ~/.config/win11-snap and extension spices config
```

---

## Command-Line Utilities (`win11-snapd`)

- `win11-snapd --settings` — opens the integrated Cinnamon Extension Settings page
- `win11-snapd --dump-windows` — lists open windows, their decoration kind (`SSD`, `GTK_CSD`, `CUSTOM_CSD`), and computed `MAX_BTN_RECT`
- `win11-snapd --learn-window` — interactive 2-click calibrator that saves `[profile:<wm_class>]` to `~/.config/win11-snap/config.ini`
- `win11-snapd --debug` — runs in foreground logging active window detection, hitboxes, shortcuts, and snap coordinates

---

## Measured Resource Usage (Linux Mint 22.3 / Cinnamon 6.6.9 / X11)

- **Idle CPU (continuous / 3 s sample in `poll`):** `0.000%` (`0` ticks, `1` thread)
- **Idle Memory:** `PSS = 959 kB` (`Pss_Anon = 684 kB`, `Private_Dirty = 684 kB`) / `RSS = 6.41 MB`
- **Post-Snap-Assist Memory (XRender downscale + `malloc_trim`):** `PSS = 2.59 MB` (`Pss_Anon = 1.52 MB`)
