#include "popup.h"

#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>
#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r <= 0.0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    double max_r = (w < h ? w : h) * 0.5;
    if (r > max_r) r = max_r;
    double deg = M_PI / 180.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -90.0 * deg, 0.0 * deg);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0 * deg, 90.0 * deg);
    cairo_arc(cr, x + r, y + h - r, r, 90.0 * deg, 180.0 * deg);
    cairo_arc(cr, x + r, y + r, r, 180.0 * deg, 270.0 * deg);
    cairo_close_path(cr);
}

static bool find_argb32_visual(Display *dpy, int screen, Visual **out_vis, int *out_depth) {
    XVisualInfo tmpl = {0};
    tmpl.screen = screen;
    tmpl.depth = 32;
    tmpl.class = TrueColor;
    int n = 0;
    XVisualInfo *list = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &tmpl, &n);
    if (!list) return false;

    for (int i = 0; i < n; i++) {
        XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, list[i].visual);
        if (fmt && fmt->type == PictTypeDirect && fmt->direct.alphaMask > 0) {
            *out_vis = list[i].visual;
            *out_depth = list[i].depth;
            XFree(list);
            return true;
        }
    }
    XFree(list);
    return false;
}

static void compute_layout_geometry(PopupWindow *p, const AppConfig *cfg,
                                    const SnapLayout layouts[MAX_LAYOUTS]) {
    int s = cfg->dpi_scale < 1 ? 1 : cfg->dpi_scale;
    int pad = cfg->padding * s;
    int tw = cfg->thumb_width * s;
    int th = cfg->thumb_height * s;
    int tgap = cfg->thumb_gap * s;
    int cgap = cfg->cell_gap * s;

    p->shadow_pad = 12 * s;
    p->card_w = 2 * pad + 3 * tw + 2 * tgap;
    p->card_h = 2 * pad + 2 * th + 1 * tgap;
    p->win_w = p->card_w + 2 * p->shadow_pad;
    p->win_h = p->card_h + 2 * p->shadow_pad;

    for (int l = 0; l < MAX_LAYOUTS; l++) {
        int col = l % 3;
        int row = l / 3;
        int tx = p->shadow_pad + pad + col * (tw + tgap);
        int ty = p->shadow_pad + pad + row * (th + tgap);
        p->zone_counts[l] = layouts[l].num_zones;
        for (int z = 0; z < layouts[l].num_zones; z++) {
            SnapZone zn = layouts[l].zones[z];
            int zx0 = tx + (int)lround(zn.fx * (double)tw);
            int zx1 = tx + (int)lround((zn.fx + zn.fw) * (double)tw);
            int zy0 = ty + (int)lround(zn.fy * (double)th);
            int zy1 = ty + (int)lround((zn.fy + zn.fh) * (double)th);

            int half_lo = cgap / 2;
            int half_hi = cgap - half_lo;
            if (zn.fx > 0.001) zx0 += half_lo;
            if (zn.fx + zn.fw < 0.999) zx1 -= half_hi;
            if (zn.fy > 0.001) zy0 += half_lo;
            if (zn.fy + zn.fh < 0.999) zy1 -= half_hi;

            p->zone_boxes[l][z] = (Rect){zx0, zy0, zx1 - zx0, zy1 - zy0};
        }
    }
}

bool popup_init(PopupWindow *p, Display *dpy, Window root, const X11Atoms *atoms,
                const AppConfig *cfg, const SnapLayout layouts[MAX_LAYOUTS]) {
    memset(p, 0, sizeof(*p));
    p->dpy = dpy;
    p->root = root;
    p->hovered_layout = -1;
    p->hovered_zone = -1;

    int screen = DefaultScreen(dpy);
    if (!find_argb32_visual(dpy, screen, &p->visual, &p->depth)) {
        p->visual = DefaultVisual(dpy, screen);
        p->depth = DefaultDepth(dpy, screen);
        p->cmap = DefaultColormap(dpy, screen);
    } else {
        p->cmap = XCreateColormap(dpy, root, p->visual, AllocNone);
    }

    compute_layout_geometry(p, cfg, layouts);

    XSetWindowAttributes attrs = {0};
    attrs.override_redirect = True;
    attrs.colormap = p->cmap;
    attrs.background_pixel = 0;
    attrs.border_pixel = 0;
    attrs.save_under = True;
    attrs.event_mask = ExposureMask | PointerMotionMask | ButtonPressMask |
                       ButtonReleaseMask | EnterWindowMask | LeaveWindowMask;

    unsigned long mask = CWOverrideRedirect | CWColormap | CWBackPixel |
                         CWBorderPixel | CWSaveUnder | CWEventMask;
    p->win = XCreateWindow(dpy, root, -2000, -2000,
                           (unsigned int)p->win_w, (unsigned int)p->win_h,
                           0, p->depth, InputOutput, p->visual, mask, &attrs);
    if (p->win == None) return false;

    XChangeProperty(dpy, p->win, atoms->net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (const unsigned char *)&atoms->net_wm_window_type_popup_menu, 1);
    XChangeProperty(dpy, p->win, atoms->net_wm_state, XA_ATOM, 32,
                    PropModeReplace, (const unsigned char *)&atoms->net_wm_state_above, 1);

    XClassHint ch = {.res_name = "win11-snapd", .res_class = "win11-snapd"};
    XSetClassHint(dpy, p->win, &ch);

    p->initialized = true;
    return true;
}

