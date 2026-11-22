#include "csd_decor.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void csd_decor_init(CsdDecorManager *mgr, Display *dpy, Window root) {
    memset(mgr, 0, sizeof(*mgr));
    mgr->dpy = dpy;
    mgr->root = root;
}

static void destroy_entry(CsdDecorManager *mgr, CsdDecorEntry *e) {
    if (!e->in_use) return;
    if (e->child_win != None) {
        x11_take_last_error();
        XDestroyWindow(mgr->dpy, e->child_win);
        XSync(mgr->dpy, False);
        x11_take_last_error();
    }
    memset(e, 0, sizeof(*e));
}

void csd_decor_cleanup_all(CsdDecorManager *mgr) {
    if (!mgr->dpy) return;
    for (int i = 0; i < MAX_CSD_DECORS; i++) {
        if (mgr->entries[i].in_use) {
            destroy_entry(mgr, &mgr->entries[i]);
        }
    }
    XFlush(mgr->dpy);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r <= 0.0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    double deg = M_PI / 180.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -90.0 * deg, 0.0 * deg);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0 * deg, 90.0 * deg);
    cairo_arc(cr, x + r, y + h - r, r, 90.0 * deg, 180.0 * deg);
    cairo_arc(cr, x + r, y + r, r, 180.0 * deg, 270.0 * deg);
    cairo_close_path(cr);
}

/* Fallback 32-bit ARGB shadow inset detector when _GTK_FRAME_EXTENTS is not yet populated */
static void detect_argb_insets(Display *dpy, Window win, int client_w, int client_h,
                               int depth, int *out_l, int *out_r, int *out_t) {
    *out_l = 0;
    *out_r = 0;
    *out_t = 0;
    if (depth != 32 || client_w < 200 || client_h < 100) return;

    int probe_x = 48;
    int max_scan = 24;
    x11_take_last_error();
    XImage *im_v = XGetImage(dpy, win, probe_x, 0, 1, (unsigned int)max_scan, AllPlanes, ZPixmap);
    if (im_v && x11_take_last_error() == 0) {
        for (int y = 0; y < max_scan; y++) {
            unsigned long p = XGetPixel(im_v, 0, y);
            if (((p >> 24) & 0xff) >= 240) {
                *out_t = y;
                break;
            }
        }
    }
    if (im_v) XDestroyImage(im_v);

    int probe_y = *out_t + 14;
    if (probe_y >= client_h) probe_y = client_h / 2;
    x11_take_last_error();
    XImage *im_h = XGetImage(dpy, win, 0, probe_y, (unsigned int)max_scan, 1, AllPlanes, ZPixmap);
    if (im_h && x11_take_last_error() == 0) {
        for (int x = 0; x < max_scan; x++) {
            unsigned long p = XGetPixel(im_h, x, 0);
            if (((p >> 24) & 0xff) >= 240) {
                *out_l = x;
                *out_r = x;
                break;
            }
        }
    }
    if (im_h) XDestroyImage(im_h);
}

