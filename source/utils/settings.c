/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include "settings.h"

#define CONFIG_FILE_PATH DATA_PATH"config.txt"

int  setting_sampleSetting;
bool setting_sampleSetting2;

void settings_reset() {
    setting_sampleSetting  = 1;
    setting_sampleSetting2 = true;
}

void settings_load() {
    settings_reset();

    /* One "name value" per line. config.txt also holds shader switches (see utils/gldiag.c) and possibly comments:
     * read line by line with bounded fields, so a long or unexpected line can neither overflow a buffer nor stall. */
    FILE *config = fopen(CONFIG_FILE_PATH, "r");

    if (config) {
        char line[128], buffer[64];
        int value;
        while (fgets(line, sizeof(line), config)) {
            if (sscanf(line, "%63s %d", buffer, &value) != 2) continue;
            if 		(strcmp("setting_sampleSetting", buffer) == 0) 	setting_sampleSetting  = (int)value;
            else if (strcmp("setting_sampleSetting2", buffer) == 0) setting_sampleSetting2 = (bool)value;
        }
        fclose(config);
    }
}

void settings_save() {
    FILE *config = fopen(CONFIG_FILE_PATH, "w+");

    if (config) {
        fprintf(config, "%s %d\n", "setting_sampleSetting", (int)(setting_sampleSetting));
        fprintf(config, "%s %d\n", "setting_sampleSetting2", (int)(setting_sampleSetting2));
        fclose(config);
    }
}
