/**
 * @file drmbuffer.h
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

#ifndef __DRMBUFFER_H
#define __DRMBUFFER_H

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavutil/hwcontext_drm.h>
}

#ifdef USE_GLES
#include <gbm.h>
#endif

class cDrmBuffer {
public:
    cDrmBuffer(void);
    cDrmBuffer(cDrmBuffer *src);
    cDrmBuffer(int, uint32_t, uint32_t, uint32_t, struct gbm_bo *);
    virtual ~cDrmBuffer(void);

    int Setup(int, uint32_t, uint32_t, uint32_t, AVDRMFrameDescriptor *);
    void Destroy(void);
    void InitBo(int, uint32_t, uint32_t, uint32_t, struct gbm_bo *);
    void FillBlack(void);

    // setter and getter methods
    uint32_t Width(void) { return m_width; };
    void SetWidth(uint32_t width) { m_width = width; };
    uint32_t Height(void) { return m_height; };
    void SetHeight(uint32_t height) { m_height = height; };
    uint32_t PixFmt(void) { return m_pixFmt; };
    void SetPixFmt(uint32_t pixFmt) { m_pixFmt = pixFmt; };

    int IsDirty(void) { return m_dirty; };
    void MarkClean(void) { m_dirty = 0; };
    void MarkDirty(void) { m_dirty = 1; };
    int IsSwBuffer(void) { return m_swbuffer; };
    void MarkAsHwBuffer(void) { m_swbuffer = 0; };
    void MarkAsSwBuffer(void) { m_swbuffer = 1; };

    void SetTrickspeed(int trickspeed) { m_trickspeed = trickspeed; };
    int IsTrickspeedBuffer(void) { return m_trickspeed; };

    int Id(void) { return m_fbId; };
    void SetFdDrm(int fdDrm) { m_fdDrm = fdDrm; };
    int NumPlanes(void) { return m_numPlanes; };
    void SetNumPlanes(int numPlanes) { m_numPlanes = numPlanes; };
    int FdPrime(int idx) { return m_fdPrime[idx]; };
    void SetFdPrime(int idx, uint32_t fd) { m_fdPrime[idx] = fd; };
    void SetNumObjects(int numObjects) { m_numObjects = numObjects; };
    void SetObjectIndex(int idx, uint32_t objIdx) { m_objIdx[idx] = objIdx; };
    uint8_t *Plane(int idx) { return m_pPlane[idx]; };
    uint32_t Handle(int idx) { return m_handle[idx]; };
    uint32_t *Handle(void) { return m_handle; };
    void SetHandle(int idx, uint32_t handle) { m_handle[idx] = handle; };
    uint32_t Offset(int idx) { return m_offset[idx]; };
    uint32_t *Offset(void) { return m_offset; };
    void SetOffset(int idx, uint32_t offset) { m_offset[idx] = offset; };
    uint32_t Pitch(int idx) { return m_pitch[idx]; };
    uint32_t *Pitch(void) { return m_pitch; };
    void SetPitch(int idx, uint32_t pitch) { m_pitch[idx] = pitch; };
    uint32_t Size(int idx) { return m_size[idx]; };
    uint32_t *Size(void) { return m_size; };
    void SetSize(int idx, uint32_t size) { m_size[idx] = size; };

    AVFrame *Frame(void) { return m_pFrame; };
    void SetFrame(AVFrame *frame) { m_pFrame = frame; };

private:
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_pixFmt;

    int m_dirty;
    int m_swbuffer;
    int m_trickspeed;

    uint32_t m_fbId;
    int m_fdDrm;

    int m_numPlanes;

    int m_fdPrime[4];		// prime fds, correspond to obj_index
    int m_numObjects;
    int m_objIdx[4];

    uint8_t *m_pPlane[4];	
    uint32_t m_handle[4];		// prime handle for plane
    uint32_t m_offset[4];
    uint32_t m_pitch[4];
    uint32_t m_size[4];
    uint32_t m_primehandle[4];	// primedata objects prime handles (count is nb_objects, index is obj_index)

    AVFrame *m_pFrame;
#ifdef USE_GLES
    struct gbm_bo *m_pBo;
#endif
};

#endif
