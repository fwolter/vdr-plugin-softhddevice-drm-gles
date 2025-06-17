///
///	@file codec_video.c	@brief Video codec functions
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

///
///	@defgroup Codec The codec module.
///
///		This module contains all decoder and codec functions.
///		It is uses ffmpeg (http://ffmpeg.org) as backend.
///

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <cassert>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#include "misc.h"
#include "buf2rgb.h"
}

#include "codec_video.h"
#include "video.h"
#include "softhddev.h"

#include "logger.h"

#define NUM_CAPTURE_BUFFERS 10
#define NUM_OUTPUT_BUFFERS 10

#define AV_LOGLEVEL AV_LOG_TRACE

/******************************************************************************
**	static functions
******************************************************************************/

/**
**	log callbacks
*/
#ifdef FFMPEG_DEBUG
static void CodecLogCallback( __attribute__ ((unused)) void *ptr,
				      __attribute__ ((unused)) int level,
				      __attribute__ ((unused)) const char *fmt,
				      va_list vl)
{
	av_log_set_level(AV_LOG_INFO);

	if (level > AV_LOGLEVEL)
		return;

	char format[256];
	char prefix[20] = "";
	pid_t threadId = syscall(__NR_gettid);

	strcpy(prefix, "[FFMpeg]");
	snprintf(format, sizeof(format), "[%d] [softhddevice]%s %s", threadId, prefix, fmt);

	vsyslog(LOG_INFO, format, vl);
}
#else
static void CodecLogCallback( __attribute__ ((unused)) void *ptr,
				      __attribute__ ((unused)) int level,
				      __attribute__ ((unused)) const char *fmt,
				      __attribute__ ((unused)) va_list vl)
{
}
#endif

/**
**	Callback to negotiate the PixelFormat.
**
**	@param video_ctx	codec context
**	@param fmt		is the list of formats which are supported by
**				the codec, it is terminated by -1 as 0 is a
**				valid format, the formats are ordered by quality.
*/
static enum AVPixelFormat GetFormat(AVCodecContext * video_ctx,
		const enum AVPixelFormat *fmt)
{
	while (*fmt != AV_PIX_FMT_NONE) {
		LOGDEBUG2(L_CODEC, "GetFormat: PixelFormat: %s video_ctx->pix_fmt: %s sw_pix_fmt: %s Codecname: %s",
			av_get_pix_fmt_name(*fmt), av_get_pix_fmt_name(video_ctx->pix_fmt),
			av_get_pix_fmt_name(video_ctx->sw_pix_fmt), video_ctx->codec->name);
		if (*fmt == AV_PIX_FMT_DRM_PRIME) {
			return AV_PIX_FMT_DRM_PRIME;
		}

		if (*fmt == AV_PIX_FMT_YUV420P) {
			return AV_PIX_FMT_YUV420P;
		}
		fmt++;
	}
	LOGWARNING("GetFormat: No pixel format found! Set default format.");

	return avcodec_default_get_format(video_ctx, fmt);
}

/******************************************************************************
**	cVideoDecoder class
******************************************************************************/

/**
**	VideoDecoder constructor
**
**	@param render	pointer to VideoRender
*/
cVideoDecoder::cVideoDecoder(cVideoRender *render, cVideoStream *stream)
{
    VideoCtx = nullptr;
    Render = render;
    Stream = stream;
    pthread_mutex_init(&CodecLockMutex, NULL);
    av_log_set_callback(CodecLogCallback);

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,18,100)
    avcodec_register_all();		// register all formats and codecs
#endif
}

/**
**	VideoDecoder destructor
*/
cVideoDecoder::~cVideoDecoder(void)
{
    pthread_mutex_destroy(&CodecLockMutex);
}

/**
**	Open video decoder
**
**	@param codec		codec for which we should find a hw config
**
**	@return			AVCodecHWConfig if found, NULL otherwise
*/
const AVCodecHWConfig *cVideoDecoder::FindHWConfig(const AVCodec *codec)
{
	const AVCodecHWConfig *config = NULL;
	for (int n = 0; (config = avcodec_get_hw_config(codec, n)); n++)
	{
		if (!(config->pix_fmt == AV_PIX_FMT_DRM_PRIME))
			continue;

		if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) ||
		    (config->methods & AV_CODEC_HW_CONFIG_METHOD_INTERNAL))
			return config;
	}

	LOGDEBUG2(L_CODEC, "cVideoDecoder::Open: no HW config found for %s", codec->long_name ? codec->long_name : codec->name);
	return NULL;
}

