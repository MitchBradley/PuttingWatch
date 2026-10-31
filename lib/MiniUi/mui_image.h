#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A full-colour picture: w*h RGB565 pixels in the panel's own byte order.
// C-compatible so the generated image sources (src/assets/*.c) can define one.
typedef struct {
    int w, h;
    const uint16_t* px;
} mui_image_t;

#ifdef __cplusplus
}
#endif
