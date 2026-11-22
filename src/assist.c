#include "assist.h"
#include "layouts.h"
#include "snap.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>
#include <dlfcn.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef Pixmap (*XCompositeNameWindowPixmapFn)(Display *dpy, Window window);
static XCompositeNameWindowPixmapFn g_name_window_pixmap = NULL;
static bool g_xcomp_checked = false;

static void ensure_xcomposite_loaded(void) {
    if (g_xcomp_checked) return;
    g_xcomp_checked = true;
    void *h = dlopen("libXcomposite.so.1", RTLD_LAZY);
    if (!h) h = dlopen("libXcomposite.so", RTLD_LAZY);
    if (h) {
        g_name_window_pixmap = (XCompositeNameWindowPixmapFn)dlsym(h, "XCompositeNameWindowPixmap");
    }
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static double cubic_ease_out(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    double inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
}

static Rect lerp_rect(Rect a, Rect b, double t) {
    double e = cubic_ease_out(t);
    return (Rect){
        (int)lround(a.x + (b.x - a.x) * e),
        (int)lround(a.y + (b.y - a.y) * e),
        (int)lround(a.w + (b.w - a.w) * e),
        (int)lround(a.h + (b.h - a.h) * e)
    };
}

static void rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r <= 0.0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    if (r > w * 0.5) r = w * 0.5;
    if (r > h * 0.5) r = h * 0.5;
    double deg = M_PI / 180.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -90.0 * deg, 0.0 * deg);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0 * deg, 90.0 * deg);
    cairo_arc(cr, x + r, y + h - r, r, 90.0 * deg, 180.0 * deg);
    cairo_arc(cr, x + r, y + r, r, 180.0 * deg, 270.0 * deg);
    cairo_close_path(cr);
}

static void rounded_top_path(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w * 0.5) r = w * 0.5;
    if (r > h) r = h;
    double deg = M_PI / 180.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -90.0 * deg, 0.0 * deg);
    cairo_line_to(cr, x + w, y + h);
    cairo_line_to(cr, x, y + h);
    cairo_arc(cr, x + r, y + r, r, 180.0 * deg, 270.0 * deg);
    cairo_close_path(cr);
}

static void free_card_surfaces(AssistCard *c) {
    if (c->thumb_surf) {
        cairo_surface_destroy(c->thumb_surf);
        c->thumb_surf = NULL;
    }
    if (c->icon_surf) {
        cairo_surface_destroy(c->icon_surf);
        c->icon_surf = NULL;
    }
}

static void free_all_session_resources(SnapAssist *sa) {
    for (int i = 0; i < MAX_ASSIST_CARDS; i++) {
        free_card_surfaces(&sa->cards[i]);
    }
    sa->num_cards = 0;
    sa->hovered_card = -1;
    sa->hovered_card_close = false;
    if (sa->blurred_bg) {
        cairo_surface_destroy(sa->blurred_bg);
        sa->blurred_bg = NULL;
    }
}

bool assist_init(SnapAssist *sa, Display *dpy, Window root) {
    memset(sa, 0, sizeof(*sa));
    sa->dpy = dpy;
    sa->root = root;
    sa->hovered_card = -1;
    ensure_xcomposite_loaded();

    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, DefaultScreen(dpy), 32, TrueColor, &vinfo)) {
        return false;
    }
    sa->visual = vinfo.visual;
    sa->depth = vinfo.depth;
    sa->colormap = XCreateColormap(dpy, root, sa->visual, AllocNone);

    XSetWindowAttributes attrs = {0};
    attrs.colormap = sa->colormap;
    attrs.background_pixel = 0;
    attrs.border_pixel = 0;
    attrs.override_redirect = True;
    attrs.save_under = False;
    attrs.event_mask = ExposureMask | PointerMotionMask | ButtonPressMask |
                       ButtonReleaseMask | LeaveWindowMask;

    sa->win = XCreateWindow(dpy, root, -4000, -4000, 1, 1, 0,
                            sa->depth, InputOutput, sa->visual,
                            CWColormap | CWBackPixel | CWBorderPixel |
                            CWOverrideRedirect | CWSaveUnder | CWEventMask,
                            &attrs);
    return sa->win != None;
}

void assist_hide(SnapAssist *sa) {
    if (sa->visible && sa->win != None) {
        XUnmapWindow(sa->dpy, sa->win);
        /* Shrink back to 1x1 so Xorg/Muffin immediately releases the 8 MB backing pixmap */
        XMoveResizeWindow(sa->dpy, sa->win, -4000, -4000, 1, 1);
        XFlush(sa->dpy);
    }
    sa->visible = false;
    sa->preview_only = false;
    sa->preview_zone_active = false;
    sa->top_bar_active = false;
    sa->top_bar_hot_layout = -1;
    sa->top_bar_hot_zone = -1;
    sa->has_preview_rect = false;
    sa->anim_active = false;
    sa->focused_card = -1;
    free_all_session_resources(sa);
    malloc_trim(0);
}

void assist_destroy(SnapAssist *sa) {
    assist_hide(sa);
    if (sa->win != None) {
        XDestroyWindow(sa->dpy, sa->win);
        sa->win = None;
    }
    if (sa->colormap != None) {
        XFreeColormap(sa->dpy, sa->colormap);
        sa->colormap = None;
    }
}

/* Fast separable 3-pass box blur on an ARGB32 image surface (approximates Gaussian blur) */
static void blur_image_surface(cairo_surface_t *surf, int radius) {
    if (!surf || radius < 1) return;
    cairo_surface_flush(surf);
    int w = cairo_image_surface_get_width(surf);
    int h = cairo_image_surface_get_height(surf);
    int stride = cairo_image_surface_get_stride(surf) / 4;
    uint32_t *pix = (uint32_t *)cairo_image_surface_get_data(surf);
    if (!pix || w <= 2 || h <= 2) return;

    uint32_t *tmp = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!tmp) return;

    int diam = 2 * radius + 1;
    for (int pass = 0; pass < 3; pass++) {
        /* Horizontal pass: pix -> tmp */
        for (int y = 0; y < h; y++) {
            int sr = 0, sg = 0, sb = 0;
            const uint32_t *row = pix + y * stride;
            for (int k = -radius; k <= radius; k++) {
                int xk = clamp_int(k, 0, w - 1);
                uint32_t p = row[xk];
                sr += (int)((p >> 16) & 0xff);
                sg += (int)((p >> 8) & 0xff);
                sb += (int)(p & 0xff);
            }
            for (int x = 0; x < w; x++) {
                uint32_t r = (uint32_t)(sr / diam);
                uint32_t g = (uint32_t)(sg / diam);
                uint32_t b = (uint32_t)(sb / diam);
                tmp[y * w + x] = 0xff000000U | (r << 16) | (g << 8) | b;

                int x_out = clamp_int(x - radius, 0, w - 1);
                int x_in = clamp_int(x + radius + 1, 0, w - 1);
                uint32_t p_out = row[x_out];
                uint32_t p_in = row[x_in];
                sr += (int)((p_in >> 16) & 0xff) - (int)((p_out >> 16) & 0xff);
                sg += (int)((p_in >> 8) & 0xff) - (int)((p_out >> 8) & 0xff);
                sb += (int)(p_in & 0xff) - (int)(p_out & 0xff);
            }
        }
        /* Vertical pass: tmp -> pix */
        for (int x = 0; x < w; x++) {
            int sr = 0, sg = 0, sb = 0;
            for (int k = -radius; k <= radius; k++) {
                int yk = clamp_int(k, 0, h - 1);
                uint32_t p = tmp[yk * w + x];
                sr += (int)((p >> 16) & 0xff);
                sg += (int)((p >> 8) & 0xff);
                sb += (int)(p & 0xff);
            }
            for (int y = 0; y < h; y++) {
                uint32_t r = (uint32_t)(sr / diam);
                uint32_t g = (uint32_t)(sg / diam);
                uint32_t b = (uint32_t)(sb / diam);
                pix[y * stride + x] = 0xff000000U | (r << 16) | (g << 8) | b;

                int y_out = clamp_int(y - radius, 0, h - 1);
                int y_in = clamp_int(y + radius + 1, 0, h - 1);
                uint32_t p_out = tmp[y_out * w + x];
                uint32_t p_in = tmp[y_in * w + x];
                sr += (int)((p_in >> 16) & 0xff) - (int)((p_out >> 16) & 0xff);
                sg += (int)((p_in >> 8) & 0xff) - (int)((p_out >> 8) & 0xff);
                sb += (int)(p_in & 0xff) - (int)(p_out & 0xff);
            }
        }
    }

    free(tmp);
    cairo_surface_mark_dirty(surf);
}

