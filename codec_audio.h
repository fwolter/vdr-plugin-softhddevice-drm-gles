///
///	@file codec_audio.h	@brief Audio codec module headerfile
///
///	Copyright (c) 2009 - 2013, 2015 by Johns.  All Rights Reserved.
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

/// @addtogroup Codec
/// @{

#ifndef __CODEC_AUDIO_H
#define __CODEC_AUDIO_H

#include <libavcodec/avcodec.h>

//----------------------------------------------------------------------------
//	Defines
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//	Variables, structs
//----------------------------------------------------------------------------

///
///	Audio decoder structure.
///
struct _audio_decoder_
{
    AVCodecContext *AudioCtx;		///< audio codec context

    AVFrame *Frame;			///< decoded audio frame buffer
    int64_t last_pts;			///< last PTS
    int64_t initial_pts;		///< initial PTS
};

//----------------------------------------------------------------------------
//	Typedefs
//----------------------------------------------------------------------------

    /// Audio decoder typedef.
typedef struct _audio_decoder_ AudioDecoder;

//----------------------------------------------------------------------------
//	Prototypes
//----------------------------------------------------------------------------

    /// Allocate a new audio decoder context.
extern AudioDecoder *CodecAudioNewDecoder(void);

    /// Deallocate an audio decoder context.
extern void CodecAudioDelDecoder(AudioDecoder *);

    /// Open audio codec.
extern void CodecAudioOpen(AudioDecoder *, int, AVCodecParameters *, AVRational *);

    /// Close audio codec.
extern void CodecAudioClose(AudioDecoder *);

    /// Decode an audio packet.
extern void CodecAudioDecode(AudioDecoder *, const AVPacket *);

    /// Flush audio buffers.
extern void CodecAudioFlushBuffers(AudioDecoder *);

/// @}

#endif
