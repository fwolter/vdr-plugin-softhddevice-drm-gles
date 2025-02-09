///
///	@file codec.c	@brief Codec functions
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

    /// compile with pass-through support (stable, AC-3, E-AC-3 only)
#define USE_PASSTHROUGH

#include <unistd.h>
#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut
#include <pthread.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

#ifdef MAIN_H
#include MAIN_H
#endif
#include "iatomic.h"
#include "misc.h"
#include "video.h"
#include "audio.h"
#include "codec.h"
#include "softhddev.h"

#define NUM_CAPTURE_BUFFERS 10
#define NUM_OUTPUT_BUFFERS 10

//----------------------------------------------------------------------------
//	Global
//----------------------------------------------------------------------------

static pthread_mutex_t CodecLockMutex;

//----------------------------------------------------------------------------
//	Call-backs
//----------------------------------------------------------------------------

/**
**	Callback to negotiate the PixelFormat.
**
**	@param video_ctx	codec context
**	@param fmt		is the list of formats which are supported by
**				the codec, it is terminated by -1 as 0 is a
**				valid format, the formats are ordered by quality.
*/
static enum AVPixelFormat Codec_get_format(AVCodecContext * video_ctx,
		const enum AVPixelFormat *fmt)
{
	VideoDecoder *decoder;
	decoder = video_ctx->opaque;

	return Video_get_format(decoder->Render, video_ctx, fmt);
}

AVCodecContext *Codec_get_VideoContext(VideoDecoder * decoder)
{
	return decoder->VideoCtx;
}

//----------------------------------------------------------------------------
//	Test
//----------------------------------------------------------------------------

/**
**	Allocate a new video decoder context.
**
**	@param hw_decoder	video hardware decoder
**
**	@returns private decoder pointer for video decoder.
*/
VideoDecoder *CodecVideoNewDecoder(VideoRender * render)
{
    VideoDecoder *decoder;

    if (!(decoder = calloc(1, sizeof(*decoder)))) {
		Fatal("codec: can't allocate vodeo decoder");
    }
    decoder->Render = render;

    return decoder;
}

/**
**	Deallocate a video decoder context.
**
**	@param decoder	private video decoder
*/
void CodecVideoDelDecoder(VideoDecoder * decoder)
{
    free(decoder);
}

static const AVCodecHWConfig *FindHWConfig(const AVCodec *codec)
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

	Debug2(L_CODEC, "CodecVideoOpen: no HW config found for %s", codec->long_name ? codec->long_name : codec->name);
	return NULL;
}

static const AVCodec* FindDecoder(enum AVCodecID codec_id, int force_software)
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

	Warning("CodecVideoOpen: no decoder found");
	return NULL;
}

