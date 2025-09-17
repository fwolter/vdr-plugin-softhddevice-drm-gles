/**
 * @file drm_buf.h
 * @brief DRM buffer struct declaration
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

#ifndef __DRM_BUF_H
#define __DRM_BUF_H

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavutil/hwcontext_drm.h>
}

#ifdef USE_GLES
#include <gbm.h>
#endif

struct drm_buf {
	uint32_t width, height, size[4], pitch[4], offset[4], fb_id;
	uint32_t handle[4];		// prime handle for plane
	int fd_prime[4];		// prime fds, correspond to obj_index
	uint8_t *plane[4];
	uint32_t pix_fmt;
	AVFrame *frame;
	int dirty;
	int num_planes;
	int nb_objects;
	int obj_index[4];
	uint32_t primehandle[4];	// primedata objects prime handles (count is nb_objects, index is obj_index)
	int trickspeed;
	int swbuffer;
#ifdef USE_GLES
	struct gbm_bo *bo;
#endif
};

#endif
