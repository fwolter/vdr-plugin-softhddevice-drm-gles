///
///	@file codec_audio.cpp	@brief Audio decoder functions
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

#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>

extern "C" {
#include <libavcodec/avcodec.h>

#include "codec_audio.h"
#include "misc.h"
#include "audio.h"
}

/*****************************************************************************
**	cAudioDecoder class
*****************************************************************************/

/**
**	cAudioDecoder constructor
*/
cAudioDecoder::cAudioDecoder(void)
{
    if (!(Frame = av_frame_alloc()))
	Fatal("cAudioDecoder::cAudioDecoder: can't allocate audio decoder frame buffer");
    AudioCtx = NULL;
}

/**
**	cAudioDecoder destructor
*/
cAudioDecoder::~cAudioDecoder(void)
{
    av_frame_free(&Frame);
}

/**
**	Open audio decoder.
**
**	@param audio_decoder	private audio decoder
**	@param codec_id	audio	codec id
*/
void cAudioDecoder::Open(enum AVCodecID codec_id, AVCodecParameters *Par, AVRational * timebase)
{
	const AVCodec *codec;

	if (codec_id == AV_CODEC_ID_AC3) {
		if (!(codec = avcodec_find_decoder_by_name("ac3_fixed"))) {
			Fatal("cAudioDecoder::Open: codec ac3_fixed ID %#06x not found", codec_id);
		}
	} else if (codec_id == AV_CODEC_ID_AAC) {
		if (!(codec = avcodec_find_decoder_by_name("aac_fixed"))) {
			Fatal("cAudioDecoder::Open: codec aac_fixed ID %#06x not found", codec_id);
		}
	} else {
		if (!(codec = avcodec_find_decoder(codec_id))) {
			Fatal("cAudioDecoder::Open: codec %s ID %#06x not found",
				avcodec_get_name(codec_id), codec_id);
			// FIXME: errors aren't fatal
		}
	}

	if (!(AudioCtx = avcodec_alloc_context3(codec))) {
		Fatal("cAudioDecoder::Open: can't allocate audio codec context");
	}

	AudioCtx->pkt_timebase.num = timebase->num;
	AudioCtx->pkt_timebase.den = timebase->den;

	if (Par) {
		if ((avcodec_parameters_to_context(AudioCtx, Par)) < 0)
			Error("cAudioDecoder::Open: insert parameters to context failed!");
	}

	// open codec
	if (avcodec_open2(AudioCtx, AudioCtx->codec, NULL) < 0) {
		Fatal("cAudioDecoder::Open: can't open audio codec");
	}
	Debug2(L_CODEC, "cAudioDecoder::Open: Codec %s found", AudioCtx->codec->long_name);
}

/**
**	Close audio decoder
**
*/
void cAudioDecoder::Close(void)
{
	Debug2(L_CODEC, "cAudioDecoder::Close");
	if (AudioCtx)
		avcodec_free_context(&AudioCtx);
}

/**
**	Decode an audio packet.
**
**	@param avpkt		audio packet
*/
void cAudioDecoder::Decode(const AVPacket * avpkt)
{
	AVFrame *frame;
	int ret_send, ret_rec;

	// FIXME: don't need to decode pass-through codecs
	frame = Frame;
	av_frame_unref(frame);

send:
	ret_send = avcodec_send_packet(AudioCtx, avpkt);
	if (ret_send < 0)
		Error("cAudioDecoder::Decode: avcodec_send_packet error: %s",
			av_err2str(ret_send));

	ret_rec = avcodec_receive_frame(AudioCtx, frame);
	if (ret_rec < 0) {
		if (ret_rec != AVERROR(EAGAIN)) {
			Error("cAudioDecoder::Decode: avcodec_receive_frame error: %s",
				av_err2str(ret_rec));
		} else if (last_pts == (int64_t)AV_NOPTS_VALUE && avpkt->pts != (int64_t)AV_NOPTS_VALUE) {
			// if multiple avpkt are needed for the (first!) frame (last_pts == AV_NOPTS_VALUE),
			// remember the avpkt->pts if we have one and use it for the frame->pts
			// if we don't get one after decode. this way, last_pts also gets set
			Debug2(L_CODEC, "cAudioDecoder::Decode: New audio stream, set initial pts to avpkt->pts %s",
				Timestamp2String(avpkt->pts * 1000 * av_q2d(AudioCtx->pkt_timebase)));
			initial_pts = avpkt->pts;
		}
	} else {
		// Control PTS is valid
		if (last_pts == (int64_t) AV_NOPTS_VALUE &&
			frame->pts == (int64_t) AV_NOPTS_VALUE) {
			Warning("cAudioDecoder::Decode: NO VALID PTS, set frame->pts to last known avpkt->pts %s",
				Timestamp2String(initial_pts * 1000 * av_q2d(AudioCtx->pkt_timebase)));
			frame->pts = initial_pts;
			initial_pts = AV_NOPTS_VALUE;
		}
		// update audio clock
		if (frame->pts != (int64_t) AV_NOPTS_VALUE) {
			last_pts = frame->pts;
		} else if (last_pts != (int64_t) AV_NOPTS_VALUE) {
			frame->pts = last_pts +
				(int64_t)(frame->nb_samples /
				av_q2d(AudioCtx->pkt_timebase) /
				frame->sample_rate);
			last_pts = frame->pts;
		}
		AudioFilter(frame, AudioCtx);
	}

	if (ret_send == AVERROR(EAGAIN))
		goto send;
}


/**
**	Flush the audio decoder.
*/
void cAudioDecoder::FlushBuffers(void)
{
	Debug2(L_CODEC, "cAudioDecoder::FlushBuffers");
	if (AudioCtx)
		avcodec_flush_buffers(AudioCtx);

	last_pts = AV_NOPTS_VALUE;
}
