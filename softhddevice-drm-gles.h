///
///	@file softhddevice.h	@brief software HD device plugin header file.
///
///	Copyright (c) 2011, 2014 by Johns.  All Rights Reserved.
///	Copyright (c) 2018 - 2019 zille.  All Rights Reserved.
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

#ifndef __SOFTHDDEVICE_DRM_GLES_H
#define __SOFTHDDEVICE_DRM_GLES_H

#ifdef USE_GLES
#include "openglosd.h"
#endif

#include "softhddevice.h"

//////////////////////////////////////////////////////////////////////////////
//	Static Variables
//////////////////////////////////////////////////////////////////////////////

    /// vdr-plugin version number.
    /// Makefile extracts the version number for generating the file name
    /// for the distribution archive.
static const char *const VERSION = "0.4.9";

    /// vdr-plugin description.
static const char *const DESCRIPTION = trNOOP("A software and GPU emulated HD device");

    /// what is displayed in the main menu
static const char *const MAINMENUENTRY = trNOOP("SHD Media Player");

//////////////////////////////////////////////////////////////////////////////
//	OSD
//////////////////////////////////////////////////////////////////////////////

/**
**	cSoftOsd - SoftHdDevice plugin software OSD class
*/
class cSoftOsd:public cOsd
{
  public:
    char Dirty;		///< flag force redraw everything
    int OsdLevel;			///< current osd level FIXME: remove

    cSoftOsd(int, int, uint, cSoftHdDevice *);	///< osd constructor
    virtual ~ cSoftOsd(void);		///< osd destructor
    /// set the sub-areas to the given areas
    virtual eOsdError SetAreas(const tArea *, int);
    virtual void Flush(void);		///< commits all data to the hardware
    virtual void SetActive(bool);	///< sets OSD to be the active one

  private:
    cSoftHdDevice *Device;
};

//////////////////////////////////////////////////////////////////////////////
//	OSD provider
//////////////////////////////////////////////////////////////////////////////

/**
**	cSoftOsdProvider - SoftHdDevice plugin OSD provider class
*/
class cSoftOsdProvider:public cOsdProvider
{
  private:
    cOsd *Osd;			///< single OSD
#ifdef USE_GLES
    std::shared_ptr<cOglThread> oglThread;
    bool StartOpenGlThread(void);
  protected:
    virtual int StoreImageData(const cImage &Image);
    virtual void DropImageData(int ImageHandle);
#endif
  public:
    virtual cOsd * CreateOsd(int, int, uint);
    virtual bool ProvidesTrueColor(void);
    cSoftOsdProvider(cSoftHdDevice *);		///< OSD provider constructor
#ifdef USE_GLES
    void StopOpenGlThread(void);
    const cImage *GetImageData(int ImageHandle);
    void OsdSizeChanged(void);
#endif
    virtual ~cSoftOsdProvider();	///< OSD provider destructor
  private:
    cSoftHdDevice *Device;
};

//////////////////////////////////////////////////////////////////////////////
//	Menu
//////////////////////////////////////////////////////////////////////////////

/**
**	cMenuSetupSoft - SoftHdDevice plugin menu setup page class
*/
class cMenuSetupSoft:public cMenuSetupPage
{
  protected:
    ///
    /// local copies of global setup variables:
    /// @{
    int General;
    int MakePrimary;
#ifdef USE_GLES
#ifdef WRITE_PNG
    int DebugMenu;
    int WritePngs;
//    const char *pngVariant[4];
#endif
#endif
    int Statistics;
    int HideMainMenuEntry;

    int Logging;
    int LogDefault;
    int LogDebug;
    int LogAVSync;
    int LogSound;
    int LogOSD;
    int LogDRM;
    int LogCodec;
    int LogStill;
    int LogTrick;
    int LogMedia;
    int LogGL;
    int LogGLTime;
    int LogGLTimeAll;
    int LogPacket;
    int LogGrab;

    int VideoMenu;
    int DisableDeint;

    int Audio;
    int AudioDelay;
    int AudioPassthroughDefault;
    int AudioPassthroughPCM;
    int AudioPassthroughAC3;
    int AudioPassthroughEAC3;
    int AudioPassthroughDTS;
    int AudioDownmix;
    int AudioSoftvol;
    int AudioNormalize;
    int AudioMaxNormalize;
    int AudioCompression;
    int AudioMaxCompression;
    int AudioStereoDescent;
    int AudioBufferTime;
    int AudioAutoAES;

    int AudioFilter;
    int AudioEq;
    int AudioEqBand[18];

#ifdef USE_GLES
    int MaxSizeGPUImageCache;
#endif

    /// @}
  private:
    inline cOsdItem * CollapsedItem(const char *, int &, const char * = NULL);
    void Create(void);			// create sub-menu
    cSoftHdDevice *Device;
  protected:
    virtual void Store(void);
  public:
    cMenuSetupSoft(cSoftHdDevice *);
    virtual eOSState ProcessKey(eKeys);	// handle input
};

//////////////////////////////////////////////////////////////////////////////
//	Plugin
//////////////////////////////////////////////////////////////////////////////

/**
**	cPluginSoftHdDevice - SoftHdDevice plugin class
*/
class cPluginSoftHdDevice:public cPlugin
{
  public:
    cPluginSoftHdDevice(void);
    virtual ~ cPluginSoftHdDevice(void);
    virtual const char *Version(void);
    virtual const char *Description(void);
    virtual const char *CommandLineHelp(void);
    virtual bool ProcessArgs(int, char *[]);
    virtual bool Initialize(void);
    virtual bool Start(void);
    virtual void Stop(void);
    virtual const char *MainMenuEntry(void);
    virtual cOsdObject *MainMenuAction(void);
    virtual cMenuSetupPage *SetupMenu(void);
    virtual bool SetupParse(const char *, const char *);
    virtual bool Service(const char *, void * = NULL);
    virtual const char **SVDRPHelpPages(void);
    virtual cString SVDRPCommand(const char *, const char *, int &);
  private:
    cSoftHdDevice *Device;
    int DoMakePrimary;	///< switch primary device to this
};

#endif
