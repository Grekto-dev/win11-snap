#define _DEFAULT_SOURCE
#include "assist.h"
#include "common.h"
#include "config.h"
#include "csd_decor.h"
#include "layouts.h"
#include "popup.h"
#include "snap.h"
#include "x11_state.h"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <malloc.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload_config = 0;

static void on_signal(int sig) {
    if (sig == SIGUSR1) {
        g_reload_config = 1;
        return;
    }
    g_running = 0;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n"
           "  --settings               Open Win11 Snap Settings GUI\n"
           "  --debug                  Log state transitions and geometry\n"
           "  --config <path>          Explicit config.ini path\n"
           "  --dump-windows           List all open client windows, their CSD/SSD kind, and max_btn_rect\n"
           "  --learn-window           Interactive 2-click calibrator for custom CSD apps\n"
           "  --test-snap <L>:<Z>      Snap active (or --window) window to layout L zone Z and exit\n"
           "  --window <wid>           Hex or decimal window ID for --test-snap\n"
           "  --show-popup-at <X>,<Y>  Show popup at screen X,Y (for visual inspection) and exit\n"
           "  --hover <L>:<Z>          Pre-highlight zone L:Z with --show-popup-at\n"
           "  --duration <ms>          Display duration for --show-popup-at (default 3500 ms)\n"
           "  --help                   Show this help\n",
           prog);
}

enum {
    ACT_SNAP_LAYOUTS = 0,
    ACT_SNAP_LEFT,
    ACT_SNAP_RIGHT,
    ACT_SNAP_UP,
    ACT_SNAP_DOWN,
    ACT_SNAP_TOP_HALF,
    ACT_SNAP_BOTTOM_HALF,
    ACT_COUNT
};

typedef struct {
    KeyCode keycode;
    unsigned int mods;
    bool valid;
} AccelBinding;

typedef struct {
    Display *dpy;
    Window root;
    const char *explicit_cfg;
    X11Atoms atoms;
    AppConfig cfg;
    SnapLayout layouts[MAX_LAYOUTS];
    WorkArea workareas[MAX_MONITORS];
    int num_workareas;
    PopupWindow popup;
    SnapAssist assist;
    CsdDecorManager csd_decor;
    WindowInfo active;
    Window sub_win;
    Window sub_frame;

    DaemonState state;
    bool opened_by_hotkey;
    uint64_t arm_deadline;
    uint64_t leave_deadline;
    uint64_t last_motion_check;
    bool motion_pending;
    uint64_t motion_flush_at;
    int csd_resync_remaining;
    uint64_t csd_resync_at;

    /* Drag-to-Snap & Top Snap Bar state */
    bool btn1_down;
    bool is_dragging_window;
    Window drag_win;
    Rect drag_start_rect;
    int drag_press_x;
    int drag_press_y;
    bool drag_top_bar_open;
    int drag_target_layout;
    int drag_target_zone;
    bool drag_target_max;
    bool drag_drop_pending;
    uint64_t drag_drop_at;

    int xi_opcode;
    int rr_event_base;
    int rr_error_base;
    bool has_rr;
    KeyCode escape_keycode;
    KeyCode return_keycode;
    KeyCode kp_enter_keycode;
    KeyCode arrow_keycodes[4]; /* Left, Right, Up, Down */
    KeyCode num_keycodes[6];
    AccelBinding bindings[ACT_COUNT];
    int last_hotkey_act;
    uint64_t last_hotkey_at;
    bool debug;
} DaemonCtx;

static const char *kind_to_str(const WindowInfo *info) {
    if (info->kind == WIN_KIND_SSD) return "SSD";
    if (info->kind == WIN_KIND_GTK_CSD) return "GTK_CSD";
    if (info->kind == WIN_KIND_CUSTOM_CSD) {
        return info->has_win11_max_button_prop ? "CUSTOM_CSD(_WIN11_MAX_BUTTON)" : "CUSTOM_CSD(profile/fallback)";
    }
    return "NONE";
}

static void update_subscriptions(DaemonCtx *ctx, Window new_win, Window new_frame) {
    ctx->sub_win = new_win;
    ctx->sub_frame = new_frame;
    if (new_win != None && new_win != ctx->root) {
        x11_select_window_events(ctx->dpy, new_win, StructureNotifyMask | PropertyChangeMask);
    }
    if (new_frame != None && new_frame != ctx->root && new_frame != new_win) {
        x11_select_window_events(ctx->dpy, new_frame, StructureNotifyMask);
    }
}

static void refresh_active(DaemonCtx *ctx) {
    Window aw = x11_get_active_window(ctx->dpy, ctx->root, &ctx->atoms);
    WindowInfo info;
    if (aw != None && x11_read_window_info(ctx->dpy, ctx->root, &ctx->atoms, &ctx->cfg, aw, &info)) {
        ctx->active = info;
        update_subscriptions(ctx, info.win, info.frame_win);
        if (ctx->debug) {
            fprintf(stderr,
                    "[active] win=0x%lx frame=0x%lx kind=%s class='%s' frame_rect=(%d,%d,%d,%d) max_btn=(%d,%d,%d,%d)\n",
                    (unsigned long)info.win, (unsigned long)info.frame_win, kind_to_str(&info),
                    info.wm_class,
                    info.frame_rect.x, info.frame_rect.y, info.frame_rect.w, info.frame_rect.h,
                    info.max_btn_rect.x, info.max_btn_rect.y, info.max_btn_rect.w, info.max_btn_rect.h);
        }
    } else {
        memset(&ctx->active, 0, sizeof(ctx->active));
        if (aw != None && aw != ctx->root) {
            update_subscriptions(ctx, aw, None);
        } else {
            update_subscriptions(ctx, None, None);
        }
        if (ctx->debug) {
            fprintf(stderr, "[active] win=0x%lx ignored (not maximizable / ignored / non-normal)\n",
                    (unsigned long)aw);
        }
    }
}

static void schedule_csd_resync(DaemonCtx *ctx, uint64_t now) {
    ctx->csd_resync_remaining = 3;
    ctx->csd_resync_at = now + 60;
}

static void transition_idle(DaemonCtx *ctx, const char *reason) {
    if (ctx->state == STATE_POPUP_VISIBLE || ctx->popup.visible) {
        popup_hide(&ctx->popup);
    }
    if (ctx->assist.visible) {
        assist_hide(&ctx->assist);
    }
    if (ctx->debug && ctx->state != STATE_IDLE) {
        fprintf(stderr, "[state] -> IDLE (%s)\n", reason);
    }
    ctx->state = STATE_IDLE;
    ctx->opened_by_hotkey = false;
    ctx->arm_deadline = 0;
    ctx->leave_deadline = 0;
}

static void sync_popup_zone_preview(DaemonCtx *ctx, uint64_t now) {
    if (ctx->state != STATE_POPUP_VISIBLE || !ctx->active.valid) return;
    if (!ctx->cfg.enable_animations) {
        assist_hide(&ctx->assist);
        return;
    }
    int hl = ctx->popup.hovered_layout;
    int hz = ctx->popup.hovered_zone;
    WorkArea wa = snap_pick_workarea(ctx->workareas, ctx->num_workareas, ctx->active.max_btn_rect);
    if (hl >= 0 && hl < MAX_LAYOUTS && hz >= 0 && hz < ctx->layouts[hl].num_zones) {
        Rect zr = layouts_compute_zone_rect(wa, ctx->layouts[hl].zones[hz], ctx->cfg.gap_px);
        assist_update_zone_preview(&ctx->assist, wa, &zr, ctx->popup.win, now);
    } else {
        assist_update_zone_preview(&ctx->assist, wa, NULL, ctx->popup.win, now);
    }
}

static void execute_snap_and_assist(DaemonCtx *ctx, Window target, int lay, int zone, uint64_t now) {
    if (target == None || lay < 0 || lay >= MAX_LAYOUTS ||
        zone < 0 || zone >= ctx->layouts[lay].num_zones) {
        transition_idle(ctx, "invalid-snap-target");
        return;
    }

    WindowInfo before = ctx->active;
    if (before.win != target || !before.valid) {
        x11_read_window_info(ctx->dpy, ctx->root, &ctx->atoms, &ctx->cfg, target, &before);
    }
    WorkArea wa = snap_pick_workarea(ctx->workareas, ctx->num_workareas,
                                     before.valid ? before.max_btn_rect : (Rect){0, 0, 100, 100});
    Rect target_rect = layouts_compute_zone_rect(wa, ctx->layouts[lay].zones[zone], ctx->cfg.gap_px);

    popup_hide(&ctx->popup);
    assist_hide(&ctx->assist);

    cairo_surface_t *before_thumb = (before.valid && ctx->cfg.enable_animations)
        ? assist_capture_window_thumb(ctx->dpy, &before, 480, 320)
        : NULL;

    /* Apply snap and run smooth 60 FPS cubic-bezier morph animation into the chosen zone */
    snap_apply(ctx->dpy, ctx->root, &ctx->atoms, &ctx->cfg,
               ctx->workareas, ctx->num_workareas, ctx->layouts,
               target, lay, zone, ctx->debug);
    if (before.valid && ctx->cfg.enable_animations) {
        assist_animate_snap_transition(&ctx->assist, target, before.frame_win,
                                       before.frame_rect, target_rect, before_thumb);
        assist_hide(&ctx->assist);
    }
    if (before_thumb) {
        cairo_surface_destroy(before_thumb);
    }

    refresh_active(ctx);
    csd_decor_sync_all(&ctx->csd_decor, &ctx->atoms, &ctx->cfg,
                       ctx->active.win, true, ctx->debug);
    schedule_csd_resync(ctx, now_ms());

    /* Open Windows 11 Snap Assist in the remaining empty quadrant(s) of the chosen layout */
    if (ctx->cfg.enable_snap_assist &&
        assist_start_session(&ctx->assist, &ctx->atoms, &ctx->cfg,
                             ctx->workareas, ctx->num_workareas, ctx->layouts,
                             lay, zone, target, now_ms(), ctx->debug)) {
        ctx->state = STATE_SNAP_ASSIST;
        ctx->opened_by_hotkey = false;
        ctx->arm_deadline = 0;
        ctx->leave_deadline = 0;
        if (ctx->debug) {
            fprintf(stderr, "[state] -> SNAP_ASSIST layout=%s first_zone=%d\n",
                    ctx->layouts[lay].name, zone);
        }
    } else {
        transition_idle(ctx, "snap-completed-no-assist");
    }
    (void)now;
}