static void capture_blurred_workarea(SnapAssist *sa, WorkArea wa) {
    if (sa->blurred_bg) {
        cairo_surface_destroy(sa->blurred_bg);
        sa->blurred_bg = NULL;
    }
    /* 1/6 scale (~320x172 on 1080p = 220 KB) downscaled directly on Xorg via XRenderComposite */
    int bw = wa.w / 6;
    int bh = wa.h / 6;
    if (bw < 64) bw = 64;
    if (bh < 48) bh = 48;
    sa->blur_w = bw;
    sa->blur_h = bh;

    sa->blurred_bg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bw, bh);
    cairo_t *cr = cairo_create(sa->blurred_bg);

    bool ok = false;
    int scr = DefaultScreen(sa->dpy);
    XRenderPictFormat *fmt_root = XRenderFindVisualFormat(sa->dpy, DefaultVisual(sa->dpy, scr));
    XRenderPictFormat *fmt32 = XRenderFindStandardFormat(sa->dpy, PictStandardARGB32);
    if (fmt_root && fmt32) {
        x11_take_last_error();
        XRenderPictureAttributes pa = {0};
        pa.subwindow_mode = IncludeInferiors;
        Picture src_pic = XRenderCreatePicture(sa->dpy, sa->root, fmt_root, CPSubwindowMode, &pa);
        Pixmap small_pm = XCreatePixmap(sa->dpy, sa->root, (unsigned int)bw, (unsigned int)bh, 32);
        Picture dst_pic = (small_pm != None) ? XRenderCreatePicture(sa->dpy, small_pm, fmt32, 0, NULL) : None;
        if (src_pic != None && dst_pic != None && x11_take_last_error() == 0) {
            XTransform xf = {{
                { XDoubleToFixed((double)wa.w / (double)bw), 0, XDoubleToFixed((double)wa.x) },
                { 0, XDoubleToFixed((double)wa.h / (double)bh), XDoubleToFixed((double)wa.y) },
                { 0, 0, XDoubleToFixed(1.0) }
            }};
            XRenderSetPictureTransform(sa->dpy, src_pic, &xf);
            XRenderSetPictureFilter(sa->dpy, src_pic, FilterGood, NULL, 0);
            XRenderComposite(sa->dpy, PictOpSrc, src_pic, None, dst_pic, 0, 0, 0, 0, 0, 0,
                             (unsigned int)bw, (unsigned int)bh);
            cairo_surface_t *xpm = cairo_xlib_surface_create(sa->dpy, small_pm, sa->visual, bw, bh);
            if (xpm && x11_take_last_error() == 0) {
                cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
                cairo_set_source_surface(cr, xpm, 0, 0);
                cairo_paint(cr);
                cairo_surface_destroy(xpm);
                ok = (x11_take_last_error() == 0);
            } else if (xpm) {
                cairo_surface_destroy(xpm);
            }
        }
        if (dst_pic != None) XRenderFreePicture(sa->dpy, dst_pic);
        if (small_pm != None) XFreePixmap(sa->dpy, small_pm);
        if (src_pic != None) XRenderFreePicture(sa->dpy, src_pic);
    }

    if (!ok) {
        cairo_set_source_rgb(cr, 0.12, 0.14, 0.18);
        cairo_paint(cr);
    }

    /* 3-pass separable blur at 1/6 scale (radius 5 => ~30px screen blur) */
    blur_image_surface(sa->blurred_bg, 5);

    /* Windows 11 Dark Acrylic tint overlay on the blurred surface */
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, 0.10, 0.11, 0.14, 0.42);
    cairo_paint(cr);
    cairo_destroy(cr);
}

static void draw_acrylic_panel(SnapAssist *sa, cairo_t *cr, Rect zone_local, double alpha) {
    int pad = 7;
    double rx = zone_local.x + pad;
    double ry = zone_local.y + pad;
    double rw = zone_local.w - 2 * pad;
    double rh = zone_local.h - 2 * pad;
    if (rw < 32 || rh < 32) return;

    cairo_save(cr);
    rounded_rect_path(cr, rx, ry, rw, rh, 8.0);
    cairo_clip(cr);

    double bg_alpha = sa->preview_only ? (alpha * 0.80) : (alpha * 0.90);
    if (sa->blurred_bg && sa->blur_w > 0 && sa->blur_h > 0) {
        cairo_save(cr);
        cairo_scale(cr, (double)sa->wa.w / (double)sa->blur_w,
                    (double)sa->wa.h / (double)sa->blur_h);
        cairo_set_source_surface(cr, sa->blurred_bg, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint_with_alpha(cr, bg_alpha);
        cairo_restore(cr);
    } else {
        cairo_set_source_rgba(cr, 0.13, 0.14, 0.17, bg_alpha * 0.86);
        cairo_paint(cr);
    }

    /* Subtle noise/luminance sheen matching Windows 11 Acrylic */
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (sa->preview_only ? 0.05 : 0.04) * alpha);
    cairo_paint(cr);
    cairo_restore(cr);

    /* 1px outer specular acrylic border */
    rounded_rect_path(cr, rx + 0.5, ry + 0.5, rw - 1.0, rh - 1.0, 8.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22 * alpha);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
}

/* Desktop Phantom Zone Preview while hovering zones in the Snap Layouts popup */
void assist_update_zone_preview(SnapAssist *sa, WorkArea wa, const Rect *target_zone,
                                Window popup_win, uint64_t now_ms) {
    sa->top_bar_active = false;
    sa->top_bar_hot_layout = -1;
    sa->top_bar_hot_zone = -1;

    if (!target_zone) {
        if (sa->visible && sa->preview_only && sa->preview_zone_active) {
            sa->preview_zone_active = false;
            sa->anim_active = false;
            assist_redraw(sa);
        }
        return;
    }

    bool wa_changed = (!sa->visible || !sa->preview_only ||
                       sa->wa.x != wa.x || sa->wa.y != wa.y ||
                       sa->wa.w != wa.w || sa->wa.h != wa.h);
    if (wa_changed) {
        free_all_session_resources(sa);
        sa->wa = wa;
        sa->preview_only = true;
        sa->preview_zone_active = true;
        sa->has_preview_rect = false;
        capture_blurred_workarea(sa, wa);
        XMoveResizeWindow(sa->dpy, sa->win, wa.x, wa.y, (unsigned int)wa.w, (unsigned int)wa.h);
        if (!sa->visible) {
            XMapRaised(sa->dpy, sa->win);
            sa->visible = true;
        }
        if (popup_win != None) {
            XRaiseWindow(sa->dpy, popup_win);
        }
        XFlush(sa->dpy);
    }

    sa->preview_zone_active = true;
    Rect local_target = {
        target_zone->x - wa.x,
        target_zone->y - wa.y,
        target_zone->w,
        target_zone->h
    };

    if (!sa->has_preview_rect) {
        sa->has_preview_rect = true;
        sa->preview_start_rect = local_target;
        sa->preview_cur_rect = local_target;
        sa->preview_target_rect = local_target;
        sa->anim_active = false;
        if (popup_win != None) XRaiseWindow(sa->dpy, popup_win);
        assist_redraw(sa);
        return;
    }

    if (sa->preview_target_rect.x != local_target.x ||
        sa->preview_target_rect.y != local_target.y ||
        sa->preview_target_rect.w != local_target.w ||
        sa->preview_target_rect.h != local_target.h) {
        sa->preview_start_rect = sa->preview_cur_rect;
        sa->preview_target_rect = local_target;
        sa->preview_anim_start_ms = now_ms;
        sa->preview_anim_dur_ms = 115;
        sa->anim_active = true;
    }
    if (popup_win != None) XRaiseWindow(sa->dpy, popup_win);
    assist_redraw(sa);
}

static void compute_top_bar_geometry(SnapAssist *sa, WorkArea wa,
                                     const SnapLayout layouts[MAX_LAYOUTS],
                                     const AppConfig *cfg) {
    int s = (cfg && cfg->dpi_scale >= 1) ? cfg->dpi_scale : 1;
    int tw = 74 * s;
    int th = 46 * s;
    int pad = 12 * s;
    int tgap = 12 * s;
    int cgap = 4 * s;
    int bar_w = 2 * pad + MAX_LAYOUTS * tw + (MAX_LAYOUTS - 1) * tgap;
    int bar_h = 2 * pad + th;
    int bar_x = (wa.w - bar_w) / 2;
    int bar_y = 8 * s;

    sa->top_bar_rect = (Rect){bar_x, bar_y, bar_w, bar_h};
    for (int l = 0; l < MAX_LAYOUTS; l++) {
        int lx = bar_x + pad + l * (tw + tgap);
        int ly = bar_y + pad;
        sa->top_bar_zone_counts[l] = layouts[l].num_zones;
        for (int z = 0; z < layouts[l].num_zones; z++) {
            SnapZone zn = layouts[l].zones[z];
            int zx0 = lx + (int)lround(zn.fx * (double)tw);
            int zx1 = lx + (int)lround((zn.fx + zn.fw) * (double)tw);
            int zy0 = ly + (int)lround(zn.fy * (double)th);
            int zy1 = ly + (int)lround((zn.fy + zn.fh) * (double)th);
            int hl = cgap / 2, hh = cgap - hl;
            if (zn.fx > 0.001) zx0 += hl;
            if (zn.fx + zn.fw < 0.999) zx1 -= hh;
            if (zn.fy > 0.001) zy0 += hl;
            if (zn.fy + zn.fh < 0.999) zy1 -= hh;
            sa->top_bar_zones[l][z] = (Rect){zx0, zy0, zx1 - zx0, zy1 - zy0};
        }
    }
}

