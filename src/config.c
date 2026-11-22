#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

static void str_tolower_copy(char *dst, size_t dst_sz, const char *src) {
    size_t i = 0;
    for (; i + 1 < dst_sz && src[i]; i++) {
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static bool parse_bool(const char *v, bool fallback) {
    if (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcasecmp(v, "yes") == 0) return true;
    if (strcasecmp(v, "false") == 0 || strcmp(v, "0") == 0 || strcasecmp(v, "no") == 0) return false;
    return fallback;
}

static bool parse_hex_color(const char *s, ColorRGBA *out) {
    if (!s || s[0] != '#') return false;
    const char *hex = s + 1;
    size_t len = strlen(hex);
    if (len != 6 && len != 8) return false;
    unsigned int r = 0, g = 0, b = 0, a = 255;
    if (len == 6) {
        if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) != 3) return false;
    } else {
        if (sscanf(hex, "%02x%02x%02x%02x", &r, &g, &b, &a) != 4) return false;
    }
    out->r = (double)r / 255.0;
    out->g = (double)g / 255.0;
    out->b = (double)b / 255.0;
    out->a = (double)a / 255.0;
    return true;
}

static void query_gsettings_str(const char *cmd, char *out, size_t out_sz) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    char buf[256];
    if (fgets(buf, sizeof(buf), fp)) {
        char *t = trim_ws(buf);
        size_t l = strlen(t);
        if (l >= 2 && t[0] == '\'' && t[l - 1] == '\'') {
            t[l - 1] = '\0';
            t++;
        }
        if (t[0]) {
            snprintf(out, out_sz, "%s", t);
        }
    }
    pclose(fp);
}

static ClassProfile *find_or_add_profile(AppConfig *cfg, const char *name_raw) {
    char low[64];
    str_tolower_copy(low, sizeof(low), name_raw);
    for (int i = 0; i < cfg->num_profiles; i++) {
        if (strcmp(cfg->profiles[i].wm_class, low) == 0) {
            return &cfg->profiles[i];
        }
    }
    if (cfg->num_profiles >= MAX_PROFILES) return NULL;
    ClassProfile *p = &cfg->profiles[cfg->num_profiles++];
    memset(p, 0, sizeof(*p));
    snprintf(p->wm_class, sizeof(p->wm_class), "%s", low);
    p->enabled = true;
    p->overlay_buttons = true;
    p->geom = cfg->custom_csd;
    return p;
}

