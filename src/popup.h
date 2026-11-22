#ifndef WIN11_SNAP_POPUP_H
#define WIN11_SNAP_POPUP_H

#include "common.h"
#include "x11_state.h"

typedef struct {
    Display *dpy;
    Window root;
    Window win;
    Colormap cmap;
    Visual *visual;
    int depth;
    bool initialized;
    bool visible;

    int shadow_pad;
    int card_w;
    int card_h;
    int win_w;
    int win_h;

    Rect card_rect;     /* Screen coordinates of the visible card (excluding shadow pad) */
    Rect corridor_rect; /* Grace bridge between max_btn_rect and card_rect */

    int hovered_layout;
    int hovered_zone;

    Rect zone_boxes[MAX_LAYOUTS][MAX_ZONES];
    int zone_counts[MAX_LAYOUTS];
} PopupWindow;

bool popup_init(PopupWindow *p, Display *dpy, Window root, const X11Atoms *atoms,
                const AppConfig *cfg, const SnapLayout layouts[MAX_LAYOUTS]);
void popup_destroy(PopupWindow *p);

void popup_show(PopupWindow *p, Rect anchor_btn, WorkArea wa,
                const AppConfig *cfg, const SnapLayout layouts[MAX_LAYOUTS]);
void popup_show_at(PopupWindow *p, int screen_x, int screen_y,
                   const AppConfig *cfg, const SnapLayout layouts[MAX_LAYOUTS]);
void popup_hide(PopupWindow *p);

void popup_redraw(PopupWindow *p, const AppConfig *cfg, const SnapLayout layouts[MAX_LAYOUTS]);
bool popup_hit_test(const PopupWindow *p, int local_x, int local_y,
                    int *out_layout, int *out_zone);
bool popup_update_hover(PopupWindow *p, int local_x, int local_y,
                        const AppConfig *cfg, const SnapLayout layouts[MAX_LAYOUTS]);

#endif
