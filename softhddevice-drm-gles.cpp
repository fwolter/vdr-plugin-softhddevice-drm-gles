///
///	@file softhddevice.cpp	@brief A software HD device plugin for VDR.
///
///	Copyright (c) 2011 - 2015 by Johns.  All Rights Reserved.
///	Copyright (c) 2018 zille.  All Rights Reserved.
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

#define __STDC_CONSTANT_MACROS		///< needed for ffmpeg UINT64_C

#include <string>
using std::string;
#include <fstream>
using std::ifstream;

#include <vdr/player.h>
#include <vdr/plugin.h>

#include "logger.h"

#include "softhddevice-drm-gles.h"
#include "softhddevice.h"
#include "mediaplayer.h"

#ifdef USE_GLES
#include "openglosd.h"
#endif

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include "softhddev.h"
#include "video.h"
#include "audio.h"
#include "codec_audio.h"

//////////////////////////////////////////////////////////////////////////////
//	OSD
//////////////////////////////////////////////////////////////////////////////

/**
**	Sets this OSD to be the active one.
**
**	@param on	true on, false off
**
**	@note only needed as workaround for text2skin plugin with
**	undrawn areas.
*/
void cSoftOsd::SetActive(bool on)
{
    LOGDEBUG2(L_OSD, "OSD %s: %d level %d", __FUNCTION__, on, OsdLevel);

    if (Active() == on) {
	return;				// already active, no action
    }
    cOsd::SetActive(on);

    if (on) {
	Dirty = 1;
	// only flush here if there are already bitmaps
	if (GetBitmap(0)) {
	    Flush();
	}
    } else {
	Device->OsdClose();
    }
}

/**
**	Constructor OSD.
**
**	Initializes the OSD with the given coordinates.
**
**	@param left	x-coordinate of osd on display
**	@param top	y-coordinate of osd on display
**	@param level	level of the osd (smallest is shown)
*/
cSoftOsd::cSoftOsd(int left, int top, uint level, cSoftHdDevice *device)
:cOsd(left, top, level)
{
    /* FIXME: OsdWidth/OsdHeight not correct!
     */
    LOGDEBUG2(L_OSD, "OSD %s: %dx%d%+d%+d, %d", __FUNCTION__, OsdWidth(),
	OsdHeight(), left, top, level);

    Device = device;
    OsdLevel = level;
}

/**
**	OSD Destructor.
**
**	Shuts down the OSD.
*/
cSoftOsd::~cSoftOsd(void)
{
    LOGDEBUG2(L_OSD, "OSD %s: level %d", __FUNCTION__, OsdLevel);

    SetActive(false);
    // done by SetActive: OsdClose();
}

/**
**	Set the sub-areas to the given areas
*/
eOsdError cSoftOsd::SetAreas(const tArea * areas, int n)
{
    LOGDEBUG2(L_OSD, "OSD %s: %d areas", __FUNCTION__, n);

    // clear old OSD, when new areas are set
    if (!IsTrueColor()) {
	cBitmap *bitmap;
	int i;

	for (i = 0; (bitmap = GetBitmap(i)); i++) {
	    bitmap->Clean();
	}
    }
    if (Active()) {
	Device->OsdClose();
	Dirty = 1;
    }
    return cOsd::SetAreas(areas, n);
}

