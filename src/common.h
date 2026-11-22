#ifndef WIN11_SNAP_COMMON_H
#define WIN11_SNAP_COMMON_H

#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_MONITORS 16
#define MAX_PROFILES 24
#define MAX_LAYOUTS 6
#define MAX_ZONES 4

typedef struct {
    int x;
    int y;
    int w;
    int h;
} Rect;

typedef struct {
    int left;
    int right;
    int top;
    int bottom;
} Extents;

typedef struct {
    int x;
    int y;
    int w;
    int h;
} WorkArea;

typedef enum {
    WIN_KIND_NONE = 0,
    WIN_KIND_SSD,
    WIN_KIND_GTK_CSD,
    WIN_KIND_CUSTOM_CSD
} WindowKind;

typedef struct {
    int button_width;
    int button_height;
    int button_gap;
    int right_offset;
    int top_offset;
} ButtonGeometry;

typedef struct {
    char wm_class[64];
    bool enabled;
    bool overlay_buttons;
    ButtonGeometry geom;
} ClassProfile;

typedef struct {
    Window win;
    Window frame_win;
    WindowKind kind;
    Rect client_rect;
    Rect frame_rect;
    Extents net_frame;
    Extents gtk_frame;
    Rect max_btn_rect;
    ButtonGeometry applied_geom;
    bool has_net_frame;
    bool has_gtk_frame;
    bool has_win11_max_button_prop;
    bool wants_button_overlay;
    bool can_maximize;
    bool is_maximized;
    bool is_fullscreen;
    bool valid;
    char wm_class[64];
} WindowInfo;

typedef struct {
    double fx;
    double fy;
    double fw;
    double fh;
} SnapZone;

typedef struct {
    const char *name;
    int num_zones;
    SnapZone zones[MAX_ZONES];
} SnapLayout;

typedef struct {
    double r;
    double g;
    double b;
    double a;
} ColorRGBA;

typedef struct {
    ColorRGBA card_bg;
    ColorRGBA card_border;
    ColorRGBA zone_bg;
    ColorRGBA zone_border;
    ColorRGBA zone_hover_bg;
    ColorRGBA zone_hover_border;
} PopupColors;

typedef struct {
    char snap_layouts[64];
    char snap_left[64];
    char snap_right[64];
    char snap_up[64];
    char snap_down[64];
    char snap_top_half[64];
    char snap_bottom_half[64];
} ShortcutConfig;

typedef struct {
    char language[16];
    int hover_delay_ms;
    int dismiss_grace_ms;
    int motion_throttle_ms;
    int gap_px;
    double ratio_split;

    bool fallback_custom_csd;
    bool decorate_custom_csd;
    bool hotkey_super_z;
    bool enable_animations;
    bool enable_snap_assist;
    bool drag_snap;
    bool top_snap_bar;
    bool enable_shortcuts;
    char ignore_wm_classes[256];

    ShortcutConfig shortcuts;

    int dpi_scale;
    char button_layout[128];
    char theme_name[128];
    char theme_geometry_file[512];

    ButtonGeometry ssd;
    ButtonGeometry csd_gtk3;
    ButtonGeometry csd_gtk4;
    ButtonGeometry custom_csd;

    ClassProfile profiles[MAX_PROFILES];
    int num_profiles;

    PopupColors colors;
    int card_radius;
    int zone_radius;
    int padding;
    int thumb_width;
    int thumb_height;
    int thumb_gap;
    int cell_gap;
    int anchor_gap;
} AppConfig;

typedef enum {
    STATE_IDLE = 0,
    STATE_ARMED,
    STATE_POPUP_VISIBLE,
    STATE_SNAP_ASSIST
} DaemonState;

static inline bool rect_contains(Rect r, int px, int py) {
    return r.w > 0 && r.h > 0 &&
           px >= r.x && px < (r.x + r.w) &&
           py >= r.y && py < (r.y + r.h);
}

static inline int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

#endif
