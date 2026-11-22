#include "layouts.h"

#include <math.h>

void layouts_build(double ratio_split, SnapLayout out[MAX_LAYOUTS]) {
    if (ratio_split < 0.55 || ratio_split > 0.85) {
        ratio_split = 0.70;
    }

    /* 0: 50 / 50 */
    out[0] = (SnapLayout){
        .name = "50/50",
        .num_zones = 2,
        .zones = {
            {0.0, 0.0, 0.5, 1.0},
            {0.5, 0.0, 0.5, 1.0},
        }
    };

    /* 1: 70 / 30 (configurable ratio_split) */
    out[1] = (SnapLayout){
        .name = "70/30",
        .num_zones = 2,
        .zones = {
            {0.0, 0.0, ratio_split, 1.0},
            {ratio_split, 0.0, 1.0 - ratio_split, 1.0},
        }
    };

    /* 2: 3 Equal Columns */
    out[2] = (SnapLayout){
        .name = "3 Columns",
        .num_zones = 3,
        .zones = {
            {0.0, 0.0, 1.0 / 3.0, 1.0},
            {1.0 / 3.0, 0.0, 1.0 / 3.0, 1.0},
            {2.0 / 3.0, 0.0, 1.0 / 3.0, 1.0},
        }
    };

    /* 3: 50% Left + 2 Quadrants Right */
    out[3] = (SnapLayout){
        .name = "50 + 2Q",
        .num_zones = 3,
        .zones = {
            {0.0, 0.0, 0.5, 1.0},
            {0.5, 0.0, 0.5, 0.5},
            {0.5, 0.5, 0.5, 0.5},
        }
    };

    /* 4: 4 Quadrants (2x2) */
    out[4] = (SnapLayout){
        .name = "4 Quadrants",
        .num_zones = 4,
        .zones = {
            {0.0, 0.0, 0.5, 0.5},
            {0.5, 0.0, 0.5, 0.5},
            {0.0, 0.5, 0.5, 0.5},
            {0.5, 0.5, 0.5, 0.5},
        }
    };

    /* 5: 25 / 50 / 25 (Focus center) */
    out[5] = (SnapLayout){
        .name = "25/50/25",
        .num_zones = 3,
        .zones = {
            {0.0, 0.0, 0.25, 1.0},
            {0.25, 0.0, 0.50, 1.0},
            {0.75, 0.0, 0.25, 1.0},
        }
    };
}

Rect layouts_compute_zone_rect(WorkArea wa, SnapZone zone, int gap_px) {
    int x0 = wa.x + (int)lround(zone.fx * (double)wa.w);
    int x1 = wa.x + (int)lround((zone.fx + zone.fw) * (double)wa.w);
    int y0 = wa.y + (int)lround(zone.fy * (double)wa.h);
    int y1 = wa.y + (int)lround((zone.fy + zone.fh) * (double)wa.h);

    if (gap_px > 0) {
        int half_lo = gap_px / 2;
        int half_hi = gap_px - half_lo;
        x0 += (zone.fx <= 0.001) ? gap_px : half_lo;
        x1 -= (zone.fx + zone.fw >= 0.999) ? gap_px : half_hi;
        y0 += (zone.fy <= 0.001) ? gap_px : half_lo;
        y1 -= (zone.fy + zone.fh >= 0.999) ? gap_px : half_hi;
    }

    Rect r = {x0, y0, x1 - x0, y1 - y0};
    if (r.w < 50) r.w = 50;
    if (r.h < 50) r.h = 50;
    return r;
}
