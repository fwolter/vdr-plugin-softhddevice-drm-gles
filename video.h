/**
 * @file video.h
 * @brief Rendering module class declaration
 *
 * Copyright: (c) 2009 - 2015 by Johns.  All Rights Reserved.
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

#ifndef __VIDEO_H
#define __VIDEO_H

#include <xf86drm.h>
#include <xf86drmMode.h>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavutil/hwcontext_drm.h>
}

#ifdef USE_GLES
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>

/* Hack:
 * xlib.h via eglplatform.h: #define Status int
 * X.h via eglplatform.h: #define CurrentTime 0L
 *
 * revert it, because it conflicts with vdr variables.
 */
#undef Status
#undef CurrentTime
#endif

#include "logger.h"
#include "iatomic.h"
#include "softhddevice.h"
#include "videostream.h"
#include "glhelpers.h"
#include "drm_buf.h"
#include "threads.h"
#include "grab.h"

#define RENDERBUFFERS		36		///< number of render video buffers

#define VIDEO_PLANE			0
#define OSD_PLANE			1
#define MAX_PLANES			2

// Hardware quirks, that are set depending on the hardware used
#define QUIRK_NO_HW_DEINT				1 << 0	///< set, if no hw deinterlacer
#define QUIRK_CODEC_FLUSH_WORKAROUND	1 << 1	///< set, if we have to close and reopen the codec instead of avcodec_flush_buffers (rpi)
#define QUIRK_CODEC_NEEDS_EXT_INIT		1 << 2	///< set, if codec needs some infos for init (coded_width and coded_height)
#define QUIRK_CODEC_SKIP_FIRST_FRAMES	1 << 3	///< set, if codec should skip first I-Frames
#define QUIRK_CODEC_SKIP_NUM_FRAMES	2 	///< skip QUIRK_CODEC_SKIP_NUM_FRAMES, in case QUIRK_CODEC_SKIP_FIRST_FRAMES is set
#define QUIRK_CODEC_DISABLE_MPEG_HW		1 << 4	///< set, if disable mpeg hardware decoder
#define QUIRK_CODEC_DISABLE_H264_HW		1 << 5	///< set, if disable h264 hardware decoder

// frame flags
#define FRAME_FLAG_TRICKSPEED			1 << 0	///< mark frame as a trickspeed frame
#define FRAME_FLAG_STILLPICTURE			1 << 1	///< mark frame as a stillpicture frame

// TODO: move structs to classes
struct format_plane_info
{
	uint8_t bitspp;
	uint8_t xsub;
	uint8_t ysub;
};

struct format_info
{
	uint32_t format;
	const char *fourcc;
	uint8_t num_planes;
	struct format_plane_info planes[4];
};

struct lastFrame {
	AVFrame *frame;
	struct drm_buf *buf;
	int trickspeed;
};

typedef struct FrameData {
	int flags;
} FrameData;

struct plane_properties {
	uint64_t crtc_id;
	uint64_t fb_id;
	uint64_t crtc_x;
	uint64_t crtc_y;
	uint64_t crtc_w;
	uint64_t crtc_h;
	uint64_t src_x;
	uint64_t src_y;
	uint64_t src_w;
	uint64_t src_h;
	uint64_t zpos;
};

struct plane {
	uint32_t plane_id;
	uint64_t type;
	drmModePlane *plane;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	struct plane_properties properties;
};

/**
 * @brief cVideoRender - Video render class
 */
class cVideoRender
{
public:
	cVideoRender(cSoftHdDevice *);
	virtual ~cVideoRender(void);

	void Init(void);
	void Exit(void);
	void CleanUp(void);
	int FindDevice(void);
	void ReadHWPlatform(void);
	int HardwareQuirks(void) { return m_hardwareQuirks; };
	void DisableDeint(int);
	void DisableOglOsd(void);

	void SetDisplayResolution(const char *);
	void SetVideoOutputPosition(int, int, int, int);
	void GetScreenSize(int *, int *, double *);
	int64_t GetVideoClock(void);
	void GetStats(int *, int *, int *);
	void StartVideo(void);
	void PauseVideo(void);
	void ResumeVideo(void);
	int VideoIsPaused(void);	void SetClosing(int);
	int ShouldClose(void) { return m_closing; };
	int ShouldFlush(void) { return m_flushing; };

