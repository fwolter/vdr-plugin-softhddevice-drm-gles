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

    /// Minimum free space in audio buffer 8 packets for 8 channels
#define AUDIO_MIN_BUFFER_FREE (3072 * 8 * 8)
#define AUDIO_BUFFER_SIZE (512 * 1024)	///< audio PES buffer default size

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

//////////////////////////////////////////////////////////////////////////////
//	cSoftHdDevice
//////////////////////////////////////////////////////////////////////////////

/**
**	Constructor device.
*/
cSoftHdDevice::cSoftHdDevice(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
    spuDecoder = NULL;

    Audio = new cSoftHdAudio(this);
    Render = new cVideoRender(this);
    VideoStream = new cVideoStream(this);
}

void cSoftHdDevice::StartThreads(void)
{
    Render->StartThreads();
}

/**
**	Destructor device.
*/
cSoftHdDevice::~cSoftHdDevice(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
    Exit();

    delete VideoStream;
    delete Audio;
    delete Render;

    delete spuDecoder;
    LOGDEBUG("%s: deleted", __FUNCTION__);
}

/**
**	Cleanup an exit.
*/
void cSoftHdDevice::Exit(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
    Audio->Exit();
    if (AudioDecoder) {
	AudioDecoder->Close();
	delete AudioDecoder;
    }
    NewAudioStream = 0;
    av_packet_free(&AudioAvPkt);

    Render->Exit();
    VideoStream->Exit();

    LOGDEBUG("%s: exited", __FUNCTION__);
}

/**
**	Prepare plugin.
*/
void cSoftHdDevice::Start(void)
{
    LOGDEBUG("Start(void):");
    if (!AudioDecoder) {
	Audio->Init(Audio->GetPassthrough());
	Audio->SetBufferTimeInMs(ConfigAudioBufferTime);
	AudioAvPkt = av_packet_alloc();
	av_new_packet(AudioAvPkt, AUDIO_BUFFER_SIZE);

	AudioDecoder = new cAudioDecoder(Audio);
	AudioCodecID = AV_CODEC_ID_NONE;
	AudioChannelID = -1;

	if (!VideoStream->Decoder())
	    VideoStream->SetCodecId(AV_CODEC_ID_NONE);

//	VideoStream->SetRender(Render);

	if (ConfigDisableOglOsd)
	    Render->DisableOglOsd();
	Render->DisableDeint(ConfigDisableDeint);
	Render->Init();

	VideoStream->SetDecoder(new cVideoDecoder(Render));
	VideoStream->InitPacketRb();
    }
}

/**
**	Stop plugin.
**
**	@note stop everything, but don't cleanup, module is still called.
*/
void cSoftHdDevice::Stop(void)
{
    LOGDEBUG("Stop(void): nothing to do");
}