static void sample_titlebar_colors(Display *dpy, CsdDecorEntry *e, bool child_mapped) {
    /* Sample just to the left of child_win where no circle/icon or child overlay exists */
    int sx = child_mapped ? (e->x - 6) : (e->x + 4);
    int min_x = e->inset_l + 12;
    if (sx < min_x) sx = min_x;
    if (sx >= e->client_w - e->inset_r - 4) sx = e->client_w - e->inset_r - 4;
    int sy_top = e->y + 2;
    int sy_bot = e->y + e->h - 1;
    if (sy_bot <= sy_top) sy_bot = sy_top;

    x11_take_last_error();
    XImage *im_top = XGetImage(dpy, e->client_win, sx, sy_top, 1, 1, AllPlanes, ZPixmap);
    if (im_top && x11_take_last_error() == 0) {
        unsigned long p = XGetPixel(im_top, 0, 0);
        /* Avoid adopting transparent shadow pixel (alpha == 0 && rgb == 0) during mid-resize */
        if (e->depth != 32 || ((p >> 24) & 0xff) >= 200 || (p & 0xffffff) != 0) {
            e->bg_pixel = p;
            e->bg_r = ((p >> 16) & 0xff) / 255.0;
            e->bg_g = ((p >> 8) & 0xff) / 255.0;
            e->bg_b = (p & 0xff) / 255.0;
            e->bot_r = e->bg_r;
            e->bot_g = e->bg_g;
            e->bot_b = e->bg_b;
            e->colors_sampled = true;
        }
    }
    if (im_top) XDestroyImage(im_top);

    x11_take_last_error();
    XImage *im_bot = XGetImage(dpy, e->client_win, sx, sy_bot, 1, 1, AllPlanes, ZPixmap);
    if (im_bot && x11_take_last_error() == 0) {
        unsigned long p = XGetPixel(im_bot, 0, 0);
        if (e->depth != 32 || ((p >> 24) & 0xff) >= 200 || (p & 0xffffff) != 0) {
            e->bot_r = ((p >> 16) & 0xff) / 255.0;
            e->bot_g = ((p >> 8) & 0xff) / 255.0;
            e->bot_b = (p & 0xff) / 255.0;
        }
    }
    if (im_bot) XDestroyImage(im_bot);

    if (!e->colors_sampled) {
        e->bg_pixel = 0xff18181bUL;
        e->bg_r = e->bot_r = 24.0 / 255.0;
        e->bg_g = e->bot_g = 24.0 / 255.0;
        e->bg_b = e->bot_b = 27.0 / 255.0;
        e->colors_sampled = true;
    }

    /* Sample top-left 10x10 corner of the visible frame (never covered by child_win)
     * so we can mirror its exact rounded corner / shadow alpha onto top-right when unmaximized */
    e->has_corner_tl = false;
    if (!e->is_maximized && e->depth == 32 &&
        e->inset_l >= 0 && e->inset_t >= 0 &&
        e->inset_l + CORNER_SAMPLE_SZ < e->client_w &&
        e->inset_t + CORNER_SAMPLE_SZ < e->client_h) {
        x11_take_last_error();
        XImage *im_tl = XGetImage(dpy, e->client_win, e->inset_l, e->inset_t,
                                  CORNER_SAMPLE_SZ, CORNER_SAMPLE_SZ, AllPlanes, ZPixmap);
        if (im_tl && x11_take_last_error() == 0) {
            for (int dy = 0; dy < CORNER_SAMPLE_SZ; dy++) {
                for (int dx = 0; dx < CORNER_SAMPLE_SZ; dx++) {
                    e->corner_tl[dy][dx] = (uint32_t)XGetPixel(im_tl, dx, dy);
                }
            }
            /* Only enable corner mask if interior pixel (9,9) is opaque */
            if (((e->corner_tl[CORNER_SAMPLE_SZ - 1][CORNER_SAMPLE_SZ - 1] >> 24) & 0xff) >= 240) {
                e->has_corner_tl = true;
            }
        }
        if (im_tl) XDestroyImage(im_tl);
    }
}

