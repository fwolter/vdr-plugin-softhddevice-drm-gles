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

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "codec_video.h"

#define VIDEO_BUFFER_SIZE (512 * 1024)	///< video PES buffer default size
#define VIDEO_PACKET_MAX 192		///< max number of video packets

class cVideoDecoder;

class cVideoStream
{
private:
public:
    cVideoStream(cSoftHdDevice *);
    virtual ~cVideoStream(void);

    cVideoDecoder *Decoder;	///< video decoder
    cSoftHdDevice *Device;
    cVideoRender *Render;

//    cVideoDecoder *GetDecoder(void) { return Decoder; };

    const unsigned char *m_pStart;
    unsigned short m_nLength;
    int m_nCurrentBit;
    unsigned int ReadBit(void);
    unsigned int ReadBits(int);
    unsigned int ReadExponentialGolombCode(void);
    unsigned int ReadSE(void);
    void ParseResolutionH264(int *, int *);

    void PacketInit(void);
    void PacketExit(void);
    void Enqueue(int64_t, const void *, int);
    void Close(void);

    void ClearVideo(void);
    int closing_stream_requested(void);
    int DecodeInput(void);
    int GetPackets(void);
    void SetInterlaced(int);

    AVPacket PacketRb[VIDEO_PACKET_MAX];	///< PES packet ring buffer
    int PacketWrite;			///< ring buffer write pointer
    int PacketRead;			///< ring buffer read pointer
    atomic_t PacketsFilled;		///< how many of the ring buffer is used

    enum AVCodecID CodecID;		///< current codec id
    AVCodecParameters * Par;
    struct AVRational timebase;
    int trickpkts;			///< how many avpkt does the decoder need in trickspeed mode?

    volatile char NewStream;		///< flag new video stream
    volatile char ClosingStream;	///< flag closing video stream
    volatile char TrickSpeed;		///< flag trickspeed stream
    volatile char StreamFreezed;	///< stream freezed
    int interlaced;			///< is this an interlaced stream?
    int StreamWait;			///< we should wait for decoding next frame
					///< 0: no need to wait, 1: wait requested, 2: wating

    pthread_mutex_t PktsLockMutex;	///< video packets lock mutex
    pthread_mutex_t WaitCloseMutex;	///< mutex for closing stream
    pthread_cond_t WaitCloseCondition;	///< condition for closing stream
};
#endif
