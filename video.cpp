/**
 * @file video.cpp
 * @brief Rendering module
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

#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <stdbool.h>
#include <unistd.h>

#include <inttypes.h>

#include <libintl.h>

#ifdef USE_GLES
#include <assert.h>
#endif
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <drm_fourcc.h>

#include "logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>

#include "misc.h"
}
#include "buf2rgb.h"

#include "video.h"
#include "audio.h"
#include "drm.h"
#include "threads.h"
#include "grab.h"
#include "drmdevice.h"

/*****************************************************************************
 * cVideoRender class
 ****************************************************************************/

 /**
 * @brief cVideoRender constructor
 *
 * @param device	pointer to cSoftHdDevice
 */
cVideoRender::cVideoRender(cSoftHdDevice *device)
{
	m_pDevice = device;
	m_pAudio = m_pDevice->Audio;
	m_pDrmDevice = new cDrmDevice(this);
}

 /**
 * @brief cVideoRender destructor
 */
cVideoRender::~cVideoRender(void)
{
	LOGDEBUG2(L_DRM, "~cVideoRender");
	if (m_pFilterThread)
		delete m_pFilterThread;
	if (m_pDisplayThread)
		delete m_pDisplayThread;
	if (m_pDecodingThread)
		delete m_pDecodingThread;
	free(m_pLastFrame);
	LOGDEBUG2(L_DRM, "~cVideoRender deleted");
}

 /**
 * @brief Prepare the threads and process variables
 */
void cVideoRender::Prepare(void)
{
	m_pDecodingThread = new cDecodingThread(m_pDevice);
	m_pDisplayThread = new cDisplayThread(this);
	m_pFilterThread = new cFilterThread(this);

	atomic_set(&m_framesFilled, 0);
	m_closing = 0;
	m_flushing = 0;
	m_flushLastFrame = 0;
	m_deintDisabled = m_configDeintDisabled;
	m_enqueueBufferIdx = 0;
	m_pLastFrame = (struct lastFrame *)calloc(1, sizeof(struct lastFrame));
	ResumeVideo();
}

 /**
 * @brief Set the display resolution and refresh rate based on a user given string
 *
 * @param resolution	string formatted like "1920x1080@50"
 */
void cVideoRender::SetDisplayResolution(const char* resolution)
{
	int userReqDisplayWidth;
	int userReqDisplayHeight;
	int userReqDisplayRefreshRate;

	sscanf(resolution, "%dx%d@%d", &userReqDisplayWidth, &userReqDisplayHeight, &userReqDisplayRefreshRate);

	m_pDrmDevice->SetUserReqDisplayWidth(userReqDisplayWidth);
	m_pDrmDevice->SetUserReqDisplayHeight(userReqDisplayHeight);
	m_pDrmDevice->SetUserReqDisplayRefreshRate(userReqDisplayRefreshRate);
}

///
/// Clean DRM
///
void cVideoRender::CleanUp(void)
{
	AVFrame *frame;
	int i;

	// first wait for m_pFilterThread to be closed
	if (m_pFilterThread->Active()) {
		LOGDEBUG("CleanUp: cancel filter thread");
		m_pFilterThread->Stop();
	}

	FramesRbLock();
	while (atomic_read(&m_framesFilled)) {
		frame = RbGetFrame();
		av_frame_free(&frame);
	}
	FramesRbUnlock();

	if (m_closing && m_pLastFrame->frame) {
		av_frame_free(&m_pLastFrame->frame);
		m_pLastFrame->trickspeed = 0;
		m_pLastFrame->buf = nullptr;
	}

	// Destroy FBs
	for (i = 0; i < RENDERBUFFERS; ++i) {
		if (!m_buffer[i].IsDirty())
			continue;

		// TODO: can m_pLastFrame->buf ever be the black buffer??
		if (m_closing || (m_buffer[i].Id() != m_pLastFrame->buf->Id())) {
			m_buffer[i].Destroy();
		}
	}

	m_numBuffers = 0;
	m_enqueueBufferIdx = 0;

	m_waitCleanCondition.Signal();
	if (m_flushing)
		m_flushLastFrame = 1;
	m_flushing = 0;
	m_closing = 0;
	m_deintDisabled = m_configDeintDisabled;

	LOGDEBUG("CleanUp: DRM cleaned (m_framesFilled %d m_numFramesToFilter %d)", atomic_read(&m_framesFilled), atomic_read(&m_numFramesToFilter));
}