static void apply_unmaximized_top_right_corner(CsdDecorEntry *e, cairo_surface_t *img) {
    if (e->is_maximized || !e->has_corner_tl || e->w < CORNER_SAMPLE_SZ || e->h < CORNER_SAMPLE_SZ) {
        return;
    }
    cairo_surface_flush(img);
    uint32_t *dst = (uint32_t *)cairo_image_surface_get_data(img);
    int stride = cairo_image_surface_get_stride(img) / 4;
    bool close_hot = (e->hovered_btn == 2 || e->pressed_btn == 2);

    for (int dy = 0; dy < CORNER_SAMPLE_SZ; dy++) {
        for (int dx = 0; dx < CORNER_SAMPLE_SZ; dx++) {
            uint32_t tl = e->corner_tl[dy][dx];
            uint8_t tl_a = (uint8_t)((tl >> 24) & 0xff);
            if (tl_a >= 0xff) continue;

            int tx = e->w - 1 - dx;
            if (!close_hot) {
                /* Copy exact anti-aliased corner + shadow pixel from top-left mirror */
                dst[dy * stride + tx] = tl;
            } else {
                /* On Close red hover/press: keep pure shadow outside corner, blend red on anti-aliased arc */
                if ((tl & 0x00ffffffU) == 0 || tl_a <= 0x35) {
                    dst[dy * stride + tx] = tl;
                } else {
                    double t = (double)(tl_a - 0x35) / (double)(0xff - 0x35);
                    if (t < 0.0) t = 0.0;
                    if (t > 1.0) t = 1.0;
                    uint32_t cur = dst[dy * stride + tx];
                    uint8_t cr = (uint8_t)((cur >> 16) & 0xff);
                    uint8_t cg = (uint8_t)((cur >> 8) & 0xff);
                    uint8_t cb = (uint8_t)(cur & 0xff);
                    uint8_t oa = tl_a;
                    uint8_t or_c = (uint8_t)(cr * t);
                    uint8_t og_c = (uint8_t)(cg * t);
                    uint8_t ob_c = (uint8_t)(cb * t);
                    dst[dy * stride + tx] = ((uint32_t)oa << 24) | ((uint32_t)or_c << 16) |
                                            ((uint32_t)og_c << 8) | (uint32_t)ob_c;
                }
            }
        }
    }
    cairo_surface_mark_dirty(img);
}

static void draw_entry(CsdDecorManager *mgr, CsdDecorEntry *e) {
    if (!e->in_use || e->child_win == None || e->w <= 0 || e->h <= 0) return;

    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, e->w, e->h);
    cairo_t *cr = cairo_create(img);

    /* 1. Paint exact titlebar background & 1px bottom border line */
    cairo_set_source_rgb(cr, e->bg_r, e->bg_g, e->bg_b);
    cairo_paint(cr);

    if (e->h >= 4) {
        cairo_set_source_rgb(cr, e->bot_r, e->bot_g, e->bot_b);
        cairo_rectangle(cr, 0, e->h - 1, e->w, 1);
        cairo_fill(cr);
    }

    double lum = 0.299 * e->bg_r + 0.587 * e->bg_g + 0.114 * e->bg_b;
    bool light_bg = (lum > 0.55);

    int step = e->bw + e->gap;
    for (int b = 0; b < 3; b++) {
        int bx = b * step;
        int bw = (b == 2) ? (e->w - bx) : e->bw;
        bool is_hot = (e->hovered_btn == b);
        bool is_press = (e->pressed_btn == b);

        /* 2. Hover / Active rectangular button background */
        if (is_hot || is_press) {
            if (b == 2) {
                /* Windows 11 Close Red: #C42B1C hover, #B22719 active */
                if (is_press) {
                    cairo_set_source_rgb(cr, 178.0 / 255.0, 39.0 / 255.0, 25.0 / 255.0);
                } else {
                    cairo_set_source_rgb(cr, 196.0 / 255.0, 43.0 / 255.0, 28.0 / 255.0);
                }
            } else {
                if (light_bg) {
                    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, is_press ? 0.04 : 0.06);
                } else {
                    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, is_press ? 0.05 : 0.08);
                }
            }
            cairo_rectangle(cr, bx, 0, bw, e->h);
            cairo_fill(cr);
        }

        /* 3. Crisp 10x10 1px-stroke Windows 11 icon centered at (bx + e->bw/2, e->h/2) */
        double icx = (double)bx + (double)e->bw * 0.5;
        double icy = floor((double)e->h * 0.5);

        if (b == 2 && (is_hot || is_press)) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, is_press ? 0.85 : 1.0);
        } else if (light_bg) {
            double a = (e->is_focused || is_hot) ? 0.88 : 0.42;
            cairo_set_source_rgba(cr, 0.08, 0.08, 0.08, a);
        } else {
            double a = (e->is_focused || is_hot) ? 0.92 : 0.45;
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, a);
        }

        if (b == 0) {
            /* Minimize: 10x1 px horizontal line */
            cairo_set_line_width(cr, 1.0);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
            double x0 = floor(icx - 5.0);
            cairo_move_to(cr, x0, icy + 0.5);
            cairo_line_to(cr, x0 + 10.0, icy + 0.5);
            cairo_stroke(cr);
        } else if (b == 1) {
            cairo_set_line_width(cr, 1.0);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
            double x0 = floor(icx - 5.0) + 0.5;
            double y0 = floor(icy - 5.0) + 0.5;
            if (!e->is_maximized) {
                /* Maximize: 10x10 1px rounded square */
                rounded_rect(cr, x0, y0, 9.0, 9.0, 1.0);
                cairo_stroke(cr);
            } else {
                /* Restore: 2 overlapping rounded squares inside 10x10 */
                rounded_rect(cr, x0, y0 + 2.0, 7.0, 7.0, 1.0);
                cairo_stroke(cr);
                cairo_move_to(cr, x0 + 2.0, y0 + 2.0);
                cairo_line_to(cr, x0 + 2.0, y0);
                cairo_line_to(cr, x0 + 9.0, y0);
                cairo_line_to(cr, x0 + 9.0, y0 + 7.0);
                cairo_line_to(cr, x0 + 7.0, y0 + 7.0);
                cairo_stroke(cr);
            }
        } else {
            /* Close: 10x10 diagonal X */
            cairo_set_line_width(cr, 1.15);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            double x0 = floor(icx - 5.0) + 0.5;
            double y0 = floor(icy - 5.0) + 0.5;
            cairo_move_to(cr, x0, y0);
            cairo_line_to(cr, x0 + 9.0, y0 + 9.0);
            cairo_move_to(cr, x0 + 9.0, y0);
            cairo_line_to(cr, x0, y0 + 9.0);
            cairo_stroke(cr);
        }
    }

    cairo_destroy(cr);

    /* Preserve rounded top-right corner when unmaximized */
    apply_unmaximized_top_right_corner(e, img);

    x11_take_last_error();
    cairo_surface_t *xs = cairo_xlib_surface_create(mgr->dpy, e->child_win, e->visual, e->w, e->h);
    cairo_t *xcr = cairo_create(xs);
    cairo_set_operator(xcr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(xcr, img, 0, 0);
    cairo_paint(xcr);
    cairo_destroy(xcr);
    cairo_surface_destroy(xs);
    cairo_surface_destroy(img);
    XFlush(mgr->dpy);
    x11_take_last_error();
}