	// OSD
	void OsdClear(void);
	void OsdDrawARGB(int, int, int, int, int, const uint8_t *, int, int);

	// TrickSpeed
	void SetTrickSpeed(int, int);
	int GetTrickSpeed(void);
	int GetTrickCounter(void);
	int GetTrickForward(void);
	void SetTrickCounter(int);
	int DecTrickCounter(void);

	// Grab
	void TriggerGrab(cCondWait *wait);
	void ConvertVideoBufToRgb(void);
	void ConvertOsdBufToRgb(void);
	void ClearGrab(void);
	cSoftHdGrab *GetGrab(int *, int *, int *, int *, int *, int);

	// Threads
	void StartThreads(void);
	int DecodingThreadIsActive(void);
	void WakeupDecodingThread(void);
	void WakeupDisplayThread(void);
	void ExitDecodingThread(void);
	void ExitDisplayThread(void);

	// DRM -> new class?
	int SetPlanePropertyRequest(drmModeAtomicReqPtr, uint32_t, const char *, uint64_t);
	void SetPlaneZpos(drmModeAtomicReqPtr, struct plane *);
	void SetPlane(drmModeAtomicReqPtr, struct plane *);
	int CheckZpos(struct plane *, uint64_t);
	int32_t find_crtc_for_connector(const drmModeRes *, const drmModeConnector *);
	int init_gbm(int, int, uint32_t, uint64_t);
	int DrmHandleEvent(void);

	// Frame and buffer
	int RenderFrame(AVCodecContext *, AVFrame *, int flags);
	int DisplayFrame(void);
	int Sync(AVFrame *, int *, struct drm_buf **);
	void EnqueueFB(AVFrame *);
	int SetupFB(struct drm_buf *, AVDRMFrameDescriptor *);
	void DestroyFB(struct drm_buf *);
	int CommitBuffer(struct drm_buf *, int);
	int GetFrame(AVFrame **);
	int GetBuffer(AVFrame *, struct drm_buf **);
	int GetFramesFilled(void) { return atomic_read(&m_framesFilled); };
	void RbPushFrame(AVFrame *);
	AVFrame *RbGetFrame(void);
	void FramesRbLock(void);
	void FramesRbUnlock(void);

	// Filter
	void ClearFramesToFilter(void) { m_numFramesToFilter = 0; };
	void IncFramesToFilter(void) { m_numFramesToFilter++; };
	void DecFramesToFilter(void) { m_numFramesToFilter--; };

#ifdef USE_GLES
	// GLES
	EGLSurface EglSurface(void) { return m_eglSurface; };
	EGLDisplay EglDisplay(void) { return m_eglDisplay; };
	EGLDisplay EglContext(void) { return m_eglContext; };
	int GlInitiated(void) { return m_glInitiated; };
#endif

private:
	cSoftHdDevice *m_pDevice;			///< pointer to cSoftHdDevice
	cSoftHdAudio *m_pAudio;				///< pointer to cSoftHdAudio
	cDecodingThread *m_pDecodingThread; ///< pointer to decoding thread
	cDisplayThread *m_pDisplayThread;	///< pointer to display thread
	cFilterThread *m_pFilterThread;		///< pointer to deinterlace filter thread

	cCondWait m_waitCleanCondition;		///< condition to wait on while display cleanup
	cMutex m_waitCleanMutex;			///< mutex used while display cleanup
	cMutex m_trickspeedMutex;
	cMutex m_playbackMutex;
	cMutex m_videoClockMutex;
	cMutex m_displayQueue;

	int m_hardwareQuirks;					///< hardware specific quirks

	int m_userReqDisplayWidth;				///< user requested display width
	int m_userReqDisplayHeight;				///< user requested display height
	uint32_t m_userReqDisplayRefreshRate;	///< user requested display refresh rate

