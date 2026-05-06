#include "frame_convert.h"
#include <math.h>

// Gamma lookup table
static uint8_t gamma_lut[256];
static float current_gamma = -1.0f;

static void build_gamma_lut(float gamma) {
    if (gamma == current_gamma) return;
    current_gamma = gamma;

    for (int i = 0; i < 256; i++) {
        if (gamma == 1.0f) {
            gamma_lut[i] = (uint8_t)i;
        } else {
            float f = powf((float)i / 255.0f, gamma) * 255.0f;
            if (f < 0.0f) f = 0.0f;
            if (f > 255.0f) f = 255.0f;
            gamma_lut[i] = (uint8_t)(f + 0.5f);
        }
    }
}

void frame_convert_direct(
    const uint8_t* input_bgrx, int width, int height, int stride,
    uint8_t* output_bgr, float gamma
) {
    build_gamma_lut(gamma);

    for (int y = 0; y < height; y++) {
        const uint8_t* src = input_bgrx + y * stride;
        uint8_t* dst = output_bgr + y * width * 3;

        if (gamma == 1.0f) {
            // Fast path: no gamma, just strip X channel
            for (int x = 0; x < width; x++) {
                dst[x * 3 + 0] = src[x * 4 + 0]; // B
                dst[x * 3 + 1] = src[x * 4 + 1]; // G
                dst[x * 3 + 2] = src[x * 4 + 2]; // R
            }
        } else {
            for (int x = 0; x < width; x++) {
                dst[x * 3 + 0] = gamma_lut[src[x * 4 + 0]];
                dst[x * 3 + 1] = gamma_lut[src[x * 4 + 1]];
                dst[x * 3 + 2] = gamma_lut[src[x * 4 + 2]];
            }
        }
    }
}

void frame_convert_scale(
    const uint8_t* input_bgrx, int in_w, int in_h, int in_stride,
    uint8_t* output_bgr, int out_w, int out_h, float gamma
) {
    if (out_w <= 0 || out_h <= 0 || in_w <= 0 || in_h <= 0) return;

    build_gamma_lut(gamma);

    // Bilinear scaling BGRX → BGR
    float x_ratio = (float)in_w / (float)out_w;
    float y_ratio = (float)in_h / (float)out_h;

    for (int y = 0; y < out_h; y++) {
        float src_y = y * y_ratio;
        int y0 = (int)src_y;
        int y1 = y0 + 1;
        if (y1 >= in_h) y1 = in_h - 1;
        float fy = src_y - y0;
        float fy_inv = 1.0f - fy;

        uint8_t* dst = output_bgr + y * out_w * 3;

        for (int x = 0; x < out_w; x++) {
            float src_x = x * x_ratio;
            int x0 = (int)src_x;
            int x1 = x0 + 1;
            if (x1 >= in_w) x1 = in_w - 1;
            float fx = src_x - x0;
            float fx_inv = 1.0f - fx;

            const uint8_t* p00 = input_bgrx + y0 * in_stride + x0 * 4;
            const uint8_t* p01 = input_bgrx + y0 * in_stride + x1 * 4;
            const uint8_t* p10 = input_bgrx + y1 * in_stride + x0 * 4;
            const uint8_t* p11 = input_bgrx + y1 * in_stride + x1 * 4;

            float w00 = fx_inv * fy_inv;
            float w01 = fx * fy_inv;
            float w10 = fx_inv * fy;
            float w11 = fx * fy;

            // BGR channels (skip X at offset 3)
            for (int c = 0; c < 3; c++) {
                float val = p00[c] * w00 + p01[c] * w01 + p10[c] * w10 + p11[c] * w11;
                int ival = (int)(val + 0.5f);
                if (ival > 255) ival = 255;
                dst[x * 3 + c] = gamma_lut[ival];
            }
        }
    }
}