static int hit_test_btn(const CsdDecorEntry *e, int lx, int ly) {
    if (lx < 0 || ly < 0 || lx >= e->w || ly >= e->h) return -1;
    int step = e->bw + e->gap;
    if (lx < e->bw) return 0;
    if (lx >= step && lx < step + e->bw) return 1;
    if (lx >= 2 * step) return 2;
    return -1;
}

static CsdDecorEntry *find_by_client(CsdDecorManager *mgr, Window client_win) {
    for (int i = 0; i < MAX_CSD_DECORS; i++) {
        if (mgr->entries[i].in_use && mgr->entries[i].client_win == client_win) {
            return &mgr->entries[i];
        }
    }
    return NULL;
}

static CsdDecorEntry *find_by_child(CsdDecorManager *mgr, Window child_win) {
    for (int i = 0; i < MAX_CSD_DECORS; i++) {
        if (mgr->entries[i].in_use && mgr->entries[i].child_win == child_win) {
            return &mgr->entries[i];
        }
    }
    return NULL;
}

static CsdDecorEntry *alloc_entry(CsdDecorManager *mgr) {
    for (int i = 0; i < MAX_CSD_DECORS; i++) {
        if (!mgr->entries[i].in_use) return &mgr->entries[i];
    }
    return NULL;
}

