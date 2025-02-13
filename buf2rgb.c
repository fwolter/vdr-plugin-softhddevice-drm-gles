#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <sys/mman.h>

#include "misc.h"
#include "buf2rgb.h"
#include <drm_fourcc.h>

#define OPAQUE		0xff
#define TRANSPARENT	0x00
#define UNMULTIPLY(color, alpha) ((0xff * color) / alpha)
#define BLEND(back, front, alpha) ((front * alpha) + (back * (255 - alpha))) / 255

/****************************************************************************************
* Helpers
****************************************************************************************/
enum AVPixelFormat drmFormatToAVFormat(struct drm_buf *buf)
{
    if (buf->pix_fmt == DRM_FORMAT_NV12)
	return AV_PIX_FMT_NV12;

    if (buf->pix_fmt == DRM_FORMAT_YUV420)
	return AV_PIX_FMT_YUV420P;

    if (buf->pix_fmt == DRM_FORMAT_ARGB8888)
	return AV_PIX_FMT_RGBA;

    return AV_PIX_FMT_NONE;
}

uint8_t *buf2rgb(struct drm_buf *buf, int *size, int dst_w, int dst_h, enum AVPixelFormat dst_pix_fmt)
{
    uint8_t *src_data[4], *dst_data[4];
    int src_linesize[4], dst_linesize[4];

    int src_w = buf->width;
    int src_h = buf->height;

    enum AVPixelFormat src_pix_fmt = drmFormatToAVFormat(buf);
    int dst_bufsize = 0;
    struct SwsContext *sws_ctx;
    int ret;
    void *buffer = NULL;

    // planes aren't mmapped, return
    // this should be done before in VideoCloneBuf
    if (!buf->plane[0]) {
        Error("buf2rgb: prime data is not mapped!");
        return NULL;
    }

    // convert yuv to rgb
    sws_ctx = sws_getContext(src_w, src_h, src_pix_fmt,
                             dst_w, dst_h, dst_pix_fmt,
                             SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        Error("buf2rgb: Could not create sws_ctx");
        munmap(buffer, buf->size[0]);
        return NULL;
    }

    if ((ret = av_image_alloc(dst_data, dst_linesize,
                              dst_w, dst_h, dst_pix_fmt, 1)) < 0) {
        Error("buf2rgb: Could not alloc dst image");
        munmap(buffer, buf->size[0]);
        sws_freeContext(sws_ctx);
        return NULL;
    }
    dst_bufsize = ret;

    // copy src pitches and data
    for (int i = 0; i < buf->num_planes; i++) {
        src_linesize[i] = buf->pitch[i];
        src_data[i] = buf->plane[i] + buf->offset[i];
    }

    // scale image
    sws_scale(sws_ctx,
              (const uint8_t * const*)src_data, src_linesize, 0, src_h,
              dst_data, dst_linesize);

    if (buffer)
        munmap(buffer, buf->size[0]);
    sws_freeContext(sws_ctx);
    *size = dst_bufsize;

    Debug2(L_GRAB, "buf2rgb: return image at %p size %d", dst_data[0], dst_bufsize);
    return dst_data[0];
}

uint8_t *scalergb24(uint8_t *src, int *size, int src_w, int src_h, int dst_w, int dst_h)
{
    struct SwsContext *sws_ctx;
    int dst_bufsize = 0;
    int ret;

    uint8_t *dst_data[4];
    int dst_linesize[4];
    uint8_t *src_data[4] = {src, NULL, NULL, NULL};
    int src_linesize[4] = {3 * src_w, 0, 0, 0};

    sws_ctx = sws_getContext(src_w, src_h, AV_PIX_FMT_RGB24,
                             dst_w, dst_h, AV_PIX_FMT_RGB24,
                             SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        Error("scalergb: Could not create sws_ctx");
        return NULL;
    }

    if ((ret = av_image_alloc(dst_data, dst_linesize,
                              dst_w, dst_h, AV_PIX_FMT_RGB24, 1)) < 0) {
        Error("scalergb: Could not alloc dst image");
        sws_freeContext(sws_ctx);
        return NULL;
    }
    dst_bufsize = ret;

    sws_scale(sws_ctx,
              (const uint8_t * const*)src_data, src_linesize, 0, src_h,
              dst_data, dst_linesize);

    sws_freeContext(sws_ctx);
    *size = dst_bufsize;

    Debug2(L_GRAB, "scalergb: return scaled image at %p size %d", dst_data[0], dst_bufsize);
    return dst_data[0];
}

void alphablend(uint8_t *result, uint8_t *front, uint8_t *back, const unsigned int width, const unsigned int height)
{
    for (unsigned long index = 0; index < width * height; index++) {
        const uint8_t frontAlpha = front[3];

        if (frontAlpha == TRANSPARENT) {
            for (int i = 0; i < 3; i++) {
                result[i] = back[i];
            }
            back += 3;
            front += 4;
            result += 3;
            continue;
        }

        if (frontAlpha == OPAQUE) {
            for (int i = 0; i < 3; i++) {
                result[i] = front[i];
            }
            back += 3;
            front += 4;
            result += 3;
            continue;
        }

        const uint8_t backR = back[0];
        const uint8_t backG = back[1];
        const uint8_t backB = back[2];

        const uint8_t frontR = UNMULTIPLY(front[0], frontAlpha);
        const uint8_t frontG = UNMULTIPLY(front[1], frontAlpha);
        const uint8_t frontB = UNMULTIPLY(front[2], frontAlpha);

        const uint8_t R = BLEND(backR, frontR, frontAlpha);
        const uint8_t G = BLEND(backG, frontG, frontAlpha);
        const uint8_t B = BLEND(backB, frontB, frontAlpha);

        result[0] = R;
        result[1] = G;
        result[2] = B;

        back += 3;
        front += 4;
        result += 3;
    }
}

uint8_t *blitvideo(uint8_t *src, int dst_w, int dst_h, int dst_x, int dst_y, int src_w, int src_h)
{
    int src_stride = src_w * 3;
    int dst_stride = dst_w * 3;

    // create a black screen
    uint8_t *result = calloc(1, dst_stride * dst_h);

    // blit the scaled image into the black one
    for (int y = 0; y < src_h; y++) {
        memcpy(&result[((dst_y + y) * dst_stride + dst_x * 3)], &src[y * src_stride], src_stride);
    }

    return result;
}
