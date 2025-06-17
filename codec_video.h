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

#include <pthread.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "video.h"
#include "softhddev.h"

//----------------------------------------------------------------------------
//	Video
//----------------------------------------------------------------------------

/**
**	cVideoDecoder - VideoDecoder class
*/
class cVideoDecoder {
private:
    VideoRender *Render;		///< video hardware decoder
    AVCodecContext *VideoCtx = nullptr;	///< video codec context
    VideoStream *Stream;
    int sent;
    int received;
    int last_coded_width;
    int last_coded_height;
    int FirstKeyFrame;
    pthread_mutex_t CodecLockMutex;

    const unsigned char *m_pStart;
    unsigned short m_nLength;
    int m_nCurrentBit;
    unsigned int ReadBit(void);
    unsigned int ReadBits(int);
    unsigned int ReadExponentialGolombCode(void);
    unsigned int ReadSE(void);
    void ParseResolutionH264(int *, int *);

    int GetExtraData(const AVPacket *);
	///< Get extra data from AVPacket
    const AVCodecHWConfig *FindHWConfig(const AVCodec *);
	///< Find a suitable decoder hw config
    const AVCodec* FindDecoder(enum AVCodecID, int);
	///< Find a suitable decoder
	///< codec log fallback
public:
    cVideoDecoder(VideoRender *, VideoStream *);
    virtual ~cVideoDecoder(void);
    int Open(enum AVCodecID, AVCodecParameters *, AVRational *, int, int, int);
	///< Open video codec
    void Close(void);
	///< Close video codec
    int SendPacket(const AVPacket *);
	///< Decode a video packet
    int ReceiveFrame(int, AVFrame **);
	///< Receive a decoded frame
    void FlushBuffers(void);
	///< Flush video buffers
    int ReopenCodec(enum AVCodecID, AVCodecParameters *, AVRational *, int);
	///< Close and reopen video codec
    AVCodecContext *GetContext(void) { return VideoCtx; };
	///< Get VideoContext
    void GetVideoSize(int *, int *, double *);
	///< Get the video size
};

#endif
/// @}