/**
**	Actually commits all data to the OSD hardware.
*/
void cSoftOsd::Flush(void)
{
    cPixmapMemory *pm;

    LOGDEBUG2(L_OSD, "OSD %s: level %d active %d", __FUNCTION__, OsdLevel,
	Active());

    if (!Active()) {			// this osd is not active
	return;
    }

    if (!IsTrueColor()) {
	cBitmap *bitmap;
	int i;

	static char warned;

	if (!warned) {
	    LOGDEBUG2(L_OSD, "OSD %s: FIXME: should be truecolor",
		__FUNCTION__);
	    warned = 1;
	}

	// draw all bitmaps
	for (i = 0; (bitmap = GetBitmap(i)); ++i) {
	    uint8_t *argb;
	    int xs;
	    int ys;
	    int x;
	    int y;
	    int w;
	    int h;
	    int x1;
	    int y1;
	    int x2;
	    int y2;

	    // get dirty bounding box
	    if (Dirty) {		// forced complete update
		x1 = 0;
		y1 = 0;
		x2 = bitmap->Width() - 1;
		y2 = bitmap->Height() - 1;
	    } else if (!bitmap->Dirty(x1, y1, x2, y2)) {
		continue;		// nothing dirty continue
	    }
	    // convert and upload only visible dirty areas
	    xs = bitmap->X0() + Left();
	    ys = bitmap->Y0() + Top();
	    // FIXME: negtative position bitmaps
	    w = x2 - x1 + 1;
	    h = y2 - y1 + 1;
	    // clip to screen
	    if (1) {			// just for the case it makes trouble
		int width;
		int height;
		double video_aspect;

		if (xs < 0) {
		    if (xs + x1 < 0) {
			x1 -= xs + x1;
			w += xs + x1;
			if (w <= 0) {
			    continue;
			}
		    }
		    xs = 0;
		}
		if (ys < 0) {
		    if (ys + y1 < 0) {
			y1 -= ys + y1;
			h += ys + y1;
			if (h <= 0) {
			    continue;
			}
		    }
		    ys = 0;
		}
		Device->GetOsdSize(width, height, video_aspect);
		if (w > width - xs - x1) {
		    w = width - xs - x1;
		    if (w <= 0) {
			continue;
		    }
		    x2 = x1 + w - 1;
		}
		if (h > height - ys - y1) {
		    h = height - ys - y1;
		    if (h <= 0) {
			continue;
		    }
		    y2 = y1 + h - 1;
		}
	    }

	    if (w > bitmap->Width() || h > bitmap->Height()) {
		LOGDEBUG2(L_OSD, ": dirty area too big");
	    }

	    argb = (uint8_t *) malloc(w * h * sizeof(uint32_t));
	    for (y = y1; y <= y2; ++y) {
		for (x = x1; x <= x2; ++x) {
		    ((uint32_t *) argb)[x - x1 + (y - y1) * w] =
			bitmap->GetColor(x, y);
		}
	    }
	    LOGDEBUG2(L_OSD, "OSD %s: draw %dx%d%+d%+d bm", __FUNCTION__, w, h,
		xs + x1, ys + y1);
	    Device->OsdDrawARGB(0, 0, w, h, w * sizeof(uint32_t), argb, xs + x1,
		ys + y1);

	    bitmap->Clean();
	    // FIXME: reuse argb
	    free(argb);
	}
	Dirty = 0;
	return;
    }

    LOCK_PIXMAPS;
    while ((pm = (dynamic_cast < cPixmapMemory * >(RenderPixmaps())))) {
	int xp;
	int yp;
	int stride;
	int x;
	int y;
	int w;
	int h;

	x = pm->ViewPort().X();
	y = pm->ViewPort().Y();
	w = pm->ViewPort().Width();
	h = pm->ViewPort().Height();
	stride = w * sizeof(tColor);

	// clip to osd
	xp = 0;
	if (x < 0) {
	    xp = -x;
	    w -= xp;
	    x = 0;
	}

	yp = 0;
	if (y < 0) {
	    yp = -y;
	    h -= yp;
	    y = 0;
	}

	if (w > Width() - x) {
	    w = Width() - x;
	}
	if (h > Height() - y) {
	    h = Height() - y;
	}

	x += Left();
	y += Top();

	// clip to screen
	if (1) {			// just for the case it makes trouble
	    // and it can happen!
	    int width;
	    int height;
	    double video_aspect;

	    if (x < 0) {
		w += x;
		xp += -x;
		x = 0;
	    }
	    if (y < 0) {
		h += y;
		yp += -y;
		y = 0;
	    }

	    Device->GetOsdSize(width, height, video_aspect);
	    if (w > width - x) {
		w = width - x;
	    }
	    if (h > height - y) {
		h = height - y;
	    }
	}
	LOGDEBUG2(L_OSD, "OSD %s: draw %dx%d%+d%+d*%d -> %+d%+d %p",
	    __FUNCTION__, w, h, xp, yp, stride, x, y, pm->Data());
	Device->OsdDrawARGB(xp, yp, w, h, stride, pm->Data(), x, y);

	DestroyPixmap(pm);
    }
    Dirty = 0;
}

//////////////////////////////////////////////////////////////////////////////
//	OSD provider
//////////////////////////////////////////////////////////////////////////////

/**
**	Create a new OSD.
**
**	@param left	x-coordinate of OSD
**	@param top	y-coordinate of OSD
**	@param level	layer level of OSD
*/
cOsd *cSoftOsdProvider::CreateOsd(int left, int top, uint level)
{
#ifdef USE_GLES
    if (Device->ConfigDisableOglOsd) {
        LOGDEBUG("OSD %s: %d, %d, %d, OpenGL disabled, using software rendering", __FUNCTION__, left, top, level);
        return Osd = new cSoftOsd(left, top, level, Device);
    }

    if (StartOpenGlThread()) {
        LOGDEBUG2(L_OSD, "OSD %s: %d, %d, %d, using OpenGL OSD support", __FUNCTION__, left, top, level);
        return Osd = new cOglOsd(left, top, level, oglThread, Device);
    }

    LOGDEBUG("OSD %s: %d, %d, %d, OpenGL failed, using software rendering", __FUNCTION__, left, top, 999);
    Device->SetDisableOglOsd();
    return Osd = new cSoftOsd(left, top, 999, Device);
#else
    LOGDEBUG2(L_OSD, "OSD %s: %d, %d, %d", __FUNCTION__, left, top, level);
    return Osd = new cSoftOsd(left, top, level, Device);
#endif
}

/**
**	Check if this OSD provider is able to handle a true color OSD.
**
**	@returns true we are able to handle a true color OSD.
*/
bool cSoftOsdProvider::ProvidesTrueColor(void)
{
    return true;
}

#ifdef USE_GLES
const cImage *cSoftOsdProvider::GetImageData(int ImageHandle) {
    return cOsdProvider::GetImageData(ImageHandle);
}

void cSoftOsdProvider::OsdSizeChanged(void) {
    // cleanup OpenGL context
    if (!Device->ConfigDisableOglOsd)
        cSoftOsdProvider::StopOpenGlThread();
    cSoftOsdProvider::UpdateOsdSize();
}

bool cSoftOsdProvider::StartOpenGlThread(void) {
    if (Device->ConfigDisableOglOsd) {
        LOGDEBUG2(L_OPENGL, "OpenGL OSD disabled, OpenGL worker thread NOT started");
        return false;
    }

    if (oglThread.get()) {
        if (oglThread->Active()) {
            return true;
        }
        oglThread.reset();
    }
    cCondWait wait;
    LOGDEBUG2(L_OPENGL, "Trying to start OpenGL worker thread");
    oglThread.reset(new cOglThread(&wait, Device->ConfigMaxSizeGPUImageCache));
    wait.Wait();

    if (oglThread->Active()) {
        LOGINFO("OpenGL worker thread started");
        return true;
    }

    LOGDEBUG2(L_OPENGL, "OpenGL worker thread NOT started");
    return false;
}

