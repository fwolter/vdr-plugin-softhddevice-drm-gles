///
///	@file video.h	@brief Video module header file
///
///	Copyright (c) 2009 - 2015 by Johns.  All Rights Reserved.
///	Copyright (c) 2018 by zille.  All Rights Reserved.
///
///	Contributor(s):
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
//////////////////////////////////////////////////////////////////////////////

/// @addtogroup Video
/// @{

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

//----------------------------------------------------------------------------
//	Defines
//----------------------------------------------------------------------------

//#define VIDEO_SURFACES_MAX	3	///< video output surfaces for queue
#define RENDERBUFFERS		36	///< render video buffers

#define VIDEO_PLANE		0
#define OSD_PLANE		1
#define MAX_PLANES		2

// CodecMode
// currently never set
#define CODEC_DISABLE_MPEG_HW	1 << 0	///< disable mpeg hardware decoder
#define CODEC_DISABLE_H264_HW	1 << 1	///< disable h264 hardware decoder

// Hardware quirks, that are set depending on the hardware used
#define QUIRK_NO_HW_DEINT		1 << 0	///< set, if no hw deinterlacer
#define QUIRK_CODEC_FLUSH_WORKAROUND	1 << 1	///< set, if we have to close and reopen the codec instead of avcodec_flush_buffers (rpi)
#define QUIRK_CODEC_NEEDS_EXT_INIT	1 << 2	///< set, if codec needs some infos for init (coded_width and coded_height)
#define QUIRK_CODEC_SKIP_FIRST_FRAMES	1 << 3	///< set, if codec should skip first I-Frames

#define QUIRK_CODEC_SKIP_NUM_FRAMES	2 ///< skip QUIRK_CODEC_SKIP_NUM_FRAMES, in case QUIRK_CODEC_SKIP_FIRST_FRAMES is set

//----------------------------------------------------------------------------
//	Typedefs
//----------------------------------------------------------------------------
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

#define FRAME_FLAG_TRICKSPEED		1 << 0
#define FRAME_FLAG_STILLPICTURE		1 << 1

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

class cVideoRender
{
private:
    int VideoDisplayWidth;
    int VideoDisplayHeight;
    uint32_t VideoDisplayRefresh;

    pthread_cond_t WaitCleanCondition;
    pthread_mutex_t WaitCleanMutex;

    pthread_mutex_t TrickSpeedMutex;
    pthread_mutex_t PlaybackMutex;
    pthread_mutex_t VideoClockMutex;
    pthread_mutex_t DisplayQueue;

    cDisplayThread *DisplayThread;
    cFilterThread *FilterThread;

    AVFrame  *FramesRb[VIDEO_SURFACES_MAX];
    int FramesWrite;			///< write pointer
    int FramesRead;			///< read pointer
    atomic_t FramesFilled;		///< how many of the buffer is used

    int TrickSpeed;			///< current trick speed
    int TrickCounter;			///< current trick speed counter
    int TrickForward;		///< true, if trickspeed plays forward

    int VideoPaused;
    int Closing;			///< flag about closing render thread
    int Flushing;			///< flag about flushing render thread
    int FlushLast;			///< flag about need to clear FB in next turn
    int ExitThread;

    int FilterFrames;
    int FilterDeintDisabled;	///< Deinterlacer disabled flag
    int ConfigFilterDeintDisabled;	///< Deinterlacer is disabled set via setup

    int DisableOglOsd;		///< ogl osd disabled flag

    int startgrab;			///< flag for triggering grabbing
    cCondWait grabWait;
    cSoftHdGrab grabOsd;
    cSoftHdGrab grabVideo;

    int StartCounter;			///< counter for video start
    int FramesDuped;			///< number of frames duplicated
    int FramesDropped;			///< number of frames dropped
    AVRational *timebase;		///< pointer to AVCodecContext pkts_timebase
    int64_t pts;

    int CodecMode;			/// CODEC_BY_ID, CODEC_NO_MPEG_HW, CODEC_V4L2M2M_H264

    int fd_drm;
    drmModeModeInfo mode;
    drmModeCrtc *saved_crtc;
    drmEventContext ev;
    struct {
	int x;
	int y;
	int width;
	int height;
	int is_scaled;
    } video;
    struct drm_buf bufs[RENDERBUFFERS];
    struct drm_buf *buf_osd;
    struct drm_buf buf_black;
    int use_zpos;
    uint64_t zpos_overlay;
    uint64_t zpos_primary;
    uint32_t connector_id, crtc_id, crtc_index;
    struct plane *planes[MAX_PLANES];
    struct lastFrame *lastframe;
    int buffers;
    int enqueue_buffer;
    int OsdShown;
//

#ifdef USE_GLES
    struct gbm_device *gbm_device;
    struct gbm_surface *gbm_surface;
    struct gbm_bo *bo;
    struct gbm_bo *old_bo;
    struct gbm_bo *next_bo;
#endif

public:
    cVideoRender(cSoftHdDevice *);
    virtual ~cVideoRender(void);

