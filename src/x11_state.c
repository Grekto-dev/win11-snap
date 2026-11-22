#include "x11_state.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int g_last_x_error = 0;

static int safe_x_error_handler(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    g_last_x_error = ev->error_code;
    return 0;
}

void x11_install_error_handler(void) {
    XSetErrorHandler(safe_x_error_handler);
}

int x11_take_last_error(void) {
    int err = g_last_x_error;
    g_last_x_error = 0;
    return err;
}

void x11_init_atoms(Display *dpy, X11Atoms *a) {
    a->net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    a->net_current_desktop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    a->net_workarea = XInternAtom(dpy, "_NET_WORKAREA", False);
    a->net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    a->net_wm_window_type_normal = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    a->net_wm_window_type_dialog = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    a->net_wm_window_type_desktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    a->net_wm_window_type_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    a->net_wm_window_type_toolbar = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    a->net_wm_window_type_menu = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
    a->net_wm_window_type_utility = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    a->net_wm_window_type_splash = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    a->net_wm_window_type_dropdown_menu = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
    a->net_wm_window_type_popup_menu = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    a->net_wm_window_type_tooltip = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
    a->net_wm_window_type_notification = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    a->net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    a->net_wm_state_maximized_horz = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    a->net_wm_state_maximized_vert = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    a->net_wm_state_fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    a->net_wm_state_above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    a->net_wm_allowed_actions = XInternAtom(dpy, "_NET_WM_ALLOWED_ACTIONS", False);
    a->net_wm_action_maximize_horz = XInternAtom(dpy, "_NET_WM_ACTION_MAXIMIZE_HORZ", False);
    a->net_wm_action_maximize_vert = XInternAtom(dpy, "_NET_WM_ACTION_MAXIMIZE_VERT", False);
    a->net_frame_extents = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
    a->gtk_frame_extents = XInternAtom(dpy, "_GTK_FRAME_EXTENTS", False);
    a->motif_wm_hints = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    a->win11_max_button = XInternAtom(dpy, "_WIN11_MAX_BUTTON", False);
    a->wm_window_role = XInternAtom(dpy, "WM_WINDOW_ROLE", False);
    a->gtk_hide_titlebar_when_maximized = XInternAtom(dpy, "_GTK_HIDE_TITLEBAR_WHEN_MAXIMIZED", False);
    a->gtk_application_id = XInternAtom(dpy, "_GTK_APPLICATION_ID", False);
    a->net_moveresize_window = XInternAtom(dpy, "_NET_MOVERESIZE_WINDOW", False);
    a->net_close_window = XInternAtom(dpy, "_NET_CLOSE_WINDOW", False);
    a->net_wm_strut_partial = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    a->net_client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    a->net_client_list_stacking = XInternAtom(dpy, "_NET_CLIENT_LIST_STACKING", False);
    a->net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    a->net_wm_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
    a->net_wm_desktop = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    a->net_wm_state_hidden = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
    a->net_wm_state_skip_taskbar = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    a->utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    a->resource_manager = XInternAtom(dpy, "RESOURCE_MANAGER", False);
}

static bool get_prop_raw(Display *dpy, Window win, Atom prop, Atom req_type,
                         unsigned long max_words, unsigned char **out_data,
                         unsigned long *out_nitems, Atom *out_type) {
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    g_last_x_error = 0;
    int st = XGetWindowProperty(dpy, win, prop, 0, (long)max_words, False, req_type,
                                &actual_type, &actual_fmt, &nitems, &bytes_after, &data);
    if (st != Success || g_last_x_error != 0 || !data || nitems == 0) {
        if (data) XFree(data);
        return false;
    }
    *out_data = data;
    *out_nitems = nitems;
    if (out_type) *out_type = actual_type;
    return true;
}

static bool read_cardinals(Display *dpy, Window win, Atom prop,
                           long *out_vals, int max_vals, int *out_n) {
    unsigned char *data = NULL;
    unsigned long n = 0;
    if (!get_prop_raw(dpy, win, prop, AnyPropertyType, (unsigned long)max_vals, &data, &n, NULL)) {
        return false;
    }
    int cnt = (int)(n < (unsigned long)max_vals ? n : (unsigned long)max_vals);
    long *arr = (long *)data;
    for (int i = 0; i < cnt; i++) {
        out_vals[i] = arr[i];
    }
    if (out_n) *out_n = cnt;
    XFree(data);
    return cnt > 0;
}

