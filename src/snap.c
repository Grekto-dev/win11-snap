#include "snap.h"
#include "layouts.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <poll.h>
#include <stdio.h>
#include <time.h>

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

WorkArea snap_pick_workarea(const WorkArea was[MAX_MONITORS], int count, Rect ref_rect) {
    if (count <= 0) {
        return (WorkArea){0, 0, 1920, 1080};
    }
    int cx = ref_rect.x + ref_rect.w / 2;
    int cy = ref_rect.y + ref_rect.h / 2;
    for (int i = 0; i < count; i++) {
        if (cx >= was[i].x && cx < was[i].x + was[i].w &&
            cy >= was[i].y && cy < was[i].y + was[i].h) {
            return was[i];
        }
    }
    long best_area = -1;
    int best_idx = 0;
    for (int i = 0; i < count; i++) {
        int ix0 = ref_rect.x > was[i].x ? ref_rect.x : was[i].x;
        int iy0 = ref_rect.y > was[i].y ? ref_rect.y : was[i].y;
        int ix1 = (ref_rect.x + ref_rect.w) < (was[i].x + was[i].w) ? (ref_rect.x + ref_rect.w) : (was[i].x + was[i].w);
        int iy1 = (ref_rect.y + ref_rect.h) < (was[i].y + was[i].h) ? (ref_rect.y + ref_rect.h) : (was[i].y + was[i].h);
        long area = (ix1 > ix0 && iy1 > iy0) ? (long)(ix1 - ix0) * (long)(iy1 - iy0) : 0;
        if (area > best_area) {
            best_area = area;
            best_idx = i;
        }
    }
    return was[best_idx];
}

static bool window_has_maximized_state(Display *dpy, Window win, const X11Atoms *atoms) {
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, atoms->net_wm_state, 0, 32, False, XA_ATOM,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) != Success ||
        !data) {
        if (data) XFree(data);
        return false;
    }
    bool max = false;
    Atom *st = (Atom *)data;
    for (unsigned long i = 0; i < nitems; i++) {
        if (st[i] == atoms->net_wm_state_maximized_horz ||
            st[i] == atoms->net_wm_state_maximized_vert) {
            max = true;
            break;
        }
    }
    XFree(data);
    return max;
}

static void send_unmaximize(Display *dpy, Window root, Window win, const X11Atoms *atoms) {
    XEvent ev = {0};
    ev.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = atoms->net_wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 0; /* _NET_WM_STATE_REMOVE */
    ev.xclient.data.l[1] = (long)atoms->net_wm_state_maximized_horz;
    ev.xclient.data.l[2] = (long)atoms->net_wm_state_maximized_vert;
    ev.xclient.data.l[3] = 2; /* source: pager/utility */
    ev.xclient.data.l[4] = 0;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

static void send_moveresize(Display *dpy, Window root, Window win, const X11Atoms *atoms,
                            int x, int y, int w, int h) {
    XEvent ev = {0};
    ev.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = atoms->net_moveresize_window;
    ev.xclient.format = 32;
    /* NorthWestGravity (1) | x(1<<8) | y(1<<9) | w(1<<10) | h(1<<11) | source=2 (2<<12) */
    ev.xclient.data.l[0] = (2L << 12) | (1L << 8) | (1L << 9) | (1L << 10) | (1L << 11) | 1L;
    ev.xclient.data.l[1] = x;
    ev.xclient.data.l[2] = y;
    ev.xclient.data.l[3] = w;
    ev.xclient.data.l[4] = h;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XSync(dpy, False);
}

static bool window_has_gtk_extents(Display *dpy, Window win, const X11Atoms *atoms) {
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, atoms->gtk_frame_extents, 0, 4, False, AnyPropertyType,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) != Success ||
        !data) {
        if (data) XFree(data);
        return false;
    }
    XFree(data);
    return nitems >= 4;
}