    cSoftHdDevice *Device;
    cSoftHdAudio *Audio;

    cDecodingThread *DecodeThread;
    int HardwareQuirks;		/// hardware specific quirks
#ifdef USE_GLES
    EGLSurface eglSurface;
    EGLDisplay eglDisplay;
    EGLContext eglContext;
    int GlInit;
#endif

/// methods
    void StartThreads(void);

    /// Render a ffmpeg frame.
    int VideoRenderFrame(AVCodecContext *, AVFrame *, int flags);

    /// Clear OSD.
    void VideoOsdClear(void);

    /// Draw an OSD ARGB image.
    void VideoOsdDrawARGB(int, int, int, int, int, const uint8_t *, int, int);

    /// Set closing flag.
    void VideoSetClosing(int);

    /// Deal with trick play mode
    void VideoTrickSpeed(int, int);
    void VideoSetTrickSpeed(int, int);
    int VideoGetTrickSpeed(void);
    int VideoGetTrickCounter(void);
    int VideoGetTrickForward(void);
    void VideoSetTrickCounter(int);
    int VideoDecTrickCounter(void);

    /// Set video output position and size
    void VideoSetOutputPosition(int, int, int, int);

    void VideoPause(void);
    void VideoResume(void);
    int VideoIsPaused(void);

    void VideoPlay(void);

    /// Grab screen
    void VideoTriggerGrab(cCondWait *wait);
    void VideoConvertVideoBufToRgb(void);
    void VideoConvertOsdBufToRgb(void);
    void VideoClearGrab(void);
    cSoftHdGrab *VideoGetGrab(int *, int *, int *, int *, int *, int);

    /// Get decoder statistics.
    void VideoGetStats(int *, int *, int *);

    /// Get screen size
    void VideoGetScreenSize(int *, int *, double *);

    /// Set display resolution
    void VideoSetDisplay(const char *);

    /// Get video clock.
    int64_t VideoGetClock(void);

    /// Set video clock.
    void VideoSetClock(int64_t);

    /// Display handler.
    void VideoThreadWakeup(int, int);
    void VideoThreadExit(void);

    void VideoInit(void);	///< Setup video module.
    void VideoExit(void);		///< Cleanup and exit video module.

    int VideoCodecMode(void);
    void VideoSetDisableDeint(int);
    void VideoSetDisableOglOsd(void);

    int SetPlanePropertyRequest(drmModeAtomicReqPtr, uint32_t, const char *, uint64_t);
    void SetPlaneZpos(drmModeAtomicReqPtr, struct plane *);
    void SetPlane(drmModeAtomicReqPtr, struct plane *);
    void ReadHWPlatform(void);
    int CheckZpos(struct plane *, uint64_t);
    int32_t find_crtc_for_connector(const drmModeRes *, const drmModeConnector *);
    int init_gbm(int, int, uint32_t, uint64_t);

    int FindDevice(void);
    int Frame2Display(void);

#ifdef USE_GLES
    EGLConfig get_config(void);
    int init_egl(void);
    struct drm_buf *drm_get_buf_from_bo(struct gbm_bo *);
#endif
    int SetupFB(struct drm_buf *, AVDRMFrameDescriptor *);
    void DestroyFB(struct drm_buf *);

    void *GrabHandlerThread(void *);
    void CleanDisplayThread(void);
    int VideoDrmCommit(struct drm_buf *, int);
    int VideoSync(AVFrame *, int *, struct drm_buf **);
    int VideoGetFrame(AVFrame **);
    int VideoGetBuffer(AVFrame *, struct drm_buf **);
    int check_closing(int *, struct drm_buf **);
    int check_pausing(int *);
    int check_pausing_with_sync(int *);

    void *DisplayHandlerThread(void *);
    void *DecodeHandlerThread(void *);
    void EnqueueFB(AVFrame *);
    void *FilterHandlerThread(void *);
    int VideoFilterInit(const AVCodecContext *, AVFrame *);

    void StartVideo(void);
    int ShouldClose(void) { return Closing; };
    int ShouldFlush(void) { return Flushing; };
    int DrmHandleEvent(void);

    void ClearFilterFrames(void) { FilterFrames = 0; };
    void IncFilterFrames(void) { FilterFrames++; };
    void DecFilterFrames(void) { FilterFrames--; };

    int GetFramesFilled(void) { return atomic_read(&FramesFilled); };
    void PushFrame(AVFrame *);
    AVFrame *GetFrame(void);
    void FramesRbLock(void);
    void FramesRbUnlock(void);

};

/// @}
#endif