static void execute_vertical_half_snap(DaemonCtx *ctx, Window target, bool top_half) {
    if (target == None) return;
    WindowInfo before;
    if (!x11_read_window_info(ctx->dpy, ctx->root, &ctx->atoms, &ctx->cfg, target, &before) || !before.valid) {
        return;
    }
    WorkArea wa = snap_pick_workarea(ctx->workareas, ctx->num_workareas, before.frame_rect);
    int g = ctx->cfg.gap_px;
    int h_half = (wa.h - 3 * g) / 2;
    Rect halves[2] = {
        {wa.x + g, wa.y + g, wa.w - 2 * g, h_half},
        {wa.x + g, wa.y + 2 * g + h_half, wa.w - 2 * g, wa.h - 3 * g - h_half}
    };
    int zone = top_half ? 0 : 1;

    popup_hide(&ctx->popup);
    assist_hide(&ctx->assist);
    cairo_surface_t *before_thumb = ctx->cfg.enable_animations
        ? assist_capture_window_thumb(ctx->dpy, &before, 480, 320) : NULL;

    snap_apply_rect(ctx->dpy, ctx->root, &ctx->atoms, &ctx->cfg, wa, halves[zone], target, ctx->debug);
    if (ctx->cfg.enable_animations) {
        assist_animate_snap_transition(&ctx->assist, target, before.frame_win,
                                       before.frame_rect, halves[zone], before_thumb);
        assist_hide(&ctx->assist);
    }
    if (before_thumb) cairo_surface_destroy(before_thumb);

    refresh_active(ctx);
    csd_decor_sync_all(&ctx->csd_decor, &ctx->atoms, &ctx->cfg, ctx->active.win, true, ctx->debug);
    schedule_csd_resync(ctx, now_ms());

    if (ctx->cfg.enable_snap_assist &&
        assist_start_custom_layout(&ctx->assist, &ctx->atoms, &ctx->cfg,
                                   ctx->workareas, ctx->num_workareas,
                                   halves, 2, zone, target, now_ms(), ctx->debug)) {
        ctx->state = STATE_SNAP_ASSIST;
    } else {
        transition_idle(ctx, "vertical-half-done");
    }
}

static bool rect_approx_eq(Rect a, Rect b) {
    return abs(a.x - b.x) <= 24 && abs(a.y - b.y) <= 24 &&
           abs(a.w - b.w) <= 36 && abs(a.h - b.h) <= 140;
}

static void handle_directional_snap(DaemonCtx *ctx, int action) {
    refresh_active(ctx);
    if (!ctx->active.valid) return;
    Window target = ctx->active.win;
    WorkArea wa = snap_pick_workarea(ctx->workareas, ctx->num_workareas, ctx->active.frame_rect);

    Rect left_half  = layouts_compute_zone_rect(wa, ctx->layouts[0].zones[0], ctx->cfg.gap_px);
    Rect right_half = layouts_compute_zone_rect(wa, ctx->layouts[0].zones[1], ctx->cfg.gap_px);
    Rect q_tl = layouts_compute_zone_rect(wa, ctx->layouts[4].zones[0], ctx->cfg.gap_px);
    Rect q_tr = layouts_compute_zone_rect(wa, ctx->layouts[4].zones[1], ctx->cfg.gap_px);
    Rect q_bl = layouts_compute_zone_rect(wa, ctx->layouts[4].zones[2], ctx->cfg.gap_px);
    Rect q_br = layouts_compute_zone_rect(wa, ctx->layouts[4].zones[3], ctx->cfg.gap_px);

    bool is_max = ctx->active.is_maximized;
    bool in_l = !is_max && rect_approx_eq(ctx->active.frame_rect, left_half);
    bool in_r = !is_max && rect_approx_eq(ctx->active.frame_rect, right_half);
    bool in_tl = !is_max && rect_approx_eq(ctx->active.frame_rect, q_tl);
    bool in_tr = !is_max && rect_approx_eq(ctx->active.frame_rect, q_tr);
    bool in_bl = !is_max && rect_approx_eq(ctx->active.frame_rect, q_bl);
    bool in_br = !is_max && rect_approx_eq(ctx->active.frame_rect, q_br);

    uint64_t now = now_ms();
    if (action == ACT_SNAP_LEFT) {
        if (in_tr) execute_snap_and_assist(ctx, target, 4, 0, now);
        else if (in_br) execute_snap_and_assist(ctx, target, 4, 2, now);
        else execute_snap_and_assist(ctx, target, 0, 0, now);
    } else if (action == ACT_SNAP_RIGHT) {
        if (in_tl) execute_snap_and_assist(ctx, target, 4, 1, now);
        else if (in_bl) execute_snap_and_assist(ctx, target, 4, 3, now);
        else execute_snap_and_assist(ctx, target, 0, 1, now);
    } else if (action == ACT_SNAP_UP) {
        if (in_l) execute_snap_and_assist(ctx, target, 4, 0, now);
        else if (in_r) execute_snap_and_assist(ctx, target, 4, 1, now);
        else if (in_bl) execute_snap_and_assist(ctx, target, 0, 0, now);
        else if (in_br) execute_snap_and_assist(ctx, target, 0, 1, now);
        else {
            transition_idle(ctx, "hotkey-maximize");
            snap_set_maximized(ctx->dpy, ctx->root, &ctx->atoms, target, true);
            schedule_csd_resync(ctx, now);
        }
    } else if (action == ACT_SNAP_DOWN) {
        if (is_max) {
            transition_idle(ctx, "hotkey-restore");
            snap_set_maximized(ctx->dpy, ctx->root, &ctx->atoms, target, false);
            schedule_csd_resync(ctx, now);
        } else if (in_l) {
            execute_snap_and_assist(ctx, target, 4, 2, now);
        } else if (in_r) {
            execute_snap_and_assist(ctx, target, 4, 3, now);
        } else if (in_tl) {
            execute_snap_and_assist(ctx, target, 0, 0, now);
        } else if (in_tr) {
            execute_snap_and_assist(ctx, target, 0, 1, now);
        } else {
            transition_idle(ctx, "hotkey-minimize");
            snap_minimize(ctx->dpy, target);
        }
    } else if (action == ACT_SNAP_TOP_HALF) {
        execute_vertical_half_snap(ctx, target, true);
    } else if (action == ACT_SNAP_BOTTOM_HALF) {
        execute_vertical_half_snap(ctx, target, false);
    }
}

static bool find_hovered_max_button_window(DaemonCtx *ctx, Window root_child, int px, int py, WindowInfo *out) {
    if (root_child == None || root_child == ctx->popup.win || root_child == ctx->assist.win) {
        return false;
    }

    /* Fast spatial pre-filter (1 X11 call): Maximize buttons always live near the top bar of root_child */
    Window r_ret = None;
    int gx = 0, gy = 0;
    unsigned int gw = 0, gh = 0, bw = 0, depth = 0;
    x11_take_last_error();
    if (!XGetGeometry(ctx->dpy, root_child, &r_ret, &gx, &gy, &gw, &gh, &bw, &depth) ||
        x11_take_last_error() != 0 || gw < 120 || gh < 60) {
        return false;
    }
    if (px < gx + (int)gw - 260 || px >= gx + (int)gw || py < gy || py >= gy + 110) {
        return false;
    }

    WindowInfo info;
    if (x11_read_window_info(ctx->dpy, ctx->root, &ctx->atoms, &ctx->cfg, root_child, &info) &&
        info.valid && rect_contains(info.max_btn_rect, px, py)) {
        *out = info;
        return true;
    }

    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    x11_take_last_error();
    if (XGetWindowProperty(ctx->dpy, ctx->root, ctx->atoms.net_client_list, 0, 256, False, XA_WINDOW,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) != Success || !data) {
        return false;
    }
    Window *list = (Window *)data;
    bool found = false;
    for (unsigned long i = 0; i < nitems; i++) {
        if (x11_read_window_info(ctx->dpy, ctx->root, &ctx->atoms, &ctx->cfg, list[i], &info) &&
            info.valid && (info.win == root_child || info.frame_win == root_child) &&
            rect_contains(info.max_btn_rect, px, py)) {
            *out = info;
            found = true;
            break;
        }
    }
    XFree(data);
    return found;
}

static void finish_window_drag(DaemonCtx *ctx, uint64_t now) {
    ctx->btn1_down = false;
    if (!ctx->is_dragging_window) return;
    ctx->is_dragging_window = false;
    ctx->drag_top_bar_open = false;
    assist_hide(&ctx->assist);
    if (ctx->drag_target_layout >= 0 || ctx->drag_target_max ||
        (ctx->cfg.drag_snap && ctx->cfg.enable_snap_assist)) {
        ctx->drag_drop_pending = true;
        ctx->drag_drop_at = now + (ctx->drag_target_layout >= 0 || ctx->drag_target_max ? 45 : 60);
    }
    if (ctx->debug) {
        fprintf(stderr, "[drag] finish_window_drag layout=%d zone=%d max=%d pending=%d\n",
                ctx->drag_target_layout, ctx->drag_target_zone,
                ctx->drag_target_max ? 1 : 0, ctx->drag_drop_pending ? 1 : 0);
    }
}

