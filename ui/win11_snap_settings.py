#!/usr/bin/env python3
"""Win11 Snap Settings - Bilingual GTK3 settings & shortcut conflict checker."""

import ctypes
import ctypes.util
import os
import re
import signal
import subprocess
import sys

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_version("Gio", "2.0")
from gi.repository import Gdk, Gio, Gtk  # noqa: E402

CONFIG_PATH = os.path.expanduser("~/.config/win11-snap/config.ini")
BACKUP_PATH = os.path.expanduser("~/.config/win11-snap/cinnamon_backup.ini")

WIN11_DEFAULT_SHORTCUTS = {
    "snap_layouts": "<Super>z",
    "snap_left": "<Super>Left",
    "snap_right": "<Super>Right",
    "snap_up": "<Super>Up",
    "snap_down": "<Super>Down",
    "snap_top_half": "<Super><Alt>Up",
    "snap_bottom_half": "<Super><Alt>Down",
}

SHORTCUT_ORDER = [
    "snap_layouts",
    "snap_left",
    "snap_right",
    "snap_up",
    "snap_down",
    "snap_top_half",
    "snap_bottom_half",
]

TRANSLATIONS = {
    "en": {
        "win_title": "Win11 Snap Settings",
        "header_title": "Window Snapping & Layouts",
        "header_sub": "Windows 11 snap layouts, edge/corner drag, Snap Assist and shortcuts",
        "lang_label": "Language:",
        "sec_behavior": "Multitasking & Snap Behavior",
        "opt_drag_snap": "Drag windows to edges or corners to snap & show Snap Assist",
        "opt_top_bar": "Show Top Snap Bar when dragging a window to the top of the screen",
        "opt_snap_assist": "Suggest windows to fill remaining zones (Snap Assist)",
        "opt_animations": "Enable smooth acrylic zone preview and snap morph animations",
        "opt_csd_decor": "Replace custom app titlebar buttons (Electron / Chrome) with Win11 style",
        "opt_shortcuts": "Enable Windows 11 snap keyboard shortcuts",
        "lbl_hover_delay": "Maximize button hover delay (ms):",
        "lbl_gap_px": "Gap between snapped windows (px):",
        "sec_shortcuts": "Snap Keyboard Shortcuts",
        "sec_shortcuts_desc": "Pre-filled with Windows 11 defaults. Click any shortcut to change it (checked against system shortcuts).",
        "btn_restore": "Restore Windows 11 Defaults",
        "btn_apply": "Save & Apply",
        "btn_close": "Close",
        "btn_disabled": "Disabled",
        "prompt_press": "Press shortcut... (Esc cancel, Backspace clear)",
        "err_need_mod": "Use at least one modifier key (Super, Ctrl, Alt, Shift) or function key.",
        "conflict_fmt": "Conflict! Shortcut '{accel}' is already used by: {owner}",
        "saved_ok": "Settings saved and applied live to win11-snapd.",
        "restore_ok": "Restored Windows 11 default shortcuts.",
        "act_snap_layouts": "Open / toggle Snap Layouts popup",
        "act_snap_left": "Snap window Left / move quadrant left",
        "act_snap_right": "Snap window Right / move quadrant right",
        "act_snap_up": "Maximize / snap to upper quadrant",
        "act_snap_down": "Restore / minimize / snap to lower quadrant",
        "act_snap_top_half": "Snap window to Top Half",
        "act_snap_bottom_half": "Snap window to Bottom Half",
        "owner_sibling": "Win11 Snap ({name})",
        "owner_cinnamon_wm": "Cinnamon WM ({key})",
        "owner_cinnamon_gen": "Cinnamon ({key})",
        "owner_media": "System Media/Keys ({key})",
        "owner_custom": "Custom Shortcut ({name})",
        "owner_x11": "Active X11 global hotkey held by another app",
    },
    "pt_BR": {
        "win_title": "Configurações do Win11 Snap",
        "header_title": "Ajuste de Janelas e Layouts (Snap)",
        "header_sub": "Layouts de encaixe do Windows 11, arrastar para bordas/cantos, Snap Assist e atalhos",
        "lang_label": "Idioma:",
        "sec_behavior": "Multitarefa e Comportamento de Ajuste",
        "opt_drag_snap": "Ao arrastar janela para bordas ou cantos, ajustar e abrir lista de janelas",
        "opt_top_bar": "Mostrar barra superior de layouts ao arrastar janela para o topo da tela",
        "opt_snap_assist": "Sugerir janelas abertas para preencher os quadrantes restantes (Snap Assist)",
        "opt_animations": "Ativar pré-visualização em acrílico e animações suaves de encaixe",
        "opt_csd_decor": "Substituir botões próprios (Electron / Chrome) pelos botões do Windows 11",
        "opt_shortcuts": "Ativar atalhos de teclado de ajuste do Windows 11",
        "lbl_hover_delay": "Atraso no hover do botão Maximizar (ms):",
        "lbl_gap_px": "Espaçamento entre janelas ajustadas (px):",
        "sec_shortcuts": "Atalhos de Teclado",
        "sec_shortcuts_desc": "Preenchidos como no Windows 11 por padrão. Clique em um atalho para alterar (verifica conflitos no sistema).",
        "btn_restore": "Restaurar padrões do Windows 11",
        "btn_apply": "Salvar e Aplicar",
        "btn_close": "Fechar",
        "btn_disabled": "Desativado",
        "prompt_press": "Pressione o atalho... (Esc cancela, Backspace limpa)",
        "err_need_mod": "Use ao menos uma tecla modificadora (Super, Ctrl, Alt, Shift) ou tecla de função.",
        "conflict_fmt": "Conflito! O atalho '{accel}' já está em uso por: {owner}",
        "saved_ok": "Configurações salvas e aplicadas ao vivo no win11-snapd.",
        "restore_ok": "Atalhos padrão do Windows 11 restaurados.",
        "act_snap_layouts": "Abrir / fechar popup Snap Layouts",
        "act_snap_left": "Ajustar janela à Esquerda / mover quadrante à esquerda",
        "act_snap_right": "Ajustar janela à Direita / mover quadrante à direita",
        "act_snap_up": "Maximizar / ajustar ao quadrante superior",
        "act_snap_down": "Restaurar / minimizar / ajustar ao quadrante inferior",
        "act_snap_top_half": "Ajustar janela na metade superior",
        "act_snap_bottom_half": "Ajustar janela na metade inferior",
        "owner_sibling": "Win11 Snap ({name})",
        "owner_cinnamon_wm": "Gerenciador Cinnamon ({key})",
        "owner_cinnamon_gen": "Cinnamon ({key})",
        "owner_media": "Atalhos do Sistema ({key})",
        "owner_custom": "Atalho Personalizado ({name})",
        "owner_x11": "Atalho global X11 em uso por outro aplicativo",
    },
}


