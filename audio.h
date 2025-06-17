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

#ifndef __AUDIO_H
#define __AUDIO_H

extern "C"
{
//#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
//#include <libavfilter/buffersink.h>
//#include <libavfilter/buffersrc.h>
//#include <libavutil/channel_layout.h>
//#include <libavutil/opt.h>
}


#include <alsa/asoundlib.h>
#include "ringbuffer.h"

//----------------------------------------------------------------------------
//	Defines
//----------------------------------------------------------------------------

#define MIN_AUDIO_BUFFER	450	///< minimal output buffer in ms
#define AudioNormMaxIndex 128		///< number of average values

class cSoftHdDevice;

//----------------------------------------------------------------------------
//	cSoftHdAudio class
//----------------------------------------------------------------------------

class cSoftHdAudio {
private:
// variables
    cSoftHdDevice *Device;

    // thread
    pthread_mutex_t AudioRbMutex;	///< audio condition mutex
    volatile char AudioRunning;	///< thread running / stopped
    pthread_t AudioThread;		///< audio play thread
    pthread_mutex_t AudioStartMutex;	///< audio condition mutex
    pthread_cond_t AudioStartCond;	///< condition variable
    char AudioThreadStop;		///< stop audio thread

    // common audio, alsa
    const int AudioBytesProSample = 2;	///< number of bytes per sample
    int Filterchanged;
    int FilterInit;
    int AudioDownMix;
    snd_pcm_t *AlsaPCMHandle;	///< alsa pcm handle
    int64_t PTS;			///< pts clock
    int AudioSkip;			///< skip audio to sync to video
    volatile char AudioVideoIsReady;	///< video ready start early
    volatile char AudioPaused;	///< audio paused
    char AlsaPlayerStop;		///< stop audio thread
    char AudioSoftVolume;		///< flag use soft volume
    int AudioPassthrough;		///< Passthrough mask
    const char *AudioPCMDevice;	///< PCM device name
    const char *AudioPassthroughDevice;	///< Passthrough device name
    int AlsaUseMmap;			///< use mmap
    char AudioAppendAES;		///< flag automatic append AES
    char AlsaCanPause;		///< hw supports pause
    unsigned AudioStartThreshold;	///< start play, if filled
    int AudioBufferTime;	///< audio buffer time in ms

    // Normalizer
    char AudioNormalize;		///< flag use volume normalize
    const int AudioNormSamples = 4096;	///< number of samples
    int AudioNormCounter;		///< sample counter
    uint32_t AudioNormAverage[AudioNormMaxIndex];	/// average of n last sample blocks
    int AudioNormIndex;		///< index into average table
    int AudioNormReady;		///< index counter
    int AudioNormalizeFactor;	///< current normalize factor
    const int AudioMinNormalize = 100;	///< min. normalize factor
    int AudioMaxNormalize;		///< max. normalize factor

    // Compressor
    char AudioCompression;		///< flag use compress volume
    int AudioCompressionFactor;	///< current compression factor
    int AudioMaxCompression;		///< max. compression factor

    // Amplifier
    char AudioMute;			///< flag muted
    int AudioAmplifier;		///< software volume factor
    int AudioStereoDescent;		///< volume descent for stereo
    int AudioVolume;			///< current volume (0 .. 1000)

    // Equalizer
    int AudioEq;
    float AudioEqBand[18];

    // mixer
    const char *AudioMixerDevice;	///< mixer device name
    const char *AudioMixerChannel;	///< mixer channel name
    snd_mixer_t *AlsaMixer;		///< alsa mixer handle
    snd_mixer_elem_t *AlsaMixerElem;	///< alsa pcm mixer element
    int AlsaRatio;			///< internal -> mixer ratio * 1000

    // filter
    AVFilterGraph *filter_graph;
    AVFilterContext *abuffersrc_ctx, *abuffersink_ctx;

    // ring buffer variables
    unsigned int HwSampleRate;		///< hardware sample rate in Hz
    unsigned int HwChannels;		///< hardware number of channels
    const unsigned AudioRingBufferSize = 3 * 5 * 7 * 8 * 2 * 1000;    /// default ring buffer size ~2s 8ch 16bit (3 * 5 * 7 * 8)
    cDeviceRingbuffer *AudioRingBuffer = nullptr;		///< sample ring buffer