static void evaluate_pointer(DaemonCtx *ctx, uint64_t now) {
    ctx->last_motion_check = now;
    ctx->motion_pending = false;

    int px = 0, py = 0;
    unsigned int mask = 0;
    Window child = None;
    if (!x11_query_pointer(ctx->dpy, ctx->root, &px, &py, &mask, &child)) {
        return;
    }
    bool btn_down = (mask & (Button1Mask | Button2Mask | Button3Mask | Button4Mask | Button5Mask)) != 0;

    if (ctx->state == STATE_IDLE && ctx->is_dragging_window && ctx->cfg.drag_snap) {
        if ((mask & Button1Mask) == 0) {
            finish_window_drag(ctx, now);
            return;
        }
        WorkArea wa = snap_pick_workarea(ctx->workareas, ctx->num_workareas, (Rect){px, py, 1, 1});
        int bar_l = -1, bar_z = -1;
        int edge_px = 14;
        int corner_x = 120;
        int corner_y = 90;

        bool top_trigger = ctx->cfg.top_snap_bar &&
                           ((py <= wa.y + 8 && px >= wa.x + wa.w / 5 && px <= wa.x + (wa.w * 4) / 5) ||
                            (ctx->drag_top_bar_open && ctx->assist.top_bar_active &&
                             px >= wa.x + ctx->assist.top_bar_rect.x - 24 &&
                             px <= wa.x + ctx->assist.top_bar_rect.x + ctx->assist.top_bar_rect.w + 24 &&
                             py >= wa.y &&
                             py <= wa.y + ctx->assist.top_bar_rect.y + ctx->assist.top_bar_rect.h + 24));

        int edge_l = -1, edge_z = -1;
        bool edge_max = false;
        Rect phantom = {0};
        const Rect *phantom_ptr = NULL;

        if (!top_trigger) {
            bool near_left = (px <= wa.x + edge_px);
            bool near_right = (px >= wa.x + wa.w - edge_px);
            bool near_top = (py <= wa.y + edge_px);
            bool near_bot = (py >= wa.y + wa.h - edge_px);

            if ((near_left && py <= wa.y + corner_y) || (near_top && px <= wa.x + corner_x)) {
                edge_l = 4; edge_z = 0; /* Top-Left Quadrant */
            } else if ((near_right && py <= wa.y + corner_y) || (near_top && px >= wa.x + wa.w - corner_x)) {
                edge_l = 4; edge_z = 1; /* Top-Right Quadrant */
            } else if ((near_left && py >= wa.y + wa.h - corner_y) || (near_bot && px <= wa.x + corner_x)) {
                edge_l = 4; edge_z = 2; /* Bottom-Left Quadrant */
            } else if ((near_right && py >= wa.y + wa.h - corner_y) || (near_bot && px >= wa.x + wa.w - corner_x)) {
                edge_l = 4; edge_z = 3; /* Bottom-Right Quadrant */
            } else if (near_left) {
                edge_l = 0; edge_z = 0; /* Left Half */
            } else if (near_right) {
                edge_l = 0; edge_z = 1; /* Right Half */
            } else if (near_top && !ctx->cfg.top_snap_bar) {
                edge_max = true;
                phantom = (Rect){wa.x, wa.y, wa.w, wa.h};
                phantom_ptr = &phantom;
            }
            if (edge_l >= 0) {
                phantom = layouts_compute_zone_rect(wa, ctx->layouts[edge_l].zones[edge_z], ctx->cfg.gap_px);
                phantom_ptr = &phantom;
            }
        }

        ctx->drag_top_bar_open = top_trigger;
        assist_update_drag_preview(&ctx->assist, wa, top_trigger, px, py, phantom_ptr,
                                   ctx->layouts, &ctx->cfg, &bar_l, &bar_z, now);
        if (bar_l >= 0 && bar_z >= 0) {
            ctx->drag_target_layout = bar_l;
            ctx->drag_target_zone = bar_z;
            ctx->drag_target_max = false;
        } else if (edge_l >= 0 && edge_z >= 0) {
            ctx->drag_target_layout = edge_l;
            ctx->drag_target_zone = edge_z;
            ctx->drag_target_max = false;
        } else if (edge_max) {
            ctx->drag_target_layout = -1;
            ctx->drag_target_zone = -1;
            ctx->drag_target_max = true;
        } else {
            ctx->drag_target_layout = -1;
            ctx->drag_target_zone = -1;
            ctx->drag_target_max = false;
        }
        return;
    }

    if (ctx->state == STATE_IDLE) {
        if (!btn_down) {
            bool hit_active = ctx->active.valid &&
                              rect_contains(ctx->active.max_btn_rect, px, py) &&
                              (child == ctx->active.frame_win || child == ctx->active.win);
            if (!hit_active && child != None &&
                child != ctx->active.win && child != ctx->active.frame_win) {
                WindowInfo hovered_win;
                if (find_hovered_max_button_window(ctx, child, px, py, &hovered_win)) {
                    ctx->active = hovered_win;
                    update_subscriptions(ctx, hovered_win.win, hovered_win.frame_win);
                    hit_active = true;
                }
            }
            if (hit_active) {
                ctx->state = STATE_ARMED;
                ctx->arm_deadline = now + (uint64_t)ctx->cfg.hover_delay_ms;
                if (ctx->debug) {
                    fprintf(stderr, "[state] -> ARMED at (%d,%d) win=0x%lx (%s)\n",
                            px, py, (unsigned long)ctx->active.win, kind_to_str(&ctx->active));
                }
            }
        }
    } else if (ctx->state == STATE_ARMED) {
        if (!ctx->active.valid || btn_down ||
            !rect_contains(ctx->active.max_btn_rect, px, py) ||
            (child != ctx->active.frame_win && child != ctx->active.win)) {
            transition_idle(ctx, btn_down ? "button-down" : "left-max-btn");
        }
    } else if (ctx->state == STATE_POPUP_VISIBLE) {
        bool in_popup = (child == ctx->popup.win) || rect_contains(ctx->popup.card_rect, px, py);
        if (in_popup) {
            ctx->opened_by_hotkey = false;
            ctx->leave_deadline = 0;
            int lx = px - ctx->popup.card_rect.x + ctx->popup.shadow_pad;
            int ly = py - ctx->popup.card_rect.y + ctx->popup.shadow_pad;
            if (popup_update_hover(&ctx->popup, lx, ly, &ctx->cfg, ctx->layouts)) {
                sync_popup_zone_preview(ctx, now);
            }
        } else if (rect_contains(ctx->active.max_btn_rect, px, py) ||
                   rect_contains(ctx->popup.corridor_rect, px, py)) {
            ctx->leave_deadline = 0;
            if (popup_update_hover(&ctx->popup, -1, -1, &ctx->cfg, ctx->layouts)) {
                sync_popup_zone_preview(ctx, now);
            }
        } else if (!ctx->opened_by_hotkey) {
            if (popup_update_hover(&ctx->popup, -1, -1, &ctx->cfg, ctx->layouts)) {
                sync_popup_zone_preview(ctx, now);
            }
            if (ctx->leave_deadline == 0) {
                ctx->leave_deadline = now + (uint64_t)ctx->cfg.dismiss_grace_ms;
                if (ctx->debug) {
                    fprintf(stderr, "[popup] cursor left grace zone (%d,%d), arming dismiss\n", px, py);
                }
            }
        }
    } else if (ctx->state == STATE_SNAP_ASSIST && ctx->assist.visible) {
        int lx = px - ctx->assist.wa.x;
        int ly = py - ctx->assist.wa.y;
        assist_update_hover(&ctx->assist, lx, ly);
    }
}

static bool setup_xinput2(DaemonCtx *ctx) {
    int ev = 0, err = 0;
    if (!XQueryExtension(ctx->dpy, "XInputExtension", &ctx->xi_opcode, &ev, &err)) {
        fprintf(stderr, "Error: XInputExtension not available\n");
        return false;
    }
    /* Request XI 2.2+: XI 2.0 suppresses XI_Raw* during active WM grabs (Super_L / window drag),
     * whereas XI 2.1+ guarantees raw events are delivered regardless of grabs. */
    int major = 2, minor = 2;
    if (XIQueryVersion(ctx->dpy, &major, &minor) != Success) {
        fprintf(stderr, "Error: XInput 2.x not supported\n");
        return false;
    }
    unsigned char mask_bytes[XIMaskLen(XI_LASTEVENT)] = {0};
    XISetMask(mask_bytes, XI_RawMotion);
    XISetMask(mask_bytes, XI_RawButtonPress);
    XISetMask(mask_bytes, XI_RawButtonRelease);
    XISetMask(mask_bytes, XI_RawKeyPress);

    XIEventMask em = {
        .deviceid = XIAllMasterDevices,
        .mask_len = sizeof(mask_bytes),
        .mask = mask_bytes
    };
    XISelectEvents(ctx->dpy, ctx->root, &em, 1);
    return true;
}