void cSoftOsdProvider::StopOpenGlThread(void) {
    LOGDEBUG2(L_OPENGL, "stopping OpenGL worker thread");
    if (oglThread) {
        oglThread->Stop();
    }
    oglThread.reset();
    LOGINFO("OpenGL worker thread stopped");
}

int cSoftOsdProvider::StoreImageData(const cImage &Image)
{
    if (StartOpenGlThread()) {
        int imgHandle = oglThread->StoreImage(Image);
        return imgHandle;
    }
    return 0;
}

void cSoftOsdProvider::DropImageData(int imgHandle)
{
    if (StartOpenGlThread())
        oglThread->DropImageData(imgHandle);
}
#endif

/**
**	Create cOsdProvider class.
*/
cSoftOsdProvider::cSoftOsdProvider(cSoftHdDevice *device)
:  cOsdProvider()
{
    LOGDEBUG("%s:", __FUNCTION__);
    LOGDEBUG2(L_OSD, "OSD %s:", __FUNCTION__);
    Device = device;

#ifdef USE_GLES
    if (!Device->ConfigDisableOglOsd)
        StopOpenGlThread();
#endif
}

/**
**	Destroy cOsdProvider class.
*/
cSoftOsdProvider::~cSoftOsdProvider()
{
    LOGDEBUG2(L_OSD, "%s:", __FUNCTION__);
#ifdef USE_GLES
    if (!Device->ConfigDisableOglOsd)
        StopOpenGlThread();
#endif
}

//////////////////////////////////////////////////////////////////////////////
//	cMenuSetupPage
//////////////////////////////////////////////////////////////////////////////

/**
**	Create a seperator item.
**
**	@param label	text inside separator
*/
static inline cOsdItem *SeparatorItem(const char *label)
{
    cOsdItem *item;

    item = new cOsdItem(cString::sprintf("* %s: ", label));
    item->SetSelectable(false);

    return item;
}

/**
**	Create a collapsed item.
**
**	@param label	text inside collapsed
**	@param flag	flag handling collapsed or opened
**	@param msg	open message
*/
inline cOsdItem *cMenuSetupSoft::CollapsedItem(const char *label, int &flag,
    const char *msg)
{
    cOsdItem *item;

    item =
	new cMenuEditBoolItem(cString::sprintf("* %s", label), &flag,
	msg ? msg : tr("show"), tr("hide"));

    return item;
}