///
///	Commit the frame to the hardware
///
//	retval 2	VIDEO and OSD modesetting and commit was done, need to process outstanding DRM events
//	retval 1	VIDEO only modesetting and commit was done, need to process outstanding DRM events
//	retval 0	OSD only modesetting and commit was done, need to process outstanding DRM events
//	retval -1	no modesetting and commit was done
//	retval -2	something went wrong, no modesetting was done
//
///
int cVideoRender::CommitBuffer(cDrmBuffer *buf, int skip_video)
{
	int dirty = 0; // 0: no commit, 1: osd only, 2: video only, 3: both
	AVFrame *frame = NULL;
	int fdDrm = m_pDrmDevice->Fd();

	cDrmPlane *videoPlane = m_pDrmDevice->VideoPlane();
	cDrmPlane *osdPlane = m_pDrmDevice->OsdPlane();

	uint64_t DispWidth = m_pDrmDevice->DisplayWidth();
	uint64_t DispHeight = m_pDrmDevice->DisplayHeight();
	uint64_t DispX = 0;
	uint64_t DispY = 0;
	uint64_t PicWidth = 0;
	uint64_t PicHeight = 0;

	drmModeAtomicReqPtr ModeReq;
	uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
	if (!(ModeReq = drmModeAtomicAlloc())) {
		LOGERROR("DisplayFrame: cannot allocate atomic request (%d): %m", errno);
		return -2;
	}

	if (skip_video)
		goto skip_video;

	if (buf)
		frame = buf->Frame();

	// handle the video plane
	// Get video size and position and set crtc rect
	if (m_videoIsScaled) {
		DispWidth = (uint64_t)m_videoRect.Width();
		DispHeight = (uint64_t)m_videoRect.Height();
		DispX = (uint64_t)m_videoRect.X();
		DispY = (uint64_t)m_videoRect.Y();
	}

	PicWidth = DispWidth;
	PicHeight = DispHeight;

	// resize frame to fit into video area/ screen and keep the aspect ratio
	if (frame) {
		// use frame->sample_aspect_ratio of 1.0f if undefined (0.0f), otherwise we have division by 0
		double frame_sar = av_q2d(frame->sample_aspect_ratio) ? av_q2d(frame->sample_aspect_ratio) : 1.0f;

		// frame b*h < display b*h, e.g. fit a 4:3 frame into 16:9 display or area
		if (1000 * DispWidth / DispHeight > 1000 * frame->width / frame->height * frame_sar) {
			PicWidth = DispHeight * frame->width / frame->height * frame_sar;
			if (PicWidth <= 0 || PicWidth > DispWidth) {
				PicWidth = DispWidth;
			}
		// frame b*h >= display b*h, e.g. fit a 16:9 frame into 4:3 display or area
		} else {
			PicHeight = DispWidth * frame->height / frame->width / frame_sar;
			if (PicHeight <= 0 || PicHeight > DispHeight) {
				PicHeight = DispHeight;
			}
		}
	}

	videoPlane->SetParams(m_pDrmDevice->CrtcId(), buf->Id(),
		DispX + (DispWidth - PicWidth) / 2, DispY + (DispHeight - PicHeight) / 2, PicWidth, PicHeight,
		0, 0, buf->Width(), buf->Height());

	// set dimensions for grab early, because we might skip this at the next frame
	m_lastVideoGrab.Set(DispX + (DispWidth - PicWidth) / 2,
		DispY + (DispHeight - PicHeight) / 2,
		PicWidth, PicHeight);

	videoPlane->SetPlane(ModeReq);
	dirty += 2;
//	LOGDEBUG2(L_DRM, "DisplayFrame: SetPlane Video (fb = %" PRIu64 ")", videoPlane->GetFbId());

skip_video:
	// handle the osd plane
	// We had draw activity on the osd buffer
	if (m_pBufOsd && m_pBufOsd->IsDirty()) {
		if (m_pDrmDevice->UseZpos()) {
			videoPlane->SetZpos(m_osdShown ? m_pDrmDevice->ZposPrimary() : m_pDrmDevice->ZposOverlay());
			osdPlane->SetZpos(m_osdShown ? m_pDrmDevice->ZposOverlay() : m_pDrmDevice->ZposPrimary());
			videoPlane->SetPlaneZpos(ModeReq);
			osdPlane->SetPlaneZpos(ModeReq);

			LOGDEBUG2(L_DRM, "DisplayFrame: SetPlaneZpos: video->plane_id %d -> zpos %" PRIu64 ", osd->plane_id %d -> zpos %" PRIu64 "",
				videoPlane->GetId(), videoPlane->GetZpos(),
				osdPlane->GetId(), osdPlane->GetZpos());
		}

		osdPlane->SetParams(m_pDrmDevice->CrtcId(), m_pBufOsd->Id(),
			0, 0, m_osdShown ? m_pBufOsd->Width() : 0, m_osdShown ? m_pBufOsd->Height() : 0,
			0, 0, m_osdShown ? m_pBufOsd->Width() : 0, m_osdShown ? m_pBufOsd->Height() : 0);

		osdPlane->SetPlane(ModeReq);
		dirty += 1;
		LOGDEBUG2(L_DRM, "DisplayFrame: SetPlane OSD %d (fb = %" PRIu64 ")", m_osdShown, osdPlane->GetFbId());
		m_pBufOsd->MarkClean();
	}

	if (m_startgrab) {
		if (m_pBufOsd && m_osdShown) {
			LOGDEBUG2(L_GRAB, "DisplayFrame: Trigger osd grab arrived");
			cDrmBuffer *osdBuf = new cDrmBuffer(m_pBufOsd);
			// dimensions should be the size on screen
			m_grabOsd.SetRect(0, 0, m_pBufOsd->Width(), m_pBufOsd->Height());
			m_grabOsd.SetBuf(osdBuf);
		}

		cDrmBuffer *pbuf = buf ? buf : (m_pLastFrame->buf ? m_pLastFrame->buf : NULL);
		if (pbuf) {
			LOGDEBUG2(L_GRAB, "DisplayFrame: Trigger video grab arrived");
			cDrmBuffer *videoBuf = new cDrmBuffer(pbuf);
			// use dimensions which have been set earlier
			m_grabVideo.SetRect(m_lastVideoGrab.X(), m_lastVideoGrab.Y(), m_lastVideoGrab.Width(), m_lastVideoGrab.Height());
			m_grabVideo.SetBuf(videoBuf);
		}
		m_grabCond.Broadcast();
	}

	// return without an atomic commit (no video frame and osd activity)
	if (!dirty) {
		drmModeAtomicFree(ModeReq);
		return -1;
	}

	// do the atomic commit
	if (drmModeAtomicCommit(fdDrm, ModeReq, flags, NULL) != 0) {
		osdPlane->DumpParameters();
		if (dirty > 1)
			videoPlane->DumpParameters();

		drmModeAtomicFree(ModeReq);
		LOGERROR("DisplayFrame: page flip failed (%d): %m", errno);
		return -2;
	}

	drmModeAtomicFree(ModeReq);

	return dirty - 1;
}

///
///	Sync the frames
///
//	retval 1	close or flush requested, skip video or show black frame
//	retval 0	nothing to sync or paused
//	retval -1	drop frame
//
///
int cVideoRender::Sync(AVFrame *frame, int *skip_video, cDrmBuffer **buf)
{
	int64_t audio_pts;
	int64_t video_pts;

	video_pts = frame->pts * 1000 * av_q2d(*m_timebase);

	if(!m_startCounter && !m_closing) {
		LOGDEBUG("Sync: start PTS %s", Timestamp2String(video_pts));
		m_pAudio->Skip(frame->pts, 0);
avready:
		if (m_pAudio->VideoReady(video_pts)) {
			usleep(10000);

			// check for close/flush request or pause
			if (m_closing) {
				LOGDEBUG2(L_DRM, "DisplayFrame: closing while sync, set a black FB");
				*buf = &m_bufBlack;
				return 1;
			} else if (m_flushing) {
				LOGDEBUG2(L_DRM, "DisplayFrame: flushing while sync, skip video");
				*skip_video = 1;
				return 1;
			} else if (VideoIsPaused()) {
				return 0;
			}

			goto avready;
		}
	}

audioclock:
	// check for close/flush request or pause
	if (m_closing) {
		LOGDEBUG2(L_DRM, "DisplayFrame: closing while sync, set a black FB");
		*buf = &m_bufBlack;
		return 1;
	} else if (m_flushing) {
		LOGDEBUG2(L_DRM, "DisplayFrame: flushing while sync, skip video");
		*skip_video = 1;
		return 1;
	} else if (VideoIsPaused()) {
		return 0;
	}

	audio_pts = m_pAudio->GetClock();

	if (audio_pts == (int64_t)AV_NOPTS_VALUE) {
		usleep(20000);
		goto audioclock;
	}

	int diff = video_pts - audio_pts - m_pDevice->GetVideoAudioDelay();

	if (abs(diff) > 5000) {	// more than 5s
		LOGDEBUG2(L_AV_SYNC, "More then 5s Pkts %d deint %d, Frames %d UsedBytes %d audio %s video %s Delay %dms diff %dms",
			m_pDevice->VideoStream->GetPacketsFilled(), m_pFilterThread->GetRbFramesFilled(),
			atomic_read(&m_framesFilled), m_pAudio->GetUsedBytes(), Timestamp2String(audio_pts),
			Timestamp2String(video_pts), m_pDevice->GetVideoAudioDelay(), diff);
	}

	if (diff < -5 && !(abs(diff) > 5000)) {	// video is more than 5ms behind audio, drop video frame
		LOGDEBUG2(L_AV_SYNC, "FrameDropped (drop %d, dup %d) Pkts %d deint %d Frames %d UsedBytes %d audio %s video %s Delay %dms diff %dms",
			m_framesDropped, m_framesDuped,
			m_pDevice->VideoStream->GetPacketsFilled(), m_pFilterThread->GetRbFramesFilled(),
			atomic_read(&m_framesFilled), m_pAudio->GetUsedBytes(), Timestamp2String(audio_pts),
			Timestamp2String(video_pts), m_pDevice->GetVideoAudioDelay(), diff);

		if (!m_startCounter)
			m_startCounter++;

		m_framesDropped++;
		return -1;
	}

	if (diff > 35 && !(abs(diff) > 5000)) {	// audio is more than 35ms behind video, duplicate video frame
		LOGDEBUG2(L_AV_SYNC, "FrameDuped (drop %d, dup %d) Pkts %d deint %d Frames %d UsedBytes %d audio %s video %s Delay %dms diff %dms",
			m_framesDropped, m_framesDuped,
			m_pDevice->VideoStream->GetPacketsFilled(), m_pFilterThread->GetRbFramesFilled(),
			atomic_read(&m_framesFilled), m_pAudio->GetUsedBytes(), Timestamp2String(audio_pts),
			Timestamp2String(video_pts), m_pDevice->GetVideoAudioDelay(), diff);

		m_framesDuped++;
		usleep(20000);
		goto audioclock;
	}

	return 0;
}

