#ifndef NDI_RECEIVER_H
#define NDI_RECEIVER_H

#include <stdint.h>
#include <stdbool.h>

#define NDI_MAX_SOURCES 64
#define NDI_SOURCE_NAME_LEN 256

typedef struct {
    char name[NDI_SOURCE_NAME_LEN];
    char url[NDI_SOURCE_NAME_LEN];
} ndi_source_info_t;

// Callback when source list changes
typedef void (*ndi_source_list_callback_t)(const ndi_source_info_t* sources, int count, void* userdata);

// Callback when a video frame is received (BGRX data)
typedef void (*ndi_frame_callback_t)(const uint8_t* data, int width, int height, int stride, void* userdata);

// Callback when NDI connection is lost
typedef void (*ndi_disconnect_callback_t)(void* userdata);

// Initialize NDI library (dynamic load)
int ndi_init(void);

// Start NDI source discovery
int ndi_start_discovery(ndi_source_list_callback_t callback, void* userdata);

// Stop discovery
void ndi_stop_discovery(void);

// Connect to an NDI source by name and url
int ndi_connect(const char* source_name, const char* source_url);

// Disconnect from current source
void ndi_disconnect(void);

// Set frame callback
void ndi_set_frame_callback(ndi_frame_callback_t callback, void* userdata);

// Set disconnect callback (called from receive thread when connection is lost)
void ndi_set_disconnect_callback(ndi_disconnect_callback_t callback, void* userdata);

// Check if connected
bool ndi_is_connected(void);

// Shutdown NDI
void ndi_shutdown(void);

#endif