/**
**	Open video decoder.
**
**	@param decoder		private video decoder
**	@param codec_id		video codec id
**	@param Par		codec parameters
**	@param timebase		timebase
**	@param force_software	force software decoding
**
**	@returns 0		success
**	@returns -1		open decoder failed
*/
int CodecVideoOpen(VideoDecoder * decoder, int codec_id, AVCodecParameters * Par,
		AVRational * timebase, int force_software, int width, int height)
{
	int swcodec = force_software;

	if ((VideoCodecMode(decoder->Render) & CODEC_DISABLE_MPEG_HW &&
	     codec_id == AV_CODEC_ID_MPEG2VIDEO))
		swcodec = 1;
	if ((VideoCodecMode(decoder->Render) & CODEC_DISABLE_H264_HW &&
	     codec_id == AV_CODEC_ID_H264))
		swcodec = 1;

	Debug2(L_CODEC, "CodecVideoOpen: Try to open decoder for CodecID %s%s", avcodec_get_name(codec_id), swcodec ? " (sw decoding forced)" : "");

	const AVCodec *codec = FindDecoder(codec_id, swcodec);
	if (!codec) {
		Error("CodecVideoOpen: Could not find any decoder for codec %s!", avcodec_get_name(codec_id));
		return -1;
	}

	Debug2(L_CODEC, "CodecVideoOpen: Codec %s for CodecID %s found%s",
	       codec->long_name ? codec->long_name : codec->name, avcodec_get_name(codec_id), swcodec ? " (sw decoding forced)" : "");

	decoder->VideoCtx = avcodec_alloc_context3(codec);
	if (!decoder->VideoCtx) {
		Error("CodecVideoOpen: can't alloc codec context!");
		return -1;
	}

	const AVCodecHWConfig *config = !swcodec ? FindHWConfig(codec) : NULL;
	static AVBufferRef *hw_device_ctx = NULL;

	if (config && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
		const char *type_name = av_hwdevice_get_type_name(config->device_type);
		if (av_hwdevice_ctx_create(&hw_device_ctx, config->device_type, NULL, NULL, 0) < 0) {
			avcodec_free_context(&decoder->VideoCtx);
			Error("CodecVideoOpen: Error creating HW context %s",
			      type_name ? type_name : "unknown");
			return -1;
		}
		Debug2(L_CODEC, "CodecVideoOpen: Using %s HW codec",
		       type_name ? type_name : "unknown");
		decoder->VideoCtx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
	}

	decoder->VideoCtx->codec_id = codec_id;
	decoder->VideoCtx->get_format = Codec_get_format;
	decoder->VideoCtx->opaque = decoder;
	decoder->VideoCtx->workaround_bugs = FF_BUG_AUTODETECT;

	// amlogic h264 decoder needs this
	if (codec_id == AV_CODEC_ID_H264) {
		if (Par) {
			decoder->VideoCtx->coded_width = Par->width;
			decoder->VideoCtx->coded_height = Par->height;
			decoder->VideoCtx->width = Par->width;
			decoder->VideoCtx->height = Par->height;
			Debug2(L_CODEC, "CodecVideoOpen: Set width %d and height %d from Par", Par->width, Par->height);
		} else if (width && height) {
			decoder->VideoCtx->coded_width = width;
			decoder->VideoCtx->coded_height = height;
			decoder->VideoCtx->width = width;
			decoder->VideoCtx->height = height;
			Debug2(L_CODEC, "CodecVideoOpen: Set width %d and height %d forced", width, height);
		} else if (decoder->Render->CodecNeedsExtInit) {
			int pWidth;
			int pHeight;
			ParseResolutionH264(&pWidth, &pHeight);
			Debug2(L_CODEC, "CodecVideoOpen: Parsed width %d height %d", pWidth, pHeight);
			decoder->VideoCtx->coded_width = pWidth;
			decoder->VideoCtx->coded_height = pHeight;
			decoder->VideoCtx->width = pWidth;
			decoder->VideoCtx->height = pHeight;
		}
	}

	if (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS ||
		AV_CODEC_CAP_SLICE_THREADS) {
		decoder->VideoCtx->thread_count = swcodec ? 4 : 1;
	}
	if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS){
		decoder->VideoCtx->thread_type = FF_THREAD_SLICE;
	}

	if (Par) {
		if ((avcodec_parameters_to_context(decoder->VideoCtx, Par)) < 0)
			Error("CodecVideoOpen: insert parameters to context failed!");
	}
	if (timebase) {
		decoder->VideoCtx->pkt_timebase.num = timebase->num;
		decoder->VideoCtx->pkt_timebase.den = timebase->den;
	} else {
		decoder->VideoCtx->pkt_timebase.num = 1;
		decoder->VideoCtx->pkt_timebase.den = 90000;
	}
/*
	if (strstr(codec->name, "_v4l2")) {
		if (av_opt_set_int(decoder->VideoCtx->priv_data, "num_capture_buffers", NUM_CAPTURE_BUFFERS, 0) < 0) {
			Error("CodecVideoOpen: can't set %d num_capture_buffers", NUM_CAPTURE_BUFFERS);
		}
		Debug2(L_CODEC, "CodecVideoOpen: set num_capture_buffers %d", NUM_CAPTURE_BUFFERS);
		if (av_opt_set_int(decoder->VideoCtx->priv_data, "num_output_buffers", NUM_OUTPUT_BUFFERS, 0) < 0) {
			Error("CodecVideoOpen: can't set %d num_output_buffers", NUM_OUTPUT_BUFFERS);
		}
		Debug2(L_CODEC, "CodecVideoOpen: set num_output_buffers %d", NUM_OUTPUT_BUFFERS);
	}
*/
	int err = avcodec_open2(decoder->VideoCtx, decoder->VideoCtx->codec, NULL);
	if (err < 0) {
		avcodec_free_context(&decoder->VideoCtx);
		if (force_software) {
			Error("CodecVideoOpen: Error opening the decoder: %s",
				av_err2str(err));
			return -1;
		}
		Debug2(L_CODEC, "CodecVideoOpen: Could not open %s decoder, try opening software decoder",
		       codec->long_name ? codec->long_name : codec->name);

		return CodecVideoOpen(decoder, codec_id, Par, timebase, 1, 0, 0);
	}

	Debug2(L_CODEC, "CodecVideoOpen: Codec %s for CodecID %s opened%s, using %d threads",
	       codec->long_name ? codec->long_name : codec->name,
	       avcodec_get_name(codec_id),
	       swcodec ? " (sw decoding forced)" : "",
	       decoder->VideoCtx->thread_count);

	decoder->sent = decoder->received = 0;
	return 0;
}

