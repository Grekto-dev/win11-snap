# Windows 11 Visual Specification (Calibrated via Windows 11 VM at 96 DPI / 1.0x)

Measurements extracted directly from native Windows 11 VM captures (`1440×900`, `effective_dpi = 96`, `scale = 1.0`).

---

## 1. Titlebar Caption Buttons

| Property | Measured Value (96 DPI) |
| :--- | :--- |
| **Button dimensions** | `46 × 32 px` (`46` width × `30–32` height; total 3-button strip ~`138 px`) |
| **Spacing (`gap` / `margin`)** | `0 px` between buttons, `0 px` at top and right edge |
| **Default order** | `:minimize,maximize,close` |
| **Top-right corner radius (Close)** | `8 px` on normal windows; `0 px` on maximized/tiled windows |
| **Symbolic icons** | `10 × 10 px` centered, solid `1 px` stroke aligned to pixel grid (`.5`) |
| **Icon color (focused)** | `#FFFFFF` (`rgb(255, 255, 255)`) |
| **Icon color (backdrop / unfocused)** | `rgba(255, 255, 255, 0.45)` |
| **Minimize / Maximize `:hover`** | `rgba(255, 255, 255, 0.08)` |
| **Minimize / Maximize `:active`** | `rgba(255, 255, 255, 0.05)` |
| **Close `:hover` (and `:backdrop:hover`)** | `#C42B1C` (`rgb(196, 43, 28)`), icon `#FFFFFF` |
| **Close `:active`** | `#B22719`, icon `rgba(255, 255, 255, 0.85)` |

---

## 2. Floating *Snap Layouts* Popup & Top Snap Bar (Dark Mode)

| Property | Measured / 1080p Calibrated Value |
| :--- | :--- |
| **Horizontal anchor (Maximize hover)** | Centered under the Maximize button (`popup_center_x = max_btn_center_x`), clamped within monitor workarea |
| **Vertical offset below titlebar** | `4–6 px` |
| **Card outer corner radius** | `8 px` |
| **Card background** | `rgba(34, 34, 36, 0.96)` |
| **Card outer border** | `1 px` `rgba(255, 255, 255, 0.14)` |
| **Card inner padding** | `14 px` |
| **Spacing between layout thumbnails** | `12 px` |
| **Spacing between inner zones** | `4 px` |
| **Zone corner radius** | `4 px` |
| **Default zone (idle)** | Fill `rgba(255, 255, 255, 0.15)`, `1 px` border `rgba(255, 255, 255, 0.35)` |
| **Hovered zone (active)** | Fill `#1F9EDE` / `#60CDFF`, border `#60CDFF` |
| **6-Layout Grid** | `50/50`, `70/30`, `3 Columns (33/33/34)`, `50% + 2 Quadrants`, `4 Quadrants (2×2)`, `25/50/25` |