/**
**	Clears all audio data from the decoder and ringbufffer.
*/
void cSoftHdDevice::ClearAudio(void)
{
	if (!SkipAudio) {
		LOGDEBUG("ClearAudio()");
		AudioDecoder->FlushBuffers();
		Audio->FlushBuffers();
		NewAudioStream = 1;
	}
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
	LOGDEBUG("%s: %d", __FUNCTION__, play_mode);

	switch (play_mode) {
	case 0:			// none audio/video
		VideoStream->Stop();
		VideoStream->Clear();
		VideoStream->CloseDecoder();

		Render->SetClosing(1);
		SkipAudio = 0;
		Audio->Resume();
		ClearAudio();	// flush all AUDIO buffers
		if (AudioDecoder && AudioCodecID != AV_CODEC_ID_NONE) {
			NewAudioStream = 1;
		}
		VideoStream->SetInterlaced(0); // probably not necessary
		VideoStream->Start();
		VideoStream->Resume();
		break;
	case 1:			// audio/video
		Render->WakeupDecodingThread();
		Render->WakeupDisplayThread();
		break;
	case 2:			// audio only
		Render->ExitDecodingThread();
		Render->ExitDisplayThread();
		break;
	case 3:			// audio only (black screen)
		LOGDEBUG("softhddev: FIXME: audio only, silence video errors");
		Render->WakeupDecodingThread();
		Render->WakeupDisplayThread();
		break;
	case 4:			// video only
		Render->WakeupDecodingThread();
		Render->WakeupDisplayThread();
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
    if (Render) {
	return Render->GetVideoClock();
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
    LOGDEBUG("%s: %d %s", __FUNCTION__, speed, forward ? "forward" : "backward");

    // start stream if closed
    VideoStream->Start();
    // start stream if paused
    VideoStream->Resume();

    Render->SetTrickSpeed(speed, forward);

    // start render thread
    Render->StartVideo();
}

/**
 * @brief Clears all video and audio data from the device.
 *
 * This is called by VDR via DeviceClear() in the Empty() call
 *
 * Empty() does clears all VDR internal packets
 *
 * DeviceClear() needs to
 *  1. stop the stream and let the decoding thread wait
 *  2. clear the packet buffer (drop packets which are not yet decoded)
 *  3. flush the packets already sent to the decoder
 *  4. stop/finish the renderer thread (this also does stopping the filter thread
 *  5. clear audio data
 *  6. start the stream again
 */
void cSoftHdDevice::Clear(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
    cDevice::Clear();

    VideoStream->Stop();
    VideoStream->Clear();
    VideoStream->FlushDecoder();

    Render->SetClosing(0);

    ClearAudio();

    // we need a Start() here, because when VDR does SkipSeconds()
    // it doesn't send a Play() again, if we skip during a playing stream
    VideoStream->Start();
}

/**
 * @brief Sets the device into play mode (after a previous trick mode)
 *
 * This is called by VDR via DevicePlay() in the Play() and Goto() call
 *
 * Play() needs to
 *  1. start and/or resume the stream
 *  2. unmute and resume audio
 *  3. wakeup the renderer
 */
void cSoftHdDevice::Play(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
    cDevice::Play();

    VideoStream->Start();
    VideoStream->Resume();

    SkipAudio = 0;
    Audio->Resume();

    Render->SetTrickSpeed(0, 1);
    Render->StartVideo();
}

/**
 * @brief Puts the device into "freeze frame" mode.
 */
void cSoftHdDevice::Freeze(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
    cDevice::Freeze();

    // pause video stream
    VideoStream->Pause();
    // pause audio playpack
    Audio->Pause();
    // pause the renderer
    Render->PauseVideo();
}

/**
**	Turns off audio while replaying.
*/
void cSoftHdDevice::Mute(void)
{
    LOGDEBUG("%s:", __FUNCTION__);
    cDevice::Mute();

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

    avpkt = VideoStream->GetPacketToWrite();
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
    VideoStream->IncreasePacketsFilled();

    VideoStream->Pause();
    if (VideoStream->Decoder()->GetContext()) {
	if ((int)(VideoStream->Decoder()->GetContext()->codec_id) != codec) {
	    VideoStream->Decoder()->Close();
	}
    }
    if (!VideoStream->Decoder()->GetContext()) {
	if (VideoStream->Decoder()->Open(codec, NULL, NULL, 0, 0, 0))
	    LOGFATAL("StillPicture: Could not open the decoder!");
	context = 1;
    }
    Audio->Pause();

    int ret = 0;
    ret = VideoStream->Decoder()->SendPacket(avpkt);
    if (ret)
	LOGDEBUG2(L_STILL, "StillPicture: SendPacket(avpkt) returned %d", ret);
    else
	LOGDEBUG2(L_STILL, "StillPicture: avpkt sent");

    // force decoder to enter draining because we only want 1 avpkt to be decoded
    VideoStream->Decoder()->SendPacket(NULL);

receive:
    ret = VideoStream->Decoder()->ReceiveFrame(1, &frame);
    if (!ret) {
	// frame received, render it and try another one (should end up with AVERROR_EOF)
	LOGDEBUG2(L_STILL, "StillPicture: frame received");
	Render->MarkAsStillpictureFrame(frame);
	while (Render->RenderFrame(VideoStream->Decoder()->GetContext(), frame)) {
	    if (VideoStream->IsClosing()) {
		av_frame_free(&frame);
		break;
	    }
	}
	goto receive;
    } else if (ret == AVERROR_EOF) {
	// AVERROR_EOF, flush needed
	VideoStream->FlushDecoder();
    } else {
	// sth went wrong or AVERROR(EAGAIN)
	LOGDEBUG2(L_STILL, "StillPicture: Receive Frame returned %d, should not happen!", ret);
    }

    if (context) {
	VideoStream->Decoder()->Close();
	VideoStream->SetCodecId(AV_CODEC_ID_NONE);
    }
    ClearAudio();
    VideoStream->Clear();
    VideoStream->FlushDecoder();
    VideoStream->Resume();
    Audio->Resume();
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

	used = Audio->GetUsedBytes();
	// FIXME: no video!
	filled = VideoStream->GetPacketsFilled();
	// soft limit + hard limit
	full = (used > AUDIO_MIN_BUFFER_FREE && filled > 3)
	    || Audio->GetFreeBytes() < AUDIO_MIN_BUFFER_FREE
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
    if (VideoStream->GetPacketsFilled()) {
	if (timeout) {			// let display thread work
	    usleep(timeout * 1000);
	}
	return !VideoStream->GetPacketsFilled();
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
    VideoStream->Decoder()->GetVideoSize(&width, &height, &aspect_ratio);
//	LOGDEBUG("GetVideoSize: %d x %d @ %f", *width, *height, *aspect_ratio);
}

/**
**	Returns the width, height and pixel_aspect ratio the OSD.
**
**	FIXME: Called every second, for nothing (no OSD displayed)?
*/
void cSoftHdDevice::GetOsdSize(int &width, int &height, double &pixel_aspect)
{
    Render->GetScreenSize(&width, &height, &pixel_aspect);
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

    if (VideoStream->IsPaused()) {	// stream is paused, don't accept new audio data
	LOGDEBUG("PlayAudio: Stream is paused");
	return 0;
    }

    if (SkipAudio) {	// skip audio
	LOGDEBUG("PlayAudio: skip audio");
	return size;
    }

    // hard limit buffer full: don't overrun audio buffers on replay
    if (Audio->GetFreeBytes() < AUDIO_MIN_BUFFER_FREE){
//	LOGDEBUG("PlayAudio: Buffer is Full (%d|%d)!", Audio->GetFreeBytes(), AUDIO_MIN_BUFFER_FREE);
	return 0;
    }

    if (NewAudioStream) {
	// this clears the audio ringbuffer indirect, open and setup does it
	LOGDEBUG("PlayAudio: NewAudioStream");
	AudioDecoder->Close();
//	FlushBuffers();
//	SetBufferTimeInMs(ConfigAudioBufferTime);		// ???
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

	return size;
    }
    // DVD track header
    if ((id & 0xF0) == 0x80 && (p[0] & 0xF0) == 0x80) {
	p += 4;
	n -= 4;				// skip track header

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
		AudioDecoder->Close();
		AudioDecoder->Open(codec_id, NULL, &timebase);
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
	    AudioDecoder->Decode(avpkt);
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

    Audio->SetVolume((volume * 1000) / 255);
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

    int64_t pts = AV_NOPTS_VALUE;
    int i, n;

    if (VideoStream->IsPaused()) {
	return 0;
    }

    // must be a PES video start code
    if (size < 9 || !data || data[0] || data[1] || data[2] != 0x01 || data[3] >> 4 != 0x0e) {
	return size;
    }

    // hard limit buffer full: needed for replay
    if (VideoStream->GetPacketsFilled() >= VIDEO_PACKET_MAX - 10) {
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
	    if (VideoStream->GetCodecId() == AV_CODEC_ID_NONE) {
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
		    VideoStream->SetCodecId(AV_CODEC_ID_MPEG2VIDEO);
		    VideoStream->SetTrickpkts(1);
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
		    VideoStream->SetCodecId(AV_CODEC_ID_H264);
		    VideoStream->SetTrickpkts(2);
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
		    VideoStream->SetCodecId(AV_CODEC_ID_HEVC);
		    VideoStream->SetTrickpkts(2);
newstream:
		    VideoStream->Open();
		    VideoStream->SetTimebase(1, 90000);
		    VideoStream->EnqueueInRb(pts, data + i + n, size - i - n);
		}
	    } else {
		VideoStream->EnqueueInRb(pts, data + i + n, size - i - n);
	    }
	    return size;
	}
    }

    // this happens when vdr sends incomplete packets and no stream is started
    if (VideoStream->GetCodecId() == AV_CODEC_ID_NONE) {
	return size;
    }

    // complete last frame
    VideoStream->EnqueueInRb(pts, data + n, size - n);
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
    if (m_grabActive) {
	LOGWARNING("%s: wait for the last grab to be finished - skip!", __FUNCTION__);
	return NULL;
    }

    if (!width || !height) {
	LOGERROR("%s: Width or height must be not 0!", __FUNCTION__);
	return NULL;
    }

    if (quality < 0) {			// caller should care, but fix it
	quality = 95;
    }

    LOGDEBUG2(L_GRAB, "%s: %d, %d, %d, %dx%d", __FUNCTION__, size, jpeg,
	quality, width, height);

    // 1. Trigger grab in video thread and wait for the buffers to be cloned
    m_grabActive = 1;
    // TriggerGrab does wait and return 0, if buffers are available,
    // otherwise it returns != 0, if we ran into a timeout
    if (Render->TriggerGrab()) {
	Render->ClearGrab();
	m_grabActive = 0;
	return NULL;
    }

    // 2. Convert the buffers to rgb and free the cloned buffers afterwards
    Render->ConvertOsdBufToRgb();
    Render->ConvertVideoBufToRgb();

    // 3. get screen dimensions
    int screenwidth;
    int screenheight;
    double pixel_aspect;
    Render->GetScreenSize(&screenwidth, &screenheight, &pixel_aspect);
    int screensize = screenwidth * screenheight * 3; // we want a RGB24

    // 4. set grab dimensions
    int grabwidth = width > 0 ? width : screenwidth;
    int grabheight = height > 0 ? height : screenheight;

    int video_size = 0;			// data size of the grabbed video
    int video_width = screenwidth;	// width of the grabbed video
    int video_height = screenheight;	// height of the grabbed video
    int video_x = 0, video_y = 0;	// x, y of the grabbed video

    // 5. fetch video data
    // Video comes as RGB, width and height is original screen dimension (video is maybe scaled)
    cSoftHdGrab *videoGrab = Render->GetGrab(&video_size, &video_width, &video_height, &video_x, &video_y, 0);
    uint8_t *video = NULL;
    if (videoGrab->GetSize())
	video = videoGrab->GetData();
    if (!video) {
        LOGDEBUG2(L_GRAB, "GrabImage: video is NULL, create black screen!");
        video = (uint8_t *)calloc(1, screensize);
    }

    // 6. fetch osd data
    // OSD comes as ARGB, width and height is original screen dimension (osd is always fullscreen)
    cSoftHdGrab *osdGrab = Render->GetGrab(NULL, NULL, NULL, NULL, NULL, 1);
    uint8_t *osd = NULL;
    if (osdGrab->GetSize())
	osd = osdGrab->GetData();;
    if (!osd)
        LOGDEBUG2(L_GRAB, "GrabImage: osd is NULL, skip it");

    // 7. blit the video into a full black screen if scaled
    uint8_t *scaledvideo;
    if (video_width != screenwidth || video_height != screenheight || video_x != 0 || video_y != 0) {
        scaledvideo = blitvideo(video, screenwidth, screenheight, video_x, video_y, video_width, video_height);
        free(video);
    } else {
        scaledvideo = video;
    }

    // 8. alphablend fullscreen video with osd if available
    uint8_t *result;
    if (!osd) {
        result = scaledvideo;
    } else {
        result = (uint8_t *)malloc(screensize);
        alphablend(result, osd, scaledvideo, screenwidth, screenheight);
        free(scaledvideo);
        free(osd);
    }

    // 9. scale result to requested size width + height, if it differs from fullscreen
    int scaledsize = screensize;
    uint8_t *scaledresult;
    if (screenwidth != grabwidth || screenheight != grabheight) {
        scaledresult = scalergb24(result, &scaledsize, screenwidth, screenheight, grabwidth, grabheight);
        free(result);
    } else {
        scaledresult = result;
    }

    // 10. make jpeg or pnm
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

    m_grabActive = 0;
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

    if (Render) {
		Render->SetVideoOutputPosition(rect);
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
		Audio->SetDevice(optarg);
		continue;
	    case 'c':			// channel of audio mixer
		Audio->SetChannel(optarg);
		continue;
	    case 'p':			// pass-through audio device
		Audio->SetPassthroughDevice(optarg);
		continue;
	    case 'd':			// set display output
		Render->SetDisplayResolution(optarg);
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
	if (Render)
		Render->DisableDeint(ConfigDisableDeint);
}

void cSoftHdDevice::SetDisableOglOsd(void)
{
	ConfigDisableOglOsd = 1;
	if (Render)
		Render->DisableOglOsd();
}

//////////////////////////////////////////////////////////////////////////////
//	OSD
//////////////////////////////////////////////////////////////////////////////

/**
**	Close OSD.
*/
void cSoftHdDevice::OsdClose(void)
{
    Render->OsdClear();
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
	Render->OsdDrawARGB(xi, yi, height, width, pitch, argb, x, y);
}

void cSoftHdDevice::SetPassthrough(int mask)
{
	Audio->SetPassthrough(mask);
	if (AudioDecoder)
		AudioDecoder->SetPassthrough(mask);
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
	if (Render) {
		Render->GetStats(duped, dropped, counter);
	}
}

//////////////////////////////////////////////////////////////////////////////
//	mediaplayer functions
//////////////////////////////////////////////////////////////////////////////

void cSoftHdDevice::SetAudioCodec(enum AVCodecID codec_id, AVCodecParameters * par, AVRational * timebase)
{
	AudioDecoder->Open(codec_id, par, timebase);
}

void cSoftHdDevice::SetVideoCodec(enum AVCodecID codec_id, AVCodecParameters * par, AVRational * timebase)
{
	VideoStream->SetCodecId(codec_id);
	VideoStream->Open();
	VideoStream->SetParameters(par);
	VideoStream->SetTimebase(timebase->den, timebase->num);
}

int cSoftHdDevice::PlayAudioPkts(AVPacket * pkt)
{
	if (Audio->GetFreeBytes() < AUDIO_MIN_BUFFER_FREE) {
//		LOGERROR("PlayAudioPkts: Audio->GetFreeBytes() < AUDIO_MIN_BUFFER_FREE!");
		return 0;
	}
	AudioDecoder->Decode(pkt);
	return 1;
}

int cSoftHdDevice::PlayVideoPkts(AVPacket * pkt)
{
	AVPacket *avpkt;

	if (VideoStream->GetPacketsFilled() >= VIDEO_PACKET_MAX - 10) {
		return 0;
	}

	avpkt = VideoStream->GetPacketToWrite();
	VideoStream->AdvancePacketToWrite();
	VideoStream->IncreasePacketsFilled();

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

int cSoftHdDevice::GetVideoAudioDelay(void)
{
	return VideoAudioDelay;
}

void cSoftHdDevice::SetVideoAudioDelay(int delay)
{
	VideoAudioDelay = delay;
}
