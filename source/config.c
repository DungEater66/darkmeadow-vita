/*
 * The player's settings from config.txt (see config.h).
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_PATH DATA_PATH "config.txt"
#define MAX_ENTRIES 128

typedef struct {
    char key[48];
    char value[200];
} entry_t;

static entry_t entries[MAX_ENTRIES];
static int count;
static int loaded;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

void config_load(void) {
    if (loaded) return;
    loaded = 1;
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f) && count < MAX_ENTRIES) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *v = s + strcspn(s, " \t");
        if (*v) *v++ = 0;
        snprintf(entries[count].key, sizeof(entries[count].key), "%s", s);
        snprintf(entries[count].value, sizeof(entries[count].value), "%s", trim(v));
        count++;
    }
    fclose(f);
}

const char *config_str(const char *key, const char *def) {
    config_load();
    /* the last line wins, as when a key appears twice */
    for (int i = count - 1; i >= 0; i--)
        if (!strcmp(entries[i].key, key)) return entries[i].value;
    return def;
}

int config_int(const char *key, int def) {
    const char *v = config_str(key, NULL);
    return v && *v ? atoi(v) : def;
}

float config_float(const char *key, float def) {
    const char *v = config_str(key, NULL);
    return v && *v ? (float)atof(v) : def;
}

int config_bool(const char *key, int def) {
    return config_int(key, def) != 0;
}
