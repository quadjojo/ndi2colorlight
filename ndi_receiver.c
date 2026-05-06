#include "ndi_receiver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

// NDI SDK headers
#include <Processing.NDI.Lib.h>
#include <Processing.NDI.structs.h>
#include <Processing.NDI.Find.h>
#include <Processing.NDI.Recv.h>
#include <Processing.NDI.DynamicLoad.h>

// NDI function table
static const NDIlib_v5* ndi = NULL;

// Discovery state
static NDIlib_find_instance_t finder = NULL;
static pthread_t discovery_thread;
static volatile bool discovery_running = false;
static ndi_source_list_callback_t discovery_callback = NULL;
static void* discovery_userdata = NULL;

// Receiver state
static NDIlib_recv_instance_t receiver = NULL;
static pthread_t receive_thread;
static volatile bool receive_running = false;
static ndi_frame_callback_t frame_callback = NULL;
static void* frame_userdata = NULL;
static ndi_disconnect_callback_t disconnect_callback = NULL;
static void* disconnect_userdata = NULL;
static volatile bool connected = false;

int ndi_init(void) {
    const char* ndi_paths[] = {
        "libndi.so.6",
        "/usr/local/lib/libndi.so.6",
        "/usr/lib/libndi.so.6",
        "libndi.so",
        NULL
    };

    void* handle = NULL;

    // Check NDI runtime env variable first
    const char* ndi_runtime = getenv("NDI_RUNTIME_DIR_V6");
    if (ndi_runtime) {
        char path[512];
        snprintf(path, sizeof(path), "%s/libndi.so.6", ndi_runtime);
        handle = dlopen(path, RTLD_LOCAL | RTLD_LAZY);
        if (!handle) {
            snprintf(path, sizeof(path), "%s/libndi.so", ndi_runtime);
            handle = dlopen(path, RTLD_LOCAL | RTLD_LAZY);
        }
    }

    if (!handle) {
        for (int i = 0; ndi_paths[i]; i++) {
            handle = dlopen(ndi_paths[i], RTLD_LOCAL | RTLD_LAZY);
            if (handle) break;
        }
    }

    if (!handle) {
        fprintf(stderr, "Failed to load NDI library: %s\n", dlerror());
        fprintf(stderr, "Install NDI SDK v6 for Linux: https://ndi.video/for-developers/ndi-sdk/download/\n");
        return -1;
    }

    typedef const NDIlib_v5* (*NDIlib_v5_load_fn)(void);
    NDIlib_v5_load_fn load_fn = (NDIlib_v5_load_fn)dlsym(handle, "NDIlib_v5_load");
    if (!load_fn) {
        fprintf(stderr, "Failed to find NDIlib_v5_load: %s\n", dlerror());
        dlclose(handle);
        return -1;
    }

    ndi = load_fn();
    if (!ndi) {
        fprintf(stderr, "NDIlib_v5_load returned NULL\n");
        dlclose(handle);
        return -1;
    }

    if (!ndi->initialize()) {
        fprintf(stderr, "NDI initialize failed\n");
        return -1;
    }

    const char* version = ndi->version();
    fprintf(stderr, "NDI initialized: %s\n", version ? version : "unknown");
    return 0;
}

static void* discovery_thread_func(void* arg) {
    (void)arg;

    NDIlib_find_create_t find_settings;
    memset(&find_settings, 0, sizeof(find_settings));
    find_settings.show_local_sources = true;
    find_settings.p_groups = NULL;
    find_settings.p_extra_ips = NULL;

    ndi_source_info_t sources[NDI_MAX_SOURCES];

    while (discovery_running) {
        // When connected, destroy finder to free resources and prevent stale state
        if (connected) {
            if (finder) {
                ndi->find_destroy(finder);
                finder = NULL;
            }
            for (int i = 0; i < 5 && connected && discovery_running; i++) {
                sleep(1);
            }
            continue;
        }

        // Not connected — create fresh finder if needed (fresh mDNS state)
        if (!finder) {
            finder = ndi->find_create_v2(&find_settings);
            if (!finder) {
                fprintf(stderr, "Failed to create NDI finder, retrying...\n");
                sleep(1);
                continue;
            }
            fprintf(stderr, "NDI discovery active (fresh finder)\n");
        }

        ndi->find_wait_for_sources(finder, 2000);

        uint32_t num_sources = 0;
        const NDIlib_source_t* ndi_sources = ndi->find_get_current_sources(finder, &num_sources);

        int count = (int)num_sources;
        if (count > NDI_MAX_SOURCES) count = NDI_MAX_SOURCES;

        for (int i = 0; i < count; i++) {
            strncpy(sources[i].name, ndi_sources[i].p_ndi_name ? ndi_sources[i].p_ndi_name : "",
                    NDI_SOURCE_NAME_LEN - 1);
            sources[i].name[NDI_SOURCE_NAME_LEN - 1] = '\0';

            strncpy(sources[i].url, ndi_sources[i].p_url_address ? ndi_sources[i].p_url_address : "",
                    NDI_SOURCE_NAME_LEN - 1);
            sources[i].url[NDI_SOURCE_NAME_LEN - 1] = '\0';
        }

        if (discovery_callback) {
            discovery_callback(sources, count, discovery_userdata);
        }
    }

    if (finder) {
        ndi->find_destroy(finder);
        finder = NULL;
    }

    return NULL;
}