/**
**	Create setup menu.
*/
void cMenuSetupSoft::Create(void)
{
    int current;

    current = Current();		// get current menu item index
    Clear();				// clear the menu
#ifdef USE_GLES
#ifdef WRITE_PNG
//    pngVariant[0] = tr("none");
//    pngVariant[1] = tr("output fb");
//    pngVariant[2] = tr("render fb");
//    pngVariant[3] = tr("both");
#endif
#endif

    //
    //	general
    //
    Add(CollapsedItem(tr("General"), General));

    if (General) {
	Add(new cMenuEditBoolItem(tr("Make primary device"), &MakePrimary,
		trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Hide main menu entry"),
		&HideMainMenuEntry, trVDR("no"), trVDR("yes")));
	//
	//	osd
	//
#ifdef USE_GLES
	if (!Device->ConfigDisableOglOsd) {
		Add(new cMenuEditIntItem(tr("GPU mem used for image caching (MB)"), &MaxSizeGPUImageCache, 0, 4000));
	}
#endif
    }

    //
    //	statistics
    //
    Add(CollapsedItem(tr("Statistics"), Statistics));
    if (Statistics) {
	int duped;
	int dropped;
	int counter;
	Device->GetStats(&duped, &dropped, &counter);
	Add(new cOsdItem(cString::sprintf(tr
		(" Frames duped(%d) dropped(%d) total(%d)"),
		duped, dropped, counter), osUnknown, false));
#ifdef USE_GLES
	Add(new cOsdItem(cString::sprintf(tr
		(" OSD: Using %s rendering"), Device->ConfigDisableOglOsd ? "software" : "hardware"), osUnknown, false));
#else
	Add(new cOsdItem(cString::sprintf(tr
		(" OSD: Using software rendering")), osUnknown, false));
#endif

    }

#ifdef USE_GLES
#ifdef WRITE_PNG
    //
    //	debug
    //
    if (!Device->ConfigDisableOglOsd) {
	Add(CollapsedItem(tr("Debug"), DebugMenu));
	if (DebugMenu) {
		Add(new cMenuEditBoolItem(tr("Write OSD to file"), &WritePngs, trVDR("no"), trVDR("yes")));
//		Add(new cMenuEditStraItem(tr("Write OSD to file"), &WritePngs, 4, pngVariant));
	}
    }
#endif
#endif
    //
    //	logging
    //
    Add(CollapsedItem(tr("Logging"), Logging));

    if (Logging) {
	Add(new cMenuEditBoolItem(tr("Logging default"),
		&LogDefault, trVDR("off"), trVDR("on")));
	if (LogDefault) {
		Add(new cMenuEditBoolItem(tr("\040\040Standard debug logs"),
			&LogDebug, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040AV Sync debug logs"),
			&LogAVSync, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040Sound debug logs"),
			&LogSound, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040OSD debug logs"),
			&LogOSD, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040DRM debug logs"),
			&LogDRM, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040Codec debug logs"),
			&LogCodec, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040Stillpicture debug logs"),
			&LogStill, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040Trickspeed debug logs"),
			&LogTrick, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040Mediaplayer debug logs"),
			&LogMedia, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040OpenGL OSD debug logs"),
			&LogGL, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040OpenGL OSD time measurement"),
			&LogGLTime, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040OpenGL OSD time measurement (extensive)"),
			&LogGLTimeAll, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040Packet tracking logs"),
			&LogPacket, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040Grabbing debug logs"),
			&LogGrab, trVDR("no"), trVDR("yes")));
	}
    }

    //
    //	video
    //
    Add(CollapsedItem(tr("Video"), VideoMenu));

    if (VideoMenu) {
	Add(new cMenuEditBoolItem(tr("Disable Deinterlacer"),
		&DisableDeint, trVDR("no"), trVDR("yes")));
    }

    //
    //	audio
    //
    Add(CollapsedItem(tr("Audio"), Audio));

    if (Audio) {
	Add(new cMenuEditIntItem(tr("Audio/Video delay (ms)"), &AudioDelay,
		-1000, 1000));
	Add(new cMenuEditBoolItem(tr("Volume control"), &AudioSoftvol,
		tr("Hardware"), tr("Software")));
	Add(new cMenuEditIntItem(tr("Audio buffer size (ms)"),
		&AudioBufferTime, 0, 1000));
	Add(new cMenuEditBoolItem(tr("Enable normalize volume"),
		&AudioNormalize, trVDR("no"), trVDR("yes")));
	if (AudioNormalize)
		Add(new cMenuEditIntItem(tr("  Max normalize factor (/1000)"),
			&AudioMaxNormalize, 0, 10000));
	Add(new cMenuEditBoolItem(tr("Enable volume compression"),
		&AudioCompression, trVDR("no"), trVDR("yes")));
	if (AudioCompression)
		Add(new cMenuEditIntItem(tr("  Max compression factor (/1000)"),
			&AudioMaxCompression, 0, 10000));
	Add(new cMenuEditIntItem(tr("Reduce stereo volume (/1000)"),
		&AudioStereoDescent, 0, 1000));
	Add(new cMenuEditBoolItem(tr("Enable Stereo downmix"),
		&AudioDownmix, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Pass-through default"),
		&AudioPassthroughDefault, trVDR("off"), trVDR("on")));
	if (AudioPassthroughDefault) {
		Add(new cMenuEditBoolItem(tr("\040\040PCM pass-through"),
			&AudioPassthroughPCM, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040AC-3 pass-through"),
			&AudioPassthroughAC3, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040E-AC-3 pass-through"),
			&AudioPassthroughEAC3, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("\040\040DTS pass-through"),
			&AudioPassthroughDTS, trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("Enable automatic AES"), &AudioAutoAES,
			trVDR("no"), trVDR("yes")));
	}
    }
    Add(CollapsedItem(tr("Audio Filter"), AudioFilter));
    if (AudioFilter) {
		Add(new cMenuEditBoolItem(tr(" Enable Audio Equalizer"), &AudioEq,
			trVDR("no"), trVDR("yes")));
		if (AudioEq) {
			Add(new cMenuEditIntItem(tr("  60 Hz band gain"),
				&AudioEqBand[0], -15, 1));
			Add(new cMenuEditIntItem(tr("  72 Hz band gain"),
				&AudioEqBand[1], -15, 1));
			Add(new cMenuEditIntItem(tr("  107 Hz band gain"),
				&AudioEqBand[2], -15, 1));
			Add(new cMenuEditIntItem(tr("  150 Hz band gain"),
				&AudioEqBand[3], -15, 1));
			Add(new cMenuEditIntItem(tr("  220 Hz band gain"),
				&AudioEqBand[4], -15, 1));
			Add(new cMenuEditIntItem(tr("  310 Hz band gain"),
				&AudioEqBand[5], -15, 1));
			Add(new cMenuEditIntItem(tr("  430 Hz band gain"),
				&AudioEqBand[6], -15, 1));
			Add(new cMenuEditIntItem(tr("  620 Hz band gain"),
				&AudioEqBand[7], -15, 1));
			Add(new cMenuEditIntItem(tr("  860 Hz band gain"),
				&AudioEqBand[8], -15, 1));
			Add(new cMenuEditIntItem(tr("  1200 Hz band gain"),
				&AudioEqBand[9], -15, 1));
			Add(new cMenuEditIntItem(tr("  1700 Hz band gain"),
				&AudioEqBand[10], -15, 1));
			Add(new cMenuEditIntItem(tr("  2500 Hz band gain"),
				&AudioEqBand[11], -15, 1));
			Add(new cMenuEditIntItem(tr("  3500 Hz band gain"),
				&AudioEqBand[12], -15, 1));
			Add(new cMenuEditIntItem(tr("  4800 Hz band gain"),
				&AudioEqBand[13], -15, 1));
			Add(new cMenuEditIntItem(tr("  7000 Hz band gain"),
				&AudioEqBand[14], -15, 1));
			Add(new cMenuEditIntItem(tr("  9500 Hz band gain"),
				&AudioEqBand[15], -15, 1));
			Add(new cMenuEditIntItem(tr("  13500 Hz band gain"),
				&AudioEqBand[16], -15, 1));
			Add(new cMenuEditIntItem(tr("  17200 Hz band gain"),
				&AudioEqBand[17], -15, 1));
		}
	}

    SetCurrent(Get(current));		// restore selected menu entry
    Display();				// display build menu
}

