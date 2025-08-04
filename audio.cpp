///
///	@file audio.c		@brief Audio module
///
///	Copyright (c) 2009 - 2014 by Johns.  All Rights Reserved.
///	Copyright (c) 2018 by zille.  All Rights Reserved.
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

///
///	@defgroup Audio The audio module.
///
///		This module contains all audio output functions.
///
///		ALSA PCM/Mixer api is supported.
///		@see http://www.alsa-project.org/alsa-doc/alsa-lib
///
///	@note alsa async playback is broken, don't use it!
///
///	@todo FIXME: there can be problems with little/big endian.
///

#include <stdint.h>
#include <math.h>

#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut

#include <alsa/asoundlib.h>

#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

#include "misc.h"
}

#include "iatomic.h"			// portable atomic_t

#include "ringbuffer.h"
#include "audio.h"
#include "video.h"
#include "codec_audio.h"
#include "videostream.h"

#include "logger.h"
#include "threads.h"

//----------------------------------------------------------------------------
//	Filter
//----------------------------------------------------------------------------

/**
**	Reorder audio frame.
**
**	ffmpeg L  R  C	Ls Rs		-> alsa L R  Ls Rs C
**	ffmpeg L  R  C	LFE Ls Rs	-> alsa L R  Ls Rs C  LFE
**	ffmpeg L  R  C	LFE Ls Rs Rl Rr	-> alsa L R  Ls Rs C  LFE Rl Rr
**
**	@param buf[IN,OUT]	sample buffer
**	@param size		size of sample buffer in bytes
**	@param channels		number of channels interleaved in sample buffer
*/
static void AudioReorderAudioFrame(int16_t * buf, int size, int channels)
{
	int i;
	int c;
	int ls;
	int rs;
	int lfe;

	switch (channels) {
		case 5:
			size /= 2;
			for (i = 0; i < size; i += 5) {
				c = buf[i + 2];
				ls = buf[i + 3];
				rs = buf[i + 4];
				buf[i + 2] = ls;
				buf[i + 3] = rs;
				buf[i + 4] = c;
			}
			break;
		case 6:
			size /= 2;
			for (i = 0; i < size; i += 6) {
				c = buf[i + 2];
				lfe = buf[i + 3];
//				ls = buf[i + 4];	tested from jsffm
//				rs = buf[i + 5];
//				buf[i + 2] = ls;
//				buf[i + 3] = rs;
				buf[i + 2] = lfe;
				buf[i + 3] = c;
//				buf[i + 4] = c;
//				buf[i + 5] = lfe;
			}
			break;
		case 8:
			size /= 2;
			for (i = 0; i < size; i += 8) {
				c = buf[i + 2];
				lfe = buf[i + 3];
				ls = buf[i + 4];
				rs = buf[i + 5];
				buf[i + 2] = ls;
				buf[i + 3] = rs;
				buf[i + 4] = c;
				buf[i + 5] = lfe;
			}
			break;
	}
}

/**
**	Audio constructor
*/
cSoftHdAudio::cSoftHdAudio(cSoftHdDevice *device)
{
    Device = device;
}

/**
**	Audio denstructor
*/
cSoftHdAudio::~cSoftHdAudio(void)
{
}

/**
**	Audio normalizer.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
void cSoftHdAudio::AudioNormalizer(int16_t * samples, int count)
{
    int i;
    int l;
    int n;
    uint32_t avg;
    int factor;
    int16_t *data;

    // average samples
    l = count / AudioBytesProSample;
    data = samples;
    do {
	n = l;
	if (AudioNormCounter + n > AudioNormSamples) {
	    n = AudioNormSamples - AudioNormCounter;
	}
	avg = AudioNormAverage[AudioNormIndex];
	for (i = 0; i < n; ++i) {
	    int t;

	    t = data[i];
	    avg += (t * t) / AudioNormSamples;
	}
	AudioNormAverage[AudioNormIndex] = avg;
	AudioNormCounter += n;
	if (AudioNormCounter >= AudioNormSamples) {
	    if (AudioNormReady < AudioNormMaxIndex) {
		AudioNormReady++;
	    } else {
		avg = 0;
		for (i = 0; i < AudioNormMaxIndex; ++i) {
		    avg += AudioNormAverage[i] / AudioNormMaxIndex;
		}

		// calculate normalize factor
		if (avg > 0) {
		    factor = ((INT16_MAX / 8) * 1000U) / (uint32_t) sqrt(avg);
		    // smooth normalize
		    AudioNormalizeFactor =
			(AudioNormalizeFactor * 500 + factor * 500) / 1000;
		    if (AudioNormalizeFactor < AudioMinNormalize) {
			AudioNormalizeFactor = AudioMinNormalize;
		    }
		    if (AudioNormalizeFactor > AudioMaxNormalize) {
			AudioNormalizeFactor = AudioMaxNormalize;
		    }
		} else {
		    factor = 1000;
		}
		LOGDEBUG2(L_SOUND, "audio/noramlize: avg %8d, fac=%6.3f, norm=%6.3f",
		    avg, factor / 1000.0, AudioNormalizeFactor / 1000.0);
	    }

	    AudioNormIndex = (AudioNormIndex + 1) % AudioNormMaxIndex;
	    AudioNormCounter = 0;
	    AudioNormAverage[AudioNormIndex] = 0U;
	}
	data += n;
	l -= n;
    } while (l > 0);

    // apply normalize factor
    for (i = 0; i < count / AudioBytesProSample; ++i) {
	int t;

	t = (samples[i] * AudioNormalizeFactor) / 1000;
	if (t < INT16_MIN) {
	    t = INT16_MIN;
	} else if (t > INT16_MAX) {
	    t = INT16_MAX;
	}
	samples[i] = t;
    }
}

/**
**	Reset normalizer.
*/
void cSoftHdAudio::AudioResetNormalizer(void)
{
    int i;

    AudioNormCounter = 0;
    AudioNormReady = 0;
    for (i = 0; i < AudioNormMaxIndex; ++i) {
		AudioNormAverage[i] = 0U;
    }
    AudioNormalizeFactor = 1000;
}

/**
**	Audio compression.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
void cSoftHdAudio::AudioCompressor(int16_t * samples, int count)
{
    int max_sample;
    int i;
    int factor;

    // find loudest sample
    max_sample = 0;
    for (i = 0; i < count / AudioBytesProSample; ++i) {
		int t;

		t = abs(samples[i]);
		if (t > max_sample) {
			max_sample = t;
		}
    }

    // calculate compression factor
    if (max_sample > 0) {
		factor = (INT16_MAX * 1000) / max_sample;
		// smooth compression (FIXME: make configurable?)
		AudioCompressionFactor =
			(AudioCompressionFactor * 950 + factor * 50) / 1000;
		if (AudioCompressionFactor > factor) {
			AudioCompressionFactor = factor;	// no clipping
		}
		if (AudioCompressionFactor > AudioMaxCompression) {
			AudioCompressionFactor = AudioMaxCompression;
		}
    } else {
		return;				// silent nothing todo
    }

    LOGDEBUG2(L_SOUND, "audio/compress: max %5d, fac=%6.3f, com=%6.3f", max_sample,
	factor / 1000.0, AudioCompressionFactor / 1000.0);

    // apply compression factor
    for (i = 0; i < count / AudioBytesProSample; ++i) {
		int t;

		t = (samples[i] * AudioCompressionFactor) / 1000;
		if (t < INT16_MIN) {
			t = INT16_MIN;
		} else if (t > INT16_MAX) {
			t = INT16_MAX;
		}
		samples[i] = t;
    }
}

/**
**	Reset compressor.
*/
void cSoftHdAudio::AudioResetCompressor(void)
{
    AudioCompressionFactor = 2000;
    if (AudioCompressionFactor > AudioMaxCompression) {
		AudioCompressionFactor = AudioMaxCompression;
    }
}

