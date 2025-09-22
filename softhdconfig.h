/**
 * @file softhdconfig.h
 * @brief SoftHdDevice config header file
 *
 * Copyright: (c) 2011, 2015 by Johns.  All Rights Reserved.
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
#ifndef __SOFTHDCONFIG_H
#define __SOFTHDCONFIG_H

#include "softhddevice.h"
#include "audio.h"

/*****************************************************************************
 * Config
 ****************************************************************************/
class cSoftHdConfig
{
public:
	cSoftHdConfig(void);
	virtual ~cSoftHdConfig(void);

	bool SetupParse(const char *, const char *);
#ifdef USE_GLES
#ifdef WRITE_PNG
	char ConfigWritePngs;                   ///< config write pngs from OSD
#endif
	int ConfigMaxSizeGPUImageCache = 128;   ///< config max gpu image cache size
	int ConfigDisableOglOsd;                ///< config disable ogl osd
#endif
	int ConfigVideoAudioDelay;              ///< config audio delay
	char ConfigAudioPassthrough;            ///< config audio pass-through mask
	char AudioPassthroughState;             ///< flag audio-passthrough on/off
	char ConfigAudioDownmix;                ///< config ffmpeg audio downmix
	char ConfigAudioSoftvol;                ///< config use software volume
	char ConfigAudioNormalize;              ///< config use normalize volume
	int ConfigAudioMaxNormalize;            ///< config max normalize factor
	char ConfigAudioCompression;            ///< config use volume compression
	int ConfigAudioMaxCompression;          ///< config max volume compression
	int ConfigAudioStereoDescent;           ///< config reduce stereo loudness
	int ConfigAudioBufferTime;              ///< config size ms of audio buffer
	int ConfigAudioAutoAES;                 ///< config automatic AES handling
	int ConfigAudioEq;                      ///< config equalizer filter 
	int SetupAudioEqBand[18];               ///< config equalizer filter bands

	char ConfigMakePrimary;                 ///< config primary wanted
	char ConfigHideMainMenuEntry;           ///< config hide main menu entry
	char LogState;                          ///< flag logging on/off
	int ConfigLog;                          ///< loglevel config

	int ConfigDisableDeint;                 ///< disable deinterlacer

private:
	cSoftHdDevice *m_pDevice;
	cSoftHdAudio *m_pAudio;
};

#endif