static bool parse_accel_string(Display *dpy, const char *str, AccelBinding *out) {
    memset(out, 0, sizeof(*out));
    if (!str || !str[0]) return false;

    const char *p = str;
    unsigned int mods = 0;
    while (*p == '<') {
        const char *end = strchr(p, '>');
        if (!end) break;
        size_t len = (size_t)(end - p - 1);
        char tok[32];
        if (len >= sizeof(tok)) len = sizeof(tok) - 1;
        memcpy(tok, p + 1, len);
        tok[len] = '\0';
        if (strcasecmp(tok, "Super") == 0 || strcasecmp(tok, "Mod4") == 0) mods |= Mod4Mask;
        else if (strcasecmp(tok, "Alt") == 0 || strcasecmp(tok, "Mod1") == 0) mods |= Mod1Mask;
        else if (strcasecmp(tok, "Control") == 0 || strcasecmp(tok, "Ctrl") == 0) mods |= ControlMask;
        else if (strcasecmp(tok, "Shift") == 0) mods |= ShiftMask;
        p = end + 1;
    }
    if (!*p) return false;
    KeySym ks = XStringToKeysym(p);
    if (ks == NoSymbol && strlen(p) == 1) {
        char buf[2] = {(char)tolower((unsigned char)p[0]), '\0'};
        ks = XStringToKeysym(buf);
    }
    if (ks == NoSymbol) return false;
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    if (kc == 0) return false;

    out->keycode = kc;
    out->mods = mods;
    out->valid = true;
    return true;
}

static void ungrab_all_shortcuts(DaemonCtx *ctx) {
    unsigned int locks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    x11_take_last_error();
    for (int i = 0; i < ACT_COUNT; i++) {
        if (!ctx->bindings[i].valid) continue;
        if ((ctx->bindings[i].mods & Mod4Mask) == 0) {
            for (size_t m = 0; m < sizeof(locks) / sizeof(locks[0]); m++) {
                XUngrabKey(ctx->dpy, ctx->bindings[i].keycode, ctx->bindings[i].mods | locks[m], ctx->root);
            }
        }
        ctx->bindings[i].valid = false;
    }
    XSync(ctx->dpy, False);
    x11_take_last_error();
}

static void grab_all_shortcuts(DaemonCtx *ctx) {
    ungrab_all_shortcuts(ctx);
    if (!ctx->cfg.enable_shortcuts) return;

    const char *specs[ACT_COUNT] = {
        ctx->cfg.hotkey_super_z ? ctx->cfg.shortcuts.snap_layouts : "",
        ctx->cfg.shortcuts.snap_left,
        ctx->cfg.shortcuts.snap_right,
        ctx->cfg.shortcuts.snap_up,
        ctx->cfg.shortcuts.snap_down,
        ctx->cfg.shortcuts.snap_top_half,
        ctx->cfg.shortcuts.snap_bottom_half
    };
    unsigned int locks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    for (int i = 0; i < ACT_COUNT; i++) {
        if (!parse_accel_string(ctx->dpy, specs[i], &ctx->bindings[i])) continue;
        /* Only passively XGrabKey non-Super accelerators: Muffin's Super_L menu/overlay
         * interaction swallows core XGrabKey KeyPress delivery on root while Xorg also
         * suppresses XI_RawKeyPress to the grabber; Super combos match reliably via XI_RawKeyPress. */
        if ((ctx->bindings[i].mods & Mod4Mask) != 0) {
            if (ctx->debug) {
                fprintf(stderr, "[hotkey] Registered '%s' via XI_RawKeyPress (action=%d)\n", specs[i], i);
            }
            continue;
        }
        x11_take_last_error();
        for (size_t m = 0; m < sizeof(locks) / sizeof(locks[0]); m++) {
            XGrabKey(ctx->dpy, ctx->bindings[i].keycode, ctx->bindings[i].mods | locks[m],
                     ctx->root, False, GrabModeAsync, GrabModeAsync);
        }
        XSync(ctx->dpy, False);
        int err = x11_take_last_error();
        if (err != 0 && ctx->debug) {
            fprintf(stderr, "[hotkey] Warning: failed to grab '%s' (X11 error %d - held by another app/WM)\n",
                    specs[i], err);
        } else if (ctx->debug) {
            fprintf(stderr, "[hotkey] Registered '%s' (action=%d)\n", specs[i], i);
        }
    }
}

static int compute_poll_timeout(const DaemonCtx *ctx, uint64_t now) {
    int t = -1;
    if (ctx->is_dragging_window || (ctx->assist.visible && ctx->assist.anim_active)) {
        t = 16; /* 60 FPS smooth drag preview / release check or cubic-bezier animation tick */
    }
    if (ctx->drag_drop_pending) {
        int dt = (ctx->drag_drop_at > now) ? (int)(ctx->drag_drop_at - now) : 0;
        if (t < 0 || dt < t) t = dt;
    }
    if (ctx->motion_pending) {
        int dt = (ctx->motion_flush_at > now) ? (int)(ctx->motion_flush_at - now) : 0;
        if (t < 0 || dt < t) t = dt;
    }
    if (ctx->csd_resync_remaining > 0 && ctx->csd_resync_at > 0) {
        int dt = (ctx->csd_resync_at > now) ? (int)(ctx->csd_resync_at - now) : 0;
        if (t < 0 || dt < t) t = dt;
    }
    if (ctx->state == STATE_ARMED && ctx->arm_deadline > 0) {
        int dt = (ctx->arm_deadline > now) ? (int)(ctx->arm_deadline - now) : 0;
        if (t < 0 || dt < t) t = dt;
    }
    if (ctx->state == STATE_POPUP_VISIBLE && ctx->leave_deadline > 0) {
        int dt = (ctx->leave_deadline > now) ? (int)(ctx->leave_deadline - now) : 0;
        if (t < 0 || dt < t) t = dt;
    }
    return t;
}

static void toggle_popup_by_hotkey(DaemonCtx *ctx) {
    if (ctx->state == STATE_POPUP_VISIBLE) {
        transition_idle(ctx, "super-z-close");
        return;
    }
    refresh_active(ctx);
    if (!ctx->active.valid) return;
    WorkArea wa = snap_pick_workarea(ctx->workareas, ctx->num_workareas, ctx->active.max_btn_rect);
    popup_show(&ctx->popup, ctx->active.max_btn_rect, wa, &ctx->cfg, ctx->layouts);
    ctx->state = STATE_POPUP_VISIBLE;
    ctx->opened_by_hotkey = true;
    ctx->leave_deadline = 0;
    if (ctx->debug) {
        fprintf(stderr, "[state] -> POPUP_VISIBLE (Super+Z) win=0x%lx card=(%d,%d,%d,%d)\n",
                (unsigned long)ctx->active.win,
                ctx->popup.card_rect.x, ctx->popup.card_rect.y,
                ctx->popup.card_rect.w, ctx->popup.card_rect.h);
    }
}

static bool dispatch_shortcut_event(DaemonCtx *ctx, KeyCode kc, unsigned int clean_state, uint64_t now) {
    if (!ctx->cfg.enable_shortcuts) return false;
    for (int i = 0; i < ACT_COUNT; i++) {
        if (ctx->bindings[i].valid &&
            kc == ctx->bindings[i].keycode &&
            clean_state == ctx->bindings[i].mods) {
            if (i == ctx->last_hotkey_act && ctx->last_hotkey_at > 0 &&
                now - ctx->last_hotkey_at < 35) {
                return true;
            }
            ctx->last_hotkey_act = i;
            ctx->last_hotkey_at = now;
            if (ctx->debug) {
                fprintf(stderr, "[hotkey] Triggered action=%d kc=%u mods=0x%x\n",
                        i, (unsigned)kc, clean_state);
            }
            if (i == ACT_SNAP_LAYOUTS) {
                toggle_popup_by_hotkey(ctx);
            } else {
                handle_directional_snap(ctx, i);
            }
            return true;
        }
    }
    return false;
}

