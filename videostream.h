/**
 * @file videostream.h
 * @brief Video Stream class declaration
 *
 * Copyright: (c) 2011 - 2015 by Johns.  All Rights Reserved.
 * Copyright: (c) 2025 by Andreas Baierl. All Rights Reserved.
 *
 * License: AGPLv3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 */

#ifndef __VIDEOSTREAM_H
#define __VIDEOSTREAM_H

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "codec_video.h"

#define VIDEO_BUFFER_SIZE (512 * 1024)		///< video PES buffer default size
#define VIDEO_PACKET_MAX 192			///< max number of video packets held in ringbuffer

class cVideoDecoder;
class cVideoRender;

/**
 * @brief cVideoStream - Video stream class
 */
class cVideoStream
{
public:
	cVideoStream(cSoftHdDevice *);
	virtual ~cVideoStream(void);

	cVideoDecoder *Decoder(void) { return m_pDecoder; };
	void SetDecoder(cVideoDecoder *decoder) { m_pDecoder = decoder; };

	void InitPacketRb(void);
	void EnqueueInRb(int64_t, const void *, int);
	int DecodeInput(void);

	void Open(void) { m_newStream = 1; };
	void RequestClose(int);
	void Freeze(void) { m_freezed = 1; };
	void WakeUp(void) { m_freezed = 0; };
	int IsClosing(void) { return m_closing; };
	int IsFreezed(void) { return m_freezed; };

	void Exit(void);
	void ClearVideo(void);

	void SetCodecId(enum AVCodecID id) { m_codecId = id; };
	void SetParameters(AVCodecParameters *par) { m_pPar = par; };
	void SetTimebase(int, int);
	void SetTrickpkts(int pkts) { m_trickpkts = pkts; };
	void SetTrickSpeed(int trick) { m_trickSpeed = trick; };
	void SetInterlaced(int);
	int GetTrickSpeed(void) { return m_trickSpeed; };
	int GetPacketsFilled(void);
	void IncreasePacketsFilled(void);
	AVPacket *GetPacketToWrite(void);
	void AdvancePacketToWrite(void);
	enum AVCodecID GetCodecId(void) { return m_codecId; };

private:
	cVideoDecoder *m_pDecoder;		///< video decoder
	cVideoRender *m_pRender;		///< video renderer

	// TODO: move ringbuffer to a separate class
	AVPacket m_packetRb[VIDEO_PACKET_MAX];	///< PES packet ring buffer
	int m_packetWrite;			///< ring buffer write pointer
	int m_packetRead;			///< ring buffer read pointer
	atomic_t m_packetsFilled;		///< how many of the ring buffer is used

	enum AVCodecID m_codecId;		///< current codec id
	AVCodecParameters *m_pPar;		///< current codec parameters
	struct AVRational m_timebase;		///< current codec timepase
	int m_trickpkts;			///< how many avpkt does the decoder need in trickspeed mode?

	volatile char m_newStream;		///< flag for new stream
	volatile char m_closing;		///< flag for closing request
	volatile char m_trickSpeed;		///< flag for trickspeed stream
	volatile char m_freezed;		///< flag for freezed stream
	int m_interlaced;			///< flag for interlaced stream
	int m_wait;				///< we should wait for decoding next frame
						///< 0: no need to wait, 1: wait requested, 2: wating
						///< TODO: need to do a better solution
	cMutex m_pktsMutex;			///< mutex for accessing the packet ringbuffer
	cCondWait m_closeCondition;		///< condition object to wait for finishing jobs while closing

	void CleanupPacketRb(void);
	int CloseRequested(void) { return (m_closing && m_codecId != AV_CODEC_ID_NONE); };
	void Close(void);
};

#endif
