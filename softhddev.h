///
///	@file softhddev.h	@brief software HD device plugin header file.
///
///	Copyright (c) 2011 - 2015 by Johns.  All Rights Reserved.
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

#ifndef __SOFTHDDEV_H
#define __SOFTHDDEV_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <libavcodec/avcodec.h>

//----------------------------------------------------------------------------
//	Typedefs
//----------------------------------------------------------------------------
    /// Video output stream typedef
    typedef struct __video_stream__ VideoStream; 		// in softhddev.h ?

//----------------------------------------------------------------------------
//	Prototypes
//----------------------------------------------------------------------------
    /// C plugin close osd
    extern void OsdClose(void);
    /// C plugin draw osd pixmap
    extern void OsdDrawARGB(int, int, int, int, int, const uint8_t *, int,
		int);

    /// C plugin play media file
    extern void SetAudioCodec(enum AVCodecID, AVCodecParameters *, AVRational *);
    extern void SetVideoCodec(enum AVCodecID, AVCodecParameters *, AVRational *);
    extern int PlayAudioPkts(AVPacket *);
    extern int PlayVideoPkts(AVPacket *);

    /// C plugin reset channel id (restarts audio)
    extern void ResetChannelId(void);

    /// Decode video input buffers.
    extern int VideoDecodeInput(VideoStream *);
    /// Get number of input buffers.
    extern int VideoGetPackets(void);

    /// Get decoder statistics
    extern void GetStats(int *, int *, int *);
    /// Get parsed width and height
    extern void ParseResolutionH264(int *, int *);

    /// C plugin get video render
    extern void *GetVideoRender(void);
    /// Set interlaced stream flag
    extern void SetInterlacedStream(int);

    /// Set audio passthrough mask
    extern void SetPassthrough(int);

    /// Set loglevel
    extern void SetLogLevel(int);

#ifdef __cplusplus
}
#endif

#endif