/**
**	Audio software amplifier.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
**
**	@todo FIXME: this does hard clipping
*/
void cSoftHdAudio::AudioSoftAmplifier(int16_t * samples, int count)
{
    int i;

    // silence
    if (AudioMute || !AudioAmplifier) {
		memset(samples, 0, count);
		return;
    }

    for (i = 0; i < count / AudioBytesProSample; ++i) {
		int t;

		t = (samples[i] * AudioAmplifier) / 1000;
		if (t < INT16_MIN) {
			t = INT16_MIN;
		} else if (t > INT16_MAX) {
			t = INT16_MAX;
		}
		samples[i] = t;
	}
}

/**
**	Set filter bands.
**
**	@param band		setting frequenz bands
**	@param onoff	set using equalizer
*/
void cSoftHdAudio::AudioSetEq(int band[17], int onoff)
{
	int i;

/*	LOGDEBUG2(L_SOUND, "AudioSetEq %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i onoff %d",
		band[0], band[1], band[2], band[3], band[4], band[5], band[6], band[7],
		band[8], band[9], band[10], band[11], band[12], band[13], band[14],
		band[15], band[16], band[17], onoff);*/

	for (i = 0; i < 18; i++) {
		switch (band[i]) {
			case 1:
				AudioEqBand[i] = 1.5;
				break;
			case 0:
				AudioEqBand[i] = 1;
				break;
			case -1:
				AudioEqBand[i] = 0.95;
				break;
			case -2:
				AudioEqBand[i] = 0.9;
				break;
			case -3:
				AudioEqBand[i] = 0.85;
				break;
			case -4:
				AudioEqBand[i] = 0.8;
				break;
			case -5:
				AudioEqBand[i] = 0.75;
				break;
			case -6:
				AudioEqBand[i] = 0.7;
				break;
			case -7:
				AudioEqBand[i] = 0.65;
				break;
			case -8:
				AudioEqBand[i] = 0.6;
				break;
			case -9:
				AudioEqBand[i] = 0.55;
				break;
			case -10:
				AudioEqBand[i] = 0.5;
				break;
			case -11:
				AudioEqBand[i] = 0.45;
				break;
			case -12:
				AudioEqBand[i] = 0.4;
				break;
			case -13:
				AudioEqBand[i] = 0.35;
				break;
			case -14:
				AudioEqBand[i] = 0.3;
				break;
			case -15:
				AudioEqBand[i] = 0.25;
				break;
		}
	}

	Filterchanged = 1;
	AudioEq = onoff;
}

/**
**	Filter init.
**
**	@retval 0	everything ok
**	@retval 1	didn't support channels, CodecDownmix set > scrap this frame, test next
**	@retval -1	something gone wrong
*/
int cSoftHdAudio::AudioFilterInit(AVCodecContext *AudioCtx)
{
	const AVFilter  *abuffer;
	AVFilterContext *filter_ctx[3];
	const AVFilter *eq;
	const AVFilter *aformat;
	const AVFilter *abuffersink;
	char ch_layout[64];
	char options_str[1024];
	int err, i, n_filter = 0;

	// Before filter init set HW parameter.
	if (AudioCtx->sample_rate != (int)HwSampleRate ||
		(AudioCtx->ch_layout.nb_channels != (int)HwChannels &&
		!(AudioDownMix && HwChannels == 2))) {

		err = AlsaSetup(AudioCtx->ch_layout.nb_channels, AudioCtx->sample_rate, 0);
		if (err)
			return err;
	}

	timebase = &AudioCtx->pkt_timebase;

#if LIBAVFILTER_VERSION_INT < AV_VERSION_INT(7,16,100)
	avfilter_register_all();
#endif

	if (!(filter_graph = avfilter_graph_alloc()))
		LOGERROR("Unable to create filter graph.");

	// input buffer
	if (!(abuffer = avfilter_get_by_name("abuffer")))
		LOGWARNING("AudioFilterInit: Could not find the abuffer filter.");
	if (!(abuffersrc_ctx = avfilter_graph_alloc_filter(filter_graph, abuffer, "src")))
		LOGWARNING("AudioFilterInit: Could not allocate the abuffersrc_ctx instance.");
	av_channel_layout_describe(&AudioCtx->ch_layout, ch_layout, sizeof(ch_layout));
	LOGDEBUG2(L_SOUND, "AudioFilterInit: IN ch_layout %s sample_fmt %s sample_rate %d channels %d",
		ch_layout, av_get_sample_fmt_name(AudioCtx->sample_fmt), AudioCtx->sample_rate, AudioCtx->ch_layout.nb_channels);
	av_opt_set    (abuffersrc_ctx, "channel_layout", ch_layout,                             AV_OPT_SEARCH_CHILDREN);
	av_opt_set    (abuffersrc_ctx, "sample_fmt",     av_get_sample_fmt_name(AudioCtx->sample_fmt), AV_OPT_SEARCH_CHILDREN);
	av_opt_set_q  (abuffersrc_ctx, "time_base",      (AVRational){ 1, AudioCtx->sample_rate }, AV_OPT_SEARCH_CHILDREN);
	av_opt_set_int(abuffersrc_ctx, "sample_rate",    AudioCtx->sample_rate,                    AV_OPT_SEARCH_CHILDREN);
//	av_opt_set_int(abuffersrc_ctx, "channel_counts", AudioCtx->channels,                    AV_OPT_SEARCH_CHILDREN);
	// initialize the filter with NULL options, set all options above.
	if (avfilter_init_str(abuffersrc_ctx, NULL) < 0)
		LOGWARNING("AudioFilterInit: Could not initialize the abuffer filter.");

	if (AudioEq) {
		// superequalizer
		if (!(eq = avfilter_get_by_name("superequalizer")))
			LOGWARNING("AudioFilterInit: Could not find the superequalizer filter.");
		if (!(filter_ctx[n_filter] = avfilter_graph_alloc_filter(filter_graph, eq, "superequalizer")))
			LOGWARNING("AudioFilterInit: Could not allocate the superequalizer instance.");
		snprintf(options_str, sizeof(options_str),"1b=%.2f:2b=%.2f:3b=%.2f:4b=%.2f:5b=%.2f"
			":6b=%.2f:7b=%.2f:8b=%.2f:9b=%.2f:10b=%.2f:11b=%.2f:12b=%.2f:13b=%.2f:14b=%.2f:"
			"15b=%.2f:16b=%.2f:17b=%.2f:18b=%.2f ", AudioEqBand[0], AudioEqBand[1],
			AudioEqBand[2], AudioEqBand[3], AudioEqBand[4], AudioEqBand[5],
			AudioEqBand[6], AudioEqBand[7], AudioEqBand[8], AudioEqBand[9],
			AudioEqBand[10], AudioEqBand[11], AudioEqBand[12], AudioEqBand[13],
			AudioEqBand[14], AudioEqBand[15], AudioEqBand[16], AudioEqBand[17]);
		if (avfilter_init_str(filter_ctx[n_filter], options_str) < 0)
			LOGWARNING("AudioFilterInit: Could not initialize the superequalizer filter.");
		n_filter++;
	}

	// aformat
	AVChannelLayout channel_layout;
	av_channel_layout_default(&channel_layout, HwChannels);
	av_channel_layout_describe(&channel_layout, ch_layout, sizeof(ch_layout));
	av_channel_layout_uninit(&channel_layout);
	// should use IN layout if more then 2 ch!?
	LOGDEBUG2(L_SOUND, "AudioFilterInit: OUT AudioDownMix %d HwChannels %d HwSampleRate %d ch_layout %s bytes_per_sample %d",
		AudioDownMix, HwChannels, HwSampleRate,
		ch_layout, av_get_bytes_per_sample(AV_SAMPLE_FMT_S16));
	if (!(aformat = avfilter_get_by_name("aformat")))
		LOGWARNING("AudioFilterInit: Could not find the aformat filter.");
	if (!(filter_ctx[n_filter] = avfilter_graph_alloc_filter(filter_graph, aformat, "aformat")))
		LOGWARNING("AudioFilterInit: Could not allocate the aformat instance.");
	snprintf(options_str, sizeof(options_str),
		"sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
		av_get_sample_fmt_name(AV_SAMPLE_FMT_S16), HwSampleRate, ch_layout);
	if (avfilter_init_str(filter_ctx[n_filter], options_str) < 0)
		LOGWARNING("AudioFilterInit: Could not initialize the aformat filter.");
	n_filter++;

	// abuffersink
	if (!(abuffersink = avfilter_get_by_name("abuffersink")))
		LOGWARNING("AudioFilterInit: Could not find the abuffersink filter.");
	if (!(filter_ctx[n_filter] = avfilter_graph_alloc_filter(filter_graph, abuffersink, "sink")))
		LOGWARNING("AudioFilterInit: Could not allocate the abuffersink instance.");
	if (avfilter_init_str(filter_ctx[n_filter], NULL) < 0)
		LOGWARNING("AudioFilterInit: Could not initialize the abuffersink instance.");
	n_filter++;

	// Connect the filters
	for (i = 0; i < n_filter; i++) {
		if (i == 0) {
			err = avfilter_link(abuffersrc_ctx, 0, filter_ctx[i], 0);
		} else {
			err = avfilter_link(filter_ctx[i - 1], 0, filter_ctx[i], 0);
		}
	}
	if (err < 0)
		LOGWARNING("AudioFilterInit: Error connecting audio filters");

	// Configure the graph.
	if (avfilter_graph_config(filter_graph, NULL) < 0)
		LOGWARNING("AudioFilterInit: Error configuring the audio filter graph");

	abuffersink_ctx = filter_ctx[n_filter - 1];
	Filterchanged = 0;
	FilterInit = 1;

	return 0;
}