/**
**	Process key for setup menu.
*/
eOSState cMenuSetupSoft::ProcessKey(eKeys key)
{
    int old_General = General;
#ifdef USE_GLES
#ifdef WRITE_PNG
    int old_DebugMenu = DebugMenu;
#endif
#endif
    int old_Statistics = Statistics;
    int old_Logging = Logging;
    int old_LogDefault = LogDefault;
    int old_Audio = Audio;
    int old_VideoMenu = VideoMenu;
    int old_AudioFilter = AudioFilter;
    int old_AudioEq = AudioEq;
    int old_AudioNormalize = AudioNormalize;
    int old_AudioCompression = AudioCompression;
    int old_AudioPassthroughDefault = AudioPassthroughDefault;
    eOSState state = cMenuSetupPage::ProcessKey(key);

    if (key != kNone) {
		// update menu only, if something on the structure has changed
		// this is needed because VDR menus are evil slow
		if (old_General != General ||
			old_Audio != Audio || old_AudioFilter != AudioFilter ||
			old_AudioEq != AudioEq || old_AudioNormalize != AudioNormalize ||
			old_AudioCompression != AudioCompression ||
			old_VideoMenu != VideoMenu ||
#ifdef USE_GLES
#ifdef WRITE_PNG
			old_DebugMenu != DebugMenu ||
#endif
#endif
			old_Statistics != Statistics || old_Logging != Logging ||
			old_LogDefault != LogDefault ||
			old_AudioPassthroughDefault != AudioPassthroughDefault) {
			Create();			// update menu
		}
    }

    return state;
}

/**
**	Constructor setup menu.
**
**	Import global config variables into setup.
*/
cMenuSetupSoft::cMenuSetupSoft(cSoftHdDevice *device)
{
    Device = device;

    //
    //	general
    //
    General = 0;
    MakePrimary = Device->ConfigMakePrimary;
#ifdef USE_GLES
#ifdef WRITE_PNG
    DebugMenu = 0;
    WritePngs = Device->GetConfigWritePngs();
#endif
#endif
    Statistics = 0;
    HideMainMenuEntry = Device->ConfigHideMainMenuEntry;
    //
    //	logging
    //
    Logging = 0;
    LogDefault = Device->LogState;
    LogDebug = Device->ConfigLog & L_DEBUG;
    LogAVSync = Device->ConfigLog & L_AV_SYNC;
    LogSound = Device->ConfigLog & L_SOUND;
    LogOSD = Device->ConfigLog & L_OSD;
    LogDRM = Device->ConfigLog & L_DRM;
    LogCodec = Device->ConfigLog & L_CODEC;
    LogStill = Device->ConfigLog & L_STILL;
    LogTrick = Device->ConfigLog & L_TRICK;
    LogMedia = Device->ConfigLog & L_MEDIA;
    LogGL = Device->ConfigLog & L_OPENGL;
    LogGLTime = Device->ConfigLog & L_OPENGL_TIME;
    LogGLTimeAll = Device->ConfigLog & L_OPENGL_TIME_ALL;
    LogPacket = Device->ConfigLog & L_PACKET;
    LogGrab = Device->ConfigLog & L_GRAB;
    //
    //	video
    //
    VideoMenu = 0;
    DisableDeint = Device->ConfigDisableDeint;
    //
    //	audio
    //
    Audio = 0;
    AudioDelay = Device->ConfigVideoAudioDelay;
    AudioPassthroughDefault = Device->AudioPassthroughState;
    AudioPassthroughPCM = Device->ConfigAudioPassthrough & CodecPCM;
    AudioPassthroughAC3 = Device->ConfigAudioPassthrough & CodecAC3;
    AudioPassthroughEAC3 = Device->ConfigAudioPassthrough & CodecEAC3;
    AudioPassthroughDTS = Device->ConfigAudioPassthrough & CodecDTS;
    AudioDownmix = Device->ConfigAudioDownmix;
    AudioSoftvol = Device->ConfigAudioSoftvol;
    AudioNormalize = Device->ConfigAudioNormalize;
    AudioMaxNormalize = Device->ConfigAudioMaxNormalize;
    AudioCompression = Device->ConfigAudioCompression;
    AudioMaxCompression = Device->ConfigAudioMaxCompression;
    AudioStereoDescent = Device->ConfigAudioStereoDescent;
    AudioBufferTime = Device->ConfigAudioBufferTime;
    AudioAutoAES = Device->ConfigAudioAutoAES;
	//
	// audio filter
	//
    AudioEq = Device->ConfigAudioEq;
    AudioFilter = 0;
    for (int i = 0; i < 18; i++) {
		AudioEqBand[i] = Device->SetupAudioEqBand[i];
	}

#ifdef USE_GLES
    MaxSizeGPUImageCache = Device->ConfigMaxSizeGPUImageCache;
#endif

    Create();
}