static bool atom_list_has(const Atom *list, unsigned long n, Atom target) {
    for (unsigned long i = 0; i < n; i++) {
        if (list[i] == target) return true;
    }
    return false;
}

int x11_get_dpi_scale(Display *dpy, Window root, const X11Atoms *atoms) {
    unsigned char *data = NULL;
    unsigned long n = 0;
    double dpi = 96.0;
    if (get_prop_raw(dpy, root, atoms->resource_manager, XA_STRING, 8192, &data, &n, NULL)) {
        const char *p = strstr((const char *)data, "Xft.dpi:");
        if (p) {
            p += 8;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            double v = atof(p);
            if (v >= 72.0 && v <= 480.0) dpi = v;
        }
        XFree(data);
    }
    int scale = (int)lround(dpi / 96.0);
    return scale < 1 ? 1 : scale;
}

int x11_get_current_desktop(Display *dpy, Window root, const X11Atoms *atoms) {
    long v = 0;
    int n = 0;
    if (read_cardinals(dpy, root, atoms->net_current_desktop, &v, 1, &n) && n >= 1) {
        return (int)(v < 0 ? 0 : v);
    }
    return 0;
}

void x11_refresh_workareas(Display *dpy, Window root, const X11Atoms *atoms,
                           WorkArea out[MAX_MONITORS], int *out_count) {
    int desk = x11_get_current_desktop(dpy, root, atoms);
    char atom_name[48];
    snprintf(atom_name, sizeof(atom_name), "_GTK_WORKAREAS_D%d", desk);
    Atom gtk_wa = XInternAtom(dpy, atom_name, False);

    long vals[MAX_MONITORS * 4];
    int n = 0;
    if (read_cardinals(dpy, root, gtk_wa, vals, MAX_MONITORS * 4, &n) && n >= 4) {
        int count = n / 4;
        if (count > MAX_MONITORS) count = MAX_MONITORS;
        for (int i = 0; i < count; i++) {
            out[i] = (WorkArea){
                (int)vals[i * 4 + 0],
                (int)vals[i * 4 + 1],
                (int)vals[i * 4 + 2],
                (int)vals[i * 4 + 3]
            };
        }
        *out_count = count;
        return;
    }

    long net_wa[4] = {0, 0, DisplayWidth(dpy, DefaultScreen(dpy)), DisplayHeight(dpy, DefaultScreen(dpy))};
    int nw_n = 0;
    read_cardinals(dpy, root, atoms->net_workarea, net_wa, 4, &nw_n);
    int wa_x0 = (int)net_wa[0];
    int wa_y0 = (int)net_wa[1];
    int wa_x1 = wa_x0 + (int)net_wa[2];
    int wa_y1 = wa_y0 + (int)net_wa[3];

    int nmon = 0;
    XRRMonitorInfo *mons = XRRGetMonitors(dpy, root, True, &nmon);
    if (mons && nmon > 0) {
        int count = 0;
        for (int i = 0; i < nmon && count < MAX_MONITORS; i++) {
            int x0 = mons[i].x;
            int y0 = mons[i].y;
            int x1 = x0 + mons[i].width;
            int y1 = y0 + mons[i].height;
            if (x0 < wa_x0) x0 = wa_x0;
            if (y0 < wa_y0) y0 = wa_y0;
            if (x1 > wa_x1) x1 = wa_x1;
            if (y1 > wa_y1) y1 = wa_y1;
            if (x1 > x0 && y1 > y0) {
                out[count++] = (WorkArea){x0, y0, x1 - x0, y1 - y0};
            }
        }
        XRRFreeMonitors(mons);
        if (count > 0) {
            *out_count = count;
            return;
        }
    }

    out[0] = (WorkArea){wa_x0, wa_y0, wa_x1 - wa_x0, wa_y1 - wa_y0};
    *out_count = 1;
}

Window x11_get_active_window(Display *dpy, Window root, const X11Atoms *atoms) {
    unsigned char *data = NULL;
    unsigned long n = 0;
    if (!get_prop_raw(dpy, root, atoms->net_active_window, XA_WINDOW, 1, &data, &n, NULL)) {
        return None;
    }
    Window w = *(Window *)data;
    XFree(data);
    return w;
}