static void sync_one_window(CsdDecorManager *mgr, const WindowInfo *info,
                            const AppConfig *cfg, Window active_win,
                            bool force_resample, bool debug) {
    int s = cfg->dpi_scale < 1 ? 1 : cfg->dpi_scale;
    int bw = info->applied_geom.button_width * s;
    int bh = info->applied_geom.button_height * s;
    int gap = info->applied_geom.button_gap * s;
    int roff = info->applied_geom.right_offset * s;
    int toff = info->applied_geom.top_offset * s;

    CsdDecorEntry *e = find_by_client(mgr, info->win);
    int depth = e ? e->depth : 0;
    Visual *visual = e ? e->visual : NULL;

    if (!e) {
        XWindowAttributes wa;
        x11_take_last_error();
        if (!XGetWindowAttributes(mgr->dpy, info->win, &wa) || x11_take_last_error() != 0 ||
            wa.map_state != IsViewable) {
            return;
        }
        depth = wa.depth;
        visual = wa.visual;
    }

    int inset_l = 0, inset_r = 0, inset_t = 0;
    if (!info->is_maximized) {
        if (info->has_gtk_frame) {
            inset_l = info->gtk_frame.left;
            inset_r = info->gtk_frame.right;
            inset_t = info->gtk_frame.top;
        } else if (depth == 32) {
            detect_argb_insets(mgr->dpy, info->win, info->client_rect.w, info->client_rect.h,
                               depth, &inset_l, &inset_r, &inset_t);
        }
    }

    int ow = 3 * bw + 2 * gap + roff;
    int oh = bh;
    int ox = info->client_rect.w - inset_r - ow;
    int oy = inset_t + toff;
    if (ox < 40 || ow <= 0 || oh <= 0) return;

    if (!e) {
        e = alloc_entry(mgr);
        if (!e) return;
        memset(e, 0, sizeof(*e));
        e->in_use = true;
        e->client_win = info->win;
        e->visual = visual;
        e->depth = depth;
        e->client_w = info->client_rect.w;
        e->client_h = info->client_rect.h;
        e->inset_l = inset_l;
        e->inset_r = inset_r;
        e->inset_t = inset_t;
        e->x = ox;
        e->y = oy;
        e->w = ow;
        e->h = oh;
        e->bw = bw;
        e->bh = bh;
        e->gap = gap;
        e->right_off = roff;
        e->top_off = toff;
        e->is_maximized = info->is_maximized;
        e->is_focused = (info->win == active_win);
        e->hovered_btn = -1;
        e->pressed_btn = -1;

        /* Sample native titlebar color BEFORE mapping our child overlay */
        sample_titlebar_colors(mgr->dpy, e, false);

        XSetWindowAttributes attrs = {0};
        attrs.win_gravity = NorthEastGravity;
        attrs.background_pixmap = None;
        attrs.border_pixel = 0;
        attrs.event_mask = ExposureMask | EnterWindowMask | LeaveWindowMask |
                           PointerMotionMask | ButtonPressMask | ButtonReleaseMask;

        x11_take_last_error();
        e->child_win = XCreateWindow(mgr->dpy, info->win, ox, oy,
                                     (unsigned int)ow, (unsigned int)oh, 0,
                                     e->depth, InputOutput, e->visual,
                                     CWWinGravity | CWBackPixmap | CWBorderPixel | CWEventMask,
                                     &attrs);
        if (e->child_win == None || x11_take_last_error() != 0) {
            memset(e, 0, sizeof(*e));
            return;
        }
        XMapRaised(mgr->dpy, e->child_win);
        x11_select_window_events(mgr->dpy, info->win, StructureNotifyMask | PropertyChangeMask);
        draw_entry(mgr, e);
        if (debug) {
            fprintf(stderr,
                    "[csd-decor] Attached Win11 buttons child=0x%lx to client=0x%lx ('%s') rect=(%d,%d,%d,%d) insets=(%d,%d,%d) max=%d\n",
                    (unsigned long)e->child_win, (unsigned long)e->client_win, info->wm_class,
                    e->x, e->y, e->w, e->h, e->inset_l, e->inset_r, e->inset_t, (int)e->is_maximized);
        }
        return;
    }

    bool expanded_horz = (info->client_rect.w > e->client_w + 32);
    bool need_move = (e->x != ox || e->y != oy || e->w != ow || e->h != oh ||
                      e->client_w != info->client_rect.w || e->client_h != info->client_rect.h ||
                      e->inset_l != inset_l || e->inset_r != inset_r || e->inset_t != inset_t ||
                      e->is_maximized != info->is_maximized);
    bool foc = (info->win == active_win);
    bool state_changed = (e->is_focused != foc || e->is_maximized != info->is_maximized);

    e->client_w = info->client_rect.w;
    e->client_h = info->client_rect.h;
    e->inset_l = inset_l;
    e->inset_r = inset_r;
    e->inset_t = inset_t;
    e->x = ox;
    e->y = oy;
    e->w = ow;
    e->h = oh;
    e->bw = bw;
    e->bh = bh;
    e->gap = gap;
    e->right_off = roff;
    e->top_off = toff;
    e->is_focused = foc;
    e->is_maximized = info->is_maximized;

    if (need_move) {
        x11_take_last_error();
        XMoveResizeWindow(mgr->dpy, e->child_win, ox, oy, (unsigned int)ow, (unsigned int)oh);
        XRaiseWindow(mgr->dpy, e->child_win);
        if (debug) {
            fprintf(stderr,
                    "[csd-decor] Moved child=0x%lx on client=0x%lx ('%s') -> rect=(%d,%d,%d,%d) insets=(%d,%d,%d) max=%d\n",
                    (unsigned long)e->child_win, (unsigned long)e->client_win, info->wm_class,
                    e->x, e->y, e->w, e->h, e->inset_l, e->inset_r, e->inset_t, (int)e->is_maximized);
        }
    }

    if (need_move || state_changed || force_resample) {
        /* If window just expanded horizontally, keep existing bg color until delayed resync
         * so we do not sample unrendered framebuffer area before Electron's GPU swap */
        if (!expanded_horz || !e->colors_sampled) {
            sample_titlebar_colors(mgr->dpy, e, true);
        }
        draw_entry(mgr, e);
    }
}

