#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <pthread.h>

#include "config.h"
#include "ndi_receiver.h"
#include "colorlight_output.h"
#include "frame_convert.h"

// App states
typedef enum {
    STATE_SEARCHING,
    STATE_CONNECTED
} app_state_t;

// Global state
static volatile sig_atomic_t running = 1;
static app_state_t state = STATE_SEARCHING;  // Protected by source_mutex
static led_config_t config;
static int out_w, out_h;

// Frame buffer (protected by frame_lock, pre-allocated at startup)
static uint8_t* output_bgr = NULL;
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;

// Source discovery (protected by source_mutex)
static ndi_source_info_t found_sources[NDI_MAX_SOURCES];
static int found_count = 0;
static pthread_mutex_t source_mutex = PTHREAD_MUTEX_INITIALIZER;

// Test pattern buffer (pre-allocated at startup)
static uint8_t* test_pattern_buf = NULL;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

static void frame_callback(const uint8_t* data, int width, int height, int stride, void* userdata) {
    (void)userdata;

    // Non-blocking: skip frame if previous still processing
    if (pthread_mutex_trylock(&frame_lock) != 0) return;

    if (width == out_w && height == out_h) {
        frame_convert_direct(data, width, height, stride, output_bgr, config.gamma);
    } else {
        frame_convert_scale(data, width, height, stride, output_bgr, out_w, out_h, config.gamma);
    }

    colorlight_send_frame(output_bgr, out_w, out_h);
    pthread_mutex_unlock(&frame_lock);
}

static void discovery_callback(const ndi_source_info_t* sources, int count, void* userdata) {
    (void)userdata;
    pthread_mutex_lock(&source_mutex);
    found_count = count < NDI_MAX_SOURCES ? count : NDI_MAX_SOURCES;
    memcpy(found_sources, sources, found_count * sizeof(ndi_source_info_t));
    pthread_mutex_unlock(&source_mutex);
}

static void on_ndi_disconnect(void* userdata) {
    (void)userdata;
    pthread_mutex_lock(&source_mutex);
    state = STATE_SEARCHING;
    pthread_mutex_unlock(&source_mutex);
    fprintf(stderr, "NDI disconnected, switching to test pattern\n");
}

static void send_corner_test_pattern(void) {
    int size = out_w * out_h * 3;
    memset(test_pattern_buf, 0, size);

    // Top-left: RED (BGR = 0,0,255)
    test_pattern_buf[0] = 0; test_pattern_buf[1] = 0; test_pattern_buf[2] = 255;
    // Top-right: GREEN (BGR = 0,255,0)
    int tr = (out_w - 1) * 3;
    test_pattern_buf[tr] = 0; test_pattern_buf[tr+1] = 255; test_pattern_buf[tr+2] = 0;
    // Bottom-left: BLUE (BGR = 255,0,0)
    int bl = (out_h - 1) * out_w * 3;
    test_pattern_buf[bl] = 255; test_pattern_buf[bl+1] = 0; test_pattern_buf[bl+2] = 0;
    // Bottom-right: WHITE (BGR = 255,255,255)
    int br = ((out_h - 1) * out_w + (out_w - 1)) * 3;
    test_pattern_buf[br] = 255; test_pattern_buf[br+1] = 255; test_pattern_buf[br+2] = 255;

    colorlight_send_frame(test_pattern_buf, out_w, out_h);
}

static void print_usage(const char* prog) {
    fprintf(stderr,
        "ndi-led-cli — NDI to Colorlight LED Wall\n\n"
        "Usage: %s [OPTIONS]\n\n"
        "Options:\n"
        "  -s, --source NAME      NDI source name (substring match)\n"
        "  -c, --config FILE      Config file (default: ./wall.conf)\n"
        "  -i, --interface IF     Network interface for LED output (e.g. eth1)\n"
        "  -l, --list             List available NDI sources and exit\n"
        "  -b, --brightness N     Override brightness (0-100)\n"
        "  -h, --help             Show this help\n\n"
        "Example:\n"
        "  sudo %s -s \"MyPC\" -i eth1 -c wall.conf\n\n",
        prog, prog);
}

