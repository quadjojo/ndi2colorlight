#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    int panel_width;
    int panel_height;
    int panels_x;
    int panels_y;
    int brightness;   // 0-100
    float gamma;
} led_config_t;

// Load config from INI-style file. Returns 0 on success.
// Missing fields keep their default values.
int config_load(const char* path, led_config_t* config);

// Set defaults
void config_defaults(led_config_t* config);

// Computed total resolution
static inline int config_total_width(const led_config_t* c) { return c->panel_width * c->panels_x; }
static inline int config_total_height(const led_config_t* c) { return c->panel_height * c->panels_y; }

#endif