void assist_update_drag_preview(SnapAssist *sa, WorkArea wa, bool show_top_bar,
                                int pointer_x, int pointer_y, const Rect *phantom_zone,
                                const SnapLayout layouts[MAX_LAYOUTS], const AppConfig *cfg,
                                int *out_bar_layout, int *out_bar_zone, uint64_t now_ms) {
    if (out_bar_layout) *out_bar_layout = -1;
    if (out_bar_zone) *out_bar_zone = -1;

    if (!show_top_bar && !phantom_zone) {
        if (sa->visible && sa->preview_only) {
            assist_hide(sa);
        }
        return;
    }

    bool wa_changed = (!sa->visible || !sa->preview_only ||
                       sa->wa.x != wa.x || sa->wa.y != wa.y ||
                       sa->wa.w != wa.w || sa->wa.h != wa.h);
    if (wa_changed) {
        free_all_session_resources(sa);
        sa->wa = wa;
        sa->preview_only = true;
        sa->has_preview_rect = false;
        capture_blurred_workarea(sa, wa);
        XMoveResizeWindow(sa->dpy, sa->win, wa.x, wa.y, (unsigned int)wa.w, (unsigned int)wa.h);
        if (!sa->visible) {
            XMapRaised(sa->dpy, sa->win);
            sa->visible = true;
        }
    }

    sa->top_bar_active = show_top_bar;
    sa->top_bar_hot_layout = -1;
    sa->top_bar_hot_zone = -1;
    Rect bar_zone_screen = {0};
    bool has_bar_zone = false;

    if (show_top_bar && layouts) {
        compute_top_bar_geometry(sa, wa, layouts, cfg);
        int lx = pointer_x - wa.x;
        int ly = pointer_y - wa.y;
        for (int l = 0; l < MAX_LAYOUTS; l++) {
            for (int z = 0; z < sa->top_bar_zone_counts[l]; z++) {
                if (rect_contains(sa->top_bar_zones[l][z], lx, ly)) {
                    sa->top_bar_hot_layout = l;
                    sa->top_bar_hot_zone = z;
                    if (out_bar_layout) *out_bar_layout = l;
                    if (out_bar_zone) *out_bar_zone = z;
                    bar_zone_screen = layouts_compute_zone_rect(wa, layouts[l].zones[z],
                                                              cfg ? cfg->gap_px : 0);
                    has_bar_zone = true;
                    break;
                }
            }
            if (has_bar_zone) break;
        }
    }

    const Rect *eff_zone = has_bar_zone ? &bar_zone_screen : phantom_zone;
    if (eff_zone) {
        Rect local_target = {
            eff_zone->x - wa.x,
            eff_zone->y - wa.y,
            eff_zone->w,
            eff_zone->h
        };
        sa->preview_zone_active = true;
        if (!sa->has_preview_rect) {
            sa->has_preview_rect = true;
            sa->preview_start_rect = local_target;
            sa->preview_cur_rect = local_target;
            sa->preview_target_rect = local_target;
            sa->anim_active = false;
        } else if (sa->preview_target_rect.x != local_target.x ||
                   sa->preview_target_rect.y != local_target.y ||
                   sa->preview_target_rect.w != local_target.w ||
                   sa->preview_target_rect.h != local_target.h) {
            sa->preview_start_rect = sa->preview_cur_rect;
            sa->preview_target_rect = local_target;
            sa->preview_anim_start_ms = now_ms;
            sa->preview_anim_dur_ms = (cfg && !cfg->enable_animations) ? 0 : 115;
            if (sa->preview_anim_dur_ms == 0) {
                sa->preview_cur_rect = local_target;
                sa->anim_active = false;
            } else {
                sa->anim_active = true;
            }
        }
    } else {
        sa->preview_zone_active = false;
        sa->has_preview_rect = false;
        sa->anim_active = false;
    }

    XRaiseWindow(sa->dpy, sa->win);
    assist_redraw(sa);
}

static cairo_surface_t *capture_window_thumbnail(SnapAssist *sa, const WindowInfo *info,
                                                 int max_w, int max_h,
                                                 int *out_w, int *out_h) {
    ensure_xcomposite_loaded();
    Display *dpy = sa->dpy;
    Window src_win = (info->frame_win != None) ? info->frame_win : info->win;
    XWindowAttributes wa;
    x11_take_last_error();
    if (!XGetWindowAttributes(dpy, src_win, &wa) || x11_take_last_error() != 0 ||
        wa.width <= 0 || wa.height <= 0) {
        return NULL;
    }

    /* Crop out client-side shadow (_GTK_FRAME_EXTENTS) if present and unmaximized */
    int crop_x = 0, crop_y = 0, crop_w = wa.width, crop_h = wa.height;
    if (src_win == info->win && info->has_gtk_frame && !info->is_maximized) {
        crop_x = info->gtk_frame.left;
        crop_y = info->gtk_frame.top;
        crop_w = wa.width - info->gtk_frame.left - info->gtk_frame.right;
        crop_h = wa.height - info->gtk_frame.top - info->gtk_frame.bottom;
        if (crop_w < 64 || crop_h < 48) {
            crop_x = 0;
            crop_y = 0;
            crop_w = wa.width;
            crop_h = wa.height;
        }
    }

    double sx = (double)max_w / (double)crop_w;
    double sy = (double)max_h / (double)crop_h;
    double s = sx < sy ? sx : sy;
    if (s > 1.0) s = 1.0;
    int tw = (int)lround(crop_w * s);
    int th = (int)lround(crop_h * s);
    if (tw < 32) tw = 32;
    if (th < 24) th = 24;

    Pixmap pix = None;
    if (g_name_window_pixmap) {
        x11_take_last_error();
        pix = g_name_window_pixmap(dpy, src_win);
        if (x11_take_last_error() != 0) pix = None;
    }

    Drawable draw = (pix != None) ? (Drawable)pix : (Drawable)src_win;
    cairo_surface_t *thumb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
    cairo_t *cr = cairo_create(thumb);
    cairo_set_source_rgb(cr, 0.09, 0.09, 0.11);
    cairo_paint(cr);

    /* Downscale directly on Xorg into a tiny tw x th Pixmap (~300 KB instead of 8 MB XGetImage) */
    bool xrender_ok = false;
    XRenderPictFormat *src_fmt = XRenderFindVisualFormat(dpy, wa.visual);
    XRenderPictFormat *fmt32 = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    if (src_fmt && fmt32) {
        x11_take_last_error();
        Picture src_pic = XRenderCreatePicture(dpy, draw, src_fmt, 0, NULL);
        Pixmap small_pm = XCreatePixmap(dpy, sa->root, (unsigned int)tw, (unsigned int)th, 32);
        Picture dst_pic = (small_pm != None) ? XRenderCreatePicture(dpy, small_pm, fmt32, 0, NULL) : None;
        if (src_pic != None && dst_pic != None && x11_take_last_error() == 0) {
            XRenderColor bg = {0x1700, 0x1700, 0x1c00, 0xffff};
            XRenderFillRectangle(dpy, PictOpSrc, dst_pic, &bg, 0, 0, (unsigned int)tw, (unsigned int)th);
            XTransform xf = {{
                { XDoubleToFixed((double)crop_w / (double)tw), 0, XDoubleToFixed((double)crop_x) },
                { 0, XDoubleToFixed((double)crop_h / (double)th), XDoubleToFixed((double)crop_y) },
                { 0, 0, XDoubleToFixed(1.0) }
            }};
            XRenderSetPictureTransform(dpy, src_pic, &xf);
            XRenderSetPictureFilter(dpy, src_pic, FilterGood, NULL, 0);
            XRenderComposite(dpy, PictOpOver, src_pic, None, dst_pic, 0, 0, 0, 0, 0, 0,
                             (unsigned int)tw, (unsigned int)th);
            cairo_surface_t *xpm = cairo_xlib_surface_create(dpy, small_pm, sa->visual, tw, th);
            if (xpm && x11_take_last_error() == 0) {
                cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
                cairo_set_source_surface(cr, xpm, 0, 0);
                cairo_paint(cr);
                cairo_surface_destroy(xpm);
                xrender_ok = (x11_take_last_error() == 0);
            } else if (xpm) {
                cairo_surface_destroy(xpm);
            }
        }
        if (dst_pic != None) XRenderFreePicture(dpy, dst_pic);
        if (small_pm != None) XFreePixmap(dpy, small_pm);
        if (src_pic != None) XRenderFreePicture(dpy, src_pic);
    }

    if (!xrender_ok) {
        x11_take_last_error();
        cairo_surface_t *xsurf = cairo_xlib_surface_create(dpy, draw, wa.visual, wa.width, wa.height);
        if (xsurf && x11_take_last_error() == 0) {
            cairo_save(cr);
            cairo_scale(cr, (double)tw / (double)crop_w, (double)th / (double)crop_h);
            cairo_set_source_surface(cr, xsurf, -crop_x, -crop_y);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_restore(cr);
        }
        if (xsurf) cairo_surface_destroy(xsurf);
    }

    cairo_destroy(cr);
    if (pix != None) {
        x11_take_last_error();
        XFreePixmap(dpy, pix);
        x11_take_last_error();
    }

    if (out_w) *out_w = tw;
    if (out_h) *out_h = th;
    return thumb;
}

