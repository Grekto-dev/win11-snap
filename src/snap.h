#ifndef WIN11_SNAP_SNAP_H
#define WIN11_SNAP_SNAP_H

#include "common.h"
#include "x11_state.h"

WorkArea snap_pick_workarea(const WorkArea was[MAX_MONITORS], int count, Rect ref_rect);
bool snap_apply_rect(Display *dpy, Window root, const X11Atoms *atoms,
                     const AppConfig *cfg, WorkArea wa, Rect target,
                     Window target_win, bool debug);
bool snap_apply(Display *dpy, Window root, const X11Atoms *atoms,
                const AppConfig *cfg, const WorkArea was[MAX_MONITORS], int num_wa,
                const SnapLayout layouts[MAX_LAYOUTS],
                Window target_win, int layout_idx, int zone_idx, bool debug);
void snap_set_maximized(Display *dpy, Window root, const X11Atoms *atoms, Window win, bool maximize);
void snap_minimize(Display *dpy, Window win);

#endif
