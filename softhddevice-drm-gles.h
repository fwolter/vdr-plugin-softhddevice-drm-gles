/**
 * @file softhddevice.h
 * @brief Software HD device plugin header file
 *
 * Copyright: (c) 2011, 2014 by Johns.  All Rights Reserved.
 * Copyright (c) 2018 - 2019 zille.  All Rights Reserved.
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

#ifndef __SOFTHDDEVICE_DRM_GLES_H
#define __SOFTHDDEVICE_DRM_GLES_H

#ifdef USE_GLES
#include "openglosd.h"
#endif

#include "softhddevice.h"

class cSoftHdDevice;

/*****************************************************************************
 * Plugin
 ****************************************************************************/

/**
 * cPluginSoftHdDevice - SoftHdDevice plugin class
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
	cSoftHdDevice *m_pDevice;          ///< pointer to cSoftHdDevice object
	cSoftHdAudio *m_pAudio;            ///< pointer to cSoftHdAudio object
	cSoftHdConfig *m_pConfig;          ///< pointer to cSoftHdConfig object
	int m_doMakePrimary;               ///< switch primary device to this
};

#endif