static void apply_kv(AppConfig *cfg, const char *section, const char *key, const char *val) {
    if (strcmp(section, "general") == 0) {
        if (strcmp(key, "language") == 0 && val[0]) {
            snprintf(cfg->language, sizeof(cfg->language), "%s", val);
        } else if (strcmp(key, "hover_delay_ms") == 0) cfg->hover_delay_ms = clamp_int(atoi(val), 80, 3000);
        else if (strcmp(key, "dismiss_grace_ms") == 0) cfg->dismiss_grace_ms = clamp_int(atoi(val), 0, 2000);
        else if (strcmp(key, "motion_throttle_ms") == 0) cfg->motion_throttle_ms = clamp_int(atoi(val), 8, 200);
        else if (strcmp(key, "gap_px") == 0) cfg->gap_px = clamp_int(atoi(val), 0, 48);
        else if (strcmp(key, "ratio_split") == 0) {
            double r = atof(val);
            if (r >= 0.55 && r <= 0.85) cfg->ratio_split = r;
        } else if (strcmp(key, "fallback_custom_csd") == 0) {
            cfg->fallback_custom_csd = parse_bool(val, cfg->fallback_custom_csd);
        } else if (strcmp(key, "decorate_custom_csd") == 0) {
            cfg->decorate_custom_csd = parse_bool(val, cfg->decorate_custom_csd);
        } else if (strcmp(key, "hotkey_super_z") == 0) {
            cfg->hotkey_super_z = parse_bool(val, cfg->hotkey_super_z);
        } else if (strcmp(key, "enable_animations") == 0) {
            cfg->enable_animations = parse_bool(val, cfg->enable_animations);
        } else if (strcmp(key, "enable_snap_assist") == 0) {
            cfg->enable_snap_assist = parse_bool(val, cfg->enable_snap_assist);
        } else if (strcmp(key, "drag_snap") == 0) {
            cfg->drag_snap = parse_bool(val, cfg->drag_snap);
        } else if (strcmp(key, "top_snap_bar") == 0) {
            cfg->top_snap_bar = parse_bool(val, cfg->top_snap_bar);
        } else if (strcmp(key, "enable_shortcuts") == 0) {
            cfg->enable_shortcuts = parse_bool(val, cfg->enable_shortcuts);
        } else if (strcmp(key, "ignore_wm_classes") == 0) {
            str_tolower_copy(cfg->ignore_wm_classes, sizeof(cfg->ignore_wm_classes), val);
        } else if (strcmp(key, "theme_geometry_file") == 0 && val[0]) {
            snprintf(cfg->theme_geometry_file, sizeof(cfg->theme_geometry_file), "%s", val);
        }
    } else if (strcmp(section, "shortcuts") == 0) {
        if (strcmp(key, "snap_layouts") == 0) snprintf(cfg->shortcuts.snap_layouts, sizeof(cfg->shortcuts.snap_layouts), "%s", val);
        else if (strcmp(key, "snap_left") == 0) snprintf(cfg->shortcuts.snap_left, sizeof(cfg->shortcuts.snap_left), "%s", val);
        else if (strcmp(key, "snap_right") == 0) snprintf(cfg->shortcuts.snap_right, sizeof(cfg->shortcuts.snap_right), "%s", val);
        else if (strcmp(key, "snap_up") == 0) snprintf(cfg->shortcuts.snap_up, sizeof(cfg->shortcuts.snap_up), "%s", val);
        else if (strcmp(key, "snap_down") == 0) snprintf(cfg->shortcuts.snap_down, sizeof(cfg->shortcuts.snap_down), "%s", val);
        else if (strcmp(key, "snap_top_half") == 0) snprintf(cfg->shortcuts.snap_top_half, sizeof(cfg->shortcuts.snap_top_half), "%s", val);
        else if (strcmp(key, "snap_bottom_half") == 0) snprintf(cfg->shortcuts.snap_bottom_half, sizeof(cfg->shortcuts.snap_bottom_half), "%s", val);
    } else if (strcmp(section, "theme") == 0) {
        if (strcmp(key, "button_layout") == 0 && val[0]) {
            snprintf(cfg->button_layout, sizeof(cfg->button_layout), "%s", val);
        } else if (strcmp(key, "name") == 0 && val[0]) {
            snprintf(cfg->theme_name, sizeof(cfg->theme_name), "%s", val);
        }
    } else if (strcmp(section, "ssd") == 0 || strcmp(section, "csd_gtk3") == 0 ||
               strcmp(section, "csd_gtk4") == 0 || strcmp(section, "custom_csd") == 0) {
        ButtonGeometry *g = (strcmp(section, "ssd") == 0) ? &cfg->ssd
                          : (strcmp(section, "csd_gtk3") == 0) ? &cfg->csd_gtk3
                          : (strcmp(section, "csd_gtk4") == 0) ? &cfg->csd_gtk4
                          : &cfg->custom_csd;
        if (strcmp(key, "button_width") == 0) g->button_width = clamp_int(atoi(val), 16, 120);
        else if (strcmp(key, "button_height") == 0) g->button_height = clamp_int(atoi(val), 16, 120);
        else if (strcmp(key, "button_gap") == 0) g->button_gap = clamp_int(atoi(val), 0, 40);
        else if (strcmp(key, "right_offset") == 0) g->right_offset = clamp_int(atoi(val), 0, 80);
        else if (strcmp(key, "top_offset") == 0) g->top_offset = clamp_int(atoi(val), 0, 80);
    } else if (strcmp(section, "popup") == 0) {
        if (strcmp(key, "card_bg") == 0) parse_hex_color(val, &cfg->colors.card_bg);
        else if (strcmp(key, "card_border") == 0) parse_hex_color(val, &cfg->colors.card_border);
        else if (strcmp(key, "zone_bg") == 0) parse_hex_color(val, &cfg->colors.zone_bg);
        else if (strcmp(key, "zone_border") == 0) parse_hex_color(val, &cfg->colors.zone_border);
        else if (strcmp(key, "zone_hover_bg") == 0) parse_hex_color(val, &cfg->colors.zone_hover_bg);
        else if (strcmp(key, "zone_hover_border") == 0) parse_hex_color(val, &cfg->colors.zone_hover_border);
        else if (strcmp(key, "card_radius") == 0) cfg->card_radius = clamp_int(atoi(val), 0, 24);
        else if (strcmp(key, "zone_radius") == 0) cfg->zone_radius = clamp_int(atoi(val), 0, 12);
        else if (strcmp(key, "padding") == 0) cfg->padding = clamp_int(atoi(val), 4, 32);
        else if (strcmp(key, "thumb_width") == 0) cfg->thumb_width = clamp_int(atoi(val), 40, 160);
        else if (strcmp(key, "thumb_height") == 0) cfg->thumb_height = clamp_int(atoi(val), 28, 120);
        else if (strcmp(key, "thumb_gap") == 0) cfg->thumb_gap = clamp_int(atoi(val), 4, 32);
        else if (strcmp(key, "cell_gap") == 0) cfg->cell_gap = clamp_int(atoi(val), 1, 12);
        else if (strcmp(key, "anchor_gap") == 0) cfg->anchor_gap = clamp_int(atoi(val), 0, 24);
    } else if (strncmp(section, "profile:", 8) == 0) {
        ClassProfile *p = find_or_add_profile(cfg, section + 8);
        if (!p) return;
        if (strcmp(key, "enabled") == 0) p->enabled = parse_bool(val, p->enabled);
        else if (strcmp(key, "overlay_buttons") == 0) p->overlay_buttons = parse_bool(val, p->overlay_buttons);
        else if (strcmp(key, "button_width") == 0) p->geom.button_width = clamp_int(atoi(val), 16, 120);
        else if (strcmp(key, "button_height") == 0) p->geom.button_height = clamp_int(atoi(val), 16, 120);
        else if (strcmp(key, "button_gap") == 0) p->geom.button_gap = clamp_int(atoi(val), 0, 40);
        else if (strcmp(key, "right_offset") == 0) p->geom.right_offset = clamp_int(atoi(val), 0, 80);
        else if (strcmp(key, "top_offset") == 0) p->geom.top_offset = clamp_int(atoi(val), 0, 80);
    }
}