//----------------------------------------------------------------------------
//	ring buffer
//----------------------------------------------------------------------------

/**
**	Setup audio ring.
*/
void cSoftHdAudio::AudioRingInit(void)
{
	// ~2s 8ch 16bit
	AudioRingBuffer = new cDeviceRingbuffer(AudioRingBufferSize);
}

/**
**	Cleanup audio ring.
*/
void cSoftHdAudio::AudioRingExit(void)
{
	if (AudioRingBuffer) {
		delete AudioRingBuffer;
		AudioRingBuffer = nullptr;
	}
	HwSampleRate = 0;	// checked for valid setup
}


//============================================================================
//	A L S A
//============================================================================

//----------------------------------------------------------------------------
//	alsa pcm
//----------------------------------------------------------------------------

/**
**   xrun recovery
**/
void cSoftHdAudio::xrun_recovery(void)
{
	int err;
	snd_pcm_state_t state;

	err = snd_pcm_prepare(AlsaPCMHandle);
	if (err < 0) {
		state = snd_pcm_state(AlsaPCMHandle);
		LOGERROR("audio/alsa: Can't recovery from xrun: %s pcm state: %s",
			snd_strerror(err), snd_pcm_state_name(state));
	}
}

/**
**	Flush alsa buffers.
*/
void cSoftHdAudio::AlsaFlushBuffers(void)
{
	int err;
	snd_pcm_state_t state;

	LOGDEBUG2(L_SOUND, "audio: AlsaFlushBuffers: AlsaFlushBuffers");

	state = snd_pcm_state(AlsaPCMHandle);
	if (state != SND_PCM_STATE_OPEN) {
		if ((err = snd_pcm_drop(AlsaPCMHandle)) < 0)
			LOGERROR("audio: AlsaFlushBuffers: snd_pcm_drop(): %s", snd_strerror(err));
		// ****ing alsa crash, when in open state here
		if ((err = snd_pcm_prepare(AlsaPCMHandle)) < 0)
			LOGERROR("audio: AlsaFlushBuffers: snd_pcm_prepare(): %s", snd_strerror(err));
	state = snd_pcm_state(AlsaPCMHandle);
	LOGDEBUG2(L_SOUND, "audio: AlsaFlushBuffers: pcm state %s", snd_pcm_state_name(state));
	}

	AudioRingBuffer->Reset();
	AudioSkip = 0;
	PTS = AV_NOPTS_VALUE;
	AudioVideoIsReady = 0;
}

//----------------------------------------------------------------------------
//	thread playback
//----------------------------------------------------------------------------

/**
**	Alsa thread
**
**	Play some samples and return.
**
**	@retval	-1	error
**	@retval	1	running
*/
int cSoftHdAudio::AlsaPlayer(void)
{
	for (;;) {
		int avail;
		int n;
		int err;
		int frames;
		const void *p;

		if (AudioPaused || AlsaPlayerStop) {
			return 1;
		}

		// wait for space in kernel buffers
		if ((err = snd_pcm_wait(AlsaPCMHandle, 150)) < 0) {
//			LOGERROR("AlsaPlayer: snd_pcm_wait error? '%s'", snd_strerror(err));
			err = snd_pcm_recover(AlsaPCMHandle, err, 0);
//			LOGERROR("AlsaPlayer: snd_pcm_wait error: snd_pcm_recover %s", snd_strerror(err));
		}

		if (AudioPaused || AlsaPlayerStop) {
			return 1;
		}

		// how many bytes can be written?
//		n = snd_pcm_avail_update(AlsaPCMHandle);
		n = snd_pcm_avail(AlsaPCMHandle);
		if (n < 0) {
			if (n == -EAGAIN) {
				continue;
			}
			err = snd_pcm_recover(AlsaPCMHandle, n, 0);
			if (err >= 0) {
				continue;
			}
			LOGERROR("audio/alsa: snd_pcm_avail_update(): %s",
				snd_strerror(n));
			return -1;
		}
		avail = snd_pcm_frames_to_bytes(AlsaPCMHandle, n);
		if (avail < 256) {		// too much overhead
			LOGDEBUG2(L_SOUND, "audio/alsa: break state '%s' avail %d",
				snd_pcm_state_name(snd_pcm_state(AlsaPCMHandle)), avail);
			break;
		}

		n = AudioRingBuffer->GetReadPointer(&p);
		if (!n) {			// ring buffer empty
// TODO render
			LOGWARNING("AlsaPlayer: ring buffer empty Videopkts: %d",
				Device->VideoStream->GetPackets());
		}
		if (n < avail) {		// not enough bytes in ring buffer
			avail = n;
		}
		if (!avail) {			// full or buffer empty
			break;
		}
		// muting pass-through AC-3, can produce disturbance
		if (AudioMute || (AudioSoftVolume
			&& !AudioPassthrough)) {
			// FIXME: quick&dirty cast
			AudioSoftAmplifier((int16_t *) p, avail);
			// FIXME: if not all are written, we double amplify them
		}

		frames = snd_pcm_bytes_to_frames(AlsaPCMHandle, avail);

		AudioRbMutex.Lock();
		if (AlsaUseMmap) {
			err = snd_pcm_mmap_writei(AlsaPCMHandle, p, frames);
		} else {
			err = snd_pcm_writei(AlsaPCMHandle, p, frames);
		}
		AudioRingBuffer->ReadAdvance(avail);
		AudioRbMutex.Unlock();
		if (err != frames) {
			if (err < 0) {
				if (err == -EAGAIN) {
					continue;
				}
				LOGWARNING("audio/alsa: writei underrun error? '%s'",
					snd_strerror(err));
				err = snd_pcm_recover(AlsaPCMHandle, err, 0);
				if (err >= 0) {
					continue;
				}
				LOGERROR("audio/alsa: snd_pcm_writei failed: %s",
					snd_strerror(err));
				return -1;
			}
			// this could happen, if underrun happened
			LOGWARNING("audio/alsa: not all frames written");
			avail = snd_pcm_frames_to_bytes(AlsaPCMHandle, err);
			break;
		}
	}
	return 0;
}