static Window find_frame_window(Display *dpy, Window root, Window win) {
    Window cur = win;
    for (int depth = 0; depth < 16; depth++) {
        Window r = None, parent = None, *children = NULL;
        unsigned int nchildren = 0;
        g_last_x_error = 0;
        if (!XQueryTree(dpy, cur, &r, &parent, &children, &nchildren) || g_last_x_error != 0) {
            if (children) XFree(children);
            return win;
        }
        if (children) XFree(children);
        if (parent == None || parent == root) {
            return cur;
        }
        cur = parent;
    }
    return win;
}

int x11_find_maximize_index_from_right(const char *button_layout) {
    const char *colon = strchr(button_layout, ':');
    const char *right = colon ? (colon + 1) : button_layout;
    if (!right[0]) return -1;

    char copy[128];
    snprintf(copy, sizeof(copy), "%s", right);

    const char *tokens[16];
    int ntok = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok && ntok < 16; tok = strtok_r(NULL, ",", &save)) {
        while (*tok && isspace((unsigned char)*tok)) tok++;
        size_t l = strlen(tok);
        while (l > 0 && isspace((unsigned char)tok[l - 1])) tok[--l] = '\0';
        if (tok[0]) tokens[ntok++] = tok;
    }

    int idx_from_right = 0;
    for (int i = ntok - 1; i >= 0; i--) {
        if (strcmp(tokens[i], "maximize") == 0) {
            return idx_from_right;
        }
        if (strcmp(tokens[i], "spacer") != 0) {
            idx_from_right++;
        }
    }
    return -1;
}

static const ClassProfile *match_profile(const AppConfig *cfg, const char *wm_class) {
    if (!wm_class[0]) return NULL;
    for (int i = 0; i < cfg->num_profiles; i++) {
        if (strcmp(cfg->profiles[i].wm_class, wm_class) == 0) {
            return &cfg->profiles[i];
        }
    }
    return NULL;
}

static bool is_wm_class_ignored(const AppConfig *cfg, const char *wm_class) {
    if (!wm_class[0] || !cfg->ignore_wm_classes[0]) return false;
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", cfg->ignore_wm_classes);
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok && isspace((unsigned char)*tok)) tok++;
        size_t l = strlen(tok);
        while (l > 0 && isspace((unsigned char)tok[l - 1])) tok[--l] = '\0';
        if (tok[0] && strcmp(tok, wm_class) == 0) return true;
    }
    return false;
}

static bool is_chromium_or_electron_csd(Display *dpy, Window win, const X11Atoms *atoms) {
    unsigned char *data = NULL;
    unsigned long n = 0;
    if (get_prop_raw(dpy, win, atoms->wm_window_role, AnyPropertyType, 32, &data, &n, NULL)) {
        bool is_browser = (strncmp((const char *)data, "browser", 7) == 0);
        XFree(data);
        if (is_browser) return true;
    }
    long hide_tb = 0;
    int hn = 0;
    if (read_cardinals(dpy, win, atoms->gtk_hide_titlebar_when_maximized, &hide_tb, 1, &hn) &&
        hn >= 1 && hide_tb != 0) {
        if (!get_prop_raw(dpy, win, atoms->gtk_application_id, AnyPropertyType, 32, &data, &n, NULL)) {
            return true;
        }
        XFree(data);
    }
    return false;
}