///
///	Get next video frame
///
//	retval 0	received frame with PTS value
//	retval 1	received frame without PTS value
//
///
int cVideoRender::GetFrame(AVFrame **frame)
{
	AVFrame *pframe = NULL;

	pframe = RbGetFrame();
	*frame = pframe;

	if (pframe->pts == AV_NOPTS_VALUE)
		return 1;

	return 0;
}

/**
 * @brief Get frame flags
 *
 * @param frame		AVFrame
 *
 * @returns		FRAME_FLAG_TRICKSPEED or FRAME_FLAG_STILLPICTURE
 */
int cVideoRender::GetFrameFlags(AVFrame *frame)
{
	if (!frame || !frame->opaque_ref)
		return 0;

	FrameData *fd = (FrameData *)frame->opaque_ref->data;
	return fd->flags;
}

/**
 * @brief Set frame flags
 *
 * @param frame		AVFrame
 * @param flags		FRAME_FLAG_TRICKSPEED and/or FRAME_FLAG_STILLPICTURE
 */
void cVideoRender::SetFrameFlags(AVFrame *frame, int flags)
{
	FrameData *fd;
	if (!frame->opaque_ref) {
		frame->opaque_ref = av_buffer_allocz(sizeof(*fd));
		if (!frame->opaque_ref) {
			LOGFATAL("%s: cannot allocate private frame data", __FUNCTION__);
		}
	}

	fd = (FrameData *)frame->opaque_ref->data;
	fd->flags = flags;
}

/**
 * @brief Check, if this is a trickspeed frame
 *
 * @param frame		AVFrame
 *
 * @return		true, if this frame is marked as a trickspeed frame
 */
int cVideoRender::IsTrickspeedFrame(AVFrame *frame)
{
	return GetFrameFlags(frame) & FRAME_FLAG_TRICKSPEED;
}

/**
 * @brief Check, if this is a stillpicture frame
 *
 * @param frame		AVFrame
 *
 * @return		true, if this frame is marked as a sillpicture frame
 */
int cVideoRender::IsStillpictureFrame(AVFrame *frame)
{
	return GetFrameFlags(frame) & FRAME_FLAG_STILLPICTURE;
}

/**
 * @brief Mark this frame as a trickspeed frame
 *
 * @param frame		AVFrame
 */
void cVideoRender::MarkAsTrickspeedFrame(AVFrame *frame)
{
	SetFrameFlags(frame, FRAME_FLAG_TRICKSPEED);
}

/**
 * @brief Mark this frame as a stillpicture frame
 *
 * @param frame		AVFrame
 */
void cVideoRender::MarkAsStillpictureFrame(AVFrame *frame)
{
	SetFrameFlags(frame, FRAME_FLAG_STILLPICTURE);
}

///
///	Get suitable framebuffer for frame
///
//	retval 0	got a buffer
//	retval 1	sth went wrong
//
///
cDrmBuffer *cVideoRender::GetBuffer(AVFrame *frame)
{
	cDrmBuffer *pbuf = nullptr;
	int i;

	AVDRMFrameDescriptor *primedata = (AVDRMFrameDescriptor *)frame->data[0];

	// search for a made fd / FB combination
	for (i = 0; i < RENDERBUFFERS; i++) {
		if (m_flushLastFrame && !m_buffer[i].IsSwBuffer())
			break;

		if (m_buffer[i].IsTrickspeedBuffer() && !m_buffer[i].IsSwBuffer())
			break;

		if (!m_buffer[i].IsDirty())
			continue;

		if (m_buffer[i].FdPrime(0) == primedata->objects[0].fd) {
			pbuf = &m_buffer[i];
			break;
		}
	}

	// search for a "free" buffer
	if (pbuf == nullptr) {
		for (i = 0; i < RENDERBUFFERS; i++) {
			if (!m_buffer[i].IsDirty())
				break;
		}
		if (m_buffer[i].IsDirty()) {
			LOGDEBUG("GetBuffer: SHOULD NOT HAPPEN! no free buffer available!");
			return nullptr;
		}

		pbuf = &m_buffer[i];
		if (pbuf->Setup(m_pDrmDevice->Fd(), (uint32_t)frame->width, (uint32_t)frame->height,
		               0, primedata))
			return nullptr;

		m_buffer[i].MarkAsHwBuffer();
	}

	if (pbuf == nullptr) {
		LOGDEBUG("GetBuffer: SHOULD NOT HAPPEN! failed, no buffer found!");
		return nullptr;
	}

	pbuf->SetTrickspeed(IsTrickspeedFrame(frame));

	return pbuf;
}


///
///	Check if we should wait for audio to come up with video
///
///	retval 0	wait for video to sync with audio
///	retval 1	wait for audio to sync with video
///
///
int cVideoRender::ShouldWaitForAudio(void) {
	int64_t audio_pts = m_pAudio->GetClock();
	int64_t video_pts = GetVideoClock() * 1000 * av_q2d(*m_timebase);
	if (video_pts == AV_NOPTS_VALUE || audio_pts == AV_NOPTS_VALUE)
		return 1;

	int diff = video_pts - audio_pts - m_pDevice->GetVideoAudioDelay();
	// audio is behind video, wait for audio
	if (diff > 0)
		return 1;

	// video is behind audio, so don't wait
	return 0;
}