cairo_surface_t *assist_capture_window_thumb(Display *dpy, const WindowInfo *info,
                                             int max_w, int max_h) {
    if (!dpy || !info || !info->valid) return NULL;
    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, DefaultScreen(dpy), 32, TrueColor, &vinfo)) return NULL;
    SnapAssist tmp = {0};
    tmp.dpy = dpy;
    tmp.root = DefaultRootWindow(dpy);
    tmp.visual = vinfo.visual;
    return capture_window_thumbnail(&tmp, info, max_w, max_h, NULL, NULL);
}

static void read_window_title(Display *dpy, const X11Atoms *atoms, Window win,
                              const char *fallback_class, char *out_title, size_t cap) {
    out_title[0] = '\0';
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;

    x11_take_last_error();
    if (XGetWindowProperty(dpy, win, atoms->net_wm_name, 0, 128, False, AnyPropertyType,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success &&
        data && nitems > 0) {
        snprintf(out_title, cap, "%s", (const char *)data);
        XFree(data);
        if (out_title[0]) return;
    } else if (data) {
        XFree(data);
    }

    data = NULL;
    x11_take_last_error();
    if (XGetWindowProperty(dpy, win, XA_WM_NAME, 0, 128, False, AnyPropertyType,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success &&
        data && nitems > 0) {
        snprintf(out_title, cap, "%s", (const char *)data);
        XFree(data);
        if (out_title[0]) return;
    } else if (data) {
        XFree(data);
    }

    snprintf(out_title, cap, "%s", (fallback_class && fallback_class[0]) ? fallback_class : "Janela");
}

static cairo_surface_t *create_fallback_icon(const char *wm_class) {
    int sz = 20;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sz, sz);
    cairo_t *cr = cairo_create(surf);

    /* Sleek Windows 11 rounded app tile with accent gradient */
    bool is_term = (wm_class && (strstr(wm_class, "term") || strstr(wm_class, "console")));
    rounded_rect_path(cr, 1.0, 1.0, 18.0, 18.0, 4.0);
    if (is_term) {
        cairo_set_source_rgb(cr, 0.16, 0.17, 0.20);
    } else {
        cairo_set_source_rgb(cr, 0.12, 0.56, 0.85);
    }
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.25);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    if (is_term) {
        /* ">_" prompt icon */
        cairo_set_source_rgb(cr, 0.92, 0.94, 0.96);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, 5.0, 7.0);
        cairo_line_to(cr, 8.5, 10.0);
        cairo_line_to(cr, 5.0, 13.0);
        cairo_move_to(cr, 10.5, 13.0);
        cairo_line_to(cr, 15.0, 13.0);
        cairo_stroke(cr);
    } else {
        /* Window header + body glyph */
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
        rounded_rect_path(cr, 4.5, 5.5, 11.0, 9.0, 1.5);
        cairo_set_line_width(cr, 1.3);
        cairo_stroke(cr);
        cairo_move_to(cr, 4.5, 8.5);
        cairo_line_to(cr, 15.5, 8.5);
        cairo_stroke(cr);
    }

    cairo_destroy(cr);
    return surf;
}