int ndi_start_discovery(ndi_source_list_callback_t callback, void* userdata) {
    if (!ndi) return -1;
    if (discovery_running) return 0;

    discovery_callback = callback;
    discovery_userdata = userdata;
    discovery_running = true;

    if (pthread_create(&discovery_thread, NULL, discovery_thread_func, NULL) != 0) {
        discovery_running = false;
        return -1;
    }

    return 0;
}

void ndi_stop_discovery(void) {
    if (!discovery_running) return;
    discovery_running = false;
    pthread_join(discovery_thread, NULL);
}

static void* receive_thread_func(void* arg) {
    (void)arg;

    NDIlib_video_frame_v2_t video_frame;
    int consecutive_empty = 0;

    bool self_terminated = false;

    while (receive_running && receiver) {
        NDIlib_frame_type_e frame_type = ndi->recv_capture_v3(
            receiver, &video_frame, NULL, NULL, 250
        );

        switch (frame_type) {
            case NDIlib_frame_type_video:
                consecutive_empty = 0;
                if (frame_callback && video_frame.p_data) {
                    frame_callback(
                        video_frame.p_data,
                        video_frame.xres,
                        video_frame.yres,
                        video_frame.line_stride_in_bytes,
                        frame_userdata
                    );
                }
                ndi->recv_free_video_v2(receiver, &video_frame);
                break;

            case NDIlib_frame_type_error:
                fprintf(stderr, "NDI connection error\n");
                self_terminated = true;
                receive_running = false;
                break;

            case NDIlib_frame_type_none:
                consecutive_empty++;
                if (connected && consecutive_empty > 20) {
                    fprintf(stderr, "NDI source timeout (no frames for 5s)\n");
                    self_terminated = true;
                    receive_running = false;
                }
                break;

            default:
                break;
        }
    }

    connected = false;

    // Only fire disconnect callback if thread terminated itself (error/timeout),
    // not when stopped externally via ndi_disconnect()
    if (self_terminated && disconnect_callback) {
        disconnect_callback(disconnect_userdata);
    }

    return NULL;
}

int ndi_connect(const char* source_name, const char* source_url) {
    if (!ndi) return -1;

    ndi_disconnect();

    NDIlib_source_t ndi_source;
    memset(&ndi_source, 0, sizeof(ndi_source));
    ndi_source.p_ndi_name = source_name;
    ndi_source.p_url_address = source_url;

    NDIlib_recv_create_v3_t recv_settings;
    memset(&recv_settings, 0, sizeof(recv_settings));
    recv_settings.source_to_connect_to = ndi_source;
    recv_settings.color_format = NDIlib_recv_color_format_BGRX_BGRA;
    recv_settings.bandwidth = NDIlib_recv_bandwidth_highest;
    recv_settings.allow_video_fields = false;
    recv_settings.p_ndi_recv_name = "NDI LED Wall";

    receiver = ndi->recv_create_v3(&recv_settings);
    if (!receiver) {
        fprintf(stderr, "Failed to create NDI receiver\n");
        return -1;
    }

    connected = true;
    receive_running = true;

    if (pthread_create(&receive_thread, NULL, receive_thread_func, NULL) != 0) {
        ndi->recv_destroy(receiver);
        receiver = NULL;
        connected = false;
        receive_running = false;
        return -1;
    }

    fprintf(stderr, "NDI connected to: %s\n", source_name);
    return 0;
}

void ndi_disconnect(void) {
    if (receive_running) {
        receive_running = false;
        pthread_join(receive_thread, NULL);
    }

    if (receiver) {
        ndi->recv_destroy(receiver);
        receiver = NULL;
    }

    connected = false;
}

void ndi_set_frame_callback(ndi_frame_callback_t callback, void* userdata) {
    frame_callback = callback;
    frame_userdata = userdata;
}

void ndi_set_disconnect_callback(ndi_disconnect_callback_t callback, void* userdata) {
    disconnect_callback = callback;
    disconnect_userdata = userdata;
}

bool ndi_is_connected(void) {
    return connected;
}

void ndi_shutdown(void) {
    ndi_disconnect();
    ndi_stop_discovery();

    if (ndi) {
        ndi->destroy();
        ndi = NULL;
    }
}
