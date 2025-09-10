/**
 * @file grab.cpp
 * @brief Grabber functions
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

#include "logger.h"
#include "grab.h"

/*****************************************************************************
 * cSoftHdGrab class
 ****************************************************************************/

/**
 * @brief Grabber class constructor
 */
 cSoftHdGrab::cSoftHdGrab(void)
{
	m_pBuf = NULL;
	m_pResult = NULL;

	cRect m_rect;
	m_size = 0;
}

/**
 * @brief Grabber class destructor
 */
cSoftHdGrab::~cSoftHdGrab(void)
{
}

/**
 * @brief Free the grab buffer
 */
void cSoftHdGrab::FreeBuf(void)
{
	if (!m_pBuf)
		return;

	for (int plane = 0; plane < m_pBuf->num_planes; plane++) {
		if (m_pBuf->size[plane]) {
		LOGDEBUG2(L_GRAB, "%s: free buf %p (plane %d)", __FUNCTION__, m_pBuf->plane[plane], plane);
		free(m_pBuf->plane[plane]);
		}
	}
	free(m_pBuf);

	m_pBuf = nullptr;
}