void csd_decor_sync_all(CsdDecorManager *mgr, const X11Atoms *atoms,
                        const AppConfig *cfg, Window active_win,
                        bool force_resample, bool debug) {
    if (!mgr->dpy) return;

    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    x11_take_last_error();
    if (XGetWindowProperty(mgr->dpy, mgr->root, atoms->net_client_list, 0, 256, False, XA_WINDOW,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) != Success || !data) {
        return;
    }
    Window *list = (Window *)data;
    bool seen[MAX_CSD_DECORS] = {false};

    for (unsigned long i = 0; i < nitems; i++) {
        WindowInfo info;
        if (x11_read_window_info(mgr->dpy, mgr->root, atoms, cfg, list[i], &info) &&
            info.valid && info.wants_button_overlay) {
            sync_one_window(mgr, &info, cfg, active_win, force_resample, debug);
            for (int j = 0; j < MAX_CSD_DECORS; j++) {
                if (mgr->entries[j].in_use && mgr->entries[j].client_win == list[i]) {
                    seen[j] = true;
                }
            }
        }
    }

    for (int j = 0; j < MAX_CSD_DECORS; j++) {
        if (mgr->entries[j].in_use && !seen[j]) {
            destroy_entry(mgr, &mgr->entries[j]);
        }
    }

    XFree(data);
}

bool csd_decor_is_our_child(const CsdDecorManager *mgr, Window w, Window *out_client) {
    if (w == None) return false;
    for (int i = 0; i < MAX_CSD_DECORS; i++) {
        if (mgr->entries[i].in_use && mgr->entries[i].child_win == w) {
            if (out_client) *out_client = mgr->entries[i].client_win;
            return true;
        }
    }
    return false;
}

