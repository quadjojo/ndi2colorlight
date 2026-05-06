#ifndef FRAME_CONVERT_H
#define FRAME_CONVERT_H

#include <stdint.h>

// Direct BGRX→BGR conversion (no scaling, input must match output dimensions)
// Applies gamma LUT if gamma != 1.0
void frame_convert_direct(
    const uint8_t* input_bgrx, int width, int height, int stride,
    uint8_t* output_bgr, float gamma
);

// Scale BGRX input to BGR output using bilinear interpolation
// Input and output may have different dimensions
void frame_convert_scale(
    const uint8_t* input_bgrx, int in_w, int in_h, int in_stride,
    uint8_t* output_bgr, int out_w, int out_h, float gamma
);

#endif
