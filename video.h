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
#include <libavfilter/avfilter.h>

#ifdef USE_GLES
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#endif

#include "iatomic.h"
#include "softhddev.h"

//----------------------------------------------------------------------------
//	Defines
//----------------------------------------------------------------------------

#define VIDEO_SURFACES_MAX	3	///< video output surfaces for queue
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

struct grabimage {
	uint8_t *result;		///< pointer to grabbed image
	struct drm_buf *buf;		///< pointer to original buffer
	int size;			///< returned grabbed data size
	int width;			///< returned grab width
	int height;			///< returned grab height
	int x;				///< x coord in screenshot
	int y;				///< y coord in screenshot
};

struct _Drm_Render_
{
	AVFrame  *FramesDeintRb[VIDEO_SURFACES_MAX];
	int FramesDeintWrite;			///< write pointer
	int FramesDeintRead;			///< read pointer
	atomic_t FramesDeintFilled;		///< how many of the buffer is used

	AVFrame  *FramesRb[VIDEO_SURFACES_MAX];
	int FramesWrite;			///< write pointer
	int FramesRead;			///< read pointer
	atomic_t FramesFilled;		///< how many of the buffer is used

	VideoStream *Stream;		///< video stream
	int TrickSpeed;			///< current trick speed
	int TrickCounter;			///< current trick speed counter
	int TrickForward;		///< true, if trickspeed plays forward
	int VideoPaused;
	int Closing;			///< flag about closing render thread
	int Flushing;			///< flag about flushing render thread
	int FlushLast;			///< flag about need to clear FB in next turn
	int FilterClosing;		///< flag about closing filter handler thread
	int Filter_Bug;
	int Filter_Trick;		///< FilterHandlerThread handles trickframes
	int Filter_Frames;
	int FilterDeintDisabled;	///< Deinterlacer is disabled via setup
	int startgrab;			///< flag for triggering grabbing
	int grabvideoready;		///< flag for finished video grabbing
	int grabosdready;		///< flag for finished osd grabbing
	struct grabimage *grabvideo;	///< struct with grabbed video image
	struct grabimage *grabosd;	///< struct with grabbed osd image
	int grabinwork;			///< grab is in work

	int StartCounter;			///< counter for video start
	int FramesDuped;			///< number of frames duplicated
	int FramesDropped;			///< number of frames dropped
	AVRational *timebase;		///< pointer to AVCodecContext pkts_timebase
	int64_t pts;

	int CodecMode;			/// CODEC_BY_ID, CODEC_NO_MPEG_HW, CODEC_V4L2M2M_H264
	int HardwareQuirks;		/// hardware specific quirks

	AVFilterGraph *filter_graph;
	AVFilterContext *buffersrc_ctx, *buffersink_ctx;

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

#ifdef USE_GLES
	struct gbm_device *gbm_device;
	struct gbm_surface *gbm_surface;
	EGLSurface eglSurface;
	EGLDisplay eglDisplay;
	EGLContext eglContext;
	struct gbm_bo *bo;
	struct gbm_bo *old_bo;
	struct gbm_bo *next_bo;
	int GlInit;
#endif
};

    /// Video hardware decoder typedef
typedef struct _Drm_Render_ VideoRender;

    /// Video output stream typedef
typedef struct __video_stream__ VideoStream; 		// in softhddev.h ?
typedef struct _video_decoder_ VideoDecoder;

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------

extern int VideoAudioDelay;		///< audio/video delay

//----------------------------------------------------------------------------
//	Prototypes
//----------------------------------------------------------------------------

    /// Allocate new video hardware decoder.
extern VideoRender *VideoNewRender(VideoStream *);

    /// Deallocate video hardware decoder.
extern void VideoDelRender(VideoRender *);

    /// Callback to negotiate the PixelFormat.
extern enum AVPixelFormat Video_get_format(VideoRender *, AVCodecContext *,
    const enum AVPixelFormat *);

    /// Render a ffmpeg frame.
extern int VideoRenderFrame(VideoRender *, AVCodecContext *,
    AVFrame *, int flags);

    /// Set audio delay.
extern void VideoSetAudioDelay(int);

    /// Clear OSD.
extern void VideoOsdClear(VideoRender *);

    /// Draw an OSD ARGB image.
extern void VideoOsdDrawARGB(VideoRender *, int, int, int,
		int, int, const uint8_t *, int, int);

    /// Set closing flag.
extern void VideoSetClosing(VideoRender *, int);

    /// Deal with trick play mode
extern void VideoTrickSpeed(VideoRender *, int, int);
extern void VideoSetTrickSpeed(VideoRender *, int, int);
extern int VideoGetTrickSpeed(VideoRender *);
extern int VideoGetTrickCounter(VideoRender *);
extern int VideoGetTrickForward(VideoRender *);
extern void VideoSetTrickCounter(VideoRender *, int);
extern int VideoDecTrickCounter(VideoRender *);

    /// Set video output position and size
extern void VideoSetOutputPosition(VideoRender *, int, int, int, int);

extern void VideoPause(VideoRender *);
extern void VideoResume(VideoRender *);
extern int VideoIsPaused(VideoRender *);

extern void VideoPlay(VideoRender *);

    /// Grab screen
void VideoGrab(struct grabimage *, struct drm_buf *, int *, int);
extern void VideoTriggerGrab(VideoRender *);
extern void VideoClearGrab(VideoRender *);
extern int VideoGrabReady(VideoRender *);
extern int VideoGrabInWork(VideoRender *);
extern uint8_t *VideoGetGrab(VideoRender *, int *, int *, int *, int *, int *, int);

    /// Get decoder statistics.
extern void VideoGetStats(VideoRender *, int *, int *, int *);

    /// Get screen size
extern void VideoGetScreenSize(VideoRender *, int *, int *, double *);

    /// Set display resolution
extern void VideoSetDisplay(const char *);

    /// Get video clock.
extern int64_t VideoGetClock(const VideoRender *);

    /// Set video clock.
extern void VideoSetClock(VideoRender *, int64_t);

    /// Display handler.
extern void VideoThreadWakeup(VideoRender *, int, int);
extern void VideoThreadExit(void);
extern int VideoDecodeThreadRunning(void);

extern void VideoInit(VideoRender *);	///< Setup video module.
extern void VideoExit(VideoRender *);		///< Cleanup and exit video module.

extern int VideoCodecMode(VideoRender *);

#ifdef USE_GLES
extern int DisableOglOsd;
#endif

/// @}
#endif