//----------------------------------------------------------------------------

char *cSoftHdAudio::opendevice(const char *device, int passthrough)
{
	int err;
	char tmp[80];

	if (!device)
		return NULL;

	LOGDEBUG2(L_SOUND, "audio/alsa: try opening %sdevice '%s'",
		passthrough ? "pass-through " : "", device);

	if (passthrough && AudioAppendAES) {
		if (!(strchr(device, ':'))) {
			sprintf(tmp, "%s:AES0=%d,AES1=%d,AES2=0,AES3=%d",
				device,
				IEC958_AES0_NONAUDIO | IEC958_AES0_PRO_EMPHASIS_NONE,
				IEC958_AES1_CON_ORIGINAL | IEC958_AES1_CON_PCM_CODER,
				IEC958_AES3_CON_FS_48000);
		} else {
			sprintf(tmp, "%s,AES0=%d,AES1=%d,AES2=0,AES3=%d",
				device,
				IEC958_AES0_NONAUDIO | IEC958_AES0_PRO_EMPHASIS_NONE,
				IEC958_AES1_CON_ORIGINAL | IEC958_AES1_CON_PCM_CODER,
				IEC958_AES3_CON_FS_48000);
		}
		LOGDEBUG2(L_SOUND, "audio/alsa: auto append AES: %s -> %s", device, tmp);
	} else {
		sprintf(tmp, "%s", device);
	}

	// open none blocking; if device is already used, we don't want wait
	if ((err = snd_pcm_open(&AlsaPCMHandle, tmp, SND_PCM_STREAM_PLAYBACK,
		SND_PCM_NONBLOCK)) < 0) {

		LOGWARNING("audio: AlsaInitPCM: could not open device '%s' error: %s", device,
			snd_strerror(err));
		return NULL;
	}

	LOGDEBUG2(L_SOUND, "audio/alsa: opened %sdevice '%s'",
		passthrough ? "pass-through " : "", device);

	return (char *)device;
}

char *cSoftHdAudio::finddevice(const char *devname, const char *hint, int passthrough)
{
	char **hints;
	int err;
	char **n;
	char *name;
	char *device = NULL;

	err = snd_device_name_hint(-1, devname, (void ***)&hints);
	if (err != 0) {
		LOGWARNING("AlsaInitPCM: Cannot get device names for %s!", hint);
		return NULL;
	}

	n = hints;
	while (*n != NULL) {
		name = snd_device_name_get_hint(*n, "NAME");

		if (strstr(name, hint)) {
			if ((device = opendevice(name, passthrough))) {
				device = (char *)malloc(sizeof(char) * (strlen(name) + 1));
				strcpy(device, name);
				free(name);
				snd_device_name_free_hint((void **)hints);
				return (char *)device;
			}
		}

		if (name && strcmp("null", name)) free(name);
		n++;
	}

	snd_device_name_free_hint((void **)hints);
	return NULL;
}

/**
**	Open alsa pcm device.
*/
void cSoftHdAudio::AlsaInitPCM(void)
{
	char *device = NULL;
	int err;
	LOGDEBUG2(L_SOUND, "AlsaInit: passthrough %d", AudioPassthrough);

	// try user set device
	if (AudioPassthrough)
		device = opendevice(AudioPassthroughDevice, AudioPassthrough);

	if (!device && AudioPassthrough)
		device = opendevice(getenv("ALSA_PASSTHROUGH_DEVICE"), AudioPassthrough);

	if (!device)
		device = opendevice(AudioPCMDevice, AudioPassthrough);

	if (!device)
		device = opendevice(getenv("ALSA_DEVICE"), AudioPassthrough);

	// walkthrough hdmi: devices
	if (!device) {
		LOGDEBUG2(L_SOUND, "AlsaInitPCM: Try hdmi: devices...");
		device = finddevice("pcm", "hdmi:", AudioPassthrough);
	}

	// walkthrough default: devices
	if (!device) {
		LOGDEBUG2(L_SOUND, "AlsaInitPCM: Try default: devices...");
		device = finddevice("pcm", "default:", AudioPassthrough);
	}

	// try default device
	if (!device) {
		LOGDEBUG2(L_SOUND, "AlsaInitPCM: Try default device...");
		device = opendevice("default", AudioPassthrough);
	}

	// use null device
	if (!device) {
		LOGDEBUG2(L_SOUND, "AlsaInitPCM: Try null device...");
		device = opendevice("null", AudioPassthrough);
	}

	if (!device)
		LOGFATAL("audio: AlsaInitPCM: could not open any device, abort!");

	if (!strcmp(device, "null"))
		LOGWARNING("audio/alsa: using %sdevice '%s'",
			AudioPassthrough ? "pass-through " : "", device);
	else
		LOGINFO("audio/alsa: using %sdevice '%s'",
			AudioPassthrough ? "pass-through " : "", device);

	if ((err = snd_pcm_nonblock(AlsaPCMHandle, 0)) < 0) {
		LOGERROR("audio/alsa: can't set block mode: %s", snd_strerror(err));
	}
}

//----------------------------------------------------------------------------
//	Alsa Mixer
//----------------------------------------------------------------------------

/**
**	Set alsa mixer volume (0-1000)
**
**	@param volume	volume (0 .. 1000)
*/
void cSoftHdAudio::AlsaSetVolume(int volume)
{
    int v;
    if (AlsaMixer && AlsaMixerElem) {
		v = (volume * AlsaRatio) / (1000 * 1000);
		snd_mixer_selem_set_playback_volume(AlsaMixerElem, SND_MIXER_SCHN_FRONT_LEFT, v);
		snd_mixer_selem_set_playback_volume(AlsaMixerElem, SND_MIXER_SCHN_FRONT_RIGHT, v);
    }
}

/**
**	Initialize alsa mixer.
*/
void cSoftHdAudio::AlsaInitMixer(void)
{
    const char *device;
    const char *channel;
    snd_mixer_t *alsa_mixer;
    snd_mixer_elem_t *alsa_mixer_elem;
    long alsa_mixer_elem_min;
    long alsa_mixer_elem_max;

    if (!(device = AudioMixerDevice)) {
		if (!(device = getenv("ALSA_MIXER"))) {
			device = "default";
		}
    }
    if (!(channel = AudioMixerChannel)) {
		if (!(channel = getenv("ALSA_MIXER_CHANNEL"))) {
			channel = "PCM";
		}
    }
    LOGDEBUG2(L_SOUND, "audio/alsa: mixer %s - %s open", device, channel);
    snd_mixer_open(&alsa_mixer, 0);
    if (alsa_mixer && snd_mixer_attach(alsa_mixer, device) >= 0
		&& snd_mixer_selem_register(alsa_mixer, NULL, NULL) >= 0
		&& snd_mixer_load(alsa_mixer) >= 0) {

		const char *const alsa_mixer_elem_name = channel;

		alsa_mixer_elem = snd_mixer_first_elem(alsa_mixer);
		while (alsa_mixer_elem) {
			const char *name;

			name = snd_mixer_selem_get_name(alsa_mixer_elem);
			if (!strcasecmp(name, alsa_mixer_elem_name)) {
			snd_mixer_selem_get_playback_volume_range(alsa_mixer_elem,
				&alsa_mixer_elem_min, &alsa_mixer_elem_max);
			AlsaRatio = 1000 * (alsa_mixer_elem_max - alsa_mixer_elem_min);
			LOGDEBUG2(L_SOUND, "audio/alsa: PCM mixer found %ld - %ld ratio %d",
				alsa_mixer_elem_min, alsa_mixer_elem_max, AlsaRatio);
			break;
			}

			alsa_mixer_elem = snd_mixer_elem_next(alsa_mixer_elem);
		}

		AlsaMixer = alsa_mixer;
		AlsaMixerElem = alsa_mixer_elem;
    } else {
		LOGERROR("audio/alsa: can't open mixer '%s'", device);
    }
}