static void handle_x11_event(DaemonCtx *ctx, XEvent *ev, uint64_t now) {
    if (ev->type == KeyPress) {
        if (ev->xkey.window == ctx->root) {
            unsigned int clean_state = ev->xkey.state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
            dispatch_shortcut_event(ctx, (KeyCode)ev->xkey.keycode, clean_state, now);
        }
        return;
    }

    if (ev->type == GenericEvent && ev->xcookie.extension == ctx->xi_opcode) {
        if (XGetEventData(ctx->dpy, &ev->xcookie)) {
            XIRawEvent *raw = (XIRawEvent *)ev->xcookie.data;
            if (raw->evtype == XI_RawMotion) {
                if (now - ctx->last_motion_check >= (uint64_t)ctx->cfg.motion_throttle_ms) {
                    evaluate_pointer(ctx, now);
                } else if (!ctx->motion_pending) {
                    ctx->motion_pending = true;
                    ctx->motion_flush_at = ctx->last_motion_check + (uint64_t)ctx->cfg.motion_throttle_ms;
                }
            } else if (raw->evtype == XI_RawButtonPress && raw->detail == 1) {
                int px = 0, py = 0;
                unsigned int mask = 0;
                Window child = None;
                ctx->btn1_down = true;
                ctx->is_dragging_window = false;
                ctx->drag_top_bar_open = false;
                ctx->drag_target_layout = -1;
                ctx->drag_target_zone = -1;
                ctx->drag_target_max = false;
                if (x11_query_pointer(ctx->dpy, ctx->root, &px, &py, &mask, &child)) {
                    ctx->drag_press_x = px;
                    ctx->drag_press_y = py;
                    if (ctx->active.valid) {
                        ctx->drag_start_rect = ctx->active.frame_rect;
                    }
                    if (ctx->debug) {
                        fprintf(stderr, "[drag] raw-btn1-press at (%d,%d) active=0x%lx start=(%d,%d,%d,%d)\n",
                                px, py, (unsigned long)ctx->active.win,
                                ctx->drag_start_rect.x, ctx->drag_start_rect.y,
                                ctx->drag_start_rect.w, ctx->drag_start_rect.h);
                    }
                }
                if (ctx->state == STATE_ARMED) {
                    transition_idle(ctx, "raw-click");
                } else if (ctx->state == STATE_POPUP_VISIBLE) {
                    if (child != ctx->popup.win && !rect_contains(ctx->popup.card_rect, px, py)) {
                        transition_idle(ctx, "click-outside");
                    }
                } else if (ctx->state == STATE_SNAP_ASSIST) {
                    Rect wa_r = {ctx->assist.wa.x, ctx->assist.wa.y, ctx->assist.wa.w, ctx->assist.wa.h};
                    if (child != ctx->assist.win && !rect_contains(wa_r, px, py)) {
                        transition_idle(ctx, "assist-click-outside-workarea");
                    }
                }
            } else if (raw->evtype == XI_RawButtonRelease && raw->detail == 1) {
                finish_window_drag(ctx, now);
            } else if (raw->evtype == XI_RawKeyPress) {
                KeyCode kc = (KeyCode)raw->detail;
                if (ctx->cfg.enable_shortcuts) {
                    bool maybe_hotkey = false;
                    for (int i = 0; i < ACT_COUNT; i++) {
                        if (ctx->bindings[i].valid && kc == ctx->bindings[i].keycode) {
                            maybe_hotkey = true;
                            break;
                        }
                    }
                    if (maybe_hotkey) {
                        int px = 0, py = 0;
                        unsigned int mask = 0;
                        Window child = None;
                        if (x11_query_pointer(ctx->dpy, ctx->root, &px, &py, &mask, &child)) {
                            unsigned int clean_state = mask & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
                            if (dispatch_shortcut_event(ctx, kc, clean_state, now)) {
                                goto done_xi_event;
                            }
                        }
                    }
                }
                if (kc == ctx->escape_keycode && ctx->state != STATE_IDLE) {
                    transition_idle(ctx, "escape-key");
                } else if (ctx->state == STATE_POPUP_VISIBLE) {
                    for (int i = 0; i < 6; i++) {
                        if (kc == ctx->num_keycodes[i] && ctx->active.win != None) {
                            Window target = ctx->active.win;
                            int zone = (ctx->popup.hovered_layout == i && ctx->popup.hovered_zone >= 0)
                                     ? ctx->popup.hovered_zone : 0;
                            execute_snap_and_assist(ctx, target, i, zone, now);
                            break;
                        }
                    }
                } else if (ctx->state == STATE_SNAP_ASSIST) {
                    if (kc == ctx->arrow_keycodes[0]) {
                        assist_move_focus(&ctx->assist, -1, 0);
                    } else if (kc == ctx->arrow_keycodes[1]) {
                        assist_move_focus(&ctx->assist, 1, 0);
                    } else if (kc == ctx->arrow_keycodes[2]) {
                        assist_move_focus(&ctx->assist, 0, -1);
                    } else if (kc == ctx->arrow_keycodes[3]) {
                        assist_move_focus(&ctx->assist, 0, 1);
                    } else if ((kc == ctx->return_keycode || kc == ctx->kp_enter_keycode) &&
                               ctx->assist.focused_card >= 0 &&
                               ctx->assist.focused_card < ctx->assist.num_cards) {
                        Window snapped_w = None;
                        bool cont = assist_select_card_index(&ctx->assist, &ctx->atoms, &ctx->cfg,
                                                             ctx->workareas, ctx->num_workareas,
                                                             ctx->layouts, ctx->assist.focused_card,
                                                             now, ctx->debug, &snapped_w);
                        if (snapped_w != None) {
                            refresh_active(ctx);
                            csd_decor_sync_all(&ctx->csd_decor, &ctx->atoms, &ctx->cfg,
                                               ctx->active.win, true, ctx->debug);
                            schedule_csd_resync(ctx, now_ms());
                        }
                        if (!cont) transition_idle(ctx, "assist-enter-finished");
                    } else {
                        for (int i = 0; i < 6; i++) {
                            if (kc == ctx->num_keycodes[i] && i < ctx->assist.num_cards) {
                                Window snapped_w = None;
                                bool cont = assist_select_card_index(&ctx->assist, &ctx->atoms, &ctx->cfg,
                                                                     ctx->workareas, ctx->num_workareas,
                                                                     ctx->layouts, i, now, ctx->debug,
                                                                     &snapped_w);
                                if (snapped_w != None) {
                                    refresh_active(ctx);
                                    csd_decor_sync_all(&ctx->csd_decor, &ctx->atoms, &ctx->cfg,
                                                       ctx->active.win, true, ctx->debug);
                                    schedule_csd_resync(ctx, now_ms());
                                }
                                if (!cont) {
                                    transition_idle(ctx, "assist-numkey-finished");
                                }
                                break;
                            }
                        }
                    }
                } else if (ctx->state == STATE_ARMED &&
                           kc != ctx->bindings[ACT_SNAP_LAYOUTS].keycode) {
                    transition_idle(ctx, "keypress");
                }
            }
        done_xi_event:;
            XFreeEventData(ctx->dpy, &ev->xcookie);
        }
        return;
    }

    if (ctx->has_rr && ev->type == ctx->rr_event_base + RRScreenChangeNotify) {
        XRRUpdateConfiguration(ev);
        x11_refresh_workareas(ctx->dpy, ctx->root, &ctx->atoms, ctx->workareas, &ctx->num_workareas);
        transition_idle(ctx, "screen-change");
        refresh_active(ctx);
        return;
    }

    if (ev->xany.window == ctx->popup.win) {
        if (ev->type == Expose && ev->xexpose.count == 0) {
            popup_redraw(&ctx->popup, &ctx->cfg, ctx->layouts);
        } else if (ev->type == MotionNotify) {
            ctx->opened_by_hotkey = false;
            ctx->leave_deadline = 0;
            if (popup_update_hover(&ctx->popup, ev->xmotion.x, ev->xmotion.y, &ctx->cfg, ctx->layouts)) {
                sync_popup_zone_preview(ctx, now);
            }
        } else if (ev->type == ButtonPress && ev->xbutton.button == Button1) {
            int lay = -1, zone = -1;
            if (popup_hit_test(&ctx->popup, ev->xbutton.x, ev->xbutton.y, &lay, &zone)) {
                Window target = ctx->active.win;
                execute_snap_and_assist(ctx, target, lay, zone, now);
            }
        } else if (ev->type == LeaveNotify) {
            evaluate_pointer(ctx, now);
        }
        return;
    }

    if (ev->xany.window == ctx->assist.win) {
        if (ev->type == Expose && ev->xexpose.count == 0) {
            assist_redraw(&ctx->assist);
        } else if (ctx->state == STATE_POPUP_VISIBLE) {
            if (ev->type == MotionNotify) {
                int rx = ev->xmotion.x_root;
                int ry = ev->xmotion.y_root;
                if (rect_contains(ctx->popup.card_rect, rx, ry)) {
                    ctx->opened_by_hotkey = false;
                    ctx->leave_deadline = 0;
                    int lx = rx - ctx->popup.card_rect.x + ctx->popup.shadow_pad;
                    int ly = ry - ctx->popup.card_rect.y + ctx->popup.shadow_pad;
                    if (popup_update_hover(&ctx->popup, lx, ly, &ctx->cfg, ctx->layouts)) {
                        sync_popup_zone_preview(ctx, now);
                    }
                } else {
                    evaluate_pointer(ctx, now);
                }
            } else if (ev->type == ButtonPress && ev->xbutton.button == Button1) {
                int rx = ev->xbutton.x_root;
                int ry = ev->xbutton.y_root;
                if (rect_contains(ctx->popup.card_rect, rx, ry)) {
                    int lx = rx - ctx->popup.card_rect.x + ctx->popup.shadow_pad;
                    int ly = ry - ctx->popup.card_rect.y + ctx->popup.shadow_pad;
                    int lay = -1, zone = -1;
                    if (popup_hit_test(&ctx->popup, lx, ly, &lay, &zone)) {
                        execute_snap_and_assist(ctx, ctx->active.win, lay, zone, now);
                    }
                } else {
                    transition_idle(ctx, "preview-click-outside-popup");
                }
            }
        } else if (ev->type == MotionNotify && ctx->state == STATE_SNAP_ASSIST) {
            assist_update_hover(&ctx->assist, ev->xmotion.x, ev->xmotion.y);
        } else if (ev->type == LeaveNotify && ctx->state == STATE_SNAP_ASSIST) {
            assist_update_hover(&ctx->assist, -1, -1);
        } else if (ev->type == ButtonPress && ctx->state == STATE_SNAP_ASSIST) {
            if (ev->xbutton.button == Button1) {
                Window snapped_w = None;
                bool cont = assist_handle_click(&ctx->assist, &ctx->atoms, &ctx->cfg,
                                                ctx->workareas, ctx->num_workareas, ctx->layouts,
                                                ev->xbutton.x, ev->xbutton.y, now, ctx->debug,
                                                &snapped_w);
                if (snapped_w != None) {
                    refresh_active(ctx);
                    csd_decor_sync_all(&ctx->csd_decor, &ctx->atoms, &ctx->cfg,
                                       ctx->active.win, true, ctx->debug);
                    schedule_csd_resync(ctx, now_ms());
                }
                if (!cont) {
                    transition_idle(ctx, "snap-assist-completed");
                }
            } else {
                transition_idle(ctx, "snap-assist-right-click-dismiss");
            }
        }
        return;
    }

    bool cancel_snap = false;
    bool need_csd_resync = false;
    bool handled_by_csd = csd_decor_handle_event(&ctx->csd_decor, &ctx->atoms, &ctx->cfg,
                                                 ctx->active.win, ev, &cancel_snap, &need_csd_resync);
    if (need_csd_resync) {
        schedule_csd_resync(ctx, now);
    }
    if (handled_by_csd) {
        if (cancel_snap) {
            transition_idle(ctx, "csd-button-click");
        } else if (ev->type == EnterNotify || ev->type == LeaveNotify || ev->type == MotionNotify) {
            evaluate_pointer(ctx, now);
        }
        return;
    }

    if (ev->xany.window == ctx->root && ev->type == ConfigureNotify) {
        x11_refresh_workareas(ctx->dpy, ctx->root, &ctx->atoms, ctx->workareas, &ctx->num_workareas);
        transition_idle(ctx, "root-resolution-changed");
        refresh_active(ctx);
        csd_decor_sync_all(&ctx->csd_decor, &ctx->atoms, &ctx->cfg, ctx->active.win, true, ctx->debug);
        return;
    }

    if (ev->xany.window == ctx->root && ev->type == PropertyNotify) {
        Atom prop = ev->xproperty.atom;
        if (prop == ctx->atoms.net_active_window) {
            if (ctx->state != STATE_SNAP_ASSIST) {
                transition_idle(ctx, "active-window-changed");
            }
            refresh_active(ctx);
            csd_decor_sync_all(&ctx->csd_decor, &ctx->atoms, &ctx->cfg, ctx->active.win, false, ctx->debug);
            if (ctx->state == STATE_SNAP_ASSIST && ctx->assist.visible) {
                XRaiseWindow(ctx->dpy, ctx->assist.win);
            } else {
                evaluate_pointer(ctx, now);
            }
        } else if (prop == ctx->atoms.net_client_list) {
            csd_decor_sync_all(&ctx->csd_decor, &ctx->atoms, &ctx->cfg, ctx->active.win, false, ctx->debug);
        } else if (prop == ctx->atoms.net_current_desktop || prop == ctx->atoms.net_workarea) {
            x11_refresh_workareas(ctx->dpy, ctx->root, &ctx->atoms, ctx->workareas, &ctx->num_workareas);
            transition_idle(ctx, "desktop-or-workarea-changed");
            refresh_active(ctx);
            csd_decor_sync_all(&ctx->csd_decor, &ctx->atoms, &ctx->cfg, ctx->active.win, true, ctx->debug);
        } else if (prop == ctx->atoms.resource_manager) {
            int new_scale = x11_get_dpi_scale(ctx->dpy, ctx->root, &ctx->atoms);
            if (new_scale != ctx->cfg.dpi_scale) {
                ctx->cfg.dpi_scale = new_scale;
                popup_destroy(&ctx->popup);
                popup_init(&ctx->popup, ctx->dpy, ctx->root, &ctx->atoms, &ctx->cfg, ctx->layouts);
            }
            x11_refresh_workareas(ctx->dpy, ctx->root, &ctx->atoms, ctx->workareas, &ctx->num_workareas);
            refresh_active(ctx);
            csd_decor_sync_all(&ctx->csd_decor, &ctx->atoms, &ctx->cfg, ctx->active.win, true, ctx->debug);
        } else {
            char *aname = XGetAtomName(ctx->dpy, prop);
            if (aname) {
                if (strncmp(aname, "_GTK_WORKAREAS", 14) == 0) {
                    x11_refresh_workareas(ctx->dpy, ctx->root, &ctx->atoms, ctx->workareas, &ctx->num_workareas);
                    refresh_active(ctx);
                    csd_decor_sync_all(&ctx->csd_decor, &ctx->atoms, &ctx->cfg, ctx->active.win, true, ctx->debug);
                }
                XFree(aname);
            }
        }
        return;
    }

    if (ev->xany.window == ctx->active.win || ev->xany.window == ctx->active.frame_win) {
        if ((ev->type == DestroyNotify || ev->type == UnmapNotify) && ctx->state != STATE_SNAP_ASSIST) {
            transition_idle(ctx, "target-unmapped");
            refresh_active(ctx);
        } else if (ev->type == ConfigureNotify || ev->type == PropertyNotify) {
            if (ev->type == ConfigureNotify &&
                (ctx->state == STATE_ARMED || ctx->state == STATE_POPUP_VISIBLE)) {
                transition_idle(ctx, "target-moved-or-resized");
            }
            WindowInfo updated;
            if (ctx->active.win != None &&
                x11_read_window_info(ctx->dpy, ctx->root, &ctx->atoms, &ctx->cfg, ctx->active.win, &updated)) {
                bool drag_started_now = false;
                if (ev->type == ConfigureNotify && ctx->state == STATE_IDLE && ctx->btn1_down &&
                    ctx->cfg.drag_snap && updated.valid && updated.can_maximize) {
                    bool moved = (abs(updated.frame_rect.x - ctx->drag_start_rect.x) >= 6 ||
                                  abs(updated.frame_rect.y - ctx->drag_start_rect.y) >= 6);
                    bool same_sz = (abs(updated.frame_rect.w - ctx->drag_start_rect.w) <= 8 &&
                                    abs(updated.frame_rect.h - ctx->drag_start_rect.h) <= 8);
                    WorkArea swa = snap_pick_workarea(ctx->workareas, ctx->num_workareas, ctx->drag_start_rect);
                    bool was_maximized_width = (swa.w > 0 && ctx->drag_start_rect.w >= swa.w - 32);
                    if (!ctx->is_dragging_window && moved && (same_sz || was_maximized_width)) {
                        ctx->is_dragging_window = true;
                        ctx->drag_win = updated.win;
                        drag_started_now = true;
                        if (ctx->debug) {
                            fprintf(stderr, "[drag] drag-started win=0x%lx start=(%d,%d,%d,%d)\n",
                                    (unsigned long)updated.win,
                                    ctx->drag_start_rect.x, ctx->drag_start_rect.y,
                                    ctx->drag_start_rect.w, ctx->drag_start_rect.h);
                        }
                    }
                }
                ctx->active = updated;
                if (ctx->is_dragging_window &&
                    (drag_started_now || now - ctx->last_motion_check >= (uint64_t)ctx->cfg.motion_throttle_ms)) {
                    evaluate_pointer(ctx, now);
                }
            } else {
                refresh_active(ctx);
            }
            schedule_csd_resync(ctx, now);
        }
    }
}