void popup_destroy(PopupWindow *p) {
    if (!p->initialized) return;
    if (p->win != None) {
        XDestroyWindow(p->dpy, p->win);
        p->win = None;
    }
    if (p->depth == 32 && p->cmap != None) {
        XFreeColormap(p->dpy, p->cmap);
        p->cmap = None;
    }
    p->initialized = false;
}

void popup_redraw(PopupWindow *p, const AppConfig *cfg, const SnapLayout layouts[MAX_LAYOUTS]) {
    if (!p->initialized || !p->visible) return;
    (void)layouts;

    int s = cfg->dpi_scale < 1 ? 1 : cfg->dpi_scale;
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, p->win_w, p->win_h);
    cairo_t *cr = cairo_create(img);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    double cx = (double)p->shadow_pad;
    double cy = (double)p->shadow_pad;
    double cw = (double)p->card_w;
    double ch = (double)p->card_h;
    double cr_rad = (double)(cfg->card_radius * s);
    double zr_rad = (double)(cfg->zone_radius * s);

    /* Subtle layered soft shadow inside ARGB shadow gutter */
    for (int i = 4; i >= 1; i--) {
        double spread = (double)(i * 2 * s);
        double yoff = (double)(i * s);
        double alpha = 0.055 * (5 - i);
        rounded_rect_path(cr, cx - spread * 0.5, cy + yoff - spread * 0.25,
                          cw + spread, ch + spread * 0.75, cr_rad + spread * 0.4);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, alpha);
        cairo_fill(cr);
    }

    /* Card surface */
    rounded_rect_path(cr, cx, cy, cw, ch, cr_rad);
    cairo_set_source_rgba(cr, cfg->colors.card_bg.r, cfg->colors.card_bg.g,
                          cfg->colors.card_bg.b, cfg->colors.card_bg.a);
    cairo_fill(cr);

    /* Crisp 1px inner border */
    rounded_rect_path(cr, cx + 0.5, cy + 0.5, cw - 1.0, ch - 1.0, cr_rad - 0.5);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, cfg->colors.card_border.r, cfg->colors.card_border.g,
                          cfg->colors.card_border.b, cfg->colors.card_border.a);
    cairo_stroke(cr);

    /* Draw 6 layout thumbnails and their interactive zones */
    for (int l = 0; l < MAX_LAYOUTS; l++) {
        for (int z = 0; z < p->zone_counts[l]; z++) {
            Rect zb = p->zone_boxes[l][z];
            bool hot = (l == p->hovered_layout && z == p->hovered_zone);
            ColorRGBA fill = hot ? cfg->colors.zone_hover_bg : cfg->colors.zone_bg;
            ColorRGBA stroke = hot ? cfg->colors.zone_hover_border : cfg->colors.zone_border;

            rounded_rect_path(cr, (double)zb.x + 0.5, (double)zb.y + 0.5,
                              (double)zb.w - 1.0, (double)zb.h - 1.0, zr_rad);
            cairo_set_source_rgba(cr, fill.r, fill.g, fill.b, fill.a);
            cairo_fill_preserve(cr);
            cairo_set_line_width(cr, 1.0);
            cairo_set_source_rgba(cr, stroke.r, stroke.g, stroke.b, stroke.a);
            cairo_stroke(cr);
        }
    }

    cairo_destroy(cr);

    /* Blit double-buffered image surface directly to X11 window */
    cairo_surface_t *xs = cairo_xlib_surface_create(p->dpy, p->win, p->visual, p->win_w, p->win_h);
    cairo_t *xcr = cairo_create(xs);
    cairo_set_operator(xcr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(xcr, img, 0, 0);
    cairo_paint(xcr);
    cairo_destroy(xcr);
    cairo_surface_destroy(xs);
    cairo_surface_destroy(img);
    XFlush(p->dpy);
}