	AVFrame *m_framesRb[VIDEO_SURFACES_MAX];///< ringbuffer for frames to be displayed
											///< (VIDEO_SURFACES_MAX is defined in thread.h)
	int m_framesWrite;						///< m_framesRb write pointer
	int m_framesRead;						///< m_framesRb read pointer
	atomic_t m_framesFilled;				///< how many of m_framesRb is used

	int m_trickSpeed;						///< current trick speed
	int m_trickCounter;						///< current trick speed counter
											///< (handles, how much trickspeed frame are left to be rendered)
	int m_trickForward;						///< true, if trickspeed plays forward

	int m_videoIsPaused;					///< true, if video is paused
	int m_closing;							///< flag if render thread should be closed
											///< a black frame is set instead of video frame
	int m_flushing;							///< flag if render thread should be closed
											///< in difference to m_closing, the video frame is untouched,
											///< i.e. the last one remains displayed
	int m_flushLastFrame;					///< flag about need to clear the last video frame in next turn
											///< i.e. when did m_flushing and the video frame hasn't been freed
	int m_exitThread;						///< internal flag, which is set, when display thread should be stopped

	int m_numFramesToFilter;				///< number of frames to be filtered
	int m_deintDisabled;					///< set, if deinterlacer is disabled
	int m_configDeintDisabled;				///< set, if a deinterlacer on/off should be triggered

	int m_disableOglOsd;					///< set, if ogl osd is disabled

	int m_startgrab;						///< internal flag to trigger grabbing
	cCondWait m_grabCond;					///< condition gets signalled, if renederer finished to clone the grabbed buffers
	cSoftHdGrab m_grabOsd;					///< keeps the current grabbed osd
	cSoftHdGrab m_grabVideo;				///< keeps the current grabbde video

	int m_startCounter;						///< counter for displayed frames, indicates a video start
	int m_framesDuped;						///< number of frames duplicated
	int m_framesDropped;					///< number of frames dropped
	AVRational *m_timebase;					///< pointer to AVCodecContext pkts_timebase
	int64_t m_pts;							///< current video PTS

	int m_fdDrm;							///< drm file descriptor
	drmModeModeInfo m_drmModeInfo;
	drmModeCrtc *m_drmModeCrtcSaved;
	drmEventContext m_drmEventCtx;

	struct {
	int x;
	int y;
	int width;
	int height;
	int is_scaled;
	} m_videoParam;							///< parameters of the current video

	struct drm_buf m_buffer[RENDERBUFFERS];	///< array of drm buffer structs
	struct drm_buf *m_pBufOsd;				///< pointer to osd drm buffer struct
	struct drm_buf m_bufBlack;				///< black drm buffer
	int m_useZpos;							///< is set, if drm hardware can use zpos
	uint64_t m_zposOverlay;					///< zpos of overlay plane
	uint64_t m_zposPrimary;					///< zpos of primary plane
	uint32_t m_connectorId;					///< current connector ID
	uint32_t m_crtcId;						///< current crtc ID
	uint32_t m_crtcIndex;					///< current crtc index
	struct plane *m_pPlanes[MAX_PLANES];	///< array of plane structs (OSD + VIDEO)
	struct lastFrame *m_pLastFrame;			///< pointer to last rendered frame struct (e.g. needed for later free)
	int m_numBuffers;						///< numer of framebuffers currently set up
	int m_enqueueBufferIdx;					///< index of the current (sw) framebuffer in the array
	int m_osdShown;							///< set, if osd is shown currently

#ifdef USE_GLES
	EGLSurface m_eglSurface;
	EGLDisplay m_eglDisplay;
	EGLContext m_eglContext;
	int m_glInitiated;						///< true, if OpenGL/ES context is initiated

	struct gbm_device *m_pGbmDevice;
	struct gbm_surface *m_pGbmSurface;
	struct gbm_bo *m_bo;
	struct gbm_bo *m_pOldBo;
	struct gbm_bo *m_pNextBo;

	int InitEGL(void);
	EGLConfig GetEGLConfig(void);
	struct drm_buf *drm_get_buf_from_bo(struct gbm_bo *);
#endif

	void SetVideoClock(int64_t);
	int ShouldWaitForAudio(void);
};

#endif