/**
**	Close video decoder.
**
**	@param decoder	private video decoder
*/
void CodecVideoClose(VideoDecoder * decoder)
{
	Debug2(L_CODEC, "CodecVideoClose: VideoCtx %p", decoder->VideoCtx);
	pthread_mutex_lock(&CodecLockMutex);
	if (decoder->VideoCtx) {
		decoder->last_coded_width = decoder->VideoCtx->coded_width;
		decoder->last_coded_height = decoder->VideoCtx->coded_height;
		avcodec_free_context(&decoder->VideoCtx);
	}
	pthread_mutex_unlock(&CodecLockMutex);
	decoder->sent = decoder->received = 0;
}

/**
**	Decode a video packet.
**
**	@param decoder	video decoder data
**	@param avpkt	video packet
**
**	@returns 1 packet not accepted, first receive frame and send packet again
**	@returns 0 packet was sent
**	@returns -1 something went wrong
*/
int CodecVideoSendPacket(VideoDecoder * decoder, const AVPacket * avpkt)
{
	int ret = 0;

	// force a flush, ich avpkt is NULL
	if (!avpkt) {
		if (decoder->VideoCtx) {
			Debug2(L_CODEC, "CodecVideoSendPacket: send NULL packet, flush reqeusted");
			avcodec_send_packet(decoder->VideoCtx, NULL);
		}
		return -1;
	}

	if (!avpkt->size) {
		return -1;
	}

	pthread_mutex_lock(&CodecLockMutex);
	if (decoder->VideoCtx) {
		ret = avcodec_send_packet(decoder->VideoCtx, avpkt);
	}
	pthread_mutex_unlock(&CodecLockMutex);
	if (ret == AVERROR(EAGAIN))
		return 1;
	else if (ret) {
		Debug2(L_CODEC, "CodecVideoSendPacket: send_packet ret: %s",
			av_err2str(ret));
		return -1;
	}

	decoder->sent++;
	Debug2(L_PACKET, "CodecVideoSendPacket:   %6d PTS %s <<---", decoder->sent, Timestamp2String(avpkt->pts / 90));
	return 0;
}

/**
**	Get a decoded a video frame.
**
**	@param decoder		video decoder data
**	@param no_deint		set interlaced_frame to 0
**
**	@returns 1	get no frame, send avpkt again
**	@returns 0	received frame
**	@returns -1	get no frame, something went wrong
**	@returns -2	EOF, needs flushing
*/
int CodecVideoReceiveFrame(VideoDecoder * decoder, int no_deint, AVFrame **frame)
{
	int ret;
	AVFrame *pFrame;

	if (!(pFrame = av_frame_alloc())) {
		Fatal("CodecVideoReceiveFrame: can't allocate decoder frame");
	}

	pthread_mutex_lock(&CodecLockMutex);
	if (decoder->VideoCtx) {
		ret = avcodec_receive_frame(decoder->VideoCtx, pFrame);
	} else {
		av_frame_free(&pFrame);
		pthread_mutex_unlock(&CodecLockMutex);
		return -1;
	}
	pthread_mutex_unlock(&CodecLockMutex);

	if (ret == AVERROR(EAGAIN)) {
		av_frame_free(&pFrame);
//		Debug2(L_CODEC, "CodecVideoReceiveFrame: receive_frame ret: AVERROR(EAGAIN)");
		return 1;
	} else if (ret) {
		av_frame_free(&pFrame);
		if (ret == AVERROR_EOF) {
			Debug2(L_CODEC, "CodecVideoReceiveFrame: receive_frame ret: AVERROR_EOF");
			return -2;
		}
		Debug2(L_CODEC, "CodecVideoReceiveFrame: receive_frame ret: %s", av_err2str(ret));
		return -1;
	}

	if (pFrame->flags == AV_FRAME_FLAG_CORRUPT)
		Debug2(L_CODEC, "CodecVideoReceiveFrame: AV_FRAME_FLAG_CORRUPT");

	if (no_deint) {
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
		pFrame->interlaced_frame = 0;
#else
		pFrame->flags &= ~AV_FRAME_FLAG_INTERLACED;
#endif
		Debug2(L_CODEC, "CodecVideoReceiveFrame: interlaced_frame = 0");
	}

	*frame = pFrame;

	decoder->received++;
	Debug2(L_PACKET, "CodecVideoReceiveFrame: %6d PTS %s --->> (%2d)", decoder->received, Timestamp2String(pFrame->pts / 90), decoder->sent - decoder->received);
	return 0;
}