def canonical_accel(accel: str) -> str:
    if not accel:
        return ""
    s = accel.strip()
    if not s or s.lower() == "disabled":
        return ""
    mods = set()
    pos = 0
    while s[pos:].startswith("<"):
        end = s.find(">", pos)
        if end < 0:
            break
        tok = s[pos + 1 : end].strip().lower()
        if tok in ("super", "mod4", "win"):
            mods.add("Super")
        elif tok in ("control", "ctrl", "primary"):
            mods.add("Control")
        elif tok in ("alt", "mod1"):
            mods.add("Alt")
        elif tok == "shift":
            mods.add("Shift")
        pos = end + 1
    key = s[pos:].strip()
    if not key:
        return ""
    if len(key) == 1:
        key = key.lower()
    else:
        aliases = {
            "left": "Left",
            "right": "Right",
            "up": "Up",
            "down": "Down",
            "return": "Return",
            "escape": "Escape",
            "space": "space",
            "tab": "Tab",
        }
        key = aliases.get(key.lower(), key)
    order = ["Control", "Shift", "Alt", "Super"]
    prefix = "".join(f"<{m}>" for m in order if m in mods)
    return prefix + key


def format_accel_human(accel: str, disabled_label: str) -> str:
    c = canonical_accel(accel)
    if not c:
        return disabled_label
    parts = []
    pos = 0
    while c[pos:].startswith("<"):
        end = c.find(">", pos)
        if end < 0:
            break
        tok = c[pos + 1 : end]
        parts.append("Win" if tok == "Super" else ("Ctrl" if tok == "Control" else tok))
        pos = end + 1
    key = c[pos:]
    if len(key) == 1:
        key = key.upper()
    parts.append(key)
    return " + ".join(parts)


def parse_ini_sections(path: str):
    """Read INI preserving section -> dict of keys."""
    sections = {}
    if not os.path.exists(path):
        return sections
    cur = "general"
    sections[cur] = {}
    with open(path, "r", encoding="utf-8") as fp:
        for raw in fp:
            s = raw.strip()
            if not s or s.startswith("#") or s.startswith(";"):
                continue
            if s.startswith("[") and "]" in s:
                cur = s[1 : s.index("]")].strip().lower()
                sections.setdefault(cur, {})
                continue
            if "=" in s:
                k, v = s.split("=", 1)
                k = k.strip().lower()
                # Strip inline comments
                v = re.split(r"\s+[#;]", v.strip(), maxsplit=1)[0].strip()
                sections[cur][k] = v
    return sections