///
///	Draw a video frame.
///
//	retval 0	modesetting and commit was done, need to process outstanding DRM events
//	retval 1	no new frames or OSD, no modesetting was done, don't process outstanding DRM events
//
///
int cVideoRender::DisplayFrame(void)
{
	cDrmBuffer *buf = NULL;
	AVFrame *frame = NULL;
	int skip_video = 0;
	int timeout; // ms
	int ret;

	if (ShouldClose()) {
		LOGDEBUG2(L_DRM, "DisplayFrame: closing, set a black FB");
		buf = &m_bufBlack;
		goto page_flip;
	}

	if (ShouldFlush()) {
		LOGDEBUG2(L_DRM, "DisplayFrame: flushing, just skip video");
		skip_video = 1;
		goto page_flip;
	}

dequeue:
	timeout = 15; // ms
	// wait for a frame in the ringbuffer
	while (!atomic_read(&m_framesFilled)) {
		if (m_exitThread) {
			LOGDEBUG2(L_DRM, "DisplayFrame -> Exit Thread");
			return 1;
		}

		if (ShouldClose()) {
			LOGDEBUG2(L_DRM, "DisplayFrame: closing, set a black FB");
			buf = &m_bufBlack;
			goto page_flip;
		}

		if (ShouldFlush()) {
			LOGDEBUG2(L_DRM, "DisplayFrame: flushing, just skip video");
			skip_video = 1;
			goto page_flip;
		}

		if (VideoIsPaused()) {
			usleep(10000);
			// LOGDEBUG2(L_DRM, "DisplayFrame: paused, skip video");
			skip_video = 1;
			goto page_flip;
		}

		// wait max. 15ms in case we have an osd
		if (m_pBufOsd && m_pBufOsd->IsDirty() && !timeout--) {
			skip_video = 1;
			LOGDEBUG2(L_DRM, "DisplayFrame: no video but osd, skip video");
			goto page_flip;
		}

		if (m_startgrab) {
			skip_video = 1;
			LOGDEBUG2(L_DRM, "DisplayFrame: grab requested, skip video");
			goto page_flip;
		}

		usleep(1000);
	}

	// if the video is paused, lets wait all remaining audio
	// this is necessary for a correct Play() after Pause()
	if (VideoIsPaused() && ShouldWaitForAudio()) {
		usleep(10000);
		skip_video = 1;
		goto page_flip;
	}

	// advance frame
	if (GetFrame(&frame)) {
		if (IsStillpictureFrame(frame)) {
			LOGDEBUG2(L_STILL, "DisplayFrame: Stillpicture has AV_NOPTS_VALUE, skip sync ...");
			goto skip_sync;
		} else {
			// TODO: fast/soft sync
			LOGDEBUG2(L_DRM, "DisplayFrame: no AV_NOPTS_VALUE, use next frame ...");
			av_frame_free(&frame);
			return 1;
		}
	}

	// skip old audio in trickspeed
	if (IsTrickspeedFrame(frame)) {
		m_pAudio->Skip(frame->pts, 0);
		goto skip_sync;
	}

	// skip old audio after trickspeed
	if (m_pLastFrame->frame && m_pLastFrame->trickspeed) {
		m_pAudio->Skip(frame->pts, 1);
		goto skip_sync;
	}

	// sync audio/video
	ret = Sync(frame, &skip_video, &buf);

	if (ret < 0) { // drop frame (dup is handled within Sync())
		av_frame_free(&frame);
		goto dequeue;
	} else if (ret) { // close or flush (black buffer or skip video)
		av_frame_free(&frame);
		goto page_flip;
	}

	m_startCounter++;

skip_sync:
	// get suitable framebuffer
	buf = GetBuffer(frame);
	if (!buf) {
		av_frame_free(&frame);
		return 1;
	}

	buf->SetFrame(frame);

page_flip:
	// no modesetting was done
	if (CommitBuffer(buf, skip_video) < 0) {
		if (frame)
			av_frame_free(&frame);
		return 1;
	}

	// only osd was set
	if (skip_video)
		return 0;

	// now, that we had a successful commit, set the STC if we have a frame
	if (frame)
		SetVideoClock(frame->pts);

	if (frame)
		LOGDEBUG2(L_PACKET, "DisplayFrame:                 PTS %s", Timestamp2String(frame->pts / 90));

	// new video frame was sent, rotate the frames
	if (m_pLastFrame->frame) {
		// if the m_pLastFrame was a trickframe or a flush is forced, destroy the FB
		if (m_flushLastFrame || m_pLastFrame->trickspeed) {
			m_pLastFrame->buf->Destroy();
			m_pLastFrame->buf = nullptr;
			m_pLastFrame->trickspeed = 0;
			m_flushLastFrame = 0;
		}
		av_frame_free(&m_pLastFrame->frame);
	}

	// TODO: the whole m_pLastFrame handling has to be done better!
	if (buf && buf->Id() != m_bufBlack.Id()) {
		m_pLastFrame->frame = buf->Frame();
		m_pLastFrame->buf = buf;
		m_pLastFrame->trickspeed = buf->IsTrickspeedBuffer();
	}

	if (buf && buf->Id() == m_bufBlack.Id()) {
		m_pLastFrame->buf = nullptr;
		m_pLastFrame->trickspeed = 0;
	}

	return 0;
}

int cVideoRender::DrmHandleEvent(void)
{
	return m_pDrmDevice->HandleEvent();
}

//----------------------------------------------------------------------------
//	OSD
//----------------------------------------------------------------------------

///
///	Clear the OSD.
///
///
void cVideoRender::OsdClear(void)
{
#ifdef USE_GLES
	if (m_disableOglOsd) {
		memset((void *)m_pBufOsd->Plane(0), 0,
			(size_t)(m_pBufOsd->Pitch(0) * m_pBufOsd->Height()));
	} else {
		cDrmBuffer *buf;

		EGL_CHECK(eglSwapBuffers(m_pDrmDevice->EglDisplay(), m_pDrmDevice->EglSurface()));
		m_pNextBo = gbm_surface_lock_front_buffer(m_pDrmDevice->GbmSurface());
		assert(m_pNextBo);

		buf = m_pDrmDevice->GetBufFromBo(m_pNextBo);
		if (!buf) {
			LOGERROR("Failed to get GL buffer");
			return;
		}

		m_pBufOsd = buf;

		// release old buffer for writing again
		if (m_bo)
			gbm_surface_release_buffer(m_pDrmDevice->GbmSurface(), m_bo);

		// rotate bos and create and keep bo as m_pOldBo to make it free'able
		m_pOldBo = m_bo;
		m_bo = m_pNextBo;

		LOGDEBUG2(L_OPENGL, "OsdClear(GL): eglSwapBuffers m_eglDisplay %p eglSurface %p (%i x %i, %i)", m_pDrmDevice->EglDisplay(), m_pDrmDevice->EglSurface(), buf->Width(), buf->Height(), buf->Pitch(0));
	}
#else
	memset((void *)m_pBufOsd->Plane(0), 0,
		(size_t)(m_pBufOsd->Pitch(0) * m_pBufOsd->Height()));
#endif

	m_pBufOsd->MarkDirty();
	m_osdShown = 0;
}

#define MIN(a, b) ((a) < (b) ? (a) : (b))

///
///	Draw an OSD ARGB image.
///
///	@param xi	x-coordinate in argb image
///	@param yi	y-coordinate in argb image
///	@paran height	height in pixel in argb image
///	@paran width	width in pixel in argb image
///	@param pitch	pitch of argb image
///	@param argb	32bit ARGB image data
///	@param x	x-coordinate on screen of argb image
///	@param y	y-coordinate on screen of argb image
///
void cVideoRender::OsdDrawARGB(int xi, int yi,
		int width, int height, int pitch,
		const uint8_t * argb, int x, int y)
{
#ifdef USE_GLES
	if (m_disableOglOsd) {
		LOGDEBUG2(L_OSD, "VideoOsdDrawARGB width %d height %d pitch %d argb %p x %d y %d pitch buf %d xi %d yi %d",
			   width, height, pitch, argb, x, y, m_pBufOsd->Pitch(0), xi, yi);
		for (int i = 0; i < height; ++i) {
			memcpy(m_pBufOsd->Plane(0) + x * 4 + (i + y) * m_pBufOsd->Pitch(0),
				argb + i * pitch, MIN((size_t)pitch, m_pBufOsd->Pitch(0)));
		}
	} else {
		cDrmBuffer *buf;

		EGL_CHECK(eglSwapBuffers(m_pDrmDevice->EglDisplay(), m_pDrmDevice->EglSurface()));
		m_pNextBo = gbm_surface_lock_front_buffer(m_pDrmDevice->GbmSurface());
		assert(m_pNextBo);

		buf = m_pDrmDevice->GetBufFromBo(m_pNextBo);
		if (!buf) {
			LOGERROR("Failed to get GL buffer");
			return;
		}

		m_pBufOsd = buf;

		// release old buffer for writing again
		if (m_bo)
			gbm_surface_release_buffer(m_pDrmDevice->GbmSurface(), m_bo);

		// rotate bos and create and keep bo as m_pOldBo to make it free'able
		m_pOldBo = m_bo;
		m_bo = m_pNextBo;

		LOGDEBUG2(L_OPENGL, "OsdDrawARGB(GL): eglSwapBuffers eglDisplay %p eglSurface %p (%i x %i, %i)", m_pDrmDevice->EglDisplay(), m_pDrmDevice->EglSurface(), buf->Width(), buf->Height(), buf->Pitch(0));
	}
#else
	for (int i = 0; i < height; ++i) {
		memcpy(m_pBufOsd->Plane(0) + x * 4 + (i + y) * m_pBufOsd->Pitch(0)),
			argb + i * pitch, (size_t)pitch);
	}
#endif
	m_pBufOsd->MarkDirty();
	m_osdShown = 1;
}

//----------------------------------------------------------------------------
//	Thread
//----------------------------------------------------------------------------