/**
**	Find a suitable video codec
**
**	@param codec_id		video codec id
**	@param force_software	force software decoding
**
**	@return			AVCodec if found, NULL otherwise
*/

const AVCodec *cVideoDecoder::FindDecoder(enum AVCodecID codec_id, int force_software)
{
	const AVCodec *codec;
	void *i = 0;

	if (!force_software) {
		while ((codec = av_codec_iterate(&i))) {
			if (!av_codec_is_decoder(codec))
				continue;
			if (codec->id != codec_id)
				continue;

			const AVCodecHWConfig *config = FindHWConfig(codec);
			if (config)
				return codec;
		}
	}

	codec = avcodec_find_decoder(codec_id);
	if (codec)
		return codec;

	LOGWARNING("cVideoDecoder::Open: no decoder found");
	return NULL;
}


/**
**	Open video decoder
**
**	@param codec_id		video codec id
**	@param Par		codec parameters
**	@param timebase		timebase
**	@param force_software	force software decoding
**
**	@returns 0		success
**	@returns -1		open decoder failed
*/
int cVideoDecoder::Open(enum AVCodecID codec_id, AVCodecParameters * Par,
			AVRational * timebase, int force_software, int width, int height)
{
	int swcodec = force_software;

	if ((Render->VideoCodecMode() & CODEC_DISABLE_MPEG_HW &&
	     codec_id == AV_CODEC_ID_MPEG2VIDEO))
		swcodec = 1;
	if ((Render->VideoCodecMode() & CODEC_DISABLE_H264_HW &&
	     codec_id == AV_CODEC_ID_H264))
		swcodec = 1;

	LOGDEBUG2(L_CODEC, "cVideoDecoder::Open: Try to open decoder for CodecID %s%s", avcodec_get_name(codec_id), swcodec ? " (sw decoding forced)" : "");

	const AVCodec *codec = FindDecoder(codec_id, swcodec);
	if (!codec) {
		LOGERROR("cVideoDecoder::Open: Could not find any decoder for codec %s!", avcodec_get_name(codec_id));
		return -1;
	}

	LOGDEBUG2(L_CODEC, "cVideoDecoder::Open: Codec %s for CodecID %s found%s",
	       codec->long_name ? codec->long_name : codec->name, avcodec_get_name(codec_id), swcodec ? " (sw decoding forced)" : "");

	VideoCtx = avcodec_alloc_context3(codec);
	if (!VideoCtx) {
		LOGERROR("cVideoDecoder::Open: can't alloc codec context!");
		return -1;
	}

	const AVCodecHWConfig *config = !swcodec ? FindHWConfig(codec) : NULL;
	static AVBufferRef *hw_device_ctx = NULL;

	if (config && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
		const char *type_name = av_hwdevice_get_type_name(config->device_type);
		if (av_hwdevice_ctx_create(&hw_device_ctx, config->device_type, NULL, NULL, 0) < 0) {
			avcodec_free_context(&VideoCtx);
			LOGERROR("cVideoDecoder::Open: Error creating HW context %s",
			      type_name ? type_name : "unknown");
			return -1;
		}
		LOGDEBUG2(L_CODEC, "cVideoDecoder::Open: Using %s HW codec",
		       type_name ? type_name : "unknown");
		VideoCtx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
		VideoCtx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
	}

	if (Par) {
		if ((avcodec_parameters_to_context(VideoCtx, Par)) < 0)
			LOGERROR("cVideoDecoder::Open: insert parameters to context failed!");
	}

	VideoCtx->codec_id = codec_id;
	VideoCtx->get_format = GetFormat;
	VideoCtx->opaque = this;
	VideoCtx->pkt_timebase.num = 1;
	VideoCtx->pkt_timebase.den = 90000;

	if (timebase) {
		VideoCtx->pkt_timebase.num = timebase->num;
		VideoCtx->pkt_timebase.den = timebase->den;
	}

	// amlogic h264 decoder needs this
	if (codec_id == AV_CODEC_ID_H264) {
		if (Par) {
			VideoCtx->coded_width = Par->width;
			VideoCtx->coded_height = Par->height;
			VideoCtx->width = Par->width;
			VideoCtx->height = Par->height;
			LOGDEBUG2(L_CODEC, "cVideoDecoder::Open: Set width %d and height %d from Par", Par->width, Par->height);
		} else if (width && height) {
			VideoCtx->coded_width = width;
			VideoCtx->coded_height = height;
			VideoCtx->width = width;
			VideoCtx->height = height;
			LOGDEBUG2(L_CODEC, "cVideoDecoder::Open: Set width %d and height %d forced", width, height);
		}
	}

	if (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS ||
		AV_CODEC_CAP_SLICE_THREADS) {
		VideoCtx->thread_count = swcodec ? 4 : 1;
	}
	if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS){
		VideoCtx->thread_type = FF_THREAD_SLICE;
	}