/**
**	Store setup.
*/
void cMenuSetupSoft::Store(void)
{
    SetupStore("MakePrimary", Device->ConfigMakePrimary = MakePrimary);
#ifdef USE_GLES
#ifdef WRITE_PNG
    Device->SetConfigWritePngs(WritePngs);
    SetupStore("WritePngs", Device->GetConfigWritePngs());
#endif
#endif
    SetupStore("HideMainMenuEntry", Device->ConfigHideMainMenuEntry = HideMainMenuEntry);
    SetupStore("AudioDelay", Device->ConfigVideoAudioDelay = AudioDelay);
    VideoSetAudioDelay(Device->ConfigVideoAudioDelay);

    Device->ConfigLog =
	(LogDebug ? L_DEBUG : 0) |
	(LogAVSync ? L_AV_SYNC : 0) |
	(LogSound ? L_SOUND : 0) |
	(LogOSD ? L_OSD : 0) |
	(LogDRM ? L_DRM : 0) |
	(LogCodec ? L_CODEC : 0) |
	(LogStill ? L_STILL : 0) |
	(LogTrick ? L_TRICK : 0) |
	(LogMedia ? L_MEDIA : 0) |
	(LogGL ? L_OPENGL : 0) |
	(LogGLTime ? L_OPENGL_TIME : 0) |
	(LogGLTimeAll ? L_OPENGL_TIME_ALL : 0) |
	(LogPacket ? L_PACKET : 0) |
	(LogGrab ? L_GRAB : 0);
    Device->LogState = LogDefault;
    if (Device->LogState) {
	SetupStore("LogLevel", Device->ConfigLog);
	Device->SetLogLevel(Device->ConfigLog);
	cSoftHdLogger::GetLogger()->SetLogLevel(Device->ConfigLog);
    } else {
	SetupStore("LogLevel", -Device->ConfigLog);
	Device->SetLogLevel(0);
	cSoftHdLogger::GetLogger()->SetLogLevel(0);
    }

    SetupStore("DisableDeint", Device->ConfigDisableDeint = DisableDeint);
    if (Device->ConfigDisableDeint) {
	LOGDEBUG("Disable deinterlacer!");
    }
    Device->SetDisableDeint();

    // FIXME: can handle more audio state changes here
    // downmix changed reset audio, to get change direct
    if (Device->ConfigAudioDownmix != AudioDownmix) {
	Device->ResetChannelId();
    }
    Device->ConfigAudioPassthrough = (AudioPassthroughPCM ? CodecPCM : 0)
	| (AudioPassthroughAC3 ? CodecAC3 : 0)
	| (AudioPassthroughEAC3 ? CodecEAC3 : 0)
	| (AudioPassthroughDTS ? CodecDTS : 0);
    Device->AudioPassthroughState = AudioPassthroughDefault;
    if (Device->AudioPassthroughState) {
	SetupStore("AudioPassthrough", Device->ConfigAudioPassthrough);
	Device->SetPassthrough(Device->ConfigAudioPassthrough);
    } else {
	SetupStore("AudioPassthrough", -Device->ConfigAudioPassthrough);
	Device->SetPassthrough(0);
    }
    SetupStore("AudioDownmix", Device->ConfigAudioDownmix = AudioDownmix);
    AudioSetDownmix(Device->ConfigAudioDownmix);
    SetupStore("AudioSoftvol", Device->ConfigAudioSoftvol = AudioSoftvol);
    AudioSetSoftvol(Device->ConfigAudioSoftvol);
    SetupStore("AudioNormalize", Device->ConfigAudioNormalize = AudioNormalize);
    SetupStore("AudioMaxNormalize", Device->ConfigAudioMaxNormalize =
	AudioMaxNormalize);
    AudioSetNormalize(Device->ConfigAudioNormalize, Device->ConfigAudioMaxNormalize);
    SetupStore("AudioCompression", Device->ConfigAudioCompression = AudioCompression);
    SetupStore("AudioMaxCompression", Device->ConfigAudioMaxCompression =
	AudioMaxCompression);
    AudioSetCompression(Device->ConfigAudioCompression, Device->ConfigAudioMaxCompression);
    SetupStore("AudioStereoDescent", Device->ConfigAudioStereoDescent =
	AudioStereoDescent);
    AudioSetStereoDescent(Device->ConfigAudioStereoDescent);
    SetupStore("AudioBufferTime", Device->ConfigAudioBufferTime = AudioBufferTime);
    AudioSetBufferTime(Device->ConfigAudioBufferTime);
    SetupStore("AudioAutoAES", Device->ConfigAudioAutoAES = AudioAutoAES);
    AudioSetAutoAES(Device->ConfigAudioAutoAES);
	SetupStore("AudioEq", Device->ConfigAudioEq = AudioEq);
	SetupStore("AudioEqBand01b", Device->SetupAudioEqBand[0] = AudioEqBand[0]);
	SetupStore("AudioEqBand02b", Device->SetupAudioEqBand[1] = AudioEqBand[1]);
	SetupStore("AudioEqBand03b", Device->SetupAudioEqBand[2] = AudioEqBand[2]);
	SetupStore("AudioEqBand04b", Device->SetupAudioEqBand[3] = AudioEqBand[3]);
	SetupStore("AudioEqBand05b", Device->SetupAudioEqBand[4] = AudioEqBand[4]);
	SetupStore("AudioEqBand06b", Device->SetupAudioEqBand[5] = AudioEqBand[5]);
	SetupStore("AudioEqBand07b", Device->SetupAudioEqBand[6] = AudioEqBand[6]);
	SetupStore("AudioEqBand08b", Device->SetupAudioEqBand[7] = AudioEqBand[7]);
	SetupStore("AudioEqBand09b", Device->SetupAudioEqBand[8] = AudioEqBand[8]);
	SetupStore("AudioEqBand10b", Device->SetupAudioEqBand[9] = AudioEqBand[9]);
	SetupStore("AudioEqBand11b", Device->SetupAudioEqBand[10] = AudioEqBand[10]);
	SetupStore("AudioEqBand12b", Device->SetupAudioEqBand[11] = AudioEqBand[11]);
	SetupStore("AudioEqBand13b", Device->SetupAudioEqBand[12] = AudioEqBand[12]);
	SetupStore("AudioEqBand14b", Device->SetupAudioEqBand[13] = AudioEqBand[13]);
	SetupStore("AudioEqBand15b", Device->SetupAudioEqBand[14] = AudioEqBand[14]);
	SetupStore("AudioEqBand16b", Device->SetupAudioEqBand[15] = AudioEqBand[15]);
	SetupStore("AudioEqBand17b", Device->SetupAudioEqBand[16] = AudioEqBand[16]);
	SetupStore("AudioEqBand18b", Device->SetupAudioEqBand[17] = AudioEqBand[17]);
    AudioSetEq(Device->SetupAudioEqBand, Device->ConfigAudioEq);
#ifdef USE_GLES
    SetupStore("MaxSizeGPUImageCache", Device->ConfigMaxSizeGPUImageCache = MaxSizeGPUImageCache);
#endif
}