///
///
///
void cVideoRender::ExitDecodingThread(void)
{
	LOGDEBUG("%s", __FUNCTION__);

	if (m_pDecodingThread->Active())
		m_pDecodingThread->Stop();
}

///
///
///
void cVideoRender::ExitDisplayThread(void)
{
	LOGDEBUG("%s", __FUNCTION__);

	SetClosing(1);
	if (m_pDisplayThread->Active()) {
		m_exitThread = 1;
		m_pDisplayThread->Stop();
	}
}

///
///	Wakeup display thread
///
///	New video arrived, wakeup video thread.
///
void cVideoRender::WakeupDisplayThread(void)
{
	LOGDEBUG("%s", __FUNCTION__);
	if (!m_pDisplayThread->Active())
		m_pDisplayThread->Start();
}

///
///	Wakeup decoding thread
///
///	New video arrived, wakeup video thread.
///
void cVideoRender::WakeupDecodingThread(void)
{
	LOGDEBUG("%s", __FUNCTION__);
	if (!m_pDecodingThread->Active())
		m_pDecodingThread->Start();
}

static void ReleaseFrame( __attribute__ ((unused)) void *opaque, uint8_t *data)
{
	AVDRMFrameDescriptor *primedata = (AVDRMFrameDescriptor *)data;

	av_free(primedata);
}

void cVideoRender::EnqueueFB(AVFrame *inframe)
{
	// inframe->format is always NV12!
	cDrmBuffer *buf = nullptr;
	int fdDrm = m_pDrmDevice->Fd();

	AVDRMFrameDescriptor * primedata;
	AVFrame *frame;
	int i;

	if (IsTrickspeedFrame(inframe)) {	// if we have trickspeed, always use a free buffer, because its destroyed in DisplayFrame after rendering
		for (i = 0; i < RENDERBUFFERS; i++) {
			if (!m_buffer[i].IsDirty())
				break;
		}
		if (m_buffer[i].IsDirty())
			LOGFATAL("EnqueueFB: SHOULD NOT HAPPEN! no free buffer available!");

		buf = &m_buffer[i];
		if (buf->Setup(fdDrm, (uint32_t)inframe->width, (uint32_t)inframe->height,
		               DRM_FORMAT_NV12, NULL)) {
			LOGERROR("EnqueueFB: SetupFB FB %i x %i failed", buf->Width(), buf->Height());
		}

		int primefd;
		if (drmPrimeHandleToFD(fdDrm, buf->Handle(0), DRM_CLOEXEC | DRM_RDWR, &primefd))
			LOGERROR("EnqueueFB: Failed to retrieve the Prime FD (%d): %m", errno);
		buf->SetFdPrime(0, primefd);
	} else {
		// create some buffers up to VIDEO_SURFACES_MAX + 2
get_buffer:
		buf = &m_buffer[m_enqueueBufferIdx];
		if (m_numBuffers < VIDEO_SURFACES_MAX + 2) {
			if (buf->IsDirty()) {
				// skip the buffer, because it is either referenced by m_pLastFrame or is already setup
				// this should be safe, because we only have 1 m_pLastFrame, which should be destroyed as soon as
				// a new buffer arrives in DisplayFrame
				// after that destroy, we should be able to setup 0, 1, 2, ..., VIDEO_SURFACES_MAX + 2 framebuffers
				m_enqueueBufferIdx = (m_enqueueBufferIdx + 1) % (VIDEO_SURFACES_MAX + 2);

				goto get_buffer;
			}

			if (buf->Setup(fdDrm, (uint32_t)inframe->width, (uint32_t)inframe->height,
			               DRM_FORMAT_NV12, NULL)) {
				LOGERROR("EnqueueFB: SetupFB FB %i x %i failed", buf->Width(), buf->Height());
			}

			m_numBuffers++;

			int primefd;
			if (drmPrimeHandleToFD(fdDrm, buf->Handle(0), DRM_CLOEXEC | DRM_RDWR, &primefd))
				LOGERROR("EnqueueFB: Failed to retrieve the Prime FD (%d): %m", errno);
			buf->SetFdPrime(0, primefd);
		}
	}

	// mark this buffer as a software decoded buffer
	buf->MarkAsSwBuffer();

	for (i = 0; i < inframe->height; ++i) {
		memcpy(buf->Plane(0) + i * buf->Pitch(0),
			inframe->data[0] + i * inframe->linesize[0], inframe->linesize[0]);
	}
	for (i = 0; i < inframe->height / 2; ++i) {
		memcpy(buf->Plane(1) + i * buf->Pitch(1),
			inframe->data[1] + i * inframe->linesize[1], inframe->linesize[1]);
	}

	frame = av_frame_alloc();
	frame->pts = inframe->pts;
	frame->width = inframe->width;
	frame->height = inframe->height;
	frame->format = AV_PIX_FMT_DRM_PRIME;
	frame->sample_aspect_ratio.num = inframe->sample_aspect_ratio.num;
	frame->sample_aspect_ratio.den = inframe->sample_aspect_ratio.den;

	if (IsTrickspeedFrame(inframe))
		MarkAsTrickspeedFrame(frame);
	if (IsStillpictureFrame(inframe))
		MarkAsStillpictureFrame(frame);

	primedata = (AVDRMFrameDescriptor *)av_mallocz(sizeof(AVDRMFrameDescriptor));
	primedata->objects[0].fd = buf->FdPrime(0);
	frame->data[0] = (uint8_t *)primedata;
	frame->buf[0] = av_buffer_create((uint8_t *)primedata, sizeof(*primedata),
				ReleaseFrame, NULL, AV_BUFFER_FLAG_READONLY);

	if (m_closing || m_flushing) {
		av_frame_free(&inframe);
		av_frame_free(&frame);
		return;
	}

	FramesRbLock();
	RbPushFrame(frame);
	FramesRbUnlock();

	if (!(IsTrickspeedFrame(inframe)))
		m_enqueueBufferIdx = (m_enqueueBufferIdx + 1) % (VIDEO_SURFACES_MAX + 2);

	av_frame_free(&inframe);
}

