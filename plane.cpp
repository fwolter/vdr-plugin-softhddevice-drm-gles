/**
 * @file plane.h
 * @brief DRM plane class declaration
 *
 * Copyright: (c) 2018 by zille.  All Rights Reserved.
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

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm_buf.h"
#include "plane.h"
#include "logger.h"

/*****************************************************************************
 * cDrmPlane class
 ****************************************************************************/

cDrmPlane::cDrmPlane(void)
{
	m_planeId = 0;
	m_zpos = 0;
}

cDrmPlane::~cDrmPlane(void)
{
}

/**
 * @brief Fill the plane properties
 *
 * This "caches" the properties within the class.
 *
 * @param fd		drm file descriptor
 */
void cDrmPlane::FillProperties(int fd)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, GetId(), DRM_MODE_OBJECT_PLANE);
	if (!props) {
		LOGERROR("could not get %u properties: %s", GetId(), strerror(errno));
		return;
	}

	SetProps(props);

	m_propsInfo = (drmModePropertyRes **)calloc(m_props->count_props, sizeof(*m_propsInfo));
	for (uint32_t i = 0; i < m_props->count_props; i++) {
		m_propsInfo[i] = drmModeGetProperty(fd, m_props->props[i]);
	}
}

/**
 * @brief Free the previously filled plane properties
 */
void cDrmPlane::FreeProperties(void)
{
	if (!GetProps())
		return;

	if (!GetPropsInfo())
		return;

	for (uint32_t i = 0; i < m_props->count_props; i++) {
		if (m_propsInfo[i])
			drmModeFreeProperty(m_propsInfo[i]);

	}
	drmModeFreeObjectProperties(m_props);
	free(m_propsInfo);
}

/**
 * @brief Set the modesetting parameters of a plane
 *
 * These values are used for drm modesetting
 *
 * @param crtcId	Mode object ID of the crtc
 * @param fbId		Mode object ID of the drm framebuffer
 * @param crtcX		X offset of the destination rect
 * @param crtcY		Y offset of the destination rect
 * @param crtcW		width of the destination rect
 * @param crtcH		height of the destination rect
 * @param srcX		X offset of the source rect
 * @param srcY		Y offset of the source rect
 * @param srcW		width of the source rect
 * @param srccH		height of the source rect
 */
void cDrmPlane::SetParams(uint64_t crtcId, uint64_t fbId,
			 uint64_t crtcX, uint64_t crtcY, uint64_t crtcW, uint64_t crtcH,
			 uint64_t srcX,  uint64_t srcY,  uint64_t srcW,  uint64_t srcH)
{
	m_crtcId = crtcId;
	m_fbId = fbId;
	m_crtcX = crtcX;
	m_crtcY = crtcY;
	m_crtcW = crtcW;
	m_crtcH = crtcH;
	m_srcX = srcX;
	m_srcY = srcY;
	m_srcW = srcW;
	m_srcH = srcH;
}

/**
 * @brief Dump the plane parameter modesetting values
 */
void cDrmPlane::DumpParameters(void)
{
	LOGINFO("DumpParameters (plane_id = %d):", GetId());
	LOGINFO("  CRTC ID: %" PRIu64 "", GetCrtcId());
	LOGINFO("  FB ID  : %" PRIu64 "", GetFbId());
	LOGINFO("  CRTC X : %" PRIu64 "", GetCrtcX());
	LOGINFO("  CRTC Y : %" PRIu64 "", GetCrtcY());
	LOGINFO("  CRTC W : %" PRIu64 "", GetCrtcW());
	LOGINFO("  CRTC H : %" PRIu64 "", GetCrtcH());
	LOGINFO("  SRC X  : %" PRIu64 "", GetSrcX());
	LOGINFO("  SRC Y  : %" PRIu64 "", GetSrcY());
	LOGINFO("  SRC W  : %" PRIu64 "", GetSrcW());
	LOGINFO("  SRC H  : %" PRIu64 "", GetSrcH());
	LOGINFO("  ZPOS   : %" PRIu64 "", GetZpos());
}
