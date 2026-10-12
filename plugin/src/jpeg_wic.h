#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode a JPEG buffer to 32bpp BGRA using Windows Imaging Component.
 * Returns 1 on success with *out_bgra malloc'd (stride * height bytes),
 * *out_w/*out_h/*out_stride set. Caller frees *out_bgra. Returns 0 on failure.
 */
int jpeg_to_bgra(const uint8_t *jpeg, int jpeg_len, uint8_t **out_bgra,
                 int *out_w, int *out_h, int *out_stride);

#ifdef __cplusplus
}
#endif