///
///	Display a ffmpeg frame
///
///	@param render	video render
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///	@retval 0	success or error, return (frame is freed or moved to the render ringbuffer)
///	@retval	-1	ringbuffer full, try again
///
int cVideoRender::RenderFrame(AVCodecContext * video_ctx, AVFrame * frame)
{
	int interlaced;

	if (!m_startCounter) {
		m_timebase = &video_ctx->pkt_timebase;
	}

	if (frame->decode_error_flags || frame->flags & AV_FRAME_FLAG_CORRUPT) {
		LOGWARNING("RenderFrame: error_flag or FRAME_FLAG_CORRUPT");
	}

	if (m_closing || m_flushing) {
		av_frame_free(&frame);
		return 0;
	}

#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
	interlaced = frame->interlaced_frame;
#else
	interlaced = !!(frame->flags & AV_FRAME_FLAG_INTERLACED);
#endif
	// we can't trust frame->interlaced_frame ...
	// do some tricks in normal playback
	if (!(IsTrickspeedFrame(frame) || IsStillpictureFrame(frame))) {

		// set the interlaced switch depending on the framerate
		if ((video_ctx->framerate.num > 0) &&
			(video_ctx->framerate.num / video_ctx->framerate.den > 30))
			interlaced = 0;
		else if (video_ctx->framerate.num > 0)
			interlaced = 1;

		// set the interlaced switch depending on an active deinterlace filter, if framerate is not available
		if ((video_ctx->framerate.num == 0) &&
			 !interlaced && m_pFilterThread->Active() && m_pFilterThread->IsInterlaceFilter()) {
			LOGWARNING("RenderFrame: WARNING!!! frame without interlaced flag arrived while deinterlace filter is active (P %d)!", ++m_numWrongProgressive);
			interlaced = 1;
		}

		// hevc is always progressive
		if (video_ctx->codec_id == AV_CODEC_ID_HEVC)
			interlaced = 0;

		m_pDevice->VideoStream->SetInterlaced(interlaced);
	}

	if (frame->format == AV_PIX_FMT_YUV420P ||
	   (frame->format == AV_PIX_FMT_DRM_PRIME && interlaced && !((m_hardwareQuirks & QUIRK_NO_HW_DEINT) || m_deintDisabled))) {
		// use deinterlace/scale filter
		// AV_PIX_FMT_YUV420P, interlaced -> software deinterlacer (bwdif filter)
		// AV_PIX_FMT_YUV420P, progressive -> scale filter to get NV12 frames
		// AV_PIX_FMT_DRM_PRIME, interlaced, hw deinterlacer available -> hw deinterlacer
		// -> put the frame into filter Rb
		if (!m_pFilterThread->Active()) {
			LOGDEBUG("RenderFrame: wakeup filter thread");
			if (m_pFilterThread->Init(video_ctx, frame, m_deintDisabled)) {
				av_frame_free(&frame);
				return 0;
			} else {
				m_pFilterThread->Start();
			}
		}

		if (m_pFilterThread->GetRbFramesFilled() < VIDEO_SURFACES_MAX) {
			m_pFilterThread->RbPushFrame(frame);
		} else {
			usleep(10000);
			return -1;
		}
	} else {
		// don't use deinterlace/scale filter
		if (frame->format == AV_PIX_FMT_DRM_PRIME) {
			// AV_PIX_FMT_DRM_PRIME, interlaced, hw deinterlacer not available
			// AV_PIX_FMT_DRM_PRIME, progressive
			// -> put the frame directly in render Rb
			if (m_closing || m_flushing) {
				av_frame_free(&frame);
				return 0;
			}
			FramesRbLock();
			if (atomic_read(&m_framesFilled) < VIDEO_SURFACES_MAX && !m_numFramesToFilter) {
				RbPushFrame(frame);
				FramesRbUnlock();
			} else {
				FramesRbUnlock();
				usleep(1000);
				return -1;
			}
		} else {
			// AV_PIX_FMT_DRM_NV12 ?
			// -> go through EnqueueFB
			FramesRbLock();
			if (atomic_read(&m_framesFilled) < VIDEO_SURFACES_MAX) {
				FramesRbUnlock();
				EnqueueFB(frame);
			} else {
				FramesRbUnlock();
				usleep(5000);
				return -1;
			}
		}
	}
	return 0;
}

void cVideoRender::FramesRbLock(void) {
	m_displayQueue.Lock();
}

void cVideoRender::FramesRbUnlock(void) {
	m_displayQueue.Unlock();
}

void cVideoRender::RbPushFrame(AVFrame * frame) {
	m_framesRb[m_framesWrite] = frame;
	m_framesWrite = (m_framesWrite + 1) % VIDEO_SURFACES_MAX;
	atomic_inc(&m_framesFilled);
}

AVFrame *cVideoRender::RbGetFrame(void) {
	AVFrame *frame = m_framesRb[m_framesRead];
	m_framesRead = (m_framesRead + 1) % VIDEO_SURFACES_MAX;
	atomic_dec(&m_framesFilled);

	return frame;
}

///
///	Set video clock.
///
void cVideoRender::SetVideoClock(int64_t pts)
{
	m_videoClockMutex.Lock();
	m_pts = pts;
	m_videoClockMutex.Unlock();
}

///
///	Get video clock.
///
///	@param hw_decoder	video hardware decoder
///
///	@note this isn't monoton, decoding reorders frames, setter keeps it
///	monotonic
///
int64_t cVideoRender::GetVideoClock(void)
{
	int64_t pts;
	m_videoClockMutex.Lock();
	pts = m_pts;
	m_videoClockMutex.Unlock();
	return pts;
}

///
///	send start condition to video thread.
///
///	@param hw_render	video hardware render
///
void cVideoRender::StartVideo(void)
{
	ResumeVideo();
	m_startCounter = 0;
	LOGDEBUG("StartVideo: reset m_startCounter %d Closing %d TrickSpeed %d",
		m_startCounter, m_closing, GetTrickSpeed());
}

///
///	Close the renderer and clear frames (and) framebuffers if needed
///
///	@param hw_render	video hardware render
///	@param black		true, if should set a black fb and clear the last framebuffer,
///				otherwise wait for the clear until the next frame arrives
///
void cVideoRender::SetClosing(int black)
{
	LOGDEBUG("SetClosing: m_startCounter %d%s", m_startCounter, black ? " closing": " flushing");
	if (!m_pDisplayThread->Active())
		return;

	if (m_pFilterThread->Active())
		m_pFilterThread->Stop();
	m_flushing = !black;
	m_closing = black;

	if (VideoIsPaused())
		ResumeVideo();

	LOGDEBUG("SetClosing: wait for cleanup");
	if (!m_waitCleanCondition.Wait(2000)) {
		LOGERROR("%s: timeout while waiting for cleanup");
	}

	m_startCounter = 0;
	m_framesDuped = 0;
	m_framesDropped = 0;
	m_numWrongProgressive = 0;
	if (black)
		SetTrickSpeed(0, 1);
}

///
//	Pause video.
//
void cVideoRender::PauseVideo(void)
{
	LOGDEBUG("PauseVideo:");
	m_playbackMutex.Lock();
	m_videoIsPaused = 1;
	m_playbackMutex.Unlock();
}

///
//	Resume video.
//
void cVideoRender::ResumeVideo(void)
{
	LOGDEBUG("ResumeVideo:");
	m_playbackMutex.Lock();
	m_videoIsPaused = 0;
	m_playbackMutex.Unlock();
}

///
//	Check pause status
//
int cVideoRender::VideoIsPaused(void)
{
	int ret;
	m_playbackMutex.Lock();
	ret = m_videoIsPaused;
	m_playbackMutex.Unlock();
	return ret;
}

///
///	Simply set trick play speed values
///
///	@param hw_render	video hardware render
///	@param speed		trick speed (0 = normal)
///	@param forward		1 if forward trick speed
///
void cVideoRender::SetTrickSpeed(int speed, int forward)
{
	LOGDEBUG2(L_TRICK, "SetTrickSpeed: set trick speed %d %s", speed, forward ? "forward" : "backward");
	m_trickspeedMutex.Lock();
	m_trickSpeed = speed;
	m_trickCounter = speed;
	m_trickForward = forward;
	m_trickspeedMutex.Unlock();
}

///
///	Return the current trick speed mode
///
int cVideoRender::GetTrickSpeed(void)
{
	int speed;
	m_trickspeedMutex.Lock();
	speed = m_trickSpeed;
	m_trickspeedMutex.Unlock();
	return speed;
}

///
///	Return the current trick speed direction
///
int cVideoRender::GetTrickForward(void)
{
	int dir;
	m_trickspeedMutex.Lock();
	dir = m_trickForward;
	m_trickspeedMutex.Unlock();
	return dir;
}

///
///	Return the current trick counter
///
int cVideoRender::GetTrickCounter(void)
{
	int counter;
	m_trickspeedMutex.Lock();
	counter = m_trickCounter;
	m_trickspeedMutex.Unlock();
	return counter;
}

///
///	Set the trick counter
///
void cVideoRender::SetTrickCounter(int counter)
{
	m_trickspeedMutex.Lock();
	m_trickCounter = counter;
	m_trickspeedMutex.Unlock();
}

///
///	Decrease the trick counter
///
int cVideoRender::DecTrickCounter(void)
{
	int counter;
	m_trickspeedMutex.Lock();
	m_trickCounter--;
	counter = m_trickCounter;
	m_trickspeedMutex.Unlock();
	return counter;
}