int main(int argc, char* argv[]) {
    const char* source_name = NULL;
    const char* config_path = "wall.conf";
    const char* interface = NULL;
    int brightness_override = -1;
    bool list_mode = false;

    static struct option long_options[] = {
        {"source",     required_argument, 0, 's'},
        {"config",     required_argument, 0, 'c'},
        {"interface",  required_argument, 0, 'i'},
        {"list",       no_argument,       0, 'l'},
        {"brightness", required_argument, 0, 'b'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:c:i:lb:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 's': source_name = optarg; break;
            case 'c': config_path = optarg; break;
            case 'i': interface = optarg; break;
            case 'l': list_mode = true; break;
            case 'b': brightness_override = atoi(optarg); break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }

    // Load and validate config
    config_load(config_path, &config);
    if (brightness_override >= 0) {
        config.brightness = brightness_override;
    }

    if (config.panel_width <= 0 || config.panel_height <= 0 ||
        config.panels_x <= 0 || config.panels_y <= 0) {
        fprintf(stderr, "Error: invalid panel dimensions in config (all must be > 0)\n");
        return 1;
    }
    if (config.gamma < 0.1f) config.gamma = 0.1f;

    out_w = config_total_width(&config);
    out_h = config_total_height(&config);
    fprintf(stderr, "LED wall: %dx%d (%dx%d panels, %dx%d each)\n",
            out_w, out_h, config.panels_x, config.panels_y,
            config.panel_width, config.panel_height);
    fprintf(stderr, "Brightness: %d%%, Gamma: %.2f\n", config.brightness, config.gamma);

    // Initialize NDI
    if (ndi_init() != 0) {
        fprintf(stderr, "Failed to initialize NDI\n");
        return 1;
    }

    // List mode
    if (list_mode) {
        fprintf(stderr, "Searching for NDI sources (5 seconds)...\n");
        ndi_start_discovery(discovery_callback, NULL);
        for (int i = 0; i < 5 && running; i++) {
            sleep(1);
        }
        ndi_stop_discovery();

        pthread_mutex_lock(&source_mutex);
        if (found_count == 0) {
            printf("No NDI sources found.\n");
        } else {
            printf("Found %d NDI source(s):\n", found_count);
            for (int i = 0; i < found_count; i++) {
                printf("  [%d] %s\n", i + 1, found_sources[i].name);
            }
        }
        pthread_mutex_unlock(&source_mutex);

        ndi_shutdown();
        return 0;
    }

    // Validate required arguments
    if (!source_name) {
        fprintf(stderr, "Error: --source is required (use --list to find sources)\n");
        ndi_shutdown();
        return 1;
    }
    if (!interface) {
        fprintf(stderr, "Error: --interface is required (e.g. --interface eth1)\n");
        ndi_shutdown();
        return 1;
    }

    // Signal handlers (sigaction for well-defined behavior)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    // Pre-allocate frame buffers
    int buf_size = out_w * out_h * 3;
    output_bgr = (uint8_t*)malloc(buf_size);
    test_pattern_buf = (uint8_t*)malloc(buf_size);
    if (!output_bgr || !test_pattern_buf) {
        fprintf(stderr, "Error: failed to allocate frame buffers (%d bytes)\n", buf_size * 2);
        free(output_bgr);
        free(test_pattern_buf);
        ndi_shutdown();
        return 1;
    }

    // Open Colorlight output
    if (colorlight_open(interface) != 0) {
        fprintf(stderr, "Failed to open Colorlight output on %s\n", interface);
        free(output_bgr);
        free(test_pattern_buf);
        ndi_shutdown();
        return 1;
    }

    colorlight_set_config(config.panel_width, config.panel_height,
                          config.panels_x, config.panels_y);

    uint8_t brightness_val = (uint8_t)(config.brightness * 255 / 100);
    colorlight_set_brightness(brightness_val);

    // Setup callbacks
    ndi_set_frame_callback(frame_callback, NULL);
    ndi_set_disconnect_callback(on_ndi_disconnect, NULL);
    ndi_start_discovery(discovery_callback, NULL);

    fprintf(stderr, "Searching for NDI source \"%s\"...\n", source_name);

    // State machine main loop
    struct timespec last_test_pattern = {0, 0};
    int search_log_count = 0;
    app_state_t current_state;

    while (running) {
        // Read state under lock
        pthread_mutex_lock(&source_mutex);
        current_state = state;
        pthread_mutex_unlock(&source_mutex);

        if (current_state == STATE_SEARCHING) {
            // Send test pattern at ~1fps
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - last_test_pattern.tv_sec) +
                           (now.tv_nsec - last_test_pattern.tv_nsec) / 1e9;
            if (elapsed >= 1.0) {
                send_corner_test_pattern();
                last_test_pattern = now;
            }

            // Check for matching NDI source in current list
            pthread_mutex_lock(&source_mutex);
            for (int i = 0; i < found_count; i++) {
                if (strstr(found_sources[i].name, source_name) != NULL) {
                    char name_buf[NDI_SOURCE_NAME_LEN];
                    char url_buf[NDI_SOURCE_NAME_LEN];
                    memcpy(name_buf, found_sources[i].name, NDI_SOURCE_NAME_LEN);
                    name_buf[NDI_SOURCE_NAME_LEN - 1] = '\0';
                    memcpy(url_buf, found_sources[i].url, NDI_SOURCE_NAME_LEN);
                    url_buf[NDI_SOURCE_NAME_LEN - 1] = '\0';
                    pthread_mutex_unlock(&source_mutex);

                    fprintf(stderr, "Found NDI source: %s\n", name_buf);
                    if (ndi_connect(name_buf, url_buf) == 0) {
                        pthread_mutex_lock(&source_mutex);
                        state = STATE_CONNECTED;
                        pthread_mutex_unlock(&source_mutex);
                        search_log_count = 0;
                        fprintf(stderr, "NDI connected, streaming\n");
                    } else {
                        fprintf(stderr, "NDI connect failed, retrying...\n");
                    }
                    goto next_iteration;
                }
            }
            pthread_mutex_unlock(&source_mutex);

            search_log_count++;
            if (search_log_count % 50 == 0) {
                fprintf(stderr, "Searching for \"%s\"... (%d sources visible)\n",
                        source_name, found_count);
            }

            usleep(100000);  // 100ms
        } else {
            // STATE_CONNECTED — frame_callback handles everything
            usleep(500000);  // 500ms
        }

        next_iteration:;
    }

    fprintf(stderr, "\nShutting down...\n");

    ndi_disconnect();
    ndi_stop_discovery();
    ndi_shutdown();
    colorlight_close();

    free(output_bgr);
    free(test_pattern_buf);

    fprintf(stderr, "Done.\n");
    return 0;
}
