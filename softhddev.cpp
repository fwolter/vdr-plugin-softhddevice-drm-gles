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

#include "misc.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>

#include "iatomic.h"			// portable atomic_t
#include "softhddev.h"
#include "audio.h"
#include "video.h"
#include "codec_audio.h"
#include "codec_video.h"
//#include "buf2rgb.h"
}
#include "buf2rgb.h"

//////////////////////////////////////////////////////////////////////////////
//	Video
//////////////////////////////////////////////////////////////////////////////

#define VIDEO_BUFFER_SIZE (512 * 1024)	///< video PES buffer default size
#define VIDEO_PACKET_MAX 192		///< max number of video packets

/**
**	Video output stream device structure.	Parser, decoder, display.
*/
struct __video_stream__
{
    VideoRender *Render;		///< video hardware decoder
    cVideoDecoder *Decoder = nullptr;	///< video decoder

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

    AVPacket PacketRb[VIDEO_PACKET_MAX];	///< PES packet ring buffer
    int PacketWrite;			///< ring buffer write pointer
    int PacketRead;			///< ring buffer read pointer
    atomic_t PacketsFilled;		///< how many of the ring buffer is used

    pthread_mutex_t PktsLockMutex;	///< video packets lock mutex
    pthread_mutex_t WaitCloseMutex;	///< mutex for closing stream
    pthread_cond_t WaitCloseCondition;	///< condition for closing stream
};

static VideoStream MyVideoStream[1];	///< normal video stream

//////////////////////////////////////////////////////////////////////////////
//	Audio
//////////////////////////////////////////////////////////////////////////////

static volatile char NewAudioStream;	///< new audio stream
static volatile char SkipAudio;		///< skip audio stream
static cAudioDecoder *MyAudioDecoder;	///< audio decoder
static enum AVCodecID AudioCodecID;	///< current codec id
static int AudioChannelID;		///< current audio channel id
static int AudioPassthrough;
int DebugLogLevel;

    /// Minimum free space in audio buffer 8 packets for 8 channels
#define AUDIO_MIN_BUFFER_FREE (3072 * 8 * 8)
#define AUDIO_BUFFER_SIZE (512 * 1024)	///< audio PES buffer default size
static AVPacket AudioAvPkt[1];		///< audio a/v packet

void PrintStreamData(const uint8_t *data, int size)
{
	LOGDEBUG("Data: %02x %02x %02x %02x %02x %02x %02x %02x %02x "
		"%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
		"%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x size %d",
		data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8],
		data[9], data[10], data[11], data[12], data[13], data[14], data[15], data[16], data[17],
		data[18], data[19], data[20], data[21], data[22], data[23], data[24], data[25], data[26],
		data[27], data[28], data[29], data[30], data[31], data[32], data[33], data[34], size);
}

//////////////////////////////////////////////////////////////////////////////
//	Audio codec parser
//////////////////////////////////////////////////////////////////////////////

