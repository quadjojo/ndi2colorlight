#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void config_defaults(led_config_t* config) {
    config->panel_width = 64;
    config->panel_height = 32;
    config->panels_x = 6;
    config->panels_y = 4;
    config->brightness = 100;
    config->gamma = 1.0f;
}

static char* strip(char* s) {
    while (isspace((unsigned char)*s)) s++;
    char* end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

int config_load(const char* path, led_config_t* config) {
    config_defaults(config);

    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Warning: cannot open config '%s', using defaults\n", path);
        return -1;
    }

    char line[256];
    int line_num = 0;
    while (fgets(line, sizeof(line), f)) {
        line_num++;
        char* s = strip(line);

        // Skip empty lines and comments
        if (*s == '\0' || *s == '#') continue;

        char* eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "Config line %d: no '=' found, skipping\n", line_num);
            continue;
        }

        *eq = '\0';
        char* key = strip(s);
        char* val = strip(eq + 1);

        if (strcmp(key, "panel_width") == 0)       config->panel_width = atoi(val);
        else if (strcmp(key, "panel_height") == 0)  config->panel_height = atoi(val);
        else if (strcmp(key, "panels_x") == 0)      config->panels_x = atoi(val);
        else if (strcmp(key, "panels_y") == 0)       config->panels_y = atoi(val);
        else if (strcmp(key, "brightness") == 0)     config->brightness = atoi(val);
        else if (strcmp(key, "gamma") == 0)          config->gamma = (float)atof(val);
        else fprintf(stderr, "Config line %d: unknown key '%s'\n", line_num, key);
    }

    fclose(f);
    return 0;
}