static int run_dump_windows(DaemonCtx *ctx) {
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(ctx->dpy, ctx->root, ctx->atoms.net_client_list, 0, 256, False, XA_WINDOW,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) != Success || !data) {
        fprintf(stderr, "Cannot read _NET_CLIENT_LIST\n");
        return 1;
    }
    Window *list = (Window *)data;
    printf("%-10s %-28s %-30s %-22s %-22s\n",
           "WID", "WM_CLASS", "KIND", "FRAME_RECT", "MAX_BTN_RECT");
    printf("----------------------------------------------------------------------------------------------------------------\n");
    for (unsigned long i = 0; i < nitems; i++) {
        WindowInfo info;
        if (x11_read_window_info(ctx->dpy, ctx->root, &ctx->atoms, &ctx->cfg, list[i], &info)) {
            char fr[32], mb[32];
            snprintf(fr, sizeof(fr), "%d,%d %dx%d",
                     info.frame_rect.x, info.frame_rect.y, info.frame_rect.w, info.frame_rect.h);
            snprintf(mb, sizeof(mb), "%d,%d %dx%d",
                     info.max_btn_rect.x, info.max_btn_rect.y, info.max_btn_rect.w, info.max_btn_rect.h);
            printf("0x%08lx %-28s %-30s %-22s %-22s\n",
                   (unsigned long)info.win, info.wm_class, kind_to_str(&info), fr, mb);
        }
    }
    XFree(data);
    return 0;
}

static bool wait_crosshair_click(Display *dpy, Window root, int *out_x, int *out_y) {
    Cursor cur = XCreateFontCursor(dpy, XC_crosshair);
    if (XGrabPointer(dpy, root, False, ButtonPressMask | ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, None, cur, CurrentTime) != GrabSuccess) {
        XFreeCursor(dpy, cur);
        return false;
    }
    bool got = false;
    XEvent ev;
    while (g_running) {
        XNextEvent(dpy, &ev);
        if (ev.type == ButtonPress && ev.xbutton.button == Button1) {
            *out_x = ev.xbutton.x_root;
            *out_y = ev.xbutton.y_root;
            got = true;
        } else if (ev.type == ButtonRelease && ev.xbutton.button == Button1 && got) {
            break;
        } else if (ev.type == ButtonPress && ev.xbutton.button == Button3) {
            got = false;
            break;
        }
    }
    XUngrabPointer(dpy, CurrentTime);
    XFreeCursor(dpy, cur);
    XFlush(dpy);
    return got;
}