/**
**	Call rgb to jpeg for C Plugin.
*/
//extern "C" uint8_t * CreateJpeg(uint8_t * image, int *size, int quality,
//    int width, int height)
//{
//    return (uint8_t *) RgbToJpeg((uchar *) image, width, height, *size,
//	quality);
//}

//////////////////////////////////////////////////////////////////////////////
//	cPlugin
//////////////////////////////////////////////////////////////////////////////

/**
**	Initialize any member variables here.
**
**	@note DON'T DO ANYTHING ELSE THAT MAY HAVE SIDE EFFECTS, REQUIRE GLOBAL
**	VDR OBJECTS TO EXIST OR PRODUCE ANY OUTPUT!
*/
cPluginSoftHdDevice::cPluginSoftHdDevice(void)
{
    LOGDEBUG("%s:", __FUNCTION__);

    Device = new cSoftHdDevice();
}

/**
**	Clean up after yourself!
*/
cPluginSoftHdDevice::~cPluginSoftHdDevice(void)
{
    LOGDEBUG("%s:", __FUNCTION__);

    Device->Exit();
    delete Device;
}

/**
**	Return plugin version number.
**
**	@returns version number as constant string.
*/
const char *cPluginSoftHdDevice::Version(void)
{
    return VERSION;
}

/**
**	Return plugin short description.
**
**	@returns short description as constant string.
*/
const char *cPluginSoftHdDevice::Description(void)
{
    return tr(DESCRIPTION);
}

/**
**	Return a string that describes all known command line options.
**
**	@returns command line help as constant string.
*/
const char *cPluginSoftHdDevice::CommandLineHelp(void)
{
    return Device->CommandLineHelp();
}

/**
**	Process the command line arguments.
*/
bool cPluginSoftHdDevice::ProcessArgs(int argc, char *argv[])
{
    LOGDEBUG("%s:", __FUNCTION__);

    return Device->ProcessArgs(argc, argv);
}

/**
**	Initializes the DVB devices.
**
**	Must be called before accessing any DVB functions.
**
**	@returns true if any devices are available.
*/
bool cPluginSoftHdDevice::Initialize(void)
{
    LOGDEBUG("%s:", __FUNCTION__);

    // nothing to do
    return true;
}

/**
**	 Start any background activities the plugin shall perform.
*/
bool cPluginSoftHdDevice::Start(void)
{
	LOGDEBUG("%s:", __FUNCTION__);

	if (!Device->IsPrimaryDevice()) {
		LOGINFO("softhddevice %d is not the primary device!",
			Device->DeviceNumber());
		if (Device->ConfigMakePrimary) {
			// Must be done in the main thread
			LOGDEBUG("makeing softhddevice %d the primary device!",
				Device->DeviceNumber());
			DoMakePrimary = Device->DeviceNumber() + 1;
		}
	}
	Device->Start();

    return true;
}

/**
**	Shutdown plugin.  Stop any background activities the plugin is
**	performing.
*/
void cPluginSoftHdDevice::Stop(void)
{
    //LOGDEBUG("%s:", __FUNCTION__);

    Device->Stop();
}

/**
**	Create main menu entry.
*/
const char *cPluginSoftHdDevice::MainMenuEntry(void)
{
    //LOGDEBUG("%s:", __FUNCTION__);

    return Device->ConfigHideMainMenuEntry ? NULL : tr(MAINMENUENTRY);
}

/**
**	Perform the action when selected from the main VDR menu.
*/
cOsdObject *cPluginSoftHdDevice::MainMenuAction(void)
{
    //LOGDEBUG("%s:", __FUNCTION__);

    return new cSoftHdMenu("SoftHdDevice", Device);
}

/**
**	Return our setup menu.
*/
cMenuSetupPage *cPluginSoftHdDevice::SetupMenu(void)
{
    //LOGDEBUG("%s:", __FUNCTION__);

    return new cMenuSetupSoft(Device);
}