/*
	if (strstr(codec->name, "_v4l2")) {
		if (av_opt_set_int(VideoCtx->priv_data, "num_capture_buffers", NUM_CAPTURE_BUFFERS, 0) < 0) {
			LOGERROR("cVideoDecoder::Open: can't set %d num_capture_buffers", NUM_CAPTURE_BUFFERS);
		}
		LOGDEBUG2(L_CODEC, "cVideoDecoder::Open: set num_capture_buffers %d", NUM_CAPTURE_BUFFERS);
		if (av_opt_set_int(VideoCtx->priv_data, "num_output_buffers", NUM_OUTPUT_BUFFERS, 0) < 0) {
			LOGERROR("cVideoDecoder::Open: can't set %d num_output_buffers", NUM_OUTPUT_BUFFERS);
		}
		LOGDEBUG2(L_CODEC, "cVideoDecoder::Open: set num_output_buffers %d", NUM_OUTPUT_BUFFERS);
	}
*/
	int err = avcodec_open2(VideoCtx, VideoCtx->codec, NULL);
	if (err < 0) {
		avcodec_free_context(&VideoCtx);
		if (force_software) {
			LOGERROR("cVideoDecoder::Open: Error opening the decoder: %s",
				av_err2str(err));
			return -1;
		}
		LOGDEBUG2(L_CODEC, "cVideoDecoder::Open: Could not open %s decoder, try opening software decoder",
		       codec->long_name ? codec->long_name : codec->name);

		return Open(codec_id, Par, timebase, 1, 0, 0);
	}

	LOGDEBUG2(L_CODEC, "cVideoDecoder::Open: Codec %s for CodecID %s opened%s, using %d threads",
	       codec->long_name ? codec->long_name : codec->name,
	       avcodec_get_name(codec_id),
	       swcodec ? " (sw decoding forced)" : "",
	       VideoCtx->thread_count);

	sent = received = 0;
	FirstKeyFrame = 1;
	return 0;
}

/**
**	Close video decoder
*/
void cVideoDecoder::Close(void)
{
	pthread_mutex_lock(&CodecLockMutex);
	if (VideoCtx) {
		LOGDEBUG2(L_CODEC, "cVideoDecoder::Close: VideoCtx %p", VideoCtx);
		last_coded_width = VideoCtx->coded_width;
		last_coded_height = VideoCtx->coded_height;
		avcodec_free_context(&VideoCtx);
		VideoCtx = nullptr;
	}
	pthread_mutex_unlock(&CodecLockMutex);
	sent = received = 0;
}

