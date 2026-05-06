#ifndef COLORLIGHT_OUTPUT_H
#define COLORLIGHT_OUTPUT_H

#include <stdint.h>
#include <stdbool.h>

// Max pixels per Colorlight packet
#define CL_MAX_PIXELS_PER_PACKET 497
#define CL_BYTES_PER_PIXEL 3

// Open raw socket on given interface (e.g. "eth1")
int colorlight_open(const char* interface_name);

// Configure panel layout
void colorlight_set_config(int panel_w, int panel_h, int panels_x, int panels_y);

// Set brightness 0-255
void colorlight_set_brightness(uint8_t brightness);

// Send a full frame of BGR pixel data (width x height x 3 bytes)
int colorlight_send_frame(const uint8_t* bgr_data, int width, int height);

// Close socket
void colorlight_close(void);

// Check if output is open
bool colorlight_is_open(void);

#endif