/**
**	Parse setup parameters
**
**	@param name	paramter name (case sensetive)
**	@param value	value as string
**
**	@returns true if the parameter is supported.
*/
bool cPluginSoftHdDevice::SetupParse(const char *name, const char *value)
{
    //LOGDEBUG("%s: '%s' = '%s'", __FUNCTION__, name, value);

    if (!strcasecmp(name, "MakePrimary")) {
	Device->ConfigMakePrimary = atoi(value);
	return true;
    }
#ifdef USE_GLES
#ifdef WRITE_PNG
    if (!strcasecmp(name, "WritePngs")) {
	Device->SetConfigWritePngs(atoi(value));
	return true;
    }
#endif
#endif
    if (!strcasecmp(name, "HideMainMenuEntry")) {
	Device->ConfigHideMainMenuEntry = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "LogLevel")) {
	int i = atoi(value);
	Device->LogState = i > 0;
	Device->ConfigLog = abs(i);
	if (Device->LogState) {
	    Device->SetLogLevel(Device->ConfigLog);
	    cSoftHdLogger::GetLogger()->SetLogLevel(Device->ConfigLog);
	} else {
	    Device->SetLogLevel(0);
	    cSoftHdLogger::GetLogger()->SetLogLevel(0);
	}
	return true;
    }
    if (!strcasecmp(name, "DisableDeint")) {
	Device->ConfigDisableDeint = atoi(value);
	Device->SetDisableDeint();
	return true;
    }
    if (!strcasecmp(name, "AudioDelay")) {
	VideoSetAudioDelay(Device->ConfigVideoAudioDelay = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioPassthrough")) {
	int i;

	i = atoi(value);
	Device->AudioPassthroughState = i > 0;
	Device->ConfigAudioPassthrough = abs(i);
	if (Device->AudioPassthroughState) {
	    Device->SetPassthrough(Device->ConfigAudioPassthrough);
	} else {
	    Device->SetPassthrough(0);
	}
	return true;
    }
    if (!strcasecmp(name, "AudioDownmix")) {
	AudioSetDownmix(Device->ConfigAudioDownmix = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioSoftvol")) {
	AudioSetSoftvol(Device->ConfigAudioSoftvol = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioNormalize")) {
	Device->ConfigAudioNormalize = atoi(value);
	AudioSetNormalize(Device->ConfigAudioNormalize, Device->ConfigAudioMaxNormalize);
	return true;
    }
    if (!strcasecmp(name, "AudioMaxNormalize")) {
	Device->ConfigAudioMaxNormalize = atoi(value);
	AudioSetNormalize(Device->ConfigAudioNormalize, Device->ConfigAudioMaxNormalize);
	return true;
    }
    if (!strcasecmp(name, "AudioCompression")) {
	Device->ConfigAudioCompression = atoi(value);
	AudioSetCompression(Device->ConfigAudioCompression, Device->ConfigAudioMaxCompression);
	return true;
    }
    if (!strcasecmp(name, "AudioMaxCompression")) {
	Device->ConfigAudioMaxCompression = atoi(value);
	AudioSetCompression(Device->ConfigAudioCompression, Device->ConfigAudioMaxCompression);
	return true;
    }
    if (!strcasecmp(name, "AudioStereoDescent")) {
	Device->ConfigAudioStereoDescent = atoi(value);
	AudioSetStereoDescent(Device->ConfigAudioStereoDescent);
	return true;
    }
    if (!strcasecmp(name, "AudioBufferTime")) {
	Device->ConfigAudioBufferTime = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioAutoAES")) {
	Device->ConfigAudioAutoAES = atoi(value);
	AudioSetAutoAES(Device->ConfigAudioAutoAES);
	return true;
    }
    if (!strcasecmp(name, "AudioEq")) {
	Device->ConfigAudioEq = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand01b")) {
	Device->SetupAudioEqBand[0] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand02b")) {
	Device->SetupAudioEqBand[1] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand03b")) {
	Device->SetupAudioEqBand[2] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand04b")) {
	Device->SetupAudioEqBand[3] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand05b")) {
	Device->SetupAudioEqBand[4] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand06b")) {
	Device->SetupAudioEqBand[5] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand07b")) {
	Device->SetupAudioEqBand[6] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand08b")) {
	Device->SetupAudioEqBand[7] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand09b")) {
	Device->SetupAudioEqBand[8] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand10b")) {
	Device->SetupAudioEqBand[9] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand11b")) {
	Device->SetupAudioEqBand[10] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand12b")) {
	Device->SetupAudioEqBand[11] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand13b")) {
	Device->SetupAudioEqBand[12] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand14b")) {
	Device->SetupAudioEqBand[13] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand15b")) {
	Device->SetupAudioEqBand[14] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand16b")) {
	Device->SetupAudioEqBand[15] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand17b")) {
	Device->SetupAudioEqBand[16] = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioEqBand18b")) {
	Device->SetupAudioEqBand[17] = atoi(value);
	AudioSetEq(Device->SetupAudioEqBand, Device->ConfigAudioEq);
	return true;
    }
#ifdef USE_GLES
    if (!strcasecmp(name, "MaxSizeGPUImageCache")) {
	Device->ConfigMaxSizeGPUImageCache = atoi(value);
	return true;
    }
#endif
    return false;
}

/**
**	Receive requests or messages.
**
**	@param id	unique identification string that identifies the
**			service protocol
**	@param data	custom data structure
*/
bool cPluginSoftHdDevice::Service(const char *id, void *data)
{
    //LOGDEBUG("%s: id %s", __FUNCTION__, id);
    (void)id;
    (void)data;

    return false;
}

//----------------------------------------------------------------------------
//	cPlugin SVDRP
//----------------------------------------------------------------------------

/**
**	SVDRP commands help text.
**	FIXME: translation?
*/
static const char *SVDRPHelpText[] = {
	"PLAY Url\n" "    Play the media from the given url.\n",
	NULL
};

/**
**	Return SVDRP commands help pages.
**
**	return a pointer to a list of help strings for all of the plugin's
**	SVDRP commands.
*/
const char **cPluginSoftHdDevice::SVDRPHelpPages(void)
{
    return SVDRPHelpText;
}

/**
**	Handle SVDRP commands.
**
**	@param command		SVDRP command
**	@param option		all command arguments
**	@param reply_code	reply code
*/
cString cPluginSoftHdDevice::SVDRPCommand(const char *command,
		__attribute__ ((unused)) const char *option,
		__attribute__ ((unused)) int &reply_code)
{
	if (!strcasecmp(command, "PLAY")) {
		LOGDEBUG2(L_MEDIA, "SVDRPCommand: %s %s", command, option);
		cControl::Launch(new cSoftHdControl(option, Device));
		return "PLAY url";
	}

    return NULL;
}

VDRPLUGINCREATOR(cPluginSoftHdDevice);	// Don't touch this!