/**
**	Reopen the video decoder.
**
**	Temporary implemented as close and open
**
**	@param decoder		private video decoder
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
int CodecVideoReopenCodec(VideoDecoder * decoder, int codec_id, AVCodecParameters * Par,
		AVRational * timebase, int force_software)
{
	Debug2(L_CODEC, "CodecVideoReopenCodec: VideoCtx %p", decoder->VideoCtx);
	if (decoder->VideoCtx) {
		CodecVideoClose(decoder);
		if (CodecVideoOpen(decoder, codec_id, Par, timebase, force_software, decoder->last_coded_width, decoder->last_coded_height))
			return -1;
	}
	decoder->sent = decoder->received = 0;

	return 0;
}

/**
**	Flush the video decoder.
**
**	@param decoder	video decoder data
*/
void CodecVideoFlushBuffers(VideoDecoder * decoder)
{
	Debug2(L_CODEC, "CodecVideoFlushBuffers: VideoCtx %p", decoder->VideoCtx);
	pthread_mutex_lock(&CodecLockMutex);
	if (decoder->VideoCtx) {
		avcodec_flush_buffers(decoder->VideoCtx);
	}
	pthread_mutex_unlock(&CodecLockMutex);
	decoder->sent = decoder->received = 0;
}

//----------------------------------------------------------------------------
//	Audio
//----------------------------------------------------------------------------

///
///	Audio decoder structure.
///
struct _audio_decoder_
{
    AVCodecContext *AudioCtx;		///< audio codec context

    AVFrame *Frame;			///< decoded audio frame buffer
    int64_t last_pts;			///< last PTS
};

///
///	IEC Data type enumeration.
///
enum IEC61937
{
    IEC61937_AC3 = 0x01,		///< AC-3 data
    // FIXME: more data types
    IEC61937_EAC3 = 0x15,		///< E-AC-3 data
};

#ifdef USE_PASSTHROUGH
    ///
    /// Pass-through flags: CodecPCM, CodecAC3, CodecEAC3, ...
    ///
static char CodecPassthrough;
#else
static const int CodecPassthrough = 0;
#endif


/**
**	Allocate a new audio decoder context.
**
**	@returns private decoder pointer for audio decoder.
*/
AudioDecoder *CodecAudioNewDecoder(void)
{
    AudioDecoder *audio_decoder;

    if (!(audio_decoder = calloc(1, sizeof(*audio_decoder)))) {
		Fatal("codec: can't allocate audio decoder");
    }
    if (!(audio_decoder->Frame = av_frame_alloc())) {
		Fatal("codec: can't allocate audio decoder frame buffer");
    }
	audio_decoder->AudioCtx = NULL;

    return audio_decoder;
}

/**
**	Deallocate an audio decoder context.
**
**	@param decoder	private audio decoder
*/
void CodecAudioDelDecoder(AudioDecoder * decoder)
{
    av_frame_free(&decoder->Frame);	// callee does checks
    free(decoder);
}

/**
**	Open audio decoder.
**
**	@param audio_decoder	private audio decoder
**	@param codec_id	audio	codec id
*/
void CodecAudioOpen(AudioDecoder * audio_decoder, int codec_id,
		AVCodecParameters *Par, AVRational * timebase)
{
	const AVCodec *codec;

	if (codec_id == AV_CODEC_ID_AC3) {
		if (!(codec = avcodec_find_decoder_by_name("ac3_fixed"))) {
			Fatal("codec: codec ac3_fixed ID %#06x not found", codec_id);
		}
	} else if (codec_id == AV_CODEC_ID_AAC) {
		if (!(codec = avcodec_find_decoder_by_name("aac_fixed"))) {
			Fatal("codec: codec aac_fixed ID %#06x not found", codec_id);
		}
	} else {
		if (!(codec = avcodec_find_decoder(codec_id))) {
			Fatal("codec: codec %s ID %#06x not found",
				avcodec_get_name(codec_id), codec_id);
			// FIXME: errors aren't fatal
		}
	}

	if (!(audio_decoder->AudioCtx = avcodec_alloc_context3(codec))) {
		Fatal("codec: can't allocate audio codec context");
	}

	audio_decoder->AudioCtx->pkt_timebase.num = timebase->num;
	audio_decoder->AudioCtx->pkt_timebase.den = timebase->den;

	if (Par) {
		if ((avcodec_parameters_to_context(audio_decoder->AudioCtx, Par)) < 0)
			Error("CodecAudioOpen: insert parameters to context failed!");
	}

	// open codec
	if (avcodec_open2(audio_decoder->AudioCtx, audio_decoder->AudioCtx->codec, NULL) < 0) {
		Fatal("codec: can't open audio codec");
	}
	Debug2(L_CODEC, "CodecAudioOpen: Codec %s found", audio_decoder->AudioCtx->codec->long_name);
}