    AVRational *timebase;			///< pointer to AVCodecContext pkts_timebase

// methods
    void AudioNormalizer(int16_t *, int);
    void AudioResetNormalizer(void);
    void AudioCompressor(int16_t *, int);
    void AudioResetCompressor(void);
    void AudioSoftAmplifier(int16_t *, int);

    int AudioFilterInit(AVCodecContext *);

    void AudioRingInit(void);
    void AudioRingExit(void);

    // alsa
    void xrun_recovery(void);
    void AlsaFlushBuffers(void);

    char *opendevice(const char *, int);
    char *finddevice(const char *, const char *, int);
    int AlsaSetup(int channels, int sample_rate, int passthrough);
    void AlsaInitPCM(void);
    void AlsaInitMixer(void);
    void AlsaSetVolume(int);
    void AlsaInit(void);
    void AlsaExit(void);
    int AlsaPlayer(void);

    void *AudioPlayHandlerThread(void *);
    void AudioInitThread(void);
    void AudioExitThread(void);

    void AudioEnqueue(AVFrame *);	///< buffer spdif audio samples

public:
    cSoftHdAudio(cSoftHdDevice *);
    virtual ~cSoftHdAudio(void);

    void AudioInit(int);		///< setup audio module
    void AudioExit(void);		///< cleanup and exit audio module
    void AudioPlay(void);		///< play audio
    void AudioPause(void);		///< pause audio

    void AudioEnqueueSpdif(AVCodecContext *, const uint16_t *, int, AVFrame *);	///< buffer spdif audio samples
    int AudioSetup(AVCodecContext *, int , int , int);	///< setup audio (for passthrough only atm)
    int AudioVideoReady(int64_t);	///< tell audio video is ready
    int AudioSkipInTrickSpeed(int64_t, int);	///< skip old audio data in trickspeed
    void AudioFilter(AVFrame *, AVCodecContext *);	///< buffer audio samples
    void AudioFlushBuffers(void);	///< flush audio buffers
    int AudioUsedBytes(void);	///< used bytes in audio output
    int AudioFreeBytes(void);	///< free bytes in audio output
    int64_t AudioGetClock();		///< get current audio clock


//    void AudioSetEq(int[17], int);  /// Set audio equalizer.
//    void AudioSetVolume(int);	///< set volume
//    void AudioSetBufferTime(int);	///< set audio buffer time
//    void AudioSetDownmix(int);
//    void AudioSetSoftvol(int);	///< enable/disable softvol
//    void AudioSetNormalize(int, int);	///< set normalize parameters
//    void AudioSetCompression(int, int);	///< set compression parameters
//    void AudioSetStereoDescent(int);	///< set stereo loudness descent
//    void AudioSetDevice(const char *);	///< set PCM audio device
//    void AudioSetPassthroughDevice(const char *);	/// set pass-through device
    int AudioGetPassthrough(void) const { return AudioPassthrough; }	/// set pass-through mask
//    void AudioSetChannel(const char *);	///< set mixer channel
//    void AudioSetAutoAES(int);	///< set automatic AES flag handling


    void AudioSetEq(int[17], int);  /// Set audio equalizer.
    void AudioSetVolume(int);	///< set volume
    void AudioSetBufferTime(int);	///< set audio buffer time
    void AudioSetDownmix(int);
    void AudioSetSoftvol(int);	///< enable/disable softvol
    void AudioSetNormalize(int, int);	///< set normalize parameters
    void AudioSetCompression(int, int);	///< set compression parameters
    void AudioSetStereoDescent(int);	///< set stereo loudness descent
    void AudioSetDevice(const char *);	///< set PCM audio device
    void AudioSetPassthroughDevice(const char *);	/// set pass-through device
    void AudioSetPassthrough(int);	/// set pass-through mask
    void AudioSetChannel(const char *);	///< set mixer channel
    void AudioSetAutoAES(int);	///< set automatic AES flag handling
};

#endif