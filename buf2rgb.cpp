/**
 * @file buf2rgb.cpp
 * @brief Some helper functions to convert and blit buffers
 *
 * Copyright: (c) 2025 by Andreas Baierl. All Rights Reserved.
 *
 * License: AGPLv3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 */

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <sys/mman.h>
#include <drm_fourcc.h>

#include "buf2rgb.h"
#include "logger.h"
#include "drmbuffer.h"

#define OPAQUE		0xff
#define TRANSPARENT	0x00
#define UNMULTIPLY(color, alpha) ((0xff * color) / alpha)
#define BLEND(back, front, alpha) ((front * alpha) + (back * (255 - alpha))) / 255

/****************************************************************************************
 * Image data conversion helpers
 ***************************************************************************************/

/**
 * @brief Convert a DRM format to a ffmpeg AV format
 */
enum AVPixelFormat drmFormatToAVFormat(cDrmBuffer *buf)
{
	if (buf->PixFmt() == DRM_FORMAT_NV12)
		return AV_PIX_FMT_NV12;

	if (buf->PixFmt() == DRM_FORMAT_YUV420)
		return AV_PIX_FMT_YUV420P;

	if (buf->PixFmt() == DRM_FORMAT_ARGB8888)
		return AV_PIX_FMT_RGBA;

	if (buf->PixFmt() == DRM_FORMAT_P030)
		return AV_PIX_FMT_NONE;

	return AV_PIX_FMT_NONE;
}

/**
 * @brief Convert a DRM buffer to rgb format image
 *
 * Conversion is done with ffmpegs swscale
 *
 * @param[in] buf		pointer to the source drm buffer struct
 * @param[out] size		size of the return data
 * @param[in] dst_w		width of the returned image
 * @param[in] dst_h		height of the returned image
 * @param[in] dst_pix_fmt	pixel format of the returned image
 *
 * @returns 			a pointer to the image data
 */
uint8_t *buf2rgb(cDrmBuffer *buf, int *size, int dst_w, int dst_h, enum AVPixelFormat dst_pix_fmt)
{
	uint8_t *src_data[4], *dst_data[4];
	int src_linesize[4], dst_linesize[4];

	int src_w = buf->Width();
	int src_h = buf->Height();

	enum AVPixelFormat src_pix_fmt = drmFormatToAVFormat(buf);
	if (src_pix_fmt == AV_PIX_FMT_NONE) {
		LOGERROR("buf2rgb: pixel format is not supported!");
		return NULL;
	}

	int dst_bufsize = 0;
	struct SwsContext *sws_ctx;
	int ret;
	void *buffer = NULL;

	// planes aren't mmapped, return
	// this should be done before in VideoCloneBuf
	if (!buf->Plane(0)) {
		LOGERROR("buf2rgb: prime data is not mapped!");
		return NULL;
	}

	// convert yuv to rgb
	sws_ctx = sws_getContext(src_w, src_h, src_pix_fmt,
							 dst_w, dst_h, dst_pix_fmt,
							 SWS_BILINEAR, NULL, NULL, NULL);
	if (!sws_ctx) {
		LOGERROR("buf2rgb: Could not create sws_ctx");
		munmap(buffer, buf->Size(0));
		return NULL;
	}

	if ((ret = av_image_alloc(dst_data, dst_linesize,
							  dst_w, dst_h, dst_pix_fmt, 1)) < 0) {
		LOGERROR("buf2rgb: Could not alloc dst image");
		munmap(buffer, buf->Size(0));
		sws_freeContext(sws_ctx);
		return NULL;
	}
	dst_bufsize = ret;

	// copy src pitches and data
	for (int i = 0; i < buf->NumPlanes(); i++) {
		src_linesize[i] = buf->Pitch(i);
		src_data[i] = buf->Plane(i) + buf->Offset(i);
	}

	// scale image
	sws_scale(sws_ctx,
			  (const uint8_t * const*)src_data, src_linesize, 0, src_h,
			  dst_data, dst_linesize);

	if (buffer)
		munmap(buffer, buf->Size(0));
	sws_freeContext(sws_ctx);
	*size = dst_bufsize;

	LOGDEBUG2(L_GRAB, "buf2rgb: return image at %p size %d", dst_data[0], dst_bufsize);
	return dst_data[0];
}

/**
 * @brief Scale an image
 *
 * Conversion is done with ffmpegs swscale
 *
 * @param[in] src		pointer to the source data
 * @param[out] size		size of the return data
 * @param[in] src_w		source width
 * @param[in] src_h		source height
 * @param[in] dst_w		width of the returned image
 * @param[in] dst_h		height of the returned image
 *
 * @returns 			a pointer to the converted image data
 */
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
		LOGERROR("scalergb: Could not create sws_ctx");
		return NULL;
	}

	if ((ret = av_image_alloc(dst_data, dst_linesize,
							  dst_w, dst_h, AV_PIX_FMT_RGB24, 1)) < 0) {
		LOGERROR("scalergb: Could not alloc dst image");
		sws_freeContext(sws_ctx);
		return NULL;
	}
	dst_bufsize = ret;

	sws_scale(sws_ctx,
			  (const uint8_t * const*)src_data, src_linesize, 0, src_h,
			  dst_data, dst_linesize);

	sws_freeContext(sws_ctx);
	*size = dst_bufsize;

	LOGDEBUG2(L_GRAB, "scalergb: return scaled image at %p size %d", dst_data[0], dst_bufsize);
	return dst_data[0];
}

/**
 * @brief Blend two images
 *
 * Both, front and back image data have to be same size
 * front is the OSD (ARGB)
 * back is the video (RGB)
 * result is RGB
 *
 * @param[out] result	pointer to the resulting image data
 * @param[in] front		pointer to the upper image data
 * @param[in] back		pointer to the lower image data
 * @param[in] width		image width
 * @param[in] height	image height
 */
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

/**
 * @brief Blit the video on black background
 *
 * @param[in] src		pointer to the source video
 * @param[in] dst_w		destination width of the image
 * @param[in] dst_h		destination height of the image
 * @param[in] dst_x		x offset of the (already scaled) video on the image
 * @param[in] dst_y		y offset of the (already scaled) video on the image
 * @param[in] src_w		source video width
 * @param[in] src_h		source video height
 *
 * @returns 			a pointer to the blitted image data
 */
uint8_t *blitvideo(uint8_t *src, int dst_w, int dst_h, int dst_x, int dst_y, int src_w, int src_h)
{
	int src_stride = src_w * 3;
	int dst_stride = dst_w * 3;

	// create a black screen
	uint8_t *result = (uint8_t *)calloc(1, dst_stride * dst_h);

	// blit the scaled image into the black one
	for (int y = 0; y < src_h; y++) {
		memcpy(&result[((dst_y + y) * dst_stride + dst_x * 3)], &src[y * src_stride], src_stride);
	}

	return result;
}

/**
 * @brief Print raw stream data
 *
 * @param data		pointer to stream data
 * @param size		data size
 */
void PrintStreamData(const uint8_t *data, int size)
{
	LOGDEBUG("Data: %02x %02x %02x %02x %02x %02x %02x %02x %02x "
		"%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
		"%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x size %d",
		data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8],
		data[9], data[10], data[11], data[12], data[13], data[14], data[15], data[16], data[17],
		data[18], data[19], data[20], data[21], data[22], data[23], data[24], data[25], data[26],
		data[27], data[28], data[29], data[30], data[31], data[32], data[33], data[34], size);
}
