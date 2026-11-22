import importlib.util
import os

from JsonSettingsWidgets import SettingsPage


def _load_panel_module():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ui", "win11_snap_settings.py")
    spec = importlib.util.spec_from_file_location("win11_snap_ext_panel", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Win11SnapSettingsPage(SettingsPage):
    """Custom SettingsPage embedded directly inside Cinnamon Extensions Settings."""

    __gtype_name__ = "Win11SnapSettingsPage"

    def __init__(self, _info, xlet_settings):
        super().__init__()
        self.set_margin_left(0)
        self.set_margin_right(0)
        self.set_margin_top(0)
        self.set_margin_bottom(0)
        self.set_spacing(0)

        mod = _load_panel_module()
        self.panel = mod.Win11SnapPanel(xlet_settings=xlet_settings, embedded=True)
        self.pack_start(self.panel, True, True, 0)
