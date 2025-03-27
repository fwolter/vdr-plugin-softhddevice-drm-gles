///
///	@file codec_video.h	@brief Video codec module headerfile
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

#ifndef __CODEC_VIDEO_H
#define __CODEC_VIDEO_H

#include <libavcodec/avcodec.h>

#include "video.h"

//----------------------------------------------------------------------------
//	Video
//----------------------------------------------------------------------------
///
///	Video decoder structure.
///
struct _video_decoder_
{
    VideoRender *Render;		///< video hardware decoder

    AVCodecContext *VideoCtx;		///< video codec context
    int sent;
    int received;
    int last_coded_width;
    int last_coded_height;
    int FirstKeyFrame;
};

//----------------------------------------------------------------------------
//	Typedefs
//----------------------------------------------------------------------------

    /// Video decoder typedef.
typedef struct _video_decoder_ VideoDecoder;


//----------------------------------------------------------------------------
//	Prototypes
//----------------------------------------------------------------------------

    /// Allocate a new video decoder context.
extern VideoDecoder *CodecVideoNewDecoder(VideoRender *);

    /// Deallocate a video decoder context.
extern void CodecVideoDelDecoder(VideoDecoder *);

	/// Get VideoContext
extern  AVCodecContext *Codec_get_VideoContext(VideoDecoder *);

    /// Open video codec.
extern int CodecVideoOpen(VideoDecoder *, int, AVCodecParameters *, AVRational *, int, int, int);

    /// Close video codec.
extern void CodecVideoClose(VideoDecoder *);

    /// Decode a video packet.
extern int CodecVideoSendPacket(VideoDecoder *, const AVPacket *);

extern int CodecVideoReceiveFrame(VideoDecoder *, int, AVFrame **);

    /// Flush video buffers.
extern void CodecVideoFlushBuffers(VideoDecoder *);

    /// Close and reopen video codec.
extern int CodecVideoReopenCodec(VideoDecoder *, int, AVCodecParameters *, AVRational *, int);

    /// Setup and initialize codec module.
extern void CodecInit(void);

    /// Cleanup and exit codec module.
extern void CodecExit(void);

/// @}

#endif
