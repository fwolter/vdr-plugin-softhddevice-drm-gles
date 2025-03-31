///
///	@file codec_audio.h	@brief Audio decoder module headerfile
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
//	Audio
//----------------------------------------------------------------------------

/**
**	cAudioDecoder - AudioDecoder class
*/

class cAudioDecoder {
private:
    AVCodecContext *AudioCtx;		///< audio codec context
    AVFrame *Frame;			///< decoded audio frame buffer
    int64_t last_pts;			///< last PTS
    int64_t initial_pts;		///< initial PTS
public:
    cAudioDecoder(void);
    virtual ~cAudioDecoder(void);
    void Open(enum AVCodecID, AVCodecParameters *, AVRational *);
	///< Open audio decoder
    void Close(void);
	///< Close audio decoder
    void Decode(const AVPacket *);
	///< Decode an audio packet
    void FlushBuffers(void);
	///< Flush audio buffers
};
#endif
/// @}