/**
**	Get extradata from avpkt
**
**	@param avpkt	video packet
**
**	@returns 0 extradata set
**	@returns -1 something went wrong
*/
int cVideoDecoder::GetExtraData(const AVPacket * avpkt)
{
	AVBSFContext *bsf_ctx;
	const AVBitStreamFilter *f;
	size_t extradata_size;
	uint8_t *extradata;
	int ret = 0;

	f = av_bsf_get_by_name("extract_extradata");
	if (!f) {
		LOGERROR("cVideoDecoder::SendPacket: extradata av_bsf_get_by_name failed!");
		return -1;
	}

	ret = av_bsf_alloc(f, &bsf_ctx);
	if (ret < 0) {
		LOGERROR("cVideoDecoder::SendPacket: extradata av_bsf_alloc failed!");
		return ret;
	}

	bsf_ctx->par_in->codec_id = VideoCtx->codec_id;

	ret = av_bsf_init(bsf_ctx);
	if (ret < 0) {
		LOGERROR("cVideoDecoder::SendPacket: extradata av_bsf_init failed!");
		av_bsf_free(&bsf_ctx);
		return ret;
	}

	AVPacket *dstPkt = av_packet_alloc();
	AVPacket *pktRef = dstPkt;

	if (!dstPkt) {
		LOGERROR("cVideoDecoder::SendPacket: extradata av_packet_alloc failed!");
		av_bsf_free(&bsf_ctx);
		return -1;
	}

	ret = av_packet_ref(pktRef, avpkt);
	if (ret < 0) {
		LOGERROR("cVideoDecoder::SendPacket: extradata av_packet_ref failed!");
		av_packet_free(&dstPkt);
		av_bsf_free(&bsf_ctx);
		return ret;
	}

	ret = av_bsf_send_packet(bsf_ctx, pktRef);
	if (ret < 0) {
		LOGERROR("cVideoDecoder::SendPacket: extradata av_bsf_send_packet failed!");
		av_packet_unref(pktRef);
		av_packet_free(&dstPkt);
		av_bsf_free(&bsf_ctx);
		return ret;
	}

	ret = av_bsf_receive_packet(bsf_ctx, pktRef);
	if (ret < 0) {
		LOGERROR("cVideoDecoder::SendPacket: extradata av_bsf_send_packet failed!");
		av_packet_unref(pktRef);
		av_packet_free(&dstPkt);
		av_bsf_free(&bsf_ctx);
		return ret;
	}

	extradata = av_packet_get_side_data(pktRef, AV_PKT_DATA_NEW_EXTRADATA,
		&extradata_size);

	VideoCtx->extradata = (uint8_t *)av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
	memcpy(VideoCtx->extradata, extradata, extradata_size);
	VideoCtx->extradata_size = extradata_size;

	av_packet_unref(pktRef);
	av_packet_free(&dstPkt);
	av_bsf_free(&bsf_ctx);
	return ret;
}

/**
**	Decode a video packet.
**
**	@param avpkt	video packet
**
**	@returns 0			packet was sent
**	@returns AVERROR(EAGAIN)	packet not accepted, first receive frame and send packet again
**	@returns AVERROR(EINVAL)	invalid input or missing VideoCtx
**	@returns ret			return other ffmpeg error
*/
int cVideoDecoder::SendPacket(const AVPacket * avpkt)
{
	int ret = 0;

	if (VideoCtx == nullptr)
		return AVERROR(EINVAL);

	// force a flush, ich avpkt is NULL
	if (!avpkt) {
		LOGDEBUG2(L_CODEC, "cVideoDecoder::SendPacket: send NULL packet, flush reqeusted");
		avcodec_send_packet(VideoCtx, NULL);
		return 0;
	}

	if (!avpkt->size) {
		return AVERROR(EINVAL);
	}

	// get extradata, if not yet done
	if (!VideoCtx->extradata_size) {
		if (!GetExtraData(avpkt))
			LOGDEBUG2(L_CODEC, "cVideoDecoder::SendPacket: set extradata %p %d", VideoCtx->extradata, VideoCtx->extradata_size);
	}

	pthread_mutex_lock(&CodecLockMutex);
	ret = avcodec_send_packet(VideoCtx, avpkt);
	pthread_mutex_unlock(&CodecLockMutex);
	if (ret) {
		if (ret != AVERROR(EAGAIN))
			LOGDEBUG2(L_CODEC, "cVideoDecoder::SendPacket: send_packet ret: %s",
				av_err2str(ret));
		return ret;
	}

	sent++;
	LOGDEBUG2(L_PACKET, "cVideoDecoder::SendPacket:   %6d PTS %s <<---", sent, Timestamp2String(avpkt->pts / 90));
	return 0;
}