def update_ini_preserve_comments(path: str, updates: dict):
    """Update only specified sections/keys while keeping comments & [profile:*] sections intact."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    lines = []
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as fp:
            lines = fp.readlines()

    out = []
    cur_sec = "general"
    written = {sec: set() for sec in updates}

    def flush_missing(sec):
        if sec in updates:
            for k, v in updates[sec].items():
                if k not in written[sec]:
                    out.append(f"{k} = {v}\n")
                    written[sec].add(k)

    seen_sections = set()
    for raw in lines:
        s = raw.strip()
        if s.startswith("[") and "]" in s:
            if cur_sec in updates:
                flush_missing(cur_sec)
            cur_sec = s[1 : s.index("]")].strip().lower()
            seen_sections.add(cur_sec)
            out.append(raw)
            continue
        if s and not s.startswith("#") and not s.startswith(";") and "=" in s and cur_sec in updates:
            k = s.split("=", 1)[0].strip().lower()
            if k in updates[cur_sec]:
                out.append(f"{k} = {updates[cur_sec][k]}\n")
                written[cur_sec].add(k)
                continue
        out.append(raw)

    if cur_sec in updates:
        flush_missing(cur_sec)

    for sec, kv in updates.items():
        if sec not in seen_sections:
            if out and not out[-1].endswith("\n"):
                out.append("\n")
            out.append(f"\n[{sec}]\n")
            for k, v in kv.items():
                out.append(f"{k} = {v}\n")

    tmp_path = path + ".tmp"
    with open(tmp_path, "w", encoding="utf-8") as fp:
        fp.writelines(out)
    os.replace(tmp_path, path)


def test_x11_grab_conflict(accel: str) -> bool:
    """Return True if another running X11 client already holds an exclusive grab on accel."""
    c = canonical_accel(accel)
    if not c:
        return False
    lib_path = ctypes.util.find_library("X11")
    if not lib_path:
        return False
    x11 = ctypes.cdll.LoadLibrary(lib_path)
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XStringToKeysym.restype = ctypes.c_ulong
    x11.XKeysymToKeycode.restype = ctypes.c_uint

    mods = 0
    pos = 0
    while c[pos:].startswith("<"):
        end = c.find(">", pos)
        tok = c[pos + 1 : end]
        if tok == "Control":
            mods |= (1 << 2)
        elif tok == "Shift":
            mods |= (1 << 0)
        elif tok == "Alt":
            mods |= (1 << 3)
        elif tok == "Super":
            mods |= (1 << 6)
        pos = end + 1
    key = c[pos:]

    dpy = x11.XOpenDisplay(None)
    if not dpy:
        return False
    try:
        ks = x11.XStringToKeysym(key.encode())
        if not ks:
            return False
        kc = x11.XKeysymToKeycode(dpy, ks)
        if not kc:
            return False
        root = x11.XDefaultRootWindow(dpy)
        bad_access = False

        err_cb_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
        x11.XSetErrorHandler.argtypes = [ctypes.c_void_p]
        x11.XSetErrorHandler.restype = ctypes.c_void_p

        @err_cb_type
        def on_err(_d, ev_ptr):
            nonlocal bad_access
            class XErr(ctypes.Structure):
                _fields_ = [
                    ("type", ctypes.c_int),
                    ("display", ctypes.c_void_p),
                    ("resourceid", ctypes.c_ulong),
                    ("serial", ctypes.c_ulong),
                    ("error_code", ctypes.c_ubyte),
                ]
            ev = ctypes.cast(ev_ptr, ctypes.POINTER(XErr)).contents
            if ev.error_code == 10:  # BadAccess
                bad_access = True
            return 0

        cb_ptr = ctypes.cast(on_err, ctypes.c_void_p)
        old_h = x11.XSetErrorHandler(cb_ptr)
        x11.XGrabKey(dpy, kc, mods, root, False, 1, 1)
        x11.XSync(dpy, False)
        if not bad_access:
            x11.XUngrabKey(dpy, kc, mods, root)
            x11.XSync(dpy, False)
        x11.XSetErrorHandler(old_h)
        return bad_access
    finally:
        x11.XCloseDisplay(dpy)


def collect_system_shortcuts(t: dict, exclude_win11_tile_handoff: bool = False):
    """Map canonical accel -> human-readable owner string across Cinnamon & custom schemas."""
    conflicts = {}
    handoff_wm_keys = {"push-tile-left", "push-tile-right", "push-tile-up", "push-tile-down"}

    def scan_schema(schema_id, path, fmt_key, skip_keys=None):
        try:
            src = Gio.SettingsSchemaSource.get_default()
            schema = src.lookup(schema_id, True)
            if not schema:
                return
            gs = Gio.Settings.new_with_path(schema_id, path) if path else Gio.Settings.new(schema_id)
            for k in schema.list_keys():
                if skip_keys and k in skip_keys:
                    continue
                val = gs.get_value(k)
                tstr = val.get_type_string()
                items = list(val.unpack()) if tstr == "as" else ([val.unpack()] if tstr == "s" else [])
                for raw in items:
                    c = canonical_accel(raw)
                    if c and c not in conflicts:
                        conflicts[c] = t[fmt_key].format(key=k)
        except Exception:
            pass

    scan_schema(
        "org.cinnamon.desktop.keybindings.wm",
        None,
        "owner_cinnamon_wm",
        skip_keys=(handoff_wm_keys if exclude_win11_tile_handoff else None),
    )
    scan_schema("org.cinnamon.desktop.keybindings", None, "owner_cinnamon_gen")
    scan_schema("org.cinnamon.desktop.keybindings.media-keys", None, "owner_media")

    try:
        gs = Gio.Settings.new("org.cinnamon.desktop.keybindings")
        custom_list = gs.get_strv("custom-list")
        for cid in custom_list:
            cpath = f"/org/cinnamon/desktop/keybindings/custom-keybindings/{cid}/"
            cgs = Gio.Settings.new_with_path("org.cinnamon.desktop.keybindings.custom-keybinding", cpath)
            name = cgs.get_string("name") or cid
            for raw in cgs.get_strv("binding"):
                c = canonical_accel(raw)
                if c and c not in conflicts:
                    conflicts[c] = t["owner_custom"].format(name=name)
    except Exception:
        pass

    return conflicts


def sync_cinnamon_handoff(enable_drag_snap: bool, enable_shortcuts: bool, shortcuts: dict):
    """Back up original Cinnamon edge-tiling / push-tile-* keys once, and manage handoff cleanly."""
    try:
        wm = Gio.Settings.new("org.cinnamon.desktop.keybindings.wm")
        muffin = Gio.Settings.new("org.cinnamon.muffin")
    except Exception:
        return

    backup = parse_ini_sections(BACKUP_PATH).get("backup", {})
    changed_backup = False
    tile_keys = ["push-tile-left", "push-tile-right", "push-tile-up", "push-tile-down"]

    if "edge_tiling" not in backup:
        backup["edge_tiling"] = "true" if muffin.get_boolean("edge-tiling") else "false"
        changed_backup = True
    for k in tile_keys:
        if k not in backup:
            backup[k] = ",".join(wm.get_strv(k))
            changed_backup = True
    if changed_backup:
        update_ini_preserve_comments(BACKUP_PATH, {"backup": backup})

    # Hand off edge-tiling when win11-snapd handles drag_snap
    muffin.set_boolean("edge-tiling", False if enable_drag_snap else (backup.get("edge_tiling", "true") == "true"))

    # Hand off push-tile-* only when win11-snapd claims those exact Super+Arrow keys
    claimed = {canonical_accel(v) for v in shortcuts.values() if canonical_accel(v)}
    defaults = [canonical_accel(x) for x in ["<Super>Left", "<Super>Right", "<Super>Up", "<Super>Down"]]
    if enable_shortcuts:
        for k, def_c in zip(tile_keys, defaults):
            cur = [canonical_accel(x) for x in wm.get_strv(k)]
            if def_c in claimed:
                if def_c in cur:
                    wm.set_strv(k, [])
            elif not cur and backup.get(k):
                wm.set_strv(k, [x for x in backup[k].split(",") if x])
    else:
        for k in tile_keys:
            if not wm.get_strv(k) and backup.get(k):
                wm.set_strv(k, [x for x in backup[k].split(",") if x])

    Gio.Settings.sync()


def send_daemon_reload():
    try:
        out = subprocess.check_output(["pgrep", "-x", "win11-snapd"], text=True)
        for line in out.splitlines():
            pid = int(line.strip())
            os.kill(pid, signal.SIGUSR1)
        return
    except Exception:
        pass
    try:
        subprocess.run(
            ["systemctl", "--user", "kill", "-s", "SIGUSR1", "win11-snapd.service"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except Exception:
        pass


class Win11SnapPanel(Gtk.Box):
    __gtype_name__ = "Win11SnapPanel"

    def __init__(self, xlet_settings=None, embedded: bool = False, on_close=None):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        self.xlet_settings = xlet_settings
        self.embedded = embedded
        self.on_close = on_close
        self._suppress_listen = False

        self.cfg = parse_ini_sections(CONFIG_PATH)
        gen = self.cfg.get("general", {})
        sc = self.cfg.get("shortcuts", {})

        lang = self._xs_get("language", gen.get("language", "en"))
        self.lang = lang if lang in TRANSLATIONS else "en"

        self.shortcuts = {
            k: str(self._xs_get(k, sc.get(k, WIN11_DEFAULT_SHORTCUTS[k])))
            for k in SHORTCUT_ORDER
        }
        self.capturing_key = None
        self.shortcut_buttons = {}
        self.shortcut_labels = {}
        self._key_handler_id = None

        self._apply_css()
        self._build_ui(gen)
        self._retranslate()
        self.connect("realize", self._on_realize_attach_keys)
        self._bind_xlet_listeners()

    def _xs_get(self, key: str, fallback):
        if self.xlet_settings is not None:
            try:
                if self.xlet_settings.has_key(key):
                    val = self.xlet_settings.get_value(key)
                    if val is not None:
                        return val
            except Exception:
                pass
        return fallback

    def _on_realize_attach_keys(self, *_args):
        top = self.get_toplevel()
        if top and self._key_handler_id is None:
            self._key_handler_id = top.connect("key-press-event", self._on_window_key_press)

    def _bind_xlet_listeners(self):
        if self.xlet_settings is None:
            return
        keys = [
            "language", "drag_snap", "top_snap_bar", "enable_snap_assist",
            "enable_animations", "csd_decor", "enable_shortcuts",
            "hover_delay_ms", "gap_px", *SHORTCUT_ORDER,
        ]
        for k in keys:
            try:
                if self.xlet_settings.has_key(k):
                    self.xlet_settings.listen(k, self._on_xlet_setting_external_change)
            except Exception:
                pass

    def _on_xlet_setting_external_change(self, key, value):
        if self._suppress_listen:
            return
        if key == "language" and value in TRANSLATIONS:
            self.lang = value
            self.combo_lang.set_active_id(value)
            self._retranslate()
        elif key == "drag_snap":
            self.sw_drag_snap.set_active(bool(value))
        elif key == "top_snap_bar":
            self.sw_top_bar.set_active(bool(value))
        elif key == "enable_snap_assist":
            self.sw_snap_assist.set_active(bool(value))
        elif key == "enable_animations":
            self.sw_animations.set_active(bool(value))
        elif key == "csd_decor":
            self.sw_csd_decor.set_active(bool(value))
        elif key == "enable_shortcuts":
            self.sw_shortcuts.set_active(bool(value))
        elif key == "hover_delay_ms":
            self.spin_hover.set_value(int(value))
        elif key == "gap_px":
            self.spin_gap.set_value(int(value))
        elif key in self.shortcuts:
            self.shortcuts[key] = str(value)
            self._refresh_shortcut_btn(key)

    def _tr(self, key: str) -> str:
        return TRANSLATIONS[self.lang].get(key, key)

    def _apply_css(self):
        bg_rule = "" if self.embedded else "window { background-color: #1c1c1e; color: #f3f3f3; }\n"
        css = (bg_rule + """
        .win11-snap-card {
            background-color: alpha(@theme_fg_color, 0.05);
            border: 1px solid alpha(@theme_fg_color, 0.14);
            border-radius: 8px;
            padding: 14px;
        }
        .section-title { font-weight: bold; font-size: 14px; }
        .muted { opacity: 0.72; font-size: 12px; }
        .shortcut-btn {
            min-width: 175px;
            padding: 6px 12px;
            border-radius: 6px;
        }
        .shortcut-capturing {
            background-color: #164e6e;
            border: 1px solid #60cdff;
            color: #ffffff;
        }
        .banner-ok {
            background-color: rgba(31, 158, 222, 0.18);
            border: 1px solid rgba(96, 205, 255, 0.55);
            border-radius: 6px;
            padding: 8px 12px;
        }
        .banner-err {
            background-color: rgba(196, 43, 28, 0.24);
            border: 1px solid rgba(255, 110, 96, 0.75);
            border-radius: 6px;
            padding: 8px 12px;
        }
        """).encode("utf-8")
        prov = Gtk.CssProvider()
        prov.load_from_data(css)
        Gtk.StyleContext.add_provider_for_screen(
            Gdk.Screen.get_default(), prov, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
        )

    def _b(self, d, key, schema_key=None, default=True):
        if schema_key:
            v = self._xs_get(schema_key, None)
            if isinstance(v, bool):
                return v
        v = d.get(key)
        if v is None:
            return default
        return str(v).lower() in ("true", "1", "yes")

    def _build_ui(self, gen):
        root_box = self
        root_box.set_margin_top(12 if self.embedded else 16)
        root_box.set_margin_bottom(12 if self.embedded else 16)
        root_box.set_margin_start(14 if self.embedded else 18)
        root_box.set_margin_end(14 if self.embedded else 18)

        # Header + Language selector
        top_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        title_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        self.lbl_header_title = Gtk.Label(xalign=0)
        self.lbl_header_title.get_style_context().add_class("section-title")
        self.lbl_header_sub = Gtk.Label(xalign=0)
        self.lbl_header_sub.get_style_context().add_class("muted")
        self.lbl_header_sub.set_line_wrap(True)
        title_box.pack_start(self.lbl_header_title, False, False, 0)
        title_box.pack_start(self.lbl_header_sub, False, False, 0)
        top_row.pack_start(title_box, True, True, 0)

        lang_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        lang_box.set_valign(Gtk.Align.CENTER)
        self.lbl_lang = Gtk.Label()
        self.combo_lang = Gtk.ComboBoxText()
        self.combo_lang.append("en", "English")
        self.combo_lang.append("pt_BR", "Português (BR)")
        self.combo_lang.set_active_id(self.lang)
        self.combo_lang.connect("changed", self._on_language_changed)
        lang_box.pack_start(self.lbl_lang, False, False, 0)
        lang_box.pack_start(self.combo_lang, False, False, 0)
        top_row.pack_end(lang_box, False, False, 0)
        root_box.pack_start(top_row, False, False, 0)

        # Scrollable main body
        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        scroll.add(body)
        root_box.pack_start(scroll, True, True, 0)

        # Card 1: Behavior
        card1 = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        card1.get_style_context().add_class("win11-snap-card")
        self.lbl_sec_behavior = Gtk.Label(xalign=0)
        self.lbl_sec_behavior.get_style_context().add_class("section-title")
        card1.pack_start(self.lbl_sec_behavior, False, False, 4)

        self.sw_drag_snap = self._add_switch_row(card1, "opt_drag_snap", self._b(gen, "drag_snap", "drag_snap", True))
        self.sw_top_bar = self._add_switch_row(card1, "opt_top_bar", self._b(gen, "top_snap_bar", "top_snap_bar", True))
        self.sw_snap_assist = self._add_switch_row(card1, "opt_snap_assist", self._b(gen, "enable_snap_assist", "enable_snap_assist", True))
        self.sw_animations = self._add_switch_row(card1, "opt_animations", self._b(gen, "enable_animations", "enable_animations", True))
        self.sw_csd_decor = self._add_switch_row(card1, "opt_csd_decor", self._b(gen, "decorate_custom_csd", "csd_decor", True))
        self.sw_shortcuts = self._add_switch_row(card1, "opt_shortcuts", self._b(gen, "enable_shortcuts", "enable_shortcuts", True))

        hover_val = int(self._xs_get("hover_delay_ms", gen.get("hover_delay_ms", "450")))
        gap_val = int(self._xs_get("gap_px", gen.get("gap_px", "0")))
        self.spin_hover = self._add_spin_row(card1, "lbl_hover_delay", hover_val, 100, 2000, 25)
        self.spin_gap = self._add_spin_row(card1, "lbl_gap_px", gap_val, 0, 32, 1)
        body.pack_start(card1, False, False, 0)

        # Card 2: Shortcuts
        card2 = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        card2.get_style_context().add_class("win11-snap-card")
        hdr2 = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.lbl_sec_shortcuts = Gtk.Label(xalign=0)
        self.lbl_sec_shortcuts.get_style_context().add_class("section-title")
        self.btn_restore = Gtk.Button()
        self.btn_restore.connect("clicked", self._on_restore_defaults)
        hdr2.pack_start(self.lbl_sec_shortcuts, True, True, 0)
        hdr2.pack_end(self.btn_restore, False, False, 0)
        card2.pack_start(hdr2, False, False, 0)

        self.lbl_sec_shortcuts_desc = Gtk.Label(xalign=0)
        self.lbl_sec_shortcuts_desc.get_style_context().add_class("muted")
        self.lbl_sec_shortcuts_desc.set_line_wrap(True)
        card2.pack_start(self.lbl_sec_shortcuts_desc, False, False, 4)

        for key in SHORTCUT_ORDER:
            row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
            lbl = Gtk.Label(xalign=0)
            lbl.set_line_wrap(True)
            btn = Gtk.Button()
            btn.get_style_context().add_class("shortcut-btn")
            btn.connect("clicked", self._on_shortcut_btn_clicked, key)
            row.pack_start(lbl, True, True, 0)
            row.pack_end(btn, False, False, 0)
            card2.pack_start(row, False, False, 2)
            self.shortcut_labels[key] = lbl
            self.shortcut_buttons[key] = btn

        body.pack_start(card2, False, False, 0)

        # Status banner (conflicts / save confirmation)
        self.status_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.lbl_status = Gtk.Label(xalign=0)
        self.lbl_status.set_line_wrap(True)
        self.status_box.pack_start(self.lbl_status, True, True, 0)
        self.status_box.set_no_show_all(True)
        root_box.pack_start(self.status_box, False, False, 0)

        # Footer action buttons
        foot = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.btn_close = None
        if not self.embedded and self.on_close is not None:
            self.btn_close = Gtk.Button()
            self.btn_close.connect("clicked", lambda *_: self.on_close())
            foot.pack_end(self.btn_close, False, False, 0)
        self.btn_apply = Gtk.Button()
        self.btn_apply.get_style_context().add_class("suggested-action")
        self.btn_apply.connect("clicked", self._on_save_clicked)
        foot.pack_end(self.btn_apply, False, False, 0)
        root_box.pack_start(foot, False, False, 0)

        self._switch_labels = {}

    def _add_switch_row(self, parent, tr_key, active):
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        lbl = Gtk.Label(xalign=0)
        lbl.set_line_wrap(True)
        lbl._tr_key = tr_key
        sw = Gtk.Switch()
        sw.set_active(active)
        sw.set_valign(Gtk.Align.CENTER)
        row.pack_start(lbl, True, True, 0)
        row.pack_end(sw, False, False, 0)
        parent.pack_start(row, False, False, 2)
        sw._label_widget = lbl
        return sw

    def _add_spin_row(self, parent, tr_key, value, lo, hi, step):
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        lbl = Gtk.Label(xalign=0)
        lbl._tr_key = tr_key
        adj = Gtk.Adjustment(value=value, lower=lo, upper=hi, step_increment=step, page_increment=step * 4)
        spin = Gtk.SpinButton(adjustment=adj, climb_rate=1, digits=0)
        spin.set_numeric(True)
        row.pack_start(lbl, True, True, 0)
        row.pack_end(spin, False, False, 0)
        parent.pack_start(row, False, False, 2)
        spin._label_widget = lbl
        return spin

    def _retranslate(self):
        top = self.get_toplevel()
        if isinstance(top, Gtk.Window) and not self.embedded:
            top.set_title(self._tr("win_title"))
        self.lbl_header_title.set_text(self._tr("header_title"))
        self.lbl_header_sub.set_text(self._tr("header_sub"))
        self.lbl_lang.set_text(self._tr("lang_label"))
        self.lbl_sec_behavior.set_text(self._tr("sec_behavior"))
        for w in (
            self.sw_drag_snap, self.sw_top_bar, self.sw_snap_assist,
            self.sw_animations, self.sw_csd_decor, self.sw_shortcuts,
            self.spin_hover, self.spin_gap,
        ):
            w._label_widget.set_text(self._tr(w._label_widget._tr_key))
        self.lbl_sec_shortcuts.set_text(self._tr("sec_shortcuts"))
        self.lbl_sec_shortcuts_desc.set_text(self._tr("sec_shortcuts_desc"))
        self.btn_restore.set_label(self._tr("btn_restore"))
        self.btn_apply.set_label(self._tr("btn_apply"))
        if self.btn_close is not None:
            self.btn_close.set_label(self._tr("btn_close"))
        for key in SHORTCUT_ORDER:
            self.shortcut_labels[key].set_text(self._tr(f"act_{key}"))
            self._refresh_shortcut_btn(key)

    def _refresh_shortcut_btn(self, key):
        btn = self.shortcut_buttons[key]
        ctx = btn.get_style_context()
        if self.capturing_key == key:
            btn.set_label(self._tr("prompt_press"))
            ctx.add_class("shortcut-capturing")
        else:
            btn.set_label(format_accel_human(self.shortcuts[key], self._tr("btn_disabled")))
            ctx.remove_class("shortcut-capturing")

    def _show_banner(self, msg: str, is_error: bool):
        ctx = self.status_box.get_style_context()
        ctx.remove_class("banner-ok")
        ctx.remove_class("banner-err")
        ctx.add_class("banner-err" if is_error else "banner-ok")
        self.lbl_status.set_text(msg)
        self.status_box.show()
        self.lbl_status.show()

    def _on_language_changed(self, combo):
        new_lang = combo.get_active_id() or "en"
        if new_lang in TRANSLATIONS and new_lang != self.lang:
            self.lang = new_lang
            self._retranslate()

    def _on_shortcut_btn_clicked(self, _btn, key):
        prev = self.capturing_key
        self.capturing_key = key if prev != key else None
        if prev and prev in self.shortcut_buttons:
            self._refresh_shortcut_btn(prev)
        self._refresh_shortcut_btn(key)

    def _find_conflict(self, target_key: str, cand_accel: str):
        c = canonical_accel(cand_accel)
        if not c:
            return None
        for other_key, other_val in self.shortcuts.items():
            if other_key != target_key and canonical_accel(other_val) == c:
                return self._tr("owner_sibling").format(name=self._tr(f"act_{other_key}"))

        sys_map = collect_system_shortcuts(TRANSLATIONS[self.lang], exclude_win11_tile_handoff=True)
        # Allow Windows default arrows to replace Cinnamon push-tile even if still present in Cinnamon before save
        if c in sys_map:
            return sys_map[c]

        # Probe X11 only if this shortcut isn't already held by win11-snapd itself
        cur_daemon_vals = {
            canonical_accel(self.cfg.get("shortcuts", {}).get(k, WIN11_DEFAULT_SHORTCUTS[k]))
            for k in SHORTCUT_ORDER
        }
        if c not in cur_daemon_vals and test_x11_grab_conflict(c):
            return self._tr("owner_x11")
        return None

    def _on_window_key_press(self, _widget, ev):
        if not self.capturing_key:
            return False

        key = self.capturing_key
        kv = ev.keyval
        if kv in (
            Gdk.KEY_Super_L, Gdk.KEY_Super_R, Gdk.KEY_Control_L, Gdk.KEY_Control_R,
            Gdk.KEY_Alt_L, Gdk.KEY_Alt_R, Gdk.KEY_Shift_L, Gdk.KEY_Shift_R,
            Gdk.KEY_Meta_L, Gdk.KEY_Meta_R, Gdk.KEY_Hyper_L, Gdk.KEY_Hyper_R,
        ):
            return True

        if kv == Gdk.KEY_Escape:
            self.capturing_key = None
            self._refresh_shortcut_btn(key)
            return True

        if kv in (Gdk.KEY_BackSpace, Gdk.KEY_Delete):
            self.shortcuts[key] = ""
            self.capturing_key = None
            self._refresh_shortcut_btn(key)
            self.status_box.hide()
            return True

        state = ev.state
        mods = ""
        if state & Gdk.ModifierType.CONTROL_MASK:
            mods += "<Control>"
        if state & Gdk.ModifierType.SHIFT_MASK:
            mods += "<Shift>"
        if state & (Gdk.ModifierType.MOD1_MASK):
            mods += "<Alt>"
        if state & (Gdk.ModifierType.SUPER_MASK | Gdk.ModifierType.MOD4_MASK | Gdk.ModifierType.HYPER_MASK):
            mods += "<Super>"

        keyname = Gdk.keyval_name(Gdk.keyval_to_lower(kv)) or ""
        is_fn = keyname.startswith("F") and keyname[1:].isdigit()
        if not mods and not is_fn:
            self._show_banner(self._tr("err_need_mod"), True)
            return True

        cand = canonical_accel(mods + keyname)
        conflict_owner = self._find_conflict(key, cand)
        if conflict_owner:
            human = format_accel_human(cand, self._tr("btn_disabled"))
            self._show_banner(self._tr("conflict_fmt").format(accel=human, owner=conflict_owner), True)
            self.capturing_key = None
            self._refresh_shortcut_btn(key)
            return True

        self.shortcuts[key] = cand
        self.capturing_key = None
        self._refresh_shortcut_btn(key)
        self.status_box.hide()
        return True

    def _on_restore_defaults(self, _btn):
        # Validate defaults against non-tile system shortcuts before applying
        for k, def_val in WIN11_DEFAULT_SHORTCUTS.items():
            sys_map = collect_system_shortcuts(TRANSLATIONS[self.lang], exclude_win11_tile_handoff=True)
            c = canonical_accel(def_val)
            if c in sys_map:
                human = format_accel_human(c, "")
                self._show_banner(self._tr("conflict_fmt").format(accel=human, owner=sys_map[c]), True)
                return
        for k in SHORTCUT_ORDER:
            self.shortcuts[k] = WIN11_DEFAULT_SHORTCUTS[k]
            self._refresh_shortcut_btn(k)
        self._show_banner(self._tr("restore_ok"), False)

    def _on_save_clicked(self, _btn):
        drag_on = self.sw_drag_snap.get_active()
        top_bar_on = self.sw_top_bar.get_active()
        assist_on = self.sw_snap_assist.get_active()
        anim_on = self.sw_animations.get_active()
        csd_on = self.sw_csd_decor.get_active()
        sc_on = self.sw_shortcuts.get_active()
        hover_ms = self.spin_hover.get_value_as_int()
        gap_px = self.spin_gap.get_value_as_int()

        updates = {
            "general": {
                "language": self.lang,
                "drag_snap": "true" if drag_on else "false",
                "top_snap_bar": "true" if top_bar_on else "false",
                "enable_snap_assist": "true" if assist_on else "false",
                "enable_animations": "true" if anim_on else "false",
                "decorate_custom_csd": "true" if csd_on else "false",
                "enable_shortcuts": "true" if sc_on else "false",
                "hotkey_super_z": "true" if (sc_on and bool(canonical_accel(self.shortcuts["snap_layouts"]))) else "false",
                "hover_delay_ms": str(hover_ms),
                "gap_px": str(gap_px),
            },
            "shortcuts": {k: self.shortcuts[k] for k in SHORTCUT_ORDER},
        }
        # First hand off Cinnamon edge-tiling / push-tile-* before daemon grabs shortcuts
        sync_cinnamon_handoff(drag_on, sc_on, self.shortcuts)
        update_ini_preserve_comments(CONFIG_PATH, updates)
        self.cfg = parse_ini_sections(CONFIG_PATH)

        if self.xlet_settings is not None:
            self._suppress_listen = True
            try:
                pairs = {
                    "language": self.lang,
                    "drag_snap": drag_on,
                    "top_snap_bar": top_bar_on,
                    "enable_snap_assist": assist_on,
                    "enable_animations": anim_on,
                    "csd_decor": csd_on,
                    "enable_shortcuts": sc_on,
                    "hover_delay_ms": hover_ms,
                    "gap_px": gap_px,
                    **{k: self.shortcuts[k] for k in SHORTCUT_ORDER},
                }
                for k, v in pairs.items():
                    if self.xlet_settings.has_key(k):
                        self.xlet_settings.set_value(k, v)
            finally:
                self._suppress_listen = False

        send_daemon_reload()
        self._show_banner(self._tr("saved_ok"), False)


class Win11SnapSettingsWindow(Gtk.Window):
    def __init__(self, xlet_settings=None):
        super().__init__(title="Win11 Snap Settings")
        self.set_default_size(680, 720)
        self.set_position(Gtk.WindowPosition.CENTER)
        self.panel = Win11SnapPanel(xlet_settings=xlet_settings, embedded=False, on_close=self.close)
        self.add(self.panel)


def main():
    win = Win11SnapSettingsWindow()
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()
