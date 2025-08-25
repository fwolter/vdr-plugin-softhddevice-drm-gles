/**
 * @file grab.h
 * @brief Grabber class declaration
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

#ifndef __GRAB_H
#define __GRAB_H

#include "drm_buf.h"

/**
 * @brief cSoftHdGrab - Grabber class
 *
 * Class to hold the data for a grabbed buffer
 * Grab is triggered by VDR/ cSoftHdDevice, data is set by the renderer
 * and composed by cSoftHdDevice again.
 */
class cSoftHdGrab
{
public:
	cSoftHdGrab(void);
	virtual ~cSoftHdGrab(void);

	void SetX(int x) { m_x = x; };
	void SetY(int y) { m_y = y; };
	void SetWidth(int width) { m_width = width; };
	void SetHeight(int height) { m_height = height; };
	void SetData(uint8_t *result) { m_pResult = result; };
	void SetSize(int size) { m_size = size; };
	void SetBuf(struct drm_buf *buf) { m_pBuf = buf; };

	int GetX(void) { return m_x; };
	int GetY(void) { return m_y; };
	int GetWidth(void) { return m_width; };
	int GetHeight(void) { return m_height; };
	uint8_t *GetData(void) { return m_pResult; };
	int GetSize(void) { return m_size; };
	struct drm_buf *GetBuf(void) { return m_pBuf; };

	void FreeBuf(void);

private:
	uint8_t *m_pResult;		    ///< pointer to grabbed image
	struct drm_buf *m_pBuf;	    ///< pointer to original buffer
	int m_size;			        ///< size of grabbed data
	int m_width;			    ///< width of grabbed data
	int m_height;			    ///< height of grabbed data
	int m_x;				    ///< x coord of grabbed data
	int m_y;				    ///< y coord of grabbed data
};

#endif
