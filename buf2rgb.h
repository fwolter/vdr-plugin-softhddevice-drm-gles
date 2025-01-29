#ifndef __BUF2RGB_H
#define __BUF2RGB_H

#include "video.h"

/****************************************************************************************
* Helpers
****************************************************************************************/
uint8_t *buf2rgb(struct drm_buf *buf, int *size, int w, int h, enum AVPixelFormat dst_pix_fmt);
uint8_t *scalergb24(uint8_t *src, int *size, int src_w, int src_h, int dst_w, int dst_h);
void alphablend(uint8_t *result, uint8_t *front, uint8_t *back, const unsigned int width, const unsigned int height);
uint8_t *blitvideo(uint8_t *src, int dst_w, int dst_h, int dst_x, int dst_y, int src_w, int src_h);

#endif
