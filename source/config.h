/*
 * The player's settings: ux0:data/darkmeadow/config.txt, one "KEY value" per line, written by the settings app
 * (configurator/). A missing key means its default. Read once at start; every module asks here.
 */
#ifndef DM_CONFIG_H
#define DM_CONFIG_H

void config_load(void);

int config_int(const char *key, int def);
float config_float(const char *key, float def);
int config_bool(const char *key, int def);
/* The rest of the line after the key (several words allowed), or `def`. */
const char *config_str(const char *key, const char *def);

#endif