//----------------------------------------------------------------------------
//	Alsa API
//----------------------------------------------------------------------------

/**
**	Setup alsa audio for requested format.
**
**	@param channels		Channels requested
**	@param sample_rate	SampleRate requested
**	@param passthrough	use pass-through (AC-3, ...) device
**
**	@retval 0	everything ok
**	@retval 1	didn't support hw channels, CodecDownmix set > retest
**	@retval -1	something gone wrong
**
**	@todo FIXME: remove pointer for freq + channels
*/
int cSoftHdAudio::AlsaSetup(int channels, int sample_rate, int passthrough)
{
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_state_t state;
	int err;
	int delay;
	unsigned buffer_time = 100000;	// 100ms

	AudioDownMix = 0;

	if (AudioRunning) {
		LOGDEBUG2(L_SOUND, "AlsaSetup: Audio is Running => AudioFlushBuffers");
		AudioFlushBuffers();
	}

	state = snd_pcm_state(AlsaPCMHandle);
	if (state == SND_PCM_STATE_XRUN) {
		LOGERROR("audio/AlsaSetup: recover from xrun pcm state: %s",
			snd_pcm_state_name(state));
		xrun_recovery();
	}

	snd_pcm_hw_params_alloca(&hwparams);
	if ((err = snd_pcm_hw_params_any(AlsaPCMHandle, hwparams)) < 0) {
		LOGERROR("AlsaSetup: Read HW config failed! %s", snd_strerror(err));
		return -1;
	}

	if (!snd_pcm_hw_params_test_access(AlsaPCMHandle, hwparams, SND_PCM_ACCESS_MMAP_INTERLEAVED)) {
		AlsaUseMmap = 1;
	}

	HwSampleRate = sample_rate;
	if ((err = snd_pcm_hw_params_set_rate_near(AlsaPCMHandle, hwparams, &HwSampleRate, 0) < 0)) {
		LOGERROR("AlsaSetup: SampleRate %d not supported! %s",
			sample_rate, snd_strerror(err));
		return -1;
	}
	if ((int)HwSampleRate != sample_rate) {
		LOGDEBUG2(L_SOUND, "AlsaSetup: sample_rate %d HwSampleRate %d",
			sample_rate, HwSampleRate);
	}

	HwChannels = channels;
	if ((err = snd_pcm_hw_params_set_channels_near(AlsaPCMHandle, hwparams, &HwChannels)) < 0) {
		LOGWARNING("AlsaSetup: %d channels not supported! %s",
			HwChannels, snd_strerror(err));
	}
	if ((int)HwChannels != channels && !passthrough) {
		AudioDownMix = 1;
	}

	if ((err = snd_pcm_hw_params_set_buffer_time_near(AlsaPCMHandle, hwparams, &buffer_time, NULL)) < 0) {
		LOGWARNING("AlsaSetup: buffer_time %d not supported! %s",
			buffer_time, snd_strerror(err));
	}

	AlsaCanPause = snd_pcm_hw_params_can_pause(hwparams);

/*	err = snd_pcm_hw_params_test_format(AlsaPCMHandle, hwparams, SND_PCM_FORMAT_S16);
	if (err < 0)	// err == 0 if is supported
		LOGERROR("AlsaSetup: SND_PCM_FORMAT_S16 not supported! %s",
			snd_strerror(err));
*/
	if ((err = snd_pcm_set_params(AlsaPCMHandle, SND_PCM_FORMAT_S16,
		AlsaUseMmap ? SND_PCM_ACCESS_MMAP_INTERLEAVED :
		SND_PCM_ACCESS_RW_INTERLEAVED, HwChannels, HwSampleRate, 1,
		buffer_time))) {

		state = snd_pcm_state(AlsaPCMHandle);
		LOGERROR("audio/alsa: set params error: %s\n"
			"AlsaSetup: Channels %d SampleRate %d\n"
			"           HWChannels %d HWSampleRate %d SampleFormat %s\n"
			"           Supports pause: %s mmap: %s\n"
			"           AlsaBufferTime %dms pcm state: %s",
			snd_strerror(err), channels, sample_rate, HwChannels,
			HwSampleRate, snd_pcm_format_name(SND_PCM_FORMAT_S16),
			AlsaCanPause ? "yes" : "no", AlsaUseMmap ? "yes" : "no",
			buffer_time, snd_pcm_state_name(state));
		return -1;
	}

	// update buffer
	AudioStartThreshold = (buffer_time / 1000) * (HwSampleRate / 1000) *
		HwChannels * AudioBytesProSample;

	// buffer time/delay in ms
	delay = AudioBufferTime;
	if (Device->GetVideoAudioDelay() > 0) {
		delay += Device->GetVideoAudioDelay();
	}
	if (AudioStartThreshold <
		(HwSampleRate * HwChannels * AudioBytesProSample * delay) / 1000U) {

		AudioStartThreshold =
			(HwSampleRate * HwChannels * AudioBytesProSample * delay) / 1000U;
	}
	// no bigger, than 1/3 the buffer
	if (AudioStartThreshold > AudioRingBufferSize / 3) {
		AudioStartThreshold = AudioRingBufferSize / 3;
	}

	LOGINFO("AlsaSetup: Channels %d SampleRate %d%s\n"
		"           HWChannels %d HWSampleRate %d SampleFormat %s\n"
		"           Supports pause: %s mmap: %s\n"
		"           AlsaBufferTime %dms AudioBufferTime %dms Threshold %ums",
		channels, sample_rate, passthrough ? " -> passthrough" : "",
		HwChannels, HwSampleRate,
		snd_pcm_format_name(SND_PCM_FORMAT_S16),
		AlsaCanPause ? "yes" : "no", AlsaUseMmap ? "yes" : "no",
		buffer_time / 1000, AudioBufferTime, (AudioStartThreshold * 1000) /
		(HwSampleRate * HwChannels * AudioBytesProSample));
	return 0;
}

/**
**	Empty log callback
*/
static void AlsaNoopCallback( __attribute__ ((unused))
    const char *file, __attribute__ ((unused))
    int line, __attribute__ ((unused))
    const char *function, __attribute__ ((unused))
    int err, __attribute__ ((unused))
    const char *fmt, ...)
{
}

/**
**	Initialize alsa audio output module.
*/
void cSoftHdAudio::AlsaInit(void)
{
#ifdef ALSA_DEBUG
    (void)AlsaNoopCallback;
#else
    // disable display of alsa error messages
    snd_lib_error_set_handler(AlsaNoopCallback);
#endif

    AudioBufferTime = MIN_AUDIO_BUFFER;
    AlsaInitPCM();
    AlsaInitMixer();
}