bool x11_read_window_info(Display *dpy, Window root, const X11Atoms *atoms,
                          const AppConfig *cfg, Window win, WindowInfo *out) {
    memset(out, 0, sizeof(*out));
    out->win = win;
    out->frame_win = win;
    if (win == None || win == root) return false;

    /* 1. Filter by _NET_WM_WINDOW_TYPE */
    unsigned char *data = NULL;
    unsigned long n = 0;
    if (get_prop_raw(dpy, win, atoms->net_wm_window_type, XA_ATOM, 16, &data, &n, NULL)) {
        Atom *types = (Atom *)data;
        bool reject = atom_list_has(types, n, atoms->net_wm_window_type_desktop) ||
                      atom_list_has(types, n, atoms->net_wm_window_type_dock) ||
                      atom_list_has(types, n, atoms->net_wm_window_type_toolbar) ||
                      atom_list_has(types, n, atoms->net_wm_window_type_menu) ||
                      atom_list_has(types, n, atoms->net_wm_window_type_utility) ||
                      atom_list_has(types, n, atoms->net_wm_window_type_splash) ||
                      atom_list_has(types, n, atoms->net_wm_window_type_dropdown_menu) ||
                      atom_list_has(types, n, atoms->net_wm_window_type_popup_menu) ||
                      atom_list_has(types, n, atoms->net_wm_window_type_tooltip) ||
                      atom_list_has(types, n, atoms->net_wm_window_type_notification);
        XFree(data);
        if (reject) return false;
    }

    /* 2. Check _NET_WM_STATE (fullscreen / maximized) */
    if (get_prop_raw(dpy, win, atoms->net_wm_state, XA_ATOM, 32, &data, &n, NULL)) {
        Atom *states = (Atom *)data;
        out->is_fullscreen = atom_list_has(states, n, atoms->net_wm_state_fullscreen);
        out->is_maximized = atom_list_has(states, n, atoms->net_wm_state_maximized_horz) ||
                            atom_list_has(states, n, atoms->net_wm_state_maximized_vert);
        XFree(data);
        if (out->is_fullscreen) return false;
    }

    /* 3. Check _NET_WM_ALLOWED_ACTIONS */
    if (get_prop_raw(dpy, win, atoms->net_wm_allowed_actions, XA_ATOM, 32, &data, &n, NULL)) {
        Atom *acts = (Atom *)data;
        out->can_maximize = atom_list_has(acts, n, atoms->net_wm_action_maximize_horz) &&
                            atom_list_has(acts, n, atoms->net_wm_action_maximize_vert);
        XFree(data);
        if (!out->can_maximize) return false;
    } else {
        XSizeHints sh;
        long sup = 0;
        if (XGetWMNormalHints(dpy, win, &sh, &sup) &&
            (sh.flags & PMinSize) && (sh.flags & PMaxSize) &&
            sh.min_width == sh.max_width && sh.min_height == sh.max_height) {
            return false;
        }
        out->can_maximize = true;
    }

    /* 4. Read WM_CLASS and check ignore list */
    XClassHint ch;
    g_last_x_error = 0;
    if (XGetClassHint(dpy, win, &ch) && g_last_x_error == 0) {
        const char *src = ch.res_class ? ch.res_class : (ch.res_name ? ch.res_name : "");
        size_t i = 0;
        for (; i + 1 < sizeof(out->wm_class) && src[i]; i++) {
            out->wm_class[i] = (char)tolower((unsigned char)src[i]);
        }
        out->wm_class[i] = '\0';
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    }
    if (is_wm_class_ignored(cfg, out->wm_class)) {
        return false;
    }

    /* 5. Geometry and extents */
    Window r_ret = None;
    int gx = 0, gy = 0;
    unsigned int gw = 0, gh = 0, bw = 0, depth = 0;
    g_last_x_error = 0;
    if (!XGetGeometry(dpy, win, &r_ret, &gx, &gy, &gw, &gh, &bw, &depth) || g_last_x_error != 0) {
        return false;
    }
    int abs_x = 0, abs_y = 0;
    Window child_ret = None;
    if (!XTranslateCoordinates(dpy, win, root, 0, 0, &abs_x, &abs_y, &child_ret) || g_last_x_error != 0) {
        return false;
    }
    out->client_rect = (Rect){abs_x, abs_y, (int)gw, (int)gh};
    out->frame_win = find_frame_window(dpy, root, win);

    long ext[4] = {0, 0, 0, 0};
    int en = 0;
    if (read_cardinals(dpy, win, atoms->net_frame_extents, ext, 4, &en) && en == 4) {
        out->net_frame = (Extents){(int)ext[0], (int)ext[1], (int)ext[2], (int)ext[3]};
        out->has_net_frame = (ext[0] > 0 || ext[1] > 0 || ext[2] > 0 || ext[3] > 0);
    }
    if (read_cardinals(dpy, win, atoms->gtk_frame_extents, ext, 4, &en) && en == 4) {
        out->gtk_frame = (Extents){(int)ext[0], (int)ext[1], (int)ext[2], (int)ext[3]};
        out->has_gtk_frame = true;
    }

    /* Compute visual frame_rect once based on extents */
    if (out->has_net_frame) {
        out->frame_rect = (Rect){
            abs_x - out->net_frame.left,
            abs_y - out->net_frame.top,
            (int)gw + out->net_frame.left + out->net_frame.right,
            (int)gh + out->net_frame.top + out->net_frame.bottom
        };
    } else if (out->has_gtk_frame && !out->is_maximized) {
        out->frame_rect = (Rect){
            abs_x + out->gtk_frame.left,
            abs_y + out->gtk_frame.top,
            (int)gw - out->gtk_frame.left - out->gtk_frame.right,
            (int)gh - out->gtk_frame.top - out->gtk_frame.bottom
        };
    } else {
        out->frame_rect = out->client_rect;
    }

    if (out->frame_rect.w < 120 || out->frame_rect.h < 60) {
        return false;
    }

    /* Tier 1: Direct _WIN11_MAX_BUTTON(CARDINAL[4]) = x, y, w, h relative to client window */
    long mb[4] = {0, 0, 0, 0};
    if (read_cardinals(dpy, win, atoms->win11_max_button, mb, 4, &en) && en == 4 &&
        mb[2] >= 12 && mb[2] <= 200 && mb[3] >= 12 && mb[3] <= 120) {
        out->kind = WIN_KIND_CUSTOM_CSD;
        out->has_win11_max_button_prop = true;
        out->wants_button_overlay = false;
        out->max_btn_rect = (Rect){
            abs_x + (int)mb[0],
            abs_y + (int)mb[1],
            (int)mb[2],
            (int)mb[3]
        };
        out->valid = true;
        return true;
    }

    /* Tier 2: Explicit [profile:<wm_class>], then SSD, then GTK_CSD, then fallback_custom_csd */
    ButtonGeometry bg = {0};
    const ClassProfile *prof = match_profile(cfg, out->wm_class);
    bool is_ssd = (out->has_net_frame && out->net_frame.top >= 16);
    if (prof) {
        if (!prof->enabled) return false;
        out->kind = is_ssd ? WIN_KIND_SSD : WIN_KIND_CUSTOM_CSD;
        bg = prof->geom;
        out->wants_button_overlay = (out->kind == WIN_KIND_CUSTOM_CSD) &&
                                    cfg->decorate_custom_csd && prof->overlay_buttons;
    } else if (is_ssd) {
        out->kind = WIN_KIND_SSD;
        bg = cfg->ssd;
        out->wants_button_overlay = false;
    } else if (out->has_gtk_frame && !is_chromium_or_electron_csd(dpy, win, atoms)) {
        out->kind = WIN_KIND_GTK_CSD;
        bg = cfg->csd_gtk3;
        out->wants_button_overlay = false;
    } else if (cfg->fallback_custom_csd) {
        out->kind = WIN_KIND_CUSTOM_CSD;
        bg = cfg->custom_csd;
        out->wants_button_overlay = cfg->decorate_custom_csd;
    } else {
        return false;
    }
    out->applied_geom = bg;

    int max_idx = x11_find_maximize_index_from_right(cfg->button_layout);
    if (max_idx < 0) return false;

    int s = cfg->dpi_scale < 1 ? 1 : cfg->dpi_scale;
    int bw_px = bg.button_width * s;
    int bh_px = bg.button_height * s;
    int gap_px = bg.button_gap * s;
    int right_px = bg.right_offset * s;
    int top_px = bg.top_offset * s;

    int btn_right = out->frame_rect.x + out->frame_rect.w - right_px
                  - max_idx * (bw_px + gap_px);
    out->max_btn_rect = (Rect){
        btn_right - bw_px,
        out->frame_rect.y + top_px,
        bw_px,
        bh_px
    };
    out->valid = true;
    return true;
}

void x11_select_window_events(Display *dpy, Window win, long mask) {
    if (win == None) return;
    g_last_x_error = 0;
    XSelectInput(dpy, win, mask);
    XSync(dpy, False);
    g_last_x_error = 0;
}

bool x11_query_pointer(Display *dpy, Window root, int *out_x, int *out_y,
                       unsigned int *out_mask, Window *out_child) {
    Window r_ret = None, c_ret = None;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    g_last_x_error = 0;
    Bool ok = XQueryPointer(dpy, root, &r_ret, &c_ret, &rx, &ry, &wx, &wy, &mask);
    if (!ok || g_last_x_error != 0) return false;
    if (out_x) *out_x = rx;
    if (out_y) *out_y = ry;
    if (out_mask) *out_mask = mask;
    if (out_child) *out_child = c_ret;
    return true;
}