static void wait_unmaximize_settle(Display *dpy, Window win, const X11Atoms *atoms,
                                   bool expect_gtk_extents, int max_wait_ms) {
    uint64_t deadline = monotonic_ms() + (uint64_t)max_wait_ms;
    for (;;) {
        if (!window_has_maximized_state(dpy, win, atoms)) {
            if (!expect_gtk_extents || window_has_gtk_extents(dpy, win, atoms)) {
                struct timespec ts = {0, 12 * 1000 * 1000};
                nanosleep(&ts, NULL);
                break;
            }
        }
        uint64_t now = monotonic_ms();
        if (now >= deadline) break;
        struct timespec ts = {0, 12 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

bool snap_apply_rect(Display *dpy, Window root, const X11Atoms *atoms,
                     const AppConfig *cfg, WorkArea wa, Rect target,
                     Window target_win, bool debug) {
    WindowInfo info;
    if (!x11_read_window_info(dpy, root, atoms, cfg, target_win, &info)) {
        if (debug) {
            fprintf(stderr, "[snap] window 0x%lx rejected or vanished\n", (unsigned long)target_win);
        }
        return false;
    }

    bool was_max = window_has_maximized_state(dpy, target_win, atoms);
    if (was_max) {
        if (debug) fprintf(stderr, "[snap] unmaximizing 0x%lx first...\n", (unsigned long)target_win);
        send_unmaximize(dpy, root, target_win, atoms);
        wait_unmaximize_settle(dpy, target_win, atoms, !info.has_net_frame, 180);
        WindowInfo updated;
        if (x11_read_window_info(dpy, root, atoms, cfg, target_win, &updated)) {
            info = updated;
        }
    }

    int req_x = target.x;
    int req_y = target.y;
    int req_w = target.w;
    int req_h = target.h;

    if (info.has_net_frame) {
        req_w -= (info.net_frame.left + info.net_frame.right);
        req_h -= (info.net_frame.top + info.net_frame.bottom);
    } else if (info.has_gtk_frame) {
        req_x -= info.gtk_frame.left;
        req_y -= info.gtk_frame.top;
        req_w += (info.gtk_frame.left + info.gtk_frame.right);
        req_h += (info.gtk_frame.top + info.gtk_frame.bottom);
    }

    XSizeHints sh;
    long sup = 0;
    if (XGetWMNormalHints(dpy, target_win, &sh, &sup) && (sh.flags & PMinSize)) {
        if (req_w < sh.min_width) req_w = sh.min_width;
        if (req_h < sh.min_height) req_h = sh.min_height;
    }
    if (req_w < 80) req_w = 80;
    if (req_h < 60) req_h = 60;

    if (debug) {
        fprintf(stderr,
                "[snap] 0x%lx kind=%d wa=(%d,%d,%d,%d) target=(%d,%d,%d,%d) req=(%d,%d,%d,%d)\n",
                (unsigned long)target_win, (int)info.kind,
                wa.x, wa.y, wa.w, wa.h,
                target.x, target.y, target.w, target.h,
                req_x, req_y, req_w, req_h);
    }

    send_moveresize(dpy, root, target_win, atoms, req_x, req_y, req_w, req_h);

    if (was_max) {
        struct timespec ts = {0, 40 * 1000 * 1000};
        nanosleep(&ts, NULL);
        WindowInfo after;
        if (x11_read_window_info(dpy, root, atoms, cfg, target_win, &after)) {
            int dx = after.frame_rect.x - target.x;
            int dy = after.frame_rect.y - target.y;
            int dw = after.frame_rect.w - target.w;
            int dh = after.frame_rect.h - target.h;
            if (dx < -16 || dx > 16 || dy < -16 || dy > 16 || dw < -36 || dw > 36 || dh < -36 || dh > 36) {
                if (after.has_net_frame) {
                    req_x = target.x;
                    req_y = target.y;
                    req_w = target.w - after.net_frame.left - after.net_frame.right;
                    req_h = target.h - after.net_frame.top - after.net_frame.bottom;
                } else if (after.has_gtk_frame) {
                    req_x = target.x - after.gtk_frame.left;
                    req_y = target.y - after.gtk_frame.top;
                    req_w = target.w + after.gtk_frame.left + after.gtk_frame.right;
                    req_h = target.h + after.gtk_frame.top + after.gtk_frame.bottom;
                }
                if (debug) fprintf(stderr, "[snap] re-applying after unmaximize settle\n");
                send_moveresize(dpy, root, target_win, atoms, req_x, req_y, req_w, req_h);
            }
        }
    }

    return true;
}

bool snap_apply(Display *dpy, Window root, const X11Atoms *atoms,
                const AppConfig *cfg, const WorkArea was[MAX_MONITORS], int num_wa,
                const SnapLayout layouts[MAX_LAYOUTS],
                Window target_win, int layout_idx, int zone_idx, bool debug) {
    if (layout_idx < 0 || layout_idx >= MAX_LAYOUTS) return false;
    const SnapLayout *lay = &layouts[layout_idx];
    if (zone_idx < 0 || zone_idx >= lay->num_zones) return false;

    WindowInfo info;
    if (!x11_read_window_info(dpy, root, atoms, cfg, target_win, &info)) {
        return false;
    }

    WorkArea wa = snap_pick_workarea(was, num_wa, info.max_btn_rect);
    Rect target = layouts_compute_zone_rect(wa, lay->zones[zone_idx], cfg->gap_px);
    return snap_apply_rect(dpy, root, atoms, cfg, wa, target, target_win, debug);
}

void snap_set_maximized(Display *dpy, Window root, const X11Atoms *atoms, Window win, bool maximize) {
    XEvent ev = {0};
    ev.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = atoms->net_wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = maximize ? 1L : 0L;
    ev.xclient.data.l[1] = (long)atoms->net_wm_state_maximized_horz;
    ev.xclient.data.l[2] = (long)atoms->net_wm_state_maximized_vert;
    ev.xclient.data.l[3] = 2;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

void snap_minimize(Display *dpy, Window win) {
    XIconifyWindow(dpy, win, DefaultScreen(dpy));
    XFlush(dpy);
}