/**
**	Close audio decoder.
**
**	@param audio_decoder	private audio decoder
*/
void CodecAudioClose(AudioDecoder * audio_decoder)
{
	Debug2(L_CODEC, "CodecAudioClose");
	if (audio_decoder->AudioCtx) {
		avcodec_free_context(&audio_decoder->AudioCtx);
	}
}

/**
**	Set audio pass-through.
**
**	@param mask	enable mask (PCM, AC-3, E-AC-3)
*/
void CodecSetAudioPassthrough(int mask)
{
#ifdef USE_PASSTHROUGH
    CodecPassthrough = mask & (CodecPCM | CodecAC3 | CodecEAC3);
#endif
    (void)mask;
}

/**
**	Decode an audio packet.
**
**	@param audio_decoder	audio decoder data
**	@param avpkt		audio packet
*/
void CodecAudioDecode(AudioDecoder * audio_decoder, const AVPacket * avpkt)
{
	AVFrame *frame;
	int ret_send, ret_rec;

	// FIXME: don't need to decode pass-through codecs
	frame = audio_decoder->Frame;
	av_frame_unref(frame);

send:
	ret_send = avcodec_send_packet(audio_decoder->AudioCtx, avpkt);
	if (ret_send < 0)
		Error("CodecAudioDecode: avcodec_send_packet error: %s",
			av_err2str(ret_send));

	ret_rec = avcodec_receive_frame(audio_decoder->AudioCtx, frame);
	if (ret_rec < 0) {
		Error("CodecAudioDecode: avcodec_receive_frame error: %s",
			av_err2str(ret_rec));
	} else {
		// Control PTS is valid
		if (audio_decoder->last_pts == (int64_t) AV_NOPTS_VALUE &&
			frame->pts == (int64_t) AV_NOPTS_VALUE) {
			Warning("CodecAudioDecode: NO VALID PTS");
		}
		// update audio clock
		if (frame->pts != (int64_t) AV_NOPTS_VALUE) {
			audio_decoder->last_pts = frame->pts;
		} else if (audio_decoder->last_pts != (int64_t) AV_NOPTS_VALUE) {
			frame->pts = audio_decoder->last_pts + 
				(int64_t)(frame->nb_samples /
				av_q2d(audio_decoder->AudioCtx->pkt_timebase) /
				frame->sample_rate);
			audio_decoder->last_pts = frame->pts;
		}
		AudioFilter(frame, audio_decoder->AudioCtx);
	}

	if (ret_send == AVERROR(EAGAIN))
		goto send;
}


/**
**	Flush the audio decoder.
**
**	@param decoder	audio decoder data
*/
void CodecAudioFlushBuffers(AudioDecoder * decoder)
{
	Debug2(L_CODEC, "CodecAudioFlushBuffers");
	if (decoder->AudioCtx) {
		avcodec_flush_buffers(decoder->AudioCtx);
	}
	decoder->last_pts = AV_NOPTS_VALUE;
}

//----------------------------------------------------------------------------
//	Codec
//----------------------------------------------------------------------------

/**
**	log callbacks
*/
#ifdef FFMPEG_DEBUG
#define AV_LOGLEVEL AV_LOG_TRACE
static void CodecLogCallback( __attribute__ ((unused))
    void *ptr, __attribute__ ((unused))
    int level, __attribute__ ((unused))
    const char *fmt, va_list vl)
{
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
static void CodecNoopCallback( __attribute__ ((unused))
    void *ptr, __attribute__ ((unused))
    int level, __attribute__ ((unused))
    const char *fmt, __attribute__ ((unused)) va_list vl)
{
}
#endif

/**
**	Codec init
*/
void CodecInit(void)
{
#ifdef FFMPEG_DEBUG
	av_log_set_level(AV_LOG_INFO);
//	av_log_set_level(AV_LOG_DEBUG );
//	av_log_set_level(AV_LOG_ERROR );
	av_log_set_callback(CodecLogCallback);
#else
	av_log_set_callback(CodecNoopCallback);
#endif

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,18,100)
	avcodec_register_all();		// register all formats and codecs
#endif
	pthread_mutex_init(&CodecLockMutex, NULL);
}

/**
**	Codec exit.
*/
void CodecExit(void)
{
	pthread_mutex_destroy(&CodecLockMutex);
}