static int run_learn_window(DaemonCtx *ctx, const char *explicit_cfg) {
    printf("=== Calibrador Interativo win11-snapd ===\n");
    printf("Deixe a janela do aplicativo alvo visível e ativa.\n");
    printf("Passo 1/2: Clique no canto SUPERIOR ESQUERDO do botão Maximizar (botão direito cancela)...\n");
    fflush(stdout);

    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    if (!wait_crosshair_click(ctx->dpy, ctx->root, &x1, &y1)) {
        printf("Cancelado.\n");
        return 1;
    }
    printf("  -> Ponto 1 capturado: (%d, %d)\n", x1, y1);
    printf("Passo 2/2: Clique no canto INFERIOR DIREITO do botão Maximizar...\n");
    fflush(stdout);
    if (!wait_crosshair_click(ctx->dpy, ctx->root, &x2, &y2)) {
        printf("Cancelado.\n");
        return 1;
    }
    printf("  -> Ponto 2 capturado: (%d, %d)\n", x2, y2);

    int bx0 = x1 < x2 ? x1 : x2;
    int by0 = y1 < y2 ? y1 : y2;
    int bx1 = x1 > x2 ? x1 : x2;
    int by1 = y1 > y2 ? y1 : y2;
    int bw = bx1 - bx0;
    int bh = by1 - by0;
    if (bw < 12 || bh < 12) {
        fprintf(stderr, "Erro: área selecionada muito pequena (%dx%d).\n", bw, bh);
        return 1;
    }

    /* Find client window containing (bx0 + bw/2, by0 + bh/2) */
    int cx = bx0 + bw / 2;
    int cy = by0 + bh / 2;
    Window target = x11_get_active_window(ctx->dpy, ctx->root, &ctx->atoms);
    WindowInfo info;
    if (target == None || !x11_read_window_info(ctx->dpy, ctx->root, &ctx->atoms, &ctx->cfg, target, &info) ||
        !rect_contains(info.frame_rect, cx, cy)) {
        Atom actual_type = None;
        int actual_fmt = 0;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(ctx->dpy, ctx->root, ctx->atoms.net_client_list, 0, 256, False, XA_WINDOW,
                               &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success && data) {
            Window *list = (Window *)data;
            for (long i = (long)nitems - 1; i >= 0; i--) {
                WindowInfo cand;
                if (x11_read_window_info(ctx->dpy, ctx->root, &ctx->atoms, &ctx->cfg, list[i], &cand) &&
                    rect_contains(cand.frame_rect, cx, cy)) {
                    info = cand;
                    target = list[i];
                    break;
                }
            }
            XFree(data);
        }
    }

    if (target == None || !info.wm_class[0]) {
        fprintf(stderr, "Erro: não foi possível identificar a WM_CLASS da janela clicada.\n");
        return 1;
    }

    int max_idx = x11_find_maximize_index_from_right(ctx->cfg.button_layout);
    if (max_idx < 0) max_idx = 1;
    int top_offset = by0 - info.frame_rect.y;
    if (top_offset < 0) top_offset = 0;
    int right_offset = (info.frame_rect.x + info.frame_rect.w) - bx1 - max_idx * bw;
    if (right_offset < 0) right_offset = 0;

    printf("\nPerfil calculado para WM_CLASS='%s':\n", info.wm_class);
    printf("[profile:%s]\n"
           "enabled = true\n"
           "button_width = %d\n"
           "button_height = %d\n"
           "button_gap = 0\n"
           "right_offset = %d\n"
           "top_offset = %d\n",
           info.wm_class, bw, bh, right_offset, top_offset);

    char cfg_path[512] = "";
    const char *home = getenv("HOME");
    if (explicit_cfg && explicit_cfg[0]) {
        snprintf(cfg_path, sizeof(cfg_path), "%s", explicit_cfg);
    } else if (home) {
        snprintf(cfg_path, sizeof(cfg_path), "%s/.config/win11-snap/config.ini", home);
    }
    if (cfg_path[0]) {
        FILE *fp = fopen(cfg_path, "a");
        if (fp) {
            fprintf(fp,
                    "\n[profile:%s]\n"
                    "enabled = true\n"
                    "button_width = %d\n"
                    "button_height = %d\n"
                    "button_gap = 0\n"
                    "right_offset = %d\n"
                    "top_offset = %d\n",
                    info.wm_class, bw, bh, right_offset, top_offset);
            fclose(fp);
            printf("\nSalvo em %s!\nReinicie o daemon com: systemctl --user restart win11-snapd.service\n",
                   cfg_path);
        }
    }
    return 0;
}