static cairo_surface_t *read_window_icon(Display *dpy, const X11Atoms *atoms,
                                         Window win, const char *wm_class) {
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;

    x11_take_last_error();
    if (XGetWindowProperty(dpy, win, atoms->net_wm_icon, 0, 65536, False, XA_CARDINAL,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) != Success ||
        !data || nitems < 64) {
        if (data) XFree(data);
        return create_fallback_icon(wm_class);
    }

    unsigned long *arr = (unsigned long *)data;
    unsigned long best_off = 0;
    int best_w = 0, best_h = 0;
    int best_score = 999999;

    unsigned long idx = 0;
    while (idx + 2 < nitems) {
        unsigned long iw = arr[idx];
        unsigned long ih = arr[idx + 1];
        if (iw == 0 || ih == 0 || iw > 256 || ih > 256 || idx + 2 + iw * ih > nitems) {
            break;
        }
        int diff = abs((int)iw - 32);
        if (diff < best_score) {
            best_score = diff;
            best_w = (int)iw;
            best_h = (int)ih;
            best_off = idx + 2;
        }
        idx += 2 + iw * ih;
    }

    if (best_w <= 0 || best_h <= 0) {
        XFree(data);
        return create_fallback_icon(wm_class);
    }

    cairo_surface_t *raw = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, best_w, best_h);
    cairo_surface_flush(raw);
    uint32_t *dst = (uint32_t *)cairo_image_surface_get_data(raw);
    int stride = cairo_image_surface_get_stride(raw) / 4;

    for (int y = 0; y < best_h; y++) {
        for (int x = 0; x < best_w; x++) {
            uint32_t argb = (uint32_t)arr[best_off + (unsigned long)(y * best_w + x)];
            uint8_t a = (uint8_t)((argb >> 24) & 0xff);
            uint8_t r = (uint8_t)(((argb >> 16) & 0xff) * a / 255);
            uint8_t g = (uint8_t)(((argb >> 8) & 0xff) * a / 255);
            uint8_t b = (uint8_t)((argb & 0xff) * a / 255);
            dst[y * stride + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                                  ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
    cairo_surface_mark_dirty(raw);
    XFree(data);

    cairo_surface_t *scaled = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 20, 20);
    cairo_t *cr = cairo_create(scaled);
    cairo_scale(cr, 20.0 / (double)best_w, 20.0 / (double)best_h);
    cairo_set_source_surface(cr, raw, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(raw);
    return scaled;
}

static bool window_is_hidden_or_skip(Display *dpy, Window win, const X11Atoms *atoms, int cur_desk) {
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;

    /* Check desktop */
    x11_take_last_error();
    if (XGetWindowProperty(dpy, win, atoms->net_wm_desktop, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success &&
        data && nitems >= 1) {
        long d = *(long *)data;
        XFree(data);
        if (d != -1 && (unsigned long)d != 0xFFFFFFFFUL && (int)d != cur_desk) {
            return true;
        }
    } else if (data) {
        XFree(data);
    }

    data = NULL;
    if (XGetWindowProperty(dpy, win, atoms->net_wm_state, 0, 32, False, XA_ATOM,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) == Success &&
        data) {
        Atom *st = (Atom *)data;
        bool skip = false;
        for (unsigned long i = 0; i < nitems; i++) {
            if (st[i] == atoms->net_wm_state_hidden ||
                st[i] == atoms->net_wm_state_skip_taskbar) {
                skip = true;
                break;
            }
        }
        XFree(data);
        if (skip) return true;
    } else if (data) {
        XFree(data);
    }

    XWindowAttributes wa;
    x11_take_last_error();
    if (!XGetWindowAttributes(dpy, win, &wa) || x11_take_last_error() != 0 ||
        wa.map_state != IsViewable) {
        return true;
    }
    return false;
}

/* Smooth 60 FPS morph animation when snapping a window into a target zone */
void assist_animate_snap_transition(SnapAssist *sa, Window target_win, Window frame_win,
                                    Rect start_rect, Rect target_rect,
                                    cairo_surface_t *optional_thumb) {
    if (!sa->dpy || sa->win == None) return;

    /* Compute union workarea bounding start_rect and target_rect */
    int x0 = start_rect.x < target_rect.x ? start_rect.x : target_rect.x;
    int y0 = start_rect.y < target_rect.y ? start_rect.y : target_rect.y;
    int x1 = (start_rect.x + start_rect.w) > (target_rect.x + target_rect.w)
           ? (start_rect.x + start_rect.w) : (target_rect.x + target_rect.w);
    int y1 = (start_rect.y + start_rect.h) > (target_rect.y + target_rect.h)
           ? (start_rect.y + start_rect.h) : (target_rect.y + target_rect.h);
    int uw = x1 - x0;
    int uh = y1 - y0;
    if (uw < 80 || uh < 60 || uw > 7680 || uh > 4320) return;

    cairo_surface_t *own_thumb = NULL;
    cairo_surface_t *thumb = optional_thumb;
    if (!thumb && target_win != None) {
        WindowInfo tmp = {0};
        tmp.win = target_win;
        tmp.frame_win = (frame_win != None) ? frame_win : target_win;
        own_thumb = capture_window_thumbnail(sa, &tmp, 480, 320, NULL, NULL);
        thumb = own_thumb;
    }

    XMoveResizeWindow(sa->dpy, sa->win, x0, y0, (unsigned int)uw, (unsigned int)uh);
    if (!sa->visible) {
        XMapRaised(sa->dpy, sa->win);
        sa->visible = true;
    } else {
        XRaiseWindow(sa->dpy, sa->win);
    }

    /* Server-side X11 Pixmap double-buffer (0 bytes of heap RAM in daemon) */
    x11_take_last_error();
    Pixmap buf_pm = XCreatePixmap(sa->dpy, sa->win, (unsigned int)uw, (unsigned int)uh, sa->depth);
    cairo_surface_t *buf = (buf_pm != None)
        ? cairo_xlib_surface_create(sa->dpy, buf_pm, sa->visual, uw, uh)
        : cairo_image_surface_create(CAIRO_FORMAT_ARGB32, uw, uh);
    cairo_t *bcr = cairo_create(buf);
    cairo_surface_t *xs = cairo_xlib_surface_create(sa->dpy, sa->win, sa->visual, uw, uh);
    cairo_t *xcr = cairo_create(xs);

    const int steps = 8;
    for (int step = 1; step <= steps; step++) {
        double t = (double)step / (double)steps;
        Rect cur = lerp_rect(start_rect, target_rect, t);
        double lx = cur.x - x0;
        double ly = cur.y - y0;
        double lw = cur.w;
        double lh = cur.h;

        cairo_save(bcr);
        cairo_set_operator(bcr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(bcr);
        cairo_restore(bcr);

        /* Soft elevation shadow around morphing window */
        rounded_rect_path(bcr, lx - 3.0, ly - 2.0, lw + 6.0, lh + 6.0, 10.0);
        cairo_set_source_rgba(bcr, 0.0, 0.0, 0.0, 0.24 * (1.0 - 0.3 * t));
        cairo_fill(bcr);

        cairo_save(bcr);
        rounded_rect_path(bcr, lx, ly, lw, lh, 8.0);
        cairo_clip(bcr);

        if (thumb) {
            int tw = cairo_image_surface_get_width(thumb);
            int th = cairo_image_surface_get_height(thumb);
            if (tw > 0 && th > 0) {
                cairo_save(bcr);
                cairo_translate(bcr, lx, ly);
                cairo_scale(bcr, lw / (double)tw, lh / (double)th);
                cairo_set_source_surface(bcr, thumb, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(bcr), CAIRO_FILTER_BILINEAR);
                cairo_paint_with_alpha(bcr, 0.94);
                cairo_restore(bcr);
            }
        } else {
            cairo_set_source_rgba(bcr, 0.12, 0.14, 0.18, 0.85);
            cairo_paint(bcr);
        }
        cairo_restore(bcr);

        /* Windows 11 accent border during snap morph */
        rounded_rect_path(bcr, lx + 0.5, ly + 0.5, lw - 1.0, lh - 1.0, 8.0);
        cairo_set_source_rgba(bcr, 96.0 / 255.0, 205.0 / 255.0, 1.0, 0.65 * (1.0 - 0.4 * t));
        cairo_set_line_width(bcr, 1.5);
        cairo_stroke(bcr);

        cairo_set_operator(xcr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(xcr, buf, 0, 0);
        cairo_paint(xcr);
        XFlush(sa->dpy);

        struct timespec ts = {0, 14 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    cairo_destroy(xcr);
    cairo_surface_destroy(xs);
    cairo_destroy(bcr);
    cairo_surface_destroy(buf);
    if (buf_pm != None) {
        XFreePixmap(sa->dpy, buf_pm);
    }
    if (own_thumb) cairo_surface_destroy(own_thumb);
}

static void layout_cards_in_current_zone(SnapAssist *sa) {
    if (sa->current_zone < 0 || sa->current_zone >= sa->num_zones || sa->num_cards <= 0) {
        return;
    }
    Rect z = sa->zone_rects[sa->current_zone];
    int n = sa->num_cards;

    /* Determine rows and columns per row matching Windows 11 Snap Assist */
    int rows = 1;
    int row_counts[3] = {0, 0, 0};
    int max_cols = 1;

    if (n == 1) {
        rows = 1;
        row_counts[0] = 1;
        max_cols = 1;
    } else if (n == 2) {
        if (z.w >= 480) {
            rows = 1;
            row_counts[0] = 2;
            max_cols = 2;
        } else {
            rows = 2;
            row_counts[0] = 1;
            row_counts[1] = 1;
            max_cols = 1;
        }
    } else if (n == 3) {
        /* Exact Windows 11 VM layout: 2 cards on top row, 1 centered card on bottom row */
        rows = 2;
        row_counts[0] = 2;
        row_counts[1] = 1;
        max_cols = 2;
    } else if (n == 4) {
        rows = 2;
        row_counts[0] = 2;
        row_counts[1] = 2;
        max_cols = 2;
    } else if (n == 5) {
        rows = 2;
        row_counts[0] = 3;
        row_counts[1] = 2;
        max_cols = 3;
    } else {
        rows = 2;
        row_counts[0] = 3;
        row_counts[1] = 3;
        max_cols = 3;
    }

    int pad_x = 34;
    int pad_y = 34;
    int gap_x = 22;
    int gap_y = 22;

    int avail_w = z.w - 2 * pad_x - (max_cols - 1) * gap_x;
    int avail_h = z.h - 2 * pad_y - (rows - 1) * gap_y;
    if (avail_w < 120) avail_w = 120;
    if (avail_h < 90) avail_h = 90;

    int cw = avail_w / max_cols;
    int ch = avail_h / rows;

    /* Clamp card aspect & maximum size to match Windows 11 proportions (~370x250 in half-screen, ~310x175 in quadrant) */
    int max_cw = (max_cols == 1) ? (int)(z.w * 0.68) : 372;
    int max_ch = (rows == 1) ? (int)(z.h * 0.56) : 252;
    if (cw > max_cw) cw = max_cw;
    if (ch > max_ch) ch = max_ch;
    if (ch > (int)(cw * 0.78)) ch = (int)(cw * 0.78);
    if (cw > (int)(ch * 1.85)) cw = (int)(ch * 1.85);
    if (cw < 130) cw = 130;
    if (ch < 96) ch = 96;

    int total_h = rows * ch + (rows - 1) * gap_y;
    int start_y = z.y + (z.h - total_h) / 2;

    int card_idx = 0;
    for (int r = 0; r < rows; r++) {
        int cols = row_counts[r];
        int row_w = cols * cw + (cols - 1) * gap_x;
        int start_x = z.x + (z.w - row_w) / 2;
        int cy = start_y + r * (ch + gap_y);
        for (int c = 0; c < cols && card_idx < n; c++) {
            int cx = start_x + c * (cw + gap_x);
            sa->cards[card_idx].card_rect = (Rect){cx, cy, cw, ch};
            sa->cards[card_idx].close_btn_rect = (Rect){cx + cw - 32, cy + 4, 28, 28};
            card_idx++;
        }
    }
}

static int find_first_unfilled_zone(const SnapAssist *sa) {
    for (int z = 0; z < sa->num_zones; z++) {
        if (!sa->zone_filled[z]) return z;
    }
    return -1;
}

bool assist_start_custom_layout(SnapAssist *sa, const X11Atoms *atoms, const AppConfig *cfg,
                                const WorkArea workareas[MAX_MONITORS], int num_workareas,
                                const Rect *abs_zones, int num_zones, int snapped_zone_idx,
                                Window snapped_win, uint64_t now_ms, bool debug) {
    if (!abs_zones || num_zones < 2 || num_zones > MAX_ZONES ||
        snapped_zone_idx < 0 || snapped_zone_idx >= num_zones) {
        return false;
    }

    WindowInfo snapped_info;
    if (!x11_read_window_info(sa->dpy, sa->root, atoms, cfg, snapped_win, &snapped_info)) {
        return false;
    }
    WorkArea wa = snap_pick_workarea(workareas, num_workareas, abs_zones[snapped_zone_idx]);

    free_all_session_resources(sa);
    sa->wa = wa;
    sa->preview_only = false;
    sa->top_bar_active = false;
    sa->has_preview_rect = false;
    sa->layout_idx = -1;
    sa->num_zones = num_zones;

    for (int z = 0; z < num_zones; z++) {
        sa->zone_rects[z] = (Rect){
            abs_zones[z].x - wa.x,
            abs_zones[z].y - wa.y,
            abs_zones[z].w,
            abs_zones[z].h
        };
        sa->zone_filled[z] = (z == snapped_zone_idx);
    }
    sa->current_zone = find_first_unfilled_zone(sa);
    if (sa->current_zone < 0) return false;

    /* Collect candidate windows from _NET_CLIENT_LIST_STACKING (top-to-bottom MRU order) */
    Atom actual_type = None;
    int actual_fmt = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    x11_take_last_error();
    if (XGetWindowProperty(sa->dpy, sa->root, atoms->net_client_list_stacking, 0, 256, False, XA_WINDOW,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &data) != Success || !data || nitems == 0) {
        if (data) XFree(data);
        data = NULL;
        if (XGetWindowProperty(sa->dpy, sa->root, atoms->net_client_list, 0, 256, False, XA_WINDOW,
                               &actual_type, &actual_fmt, &nitems, &bytes_after, &data) != Success || !data) {
            return false;
        }
    }

    Window *list = (Window *)data;
    int cur_desk = x11_get_current_desktop(sa->dpy, sa->root, atoms);

    /* Two passes: first windows on the same monitor workarea, then other monitors if < MAX_ASSIST_CARDS */
    for (int pass = 0; pass < 2 && sa->num_cards < MAX_ASSIST_CARDS; pass++) {
        for (long i = (long)nitems - 1; i >= 0 && sa->num_cards < MAX_ASSIST_CARDS; i--) {
            Window cand_w = list[i];
            if (cand_w == snapped_win || cand_w == sa->win) continue;

            bool already = false;
            for (int k = 0; k < sa->num_cards; k++) {
                if (sa->cards[k].win == cand_w) {
                    already = true;
                    break;
                }
            }
            if (already) continue;
            if (window_is_hidden_or_skip(sa->dpy, cand_w, atoms, cur_desk)) continue;

            WindowInfo cinfo;
            if (!x11_read_window_info(sa->dpy, sa->root, atoms, cfg, cand_w, &cinfo) || !cinfo.valid) {
                continue;
            }
            WorkArea cwa = snap_pick_workarea(workareas, num_workareas, cinfo.frame_rect);
            bool same_mon = (cwa.x == wa.x && cwa.y == wa.y);
            if ((pass == 0 && !same_mon) || (pass == 1 && same_mon)) continue;

            AssistCard *card = &sa->cards[sa->num_cards];
            memset(card, 0, sizeof(*card));
            card->win = cinfo.win;
            card->frame_win = cinfo.frame_win;
            card->src_frame_rect = cinfo.frame_rect;
            snprintf(card->wm_class, sizeof(card->wm_class), "%s", cinfo.wm_class);
            read_window_title(sa->dpy, atoms, cinfo.win, cinfo.wm_class,
                              card->title, sizeof(card->title));
            card->icon_surf = read_window_icon(sa->dpy, atoms, cinfo.win, cinfo.wm_class);
            card->thumb_surf = capture_window_thumbnail(sa, &cinfo, 360, 230,
                                                        &card->thumb_w, &card->thumb_h);
            sa->num_cards++;
        }
    }
    XFree(data);

    if (sa->num_cards == 0) {
        free_all_session_resources(sa);
        return false;
    }

    if (sa->visible) {
        XUnmapWindow(sa->dpy, sa->win);
        XSync(sa->dpy, False);
        sa->visible = false;
    }
    capture_blurred_workarea(sa, wa);
    layout_cards_in_current_zone(sa);

    sa->hovered_card = -1;
    sa->focused_card = 0;
    sa->hovered_card_close = false;
    sa->anim_active = cfg->enable_animations;
    sa->anim_start_ms = now_ms;
    sa->anim_dur_ms = cfg->enable_animations ? 150 : 0;
    sa->anim_progress = cfg->enable_animations ? 0.18 : 1.0;

    XMoveResizeWindow(sa->dpy, sa->win, wa.x, wa.y, (unsigned int)wa.w, (unsigned int)wa.h);
    XMapRaised(sa->dpy, sa->win);
    sa->visible = true;
    assist_redraw(sa);

    if (debug) {
        fprintf(stderr,
                "[snap-assist] Opened on monitor (%d,%d %dx%d) current_zone=%d/%d cards=%d\n",
                wa.x, wa.y, wa.w, wa.h, sa->current_zone, sa->num_zones, sa->num_cards);
    }
    return true;
}

bool assist_start_session(SnapAssist *sa, const X11Atoms *atoms, const AppConfig *cfg,
                          const WorkArea workareas[MAX_MONITORS], int num_workareas,
                          const SnapLayout layouts[MAX_LAYOUTS],
                          int layout_idx, int snapped_zone_idx,
                          Window snapped_win, uint64_t now_ms, bool debug) {
    if (layout_idx < 0 || layout_idx >= MAX_LAYOUTS) return false;
    const SnapLayout *lay = &layouts[layout_idx];
    if (snapped_zone_idx < 0 || snapped_zone_idx >= lay->num_zones || lay->num_zones < 2) {
        return false;
    }

    WindowInfo snapped_info;
    if (!x11_read_window_info(sa->dpy, sa->root, atoms, cfg, snapped_win, &snapped_info)) {
        return false;
    }
    WorkArea wa = snap_pick_workarea(workareas, num_workareas, snapped_info.frame_rect);

    Rect abs_zones[MAX_ZONES];
    for (int z = 0; z < lay->num_zones; z++) {
        abs_zones[z] = layouts_compute_zone_rect(wa, lay->zones[z], cfg->gap_px);
    }
    bool ok = assist_start_custom_layout(sa, atoms, cfg, workareas, num_workareas,
                                         abs_zones, lay->num_zones, snapped_zone_idx,
                                         snapped_win, now_ms, debug);
    if (ok) sa->layout_idx = layout_idx;
    return ok;
}

static void draw_single_card(cairo_t *cr, const AssistCard *c, bool is_hovered,
                             bool is_focused, bool close_hovered, double anim_eased) {
    double cx = c->card_rect.x;
    double cy = c->card_rect.y;
    double cw = c->card_rect.w;
    double ch = c->card_rect.h;
    double header_h = 36.0;

    /* Entrance scale (0.92 -> 1.0) + hover zoom (1.0 -> 1.022) around card center */
    double scale = (0.92 + 0.08 * anim_eased) * (is_hovered ? 1.022 : 1.0);
    double alpha = anim_eased;

    cairo_save(cr);
    double mid_x = cx + cw * 0.5;
    double mid_y = cy + ch * 0.5;
    cairo_translate(cr, mid_x, mid_y);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -mid_x, -mid_y);

    /* 1. Multi-layer elevation drop shadow */
    for (int s = 3; s >= 1; s--) {
        double sp = s * 2.5;
        rounded_rect_path(cr, cx - sp, cy - sp + 2.0, cw + 2.0 * sp, ch + 2.0 * sp, 8.0 + sp * 0.5);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, (is_hovered ? 0.13 : 0.09) * alpha);
        cairo_fill(cr);
    }

    /* 2. Clip to 8px rounded card body */
    cairo_save(cr);
    rounded_rect_path(cr, cx, cy, cw, ch, 8.0);
    cairo_clip(cr);

    /* Card dark body background */
    cairo_set_source_rgba(cr, 0.08, 0.08, 0.10, 0.96 * alpha);
    cairo_paint(cr);

    /* 3. Live Window Thumbnail inside body (cy + header_h .. cy + ch) */
    double body_y = cy + header_h;
    double body_h = ch - header_h;
    if (c->thumb_surf && c->thumb_w > 0 && c->thumb_h > 0 && body_h > 10.0) {
        /* Scale thumbnail to cover width cleanly while centered */
        double sx = cw / (double)c->thumb_w;
        double sy = body_h / (double)c->thumb_h;
        double ts = sx > sy ? sx : sy;
        /* Avoid cropping more than 25% on extreme aspect ratios */
        if (ts > sy * 1.28) ts = sy * 1.28;
        double draw_w = c->thumb_w * ts;
        double tx = cx + (cw - draw_w) * 0.5;
        double ty = body_y;

        cairo_save(cr);
        cairo_rectangle(cr, cx, body_y, cw, body_h);
        cairo_clip(cr);
        cairo_translate(cr, tx, ty);
        cairo_scale(cr, ts, ts);
        cairo_set_source_surface(cr, c->thumb_surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint_with_alpha(cr, alpha);
        cairo_restore(cr);
    }

    /* 4. Header Bar (36 px tall, matching Windows 11 Snap Assist card header) */
    rounded_top_path(cr, cx, cy, cw, header_h, 8.0);
    if (is_hovered) {
        cairo_set_source_rgba(cr, 0.19, 0.19, 0.23, 0.96 * alpha);
    } else {
        cairo_set_source_rgba(cr, 0.13, 0.13, 0.16, 0.93 * alpha);
    }
    cairo_fill(cr);

    /* Subtle 1px divider below header bar */
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08 * alpha);
    cairo_rectangle(cr, cx, cy + header_h - 1.0, cw, 1.0);
    cairo_fill(cr);

    /* 5. App Icon (18x18 at cx + 10, cy + 9) */
    if (c->icon_surf) {
        cairo_save(cr);
        cairo_translate(cr, cx + 10.0, cy + 9.0);
        cairo_scale(cr, 18.0 / 20.0, 18.0 / 20.0);
        cairo_set_source_surface(cr, c->icon_surf, 0, 0);
        cairo_paint_with_alpha(cr, alpha);
        cairo_restore(cr);
    }

    /* 6. Window Title (clipped before right edge / close button) */
    double text_x = cx + 35.0;
    double max_text_w = cw - (is_hovered ? 72.0 : 45.0);
    if (max_text_w > 24.0) {
        cairo_save(cr);
        cairo_rectangle(cr, text_x, cy, max_text_w, header_h);
        cairo_clip(cr);

        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12.0);
        cairo_set_source_rgba(cr, 0.96, 0.97, 0.98, 0.95 * alpha);

        /* Truncate with '...' if wider than max_text_w */
        char buf[140];
        snprintf(buf, sizeof(buf), "%s", c->title);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, buf, &ext);
        if (ext.width > max_text_w) {
            size_t len = strlen(buf);
            while (len > 4) {
                len--;
                buf[len] = '\0';
                char tmp[136];
                snprintf(tmp, sizeof(tmp), "%.128s...", buf);
                cairo_text_extents(cr, tmp, &ext);
                if (ext.width <= max_text_w) {
                    snprintf(buf, sizeof(buf), "%s", tmp);
                    break;
                }
            }
        }
        cairo_move_to(cr, text_x, cy + 22.5);
        cairo_show_text(cr, buf);
        cairo_restore(cr);
    }

    /* 7. Hover Close ('x') button on top-right of card header (Windows 11 Snap Assist feature) */
    if (is_hovered) {
        double bx = c->close_btn_rect.x;
        double by = c->close_btn_rect.y;
        double bw = c->close_btn_rect.w;
        double bh = c->close_btn_rect.h;
        if (close_hovered) {
            rounded_rect_path(cr, bx, by, bw, bh, 5.0);
            cairo_set_source_rgba(cr, 196.0 / 255.0, 43.0 / 255.0, 28.0 / 255.0, 0.96 * alpha);
            cairo_fill(cr);
        }
        double icx = bx + bw * 0.5;
        double icy = by + bh * 0.5;
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, (close_hovered ? 1.0 : 0.78) * alpha);
        cairo_set_line_width(cr, 1.2);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, icx - 4.0, icy - 4.0);
        cairo_line_to(cr, icx + 4.0, icy + 4.0);
        cairo_move_to(cr, icx + 4.0, icy - 4.0);
        cairo_line_to(cr, icx - 4.0, icy + 4.0);
        cairo_stroke(cr);
    }

    cairo_restore(cr); /* End card clip */

    /* 8. Outer Card Border (brighter cyan focus/hover ring matching Windows 11) */
    rounded_rect_path(cr, cx + 0.5, cy + 0.5, cw - 1.0, ch - 1.0, 8.0);
    if (is_hovered || is_focused) {
        cairo_set_source_rgba(cr, 96.0 / 255.0, 205.0 / 255.0, 1.0, is_hovered ? 0.90 * alpha : 0.85 * alpha);
        cairo_set_line_width(cr, is_focused && !is_hovered ? 2.0 : 1.75);
    } else {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.16 * alpha);
        cairo_set_line_width(cr, 1.0);
    }
    cairo_stroke(cr);

    cairo_restore(cr);
}