///
///	Trigger grabbing in render thread
///
///
int cVideoRender::TriggerGrab(void)
{
	int timeout = 50;
	cMutex mutex;
	mutex.Lock();
	m_startgrab = 1;
	int err = 0;

	if (!m_grabCond.TimedWait(mutex, timeout)) {
		LOGWARNING("%s: timed out after %dms", __FUNCTION__, timeout);
		err = 1;
	}

	m_startgrab = 0;
	return err;
}

///
///	Grab full screen image
///
///	@param grabimage[out]	the struct to grab in
///	@param buf[in]		current video buffer
///	@param ready[out]	ready is set true if we finished
///	@param is_osd		is this an osd grab? (just for logs)
///
void cVideoRender::ConvertVideoBufToRgb(void)
{
	int size;
	cSoftHdGrab *grab = &m_grabVideo;
	cDrmBuffer *buf = grab->GetBuf();

	// early return if buf = NULL
	if (!buf) {
		grab->SetData(NULL);
		grab->SetSize(0);
		return;
	}

	for (int plane = 0; plane < buf->NumPlanes(); plane++) {
		LOGDEBUG2(L_GRAB, "ConvertVideoBufToRgb: VIDEO plane %d address %p pitch %d offset %d handle %d size %d",
			   plane, buf->Plane(plane), buf->Pitch(plane), buf->Offset(plane), buf->Handle(plane), buf->Size(plane));
	}
	// result's width and height are original dimensions how buffer is presented on the screen
	uint8_t * result = buf2rgb(buf, &size, grab->GetWidth(), grab->GetHeight(), AV_PIX_FMT_RGB24);
	if (result) {
		grab->SetData(result);
		grab->SetSize(size);
		grab->FreeBuf();
	} else {
		grab->SetData(NULL);
		grab->SetSize(0);
	}

	return;
}

void cVideoRender::ConvertOsdBufToRgb(void)
{
	int size;
	cSoftHdGrab *grab = &m_grabOsd;
	cDrmBuffer *buf = grab->GetBuf();

	// early return if buf = NULL
	if (!buf) {
		grab->SetData(NULL);
		grab->SetSize(0);
		return;
	}

	for (int plane = 0; plane < buf->NumPlanes(); plane++) {
		LOGDEBUG2(L_GRAB, "VideoGrab: OSD plane %d address %p pitch %d offset %d handle %d size %d",
			   plane, buf->Plane(plane), buf->Pitch(plane), buf->Offset(plane), buf->Handle(plane), buf->Size(plane));
	}
	// result's width and height are original dimensions how buffer is presented on the screen
	uint8_t * result = buf2rgb(buf, &size, grab->GetWidth(), grab->GetHeight(), AV_PIX_FMT_BGRA);
	grab->SetData(result);
	grab->SetSize(size);
	grab->FreeBuf();

	return;
}

///
///	Clear grab buffers
///
///
void cVideoRender::ClearGrab(void)
{
	if (m_grabOsd.GetBuf())
		m_grabOsd.FreeBuf();
	if (m_grabVideo.GetBuf())
		m_grabVideo.FreeBuf();
}

///
///	Get the grabbed image
///
///	@param hw_render	video hardware render
///	@param[out] size	returns output size (memory)
///	@param[out] width	returns output width
///	@param[out] height	returns output height
///	@param[in] is_osd	is this an osd grab?
///
///	@returns the pointer to the image data
///
cSoftHdGrab *cVideoRender::GetGrab(int *size, int *width, int *height, int *x, int *y, int is_osd)
{
	cSoftHdGrab *grab;
	if (is_osd)
		grab = &m_grabOsd;
	else
		grab = &m_grabVideo;

	LOGDEBUG2(L_GRAB, "GetGrab: %s size %d %dx%d at %d|%d %p", is_osd ? "OSD" : "VIDEO", grab->GetSize(), grab->GetWidth(), grab->GetHeight(), grab->GetX(), grab->GetY(), grab->GetData());

	if (size)
		*size = grab->GetSize();
	if (width)
		*width = grab->GetWidth();
	if (height)
		*height = grab->GetHeight();
	if (x)
		*x = grab->GetX();
	if (y)
		*y = grab->GetY();

	return grab;
}

///
///	Get render statistics.
///
///	@param hw_render	video hardware render
///	@param[out] duped	duped frames
///	@param[out] dropped	dropped frames
///	@param[out] count	number of decoded frames
///
void cVideoRender::GetStats(int *duped, int *dropped, int *counter)
{
	*duped = m_framesDuped;
	*dropped = m_framesDropped;
	*counter = m_startCounter;
}

//----------------------------------------------------------------------------
//	Setup
//----------------------------------------------------------------------------


///
///	Get screen size.
///
///	@param[out] width	video stream width
///	@param[out] height	video stream height
///	@param[out] aspect_num	video stream aspect numerator
///	@param[out] aspect_den	video stream aspect denominator
///
void cVideoRender::GetScreenSize(int *width, int *height, double *pixel_aspect)
{
	m_pDrmDevice->GetScreenSize(width, height, pixel_aspect);
}

static size_t ReadLineFromFile(char *buf, size_t size, const char * file)
{
	FILE *fd = NULL;
	size_t character;

	fd = fopen(file, "r");
	if (fd == NULL) {
		LOGERROR("Can't open %s", file);
		return 0;
	}

	character = getline(&buf, &size, fd);

	fclose(fd);

	return character;
}

static int ReadHWPlatform(void)
{
	char *txt_buf;
	char *read_ptr;
	size_t bufsize = 128;
	size_t read_size;

	txt_buf = (char *) calloc(bufsize, sizeof(char));
	int hardwareQuirks = 0;

	read_size = ReadLineFromFile(txt_buf, bufsize, "/sys/firmware/devicetree/base/compatible");
	if (!read_size) {
		free((void *)txt_buf);
		return 0;
	}

	read_ptr = txt_buf;
	// be aware: device tree string can contain \x0 bytes, so every C-string function
	// thinks, we already reached the string's terminating null bytes
	// so copy the string into a temporary string without the "\0"
	char *_txt_buf = (char *) calloc(bufsize, sizeof(char));
	char *_read_ptr = _txt_buf;
	for (size_t i = 0; i < bufsize; i++) {
		if (memcmp(read_ptr, "\0", sizeof(char))) {
			memcpy(_read_ptr, read_ptr, sizeof(char));
			_read_ptr++;
		}
		read_ptr++;
	}

	read_ptr = txt_buf;
	LOGDEBUG2(L_DRM, "ReadHWPlatform: found \"%s\", set hardware quirks", _txt_buf);

	while(read_size) {
		if (strstr(read_ptr, "bcm2837")) {
			LOGDEBUG2(L_DRM, "ReadHWPlatform: bcm2837 (Raspberry Pi 2/3) found");
			hardwareQuirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
			break;
		}
		if (strstr(read_ptr, "bcm2711")) {
			LOGDEBUG2(L_DRM, "ReadHWPlatform: bcm2711 (Raspberry Pi 4 Model B, Compute Module 4, Pi 400) found");
			hardwareQuirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
			break;
		}
		if (strstr(read_ptr, "bcm2712")) {
			LOGDEBUG2(L_DRM, "ReadHWPlatform: bcm2712 (Raspberry Pi 5, Compute Module 5, Pi 500) found");
			hardwareQuirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
			break;
		}
		if (strstr(read_ptr, "amlogic")) {
			LOGDEBUG2(L_DRM, "ReadHWPlatform: amlogic found, disable HW deinterlacer");
			hardwareQuirks |= QUIRK_CODEC_NEEDS_EXT_INIT
					   |  QUIRK_CODEC_SKIP_FIRST_FRAMES
					   |  QUIRK_NO_HW_DEINT;
			break;
		}

		read_size -= (strlen(read_ptr) + 1);
		read_ptr = (char *)&read_ptr[(strlen(read_ptr) + 1)];
	}
	free((void *)txt_buf);

	return hardwareQuirks;
}