/**
**	Cleanup alsa audio output module.
*/
void cSoftHdAudio::AlsaExit(void)
{
    if (AlsaPCMHandle) {
		snd_pcm_close(AlsaPCMHandle);
		AlsaPCMHandle = NULL;
    }
    if (AlsaMixer) {
		snd_mixer_close(AlsaMixer);
		AlsaMixer = NULL;
		AlsaMixerElem = NULL;
    }
}

// --- ab hier weiter

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

/**
**	Place samples in audio output queue.
**
**	@param frame	audio frame
*/
void cSoftHdAudio::AudioEnqueue(AVFrame *frame)
{
	size_t n;
	int16_t *buffer;

	if (AlsaPlayerStop) {
		av_frame_unref(frame);
//		LOGDEBUG2(L_SOUND, "AudioEnqueue: AlsaPlayerStop!!!");
		return;
	}

	int count = frame->nb_samples * frame->ch_layout.nb_channels * AudioBytesProSample;
	buffer = (int16_t *)frame->data[0];

	if (AudioCompression) {		// in place operation
		AudioCompressor(buffer, count);
	}
	if (AudioNormalize) {		// in place operation
		AudioNormalizer(buffer, count);
	}

	AudioReorderAudioFrame(buffer, count, frame->ch_layout.nb_channels);

	AudioRbMutex.Lock();
	n = AudioRingBuffer->Write(buffer, count);
	if (n != (size_t) count)
		LOGERROR("audio: AudioEnqueue: can't place %d samples in ring buffer", count);
	PTS = frame->pts + (frame->nb_samples * timebase->den /
		timebase->num / frame->sample_rate);
	AudioRbMutex.Unlock();

	if (!AudioRunning && !AudioPaused) {		// check, if we can start the thread
		int skip;

		n = AudioRingBuffer->UsedBytes();
		skip = AudioSkip;
		// FIXME: round to packet size

		LOGDEBUG2(L_AV_SYNC, "AudioEnqueue: start? in Rb %4zdms to skip %dms nb_samples %d",
			n * 1000 / HwSampleRate / HwChannels / AudioBytesProSample,
			skip * 1000 / HwSampleRate / HwChannels / AudioBytesProSample,
			frame->nb_samples);
		if (skip) {
			if (n < (unsigned)skip) {
				skip = n;
			}
			AudioSkip -= skip;
			AudioRingBuffer->ReadAdvance(skip);
			n = AudioRingBuffer->UsedBytes();
		}
		// forced start or enough video + audio buffered
		// for some exotic channels * 4 too small
		if ((AudioVideoIsReady && AudioStartThreshold < n) ||
			AudioStartThreshold * 4 < n) {
			// restart play-back
			// no lock needed, can wakeup next time
			LOGDEBUG2(L_AV_SYNC, "AudioEnqueue: start play-back Threshold %ums RingBuffer %zums AudioVideoIsReady %d",
				AudioStartThreshold * 1000 / HwSampleRate / HwChannels / AudioBytesProSample,
				n * 1000 / HwSampleRate / HwChannels / AudioBytesProSample,
				AudioVideoIsReady);
			AudioRunning = 1;
			AudioThread->SendStartSignal();
		}
	}
	av_frame_free(&frame);
}

/**
**	Place samples in spdif audio output queue
**
**	@param samples		sample buffer
**	@param count		number of bytes in sample buffer
**	@param frame		decoded frame (used to get frame parameters), unref'd at the end
*/
void cSoftHdAudio::AudioEnqueueSpdif(AVCodecContext *ctx, const uint16_t *samples, int count, AVFrame *frame)
{
	size_t n;
	const uint16_t *buffer;

	timebase = &ctx->pkt_timebase;

	if (AlsaPlayerStop) {
//		LOGDEBUG2(L_SOUND, "AudioEnqueueSpdif: AlsaPlayerStop!!!");
		av_frame_unref(frame);
		return;
	}

	buffer = samples;

	AudioRbMutex.Lock();
	n = AudioRingBuffer->Write(buffer, count);
	if (n != (size_t) count)
		LOGERROR("audio: AudioEnqueueSpdif: can't place %d samples in ring buffer", count);

	PTS = frame->pts + (frame->nb_samples * timebase->den /
		timebase->num / frame->sample_rate);
	AudioRbMutex.Unlock();

	if (!AudioRunning && !AudioPaused) {		// check, if we can start the thread
		int skip;

		n = AudioRingBuffer->UsedBytes();
		skip = AudioSkip;
		// FIXME: round to packet size

		LOGDEBUG2(L_AV_SYNC, "AudioEnqueueSpdif: start? in Rb %4zdms to skip %dms nb_samples %d",
			n * 1000 / HwSampleRate / HwChannels / AudioBytesProSample,
			skip * 1000 / HwSampleRate / HwChannels / AudioBytesProSample,
			frame->nb_samples);
		if (skip) {
			if (n < (unsigned)skip) {
				skip = n;
			}
			AudioSkip -= skip;
			AudioRingBuffer->ReadAdvance(skip);
			n = AudioRingBuffer->UsedBytes();
		}
		// forced start or enough video + audio buffered
		// for some exotic channels * 4 too small
		if ((AudioVideoIsReady && AudioStartThreshold < n) ||
			AudioStartThreshold * 4 < n) {
			// restart play-back
			// no lock needed, can wakeup next time
			LOGDEBUG2(L_AV_SYNC, "AudioEnqueueSpdif: start play-back Threshold %ums RingBuffer %zums AudioVideoIsReady %d",
				AudioStartThreshold * 1000 / HwSampleRate / HwChannels / AudioBytesProSample,
				n * 1000 / HwSampleRate / HwChannels / AudioBytesProSample,
				AudioVideoIsReady);
			AudioRunning = 1;
			AudioThread->SendStartSignal();
		}
	}
	av_frame_unref(frame);
}

/**
**	Setup alsa (only used for passthrough atm)
**	setting up PCM goes via AudioFilter()
**
**	@param AudioCtx		decoding context
**	@param samplerate	stream samplerate
**	@param channels		stream nb of channels
**	@param passthrough	passthrough enabled
**
**	@retval 0	everything ok
**	@retval err	something gone wrong
*/
int cSoftHdAudio::AudioSetup(AVCodecContext *AudioCtx, int samplerate, int channels, int passthrough)
{
	int err = 0;

	if (samplerate != (int)HwSampleRate ||
	   (channels != (int)HwChannels && !(AudioDownMix && HwChannels == 2))) {

		err = AlsaSetup(channels, samplerate, passthrough);
		if (err) {
			LOGDEBUG2(L_SOUND, "AudioSetup: failed!");
			return err;
		}
	}
	timebase = &AudioCtx->pkt_timebase;

	return 0;
}