static void draw_top_snap_bar(SnapAssist *sa, cairo_t *cr) {
    if (!sa->top_bar_active) return;
    double bx = sa->top_bar_rect.x;
    double by = sa->top_bar_rect.y;
    double bw = sa->top_bar_rect.w;
    double bh = sa->top_bar_rect.h;

    /* Shadow beneath Top Snap Bar */
    for (int i = 3; i >= 1; i--) {
        double sp = i * 2.0;
        rounded_rect_path(cr, bx - sp * 0.5, by + i, bw + sp, bh + sp * 0.6, 10.0 + sp * 0.3);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.08 * (4 - i));
        cairo_fill(cr);
    }

    /* Dark Acrylic bar background */
    rounded_rect_path(cr, bx, by, bw, bh, 8.0);
    cairo_set_source_rgba(cr, 34.0 / 255.0, 34.0 / 255.0, 36.0 / 255.0, 0.96);
    cairo_fill(cr);

    rounded_rect_path(cr, bx + 0.5, by + 0.5, bw - 1.0, bh - 1.0, 7.5);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.16);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    for (int l = 0; l < MAX_LAYOUTS; l++) {
        for (int z = 0; z < sa->top_bar_zone_counts[l]; z++) {
            Rect zb = sa->top_bar_zones[l][z];
            bool hot = (l == sa->top_bar_hot_layout && z == sa->top_bar_hot_zone);
            rounded_rect_path(cr, zb.x + 0.5, zb.y + 0.5, zb.w - 1.0, zb.h - 1.0, 4.0);
            if (hot) {
                cairo_set_source_rgba(cr, 31.0 / 255.0, 158.0 / 255.0, 222.0 / 255.0, 0.92);
            } else {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.16);
            }
            cairo_fill_preserve(cr);
            cairo_set_line_width(cr, 1.0);
            if (hot) {
                cairo_set_source_rgba(cr, 96.0 / 255.0, 205.0 / 255.0, 1.0, 1.0);
            } else {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.36);
            }
            cairo_stroke(cr);
        }
    }
}

