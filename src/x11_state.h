#ifndef WIN11_SNAP_X11_STATE_H
#define WIN11_SNAP_X11_STATE_H

#include "common.h"

typedef struct {
    Atom net_active_window;
    Atom net_current_desktop;
    Atom net_workarea;
    Atom net_wm_window_type;
    Atom net_wm_window_type_normal;
    Atom net_wm_window_type_dialog;
    Atom net_wm_window_type_desktop;
    Atom net_wm_window_type_dock;
    Atom net_wm_window_type_toolbar;
    Atom net_wm_window_type_menu;
    Atom net_wm_window_type_utility;
    Atom net_wm_window_type_splash;
    Atom net_wm_window_type_dropdown_menu;
    Atom net_wm_window_type_popup_menu;
    Atom net_wm_window_type_tooltip;
    Atom net_wm_window_type_notification;
    Atom net_wm_state;
    Atom net_wm_state_maximized_horz;
    Atom net_wm_state_maximized_vert;
    Atom net_wm_state_fullscreen;
    Atom net_wm_state_above;
    Atom net_wm_allowed_actions;
    Atom net_wm_action_maximize_horz;
    Atom net_wm_action_maximize_vert;
    Atom net_frame_extents;
    Atom gtk_frame_extents;
    Atom motif_wm_hints;
    Atom win11_max_button;
    Atom wm_window_role;
    Atom gtk_hide_titlebar_when_maximized;
    Atom gtk_application_id;
    Atom net_moveresize_window;
    Atom net_close_window;
    Atom net_wm_strut_partial;
    Atom net_client_list;
    Atom net_client_list_stacking;
    Atom net_wm_name;
    Atom net_wm_icon;
    Atom net_wm_desktop;
    Atom net_wm_state_hidden;
    Atom net_wm_state_skip_taskbar;
    Atom utf8_string;
    Atom resource_manager;
} X11Atoms;

void x11_install_error_handler(void);
int x11_take_last_error(void);
void x11_init_atoms(Display *dpy, X11Atoms *atoms);
int x11_get_dpi_scale(Display *dpy, Window root, const X11Atoms *atoms);
int x11_get_current_desktop(Display *dpy, Window root, const X11Atoms *atoms);
void x11_refresh_workareas(Display *dpy, Window root, const X11Atoms *atoms,
                           WorkArea out[MAX_MONITORS], int *out_count);
Window x11_get_active_window(Display *dpy, Window root, const X11Atoms *atoms);
int x11_find_maximize_index_from_right(const char *button_layout);
bool x11_read_window_info(Display *dpy, Window root, const X11Atoms *atoms,
                          const AppConfig *cfg, Window win, WindowInfo *out);
void x11_select_window_events(Display *dpy, Window win, long mask);
bool x11_query_pointer(Display *dpy, Window root, int *out_x, int *out_y,
                       unsigned int *out_mask, Window *out_child);

#endif
