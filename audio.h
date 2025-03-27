///
///	@file audio.h		@brief Audio module headerfile
///
///	Copyright (c) 2009 - 2014 by Johns.  All Rights Reserved.
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

/// @addtogroup Audio
/// @{

//----------------------------------------------------------------------------
//	Defines
//----------------------------------------------------------------------------

#define CodecPCM  0x01			///< PCM bit mask
#define CodecMPA  0x02			///< MPA bit mask (planned)
#define CodecAC3  0x04			///< AC-3 bit mask
#define CodecEAC3 0x08			///< E-AC-3 bit mask
#define CodecDTS  0x10			///< DTS bit mask (planned)

//----------------------------------------------------------------------------
//	Prototypes
//----------------------------------------------------------------------------

extern void AudioFilter(AVFrame *, AVCodecContext *);	///< buffer audio samples
extern void AudioFlushBuffers(void);	///< flush audio buffers
extern void AudioPoller(void);		///< poll audio events/handling		not used!
extern int AudioFreeBytes(void);	///< free bytes in audio output
extern int AudioUsedBytes(void);	///< used bytes in audio output
extern int64_t AudioGetClock();		///< get current audio clock
extern int AudioVideoReady(int64_t);	///< tell audio video is ready
extern int AudioSkipInTrickSpeed(int64_t, int);	///< skip old audio data in trickspeed

extern void AudioSetVolume(int);	///< set volume
//extern int AudioSetup(int *, int *, int);	///< setup audio output

extern void AudioPlay(void);		///< play audio
extern void AudioPause(void);		///< pause audio

extern void AudioSetBufferTime(int);	///< set audio buffer time
extern void AudioSetSoftvol(int);	///< enable/disable softvol
extern void AudioSetNormalize(int, int);	///< set normalize parameters
extern void AudioSetCompression(int, int);	///< set compression parameters
extern void AudioSetStereoDescent(int);	///< set stereo loudness descent
extern void AudioSetEq(int[17], int);  /// Set audio equalizer.
extern void AudioSetDownmix(int);

extern void AudioSetDevice(const char *);	///< set PCM audio device
extern void AudioSetPassthroughDevice(const char *);	/// set pass-through device
extern void AudioSetPassthrough(int);	/// set pass-through mask
extern void AudioSetChannel(const char *);	///< set mixer channel
extern void AudioSetAutoAES(int);	///< set automatic AES flag handling

extern void AudioInit(void);		///< setup audio module
extern void AudioExit(void);		///< cleanup and exit audio module

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------

/// @}