/**
**	Get a decoded a video frame.
**
**	@param no_deint		set interlaced_frame to 0
**
**	@returns 0	received frame
**	@returns AVERROR(EAGAIN)	get no frame, send avpkt again
**	@returns AVERROR_EOF		EOF, needs flushing
**	@returns AVERROR(EINVAL)	get no frame, something went wrong
**	@returns ret			return other ffmpeg error
*/
int cVideoDecoder::ReceiveFrame(int no_deint, AVFrame **frame)
{
	int ret;
	AVFrame *pFrame;

	if (VideoCtx == nullptr)
		return AVERROR(EINVAL);

	if (!(pFrame = av_frame_alloc()))
		LOGFATAL("cVideoDecoder::ReceiveFrame: can't allocate decoder frame");

	pthread_mutex_lock(&CodecLockMutex);
	ret = avcodec_receive_frame(VideoCtx, pFrame);
	pthread_mutex_unlock(&CodecLockMutex);

	if (ret) {
		if (ret == AVERROR_EOF)
			LOGDEBUG2(L_CODEC, "cVideoDecoder::ReceiveFrame: receive_frame ret: AVERROR_EOF");
		if (ret != AVERROR(EAGAIN))
			LOGDEBUG2(L_CODEC, "cVideoDecoder::ReceiveFrame: receive_frame ret: %s", av_err2str(ret));
		av_frame_free(&pFrame);
		return ret;
	}

	if (pFrame->flags == AV_FRAME_FLAG_CORRUPT)
		LOGDEBUG2(L_CODEC, "cVideoDecoder::ReceiveFrame: AV_FRAME_FLAG_CORRUPT");

	if (no_deint) {
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
		pFrame->interlaced_frame = 0;
#else
		pFrame->flags &= ~AV_FRAME_FLAG_INTERLACED;
#endif
		LOGDEBUG2(L_CODEC, "cVideoDecoder::ReceiveFrame: interlaced_frame = 0");
	}

	// codec artifacts workaround for amlogic H264, skip some key frames
	if (VideoCtx->codec_id == AV_CODEC_ID_H264 &&
	   (Render->HardwareQuirks & QUIRK_CODEC_SKIP_FIRST_FRAMES) && FirstKeyFrame) {
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
		if (pFrame->key_frame) {
			LOGDEBUG2(L_CODEC, "cVideoDecoder::ReceiveFrame: artifact workaround - skip %s I-frame nr %d",
			       pFrame->interlaced_frame ? "interlaced" : "progressive", FirstKeyFrame);
#else
		if (pFrame->flags & AV_FRAME_FLAG_KEY) {
			LOGDEBUG2(L_CODEC, "cVideoDecoder::ReceiveFrame: artifact workaround - skip %s I-frame nr %d",
			       pFrame->flags & AV_FRAME_FLAG_INTERLACED ? "interlaced" : "progressive", FirstKeyFrame);
#endif
			if (FirstKeyFrame++ > QUIRK_CODEC_SKIP_NUM_FRAMES - 1)
				FirstKeyFrame = 0;
		}

		av_frame_free(&pFrame);
		return AVERROR(EAGAIN);
	}

	*frame = pFrame;

	received++;
	LOGDEBUG2(L_PACKET, "cVideoDecoder::ReceiveFrame: %6d PTS %s --->> (%2d)", received, Timestamp2String(pFrame->pts / 90), sent - received);
	return 0;
}

/**
**	Reopen the video decoder.
**
**	Temporary implemented as close and open
**
**	@param codec_id		video codec id
**	@param Par		codec parameters
**	@param timebase		timebase
**	@param force_software	force software decoding
**
**	@returns 0		success
**	@returns -1		reopen decoder failed
**
**	TODO: only flush
*/
int cVideoDecoder::ReopenCodec(enum AVCodecID codec_id, AVCodecParameters * Par,
		AVRational * timebase, int force_software)
{
	LOGDEBUG2(L_CODEC, "cVideoDecoder::ReopenCodec: VideoCtx %p", VideoCtx);
	if (VideoCtx) {
		Close();
		if (Open(codec_id, Par, timebase, force_software, last_coded_width, last_coded_height))
			return -1;
	}
	FirstKeyFrame = 0; // unused, because we have no hardware which needs both quirks, but set here for safety reasons
	sent = received = 0;

	return 0;
}

/**
**	Flush the video decoder.
*/
void cVideoDecoder::FlushBuffers(void)
{
	LOGDEBUG2(L_CODEC, "cVideoDecoder::FlushBuffers: VideoCtx %p", VideoCtx);
	pthread_mutex_lock(&CodecLockMutex);
	if (VideoCtx) {
		avcodec_flush_buffers(VideoCtx);
	}
	pthread_mutex_unlock(&CodecLockMutex);
	sent = received = 0;
}

/**
**	Get video size.
**
**	@param[out] width		video stream width
**	@param[out] height		video stream height
**	@param[out] aspect_ratio	video stream aspect ratio
*/
void cVideoDecoder::GetVideoSize(int *width, int *height, double *aspect_ratio)
{
	*width = 0;
	*height = 0;
	*aspect_ratio = 1.0f;

	if (VideoCtx == nullptr)
		return;

	*width = VideoCtx->coded_width;
	*height = VideoCtx->coded_height;
	if (VideoCtx->coded_height > 0)
		*aspect_ratio = (double)(VideoCtx->coded_width) / (double)(VideoCtx->coded_height);
}