static bool parse_ini_file(AppConfig *cfg, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    char line[512];
    char section[96] = "general";
    while (fgets(line, sizeof(line), fp)) {
        char *s = trim_ws(line);
        if (!s[0] || s[0] == '#' || s[0] == ';') continue;
        if (s[0] == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                str_tolower_copy(section, sizeof(section), trim_ws(s + 1));
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char key[96];
        str_tolower_copy(key, sizeof(key), trim_ws(s));
        char *val = trim_ws(eq + 1);
        for (char *p = val; *p; p++) {
            if ((*p == '#' || *p == ';') && p > val && isspace((unsigned char)p[-1])) {
                *p = '\0';
                val = trim_ws(val);
                break;
            }
        }
        apply_kv(cfg, section, key, val);
    }
    fclose(fp);
    return true;
}

void config_init_defaults(AppConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->language, sizeof(cfg->language), "en");
    cfg->hover_delay_ms = 450;
    cfg->dismiss_grace_ms = 180;
    cfg->motion_throttle_ms = 30;
    cfg->gap_px = 0;
    cfg->ratio_split = 0.70;
    cfg->fallback_custom_csd = true;
    cfg->decorate_custom_csd = true;
    cfg->hotkey_super_z = true;
    cfg->enable_animations = true;
    cfg->enable_snap_assist = true;
    cfg->drag_snap = true;
    cfg->top_snap_bar = true;
    cfg->enable_shortcuts = true;
    cfg->dpi_scale = 1;

    /* Exact Windows 11 Snap keyboard shortcuts */
    snprintf(cfg->shortcuts.snap_layouts, sizeof(cfg->shortcuts.snap_layouts), "<Super>z");
    snprintf(cfg->shortcuts.snap_left, sizeof(cfg->shortcuts.snap_left), "<Super>Left");
    snprintf(cfg->shortcuts.snap_right, sizeof(cfg->shortcuts.snap_right), "<Super>Right");
    snprintf(cfg->shortcuts.snap_up, sizeof(cfg->shortcuts.snap_up), "<Super>Up");
    snprintf(cfg->shortcuts.snap_down, sizeof(cfg->shortcuts.snap_down), "<Super>Down");
    snprintf(cfg->shortcuts.snap_top_half, sizeof(cfg->shortcuts.snap_top_half), "<Super><Alt>Up");
    snprintf(cfg->shortcuts.snap_bottom_half, sizeof(cfg->shortcuts.snap_bottom_half), "<Super><Alt>Down");

    snprintf(cfg->button_layout, sizeof(cfg->button_layout), ":minimize,maximize,close");
    snprintf(cfg->theme_name, sizeof(cfg->theme_name), "Mint-Y-Dark-Aqua-Win11");

    cfg->ssd = (ButtonGeometry){46, 32, 6, 0, 0};
    cfg->csd_gtk3 = (ButtonGeometry){46, 46, 0, 0, 0};
    cfg->csd_gtk4 = (ButtonGeometry){46, 32, 0, 0, 0};
    cfg->custom_csd = (ButtonGeometry){46, 34, 0, 0, 0};

    parse_hex_color("#222224F5", &cfg->colors.card_bg);
    parse_hex_color("#FFFFFF24", &cfg->colors.card_border);
    parse_hex_color("#FFFFFF26", &cfg->colors.zone_bg);
    parse_hex_color("#FFFFFF59", &cfg->colors.zone_border);
    parse_hex_color("#1F9EDEE8", &cfg->colors.zone_hover_bg);
    parse_hex_color("#60CDFFFF", &cfg->colors.zone_hover_border);

    cfg->card_radius = 8;
    cfg->zone_radius = 4;
    cfg->padding = 14;
    cfg->thumb_width = 76;
    cfg->thumb_height = 48;
    cfg->thumb_gap = 12;
    cfg->cell_gap = 4;
    cfg->anchor_gap = 6;
}

