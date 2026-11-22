#ifndef WIN11_SNAP_LAYOUTS_H
#define WIN11_SNAP_LAYOUTS_H

#include "common.h"

void layouts_build(double ratio_split, SnapLayout out[MAX_LAYOUTS]);
Rect layouts_compute_zone_rect(WorkArea wa, SnapZone zone, int gap_px);

#endif
