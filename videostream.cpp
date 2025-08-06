///
///	@file softhddev.c	@brief A software HD device plugin for VDR.
///
///	Copyright (c) 2011 - 2015 by Johns.  All Rights Reserved.
///	Copyright (c) 2018 - 2019 by zille.  All Rights Reserved.
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

#define noDUMP_TRICKSPEED		///< dump raw trickspeed packets

#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <assert.h>
#include <unistd.h>

#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut

#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "softhddevice-drm-gles.h"
#include "softhddevice.h"
#include "logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>

}
#include "buf2rgb.h"

#include "iatomic.h"			// portable atomic_t
#include "videostream.h"
#include "audio.h"
#include "video.h"
#include "codec_audio.h"
#include "codec_video.h"

//////////////////////////////////////////////////////////////////////////////
//	cVideoStream
//////////////////////////////////////////////////////////////////////////////

/**
**	Constructor stream
*/
cVideoStream::cVideoStream(cSoftHdDevice *device)
{
    Device = device;
    Render = Device->Render;

    ClosingStream = 0;

    LOGDEBUG("%s:", __FUNCTION__);
}

/**
**	Constructor stream
*/
cVideoStream::~cVideoStream(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
}

/**
**	Initialize video packet ringbuffer.
*/
void cVideoStream::PacketInit(void)
{
	for (int i = 0; i < VIDEO_PACKET_MAX; ++i) {
		AVPacket *avpkt;

		avpkt = &PacketRb[i];
		if (av_new_packet(avpkt, VIDEO_BUFFER_SIZE)) {
			LOGFATAL("out of memory");
		}
		avpkt->size = 0;
	}

	atomic_set(&PacketsFilled, 0);
	PacketRead = 0;
	PacketWrite = 0;
}

/**
**	Cleanup video packet ringbuffer.
*/
void cVideoStream::PacketExit(void)
{
	atomic_set(&PacketsFilled, 0);

	for (int i = 0; i < VIDEO_PACKET_MAX; ++i) {
		av_packet_unref(&PacketRb[i]);
	}
}

