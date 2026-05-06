#include "colorlight_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>

// Colorlight protocol constants
static const uint8_t CL_DST_MAC[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
static const uint8_t CL_SRC_MAC[6] = {0x22, 0x22, 0x33, 0x44, 0x55, 0x66};

// Colorlight frame layout (NOT standard Ethernet!):
//   Bytes 0-5:   Destination MAC
//   Bytes 6-11:  Source MAC
//   Byte 12:     Packet Type (0x55=pixel, 0x01=sync, 0x0A=brightness)
//   Byte 13+:    Payload data (NO standard 2-byte EtherType!)
#define CL_MAC_SIZE             12
#define CL_PACKET_TYPE_OFFSET   12
#define CL_DATA_OFFSET          13

#define CL_PIXEL_HEADER_SIZE    8
#define CL_SYNC_DATA_SIZE       99
#define CL_BRIGHTNESS_DATA_SIZE 64

#define CL_SYNC_PACKET_SIZE     112 // 12 MAC + 1 type + 99 data
#define CL_BRIG_PACKET_SIZE     77  // 12 MAC + 1 type + 64 data

// Max packet: 13 header + 8 pixel header + 497*3 BGR = 1512 bytes
#define CL_MAX_PACKET_SIZE (CL_DATA_OFFSET + CL_PIXEL_HEADER_SIZE + CL_MAX_PIXELS_PER_PACKET * CL_BYTES_PER_PIXEL)
static uint8_t row_packet_buf[CL_MAX_PACKET_SIZE];

// State
static int sock_fd = -1;
static struct sockaddr_ll sock_addr;
static int config_panel_w = 32;
static int config_panel_h = 16;
static int config_panels_x = 1;
static int config_panels_y = 1;
static uint8_t config_brightness = 255;
static int frame_count = 0;

// Error tracking
static int send_error_count = 0;
static int send_error_logged = 0;

// Forward declarations
static int send_brightness_packet(void);

// Build frame header: DST MAC + SRC MAC + packet type byte
static void build_frame_header(uint8_t* buf, uint8_t packet_type) {
    memcpy(buf, CL_DST_MAC, 6);
    memcpy(buf + 6, CL_SRC_MAC, 6);
    buf[CL_PACKET_TYPE_OFFSET] = packet_type;
}

// Send raw ethernet frame via AF_PACKET
static int raw_send(const uint8_t* data, size_t len) {
    if (sock_fd < 0) return -1;

    ssize_t written = sendto(sock_fd, data, len, 0,
                             (struct sockaddr*)&sock_addr, sizeof(sock_addr));
    if (written < 0) {
        send_error_count++;
        if (send_error_count - send_error_logged >= 1000) {
            fprintf(stderr, "Send errors: %d (latest: %s)\n",
                    send_error_count, strerror(errno));
            send_error_logged = send_error_count;
        }
        return -1;
    }
    return 0;
}

int colorlight_open(const char* interface_name) {
    if (sock_fd >= 0) {
        colorlight_close();
    }

    sock_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock_fd < 0) {
        perror("Failed to create raw socket. Run with sudo or set CAP_NET_RAW");
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, interface_name, IFNAMSIZ - 1);
    if (ioctl(sock_fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("Failed to get interface index");
        close(sock_fd);
        sock_fd = -1;
        return -1;
    }

    int ifindex = ifr.ifr_ifindex;

    memset(&sock_addr, 0, sizeof(sock_addr));
    sock_addr.sll_family = AF_PACKET;
    sock_addr.sll_protocol = htons(ETH_P_ALL);
    sock_addr.sll_ifindex = ifindex;
    sock_addr.sll_halen = 6;
    memcpy(sock_addr.sll_addr, CL_DST_MAC, 6);

    if (bind(sock_fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr)) < 0) {
        perror("Failed to bind to interface");
        close(sock_fd);
        sock_fd = -1;
        return -1;
    }

    frame_count = 0;
    send_error_count = 0;
    send_error_logged = 0;
    fprintf(stderr, "Colorlight output opened on %s (ifindex=%d)\n",
            interface_name, ifindex);
    return 0;
}

