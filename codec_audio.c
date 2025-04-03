///
///	@file codec_audio.c	@brief Audio codec functions
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

#include <libavcodec/avcodec.h>

#include "codec_audio.h"
#include "misc.h"
#include "audio.h"

//----------------------------------------------------------------------------
//	Audio
//----------------------------------------------------------------------------

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
		if (ret_rec != AVERROR(EAGAIN)) {
			Error("CodecAudioDecode: avcodec_receive_frame error: %s",
				av_err2str(ret_rec));
		}
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