/**
**	Place video data in packet ringbuffer.
**
**	@param pts	presentation timestamp of pes packet
**	@param data	data of pes packet
**	@param size	size of pes packet
*/
void cVideoStream::Enqueue(int64_t pts, const void *data, int size)
{
	AVPacket *avpkt;

	avpkt = &PacketRb[PacketWrite];

	if (pts != AV_NOPTS_VALUE) {
		if (avpkt->size) {
			PacketWrite = (PacketWrite + 1) % VIDEO_PACKET_MAX;
			atomic_inc(&PacketsFilled);
		}
		avpkt = &PacketRb[PacketWrite];
		avpkt->size = 0;
		avpkt->pts = pts;
		avpkt->dts = AV_NOPTS_VALUE;
	}

	if ((size_t)(avpkt->size + size) >= avpkt->buf->size) {
		int pkt_size = avpkt->size;
		LOGWARNING("video: packet buffer too small for %d",
			avpkt->size + size);
		av_grow_packet(avpkt, size);
		avpkt->size = pkt_size;
	}

	memcpy(avpkt->data + avpkt->size, data, size);
	avpkt->size += size;
	memset(avpkt->data + avpkt->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
}

/**
**	Close video stream.
**
**	@param stream	video stream
**
*/
void cVideoStream::Close(void)
{
	LOGDEBUG("VideoStreamClose:");
	if (Decoder) {
		Decoder->Close();
		delete(Decoder);
		Decoder = nullptr;
	}

	PacketExit();
}

/**
**	Clears all video data from the device.
*/
void cVideoStream::ClearVideo(void)
{
	// ClearVideo does not come from Play() or SetPlayMode() (closing_stream_requested)
	// but from Clear() (or StillPicture) which is directly from VDR
	// Wait for the clear, until the decode thread finished a decoding process
	// The problem here is, that the reopen workaround deletes the VideoCtx for a moment,
	// which of course is a problem for VideoDecodeInput in the separate thread, because
	// that one wants VideoCtx....
	if (!ClosingStream) {
		StreamWait = 1;
		while (StreamWait == 1) {
			// don't wait, if no thread is running, which should set stream->wait = 2
			// otherwise we run into an endless loop here
			if (!Render->DecodeThread->Active())
				StreamWait = 2;
			usleep(10000);
		}
	}

	AVPacket *avpkt;
	LOGDEBUG("ClearVideo() packets %d", atomic_read(&PacketsFilled));
	pthread_mutex_lock(&PktsLockMutex);
	atomic_set(&PacketsFilled, 0);
	PacketRead = PacketWrite = 0;

	avpkt = &PacketRb[PacketWrite];
	avpkt->size = 0;
	avpkt->pts = AV_NOPTS_VALUE;

	if (Render->HardwareQuirks & QUIRK_CODEC_FLUSH_WORKAROUND) {
		if (Decoder->ReopenCodec(CodecID, Par, &timebase, 0))
			LOGFATAL("ClearVideo: Could not reopen the decoder (flush buffers)!");
	} else {
		Decoder->FlushBuffers();
	}
	pthread_mutex_unlock(&PktsLockMutex);

	// no need to wait in decoding thread anymore
	StreamWait = 0;
}

int cVideoStream::closing_stream_requested(void)
{
	if (ClosingStream && CodecID != AV_CODEC_ID_NONE) {
		pthread_mutex_lock(&WaitCloseMutex);
		if (!TrickSpeed || !Render->VideoGetTrickForward()) {
			ClearVideo();
			CodecID = AV_CODEC_ID_NONE;
			Decoder->Close();
			Par = NULL;
		}
		ClosingStream = 0;
		pthread_cond_signal(&WaitCloseCondition);
		pthread_mutex_unlock(&WaitCloseMutex);
		return -1;
	}
	return 0;
}

/**
**	Decode from PES packet ringbuffer.
**
**	@param stream	video stream
**
**	@retval 0	packet decoded or more data needed
**	@retval	1	stream paused
**	@retval	-1	empty stream
*/
int cVideoStream::DecodeInput(void)
{
	AVPacket *avpkt;
	AVFrame *frame;
	int ret = 0;
	static int sent = 0;

	if (closing_stream_requested())
		return -1;

	if (StreamWait) {
		StreamWait = 2; // signalise, the turn has finished and we are waiting
		return -1;
	}

	if (StreamFreezed) {		// stream freezed
//		LOGINFO("VideoDecodeInput: stream->Freezed");
		// clear is called during freezed
		return 1;
	}

	if (NewStream && CodecID != AV_CODEC_ID_NONE) {
		int pWidth = 0;
		int pHeight = 0;

		// amlogic h264 decoder needs this
		if ((CodecID == AV_CODEC_ID_H264) && (Render->HardwareQuirks & QUIRK_CODEC_NEEDS_EXT_INIT)) {
			ParseResolutionH264(&pWidth, &pHeight);
			LOGDEBUG2(L_CODEC, "cVideoStream::DecodeInput: Parsed width %d height %d", pWidth, pHeight);
		}

		if (Decoder->Open(CodecID, Par, &timebase, 0, pWidth, pHeight))
			LOGFATAL("VideoDecodeInput: Could not open the decoder!");
		NewStream = 0;
	}

	if (CodecID != AV_CODEC_ID_NONE) {
		pthread_mutex_lock(&PktsLockMutex);
		// in trickspeed wait for minimum pkts needed to decode a frame
		int minpkts = (Render->VideoGetTrickSpeed() && interlaced) ? trickpkts : 1;
		if (atomic_read(&PacketsFilled) < minpkts) {
			pthread_mutex_unlock(&PktsLockMutex);
			return -1;
		}
		avpkt = &PacketRb[PacketRead];

		// try sending packet to decoder
		ret = Decoder->SendPacket(avpkt);
		if (ret != AVERROR(EAGAIN)) { // something went wrong or packet was sent, advance packet
			PacketRead = (PacketRead + 1) % VIDEO_PACKET_MAX;
			atomic_dec(&PacketsFilled);
			// in backward trickspeed force the decoder to decode the frame
			if (ret == 0 && Render->VideoGetTrickSpeed() && !Render->VideoGetTrickForward()) {
				sent++;
				if (sent >= minpkts) {
					Decoder->SendPacket(NULL);
					sent = 0;
				}
			}
		}

		pthread_mutex_unlock(&PktsLockMutex);

// this is normal Playback
		if (!Render->VideoGetTrickSpeed()) {
			if (!NewStream) { // this is for mediaplayer ?
				if (!Decoder->ReceiveFrame(0, &frame)) {
					while (Render->VideoRenderFrame(Decoder->GetContext(), frame, 0)) {
						if (closing_stream_requested()) {
							av_frame_free(&frame);
							return -1;
						}
					}
				}
			}
// this is normal TrickSpeed
		} else {
receive_trickspeed:
			// try receiving frame from decoder
			ret = Decoder->ReceiveFrame(1, &frame);
			if (ret == 0) {
				while (Render->VideoGetTrickSpeed() && Render->VideoGetTrickCounter() > 0) {
					AVFrame *trickframe = av_frame_clone(frame);
					if (!trickframe) {
						LOGERROR("VideoDecodeInput: could not clone frame");
						break;
					}
					LOGDEBUG2(L_TRICK, "VideoDecodeInput: Trickspeed, send another cloned trick frame %d %p", Render->VideoGetTrickCounter(), trickframe);
					while (Render->VideoRenderFrame(Decoder->GetContext(), trickframe, FRAME_FLAG_TRICKSPEED)) {
						if (closing_stream_requested()) {
							av_frame_free(&trickframe);
							av_frame_free(&frame);
							sent = 0;
							return -1;
						}
					}
					Render->VideoDecTrickCounter();
					if (closing_stream_requested()) {
						av_frame_free(&frame);
						sent = 0;
						return -1;
					}
				}
				av_frame_free(&frame);
				sent = 0;

				int TrickSpeed = Render->VideoGetTrickSpeed();
				Render->VideoSetTrickCounter(TrickSpeed);

				goto receive_trickspeed; // try to get another frame
			} else if (ret == AVERROR_EOF) { // needs flush / reopen
				if (Render->HardwareQuirks & QUIRK_CODEC_FLUSH_WORKAROUND) {
					if (Decoder->ReopenCodec(CodecID, Par, &timebase, 0))
						LOGFATAL("VideoDecodeInput: Could not reopen the decoder (flush buffers)!");
				} else {
					Decoder->FlushBuffers();
				}
				sent = 0;
			}
		}
		return 0;
	}

	return -1;
}

/**
**	Get number of video buffers.
**
**	@param stream	video stream
*/
int cVideoStream::GetPackets(void)
{
    return atomic_read(&PacketsFilled);
}

void cVideoStream::SetInterlaced(int interl)
{
//	LOGDEBUG("SetInterlaced %d", interlaced);
	interlaced = interl;
}

/**
**	helper functions to parse resolution from stream
*/
unsigned int cVideoStream::ReadBit()
{
	assert(m_nCurrentBit <= m_nLength * 8);
	int nIndex = m_nCurrentBit / 8;
	int nOffset = m_nCurrentBit % 8 + 1;

	m_nCurrentBit++;
	return (m_pStart[nIndex] >> (8-nOffset)) & 0x01;
}

unsigned int cVideoStream::ReadBits(int n)
{
	int r = 0;

	for (int i = 0; i < n; i++) {
		r |= ( ReadBit() << ( n - i - 1 ) );
	}
	return r;
}

unsigned int cVideoStream::ReadExponentialGolombCode()
{
	int r = 0;
	int i = 0;

	while((ReadBit() == 0) && (i < 32)) {
		i++;
	}

	r = ReadBits(i);
	r += (1 << i) - 1;
	return r;
}

unsigned int cVideoStream::ReadSE()
{
	int r = ReadExponentialGolombCode();

	if (r & 0x01) {
		r = (r+1)/2;
	} else {
		r = -(r/2);
	}
	return r;
}

/**
**	Parse h264 stream to get width and height
**
**	@param[out] width		video stream width
**	@param[out] height		video stream height
*/
void cVideoStream::ParseResolutionH264(int *width, int *height)
{
	AVPacket *avpkt;
	m_pStart = NULL;
	int i;

	while (!atomic_read(&PacketsFilled)) {
		usleep(10000);
	}

	avpkt = &PacketRb[PacketRead];

	for (i = 0; i < avpkt->size; i++) {
		if (!avpkt->data[i] && !avpkt->data[i + 1] && avpkt->data[i + 2] == 0x01 &&
			(avpkt->data[i + 3] == 0x67 || avpkt->data[i + 3] == 0x27)) {

			m_pStart = &avpkt->data[i + 4];
			m_nLength = avpkt->size - i - 4;
			break;
		}
	}
	if (!m_pStart) {
		LOGDEBUG("ParseResolutionH264: No m_pStart %p Pkt %p Packets %d i %d",
			m_pStart, avpkt, atomic_read(&PacketsFilled), i);
//		PrintStreamData(avpkt->data, avpkt->size);
		return;
	}

	m_nCurrentBit = 0;
	int frame_crop_left_offset = 0;
	int frame_crop_right_offset = 0;
	int frame_crop_top_offset = 0;
	int frame_crop_bottom_offset = 0;
	int chroma_format_idc = 0;
	int separate_colour_plane_flag = 0;

	int profile_idc = ReadBits(8);
	ReadBits(16);
	ReadExponentialGolombCode();

	if (profile_idc == 100 || profile_idc == 110 ||
		profile_idc == 122 || profile_idc == 244 ||
		profile_idc == 44 || profile_idc == 83 ||
		profile_idc == 86 || profile_idc == 118) {

		chroma_format_idc = ReadExponentialGolombCode();
		if (chroma_format_idc == 3) {
			separate_colour_plane_flag = ReadBit();
		}
		ReadExponentialGolombCode();
		ReadExponentialGolombCode();
		ReadBit();
		int seq_scaling_matrix_present_flag = ReadBit();
		if (seq_scaling_matrix_present_flag) {
			for (int i = 0; i < 8; i++) {
				int seq_scaling_list_present_flag = ReadBit();
				if (seq_scaling_list_present_flag) {
					int sizeOfScalingList = (i < 6) ? 16 : 64;
					int lastScale = 8;
					int nextScale = 8;
					for (int j = 0; j < sizeOfScalingList; j++) {
						if (nextScale != 0) {
							int delta_scale = ReadSE();
							nextScale = (lastScale + delta_scale + 256) % 256;
						}
						lastScale = (nextScale == 0) ? lastScale : nextScale;
					}
				}
			}
		}
	}
	ReadExponentialGolombCode();
	int pic_order_cnt_type = ReadExponentialGolombCode();
	if (pic_order_cnt_type == 0) {
		ReadExponentialGolombCode();
	} else if (pic_order_cnt_type == 1) {
		ReadBit();
		ReadSE();
		ReadSE();
		int num_ref_frames_in_pic_order_cnt_cycle = ReadExponentialGolombCode();
		for (int i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++ ) {
			ReadSE();
		}
	}
	ReadExponentialGolombCode();
	ReadBit();
	int pic_width_in_mbs_minus1 = ReadExponentialGolombCode();
	int pic_height_in_map_units_minus1 = ReadExponentialGolombCode();
	int frame_mbs_only_flag = ReadBit();
	if (!frame_mbs_only_flag) {
		ReadBit();
	}
	ReadBit();
	int frame_cropping_flag = ReadBit();
	if (frame_cropping_flag) {
		frame_crop_left_offset = ReadExponentialGolombCode();
		frame_crop_right_offset = ReadExponentialGolombCode();
		frame_crop_top_offset = ReadExponentialGolombCode();
		frame_crop_bottom_offset = ReadExponentialGolombCode();
	}

	int SubWidthC = 0;
	int SubHeightC = 0;

	if (chroma_format_idc == 0 && separate_colour_plane_flag == 0) { //monochrome
		SubWidthC = SubHeightC = 2;
	} else if (chroma_format_idc == 1 && separate_colour_plane_flag == 0) { //4:2:0
		SubWidthC = SubHeightC = 2;
	} else if (chroma_format_idc == 2 && separate_colour_plane_flag == 0) { //4:2:2
		SubWidthC = 2;
		SubHeightC = 1;
	} else if (chroma_format_idc == 3) { //4:4:4
		if (separate_colour_plane_flag == 0) {
		SubWidthC = SubHeightC = 1;
		} else if (separate_colour_plane_flag == 1) {
			SubWidthC = SubHeightC = 0;
		}
	}

	*width = ((pic_width_in_mbs_minus1 + 1) * 16) -
		SubWidthC * (frame_crop_right_offset + frame_crop_left_offset);

	*height = ((2 - frame_mbs_only_flag)* (pic_height_in_map_units_minus1 +1) * 16) -
		SubHeightC * ((frame_crop_bottom_offset * 2) + (frame_crop_top_offset * 2));
}