void colorlight_set_config(int panel_w, int panel_h, int panels_x, int panels_y) {
    config_panel_w = panel_w;
    config_panel_h = panel_h;
    config_panels_x = panels_x;
    config_panels_y = panels_y;
}

void colorlight_set_brightness(uint8_t brightness) {
    config_brightness = brightness;
    if (sock_fd >= 0) {
        send_brightness_packet();
    }
}

// Send pixel data for one row
static int send_row_data(int row, const uint8_t* bgr_data, int width) {
    int pixels_sent = 0;
    int errors = 0;

    while (pixels_sent < width) {
        int pixels_in_packet = width - pixels_sent;
        if (pixels_in_packet > CL_MAX_PIXELS_PER_PACKET)
            pixels_in_packet = CL_MAX_PIXELS_PER_PACKET;

        int pixel_data_size = pixels_in_packet * CL_BYTES_PER_PIXEL;
        int packet_size = CL_DATA_OFFSET + CL_PIXEL_HEADER_SIZE + pixel_data_size;

        memset(row_packet_buf, 0, CL_DATA_OFFSET + CL_PIXEL_HEADER_SIZE);
        build_frame_header(row_packet_buf, 0x55);

        uint8_t* hdr = row_packet_buf + CL_DATA_OFFSET;
        hdr[0] = (row >> 8) & 0xFF;
        hdr[1] = row & 0xFF;
        hdr[2] = (pixels_sent >> 8) & 0xFF;
        hdr[3] = pixels_sent & 0xFF;
        hdr[4] = (pixels_in_packet >> 8) & 0xFF;
        hdr[5] = pixels_in_packet & 0xFF;
        hdr[6] = 0x08;
        hdr[7] = 0x88;

        memcpy(hdr + CL_PIXEL_HEADER_SIZE,
               bgr_data + pixels_sent * CL_BYTES_PER_PIXEL,
               pixel_data_size);

        if (raw_send(row_packet_buf, packet_size) != 0) {
            errors++;
        }
        pixels_sent += pixels_in_packet;
    }

    return errors > 0 ? -1 : 0;
}

// Send sync/display update packet (112 bytes)
static int send_sync_packet(void) {
    uint8_t packet[CL_SYNC_PACKET_SIZE];
    memset(packet, 0, sizeof(packet));

    build_frame_header(packet, 0x01);

    uint8_t* data = packet + CL_DATA_OFFSET;
    data[0] = 0x07;
    data[22] = config_brightness;
    data[23] = 0x05;
    data[25] = config_brightness;
    data[26] = config_brightness;
    data[27] = config_brightness;

    raw_send(packet, CL_SYNC_PACKET_SIZE);
    return 0;
}

// Send brightness packet (77 bytes)
static int send_brightness_packet(void) {
    uint8_t packet[CL_BRIG_PACKET_SIZE];
    memset(packet, 0, sizeof(packet));

    build_frame_header(packet, 0x0A);

    uint8_t* data = packet + CL_DATA_OFFSET;
    data[0] = config_brightness;
    data[1] = config_brightness;
    data[2] = config_brightness;
    data[3] = 0xFF;

    raw_send(packet, CL_BRIG_PACKET_SIZE);
    return 0;
}

int colorlight_send_frame(const uint8_t* bgr_data, int width, int height) {
    if (sock_fd < 0) return -1;
    if (!bgr_data) return -1;

    int errors = 0;
    for (int y = 0; y < height; y++) {
        const uint8_t* row_data = bgr_data + y * width * CL_BYTES_PER_PIXEL;
        if (send_row_data(y, row_data, width) != 0) {
            errors++;
        }
    }

    // 5ms delay before sync
    struct timespec sync_delay = {0, 5000000};
    nanosleep(&sync_delay, NULL);

    send_sync_packet();

    frame_count++;
    return errors > 0 ? -1 : 0;
}

void colorlight_close(void) {
    if (sock_fd >= 0) {
        close(sock_fd);
        sock_fd = -1;
        fprintf(stderr, "Colorlight output closed (%d frames, %d send errors)\n",
                frame_count, send_error_count);
    }
}

bool colorlight_is_open(void) {
    return sock_fd >= 0;
}