static void trigger_button_action(CsdDecorManager *mgr, const X11Atoms *atoms,
                                  CsdDecorEntry *e, int btn) {
    if (btn == 0) {
        /* Minimize */
        XIconifyWindow(mgr->dpy, e->client_win, DefaultScreen(mgr->dpy));
        XFlush(mgr->dpy);
    } else if (btn == 1) {
        /* Toggle Maximize / Restore */
        XEvent ev = {0};
        ev.type = ClientMessage;
        ev.xclient.window = e->client_win;
        ev.xclient.message_type = atoms->net_wm_state;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = 2; /* _NET_WM_STATE_TOGGLE */
        ev.xclient.data.l[1] = (long)atoms->net_wm_state_maximized_horz;
        ev.xclient.data.l[2] = (long)atoms->net_wm_state_maximized_vert;
        ev.xclient.data.l[3] = 2;
        ev.xclient.data.l[4] = 0;
        XSendEvent(mgr->dpy, mgr->root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(mgr->dpy);
    } else if (btn == 2) {
        /* Graceful EWMH Close */
        XEvent ev = {0};
        ev.type = ClientMessage;
        ev.xclient.window = e->client_win;
        ev.xclient.message_type = atoms->net_close_window;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = CurrentTime;
        ev.xclient.data.l[1] = 2;
        XSendEvent(mgr->dpy, mgr->root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(mgr->dpy);
    }
}

bool csd_decor_handle_event(CsdDecorManager *mgr, const X11Atoms *atoms,
                            const AppConfig *cfg, Window active_win,
                            const XEvent *ev, bool *out_cancel_snap_popup,
                            bool *out_schedule_resync) {
    if (out_cancel_snap_popup) *out_cancel_snap_popup = false;
    if (out_schedule_resync) *out_schedule_resync = false;

    CsdDecorEntry *e = find_by_child(mgr, ev->xany.window);
    if (e) {
        if (ev->type == Expose && ev->xexpose.count == 0) {
            draw_entry(mgr, e);
        } else if (ev->type == MotionNotify || ev->type == EnterNotify) {
            if (ev->type == EnterNotify) {
                sample_titlebar_colors(mgr->dpy, e, true);
            }
            int lx = (ev->type == MotionNotify) ? ev->xmotion.x : ev->xcrossing.x;
            int ly = (ev->type == MotionNotify) ? ev->xmotion.y : ev->xcrossing.y;
            int hb = hit_test_btn(e, lx, ly);
            if (hb != e->hovered_btn) {
                e->hovered_btn = hb;
                draw_entry(mgr, e);
            }
        } else if (ev->type == LeaveNotify) {
            if (e->hovered_btn != -1 || e->pressed_btn != -1) {
                e->hovered_btn = -1;
                e->pressed_btn = -1;
                draw_entry(mgr, e);
            }
        } else if (ev->type == ButtonPress && ev->xbutton.button == Button1) {
            int hb = hit_test_btn(e, ev->xbutton.x, ev->xbutton.y);
            e->pressed_btn = hb;
            e->hovered_btn = hb;
            draw_entry(mgr, e);
            if (out_cancel_snap_popup) *out_cancel_snap_popup = true;
        } else if (ev->type == ButtonRelease && ev->xbutton.button == Button1) {
            int hb = hit_test_btn(e, ev->xbutton.x, ev->xbutton.y);
            int was = e->pressed_btn;
            e->pressed_btn = -1;
            e->hovered_btn = (hb >= 0 && hb == was) ? -1 : hb;
            draw_entry(mgr, e);
            if (hb >= 0 && hb == was) {
                if (out_cancel_snap_popup) *out_cancel_snap_popup = true;
                trigger_button_action(mgr, atoms, e, hb);
                if (hb == 1 && out_schedule_resync) {
                    *out_schedule_resync = true;
                }
            }
        }
        return true;
    }

    CsdDecorEntry *ce = find_by_client(mgr, ev->xany.window);
    if (ce) {
        if (ev->type == DestroyNotify) {
            memset(ce, 0, sizeof(*ce));
        } else if (ev->type == ConfigureNotify || ev->type == PropertyNotify) {
            bool relevant_prop = (ev->type == ConfigureNotify) ||
                                 (ev->xproperty.atom == atoms->net_wm_state) ||
                                 (ev->xproperty.atom == atoms->gtk_frame_extents) ||
                                 (ev->xproperty.atom == atoms->net_frame_extents);
            if (relevant_prop) {
                WindowInfo info;
                if (x11_read_window_info(mgr->dpy, mgr->root, atoms, cfg, ce->client_win, &info) &&
                    info.valid && info.wants_button_overlay) {
                    sync_one_window(mgr, &info, cfg, active_win, true, false);
                }
                if (out_schedule_resync) *out_schedule_resync = true;
            }
        }
        /* Return false so main.c also processes active window ConfigureNotify/PropertyNotify */
        return false;
    }

    return false;
}