static void map_at_card_pos(PopupWindow *p, int card_x, int card_y,
                            const AppConfig *cfg, const SnapLayout layouts[MAX_LAYOUTS]) {
    p->card_rect = (Rect){card_x, card_y, p->card_w, p->card_h};
    p->hovered_layout = -1;
    p->hovered_zone = -1;

    XMoveResizeWindow(p->dpy, p->win,
                      card_x - p->shadow_pad, card_y - p->shadow_pad,
                      (unsigned int)p->win_w, (unsigned int)p->win_h);
    p->visible = true;
    XMapRaised(p->dpy, p->win);
    popup_redraw(p, cfg, layouts);
}

void popup_show(PopupWindow *p, Rect anchor_btn, WorkArea wa,
                const AppConfig *cfg, const SnapLayout layouts[MAX_LAYOUTS]) {
    if (!p->initialized) return;
    int s = cfg->dpi_scale < 1 ? 1 : cfg->dpi_scale;
    int margin = 4 * s;
    int anchor_gap = cfg->anchor_gap * s;

    int card_x = anchor_btn.x + anchor_btn.w / 2 - p->card_w / 2;
    int card_y = anchor_btn.y + anchor_btn.h + anchor_gap;

    int min_x = wa.x + margin;
    int max_x = wa.x + wa.w - p->card_w - margin;
    int min_y = wa.y + margin;
    int max_y = wa.y + wa.h - p->card_h - margin;
    if (max_x < min_x) max_x = min_x;
    if (max_y < min_y) max_y = min_y;

    card_x = clamp_int(card_x, min_x, max_x);
    card_y = clamp_int(card_y, min_y, max_y);

    /* Grace corridor bridging max button down to popup card */
    int cx0 = anchor_btn.x < card_x ? anchor_btn.x : card_x;
    int cx1 = (anchor_btn.x + anchor_btn.w) > (card_x + p->card_w)
            ? (anchor_btn.x + anchor_btn.w)
            : (card_x + p->card_w);
    p->corridor_rect = (Rect){cx0 - 4, anchor_btn.y, (cx1 - cx0) + 8, (card_y - anchor_btn.y) + 8};

    map_at_card_pos(p, card_x, card_y, cfg, layouts);
}

void popup_show_at(PopupWindow *p, int screen_x, int screen_y,
                   const AppConfig *cfg, const SnapLayout layouts[MAX_LAYOUTS]) {
    if (!p->initialized) return;
    p->corridor_rect = (Rect){screen_x, screen_y, p->card_w, p->card_h};
    map_at_card_pos(p, screen_x, screen_y, cfg, layouts);
}

void popup_hide(PopupWindow *p) {
    if (!p->initialized || !p->visible) return;
    XUnmapWindow(p->dpy, p->win);
    XFlush(p->dpy);
    p->visible = false;
    p->hovered_layout = -1;
    p->hovered_zone = -1;
}

bool popup_hit_test(const PopupWindow *p, int local_x, int local_y,
                    int *out_layout, int *out_zone) {
    for (int l = 0; l < MAX_LAYOUTS; l++) {
        for (int z = 0; z < p->zone_counts[l]; z++) {
            if (rect_contains(p->zone_boxes[l][z], local_x, local_y)) {
                if (out_layout) *out_layout = l;
                if (out_zone) *out_zone = z;
                return true;
            }
        }
    }
    if (out_layout) *out_layout = -1;
    if (out_zone) *out_zone = -1;
    return false;
}

bool popup_update_hover(PopupWindow *p, int local_x, int local_y,
                        const AppConfig *cfg, const SnapLayout layouts[MAX_LAYOUTS]) {
    int nl = -1, nz = -1;
    popup_hit_test(p, local_x, local_y, &nl, &nz);
    if (nl != p->hovered_layout || nz != p->hovered_zone) {
        p->hovered_layout = nl;
        p->hovered_zone = nz;
        popup_redraw(p, cfg, layouts);
        return true;
    }
    return false;
}