static Display *open_display_with_retry(int max_wait_ms) {
    uint64_t deadline = now_ms() + (uint64_t)max_wait_ms;
    for (;;) {
        Display *dpy = XOpenDisplay(NULL);
        if (dpy) return dpy;
        if (now_ms() >= deadline || !g_running) return NULL;
        struct timespec ts = {0, 250 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

int main(int argc, char **argv) {
    /* Return transient buffers >= 64 KB directly via munmap so idle RSS/PSS stays ~1-2 MB */
    mallopt(M_MMAP_THRESHOLD, 64 * 1024);
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);

    bool debug = false;
    bool dump_windows = false;
    bool learn_window = false;
    const char *explicit_cfg = NULL;
    const char *test_snap_arg = NULL;
    const char *window_arg = NULL;
    const char *show_popup_arg = NULL;
    const char *hover_arg = NULL;
    int duration_ms = 3500;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--settings") == 0) {
            execlp("python3", "python3",
                   "/usr/share/cinnamon/cinnamon-settings/xlet-settings.py",
                   "extension", "win11-snap@pedro", (char *)NULL);
            execlp("cinnamon-settings", "cinnamon-settings", "extensions", (char *)NULL);
            fprintf(stderr, "Error: Cinnamon extension settings launcher not found\n");
            return 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug = true;
        } else if (strcmp(argv[i], "--dump-windows") == 0) {
            dump_windows = true;
        } else if (strcmp(argv[i], "--learn-window") == 0) {
            learn_window = true;
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            explicit_cfg = argv[++i];
        } else if (strcmp(argv[i], "--test-snap") == 0 && i + 1 < argc) {
            test_snap_arg = argv[++i];
        } else if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            window_arg = argv[++i];
        } else if (strcmp(argv[i], "--show-popup-at") == 0 && i + 1 < argc) {
            show_popup_arg = argv[++i];
        } else if (strcmp(argv[i], "--hover") == 0 && i + 1 < argc) {
            hover_arg = argv[++i];
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);

    DaemonCtx ctx = {0};
    ctx.debug = debug;
    ctx.explicit_cfg = explicit_cfg;
    config_load(&ctx.cfg, explicit_cfg);

    ctx.dpy = open_display_with_retry(10000);
    if (!ctx.dpy) {
        fprintf(stderr, "Error: cannot open X11 display\n");
        return 1;
    }

    x11_install_error_handler();
    ctx.root = DefaultRootWindow(ctx.dpy);
    x11_init_atoms(ctx.dpy, &ctx.atoms);
    ctx.cfg.dpi_scale = x11_get_dpi_scale(ctx.dpy, ctx.root, &ctx.atoms);
    layouts_build(ctx.cfg.ratio_split, ctx.layouts);
    x11_refresh_workareas(ctx.dpy, ctx.root, &ctx.atoms, ctx.workareas, &ctx.num_workareas);

    if (dump_windows) {
        int rc = run_dump_windows(&ctx);
        XCloseDisplay(ctx.dpy);
        return rc;
    }

    if (learn_window) {
        int rc = run_learn_window(&ctx, explicit_cfg);
        XCloseDisplay(ctx.dpy);
        return rc;
    }

    /* One-shot --test-snap mode */
    if (test_snap_arg) {
        int l = -1, z = -1;
        if (sscanf(test_snap_arg, "%d:%d", &l, &z) != 2) {
            fprintf(stderr, "Invalid --test-snap format, expected L:Z\n");
            XCloseDisplay(ctx.dpy);
            return 1;
        }
        Window target = None;
        if (window_arg) {
            target = (Window)strtoul(window_arg, NULL, 0);
        } else {
            target = x11_get_active_window(ctx.dpy, ctx.root, &ctx.atoms);
        }
        bool ok = snap_apply(ctx.dpy, ctx.root, &ctx.atoms, &ctx.cfg,
                             ctx.workareas, ctx.num_workareas, ctx.layouts,
                             target, l, z, true);
        XCloseDisplay(ctx.dpy);
        return ok ? 0 : 1;
    }

    if (!popup_init(&ctx.popup, ctx.dpy, ctx.root, &ctx.atoms, &ctx.cfg, ctx.layouts) ||
        !assist_init(&ctx.assist, ctx.dpy, ctx.root)) {
        fprintf(stderr, "Error: failed to create ARGB popup/assist windows\n");
        XCloseDisplay(ctx.dpy);
        return 1;
    }

    /* One-shot --show-popup-at visual inspection mode */
    if (show_popup_arg) {
        int sx = 200, sy = 200;
        if (sscanf(show_popup_arg, "%d,%d", &sx, &sy) != 2) {
            fprintf(stderr, "Invalid --show-popup-at format, expected X,Y\n");
            popup_destroy(&ctx.popup);
            XCloseDisplay(ctx.dpy);
            return 1;
        }
        popup_show_at(&ctx.popup, sx, sy, &ctx.cfg, ctx.layouts);
        if (hover_arg) {
            int hl = -1, hz = -1;
            if (sscanf(hover_arg, "%d:%d", &hl, &hz) == 2 &&
                hl >= 0 && hl < MAX_LAYOUTS && hz >= 0 && hz < ctx.popup.zone_counts[hl]) {
                ctx.popup.hovered_layout = hl;
                ctx.popup.hovered_zone = hz;
                popup_redraw(&ctx.popup, &ctx.cfg, ctx.layouts);
            }
        }
        uint64_t end_at = now_ms() + (uint64_t)(duration_ms > 0 ? duration_ms : 3500);
        struct pollfd pfd = { .fd = ConnectionNumber(ctx.dpy), .events = POLLIN };
        while (g_running) {
            while (XPending(ctx.dpy) > 0) {
                XEvent ev;
                XNextEvent(ctx.dpy, &ev);
                if (ev.xany.window == ctx.popup.win && ev.type == Expose && ev.xexpose.count == 0) {
                    popup_redraw(&ctx.popup, &ctx.cfg, ctx.layouts);
                } else if (ev.xany.window == ctx.popup.win && ev.type == MotionNotify) {
                    popup_update_hover(&ctx.popup, ev.xmotion.x, ev.xmotion.y, &ctx.cfg, ctx.layouts);
                } else if (ev.xany.window == ctx.popup.win && ev.type == ButtonPress) {
                    g_running = 0;
                }
            }
            uint64_t n = now_ms();
            if (n >= end_at) break;
            poll(&pfd, 1, (int)(end_at - n));
        }
        popup_destroy(&ctx.popup);
        XCloseDisplay(ctx.dpy);
        return 0;
    }

    if (!setup_xinput2(&ctx)) {
        popup_destroy(&ctx.popup);
        XCloseDisplay(ctx.dpy);
        return 1;
    }

    ctx.has_rr = XRRQueryExtension(ctx.dpy, &ctx.rr_event_base, &ctx.rr_error_base) ? true : false;
    if (ctx.has_rr) {
        XRRSelectInput(ctx.dpy, ctx.root, RRScreenChangeNotifyMask);
    }

    ctx.escape_keycode = XKeysymToKeycode(ctx.dpy, XK_Escape);
    ctx.return_keycode = XKeysymToKeycode(ctx.dpy, XK_Return);
    ctx.kp_enter_keycode = XKeysymToKeycode(ctx.dpy, XK_KP_Enter);
    ctx.arrow_keycodes[0] = XKeysymToKeycode(ctx.dpy, XK_Left);
    ctx.arrow_keycodes[1] = XKeysymToKeycode(ctx.dpy, XK_Right);
    ctx.arrow_keycodes[2] = XKeysymToKeycode(ctx.dpy, XK_Up);
    ctx.arrow_keycodes[3] = XKeysymToKeycode(ctx.dpy, XK_Down);
    for (int i = 0; i < 6; i++) {
        ctx.num_keycodes[i] = XKeysymToKeycode(ctx.dpy, XK_1 + i);
    }
    grab_all_shortcuts(&ctx);
    XSelectInput(ctx.dpy, ctx.root, PropertyChangeMask | StructureNotifyMask);
    csd_decor_init(&ctx.csd_decor, ctx.dpy, ctx.root);
    refresh_active(&ctx);
    csd_decor_sync_all(&ctx.csd_decor, &ctx.atoms, &ctx.cfg, ctx.active.win, false, ctx.debug);
    XFlush(ctx.dpy);

    if (debug) {
        fprintf(stderr, "[init] theme='%s' layout='%s' dpi_scale=%d monitors=%d fallback_custom_csd=%d super_z=%d\n",
                ctx.cfg.theme_name, ctx.cfg.button_layout, ctx.cfg.dpi_scale, ctx.num_workareas,
                (int)ctx.cfg.fallback_custom_csd, (int)ctx.cfg.hotkey_super_z);
        for (int i = 0; i < ctx.num_workareas; i++) {
            fprintf(stderr, "[init] monitor[%d] workarea=(%d,%d,%d,%d)\n",
                    i, ctx.workareas[i].x, ctx.workareas[i].y, ctx.workareas[i].w, ctx.workareas[i].h);
        }
    }

    struct pollfd pfd = { .fd = ConnectionNumber(ctx.dpy), .events = POLLIN };
    while (g_running) {
        if (g_reload_config) {
            g_reload_config = 0;
            transition_idle(&ctx, "config-reload");
            config_load(&ctx.cfg, ctx.explicit_cfg);
            ctx.cfg.dpi_scale = x11_get_dpi_scale(ctx.dpy, ctx.root, &ctx.atoms);
            layouts_build(ctx.cfg.ratio_split, ctx.layouts);
            grab_all_shortcuts(&ctx);
            popup_destroy(&ctx.popup);
            popup_init(&ctx.popup, ctx.dpy, ctx.root, &ctx.atoms, &ctx.cfg, ctx.layouts);
            csd_decor_sync_all(&ctx.csd_decor, &ctx.atoms, &ctx.cfg, ctx.active.win, true, ctx.debug);
            if (ctx.debug) fprintf(stderr, "[reload] Reloaded config & shortcuts via SIGUSR1\n");
        }

        while (XPending(ctx.dpy) > 0) {
            XEvent ev;
            XNextEvent(ctx.dpy, &ev);
            handle_x11_event(&ctx, &ev, now_ms());
        }

        uint64_t now = now_ms();

        if ((ctx.motion_pending && now >= ctx.motion_flush_at) ||
            (ctx.is_dragging_window && now - ctx.last_motion_check >= 16)) {
            evaluate_pointer(&ctx, now);
            now = now_ms();
        }

        if (ctx.drag_drop_pending && now >= ctx.drag_drop_at) {
            ctx.drag_drop_pending = false;
            Window dw = ctx.drag_win != None ? ctx.drag_win : ctx.active.win;
            if (ctx.drag_target_max && dw != None) {
                snap_set_maximized(ctx.dpy, ctx.root, &ctx.atoms, dw, true);
                schedule_csd_resync(&ctx, now);
            } else if (ctx.drag_target_layout >= 0 && ctx.drag_target_zone >= 0 && dw != None) {
                execute_snap_and_assist(&ctx, dw, ctx.drag_target_layout, ctx.drag_target_zone, now);
            } else if (dw != None && ctx.cfg.drag_snap && ctx.cfg.enable_snap_assist) {
                /* Check if Muffin edge-tiling snapped dw to a half or quadrant on release */
                WindowInfo dinfo;
                if (x11_read_window_info(ctx.dpy, ctx.root, &ctx.atoms, &ctx.cfg, dw, &dinfo) &&
                    dinfo.valid && !dinfo.is_maximized) {
                    WorkArea dwa = snap_pick_workarea(ctx.workareas, ctx.num_workareas, dinfo.frame_rect);
                    int matched_l = -1, matched_z = -1;
                    int check_layouts[2] = {0, 4};
                    for (int li = 0; li < 2 && matched_l < 0; li++) {
                        int l = check_layouts[li];
                        for (int z = 0; z < ctx.layouts[l].num_zones; z++) {
                            Rect zr = layouts_compute_zone_rect(dwa, ctx.layouts[l].zones[z], ctx.cfg.gap_px);
                            if (rect_approx_eq(dinfo.frame_rect, zr)) {
                                matched_l = l;
                                matched_z = z;
                                break;
                            }
                        }
                    }
                    if (matched_l >= 0 &&
                        assist_start_session(&ctx.assist, &ctx.atoms, &ctx.cfg,
                                             ctx.workareas, ctx.num_workareas, ctx.layouts,
                                             matched_l, matched_z, dw, now, ctx.debug)) {
                        ctx.state = STATE_SNAP_ASSIST;
                    }
                }
            }
            ctx.drag_win = None;
            ctx.drag_target_layout = -1;
            ctx.drag_target_zone = -1;
            ctx.drag_target_max = false;
            now = now_ms();
        }

        if (ctx.assist.visible && ctx.assist.anim_active) {
            assist_tick_animation(&ctx.assist, now);
            now = now_ms();
        }

        if (ctx.csd_resync_remaining > 0 && now >= ctx.csd_resync_at) {
            if (ctx.state == STATE_IDLE) {
                refresh_active(&ctx);
            } else if (ctx.active.win != None) {
                WindowInfo updated;
                if (x11_read_window_info(ctx.dpy, ctx.root, &ctx.atoms, &ctx.cfg, ctx.active.win, &updated)) {
                    ctx.active = updated;
                }
            }
            csd_decor_sync_all(&ctx.csd_decor, &ctx.atoms, &ctx.cfg,
                               x11_get_active_window(ctx.dpy, ctx.root, &ctx.atoms), true, ctx.debug);
            ctx.csd_resync_remaining--;
            uint64_t next_delay = (ctx.csd_resync_remaining == 2) ? 120 : 180;
            ctx.csd_resync_at = (ctx.csd_resync_remaining > 0) ? (now + next_delay) : 0;
            now = now_ms();
        }

        if (ctx.state == STATE_ARMED && now >= ctx.arm_deadline) {
            int px = 0, py = 0;
            unsigned int mask = 0;
            Window child = None;
            bool btn_down = false;
            if (x11_query_pointer(ctx.dpy, ctx.root, &px, &py, &mask, &child)) {
                btn_down = (mask & (Button1Mask | Button2Mask | Button3Mask | Button4Mask | Button5Mask)) != 0;
            }
            if (ctx.active.valid && !btn_down &&
                rect_contains(ctx.active.max_btn_rect, px, py) &&
                (child == ctx.active.frame_win || child == ctx.active.win)) {
                WorkArea wa = snap_pick_workarea(ctx.workareas, ctx.num_workareas, ctx.active.max_btn_rect);
                popup_show(&ctx.popup, ctx.active.max_btn_rect, wa, &ctx.cfg, ctx.layouts);
                ctx.state = STATE_POPUP_VISIBLE;
                ctx.opened_by_hotkey = false;
                ctx.leave_deadline = 0;
                if (ctx.debug) {
                    fprintf(stderr, "[state] -> POPUP_VISIBLE card=(%d,%d,%d,%d)\n",
                            ctx.popup.card_rect.x, ctx.popup.card_rect.y,
                            ctx.popup.card_rect.w, ctx.popup.card_rect.h);
                }
            } else {
                transition_idle(&ctx, "arm-expired-invalid");
            }
        }

        if (ctx.state == STATE_POPUP_VISIBLE && ctx.leave_deadline > 0 && now >= ctx.leave_deadline) {
            transition_idle(&ctx, "grace-timeout");
        }

        if (XPending(ctx.dpy) > 0) continue;

        int timeout_ms = compute_poll_timeout(&ctx, now_ms());
        poll(&pfd, 1, timeout_ms);
    }

    transition_idle(&ctx, "shutdown");
    csd_decor_cleanup_all(&ctx.csd_decor);
    assist_destroy(&ctx.assist);
    popup_destroy(&ctx.popup);
    XCloseDisplay(ctx.dpy);
    return 0;
}