void config_load(AppConfig *cfg, const char *explicit_config_path) {
    config_init_defaults(cfg);

    query_gsettings_str("gsettings get org.cinnamon.desktop.interface gtk-theme 2>/dev/null",
                        cfg->theme_name, sizeof(cfg->theme_name));
    query_gsettings_str("gsettings get org.cinnamon.desktop.wm.preferences button-layout 2>/dev/null",
                        cfg->button_layout, sizeof(cfg->button_layout));

    const char *home = getenv("HOME");
    char user_cfg[512] = "";
    if (explicit_config_path && explicit_config_path[0]) {
        snprintf(user_cfg, sizeof(user_cfg), "%s", explicit_config_path);
    } else if (home) {
        snprintf(user_cfg, sizeof(user_cfg), "%s/.config/win11-snap/config.ini", home);
    }

    if (user_cfg[0]) {
        parse_ini_file(cfg, user_cfg);
    }

    char geom_path[512] = "";
    if (cfg->theme_geometry_file[0]) {
        snprintf(geom_path, sizeof(geom_path), "%s", cfg->theme_geometry_file);
    } else if (home) {
        snprintf(geom_path, sizeof(geom_path), "%s/.themes/%s/snap-geometry.conf",
                 home, cfg->theme_name);
        FILE *t = fopen(geom_path, "r");
        if (t) {
            fclose(t);
        } else {
            snprintf(geom_path, sizeof(geom_path), "%s/.local/share/themes/%s/snap-geometry.conf",
                     home, cfg->theme_name);
            t = fopen(geom_path, "r");
            if (t) {
                fclose(t);
            } else {
                snprintf(geom_path, sizeof(geom_path), "/usr/share/themes/%s/snap-geometry.conf",
                         cfg->theme_name);
            }
        }
    }

    if (geom_path[0]) {
        parse_ini_file(cfg, geom_path);
    }

    if (user_cfg[0]) {
        parse_ini_file(cfg, user_cfg);
    }
}