/**
**	audio filter
**
**	@retval	1	error, send again
**	@retval	0	running
*/
void cSoftHdAudio::AudioFilter(AVFrame *inframe, AVCodecContext *AudioCtx)
{
	AVFrame *outframe = NULL;
	int err;
	int err_count = 0;

	if (!inframe) {
//		LOGDEBUG2(L_SOUND, "AudioFilter: NO inframe!!!");
		goto get_frame;
	}
in:
	if (FilterInit && Filterchanged) {

//		LOGDEBUG2(L_SOUND, "AudioFilter: FilterInit %d sink_links_count %d channels %d nb_filters %d nb_outputs %d channels %d Filterchanged %d",
//			FilterInit,
//			filter_graph->sink_links_count, filter_graph->sink_links[0]->channels,
//			filter_graph->filters[filter_graph->nb_filters - 1]->nb_outputs,
//			filter_graph->nb_filters, filter_graph->filters[filter_graph->nb_filters - 1]->outputs[filter_graph->filters[filter_graph->nb_filters - 1]->nb_outputs - 1]->channels,
//			Filterchanged);

		avfilter_graph_free(&filter_graph);
		FilterInit = 0;
		LOGDEBUG2(L_SOUND, "AudioFilter: Free the filter graph.");
	}

	if (!FilterInit) {
		err = AudioFilterInit(AudioCtx);
		if (err) {
			LOGDEBUG2(L_SOUND, "AudioFilter: AudioFilterInit failed!");
			return;
		}
	}

	if ((err = av_buffersrc_add_frame(abuffersrc_ctx, inframe)) < 0) {
		if (err_count) {
			char errbuf[128];
			av_strerror(err, errbuf, sizeof(errbuf));
			LOGERROR("AudioFilter: Error submitting the frame to the filter fmt %s channels %d %s",
				av_get_sample_fmt_name(AudioCtx->sample_fmt), AudioCtx->ch_layout.nb_channels, errbuf);
			return;
		} else {
			Filterchanged = 1;
			err_count++;
			LOGDEBUG2(L_SOUND, "AudioFilter: Filterchanged %d  err_count %d", Filterchanged, err_count);
			goto in;
		}
	}

get_frame:
	outframe  = av_frame_alloc();
	err = av_buffersink_get_frame(abuffersink_ctx, outframe);

	if (err == AVERROR(EAGAIN)) {
//		LOGERROR("AudioFilter: Error filtering AVERROR(EAGAIN)");
		av_frame_free(&outframe);
	} else if (err == AVERROR_EOF) {
		LOGERROR("AudioFilter: Error filtering AVERROR_EOF");
		av_frame_free(&outframe);
	} else if (err < 0) {
		LOGERROR("AudioFilter: Error filtering the data");
		av_frame_free(&outframe);
	}

	if (outframe)
		AudioEnqueue(outframe);
}

/**
**	Video is ready.
**
**	@param video_pts	real video presentation timestamp
*/
int cSoftHdAudio::AudioVideoReady(int64_t video_pts)
{
	int64_t audio_pts;
	int64_t used;
	int skip;

	if (AudioRunning) {
		LOGDEBUG2(L_SOUND, "AudioVideoReady: Audio is Running !!!???");
		return 0;
	}

	// no valid audio known
	if (PTS == AV_NOPTS_VALUE) {
		LOGDEBUG2(L_SOUND, "AudioVideoReady: can't a/v start, no valid PTS");
		return -1;
	}

	used = AudioRingBuffer->UsedBytes();
	audio_pts = PTS * 1000 * av_q2d(*timebase) -
				used * 1000 / HwSampleRate / HwChannels / AudioBytesProSample;

	skip = video_pts - audio_pts - Device->GetVideoAudioDelay();

	if (skip > 0) {
		skip = (int64_t)skip * HwSampleRate * HwChannels * AudioBytesProSample / 1000;

		//skip must be a multiple of HwChannels * AudioBytesProSample
		int frames = skip / HwChannels / AudioBytesProSample;
		skip = frames * HwChannels * AudioBytesProSample;

		if ((unsigned)skip > used) {
			AudioSkip = skip - used;
			skip = used;
		}
		LOGDEBUG2(L_AV_SYNC, "AudioVideoReady: RB %" PRId64 "ms skip %dms to skip %dms",
			used * 1000 / HwSampleRate / HwChannels / AudioBytesProSample,
			skip * 1000 / HwSampleRate / HwChannels / AudioBytesProSample,
			AudioSkip * 1000 / HwSampleRate / HwChannels / AudioBytesProSample);
		AudioRingBuffer->ReadAdvance(skip);

		used = AudioRingBuffer->UsedBytes();
	}

	// enough audio buffered
	if (AudioStartThreshold < used) {
		AudioRunning = 1;
		AudioThread->SendStartSignal();
	}
	AudioVideoIsReady = 1;
	return 0;
}

/**
**	Skip Audio in trickspeed.
**
**	@param video_pts	real video presentation timestamp
*/
int cSoftHdAudio::AudioSkipInTrickSpeed(int64_t video_pts, int full)
{
	int64_t audio_pts;
	int64_t used;
	int skip;

	// no valid audio known, if we had a ClearAudio just before
	if (PTS == AV_NOPTS_VALUE) {
		LOGDEBUG2(L_AV_SYNC, "AudioSkipInTrickSpeed: can't sync, no valid PTS");
		return -1;
	}

	// no valid video pts
	if (video_pts == AV_NOPTS_VALUE) {
		LOGDEBUG2(L_AV_SYNC, "AudioSkipInTrickSpeed: can't sync, no valid video PTS");
		return -1;
	}

	while (1) {
		used = AudioRingBuffer->UsedBytes(); // in bytes

		if (used * 1000 / HwSampleRate / HwChannels / AudioBytesProSample == 0)
			break;

		audio_pts = PTS * 1000 * av_q2d(*timebase) -
					used * 1000 / HwSampleRate / HwChannels / AudioBytesProSample;

		skip = video_pts * 1000 * av_q2d(*timebase) - audio_pts - Device->GetVideoAudioDelay(); // in ms

		if (skip <= 0) // audio >= video
			break;

		skip = (int64_t)skip * HwSampleRate * HwChannels * AudioBytesProSample / 1000;

		//skip must be a multiple of HwChannels * AudioBytesProSample
		int frames = skip / HwChannels / AudioBytesProSample;
		skip = frames * HwChannels * AudioBytesProSample;

		if ((unsigned)skip >= used)
			skip = used - (1 - full) * HwChannels * AudioBytesProSample;

		LOGDEBUG2(L_AV_SYNC, "AudioSkipInTrickSpeed: RB %" PRId64 "ms skip %dms audio %s -> %s video %s",
			used * 1000 / HwSampleRate / HwChannels / AudioBytesProSample,
			skip * 1000 / HwSampleRate / HwChannels / AudioBytesProSample,
			Timestamp2String(audio_pts),
			Timestamp2String(audio_pts + skip * 1000 / HwSampleRate / HwChannels / AudioBytesProSample),
			Timestamp2String(video_pts * 1000 * av_q2d(*timebase)));

		AudioRingBuffer->ReadAdvance(skip);
	}

	return 0;
}

/**
**	Flush audio buffers.
*/
void cSoftHdAudio::AudioFlushBuffers(void)
{
	LOGDEBUG2(L_SOUND, "AudioFlushBuffers: AudioFlushBuffers");

	if (AudioRunning)
		AlsaPlayerStop = 1;
	else if (PTS != AV_NOPTS_VALUE)
		AlsaFlushBuffers();

	while(AudioRunning) {
		usleep(5000);
	}

	Filterchanged = 1;
}

/**
**	Get free bytes in audio output.
*/
int cSoftHdAudio::AudioFreeBytes(void)
{
    return AudioRingBuffer ?
		AudioRingBuffer->FreeBytes()
		: INT32_MAX;
}

/**
**	Get used bytes in audio output.
*/
int cSoftHdAudio::AudioUsedBytes(void)
{
    // FIXME: not correct, if multiple buffer are in use
    return AudioRingBuffer ?
	AudioRingBuffer->UsedBytes() : 0;
}

