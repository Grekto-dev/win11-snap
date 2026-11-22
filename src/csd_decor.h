#ifndef WIN11_SNAP_CSD_DECOR_H
#define WIN11_SNAP_CSD_DECOR_H

#include "common.h"
#include "x11_state.h"

#define MAX_CSD_DECORS 32
#define CORNER_SAMPLE_SZ 10

typedef struct {
    bool in_use;
    Window client_win;
    Window child_win;
    Visual *visual;
    int depth;

    int client_w;
    int client_h;
    int inset_l;
    int inset_r;
    int inset_t;
    int x;
    int y;
    int w;
    int h;

    int bw;
    int bh;
    int gap;
    int right_off;
    int top_off;

    bool is_maximized;
    bool is_focused;
    int hovered_btn;
    int pressed_btn;

    bool colors_sampled;
    unsigned long bg_pixel;
    double bg_r, bg_g, bg_b;
    double bot_r, bot_g, bot_b;

    bool has_corner_tl;
    uint32_t corner_tl[CORNER_SAMPLE_SZ][CORNER_SAMPLE_SZ];
} CsdDecorEntry;

typedef struct {
    Display *dpy;
    Window root;
    CsdDecorEntry entries[MAX_CSD_DECORS];
} CsdDecorManager;

void csd_decor_init(CsdDecorManager *mgr, Display *dpy, Window root);
void csd_decor_cleanup_all(CsdDecorManager *mgr);
void csd_decor_sync_all(CsdDecorManager *mgr, const X11Atoms *atoms,
                        const AppConfig *cfg, Window active_win,
                        bool force_resample, bool debug);
bool csd_decor_is_our_child(const CsdDecorManager *mgr, Window w, Window *out_client);
bool csd_decor_handle_event(CsdDecorManager *mgr, const X11Atoms *atoms,
                            const AppConfig *cfg, Window active_win,
                            const XEvent *ev, bool *out_cancel_snap_popup,
                            bool *out_schedule_resync);

#endif
