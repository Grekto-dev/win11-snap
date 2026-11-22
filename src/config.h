#ifndef WIN11_SNAP_CONFIG_H
#define WIN11_SNAP_CONFIG_H

#include "common.h"

void config_init_defaults(AppConfig *cfg);
void config_load(AppConfig *cfg, const char *explicit_config_path);

#endif