///
///	Mpeg bitrate table.
///
///	BitRateTable[Version][Layer][Index]
///
static const uint16_t BitRateTable[2][4][16] = {
    // MPEG Version 1
    {{},
	{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448,
	    0},
	{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
	{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
    // MPEG Version 2 & 2.5
    {{},
	{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
	{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
	{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}
	}
};

///
///	Mpeg samperate table.
///
static const uint16_t SampleRateTable[4] = {
    44100, 48000, 32000, 0
};

///
///	Fast check for Mpeg audio.
///
///	4 bytes 0xFFExxxxx Mpeg audio
///
static inline int FastMpegCheck(const uint8_t * p)
{
    if (p[0] != 0xFF) {			// 11bit frame sync
	return 0;
    }
    if ((p[1] & 0xE0) != 0xE0) {
	return 0;
    }
    if ((p[1] & 0x18) == 0x08) {	// version ID - 01 reserved
	return 0;
    }
    if (!(p[1] & 0x06)) {		// layer description - 00 reserved
	return 0;
    }
    if ((p[2] & 0xF0) == 0xF0) {	// bitrate index - 1111 reserved
	return 0;
    }
    if ((p[2] & 0x0C) == 0x0C) {	// sampling rate index - 11 reserved
	return 0;
    }
    return 1;
}


///
///	Check for Mpeg audio.
///
///	0xFFEx already checked.
///
///	@param data	incomplete PES packet
///	@param size	number of bytes
///
///	@retval <0	possible mpeg audio, but need more data
///	@retval 0	no valid mpeg audio
///	@retval >0	valid mpeg audio
///
///	From: http://www.mpgedit.org/mpgedit/mpeg_format/mpeghdr.htm
///
///	AAAAAAAA AAABBCCD EEEEFFGH IIJJKLMM
///
///	o a 11x Frame sync
///	o b 2x	Mpeg audio version (2.5, reserved, 2, 1)
///	o c 2x	Layer (reserved, III, II, I)
///	o e 2x	BitRate index
///	o f 2x	SampleRate index (4100, 48000, 32000, 0)
///	o g 1x	Paddding bit
///	o ..	Doesn't care
///
///	frame length:
///	Layer I:
///		FrameLengthInBytes = (12 * BitRate / SampleRate + Padding) * 4
///	Layer II & III:
///		FrameLengthInBytes = 144 * BitRate / SampleRate + Padding
///
static int MpegCheck(const uint8_t * data, int size)
{
    int mpeg2;
    int mpeg25;
    int layer;
    int bit_rate_index;
    int sample_rate_index;
    int padding;
    int bit_rate;
    int sample_rate;
    int frame_size;

    mpeg2 = !(data[1] & 0x08) && (data[1] & 0x10);
    mpeg25 = !(data[1] & 0x08) && !(data[1] & 0x10);
    layer = 4 - ((data[1] >> 1) & 0x03);
    bit_rate_index = (data[2] >> 4) & 0x0F;
    sample_rate_index = (data[2] >> 2) & 0x03;
    padding = (data[2] >> 1) & 0x01;

    sample_rate = SampleRateTable[sample_rate_index];
    if (!sample_rate) {			// no valid sample rate try next
	// moved into fast check
	abort();
	return 0;
    }
    sample_rate >>= mpeg2;		// mpeg 2 half rate
    sample_rate >>= mpeg25;		// mpeg 2.5 quarter rate

    bit_rate = BitRateTable[mpeg2 | mpeg25][layer][bit_rate_index];
    if (!bit_rate) {			// no valid bit-rate try next
	// FIXME: move into fast check?
	return 0;
    }
    bit_rate *= 1000;
    switch (layer) {
	case 1:
	    frame_size = (12 * bit_rate) / sample_rate;
	    frame_size = (frame_size + padding) * 4;
	    break;
	case 2:
	case 3:
	default:
	    frame_size = (144 * bit_rate) / sample_rate;
	    frame_size = frame_size + padding;
	    break;
    }
    if (0) {
	LOGDEBUG("pesdemux: mpeg%s layer%d bitrate=%d samplerate=%d %d bytes",
	    mpeg25 ? "2.5" : mpeg2 ? "2" : "1", layer, bit_rate, sample_rate,
	    frame_size);
    }

    if (frame_size + 4 > size) {
	return -frame_size - 4;
    }

    if (FastMpegCheck(data + frame_size)) {
	return frame_size;
    } else {
	LOGDEBUG("MpegCheck: after this frame NO new mpeg frame starts");
	PrintStreamData(data + frame_size, frame_size);
    }

    return 0;
}

///
///	Fast check for AAC LATM audio.
///
///	3 bytes 0x56Exxx AAC LATM audio
///
static inline int FastLatmCheck(const uint8_t * p)
{
    if (p[0] != 0x56) {			// 11bit sync
	return 0;
    }
    if ((p[1] & 0xE0) != 0xE0) {
	return 0;
    }
    return 1;
}

///
///	Check for AAC LATM audio.
///
///	0x56Exxx already checked.
///
///	@param data	incomplete PES packet
///	@param size	number of bytes
///
///	@retval <0	possible AAC LATM audio, but need more data
///	@retval 0	no valid AAC LATM audio
///	@retval >0	valid AAC LATM audio
///
static int LatmCheck(const uint8_t * data, int size)
{
    int frame_size;

    // 13 bit frame size without header
    frame_size = ((data[1] & 0x1F) << 8) + data[2];
    frame_size += 3;

    if (frame_size + 2 > size) {
	return -frame_size - 2;
    }
    // check if after this frame a new AAC LATM frame starts
    if (FastLatmCheck(data + frame_size)) {
	return frame_size;
    }

    return 0;
}

///
///	Possible AC-3 frame sizes.
///
///	from ATSC A/52 table 5.18 frame size code table.
///
const uint16_t Ac3FrameSizeTable[38][3] = {
    {64, 69, 96}, {64, 70, 96}, {80, 87, 120}, {80, 88, 120},
    {96, 104, 144}, {96, 105, 144}, {112, 121, 168}, {112, 122, 168},
    {128, 139, 192}, {128, 140, 192}, {160, 174, 240}, {160, 175, 240},
    {192, 208, 288}, {192, 209, 288}, {224, 243, 336}, {224, 244, 336},
    {256, 278, 384}, {256, 279, 384}, {320, 348, 480}, {320, 349, 480},
    {384, 417, 576}, {384, 418, 576}, {448, 487, 672}, {448, 488, 672},
    {512, 557, 768}, {512, 558, 768}, {640, 696, 960}, {640, 697, 960},
    {768, 835, 1152}, {768, 836, 1152}, {896, 975, 1344}, {896, 976, 1344},
    {1024, 1114, 1536}, {1024, 1115, 1536}, {1152, 1253, 1728},
    {1152, 1254, 1728}, {1280, 1393, 1920}, {1280, 1394, 1920},
};

///
///	Fast check for (E-)AC-3 audio.
///
///	5 bytes 0x0B77xxxxxx AC-3 audio
///
static inline int FastAc3Check(const uint8_t * p)
{
    if (p[0] != 0x0B) {			// 16bit sync
	return 0;
    }
    if (p[1] != 0x77) {
	return 0;
    }
    return 1;
}

///
///	Check for (E-)AC-3 audio.
///
///	0x0B77xxxxxx already checked.
///
///	@param data	incomplete PES packet
///	@param size	number of bytes
///
///	@retval <0	possible AC-3 audio, but need more data
///	@retval 0	no valid AC-3 audio
///	@retval >0	valid AC-3 audio
///
///	o AC-3 Header
///	AAAAAAAA AAAAAAAA BBBBBBBB BBBBBBBB CCDDDDDD EEEEEFFF
///
///	o a 16x Frame sync, always 0x0B77
///	o b 16x CRC 16
///	o c 2x	Samplerate
///	o d 6x	Framesize code
///	o e 5x	Bitstream ID
///	o f 3x	Bitstream mode
///
///	o E-AC-3 Header
///	AAAAAAAA AAAAAAAA BBCCCDDD DDDDDDDD EEFFGGGH IIIII...
///
///	o a 16x Frame sync, always 0x0B77
///	o b 2x	Frame type
///	o c 3x	Sub stream ID
///	o d 10x Framesize - 1 in words
///	o e 2x	Framesize code
///	o f 2x	Framesize code 2
///
static int Ac3Check(const uint8_t * data, int size)
{
    int frame_size;

    if (size < 5) {			// need 5 bytes to see if AC-3/E-AC-3
	return -5;
    }

    if (data[5] > (10 << 3)) {		// E-AC-3
	if ((data[4] & 0xF0) == 0xF0) {	// invalid fscod fscod2
	    return 0;
	}
	frame_size = ((data[2] & 0x07) << 8) + data[3] + 1;
	frame_size *= 2;
    } else {				// AC-3
	int fscod;
	int frmsizcod;

	// crc1 crc1 fscod|frmsizcod
	fscod = data[4] >> 6;
	if (fscod == 0x03) {		// invalid sample rate
	    return 0;
	}
	frmsizcod = data[4] & 0x3F;
	if (frmsizcod > 37) {		// invalid frame size
	    return 0;
	}
	// invalid is checked above
	frame_size = Ac3FrameSizeTable[frmsizcod][fscod] * 2;
    }

    if (frame_size + 5 > size) {
	return -frame_size - 5;
    }
    // FIXME: relaxed checks if codec is already detected
    // check if after this frame a new AC-3 frame starts
    if (FastAc3Check(data + frame_size)) {
	return frame_size;
    }

    return 0;
}

///
///	Fast check for ADTS Audio Data Transport Stream.
///
///	7/9 bytes 0xFFFxxxxxxxxxxx(xxxx)  ADTS audio
///
static inline int FastAdtsCheck(const uint8_t * p)
{
    if (p[0] != 0xFF) {			// 12bit sync
	return 0;
    }
    if ((p[1] & 0xF6) != 0xF0) {	// sync + layer must be 0
	return 0;
    }
    if ((p[2] & 0x3C) == 0x3C) {	// sampling frequency index != 15
	return 0;
    }
    return 1;
}

///
///	Check for ADTS Audio Data Transport Stream.
///
///	0xFFF already checked.
///
///	@param data	incomplete PES packet
///	@param size	number of bytes
///
///	@retval <0	possible ADTS audio, but need more data
///	@retval 0	no valid ADTS audio
///	@retval >0	valid AC-3 audio
///
///	AAAAAAAA AAAABCCD EEFFFFGH HHIJKLMM MMMMMMMM MMMOOOOO OOOOOOPP
///	(QQQQQQQQ QQQQQQQ)
///
///	o A*12	syncword 0xFFF
///	o B*1	MPEG Version: 0 for MPEG-4, 1 for MPEG-2
///	o C*2	layer: always 0
///	o ..
///	o F*4	sampling frequency index (15 is invalid)
///	o ..
///	o M*13	frame length
///
static int AdtsCheck(const uint8_t * data, int size)
{
    int frame_size;

    if (size < 6) {
	return -6;
    }

    frame_size = (data[3] & 0x03) << 11;
    frame_size |= (data[4] & 0xFF) << 3;
    frame_size |= (data[5] & 0xE0) >> 5;

    if (frame_size + 3 > size) {
	return -frame_size - 3;
    }
    // check if after this frame a new ADTS frame starts
    if (FastAdtsCheck(data + frame_size)) {
	return frame_size;
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////////////
//	Video
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////

// helper functions to parse resolution from stream
const unsigned char * m_pStart;
unsigned short m_nLength;
int m_nCurrentBit;

unsigned int ReadBit()
{
	assert(m_nCurrentBit <= m_nLength * 8);
	int nIndex = m_nCurrentBit / 8;
	int nOffset = m_nCurrentBit % 8 + 1;

	m_nCurrentBit++;
	return (m_pStart[nIndex] >> (8-nOffset)) & 0x01;
}

unsigned int ReadBits(int n)
{
	int r = 0;

	for (int i = 0; i < n; i++) {
		r |= ( ReadBit() << ( n - i - 1 ) );
	}
	return r;
}

unsigned int ReadExponentialGolombCode()
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

unsigned int ReadSE()
{
	int r = ReadExponentialGolombCode();

	if (r & 0x01) {
		r = (r+1)/2;
	} else {
		r = -(r/2);
	}
	return r;
}

void ParseResolutionH264(int *width, int *height)
{
	AVPacket *avpkt;
	m_pStart = NULL;
	int i;

	while (!VideoGetPackets()) {
		usleep(10000);
	}

	avpkt = &MyVideoStream->PacketRb[MyVideoStream->PacketRead];

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
			m_pStart, avpkt, VideoGetPackets(), i);
		PrintStreamData(avpkt->data, avpkt->size);
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

/**
**	Initialize video packet ringbuffer.
**
**	@param stream	video stream
*/
static void VideoPacketInit(VideoStream * stream)
{
	for (int i = 0; i < VIDEO_PACKET_MAX; ++i) {
		AVPacket *avpkt;

		avpkt = &stream->PacketRb[i];
		if (av_new_packet(avpkt, VIDEO_BUFFER_SIZE)) {
			LOGFATAL("out of memory");
		}
		avpkt->size = 0;
	}

	atomic_set(&stream->PacketsFilled, 0);
	stream->PacketRead = 0;
	stream->PacketWrite = 0;
}

/**
**	Cleanup video packet ringbuffer.
**
**	@param stream	video stream
*/
static void VideoPacketExit(VideoStream * stream)
{
	atomic_set(&stream->PacketsFilled, 0);

	for (int i = 0; i < VIDEO_PACKET_MAX; ++i) {
		av_packet_unref(&stream->PacketRb[i]);
	}
}

/**
**	Place video data in packet ringbuffer.
**
**	@param stream	video stream
**	@param pts	presentation timestamp of pes packet
**	@param data	data of pes packet
**	@param size	size of pes packet
*/
static void VideoEnqueue(VideoStream * stream, int64_t pts, const void *data,
		int size)
{
	AVPacket *avpkt;

	avpkt = &stream->PacketRb[stream->PacketWrite];

	if (pts != AV_NOPTS_VALUE) {
		if (avpkt->size) {
			stream->PacketWrite = (stream->PacketWrite + 1) % VIDEO_PACKET_MAX;
			atomic_inc(&stream->PacketsFilled);
		}
		avpkt = &stream->PacketRb[stream->PacketWrite];
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
static void VideoStreamClose(VideoStream * stream)
{
	LOGDEBUG("VideoStreamClose:");
	if (stream->Decoder) {
		stream->Decoder->Close();
		delete(stream->Decoder);
		stream->Decoder = nullptr;
	}
	if (stream->Render) {
		VideoDelRender(stream->Render);
		stream->Render = NULL;
	}
	VideoPacketExit(stream);
}

/**
**	Clears all audio data from the decoder and ringbufffer.
*/
void ClearAudio(void)
{
	if (!SkipAudio) {
		LOGDEBUG("ClearAudio()");
		MyAudioDecoder->FlushBuffers();
		AudioFlushBuffers();
		NewAudioStream = 1;
	}
}

/**
**	Clears all video data from the device.
*/
void ClearVideo(VideoStream * stream)
{
	// ClearVideo does not come from Play() or SetPlayMode() (closing_stream_requested)
	// but from Clear() (or StillPicture) which is directly from VDR
	// Wait for the clear, until the decode thread finished a decoding process
	// The problem here is, that the reopen workaround deletes the VideoCtx for a moment,
	// which of course is a problem for VideoDecodeInput in the separate thread, because
	// that one wants VideoCtx....
	if (!stream->ClosingStream) {
		stream->StreamWait = 1;
		while (stream->StreamWait == 1) {
			// don't wait, if no thread is running, which should set stream->wait = 2
			// otherwise we run into an endless loop here
			if (!VideoDecodeThreadRunning())
				stream->StreamWait = 2;
			usleep(10000);
		}
	}

	AVPacket *avpkt;
	LOGDEBUG("ClearVideo() packets %d", atomic_read(&stream->PacketsFilled));
	pthread_mutex_lock(&stream->PktsLockMutex);
	atomic_set(&stream->PacketsFilled, 0);
	stream->PacketRead = stream->PacketWrite = 0;

	avpkt = &stream->PacketRb[stream->PacketWrite];
	avpkt->size = 0;
	avpkt->pts = AV_NOPTS_VALUE;

	if (stream->Render->HardwareQuirks & QUIRK_CODEC_FLUSH_WORKAROUND) {
		if (stream->Decoder->ReopenCodec(stream->CodecID, stream->Par, &stream->timebase, 0))
			LOGFATAL("ClearVideo: Could not reopen the decoder (flush buffers)!");
	} else {
		stream->Decoder->FlushBuffers();
	}
	pthread_mutex_unlock(&stream->PktsLockMutex);

	// no need to wait in decoding thread anymore
	stream->StreamWait = 0;
}

int closing_stream_requested(VideoStream *stream)
{
	if (stream->ClosingStream && stream->CodecID != AV_CODEC_ID_NONE) {
		pthread_mutex_lock(&stream->WaitCloseMutex);
		if (!MyVideoStream->TrickSpeed || !VideoGetTrickForward(stream->Render)) {
			ClearVideo(stream);
			stream->CodecID = AV_CODEC_ID_NONE;
			stream->Decoder->Close();
			stream->Par = NULL;
		}
		stream->ClosingStream = 0;
		pthread_cond_signal(&stream->WaitCloseCondition);
		pthread_mutex_unlock(&stream->WaitCloseMutex);
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
int VideoDecodeInput(VideoStream * stream)
{
	AVPacket *avpkt;
	AVFrame *frame;
	int ret = 0;
	static int sent = 0;

	if (closing_stream_requested(stream))
		return -1;

	if (stream->StreamWait) {
		stream->StreamWait = 2; // signalise, the turn has finished and we are waiting
		return -1;
	}

	if (MyVideoStream->StreamFreezed) {		// stream freezed
//		LOGINFO("VideoDecodeInput: stream->Freezed");
		// clear is called during freezed
		return 1;
	}

	if (stream->NewStream && stream->CodecID != AV_CODEC_ID_NONE) {
		if (stream->Decoder->Open(stream->CodecID, stream->Par, &stream->timebase, 0, 0, 0))
			LOGFATAL("VideoDecodeInput: Could not open the decoder!");
		stream->NewStream = 0;
	}

	if (stream->CodecID != AV_CODEC_ID_NONE) {
		pthread_mutex_lock(&stream->PktsLockMutex);
		// in trickspeed wait for minimum pkts needed to decode a frame
		int minpkts = (VideoGetTrickSpeed(stream->Render) && stream->interlaced) ? stream->trickpkts : 1;
		if (atomic_read(&stream->PacketsFilled) < minpkts) {
			pthread_mutex_unlock(&stream->PktsLockMutex);
			return -1;
		}
		avpkt = &stream->PacketRb[stream->PacketRead];

		// try sending packet to decoder
		ret = stream->Decoder->SendPacket(avpkt);
		if (ret != AVERROR(EAGAIN)) { // something went wrong or packet was sent, advance packet
			stream->PacketRead = (stream->PacketRead + 1) % VIDEO_PACKET_MAX;
			atomic_dec(&stream->PacketsFilled);
			// in backward trickspeed force the decoder to decode the frame
			if (ret == 0 && VideoGetTrickSpeed(stream->Render) && !VideoGetTrickForward(stream->Render)) {
				sent++;
				if (sent >= minpkts) {
					stream->Decoder->SendPacket(NULL);
					sent = 0;
				}
			}
		}

		pthread_mutex_unlock(&stream->PktsLockMutex);

// this is normal Playback
		if (!VideoGetTrickSpeed(stream->Render)) {
			if (!stream->NewStream) { // this is for mediaplayer ?
				if (!stream->Decoder->ReceiveFrame(0, &frame)) {
					while (VideoRenderFrame(stream->Render, stream->Decoder->GetContext(), frame, 0)) {
						if (closing_stream_requested(stream)) {
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
			ret = stream->Decoder->ReceiveFrame(1, &frame);
			if (ret == 0) {
				while (VideoGetTrickSpeed(stream->Render) && VideoGetTrickCounter(stream->Render) > 0) {
					AVFrame *trickframe = av_frame_clone(frame);
					if (!trickframe) {
						LOGERROR("VideoDecodeInput: could not clone frame");
						break;
					}
					LOGDEBUG2(L_TRICK, "VideoDecodeInput: Trickspeed, send another cloned trick frame %d %p", VideoGetTrickCounter(stream->Render), trickframe);
					while (VideoRenderFrame(stream->Render, stream->Decoder->GetContext(), trickframe, FRAME_FLAG_TRICKSPEED)) {
						if (closing_stream_requested(stream)) {
							av_frame_free(&trickframe);
							av_frame_free(&frame);
							sent = 0;
							return -1;
						}
					}
					VideoDecTrickCounter(stream->Render);
					if (closing_stream_requested(stream)) {
						av_frame_free(&frame);
						sent = 0;
						return -1;
					}
				}
				av_frame_free(&frame);
				sent = 0;

				int TrickSpeed = VideoGetTrickSpeed(stream->Render);
				VideoSetTrickCounter(stream->Render, TrickSpeed);

				goto receive_trickspeed; // try to get another frame
			} else if (ret == AVERROR_EOF) { // needs flush / reopen
				if (stream->Render->HardwareQuirks & QUIRK_CODEC_FLUSH_WORKAROUND) {
					if (stream->Decoder->ReopenCodec(stream->CodecID, stream->Par, &stream->timebase, 0))
						LOGFATAL("VideoDecodeInput: Could not reopen the decoder (flush buffers)!");
				} else {
					stream->Decoder->FlushBuffers();
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
int VideoGetPackets(void)
{
    return atomic_read(&MyVideoStream->PacketsFilled);
}


/**
**	Call rgb to jpeg for C Plugin.
*/
extern "C" uint8_t * CreateJpeg(uint8_t * image, int *size, int quality,
    int width, int height)
{
    return (uint8_t *) RgbToJpeg((uchar *) image, width, height, *size,
	quality);
}

#if defined(USE_JPEG) && JPEG_LIB_VERSION >= 80
/**
**	Create a jpeg image in memory.
**
**	@param image		raw RGB image
**	@param raw_size		size of raw image
**	@param size[out]	size of jpeg image
**	@param quality		jpeg quality
**	@param width		number of horizontal pixels in image
**	@param height		number of vertical pixels in image
**
**	@returns allocated jpeg image.
*/
uint8_t *CreateJpeg(uint8_t * image, int raw_size, int *size, int quality,
    int width, int height)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_ptr[1];
    int row_stride;
    uint8_t *outbuf;
    long unsigned int outsize;

    outbuf = NULL;
    outsize = 0;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &outbuf, &outsize);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = raw_size / height / width;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    row_stride = width * 3;
    while (cinfo.next_scanline < cinfo.image_height) {
	row_ptr[0] = &image[cinfo.next_scanline * row_stride];
	jpeg_write_scanlines(&cinfo, row_ptr, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    *size = outsize;

    return outbuf;
}
#endif

void *GetVideoRender()
{
	return (void *)(MyVideoStream->Render);
}

void SetInterlacedStream(int interlaced)
{
//	LOGDEBUG("SetInterlacedStream %d", interlaced);
	MyVideoStream->interlaced = interlaced;
}


//////////////////////////////////////////////////////////////////////////////
//	cDevice
//////////////////////////////////////////////////////////////////////////////

/**
**	Constructor device.
*/
cSoftHdDevice::cSoftHdDevice(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
    spuDecoder = NULL;
}

/**
**	Destructor device.
*/
cSoftHdDevice::~cSoftHdDevice(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
    delete spuDecoder;
}

/**
**	Prepare plugin.
*/
void cSoftHdDevice::Start(void)
{
    LOGDEBUG("Start(void):");
    if (!MyAudioDecoder) {
	AudioInit(AudioPassthrough);
	AudioSetBufferTime(ConfigAudioBufferTime);
	av_new_packet(AudioAvPkt, AUDIO_BUFFER_SIZE);
	MyAudioDecoder = new cAudioDecoder(AudioPassthrough);
	AudioCodecID = AV_CODEC_ID_NONE;
	AudioChannelID = -1;

	if (!MyVideoStream->Decoder) {
	    MyVideoStream->CodecID = AV_CODEC_ID_NONE;
	}

	if ((MyVideoStream->Render = VideoNewRender(MyVideoStream))) {
	    if (ConfigDisableOglOsd)
		VideoSetDisableOglOsd(MyVideoStream->Render);
	    VideoSetDisableDeint(MyVideoStream->Render, ConfigDisableDeint);
	    VideoInit(MyVideoStream->Render);
	    MyVideoStream->Decoder = new cVideoDecoder(MyVideoStream->Render);
	    VideoPacketInit(MyVideoStream);
	}
    }
}

/**
**	Stop plugin.
**
**	@note stop everything, but don't cleanup, module is still called.
*/
void cSoftHdDevice::Stop(void)
{
	LOGDEBUG("Stop(void): nothing to do.");
}

/**
**	Exit + cleanup.
*/
void cSoftHdDevice::Exit(void)
{
    LOGDEBUG("SoftHdDeviceExit(void):");
    AudioExit();
    if (MyAudioDecoder) {
	MyAudioDecoder->Close();
	delete MyAudioDecoder;
    }
    NewAudioStream = 0;
    av_packet_unref(AudioAvPkt);

    VideoExit(MyVideoStream->Render);
    VideoStreamClose(MyVideoStream);
}

/**
**	Informs a device that it will be the primary device.
**
**	@param on	flag if becoming or loosing primary
*/
void cSoftHdDevice::MakePrimaryDevice(bool on)
{
	LOGDEBUG("%s: %d", __FUNCTION__, on);
	if (!on) {
		Exit();
	} else {
		Start();
	}

	cDevice::MakePrimaryDevice(on);
	if (on) {
		new cSoftOsdProvider(this);
	}
}

/**
**	Get the device SPU decoder.
**
**	@returns a pointer to the device's SPU decoder (or NULL, if this
**	device doesn't have an SPU decoder)
*/
cSpuDecoder *cSoftHdDevice::GetSpuDecoder(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
    if (!spuDecoder && IsPrimaryDevice()) {
	spuDecoder = new cDvbSpuDecoder();
    }
    return spuDecoder;
}

/**
**	Tells whether this device has a MPEG decoder.
*/
bool cSoftHdDevice::HasDecoder(void) const
{
    return true;
}

/**
**	Returns true if this device can currently start a replay session.
*/
bool cSoftHdDevice::CanReplay(void) const
{
    LOGDEBUG("%s:", __FUNCTION__);
    return true;
}

/**
**	Sets the device into the given play mode.
**
**	@param play_mode	new play mode (Audio/Video/External...)
*/
bool cSoftHdDevice::SetPlayMode(ePlayMode play_mode)
{
	LOGDEBUG("SetPlayMode: play_mode %d", play_mode);
	LOGDEBUG("%s: %d", __FUNCTION__, play_mode);

	switch (play_mode) {
	case 0:			// none audio/video
		MyVideoStream->TrickSpeed = 0;
		if (MyVideoStream->CodecID != AV_CODEC_ID_NONE) {
			pthread_mutex_lock(&MyVideoStream->WaitCloseMutex);
			MyVideoStream->ClosingStream = 1;
			while (MyVideoStream->ClosingStream)
				pthread_cond_wait(&MyVideoStream->WaitCloseCondition, &MyVideoStream->WaitCloseMutex);
			pthread_mutex_unlock(&MyVideoStream->WaitCloseMutex);
		}
		VideoSetClosing(MyVideoStream->Render, 1);
		SkipAudio = 0;
		AudioPlay();
		ClearAudio();	// flush all AUDIO buffers
		if (MyAudioDecoder && AudioCodecID != AV_CODEC_ID_NONE) {
			NewAudioStream = 1;
		}
		MyVideoStream->interlaced = 0; // probably not necessary
		MyVideoStream->StreamFreezed = 0;
		break;
	case 1:			// audio/video
		VideoThreadWakeup(MyVideoStream->Render, 1, 1);
		//Play(); Play is a vdr command!!!
		break;
	case 2:			// audio only
		VideoThreadExit();
		break;
	case 3:			// audio only (black screen)
		LOGDEBUG("softhddev: FIXME: audio only, silence video errors");
		VideoThreadWakeup(MyVideoStream->Render, 1, 1);
		//Play();
		break;
	case 4:			// video only
		VideoThreadWakeup(MyVideoStream->Render, 1, 1);
		//Play();
		break;
	default:
		LOGERROR("SetPlayMode: playmode not supported %d", play_mode);
		return 0;
		break;
	}

	return 1;
}

/**
**	Gets the current System Time Counter, which can be used to
**	synchronize audio, video and subtitles.
*/
int64_t cSoftHdDevice::GetSTC(void)
{
//    LOGDEBUG("%s:", __FUNCTION__);
    if (MyVideoStream->Render) {
	return VideoGetClock(MyVideoStream->Render);
    }
    // could happen during dettached
    LOGWARNING("softhddev: %s called without hw decoder", __FUNCTION__);
    return AV_NOPTS_VALUE;
}

/**
**	Set trick play speed.
**
**	Every single frame shall then be displayed the given number of
**	times.
**
**	@param speed	trick speed
**	@param forward	flag forward direction
*/
void cSoftHdDevice::TrickSpeed(int speed, bool forward)
{
    LOGDEBUG("%s: %d %d", __FUNCTION__, speed, forward);
    LOGDEBUG("TrickSpeed: speed %d %s",
		speed, forward ? "forward" : "backward");

    LOGDEBUG("TrickSpeed: speed %d %s, trigger new trickspeed", speed, forward ? "forward" : "backward");

    VideoSetClosing(MyVideoStream->Render, 0);
    if (MyVideoStream->StreamFreezed) {
	LOGDEBUG("TrickSpeed: StreamFreezed %d SkipAudio %d", MyVideoStream->StreamFreezed, SkipAudio);
	MyVideoStream->StreamFreezed = 0;
	ClearAudio();
	SkipAudio = 0;
    }

    MyVideoStream->TrickSpeed = 1;
    VideoTrickSpeed(MyVideoStream->Render, speed, forward);
}

/**
**	Clears all video and audio data from the device.
*/
void cSoftHdDevice::Clear(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
    cDevice::Clear();

    LOGDEBUG("Clear(void)");
    ClearVideo(MyVideoStream);
    VideoSetClosing(MyVideoStream->Render, 0);
    ClearAudio();
}

/**
**	Sets the device into play mode (after a previous trick mode)
*/
void cSoftHdDevice::Play(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
    cDevice::Play();

    LOGDEBUG("Play(void)");
    if (MyVideoStream->TrickSpeed && MyVideoStream->CodecID != AV_CODEC_ID_NONE) {
	pthread_mutex_lock(&MyVideoStream->WaitCloseMutex);
	MyVideoStream->ClosingStream = 1;
	while (MyVideoStream->ClosingStream)
	    pthread_cond_wait(&MyVideoStream->WaitCloseCondition, &MyVideoStream->WaitCloseMutex);
	pthread_mutex_unlock(&MyVideoStream->WaitCloseMutex);
	VideoSetClosing(MyVideoStream->Render, 0);
    }
    MyVideoStream->TrickSpeed = 0;
    SkipAudio = 0;
    MyVideoStream->StreamFreezed = 0;
    AudioPlay();
    VideoPlay(MyVideoStream->Render);
}

/**
**	Puts the device into "freeze frame" mode.
*/
void cSoftHdDevice::Freeze(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
    cDevice::Freeze();

    LOGDEBUG("Freeze(void)");
    MyVideoStream->StreamFreezed = 1;
    AudioPause();
    VideoPause(MyVideoStream->Render);
}

/**
**	Turns off audio while replaying.
*/
void cSoftHdDevice::Mute(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
    cDevice::Mute();

    LOGDEBUG("Mute(void)");
    SkipAudio = 1;
}

/**
**	Display the given I-frame as a still picture.
**
**	@param data	pes or ts data of a frame
**	@param length	length of data area
*/
void cSoftHdDevice::StillPicture(const uchar * data, int size)
{
    if (data[0] == 0x47) {		// ts sync
	cDevice::StillPicture(data, size);
	return;
    }

    LOGDEBUG("%s: %s %p %d", __FUNCTION__,
	data[0] == 0x47 ? "ts" : "pes", data, size);

    LOGDEBUG2(L_STILL, "StillPicture");
    AVPacket *avpkt;
    AVFrame *frame;

    const uchar * pos;
    int size_rest;
    enum AVCodecID codec = AV_CODEC_ID_NONE;
    int i;
    int pes_length;
    int head_length;
    int context = 0;

    avpkt = &MyVideoStream->PacketRb[MyVideoStream->PacketWrite];
    avpkt->size = 0;
    avpkt->pts = AV_NOPTS_VALUE;
    pos = data;
    size_rest = size;

    while (size_rest >= 6 ) {
	if (pos[3] >> 4 == 0x0e) {	// PES video start code
	    pes_length = PesHasLength(pos) ? PesLength(pos) : size;
	    head_length = PesHeadLength(pos);
	} else {	// ES video start code
	    pes_length = size;
	    head_length = 0;
	}

	if (codec == AV_CODEC_ID_NONE) {
	    for (i = 0; (i < 2); i++) {
		// ES start code 0x00 0x00 0x01
		if (!pos[i + head_length] && !pos[i + head_length + 1] &&
		    pos[i + head_length + 2] == 0x01) {

		    // AV_CODEC_ID_MPEG2VIDEO 0x00 0x00 0x01 0xb3
		    if (pos[i + head_length + 3] == 0xb3) {
			codec = AV_CODEC_ID_MPEG2VIDEO;
			break;
		    }
		    // AV_CODEC_ID_H264 0x00 0x00 0x01 0x09
		    if (pos[i + head_length + 3] == 0x09) {
			codec = AV_CODEC_ID_H264;
			break;
		    }
		    // AV_CODEC_ID_HEVC 0x00 0x00 0x01 0x46
		    if (pos[i + head_length + 3] == 0x46) {
			codec = AV_CODEC_ID_HEVC;
			break;
		    }
		}
	    }
	}

	LOGDEBUG2(L_STILL, "StillPicture: memcpy avpkt.size %d size %d size_rest %d peslength %d headlength %d I %d",
	    avpkt->size, size, size_rest, pes_length, head_length, i);
	if ((size_t)(avpkt->size + pes_length - head_length - i) >= avpkt->buf->size) {
	    int pkt_size = avpkt->size;
	    LOGWARNING("video: packet buffer too small for %d",
		avpkt->size + pes_length - head_length - i);
	    av_grow_packet(avpkt, pes_length - head_length - i);
	    avpkt->size = pkt_size;
	}

	memcpy(avpkt->data + avpkt->size, pos + head_length + i, pes_length - head_length - i);
	avpkt->size += pes_length - head_length - i;
	size_rest -= pes_length;
	pos += pes_length;
	i = 0;
    }
    atomic_inc(&MyVideoStream->PacketsFilled);

    MyVideoStream->StreamFreezed = 1;
    if (MyVideoStream->Decoder->GetContext()) {
	if ((int)(MyVideoStream->Decoder->GetContext()->codec_id) != codec) {
	    MyVideoStream->Decoder->Close();
	}
    }
    if (!MyVideoStream->Decoder->GetContext()) {
	if (MyVideoStream->Decoder->Open(codec, NULL, NULL, 0, 0, 0))
	    LOGFATAL("StillPicture: Could not open the decoder!");
	context = 1;
    }
    AudioPause();

    int ret = 0;
    ret = MyVideoStream->Decoder->SendPacket(avpkt);
    if (ret)
	LOGDEBUG2(L_STILL, "StillPicture: SendPacket(avpkt) returned %d", ret);
    else
	LOGDEBUG2(L_STILL, "StillPicture: avpkt sent");

    // force decoder to enter draining because we only want 1 avpkt to be decoded
    MyVideoStream->Decoder->SendPacket(NULL);

receive:
    ret = MyVideoStream->Decoder->ReceiveFrame(1, &frame);
    if (!ret) {
	// frame received, render it and try another one (should end up with AVERROR_EOF)
	LOGDEBUG2(L_STILL, "StillPicture: frame received");
	while (VideoRenderFrame(MyVideoStream->Render, MyVideoStream->Decoder->GetContext(), frame, FRAME_FLAG_STILLPICTURE)) {
	    if (MyVideoStream->ClosingStream) {
		av_frame_free(&frame);
		break;
	    }
	}
	goto receive;
    } else if (ret == AVERROR_EOF) {
	// AVERROR_EOF, flush needed
	if (MyVideoStream->Render->HardwareQuirks & QUIRK_CODEC_FLUSH_WORKAROUND) {
	    if (MyVideoStream->Decoder->ReopenCodec(codec, NULL, NULL, 0))
		LOGFATAL("StillPicture: Could not reopen the decoder (flush buffers)!");
	} else {
	    MyVideoStream->Decoder->FlushBuffers();
	}
    } else {
	// sth went wrong or AVERROR(EAGAIN)
	LOGDEBUG2(L_STILL, "StillPicture: Receive Frame returned %d, should not happen!", ret);
    }

    if (context) {
	MyVideoStream->Decoder->Close();
	MyVideoStream->CodecID = AV_CODEC_ID_NONE;
    }
    ClearAudio();
    ClearVideo(MyVideoStream);
    MyVideoStream->StreamFreezed = 0;
    AudioPlay();
}

/**
**	Check if the device is ready for further action.
**
**	This function is useless, the return value is ignored and
**	all buffers are overrun by vdr.
**
**	The dvd plugin is using this correct.
**
**	@param poller		file handles (unused)
**	@param timeout_ms	timeout in ms to become ready
**
**	@retval true	if ready
**	@retval false	if busy
*/
bool cSoftHdDevice::Poll(
    __attribute__ ((unused)) cPoller & poller, int timeout)
{
    //LOGDEBUG("%s: timeout %d", __FUNCTION__, timeout_ms);

    for (;;) {
	int full;
	int t;
	int used;
	int filled;

//	LOGDEBUG("Poll: timeout %d", timeout);

	used = AudioUsedBytes();
	// FIXME: no video!
	filled = atomic_read(&MyVideoStream->PacketsFilled);
	// soft limit + hard limit
	full = (used > AUDIO_MIN_BUFFER_FREE && filled > 3)
	    || AudioFreeBytes() < AUDIO_MIN_BUFFER_FREE
	    || filled >= VIDEO_PACKET_MAX - 10;

	if (!full || !timeout) {
	    return !full;
	}

	t = 15;
	if (timeout < t) {
	    t = timeout;
	}
	usleep(t * 1000);		// let display thread work
	timeout -= t;
    }
}

/**
**	Flush the device output buffers.
**
**	@param timeout_ms	timeout in ms to become ready
*/
bool cSoftHdDevice::Flush(int timeout)
{
    LOGDEBUG("%s: %d ms", __FUNCTION__, timeout);

    LOGDEBUG("Flush: timeout %d", timeout);
    if (atomic_read(&MyVideoStream->PacketsFilled)) {
	if (timeout) {			// let display thread work
	    usleep(timeout * 1000);
	}
	return !atomic_read(&MyVideoStream->PacketsFilled);
    }
    return 1;
}

// ----------------------------------------------------------------------------

/**
**	Sets the video display format to the given one (only useful if this
**	device has an MPEG decoder).
*/
void cSoftHdDevice::SetVideoDisplayFormat(eVideoDisplayFormat
    video_display_format)
{
    LOGDEBUG("%s: %d", __FUNCTION__, video_display_format);

    cDevice::SetVideoDisplayFormat(video_display_format);
}

/**
**	Sets the output video format to either 16:9 or 4:3 (only useful
**	if this device has an MPEG decoder).
**
**	Should call SetVideoDisplayFormat.
**
**	@param video_format16_9	flag true 16:9.
*/
void cSoftHdDevice::SetVideoFormat(bool video_format16_9)
{
    LOGDEBUG("%s: %d", __FUNCTION__, video_format16_9);

    // FIXME: 4:3 / 16:9 video format not supported.

    SetVideoDisplayFormat(eVideoDisplayFormat(Setup.VideoDisplayFormat));
}

/**
**	Returns the width, height and video_aspect ratio of the currently
**	displayed video material.
**
**	@note the video_aspect is used to scale the subtitle.
*/
void cSoftHdDevice::GetVideoSize(int &width, int &height, double &aspect_ratio)
{
    MyVideoStream->Decoder->GetVideoSize(&width, &height, &aspect_ratio);
//	LOGDEBUG("GetVideoSize: %d x %d @ %f", *width, *height, *aspect_ratio);
}

/**
**	Returns the width, height and pixel_aspect ratio the OSD.
**
**	FIXME: Called every second, for nothing (no OSD displayed)?
*/
void cSoftHdDevice::GetOsdSize(int &width, int &height, double &pixel_aspect)
{
    VideoGetScreenSize(MyVideoStream->Render, &width, &height, &pixel_aspect);
}

// ----------------------------------------------------------------------------

/**
**	Play a audio packet.
**
**	@param data	data of exactly one complete PES packet
**	@param size	size of PES packet
**	@param id	PES packet type
*/
int cSoftHdDevice::PlayAudio(const uchar *data, int size, uchar id)
{
    //LOGDEBUG("%s: %p %p %d %d", __FUNCTION__, this, data, length, id);

    int n;
    const uint8_t *p;
    AVRational timebase;
    timebase.den = 90000;
    timebase.num = 1;

    AudioAvPkt->pts = AV_NOPTS_VALUE;

    if (MyVideoStream->StreamFreezed) {	// stream is freezed, don't accept new audio data
	LOGDEBUG("PlayAudio: StreamFreezed");
	return 0;
    }

    if (SkipAudio) {	// skip audio
	LOGDEBUG("PlayAudio: skip audio");
	return size;
    }

    // hard limit buffer full: don't overrun audio buffers on replay
    // stream freezed
    if (AudioFreeBytes() < AUDIO_MIN_BUFFER_FREE){
//	LOGDEBUG("PlayAudio: Buffer is Full (%d|%d)!", AudioFreeBytes(), AUDIO_MIN_BUFFER_FREE);
	return 0;
    }

    if (NewAudioStream) {
	// this clears the audio ringbuffer indirect, open and setup does it
	LOGDEBUG("PlayAudio: NewAudioStream");
	MyAudioDecoder->Close();
//	AudioFlushBuffers();
//	AudioSetBufferTime(ConfigAudioBufferTime);		// ???
	AudioCodecID = AV_CODEC_ID_NONE;
	AudioChannelID = -1;
	NewAudioStream = 0;
    }
    // PES header 0x00 0x00 0x01 ID
    // ID 0xBD 0xC0-0xCF
    // must be a PES start code
    if (size < 9 || !data || data[0] || data[1] || data[2] != 0x01) {
	LOGERROR("invalid PES audio packet");
	return size;
    }
    n = data[8];			// header size

    if (size < 9 + n + 4) {		// wrong size
	if (size == 9 + n) {
	    LOGWARNING("empty audio packet");
	} else {
	    LOGERROR("invalid audio packet %d bytes", size);
	}
	LOGINFO("PlayAudio: wrong size");
	return size;
    }

    if (data[7] & 0x80 && n >= 5) {
	AudioAvPkt->pts =
	    (int64_t) (data[9] & 0x0E) << 29 | data[10] << 22 | (data[11] &
	    0xFE) << 14 | data[12] << 7 | (data[13] & 0xFE) >> 1;
	//LOGDEBUG("audio: pts %#012" PRIx64 "\n", AudioAvPkt->pts);
    } else {
	LOGINFO("PlayAudio: No PTS!");
    }

    p = data + 9 + n;
    n = size - 9 - n;			// skip pes header
    if (n + AudioAvPkt->stream_index > AudioAvPkt->size) {
	LOGERROR("audio buffer too small needed %d avail %d",
	    n + AudioAvPkt->stream_index, AudioAvPkt->size);
	AudioAvPkt->stream_index = 0;
    }

    if (AudioChannelID != id) {		// id changed audio track changed
	AudioChannelID = id;
	AudioCodecID = AV_CODEC_ID_NONE;
	LOGDEBUG("audio/demux: new channel id");
    }
    // Private stream + LPCM ID
    if ((id & 0xF0) == 0xA0) {
	if (n < 7) {
	    LOGERROR("invalid LPCM audio packet %d bytes", size);
	    return size;
	}
/*	if (AudioCodecID != AV_CODEC_ID_PCM_DVD) {
	    static int samplerates[] = { 48000, 96000, 44100, 32000 };
	    int samplerate;
	    int channels;
	    int bits_per_sample;

	    LOGDEBUG("%s: LPCM %d sr:%d bits:%d chan:%d\n",
		__FUNCTION__, id, p[5] >> 4, (((p[5] >> 6) & 0x3) + 4) * 4,
		(p[5] & 0x7) + 1);
	    MyAudioDecoder->Close();

	    bits_per_sample = (((p[5] >> 6) & 0x3) + 4) * 4;
	    if (bits_per_sample != 16) {
		LOGERROR(_
		    ("LPCM %d bits per sample aren't supported"),
		    bits_per_sample);
		// FIXME: handle unsupported formats.
	    }
	    samplerate = samplerates[p[5] >> 4];
	    channels = (p[5] & 0x7) + 1;

	    // FIXME: ConfigAudioBufferTime + x
//	    AudioSetBufferTime(400);
//	    AudioSetup(&samplerate, &channels, 0);
	    if (samplerate != samplerates[p[5] >> 4]) {
		LOGERROR("LPCM %d sample-rate is unsupported",
		    samplerates[p[5] >> 4]);
		// FIXME: support resample
	    }
	    if (channels != (p[5] & 0x7) + 1) {
		LOGERROR("LPCM %d channels are unsupported",
		    (p[5] & 0x7) + 1);
		// FIXME: support resample
	    }
	    //MyAudioDecoder->Open(AV_CODEC_ID_PCM_DVD);
	    AudioCodecID = AV_CODEC_ID_PCM_DVD;
	}

	if (AudioAvPkt->pts != (int64_t) AV_NOPTS_VALUE) {
//	    AudioSetClock(AudioAvPkt->pts);
	    AudioAvPkt->pts = AV_NOPTS_VALUE;
	}
	swab(p + 7, AudioAvPkt->data, n - 7);
	Audiofilter(AudioAvPkt->data, n - 7, NULL);		// Das muss in ein AVFrame gepackt werden!!!
*/
	return size;
    }
    // DVD track header
    if ((id & 0xF0) == 0x80 && (p[0] & 0xF0) == 0x80) {
	p += 4;
	n -= 4;				// skip track header
/*
	if (AudioCodecID == AV_CODEC_ID_NONE) {
	    // FIXME: ConfigAudioBufferTime + x
	    AudioSetBufferTime(400);
	}
*/
    }
    // append new packet, to partial old data
    memcpy(AudioAvPkt->data + AudioAvPkt->stream_index, p, n);
    AudioAvPkt->stream_index += n;

    n = AudioAvPkt->stream_index;
    p = AudioAvPkt->data;
    while (n >= 5) {
	int r;
	enum AVCodecID codec_id;

	// 4 bytes 0xFFExxxxx Mpeg audio
	// 3 bytes 0x56Exxx AAC LATM audio
	// 5 bytes 0x0B77xxxxxx AC-3 audio
	// 6 bytes 0x0B77xxxxxxxx E-AC-3 audio
	// 7/9 bytes 0xFFFxxxxxxxxxxx ADTS audio
	// PCM audio can't be found
	r = 0;
	codec_id = AV_CODEC_ID_NONE;	// keep compiler happy
	if (id != 0xbd && FastMpegCheck(p)) {
	    r = MpegCheck(p, n);
	    codec_id = AV_CODEC_ID_MP2;
	}
	if (id != 0xbd && !r && FastLatmCheck(p)) {
	    r = LatmCheck(p, n);
	    codec_id = AV_CODEC_ID_AAC_LATM;
	}
	if ((id == 0xbd || (id & 0xF0) == 0x80) && !r && FastAc3Check(p)) {
	    r = Ac3Check(p, n);
	    codec_id = AV_CODEC_ID_AC3;
	    if (r > 0 && p[5] > (10 << 3)) {
		codec_id = AV_CODEC_ID_EAC3;
	    }
	    /* faster ac3 detection at end of pes packet (no improvemnts)
	    if (AudioCodecID == codec_id && -r - 2 == n) {
		r = n;
	    }
	    */
	}
	if (id != 0xbd && !r && FastAdtsCheck(p)) {
	    r = AdtsCheck(p, n);
	    codec_id = AV_CODEC_ID_AAC;
	}
	if (r < 0) {			// need more bytes
	    break;
	}
	if (r > 0) {
	    AVPacket *avpkt;

	    // new codec id, close and open new
	    if (AudioCodecID != codec_id) {
		MyAudioDecoder->Close();
		MyAudioDecoder->Open(codec_id, NULL, &timebase);
		AudioCodecID = codec_id;
	    }
	    avpkt = av_packet_alloc();
	    if (avpkt == NULL) {
		LOGERROR("avpkt allocation failed");
		continue;
	    };
	    avpkt->data = (uint8_t *)p;
	    avpkt->size = r;
	    avpkt->pts = AudioAvPkt->pts;
	    MyAudioDecoder->Decode(avpkt);
	    AudioAvPkt->pts = AV_NOPTS_VALUE;
	    av_packet_free(&avpkt);
	    p += r;
	    n -= r;
	    continue;
	}
	++p;
	--n;
    }

    // copy remaining bytes to start of packet
    if (n) {
	memmove(AudioAvPkt->data, p, n);
    }
    AudioAvPkt->stream_index = n;

    return size;
}

void cSoftHdDevice::SetAudioTrackDevice(
    __attribute__ ((unused)) eTrackType type)
{
    //LOGDEBUG("%s:", __FUNCTION__);
}

void cSoftHdDevice::SetDigitalAudioDevice( __attribute__ ((unused)) bool on)
{
    //LOGDEBUG("%s: %s", __FUNCTION__, on ? "true" : "false");
}

void cSoftHdDevice::SetAudioChannelDevice( __attribute__ ((unused))
    int audio_channel)
{
    //LOGDEBUG("%s: %d", __FUNCTION__, audio_channel);
}

int cSoftHdDevice::GetAudioChannelDevice(void)
{
    //LOGDEBUG("%s:", __FUNCTION__);
    return 0;
}

/**
**	Sets the audio volume on this device (Volume = 0...255).
**
**	@param volume	device volume
*/
void cSoftHdDevice::SetVolumeDevice(int volume)
{
    LOGDEBUG("%s: %d", __FUNCTION__, volume);

    AudioSetVolume((volume * 1000) / 255);
}

/**
**	Read the PES header length from PES header.
**
**	@returns length
*/
int cSoftHdDevice::PesHeadLength(const uint8_t *p)
{
  return 9 + p[8];
}

/**
**	Play a video packet.
**
**	@param data	exactly one complete PES packet (which is incomplete)
**	@param size	length of PES packet
*/
int cSoftHdDevice::PlayVideo(const uchar * data, int size)
{
    //LOGDEBUG("%s: %p %d", __FUNCTION__, data, length);

    VideoStream * stream = MyVideoStream;
    int64_t pts = AV_NOPTS_VALUE;
    int i, n;

    if (MyVideoStream->StreamFreezed) {
	return 0;
    }

    // must be a PES video start code
    if (size < 9 || !data || data[0] || data[1] || data[2] != 0x01 || data[3] >> 4 != 0x0e) {
	return size;
    }

    // hard limit buffer full: needed for replay
    if (atomic_read(&stream->PacketsFilled) >= VIDEO_PACKET_MAX - 10) {
	return 0;
    }

    // get pts
    if (data[7] & 0x80) {
	pts = (int64_t) (data[9] & 0x0E) << 29 | data[10] << 22 | (data[11] &
	       0xFE) << 14 | data[12] << 7 | (data[13] & 0xFE) >> 1;
    }

    n = PesHeadLength(data);	// PES header size

    for (i = 0; (i < 2) && (i + 4 < size); i++) {
	// ES start code 0x00 0x00 0x01
	if (!data[i + n] && !data[i + n + 1] && data[i + n + 2] == 0x01) {
	    if (stream->CodecID == AV_CODEC_ID_NONE) {
		if (data[i + n + 3] == 0xb3) {
		    // MPEG2 I-Frame
		    LOGDEBUG("PlayVideo: mpeg2 detected");
		    LOGDEBUG2(L_CODEC, "video: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x",
		           data[i + n],
		           data[i + n + 1],
		           data[i + n + 2],
		           data[i + n + 3],
		           data[i + n + 4],
		           data[i + n + 5],
		           data[i + n + 6],
		           data[i + n + 7],
		           data[i + n + 8],
			   data[i + n + 9],
			   data[i + n + 10]);
		    stream->CodecID = AV_CODEC_ID_MPEG2VIDEO;
		    stream->trickpkts = 1;
		    goto newstream;
		} else if (data[i + n + 3] == 0x09 && (data[i + n + 4] == 0x10 || data[i + n + 4] == 0xF0 || data[i + n + 10] == 0x64)) {
		    // H264 I-Frame
		    LOGDEBUG("PlayVideo: H264 detected");
		    LOGDEBUG2(L_CODEC, "video: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x",
		           data[i + n],
		           data[i + n + 1],
		           data[i + n + 2],
		           data[i + n + 3],
		           data[i + n + 4],
		           data[i + n + 5],
		           data[i + n + 6],
		           data[i + n + 7],
		           data[i + n + 8],
		           data[i + n + 9],
		           data[i + n + 10]);
		    stream->CodecID = AV_CODEC_ID_H264;
		    stream->trickpkts = 2;
		    goto newstream;
		} else if (data[i + n + 3] == 0x46 && (data[i + n + 5] == 0x10 || data[i + n + 5] == 0x50 || data[i + n + 10] == 0x40)) {
		    // HEVC I-Frame
		    LOGDEBUG("PlayVideo: hevc detected");
		    LOGDEBUG2(L_CODEC, "video: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x",
		           data[i + n],
		           data[i + n + 1],
		           data[i + n + 2],
		           data[i + n + 3],
		           data[i + n + 4],
		           data[i + n + 5],
		           data[i + n + 6],
		           data[i + n + 7],
		           data[i + n + 8],
		           data[i + n + 9],
		           data[i + n + 10]);
		    stream->CodecID = AV_CODEC_ID_HEVC;
		    stream->trickpkts = 2;
newstream:
		    stream->NewStream = 1;
		    stream->timebase.den = 90000;
		    stream->timebase.num = 1;
		    VideoEnqueue(stream, pts, data + i + n, size - i - n);
		}
	    } else {
		VideoEnqueue(stream, pts, data + i + n, size - i - n);
	    }
	    return size;
	}
    }

    // this happens when vdr sends incomplete packets and no stream is started
    if (stream->CodecID == AV_CODEC_ID_NONE) {
	return size;
    }

    // complete last frame
    VideoEnqueue(stream, pts, data + n, size - n);
    return size;
}

/**
**	Grabs the currently visible screen image.
**
**	@param size	size of the returned data
**	@param jpeg	flag true, create JPEG data
**	@param quality	JPEG quality
**	@param width	number of horizontal pixels in the frame
**	@param height	number of vertical pixels in the frame
*/
uchar *cSoftHdDevice::GrabImage(int &size, bool jpeg, int quality, int width,
    int height)
{
    LOGDEBUG("%s: %d, %d, %d, %dx%d", __FUNCTION__, size, jpeg,
	quality, width, height);

    if (!width || !height) {
	LOGERROR("%s: Width or height must be not 0!", __FUNCTION__);
	return NULL;
    }

    if (quality < 0) {			// caller should care, but fix it
	quality = 95;
    }

    LOGDEBUG2(L_GRAB, "GrabImage: Start grabbing");
    if (VideoGrabInWork(MyVideoStream->Render)) {
	LOGDEBUG2(L_GRAB, "GrabImage: waiting for last grab...");
	return NULL;
    }

    VideoTriggerGrab(MyVideoStream->Render);

    int timeout = 100;
    while(!VideoGrabReady(MyVideoStream->Render)) {
	usleep(10000);
	if (timeout-- <= 0) {
	    // TODO: This is not safe! It can occur when we get stuck in Frame2Display,
	    // if no OSD or video frames are filled. Reset Grabbing after a timeout.
	    LOGDEBUG2(L_GRAB, "GrabImage: Timeout!");
	    VideoClearGrab(MyVideoStream->Render);
	    return NULL;
	}
    }

    // get screen dimensions
    int screenwidth;
    int screenheight;
    double pixel_aspect;
    VideoGetScreenSize(MyVideoStream->Render, &screenwidth, &screenheight, &pixel_aspect);
    int screensize = screenwidth * screenheight * 3; // we want a RGB24

    // set grab dimensions
    int grabwidth = width > 0 ? width : screenwidth;
    int grabheight = height > 0 ? height : screenheight;

    int video_size = 0;			// data size of the grabbed video
    int video_width = screenwidth;	// width of the grabbed video
    int video_height = screenheight;	// height of the grabbed video
    int video_x = 0, video_y = 0;	// x, y of the grabbed video

    // Video comes as RGB, width and height is original screen dimension (video is maybe scaled)
    uint8_t *video = VideoGetGrab(MyVideoStream->Render, &video_size, &video_width, &video_height, &video_x, &video_y, 0);
    if (!video) {
        LOGDEBUG2(L_GRAB, "GrabImage: video is NULL, create black screen!");
        video = (uint8_t *)calloc(1, screensize);
    }

    // OSD comes as ARGB, width and height is original screen dimension (osd is always fullscreen)
    uint8_t *osd = VideoGetGrab(MyVideoStream->Render, NULL, NULL, NULL, NULL, NULL, 1);
    if (!osd)
        LOGDEBUG2(L_GRAB, "GrabImage: osd is NULL, skip it");

    // first step: blit the video into a full black screen if scaled
    uint8_t *scaledvideo;
    if (video_width != screenwidth || video_height != screenheight || video_x != 0 || video_y != 0) {
        scaledvideo = blitvideo(video, screenwidth, screenheight, video_x, video_y, video_width, video_height);
        free(video);
    } else {
        scaledvideo = video;
    }

    // second step: alphablend fullscreen video with osd if available
    uint8_t *result;
    if (!osd) {
        result = scaledvideo;
    } else {
        result = (uint8_t *)malloc(screensize);
        alphablend(result, osd, scaledvideo, screenwidth, screenheight);
        free(scaledvideo);
        free(osd);
    }

    // need to scale result to requested size width + height, if it differs from fullscreen
    int scaledsize = screensize;
    uint8_t *scaledresult;
    if (screenwidth != grabwidth || screenheight != grabheight) {
        scaledresult = scalergb24(result, &scaledsize, screenwidth, screenheight, grabwidth, grabheight);
        free(result);
    } else {
        scaledresult = result;
    }

    // make jpeg or pnm
    uint8_t *grabbedimage;
    if (jpeg) {
        grabbedimage = CreateJpeg(scaledresult, &size, quality, grabwidth, grabheight);
    } else {  // add header to raw data
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255\n", grabwidth, grabheight);
        grabbedimage = (uint8_t *)malloc(scaledsize + n);
        memcpy(grabbedimage, buf, n);
        memcpy(grabbedimage + n, scaledresult, scaledsize);
        size = scaledsize + n;
    }
    free(scaledresult);
    LOGDEBUG2(L_GRAB, "GrabImage: finished %s image (%dx%d, quality %d) at %p (size %d)", jpeg ? "jpg" : "pnm", grabwidth, grabheight, jpeg ? quality : 0, grabbedimage, size);
    VideoClearGrab(MyVideoStream->Render);
    return grabbedimage;
}

/**
**	Ask the output, if it can scale video.
**
**	@param rect	requested video window rectangle
**
**	@returns	the real rectangle or cRect::NULL if invalid
*/
cRect cSoftHdDevice::CanScaleVideo(const cRect & rect, __attribute__ ((unused)) int alignment)
{
    return rect;
}

/**
**	Scale the currently shown video.
**
**	@param x	video window x coordinate OSD relative
**	@param y	video window x coordinate OSD relative
**	@param width	video window width OSD relative
**	@param height	video window height OSD relative
*/
void cSoftHdDevice::ScaleVideo(const cRect & rect)
{
    LOGDEBUG2(L_OSD, "OSD %s: %dx%d%+d%+d",
        __FUNCTION__, rect.Width(), rect.Height(), rect.X(), rect.Y());

    if (MyVideoStream->Render) {
	VideoSetOutputPosition(MyVideoStream->Render, rect.X(), rect.Y(), rect.Width(), rect.Height());
    }
}

/**
**	Can play IBP frames in fast forward trickspeed
*/
bool cSoftHdDevice::HasIBPTrickSpeed(void) const
{
	return false;
//	return true;
}


/**
**	Return command line help string.
*/
const char *cSoftHdDevice::CommandLineHelp(void)
{
    return "  -a device\taudio device (fe. alsa: hw:0,0)\n"
	"  -p device\taudio device for pass-through (hw:0,1)\n"
	"  -c channel\taudio mixer channel name (fe. PCM)\n"
	"  -d resolution\tdisplay resolution (fe. 1920x1080@50)\n"
#ifdef USE_GLES
	"  -w workaround\tenable/disable workarounds\n"
	"\tdisable-ogl-osd disable openGL osd\n"
#endif
	"\n";
}

/**
**	Process the command line arguments.
**
**	@param argc	number of arguments
**	@param argv	arguments vector
*/
int cSoftHdDevice::ProcessArgs(int argc, char *argv[])
{
    //
    //	Parse arguments.
    //

    for (;;) {
#ifdef USE_GLES
	switch (getopt(argc, argv, "-a:c:p:d:w:")) {
#else
	switch (getopt(argc, argv, "-a:c:p:d:")) {
#endif
	    case 'a':			// audio device for pcm
		AudioSetDevice(optarg);
		continue;
	    case 'c':			// channel of audio mixer
		AudioSetChannel(optarg);
		continue;
	    case 'p':			// pass-through audio device
		AudioSetPassthroughDevice(optarg);
		continue;
	    case 'd':			// set display output
		VideoSetDisplay(optarg);
		continue;
#ifdef USE_GLES
	    case 'w':			// workarounds
		if (!strcasecmp("disable-ogl-osd", optarg)) {
		    SetDisableOglOsd();
		} else {
		    fprintf(stderr, _("Workaround '%s' unsupported\n"),
			optarg);
		    return 0;
		}
		continue;
#endif
	    case EOF:
		break;
	    case '-':
		fprintf(stderr, _("We need no long options\n"));
		return 0;
	    case ':':
		fprintf(stderr, _("Missing argument for option '%c'\n"),
		    optopt);
		return 0;
	    default:
		fprintf(stderr, _("Unknown option '%c'\n"), optopt);
		return 0;
	}
	break;
    }
    while (optind < argc) {
		fprintf(stderr, _("Unhandled argument '%s'\n"), argv[optind++]);
    }

    return 1;
}

void cSoftHdDevice::SetDisableDeint(void)
{
	if (MyVideoStream->Render)
		VideoSetDisableDeint(MyVideoStream->Render, ConfigDisableDeint);
}

void cSoftHdDevice::SetDisableOglOsd(void)
{
	ConfigDisableOglOsd = 1;
	if (MyVideoStream->Render)
		VideoSetDisableOglOsd(MyVideoStream->Render);
}

//////////////////////////////////////////////////////////////////////////////
//	OSD
//////////////////////////////////////////////////////////////////////////////

/**
**	Close OSD.
*/
void cSoftHdDevice::OsdClose(void)
{
    VideoOsdClear(MyVideoStream->Render);
}

/**
**	Draw an OSD pixmap.
**
**	@param xi	x-coordinate in argb image
**	@param yi	y-coordinate in argb image
**	@paran height	height in pixel in argb image
**	@paran width	width in pixel in argb image
**	@param pitch	pitch of argb image
**	@param argb	32bit ARGB image data
**	@param x	x-coordinate on screen of argb image
**	@param y	y-coordinate on screen of argb image
*/
void cSoftHdDevice::OsdDrawARGB(int xi, int yi, int height, int width, int pitch,
	const uint8_t * argb, int x, int y)
{
	VideoOsdDrawARGB(MyVideoStream->Render, xi, yi, height, width,
			pitch, argb, x, y);
}

void cSoftHdDevice::SetPassthrough(int mask)
{
	AudioPassthrough = mask;
	AudioSetPassthrough(mask);
	if (MyAudioDecoder)
		MyAudioDecoder->SetPassthrough(mask);
}

/**
**	Resets channel ID (restarts audio).
*/
void cSoftHdDevice::ResetChannelId(void)
{
    LOGDEBUG("ResetChannelId:");
    AudioChannelID = -1;
    LOGDEBUG("audio/demux: reset channel id");
}

/**
**	Set log level
*/
void cSoftHdDevice::SetLogLevel(int loglevel)
{
	DebugLogLevel = loglevel;

	if (!loglevel)
		return;

	char prefix[256] = "Set loglevels:";
	if (loglevel & L_DEBUG)
		strcat(prefix, " standard debugs,");
	if (loglevel & L_AV_SYNC)
		strcat(prefix, " AV-Sync,");
	if (loglevel & L_SOUND)
		strcat(prefix, " sound,");
	if (loglevel & L_OSD)
		strcat(prefix, " osd,");
	if (loglevel & L_DRM)
		strcat(prefix, " drm,");
	if (loglevel & L_CODEC)
		strcat(prefix, " codec,");
	if (loglevel & L_STILL)
		strcat(prefix, " stillpicture,");
	if (loglevel & L_TRICK)
		strcat(prefix, " trickspeed,");
	if (loglevel & L_MEDIA)
		strcat(prefix, " mediaplayer,");
	if ((loglevel & L_OPENGL) ||
	    (loglevel & L_OPENGL_TIME) ||
	    (loglevel & L_OPENGL_TIME_ALL))
		strcat(prefix, " OpenGL OSD,");
	if (loglevel & L_PACKET)
		strcat(prefix, " packet tracking,");
	if (loglevel & L_GRAB)
		strcat(prefix, " grabbing");

	LOGINFO("%s", prefix);
}

/**
**	Get decoder statistics.
**
**	@param[out] duped	duped frames
**	@param[out] dropped	dropped frames
**	@param[out] count	number of decoded frames
*/
void cSoftHdDevice::GetStats(int *duped, int *dropped, int *counter)
{
	*duped = 0;
	*dropped = 0;
	*counter = 0;
	if (MyVideoStream->Render) {
		VideoGetStats(MyVideoStream->Render, duped, dropped, counter);
	}
}

//////////////////////////////////////////////////////////////////////////////
//	mediaplayer functions
//////////////////////////////////////////////////////////////////////////////

void cSoftHdDevice::SetAudioCodec(enum AVCodecID codec_id, AVCodecParameters * par, AVRational * timebase)
{
	MyAudioDecoder->Open(codec_id, par, timebase);
}

void cSoftHdDevice::SetVideoCodec(enum AVCodecID codec_id, AVCodecParameters * par, AVRational * timebase)
{
	MyVideoStream->CodecID = codec_id;
	MyVideoStream->NewStream = 1;
	MyVideoStream->Par = par;
	MyVideoStream->timebase.num = timebase->num;
	MyVideoStream->timebase.den = timebase->den;
}

int cSoftHdDevice::PlayAudioPkts(AVPacket * pkt)
{
	if (AudioFreeBytes() < AUDIO_MIN_BUFFER_FREE) {
//		LOGERROR("PlayAudioPkts: AudioFreeBytes() < AUDIO_MIN_BUFFER_FREE!");
		return 0;
	}
	MyAudioDecoder->Decode(pkt);
	return 1;
}

int cSoftHdDevice::PlayVideoPkts(AVPacket * pkt)
{
	AVPacket *avpkt;

	if (atomic_read(&MyVideoStream->PacketsFilled) >= VIDEO_PACKET_MAX - 10) {
		return 0;
	}

	avpkt = &MyVideoStream->PacketRb[MyVideoStream->PacketWrite];
	MyVideoStream->PacketWrite = (MyVideoStream->PacketWrite + 1) % VIDEO_PACKET_MAX;
	atomic_inc(&MyVideoStream->PacketsFilled);

	if ((size_t)pkt->size > avpkt->buf->size) {
		LOGINFO("PlayVideoPkts: grow packet buffer size by %d",
			(int)(pkt->size - avpkt->buf->size + AV_INPUT_BUFFER_PADDING_SIZE));
		av_grow_packet(avpkt, pkt->size - avpkt->buf->size +
			AV_INPUT_BUFFER_PADDING_SIZE);
	}

	memcpy(avpkt->data, pkt->data, pkt->size);
	avpkt->pts = pkt->pts;
	avpkt->size = pkt->size;
	return 1;
}