///
///	Initialize video output module.
///
void cVideoRender::Init(void)
{
	if (m_pDrmDevice->Init())
		LOGFATAL("VideoInit: FindDevice() failed");

	cDrmPlane *videoPlane = m_pDrmDevice->VideoPlane();
	cDrmPlane *osdPlane = m_pDrmDevice->OsdPlane();

	m_hardwareQuirks = ReadHWPlatform();

//	m_buffer[0]->Init(0, 0, DRM_FORMAT_NV12, m_pDrmDevice->Fd());
//	m_buffer[1]->Init(0, 0, DRM_FORMAT_NV12, m_pDrmDevice->Fd());

	// osd FB
#ifndef USE_GLES
	if (!m_pBufOsd)
		m_pBufOsd = new cDrmBuffer();
	
	if (m_pBufOsd->Setup(m_pDrmDevice->Fd(), m_pDrmDevice->DisplayWidth(), m_pDrmDevice->DisplayHeight(), DRM_FORMAT_ARGB8888, NULL)) {
		LOGFATAL("VideoOsdInit: SetupFB FB OSD failed!");
	}
#else
	if (m_disableOglOsd) {
		if (!m_pBufOsd)
			m_pBufOsd = new cDrmBuffer();

		if (m_pBufOsd->Setup(m_pDrmDevice->Fd(), m_pDrmDevice->DisplayWidth(), m_pDrmDevice->DisplayHeight(), DRM_FORMAT_ARGB8888, NULL)) {
			LOGFATAL("VideoOsdInit: SetupFB FB OSD failed!");
		}
	}
#endif

	// black fb
	LOGDEBUG2(L_DRM, "Videoinit: Try to create a black FB");
	if (m_bufBlack.Setup(m_pDrmDevice->Fd(), m_pDrmDevice->DisplayWidth(), m_pDrmDevice->DisplayHeight(), DRM_FORMAT_NV12, NULL))
		LOGFATAL("VideoInit: SetupFB black FB %i x %i failed", m_bufBlack.Width(), m_bufBlack.Height());
	m_bufBlack.FillBlack();

	// save actual modesetting
	m_pDrmDevice->SaveCrtc();

	drmModeAtomicReqPtr ModeReq;
	const uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	uint32_t modeID = 0;

	if (m_pDrmDevice->CreatePropertyBlob(&modeID) != 0)
		LOGFATAL("Failed to create mode property blob.");
	if (!(ModeReq = drmModeAtomicAlloc()))
		LOGFATAL("cannot allocate atomic request (%d): %m", errno);

	m_pDrmDevice->SetPropertyRequest(ModeReq, m_pDrmDevice->CrtcId(),
						DRM_MODE_OBJECT_CRTC, "MODE_ID", modeID);
	m_pDrmDevice->SetPropertyRequest(ModeReq, m_pDrmDevice->ConnectorId(),
						DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", m_pDrmDevice->CrtcId());
	m_pDrmDevice->SetPropertyRequest(ModeReq, m_pDrmDevice->CrtcId(),
						DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);

	// Osd plane
	// We don't have the m_pBufOsd for OpenGL yet, so we can't set anything. Set src and FbId later when osd was drawn,
	// but initially move the OSD behind the VIDEO
#ifndef USE_GLES
	osdPlane->SetParams(m_pDrmDevice->CrtcId(), m_pBufOsd->Id(),
		0, 0, m_pDrmDevice->DisplayWidth(), m_pDrmDevice->DisplayHeight(),
		0, 0, m_pBufOsd->Width(), m_pBufOsd->Height());

	osdPlane->SetPlane(ModeReq);
#else
	if (m_disableOglOsd) {
		osdPlane->SetParams(m_pDrmDevice->CrtcId(), m_pBufOsd->Id(),
			0, 0, m_pDrmDevice->DisplayWidth(), m_pDrmDevice->DisplayHeight(),
			0, 0, m_pBufOsd->Width(), m_pBufOsd->Height());

		osdPlane->SetPlane(ModeReq);
	}
#endif
	if (m_pDrmDevice->UseZpos()) {
		videoPlane->SetZpos(m_pDrmDevice->ZposOverlay());
		videoPlane->SetPlaneZpos(ModeReq);
#ifdef USE_GLES
		osdPlane->SetZpos(m_pDrmDevice->ZposPrimary());
		osdPlane->SetPlaneZpos(ModeReq);
#endif
	}

	// Black Buffer for video plane
	videoPlane->SetParams(m_pDrmDevice->CrtcId(), m_bufBlack.Id(),
		0, 0, m_pDrmDevice->DisplayWidth(), m_pDrmDevice->DisplayHeight(),
		0, 0, m_bufBlack.Width(), m_bufBlack.Height());

	videoPlane->SetPlane(ModeReq);

	if (drmModeAtomicCommit(m_pDrmDevice->Fd(), ModeReq, flags, NULL) != 0) {
#ifndef USE_GLES
		osdPlane->DumpParameters();
#endif
		videoPlane->DumpParameters();

		drmModeAtomicFree(ModeReq);
		LOGFATAL("VideoInit: cannot set atomic mode (%d): %m", errno);
	}

	drmModeAtomicFree(ModeReq);

	m_osdShown = 0;

	// init variables page flip
	m_pDrmDevice->InitEvent();

	// Wakeup DisplayHandlerThread
	WakeupDisplayThread();
}

///
///	Cleanup video output module.
///
void cVideoRender::Exit(void)
{
	cDrmPlane *videoPlane = m_pDrmDevice->VideoPlane();
	cDrmPlane *osdPlane = m_pDrmDevice->OsdPlane();

	ExitDecodingThread();
	ExitDisplayThread();

	// restore saved CRTC configuration
	m_pDrmDevice->RestoreCrtc();

	videoPlane->FreeProperties();
	osdPlane->FreeProperties();

	m_bufBlack.Destroy();
#ifdef USE_GLES
	if (m_disableOglOsd) {
		if (m_pBufOsd) {
			m_pBufOsd->Destroy();
			delete m_pBufOsd;
		}
	} else {
		if (m_pNextBo)
			gbm_bo_destroy(m_pNextBo);
		if (m_pOldBo)
			gbm_bo_destroy(m_pOldBo);
	}
#else
	if (m_pBufOsd) {
		m_pBufOsd->Destroy(m_pBufOsd);
		delete m_pBufOsd;
	}
#endif

	m_pDrmDevice->Close();
	delete m_pDrmDevice;
}

///
///	Set size and position of the video.
///
void cVideoRender::SetVideoOutputPosition(const cRect &rect)
{
	m_videoRect.Set(rect.Point(), rect.Size());

	if (m_videoRect.IsEmpty())
		m_videoIsScaled = 0;
	else
		m_videoIsScaled = 1;

	LOGDEBUG("SetVideoOutputPosition %d %d %d %d%s", rect.X(), rect.Y(), rect.Width(), rect.Height(), m_videoIsScaled ? ", video is scaled" : "");
}

void cVideoRender::DisableDeint(int disable)
{
	m_configDeintDisabled = disable;
}

int cVideoRender::DecodingThreadIsActive(void) {
	return m_pDecodingThread->Active();
};

void cVideoRender::StopFilter(void)
{
	m_pFilterThread->Stop();
}

void cVideoRender::WaitForFilterIdle(void)
{
	m_pFilterThread->WaitForIdle();
}