void assist_redraw(SnapAssist *sa) {
    if (!sa->visible || sa->win == None || sa->wa.w <= 0 || sa->wa.h <= 0) return;

    /* Render into a server-side X11 Pixmap (0 bytes of heap RAM in daemon) */
    x11_take_last_error();
    Pixmap buf_pm = XCreatePixmap(sa->dpy, sa->win, (unsigned int)sa->wa.w, (unsigned int)sa->wa.h, sa->depth);
    cairo_surface_t *img = (buf_pm != None)
        ? cairo_xlib_surface_create(sa->dpy, buf_pm, sa->visual, sa->wa.w, sa->wa.h)
        : cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sa->wa.w, sa->wa.h);
    cairo_t *cr = cairo_create(img);

    /* Clear entire workarea overlay to 100% transparent so snapped windows show through */
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    if (sa->preview_only) {
        if (sa->has_preview_rect && sa->preview_zone_active) {
            draw_acrylic_panel(sa, cr, sa->preview_cur_rect, 0.92);
            /* Accent highlight border on phantom zone preview */
            int pad = 7;
            double rx = sa->preview_cur_rect.x + pad + 0.5;
            double ry = sa->preview_cur_rect.y + pad + 0.5;
            double rw = sa->preview_cur_rect.w - 2 * pad - 1.0;
            double rh = sa->preview_cur_rect.h - 2 * pad - 1.0;
            if (rw > 16 && rh > 16) {
                rounded_rect_path(cr, rx, ry, rw, rh, 8.0);
                cairo_set_source_rgba(cr, 96.0 / 255.0, 205.0 / 255.0, 1.0, 0.65);
                cairo_set_line_width(cr, 1.5);
                cairo_stroke(cr);
            }
        }
        if (sa->top_bar_active) {
            draw_top_snap_bar(sa, cr);
        }
    } else {
        double eased = sa->anim_active ? cubic_ease_out(sa->anim_progress) : 1.0;

        /* 1. Draw Frosted Acrylic Glass Panels on ALL unfilled quadrants in the layout */
        for (int z = 0; z < sa->num_zones; z++) {
            if (!sa->zone_filled[z]) {
                draw_acrylic_panel(sa, cr, sa->zone_rects[z], eased);
            }
        }

        /* 2. Draw Candidate Window Thumbnail Cards inside current_zone */
        for (int i = 0; i < sa->num_cards; i++) {
            if (i == sa->hovered_card) continue; /* Draw hovered card last (on top) */
            draw_single_card(cr, &sa->cards[i], false, (i == sa->focused_card), false, eased);
        }
        if (sa->hovered_card >= 0 && sa->hovered_card < sa->num_cards) {
            draw_single_card(cr, &sa->cards[sa->hovered_card], true,
                             (sa->hovered_card == sa->focused_card),
                             sa->hovered_card_close, eased);
        }
    }

    cairo_destroy(cr);

    x11_take_last_error();
    cairo_surface_t *xs = cairo_xlib_surface_create(sa->dpy, sa->win, sa->visual,
                                                    sa->wa.w, sa->wa.h);
    cairo_t *xcr = cairo_create(xs);
    cairo_set_operator(xcr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(xcr, img, 0, 0);
    cairo_paint(xcr);
    cairo_destroy(xcr);
    cairo_surface_destroy(xs);
    cairo_surface_destroy(img);
    if (buf_pm != None) {
        XFreePixmap(sa->dpy, buf_pm);
    }
    XFlush(sa->dpy);
    x11_take_last_error();
}