/**
**	Get current audio clock.
**
**	@returns the audio clock in time stamps.
*/
int64_t cSoftHdAudio::AudioGetClock(void)
{
	if (!AudioRunning || !HwSampleRate ||
		!AlsaPCMHandle || PTS == AV_NOPTS_VALUE) {

		return AV_NOPTS_VALUE;
	}
	snd_pcm_sframes_t delay;
	int64_t pts;
	int64_t ret;

	AudioRbMutex.Lock();
	// delay in frames in alsa + kernel buffers
	if (snd_pcm_delay(AlsaPCMHandle, &delay) < 0) {
		if (!AudioPaused)
			LOGDEBUG2(L_SOUND, "AudioGetClock: no hw delay");
		delay = 0L;
	}

	if (delay < 0) {
		LOGDEBUG2(L_SOUND, "AudioGetClock: delay < 0");
		delay = 0L;
	}

	pts = (int64_t)delay * 1000 / HwSampleRate;

	pts += (int64_t)AudioRingBuffer->UsedBytes() * 1000 /
			HwSampleRate / HwChannels / AudioBytesProSample;

	ret = PTS * 1000 * av_q2d(*timebase) - pts;
	AudioRbMutex.Unlock();

	return ret;
}

/**
**	Set mixer volume (0-1000)
**
**	@param volume	volume (0 .. 1000)
*/
void cSoftHdAudio::AudioSetVolume(int volume)
{
    AudioVolume = volume;
    AudioMute = !volume;
    // reduce loudness for stereo output
    if (AudioStereoDescent && HwChannels == 2 && !AudioPassthrough) {
		volume -= AudioStereoDescent;
		if (volume < 0) {
			volume = 0;
		} else if (volume > 1000) {
			volume = 1000;
		}
    }
    AudioAmplifier = volume;
    if (!AudioSoftVolume) {
		AlsaSetVolume(volume);
    }
}

/**
**	Play audio.
*/
void cSoftHdAudio::AudioPlay(void)
{
	int err;

	if (!AudioPaused && !AlsaCanPause) {
		LOGDEBUG2(L_SOUND, "AudioPlay: not paused, check the code");
		return;
	}
	LOGDEBUG2(L_SOUND, "AudioPlay: resume");
	if (AlsaCanPause) {
		snd_pcm_state_t state;
		state = snd_pcm_state(AlsaPCMHandle);
		if (state == SND_PCM_STATE_PAUSED) {
			LOGDEBUG2(L_SOUND, "AudioPlay: state paused, try snd_pcm_pause(0)!");
			if ((err = snd_pcm_pause(AlsaPCMHandle, 0))) {
				LOGERROR("AudioPlay: snd_pcm_pause(): %s", snd_strerror(err));
			}
		}
	} else {
		AudioPaused = 0;
		if (AudioStartThreshold < AudioRingBuffer->UsedBytes()) {
			AudioThread->SendStartSignal();
		}
	}
}

/**
**	Pause audio.
*/
void cSoftHdAudio::AudioPause(void)
{
	int err;

	if (AudioPaused) {
		LOGDEBUG2(L_SOUND, "AudioPause: already paused, check the code");
	return;
	}
	LOGDEBUG2(L_SOUND, "AudioPause: paused");
	if (AlsaCanPause) {
		snd_pcm_state_t state;
		state = snd_pcm_state(AlsaPCMHandle);
		if (state == SND_PCM_STATE_RUNNING) {
			LOGDEBUG2(L_SOUND, "AudioPlay: state running, try snd_pcm_pause(1)!");
			if ((err = snd_pcm_pause(AlsaPCMHandle, 1))) {
				LOGERROR("AudioPause: snd_pcm_pause(): %s", snd_strerror(err));
			}
		}
	} else {
		AudioPaused = 1;
	}
}

/**
**	Set audio buffer time.
**
**	PES audio packets have a max distance of 300 ms.
**	TS audio packet have a max distance of 100 ms.
**	The period size of the audio buffer is 24 ms.
**	With streamdev sometimes extra +100ms are needed.
*/
void cSoftHdAudio::AudioSetBufferTime(int delay)
{
	AudioBufferTime = MIN_AUDIO_BUFFER + delay;
}

/**
**	Set audio downmix.
**
**	@param onoff	enable/disable downmix.
*/
void cSoftHdAudio::AudioSetDownmix(int onoff)
{
	if (onoff == -1) {
		AudioDownMix ^= 1;
		return;
	}
	AudioDownMix = onoff;
}

/**
**	Enable/disable software volume.
**
**	@param onoff	-1 toggle, true turn on, false turn off
*/
void cSoftHdAudio::AudioSetSoftvol(int onoff)
{
    if (onoff < 0) {
	AudioSoftVolume ^= 1;
    } else {
	AudioSoftVolume = onoff;
    }
}

/**
**	Set normalize volume parameters.
**
**	@param onoff	-1 toggle, true turn on, false turn off
**	@param maxfac	max. factor of normalize /1000
*/
void cSoftHdAudio::AudioSetNormalize(int onoff, int maxfac)
{
    if (onoff < 0) {
	AudioNormalize ^= 1;
    } else {
	AudioNormalize = onoff;
    }
    AudioMaxNormalize = maxfac;
}

/**
**	Set volume compression parameters.
**
**	@param onoff	-1 toggle, true turn on, false turn off
**	@param maxfac	max. factor of compression /1000
*/
void cSoftHdAudio::AudioSetCompression(int onoff, int maxfac)
{
    if (onoff < 0) {
		AudioCompression ^= 1;
    } else {
		AudioCompression = onoff;
    }
    AudioMaxCompression = maxfac;
    if (!AudioCompressionFactor) {
		AudioCompressionFactor = 1000;
    }
    if (AudioCompressionFactor > AudioMaxCompression) {
		AudioCompressionFactor = AudioMaxCompression;
    }
}

/**
**	Set stereo loudness descent.
**
**	@param delta	value (/1000) to reduce stereo volume
*/
void cSoftHdAudio::AudioSetStereoDescent(int delta)
{
    AudioStereoDescent = delta;
    AudioSetVolume(AudioVolume);	// update channel delta
}

/**
**	Set pcm audio device.
**
**	@param device	name of pcm device (fe. "hw:0,9")
**
**	@note this is currently used to select alsa output module.
*/
void cSoftHdAudio::AudioSetDevice(const char *device)
{
    AudioPCMDevice = device;
}

/**
**	Set pass-through audio device.
**
**	@param device	name of pass-through device (fe. "hw:0,1")
**
**	@note this is currently usable with alsa only.
*/
void cSoftHdAudio::AudioSetPassthroughDevice(const char *device)
{
    AudioPassthroughDevice = device;
}

/**
**	Set audio pass-through mask
**
**	@param mask	passthrough-mask
*/
void cSoftHdAudio::AudioSetPassthrough(int mask)
{
    AudioPassthrough = mask;
}

/**
**	Set pcm audio mixer channel.
**
**	@param channel	name of the mixer channel (fe. PCM or Master)
**
**	@note this is currently used to select alsa output module.
*/
void cSoftHdAudio::AudioSetChannel(const char *channel)
{
    AudioMixerChannel = channel;
}

/**
**	Set automatic AES flag handling.
**
**	@param onoff	turn setting AES flag on or off
*/
void cSoftHdAudio::AudioSetAutoAES(int onoff)
{
    if (onoff < 0) {
	AudioAppendAES ^= 1;
    } else {
	AudioAppendAES = onoff;
    }
}

/**
**	Initialize audio output module.
**
**	@param passthrough	passthrough enabled
**
**	@todo FIXME: make audio output module selectable.
*/
void cSoftHdAudio::AudioInit(int passthrough)
{
	AudioPassthrough = passthrough;
	AudioRingInit();
	AlsaInit();
	AudioThread = new cAudioHandlerThread(this);
}

/**
**	Cleanup audio output module.
*/
void cSoftHdAudio::AudioExit(void)
{
	LOGDEBUG2(L_SOUND, "audio: %s", __FUNCTION__);

	AudioThread->Stop();
	delete AudioThread;

	AlsaExit();
	AudioRingExit();
	AudioRunning = 0;
	AudioPaused = 0;
}
