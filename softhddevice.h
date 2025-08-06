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

#ifndef __SOFTHDDEVICE_H
#define __SOFTHDDEVICE_H

#include <vdr/dvbspu.h>

extern "C"
{
#include <libavcodec/avcodec.h>
}

class cAudioDecoder;

//////////////////////////////////////////////////////////////////////////////
//	cDevice
//////////////////////////////////////////////////////////////////////////////

class cVideoStream;
class cVideoRender;
class cSoftHdAudio;

class cSoftHdDevice:public cDevice
{
  public:
    cSoftHdDevice(void);
    virtual ~ cSoftHdDevice(void);

    virtual cString DeviceName(void) const { return "softhddevice-drm-gles"; }

    virtual bool HasDecoder(void) const;
    virtual bool HasIBPTrickSpeed(void) const;
    virtual bool CanReplay(void) const;
    virtual bool SetPlayMode(ePlayMode);
    virtual void TrickSpeed(int, bool);
    virtual void Clear(void);
    virtual void Play(void);
    virtual void Freeze(void);
    virtual void Mute(void);
    virtual void StillPicture(const uchar *, int);
    virtual bool Poll(cPoller &, int = 0);
    virtual bool Flush(int = 0);
    virtual int64_t GetSTC(void);
    virtual cRect CanScaleVideo(const cRect &, int taCenter);
    virtual void ScaleVideo(const cRect & = cRect::Null);
    virtual void SetVideoDisplayFormat(eVideoDisplayFormat);
    virtual void SetVideoFormat(bool);
    virtual void GetVideoSize(int &, int &, double &);
    virtual void GetOsdSize(int &, int &, double &);
    virtual int PlayVideo(const uchar *, int);
    virtual int PlayAudio(const uchar *, int, uchar);
    virtual void SetAudioChannelDevice(int);
    virtual int GetAudioChannelDevice(void);
    virtual void SetDigitalAudioDevice(bool);
    virtual void SetAudioTrackDevice(eTrackType);
    virtual void SetVolumeDevice(int);
private:
    int PesHeadLength(const uint8_t *);

public:

// Image Grab facilities

    virtual uchar *GrabImage(int &, bool, int, int, int);

// SPU facilities
  private:
    cDvbSpuDecoder * spuDecoder;
  public:
    virtual cSpuDecoder * GetSpuDecoder(void);

  protected:
    virtual void MakePrimaryDevice(bool);

  public:
    void Start(void);
    void Stop(void);
    void Exit(void);
    const char *CommandLineHelp(void);
    int ProcessArgs(int, char *[]);

// config
#ifdef USE_GLES
#ifdef WRITE_PNG
  private:
    char ConfigWritePngs;		///< config write pngs from OSD
  public:
    void SetConfigWritePngs(char value) { ConfigWritePngs = value; };
    char GetConfigWritePngs(void) { return ConfigWritePngs; };
#endif
#endif

// Menu config variables
  public:
    int ConfigVideoAudioDelay;		///< config audio delay
    char ConfigAudioPassthrough;	///< config audio pass-through mask
    char AudioPassthroughState;		///< flag audio-passthrough on/off
    char ConfigAudioDownmix;		///< config ffmpeg audio downmix
    char ConfigAudioSoftvol;		///< config use software volume
    char ConfigAudioNormalize;		///< config use normalize volume
    int ConfigAudioMaxNormalize;	///< config max normalize factor
    char ConfigAudioCompression;	///< config use volume compression
    int ConfigAudioMaxCompression;	///< config max volume compression
    int ConfigAudioStereoDescent;	///< config reduce stereo loudness
    int ConfigAudioBufferTime;			///< config size ms of audio buffer
    int ConfigAudioAutoAES;		///< config automatic AES handling
    int ConfigAudioEq;			///< config equalizer filter 
    int SetupAudioEqBand[18];		///< config equalizer filter bands

    char ConfigMakePrimary;		///< config primary wanted
    char ConfigHideMainMenuEntry;	///< config hide main menu entry
    char LogState;			///< flag logging on/off
    int ConfigLog;			///< loglevel config

    int ConfigDisableDeint;		///< disable deinterlacer
    void SetDisableDeint(void);
#ifdef USE_GLES
    int ConfigMaxSizeGPUImageCache = 128;
    int ConfigDisableOglOsd;
    void SetDisableOglOsd(void);
#endif

// OSD
    void OsdClose(void);
    void OsdDrawARGB(int, int, int, int, int, const uint8_t *, int, int);

// Audio
    cSoftHdAudio *Audio;
    cAudioDecoder *AudioDecoder;
    AVPacket *AudioAvPkt;
    enum AVCodecID AudioCodecID;
    int AudioChannelID;
    void ClearAudio(void);
    volatile char NewAudioStream;
    volatile char SkipAudio;

    int VideoAudioDelay;
    void SetVideoAudioDelay(int);
    int GetVideoAudioDelay(void);

    void SetPassthrough(int);	///< Set audio passthrough mask
    void ResetChannelId(void);

// Stream
    cVideoStream *VideoStream;
    void StartThreads(void);

// Render
    cVideoRender *Render;

// Logging, statistics
    void SetLogLevel(int);
    void GetStats(int *, int *, int *);

// Mediaplayer
    void SetAudioCodec(enum AVCodecID, AVCodecParameters *, AVRational *);
    void SetVideoCodec(enum AVCodecID, AVCodecParameters *, AVRational *);
    int PlayAudioPkts(AVPacket *);
    int PlayVideoPkts(AVPacket *);
};

#endif