bool assist_tick_animation(SnapAssist *sa, uint64_t now_ms) {
    if (!sa->visible || !sa->anim_active) return false;

    if (sa->preview_only) {
        uint64_t elapsed = (now_ms > sa->preview_anim_start_ms)
                         ? (now_ms - sa->preview_anim_start_ms) : 0;
        if (elapsed >= sa->preview_anim_dur_ms) {
            sa->preview_cur_rect = sa->preview_target_rect;
            sa->anim_active = false;
            assist_redraw(sa);
            return false;
        }
        double t = (double)elapsed / (double)sa->preview_anim_dur_ms;
        sa->preview_cur_rect = lerp_rect(sa->preview_start_rect, sa->preview_target_rect, t);
        assist_redraw(sa);
        return true;
    }

    uint64_t elapsed = (now_ms > sa->anim_start_ms) ? (now_ms - sa->anim_start_ms) : 0;
    if (elapsed >= sa->anim_dur_ms) {
        sa->anim_progress = 1.0;
        sa->anim_active = false;
        assist_redraw(sa);
        return false;
    }
    sa->anim_progress = (double)elapsed / (double)sa->anim_dur_ms;
    assist_redraw(sa);
    return true;
}

void assist_update_hover(SnapAssist *sa, int lx, int ly) {
    if (!sa->visible || sa->preview_only) return;
    int hit = -1;
    bool hit_close = false;
    for (int i = 0; i < sa->num_cards; i++) {
        if (rect_contains(sa->cards[i].card_rect, lx, ly)) {
            hit = i;
            hit_close = rect_contains(sa->cards[i].close_btn_rect, lx, ly);
            break;
        }
    }
    if (hit != sa->hovered_card || hit_close != sa->hovered_card_close) {
        sa->hovered_card = hit;
        sa->hovered_card_close = hit_close;
        assist_redraw(sa);
    }
}

static void activate_and_raise_window(Display *dpy, Window root, const X11Atoms *atoms, Window win) {
    XEvent ev = {0};
    ev.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = atoms->net_active_window;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2; /* pager/utility */
    ev.xclient.data.l[1] = CurrentTime;
    ev.xclient.data.l[2] = 0;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XRaiseWindow(dpy, win);
    XFlush(dpy);
}

static void send_close_window_msg(Display *dpy, Window root, const X11Atoms *atoms, Window win) {
    XEvent ev = {0};
    ev.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = atoms->net_close_window;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = 2;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

static void remove_card_at(SnapAssist *sa, int idx) {
    if (idx < 0 || idx >= sa->num_cards) return;
    free_card_surfaces(&sa->cards[idx]);
    for (int i = idx; i + 1 < sa->num_cards; i++) {
        sa->cards[i] = sa->cards[i + 1];
    }
    memset(&sa->cards[sa->num_cards - 1], 0, sizeof(AssistCard));
    sa->num_cards--;
    sa->hovered_card = -1;
    sa->hovered_card_close = false;
}

void assist_move_focus(SnapAssist *sa, int dx, int dy) {
    if (!sa->visible || sa->preview_only || sa->num_cards <= 0) return;
    if (sa->focused_card < 0 || sa->focused_card >= sa->num_cards) {
        sa->focused_card = 0;
        assist_redraw(sa);
        return;
    }
    int n = sa->num_cards;
    if (dx != 0 && dy == 0) {
        sa->focused_card = (sa->focused_card + dx + n) % n;
    } else if (dy != 0) {
        int cols = (n <= 2) ? n : (n <= 4 ? 2 : 3);
        int cand = sa->focused_card + dy * cols;
        if (cand >= 0 && cand < n) {
            sa->focused_card = cand;
        } else {
            sa->focused_card = (sa->focused_card + dy + n) % n;
        }
    }
    assist_redraw(sa);
}

bool assist_select_card_index(SnapAssist *sa, const X11Atoms *atoms, const AppConfig *cfg,
                              const WorkArea workareas[MAX_MONITORS], int num_workareas,
                              const SnapLayout layouts[MAX_LAYOUTS],
                              int card_idx, uint64_t now_ms, bool debug,
                              Window *out_last_snapped) {
    (void)layouts;
    (void)workareas;
    (void)num_workareas;
    if (out_last_snapped) *out_last_snapped = None;
    if (!sa->visible || sa->preview_only || card_idx < 0 || card_idx >= sa->num_cards ||
        sa->current_zone < 0 || sa->current_zone >= sa->num_zones) {
        assist_hide(sa);
        return false;
    }

    Window target_win = sa->cards[card_idx].win;
    Window target_frame = sa->cards[card_idx].frame_win;
    Rect start_screen_rect = {
        sa->wa.x + sa->cards[card_idx].card_rect.x,
        sa->wa.y + sa->cards[card_idx].card_rect.y,
        sa->cards[card_idx].card_rect.w,
        sa->cards[card_idx].card_rect.h
    };
    Rect target_screen_rect = {
        sa->wa.x + sa->zone_rects[sa->current_zone].x,
        sa->wa.y + sa->zone_rects[sa->current_zone].y,
        sa->zone_rects[sa->current_zone].w,
        sa->zone_rects[sa->current_zone].h
    };

    /* Activate & snap the selected candidate window into current_zone */
    activate_and_raise_window(sa->dpy, sa->root, atoms, target_win);
    snap_apply_rect(sa->dpy, sa->root, atoms, cfg, sa->wa, target_screen_rect, target_win, debug);

    /* Animate card expanding smoothly into the quadrant */
    if (cfg->enable_animations) {
        assist_animate_snap_transition(sa, target_win, target_frame,
                                       start_screen_rect, target_screen_rect,
                                       sa->cards[card_idx].thumb_surf);
    }

    if (out_last_snapped) *out_last_snapped = target_win;

    /* Mark current quadrant as filled and remove chosen window from candidate list */
    sa->zone_filled[sa->current_zone] = true;
    remove_card_at(sa, card_idx);

    int next_zone = find_first_unfilled_zone(sa);
    if (next_zone < 0 || sa->num_cards <= 0) {
        if (debug) {
            fprintf(stderr, "[snap-assist] All quadrants filled (or no candidates left), finishing\n");
        }
        assist_hide(sa);
        return false;
    }

    /* Advance Snap Assist to the next unfilled quadrant! */
    sa->current_zone = next_zone;
    sa->focused_card = 0;
    layout_cards_in_current_zone(sa);
    sa->anim_active = cfg->enable_animations;
    sa->anim_start_ms = monotonic_ms();
    (void)now_ms;
    sa->anim_dur_ms = cfg->enable_animations ? 150 : 0;
    sa->anim_progress = cfg->enable_animations ? 0.18 : 1.0;

    XMoveResizeWindow(sa->dpy, sa->win, sa->wa.x, sa->wa.y,
                      (unsigned int)sa->wa.w, (unsigned int)sa->wa.h);
    XRaiseWindow(sa->dpy, sa->win);
    assist_redraw(sa);

    if (debug) {
        fprintf(stderr, "[snap-assist] Advanced to next quadrant zone=%d/%d remaining_cards=%d\n",
                sa->current_zone, sa->num_zones, sa->num_cards);
    }
    return true;
}

bool assist_handle_click(SnapAssist *sa, const X11Atoms *atoms, const AppConfig *cfg,
                         const WorkArea workareas[MAX_MONITORS], int num_workareas,
                         const SnapLayout layouts[MAX_LAYOUTS],
                         int lx, int ly, uint64_t now_ms, bool debug,
                         Window *out_last_snapped) {
    if (out_last_snapped) *out_last_snapped = None;
    if (!sa->visible || sa->preview_only) {
        assist_hide(sa);
        return false;
    }

    int hit = -1;
    bool hit_close = false;
    for (int i = 0; i < sa->num_cards; i++) {
        if (rect_contains(sa->cards[i].card_rect, lx, ly)) {
            hit = i;
            hit_close = rect_contains(sa->cards[i].close_btn_rect, lx, ly);
            break;
        }
    }

    if (hit < 0) {
        /* Clicked outside cards -> dismiss Snap Assist immediately */
        if (debug) fprintf(stderr, "[snap-assist] Clicked outside cards (%d,%d), dismissing\n", lx, ly);
        assist_hide(sa);
        return false;
    }

    if (hit_close) {
        /* Clicked the 'x' close button on top-right of card header -> close that app & remove card */
        Window close_w = sa->cards[hit].win;
        if (debug) fprintf(stderr, "[snap-assist] Closing window 0x%lx from card close button\n", (unsigned long)close_w);
        send_close_window_msg(sa->dpy, sa->root, atoms, close_w);
        remove_card_at(sa, hit);
        if (sa->num_cards <= 0) {
            assist_hide(sa);
            return false;
        }
        layout_cards_in_current_zone(sa);
        assist_redraw(sa);
        return true;
    }

    return assist_select_card_index(sa, atoms, cfg, workareas, num_workareas, layouts,
                                    hit, now_ms, debug, out_last_snapped);
}
