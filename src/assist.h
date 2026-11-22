#ifndef WIN11_SNAP_ASSIST_H
#define WIN11_SNAP_ASSIST_H

#include "common.h"
#include "x11_state.h"
#include <cairo/cairo.h>

#define MAX_ASSIST_CARDS 6

typedef struct {
    Window win;
    Window frame_win;
    char title[128];
    char wm_class[64];
    Rect src_frame_rect;
    Rect card_rect;       /* Relative to workarea (sa->win local coordinates) */
    Rect close_btn_rect;  /* Top-right close button inside card header */
    cairo_surface_t *thumb_surf;
    int thumb_w;
    int thumb_h;
    cairo_surface_t *icon_surf;
} AssistCard;

typedef struct {
    Display *dpy;
    Window root;
    Visual *visual;
    Colormap colormap;
    int depth;

    /* Full-workarea ARGB overlay window for Snap Assist & Phantom Zone Preview */
    Window win;
    bool visible;
    bool preview_only;
    WorkArea wa;

    /* Phantom zone preview state (while hovering zones in Snap Layouts popup) */
    bool preview_zone_active;
    bool has_preview_rect;
    Rect preview_start_rect;
    Rect preview_cur_rect;
    Rect preview_target_rect;
    uint64_t preview_anim_start_ms;
    uint64_t preview_anim_dur_ms;

    /* Active layout & multi-quadrant progression state */
    int layout_idx;
    int num_zones;
    Rect zone_rects[MAX_ZONES];
    bool zone_filled[MAX_ZONES];
    int current_zone;

    /* Windows 11 Top Snap Bar (shown when dragging a window to top-center) */
    bool top_bar_active;
    Rect top_bar_rect; /* Relative to wa (sa->win local coordinates) */
    Rect top_bar_zones[MAX_LAYOUTS][MAX_ZONES];
    int top_bar_zone_counts[MAX_LAYOUTS];
    int top_bar_hot_layout;
    int top_bar_hot_zone;

    /* Candidate windows for Snap Assist picker */
    AssistCard cards[MAX_ASSIST_CARDS];
    int num_cards;
    int hovered_card;
    int focused_card;
    bool hovered_card_close;

    /* Frosted Acrylic blurred background surface (1/4 scale blurred workarea) */
    cairo_surface_t *blurred_bg;
    int blur_w;
    int blur_h;

    /* Entrance animation state for Snap Assist cards */
    bool anim_active;
    uint64_t anim_start_ms;
    uint64_t anim_dur_ms;
    double anim_progress; /* 0.0 -> 1.0 */
} SnapAssist;

bool assist_init(SnapAssist *sa, Display *dpy, Window root);
void assist_destroy(SnapAssist *sa);
void assist_hide(SnapAssist *sa);

/* Desktop Phantom Zone Preview while hovering zones in the Snap Layouts popup */
void assist_update_zone_preview(SnapAssist *sa, WorkArea wa, const Rect *target_zone,
                                Window popup_win, uint64_t now_ms);

/* Top Snap Bar + Phantom Zone Preview while dragging a window near edges/corners/top */
void assist_update_drag_preview(SnapAssist *sa, WorkArea wa, bool show_top_bar,
                                int pointer_x, int pointer_y, const Rect *phantom_zone,
                                const SnapLayout layouts[MAX_LAYOUTS], const AppConfig *cfg,
                                int *out_bar_layout, int *out_bar_zone, uint64_t now_ms);

/* Capture a crisp thumbnail of any window (cropped to visible frame) */
cairo_surface_t *assist_capture_window_thumb(Display *dpy, const WindowInfo *info,
                                             int max_w, int max_h);

/* Smooth 60 FPS morph animation when snapping a window into a target zone */
void assist_animate_snap_transition(SnapAssist *sa, Window target_win, Window frame_win,
                                    Rect start_rect, Rect target_rect,
                                    cairo_surface_t *optional_thumb);

/* Start interactive post-snap multi-quadrant Snap Assist session */
bool assist_start_session(SnapAssist *sa, const X11Atoms *atoms, const AppConfig *cfg,
                          const WorkArea workareas[MAX_MONITORS], int num_workareas,
                          const SnapLayout layouts[MAX_LAYOUTS],
                          int layout_idx, int snapped_zone_idx,
                          Window snapped_win, uint64_t now_ms, bool debug);

/* Start Snap Assist with explicit screen-space zone rects (e.g. top/bottom half) */
bool assist_start_custom_layout(SnapAssist *sa, const X11Atoms *atoms, const AppConfig *cfg,
                                const WorkArea workareas[MAX_MONITORS], int num_workareas,
                                const Rect *abs_zones, int num_zones, int snapped_zone_idx,
                                Window snapped_win, uint64_t now_ms, bool debug);

/* Move keyboard focus among Snap Assist cards (-1/+1 horizontal or vertical) */
void assist_move_focus(SnapAssist *sa, int dx, int dy);

/* Advance animation tick; returns true if another frame is needed */
bool assist_tick_animation(SnapAssist *sa, uint64_t now_ms);

void assist_redraw(SnapAssist *sa);
void assist_update_hover(SnapAssist *sa, int lx, int ly);

/* Handle click at (lx, ly) inside sa->win:
 * - If a card is clicked, snaps that window into current_zone and advances to next unfilled quadrant.
 * - Returns true if Snap Assist continues to the next quadrant, false if Snap Assist finished or was dismissed. */
bool assist_handle_click(SnapAssist *sa, const X11Atoms *atoms, const AppConfig *cfg,
                         const WorkArea workareas[MAX_MONITORS], int num_workareas,
                         const SnapLayout layouts[MAX_LAYOUTS],
                         int lx, int ly, uint64_t now_ms, bool debug,
                         Window *out_last_snapped);

/* Select card by index (e.g. keyboard 1..6); returns true if session continues, false if finished */
bool assist_select_card_index(SnapAssist *sa, const X11Atoms *atoms, const AppConfig *cfg,
                              const WorkArea workareas[MAX_MONITORS], int num_workareas,
                              const SnapLayout layouts[MAX_LAYOUTS],
                              int card_idx, uint64_t now_ms, bool debug,
                              Window *out_last_snapped);

#endif
