///
///	@file video.c	@brief Video module
///
///	Copyright (c) 2009 - 2015 by Johns.  All Rights Reserved.
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
///	@defgroup Video The video module.
///
///	This module contains all video rendering functions.
///

#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <stdbool.h>
#include <unistd.h>

#include <inttypes.h>

#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut

#ifdef USE_GLES
#include <assert.h>
#endif
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <drm_fourcc.h>

#include "logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>

#include "misc.h"
#include "buf2rgb.h"
}

#include "video.h"
#include "audio.h"
#include "drm.h"
//#include "openglosd.h"
#include "threads.h"

//----------------------------------------------------------------------------
//	Helper functions
//----------------------------------------------------------------------------

static void ReleaseFrame( __attribute__ ((unused)) void *opaque, uint8_t *data)
{
	AVDRMFrameDescriptor *primedata = (AVDRMFrameDescriptor *)data;

	av_free(primedata);
}

static int GetPropertyValue(int fd_drm, uint32_t objectID,
		     uint32_t objectType, const char *propName, uint64_t *value)
{
	uint32_t i;
	int found = 0;
	drmModePropertyPtr Prop;
	drmModeObjectPropertiesPtr objectProps =
		drmModeObjectGetProperties(fd_drm, objectID, objectType);

	for (i = 0; i < objectProps->count_props; i++) {
		if ((Prop = drmModeGetProperty(fd_drm, objectProps->props[i])) == NULL)
			LOGDEBUG2(L_DRM, "GetPropertyValue: Unable to query property.");

		if (strcmp(propName, Prop->name) == 0) {
			*value = objectProps->prop_values[i];
			found = 1;
		}

		drmModeFreeProperty(Prop);

		if (found)
			break;
	}

	drmModeFreeObjectProperties(objectProps);

	if (!found) {
		LOGDEBUG2(L_DRM, "GetPropertyValue: Unable to find value for property \'%s\'.",
			propName);
		return -1;
	}

	return 0;
}

int cVideoRender::SetPlanePropertyRequest(drmModeAtomicReqPtr ModeReq, uint32_t objectID, const char *propName, uint64_t value)
{
	struct plane *obj = NULL;

	if (objectID == planes[VIDEO_PLANE]->plane_id)
		obj = planes[VIDEO_PLANE];
	else if (objectID == planes[OSD_PLANE]->plane_id)
		obj = planes[OSD_PLANE];

	if (!obj) {
		LOGERROR("SetPlanePropertyRequest: Unable to find plane with id %d", objectID);
		return -EINVAL;
	}

	uint32_t i;
	int id = -1;

	for (i = 0; i < obj->props->count_props; i++) {
		if (strcmp(obj->props_info[i]->name, propName) == 0) {
			id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (id < 0) {
		LOGERROR("SetPlanePropertyRequest: Unable to find value for property \'%s\'.",
			propName);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(ModeReq, objectID, id, value);
}

static int SetPropertyRequest(drmModeAtomicReqPtr ModeReq, int fd_drm,
					uint32_t objectID, uint32_t objectType,
					const char *propName, uint64_t value)
{
	uint32_t i;
	uint64_t id = 0;
	drmModePropertyPtr Prop;
	drmModeObjectPropertiesPtr objectProps =
		drmModeObjectGetProperties(fd_drm, objectID, objectType);

	for (i = 0; i < objectProps->count_props; i++) {
		if ((Prop = drmModeGetProperty(fd_drm, objectProps->props[i])) == NULL)
			LOGDEBUG2(L_DRM, "SetPropertyRequest: Unable to query property.");

		if (strcmp(propName, Prop->name) == 0) {
			id = Prop->prop_id;
			drmModeFreeProperty(Prop);
			break;
		}

		drmModeFreeProperty(Prop);
	}

	drmModeFreeObjectProperties(objectProps);

	if (id == 0)
		LOGDEBUG2(L_DRM, "SetPropertyRequest: Unable to find value for property \'%s\'.",
			propName);

	return drmModeAtomicAddProperty(ModeReq, objectID, id, value);
}

void cVideoRender::SetPlaneZpos(drmModeAtomicReqPtr ModeReq, struct plane *plane)
{
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "zpos", plane->properties.zpos);
}

void cVideoRender::SetPlane(drmModeAtomicReqPtr ModeReq, struct plane *plane)
{
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "CRTC_ID", plane->properties.crtc_id);
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "FB_ID", plane->properties.fb_id);

	SetPlanePropertyRequest(ModeReq, plane->plane_id, "CRTC_X", plane->properties.crtc_x);
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "CRTC_Y", plane->properties.crtc_y);
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "CRTC_W", plane->properties.crtc_w);
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "CRTC_H", plane->properties.crtc_h);


	SetPlanePropertyRequest(ModeReq, plane->plane_id, "SRC_X", plane->properties.src_x);
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "SRC_Y", plane->properties.src_y);
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "SRC_W", plane->properties.src_w << 16);
	SetPlanePropertyRequest(ModeReq, plane->plane_id, "SRC_H", plane->properties.src_h << 16);
}

static void DumpPlaneProperties(struct plane *plane)
{
	LOGINFO("DumpPlaneProperties (plane_id = %d):", plane->plane_id);
	LOGINFO("  CRTC ID: %" PRIu64 "", plane->properties.crtc_id);
	LOGINFO("  FB ID  : %" PRIu64 "", plane->properties.fb_id);
	LOGINFO("  CRTC X : %" PRIu64 "", plane->properties.crtc_x);
	LOGINFO("  CRTC Y : %" PRIu64 "", plane->properties.crtc_y);
	LOGINFO("  CRTC W : %" PRIu64 "", plane->properties.crtc_w);
	LOGINFO("  CRTC H : %" PRIu64 "", plane->properties.crtc_h);
	LOGINFO("  SRC X  : %" PRIu64 "", plane->properties.src_x);
	LOGINFO("  SRC Y  : %" PRIu64 "", plane->properties.src_y);
	LOGINFO("  SRC W  : %" PRIu64 "", plane->properties.src_w);
	LOGINFO("  SRC H  : %" PRIu64 "", plane->properties.src_h);
	LOGINFO("  ZPOS   : %" PRIu64 "", plane->properties.zpos);
}

static size_t ReadLineFromFile(char *buf, size_t size, const char * file)
{
	FILE *fd = NULL;
	size_t character;

	fd = fopen(file, "r");
	if (fd == NULL) {
		LOGERROR("Can't open %s", file);
		return 0;
	}

	character = getline(&buf, &size, fd);

	fclose(fd);

	return character;
}

void cVideoRender::ReadHWPlatform(void)
{
	char *txt_buf;
	char *read_ptr;
	size_t bufsize = 128;
	size_t read_size;

	txt_buf = (char *) calloc(bufsize, sizeof(char));
	HardwareQuirks = 0;

	read_size = ReadLineFromFile(txt_buf, bufsize, "/sys/firmware/devicetree/base/compatible");
	if (!read_size) {
		free((void *)txt_buf);
		return;
	}

	read_ptr = txt_buf;
	// be aware: device tree string can contain \x0 bytes, so every C-string function
	// thinks, we already reached the string's terminating null bytes
	// so copy the string into a temporary string without the "\0"
	char *_txt_buf = (char *) calloc(bufsize, sizeof(char));
	char *_read_ptr = _txt_buf;
	for (size_t i = 0; i < bufsize; i++) {
		if (memcmp(read_ptr, "\0", sizeof(char))) {
			memcpy(_read_ptr, read_ptr, sizeof(char));
			_read_ptr++;
		}
		read_ptr++;
	}

	read_ptr = txt_buf;
	LOGDEBUG2(L_DRM, "ReadHWPlatform: found \"%s\", set hardware quirks", _txt_buf);

	while(read_size) {
		if (strstr(read_ptr, "bcm2837")) {
			LOGDEBUG2(L_DRM, "ReadHWPlatform: bcm2837 (Raspberry Pi 2/3) found");
			HardwareQuirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
			break;
		}
		if (strstr(read_ptr, "bcm2711")) {
			LOGDEBUG2(L_DRM, "ReadHWPlatform: bcm2711 (Raspberry Pi 4 Model B, Compute Module 4, Pi 400) found");
			HardwareQuirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
			break;
		}
		if (strstr(read_ptr, "bcm2712")) {
			LOGDEBUG2(L_DRM, "ReadHWPlatform: bcm2712 (Raspberry Pi 5, Compute Module 5, Pi 500) found");
			HardwareQuirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
			break;
		}
		if (strstr(read_ptr, "amlogic")) {
			LOGDEBUG2(L_DRM, "ReadHWPlatform: amlogic found, disable HW deinterlacer");
			HardwareQuirks |= QUIRK_CODEC_NEEDS_EXT_INIT
				       |  QUIRK_CODEC_SKIP_FIRST_FRAMES
				       |  QUIRK_NO_HW_DEINT;
			break;
		}

		read_size -= (strlen(read_ptr) + 1);
		read_ptr = (char *)&read_ptr[(strlen(read_ptr) + 1)];
	}
	free((void *)txt_buf);
}

static int TestCaps(int fd)
{
	uint64_t test;

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &test) < 0 || test == 0)
		return 1;

	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0)
		return 1;

	if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
		return 1;

	if (drmGetCap(fd, DRM_CAP_PRIME, &test) < 0)
		return 1;

	if (drmGetCap(fd, DRM_PRIME_CAP_EXPORT, &test) < 0)
		return 1;

	if (drmGetCap(fd, DRM_PRIME_CAP_IMPORT, &test) < 0)
		return 1;

	return 0;
}

int cVideoRender::CheckZpos(struct plane *plane, uint64_t zpos)
{
	drmModeAtomicReqPtr ModeReq;
	const uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;

	if (!(ModeReq = drmModeAtomicAlloc()))
		LOGERROR("CheckZpos: cannot allocate atomic request (%d): %m", errno);

	plane->properties.zpos = zpos;
	SetPlaneZpos(ModeReq, plane);

	if (drmModeAtomicCommit(fd_drm, ModeReq, flags, NULL) != 0) {
		LOGDEBUG2(L_DRM, "CheckZpos: cannot set atomic mode (%d), don't use zpos change: %m", errno);
		use_zpos = 0;
		drmModeAtomicFree(ModeReq);
		return 1;
	}

	drmModeAtomicFree(ModeReq);

	return 0;
}

#ifdef USE_GLES
static const EGLint context_attribute_list[] =
{
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE
};

EGLConfig cVideoRender::get_config(void)
{
    EGLint config_attribute_list[] = {
        EGL_BUFFER_SIZE, 32,
        EGL_STENCIL_SIZE, EGL_DONT_CARE,
        EGL_DEPTH_SIZE, EGL_DONT_CARE,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLConfig *configs;
    EGLint matched;
    EGLint count;
    eglGetConfigs(eglDisplay, NULL, 0, &count);
//    EGL_CHECK(eglGetConfigs(eglDisplay, NULL, 0, &count));
    if (count < 1) {
        LOGFATAL("no EGL configs to choose from");
    }

    LOGDEBUG2(L_OPENGL, "%d EGL configs found", count);

    configs = (EGLConfig *)malloc(count * sizeof(*configs));
    if (!configs)
        LOGFATAL("can't allocate space for EGL configs");

    eglChooseConfig(eglDisplay, config_attribute_list, configs, count, &matched);
//    EGL_CHECK(eglChooseConfig(eglDisplay, config_attribute_list, configs, count, &matched));
    if (!matched) {
        LOGFATAL("no EGL configs with appropriate attributes");
    }

    LOGDEBUG2(L_OPENGL, "%d appropriate EGL configs found, which match attributes", matched);

    for (int i = 0; i < matched; ++i) {
        EGLint gbm_format;
        eglGetConfigAttrib(eglDisplay, configs[i], EGL_NATIVE_VISUAL_ID, &gbm_format);
//        EGL_CHECK(eglGetConfigAttrib(eglDisplay, configs[i], EGL_NATIVE_VISUAL_ID, &gbm_format));

        if (gbm_format == GBM_FORMAT_ARGB8888)
            return configs[i];
    }

    LOGFATAL("no matching gbm config found");
    return NULL;
}

PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = NULL;
PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC get_platform_surface = NULL;
#endif

static void get_properties(int fd, int plane_id, struct plane *plane)
{
	uint32_t i;
	plane->props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
	if (!plane->props) {
		LOGERROR("could not get %u properties: %s",
			plane_id, strerror(errno));
		return;
	}
	plane->props_info = (drmModePropertyRes **)calloc(plane->props->count_props, sizeof(*plane->props_info)); \
	for (i = 0; i < plane->props->count_props; i++) {
		plane->props_info[i] = drmModeGetProperty(fd, plane->props->props[i]);
	}
}

static void free_properties(struct plane *plane)
{
	if (!plane->props)
		return;

	if (!plane->props_info)
		return;

	for (uint32_t i = 0; i < plane->props->count_props; i++) {
		if (plane->props_info[i]) {
			drmModeFreeProperty(plane->props_info[i]);
		}
	}
	drmModeFreeObjectProperties(plane->props);
	free(plane->props_info);
}

void cVideoRender::VideoSetDisplay(const char* resolution)
{
	sscanf(resolution, "%dx%d@%d", &VideoDisplayWidth, &VideoDisplayHeight, &VideoDisplayRefresh);
}

static drmModeConnector *find_drm_connector(int fd, drmModeRes *resources)
{
	drmModeConnector *connector = NULL;
	int i;

	// search for a connected connector
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (connector && connector->connection == DRM_MODE_CONNECTED)
			return connector;
		drmModeFreeConnector(connector);
		connector = NULL;
	}

	// search for a not connected connector, but with available modes
	// this is a workaround for RPI: in case we don't have a monitor connected
	// we can load an edid file at boot time, where the available modes are listed.
	// To bring softhddevice up, we also have to go through the not connected connectors
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if (connector && connector->count_modes > 0)
			return connector;
		drmModeFreeConnector(connector);
		connector = NULL;
	}

	// we couldn't find a connector
	return connector;
}

static int32_t find_crtc_for_encoder(const drmModeRes *resources, const drmModeEncoder *encoder)
{
	int i;

	for (i = 0; i < resources->count_crtcs; i++) {
		const uint32_t crtc_mask = 1 << i;
		const uint32_t crtc_id = resources->crtcs[i];
		if (encoder->possible_crtcs & crtc_mask) {
			return crtc_id;
		}
	}

	return -1;
}

static int get_resources(int fd, drmModeRes **resources)
{
	*resources = drmModeGetResources(fd);
	if (*resources == NULL) {
		LOGERROR("FindDevice: cannot retrieve DRM resources (%d): %m", errno);
		return -1;
	}
	return 0;
}

#define MAX_DRM_DEVICES 64
static int find_drm_device(drmModeRes **resources)
{
	drmDevicePtr devices[MAX_DRM_DEVICES] = { NULL };
	int num_devices, fd = -1;

	num_devices = drmGetDevices2(0, devices,MAX_DRM_DEVICES);
	if (num_devices < 0) {
		LOGERROR("FindDevice: drmGetDevices2 failed: %s", strerror(-num_devices));
		return fd;
	}

	for (int i = 0; i < num_devices && fd < 0; i++) {
		drmDevicePtr device = devices[i];
		int ret;

		if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY)))
			continue;
		fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR);
		if (fd < 0)
			continue;

		if (TestCaps(fd)) {
			close(fd);
			fd = -1;
			continue;
		}

		ret = get_resources(fd, resources);
		if (!ret)
			break;
		close(fd);
		fd = -1;
	}
	drmFreeDevices(devices, num_devices);

	if (fd < 0)
		LOGERROR("FindDevice: no drm device found!");

	return fd;
}

int32_t cVideoRender::find_crtc_for_connector(const drmModeRes *resources, const drmModeConnector *connector)
{
	int i;

	for (i = 0; i < connector->count_encoders; i++) {
		const uint32_t encoder_id = connector->encoders[i];
		drmModeEncoder *encoder = drmModeGetEncoder(fd_drm, encoder_id);

		if (encoder) {
			const int32_t crtc_id = find_crtc_for_encoder(resources, encoder);
			drmModeFreeEncoder(encoder);
			if (crtc_id != 0) {
				return crtc_id;
			}
		}
	}

	return -1;
}

#ifdef USE_GLES
int cVideoRender::init_gbm(int w, int h, uint32_t format, uint64_t modifier)
{
	gbm_device = gbm_create_device(fd_drm);
	if (!gbm_device) {
		LOGERROR("FindDevice: failed to create gbm device!");
		return -1;
	}

	gbm_surface = gbm_surface_create(gbm_device, w, h, format, modifier);
	if (!gbm_surface) {
		LOGERROR("FindDevice: failed to create %d x %d surface bo", w, h);
		return -1;
	}

	return 0;
}

int cVideoRender::init_egl(void)
{
	EGLint iMajorVersion, iMinorVersion;

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	assert(get_platform_display != NULL);
	PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC get_platform_surface = (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
	assert(get_platform_surface != NULL);

	eglDisplay = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm_device, NULL);
//	EGL_CHECK(eglDisplay = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm_device, NULL));
	if (!eglDisplay) {
		LOGERROR("FindDevice: failed to get eglDisplay");
		return -1;
	}

	if (!eglInitialize(eglDisplay, &iMajorVersion, &iMinorVersion)) {
		LOGERROR("FindDevice: eglInitialize failed");
		return -1;
	}

	LOGDEBUG2(L_OPENGL, "FindDevice: Using display %p with EGL version %d.%d", eglDisplay, iMajorVersion, iMinorVersion);
	EGL_CHECK(LOGDEBUG2(L_OPENGL, "  EGL Version: \"%s\"", eglQueryString(eglDisplay, EGL_VERSION)));
	EGL_CHECK(LOGDEBUG2(L_OPENGL, "  EGL Vendor: \"%s\"", eglQueryString(eglDisplay, EGL_VENDOR)));
	EGL_CHECK(LOGDEBUG2(L_OPENGL, "  EGL Extensions: \"%s\"", eglQueryString(eglDisplay, EGL_EXTENSIONS)));
	EGL_CHECK(LOGDEBUG2(L_OPENGL, "  EGL APIs: \"%s\"", eglQueryString(eglDisplay, EGL_CLIENT_APIS)));

	EGLConfig eglConfig = get_config();

	EGL_CHECK(eglBindAPI(EGL_OPENGL_ES_API));
	EGL_CHECK(eglContext = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, context_attribute_list));
	if (!eglContext) {
		LOGERROR("FindDevice: failed to create eglContext");
		return -1;
	}

	EGL_CHECK(eglSurface = get_platform_surface(eglDisplay, eglConfig, gbm_surface, NULL));
	if (eglSurface == EGL_NO_SURFACE) {
		LOGERROR("FindDevice: failed to create eglSurface");
		return -1;
	}

	EGLint s_width, s_height;
	EGL_CHECK(eglQuerySurface(eglDisplay, eglSurface, EGL_WIDTH, &s_width));
	EGL_CHECK(eglQuerySurface(eglDisplay, eglSurface, EGL_HEIGHT, &s_height));

	LOGDEBUG2(L_OPENGL, "FindDevice: EGLSurface %p on EGLDisplay %p for %d x %d BO created", eglSurface, eglDisplay, s_width, s_height);

	GlInit = 1;
	LOGINFO("EGL context initialized");

	return 0;
}
#endif

int cVideoRender::FindDevice(void)
{
	drmModeRes *resources;
	drmModeConnector *connector;
	drmModeEncoder *encoder = NULL;
	drmModeModeInfo *drmmode = NULL;
	drmModePlane *plane;
	drmModePlaneRes *plane_res;
	int i;
	uint32_t j, k;

	// find a drm device
	fd_drm = find_drm_device(&resources);
	if (fd_drm < 0) {
		LOGERROR("FindDevice: Could not open device!");
		return -1;
	}

	LOGDEBUG2(L_DRM, "FindDevice: DRM have %i connectors, %i crtcs, %i encoders",
		resources->count_connectors, resources->count_crtcs,
		resources->count_encoders);

	// find a connector
	connector = find_drm_connector(fd_drm, resources);
	if (!connector) {
		LOGERROR("FindDevice: cannot retrieve DRM connector (%d): %m", errno);
		return -errno;
	}
	connector_id = connector->connector_id;

	// find a user requested mode
	if (VideoDisplayWidth) {
		for (i = 0; i < connector->count_modes; i++) {
			drmModeModeInfo *current_mode = &connector->modes[i];
			if(current_mode->hdisplay == VideoDisplayWidth && current_mode->vdisplay == VideoDisplayHeight &&
			   current_mode->vrefresh == VideoDisplayRefresh && !(current_mode->flags & DRM_MODE_FLAG_INTERLACE)) {
				drmmode = current_mode;
				LOGDEBUG2(L_DRM, "FindDevice: Use user requested mode: %dx%d@%d", drmmode->hdisplay, drmmode->vdisplay, drmmode->vrefresh);
				break;
			}
		}
		if (!drmmode)
			LOGWARNING("FindDevice: User requested mode not found, try default modes");
	}

	uint32_t preferred_hz[3] = {50, 60, 0};

	// find the highest resolution mode with 50, 60 or any refresh rate
	if (!drmmode) {
		j = 0;
		int width;
find_mode:
		for (i = 0, width = 0; i < connector->count_modes; i++) {
			drmModeModeInfo *current_mode = &connector->modes[i];
			if (preferred_hz[j] && current_mode->vrefresh != preferred_hz[j])
				continue;

			int current_width = current_mode->hdisplay;
			if (current_width > width) {
				drmmode = current_mode;
				width = current_width;
			}
		}

		if (!drmmode && preferred_hz[j]) {
			j++;
			goto find_mode;
		}

		if (drmmode)
			LOGDEBUG2(L_DRM, "FindDevice: Use mode with the biggest width: %dx%d@%d",
				drmmode->hdisplay, drmmode->vdisplay, drmmode->vrefresh);
	}

	if (!drmmode) {
		LOGERROR("FindDevice: No monitor mode found! Probably no monitor connected, giving up!");
		return -1;
	}

	memcpy(&mode, drmmode, sizeof(drmModeModeInfo));

	// find encoder
	for (i = 0; i < resources->count_encoders; i++) {
		encoder = drmModeGetEncoder(fd_drm, resources->encoders[i]);
		if (encoder->encoder_id == connector->encoder_id)
			break;
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}

	if (encoder) {
		crtc_id = encoder->crtc_id;
		LOGDEBUG2(L_DRM, "FindDevice: have encoder, crtc_id %d", crtc_id);
	} else {
		int32_t crtc_id = find_crtc_for_connector(resources, connector);
		if (crtc_id == -1) {
			LOGERROR("FindDevice: No crtc found!");
			return -errno;
		}

		crtc_id = crtc_id;
		LOGDEBUG2(L_DRM, "FindDevice: have no encoder, crtc_id %d", crtc_id);
	}

	for (i = 0; i < resources->count_crtcs; i++) {
		if (resources->crtcs[i] == crtc_id) {
			crtc_index = i;
			break;
		}
	}

	LOGINFO("FindDevice: Using Monitor Mode %dx%d@%d, crtc_id %d crtc_idx %d",
		mode.hdisplay, mode.vdisplay, mode.vrefresh, crtc_id, crtc_index);

	drmModeFreeConnector(connector);


	// find planes
	if ((plane_res = drmModeGetPlaneResources(fd_drm)) == NULL) {
		LOGERROR("FindDevice: cannot retrieve PlaneResources (%d): %m", errno);
		return -1;
	}

	// allocate local plane structs
	for (i = 0; i < MAX_PLANES; i++) {
		planes[i] = (struct plane *)calloc(1, sizeof(struct plane));
	}

	// test and list the local plane structs
	struct plane best_primary_video_plane; // is the NV12 capable primary plane with the lowest plane_id
	best_primary_video_plane.plane_id = 0;
	best_primary_video_plane.properties.zpos = 0;
	struct plane best_overlay_video_plane; // is the NV12 capable overlay plane with the lowest plane_id
	best_overlay_video_plane.plane_id = 0;
	best_overlay_video_plane.properties.zpos = 0;
	struct plane best_primary_osd_plane;   // is the AR24 capable primary plane with the highest plane_id
	best_primary_osd_plane.plane_id = 0;
	best_primary_osd_plane.properties.zpos = 0;
	struct plane best_overlay_osd_plane;   // is the AR24 capable overlay plane with the highest plane_id
	best_overlay_osd_plane.plane_id = 0;
	best_overlay_osd_plane.properties.zpos = 0;

	for (j = 0; j < plane_res->count_planes; j++) {
		plane = drmModeGetPlane(fd_drm, plane_res->planes[j]);

		if (plane == NULL) {
			LOGERROR("FindDevice: cannot query DRM-KMS plane %d", j);
			continue;
		}

		uint64_t type;
		uint64_t zpos;
		char pixelformats[256];

		if (plane->possible_crtcs & (1 << crtc_index)) {
			if (GetPropertyValue(fd_drm, plane_res->planes[j],
					     DRM_MODE_OBJECT_PLANE, "type", &type)) {
				LOGDEBUG2(L_DRM, "FindDevice: Failed to get property 'type'");
			}
			if (GetPropertyValue(fd_drm, plane_res->planes[j],
					     DRM_MODE_OBJECT_PLANE, "zpos", &zpos)) {
				LOGDEBUG2(L_DRM, "FindDevice: Failed to get property 'zpos'");
			} else {
				use_zpos = 1;
			}

			LOGDEBUG2(L_DRM, "FindDevice: %s: id %i possible_crtcs %i",
				(type == DRM_PLANE_TYPE_PRIMARY) ? "PRIMARY " :
				(type == DRM_PLANE_TYPE_OVERLAY) ? "OVERLAY " :
				(type == DRM_PLANE_TYPE_CURSOR) ? "CURSOR " : "UNKNOWN",
				plane->plane_id, plane->possible_crtcs);
			strcpy(pixelformats, "            ");

			// test pixel format and plane caps
			for (k = 0; k < plane->count_formats; k++) {
				if (encoder->possible_crtcs & plane->possible_crtcs) {
					char tmp[10];
					switch (plane->formats[k]) {
						case DRM_FORMAT_NV12:
							snprintf(tmp, sizeof(tmp), " %4.4s", (char *)&plane->formats[k]);
							strcat(pixelformats, tmp);
							if (type == DRM_PLANE_TYPE_PRIMARY && !best_primary_video_plane.plane_id) {
								best_primary_video_plane.plane_id = plane->plane_id;
								best_primary_video_plane.type = type;
								best_primary_video_plane.properties.zpos = zpos;
								strcat(pixelformats, "!  ");
							}
							if (type == DRM_PLANE_TYPE_OVERLAY && !best_overlay_video_plane.plane_id) {
								best_overlay_video_plane.plane_id = plane->plane_id;
								best_overlay_video_plane.type = type;
								best_overlay_video_plane.properties.zpos = zpos;
								strcat(pixelformats, "!  ");
							}
							break;
						case DRM_FORMAT_ARGB8888:
							snprintf(tmp, sizeof(tmp), " %4.4s", (char *)&plane->formats[k]);
							strcat(pixelformats, tmp);
							if (type == DRM_PLANE_TYPE_PRIMARY) {
								best_primary_osd_plane.plane_id = plane->plane_id;
								best_primary_osd_plane.type = type;
								best_primary_osd_plane.properties.zpos = zpos;
								strcat(pixelformats, "!  ");
							}
							if (type == DRM_PLANE_TYPE_OVERLAY) {
								best_overlay_osd_plane.plane_id = plane->plane_id;
								best_overlay_osd_plane.type = type;
								best_overlay_osd_plane.properties.zpos = zpos;
								strcat(pixelformats, "!  ");
							}
							break;
						default:
							break;
					}
				}
			}
			LOGDEBUG2(L_DRM, "%s", pixelformats);
		}
		drmModeFreePlane(plane);
	}

	// debug output
	if (best_primary_video_plane.plane_id) {
		LOGDEBUG2(L_DRM, "FindDevice: best_primary_video_plane: plane_id %d, type %s, zpos %" PRIu64 "",
			best_primary_video_plane.plane_id, best_primary_video_plane.type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY", best_primary_video_plane.properties.zpos);
	}
	if (best_overlay_video_plane.plane_id) {
		LOGDEBUG2(L_DRM, "FindDevice: best_overlay_video_plane: plane_id %d, type %s, zpos %" PRIu64 "",
			best_overlay_video_plane.plane_id, best_overlay_video_plane.type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY", best_overlay_video_plane.properties.zpos);
	}
	if (best_primary_osd_plane.plane_id) {
		LOGDEBUG2(L_DRM, "FindDevice: best_primary_osd_plane: plane_id %d, type %s, zpos %" PRIu64 "",
			best_primary_osd_plane.plane_id, best_primary_osd_plane.type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY", best_primary_osd_plane.properties.zpos);
	}
	if (best_overlay_osd_plane.plane_id) {
		LOGDEBUG2(L_DRM, "FindDevice: best_overlay_osd_plane: plane_id %d, type %s, zpos %" PRIu64 "",
			best_overlay_osd_plane.plane_id, best_overlay_osd_plane.type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY", best_overlay_osd_plane.properties.zpos);
	}

	// See which planes we should use
	if (best_primary_video_plane.plane_id && best_overlay_osd_plane.plane_id) {
		planes[VIDEO_PLANE]->plane_id = best_primary_video_plane.plane_id;
		planes[VIDEO_PLANE]->type = best_primary_video_plane.type;
		planes[VIDEO_PLANE]->properties.zpos = zpos_primary = best_primary_video_plane.properties.zpos;
		planes[OSD_PLANE]->plane_id = best_overlay_osd_plane.plane_id;
		planes[OSD_PLANE]->type = best_overlay_osd_plane.type;
		planes[OSD_PLANE]->properties.zpos = zpos_overlay = best_overlay_osd_plane.properties.zpos;
	} else if (best_overlay_video_plane.plane_id && best_primary_osd_plane.plane_id) {
		planes[VIDEO_PLANE]->plane_id = best_overlay_video_plane.plane_id;
		planes[VIDEO_PLANE]->type = best_overlay_video_plane.type;
		planes[VIDEO_PLANE]->properties.zpos = zpos_overlay = best_overlay_video_plane.properties.zpos;
		planes[OSD_PLANE]->plane_id = best_primary_osd_plane.plane_id;
		planes[OSD_PLANE]->type = best_primary_osd_plane.type;
		planes[OSD_PLANE]->properties.zpos = zpos_primary = best_primary_osd_plane.properties.zpos;
		use_zpos = 1;
	} else {
		LOGERROR("FindDevice: No suitable planes found!");
		return -1;
	}

	// fill the plane's properties to speed up SetPropertyRequest later
	get_properties(fd_drm, planes[VIDEO_PLANE]->plane_id, planes[VIDEO_PLANE]);
	get_properties(fd_drm, planes[OSD_PLANE]->plane_id, planes[OSD_PLANE]);

	// Check, if we can set z-order (meson and rpi have fixed z-order, which cannot be changed)
	if (use_zpos && CheckZpos(planes[VIDEO_PLANE], planes[VIDEO_PLANE]->properties.zpos)) {
		use_zpos = 0;
	}
	if (use_zpos && CheckZpos(planes[OSD_PLANE], planes[OSD_PLANE]->properties.zpos)) {
		use_zpos = 0;
	}

	// use_zpos was set, if Video is on OVERLAY, and Osd is on PRIMARY
	// Check if the OVERLAY plane really got a higher zpos than the PRIMARY plane
	// If not, change their zpos values or hardcode them to
	// 1 OVERLAY (Video)
	// 0 PRIMARY (Osd)
	if (use_zpos && zpos_overlay <= zpos_primary) {
		char str_zpos[256];
		strcpy(str_zpos, "FindDevice: zpos values are wrong, so ");
		if (zpos_overlay == zpos_primary) {
			// is this possible?
			strcat(str_zpos, "hardcode them to 0 and 1, because they are equal");
			zpos_primary = 0;
			zpos_overlay = 1;
		} else {
			strcat(str_zpos, "switch them");
			uint64_t zpos_tmp = zpos_primary;
			zpos_primary = zpos_overlay;
			zpos_overlay = zpos_tmp;
		}
		LOGDEBUG2(L_DRM, "%s", str_zpos);
	}
	drmModeFreePlaneResources(plane_res);
	drmModeFreeEncoder(encoder);
	drmModeFreeResources(resources);

	LOGINFO("FindDevice: DRM setup - CRTC: %i video_plane: %i (%s %" PRIu64 ") osd_plane: %i (%s %" PRIu64 ") use_zpos: %d",
		crtc_id, planes[VIDEO_PLANE]->plane_id,
		planes[VIDEO_PLANE]->type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY",
		planes[VIDEO_PLANE]->properties.zpos,
		planes[OSD_PLANE]->plane_id,
		planes[OSD_PLANE]->type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY",
		planes[OSD_PLANE]->properties.zpos,
		use_zpos);

#ifdef USE_GLES
	if (DisableOglOsd)
		return 0;

	// init gbm
	int w, h;
	double pixel_aspect;
	VideoGetScreenSize(&w, &h, &pixel_aspect);

	if (init_gbm(w, h, DRM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING)) {
		LOGERROR("FindDevice: failed to init gbm device and surface!");
		return -1;
	}

	// init egl
	if (init_egl()) {
		LOGERROR("FindDevice: failed to init egl!");
		return -1;
	}
#endif

	return 0;
}

#ifdef USE_GLES
static void drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
	int drm_fd = gbm_device_get_fd(gbm_bo_get_device(bo));
	struct drm_buf *buf = (struct drm_buf *)data;

	if (buf->fb_id)
		drmModeRmFB(drm_fd, buf->fb_id);

	free(buf);
}

__attribute__ ((weak)) union gbm_bo_handle
gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane);

__attribute__ ((weak)) int
gbm_bo_get_fd(struct gbm_bo *bo);

__attribute__ ((weak)) uint64_t
gbm_bo_get_modifier(struct gbm_bo *bo);

__attribute__ ((weak)) int
gbm_bo_get_plane_count(struct gbm_bo *bo);

__attribute__ ((weak)) uint32_t
gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane);

__attribute__ ((weak)) uint32_t
gbm_bo_get_offset(struct gbm_bo *bo, int plane);

struct drm_buf *cVideoRender::drm_get_buf_from_bo(struct gbm_bo *bo)
{
	struct drm_buf *buf = (struct drm_buf *)gbm_bo_get_user_data(bo);
	uint32_t mod_flags = 0;
	int ret = -1;

	// the buffer was already allocated
	if (buf)
		return buf;

	buf = (struct drm_buf *)calloc(1, sizeof *buf);
	buf->bo = bo;

	buf->width = gbm_bo_get_width(bo);
	buf->height = gbm_bo_get_height(bo);
	buf->pix_fmt = gbm_bo_get_format(bo);

	if (gbm_bo_get_handle_for_plane && gbm_bo_get_modifier &&
            gbm_bo_get_plane_count && gbm_bo_get_stride_for_plane &&
            gbm_bo_get_offset) {
		uint64_t modifiers[4] = {0};
		modifiers[0] = gbm_bo_get_modifier(bo);
		const int num_planes = gbm_bo_get_plane_count(bo);
		buf->num_planes = num_planes;
		for (int i = 0; i < num_planes; i++) {
			buf->handle[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
			buf->pitch[i] = gbm_bo_get_stride_for_plane(bo, i);
			buf->offset[i] = gbm_bo_get_offset(bo, i);
			modifiers[i] = modifiers[0];
			buf->size[i] = buf->height * buf->pitch[i];
			LOGDEBUG2(L_DRM, "drm_get_buf_from_bo: %d: handle %d pitch %d, offset %d, size %d", i, buf->handle[i], buf->pitch[i], buf->offset[i], buf->size[i]);
		}
		buf->nb_objects = 1;
		buf->obj_index[0] = 0;
		buf->fd_prime[0] = gbm_bo_get_fd(bo);

		if (modifiers[0]) {
			mod_flags = DRM_MODE_FB_MODIFIERS;
			LOGDEBUG2(L_DRM, "drm_get_buf_from_bo: Using modifier %" PRIx64 "", modifiers[0]);
		}

		// Add FB
		ret = drmModeAddFB2WithModifiers(fd_drm, buf->width, buf->height, buf->pix_fmt,
			buf->handle, buf->pitch, buf->offset, modifiers, &buf->fb_id, mod_flags);
	}

	if (ret) {
		if (mod_flags)
			LOGDEBUG2(L_DRM, "drm_get_buf_from_bo: Modifiers failed!");

		buf->num_planes = 1;
		memcpy(buf->handle, (uint32_t [4]){ gbm_bo_get_handle(bo).u32, 0, 0, 0}, 16);
		memcpy(buf->pitch, (uint32_t [4]){ gbm_bo_get_stride(bo), 0, 0, 0}, 16);
		memset(buf->offset, 0, 16);
		memcpy(buf->size, (uint32_t [4]){ buf->height * buf->width * buf->pitch[0], 0, 0, 0}, 16);
		buf->nb_objects = 1;
		buf->obj_index[0] = 0;
		buf->fd_prime[0] = gbm_bo_get_fd(bo);
		ret = drmModeAddFB2(fd_drm, buf->width, buf->height, buf->pix_fmt,
			buf->handle, buf->pitch, buf->offset, &buf->fb_id, 0);
	}

	if (ret) {
		LOGFATAL("drm_get_buf_from_bo: cannot create framebuffer (%d): %m", errno);
		free(buf);
		return NULL;
	}

	LOGDEBUG2(L_DRM, "drm_get_buf_from_bo: New GL buffer %d x %d pix_fmt %4.4s fb_id %d",
		buf->width, buf->height, (char *)&buf->pix_fmt, buf->fb_id);

	gbm_bo_set_user_data(bo, buf, drm_fb_destroy_callback);
	return buf;
}
#endif

void cVideoRender::ThreadExitHandler( __attribute__ ((unused)) void * arg)
{
	FilterThread = 0;
}

static const struct format_info format_info_array[] = {
	{ DRM_FORMAT_NV12, "NV12", 2, { { 8, 1, 1 }, { 16, 2, 2 } }, },
	{ DRM_FORMAT_YUV420, "YU12", 3, { { 8, 1, 1 }, { 8, 2, 2 }, {8, 2, 2 } }, },
	{ DRM_FORMAT_ARGB8888, "AR24", 1, { { 32, 1, 1 } }, },
};

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
static const struct format_info *find_format(uint32_t format)
{
	for (int i = 0; i < (int)ARRAY_SIZE(format_info_array); i++) {
		if (format == format_info_array[i].format)
			return &format_info_array[i];
	}
	return NULL;
}

int cVideoRender::SetupFB(struct drm_buf *buf,
			AVDRMFrameDescriptor *primedata)
{
	uint64_t modifier[4] = { 0, 0, 0, 0 };
	uint32_t mod_flags = 0;
	buf->handle[0] = buf->handle[1] = buf->handle[2] = buf->handle[3] = 0;
	buf->pitch[0] = buf->pitch[1] = buf->pitch[2] = buf->pitch[3] = 0;
	buf->offset[0] = buf->offset[1] = buf->offset[2] = buf->offset[3] = 0;
	buf->nb_objects = buf->num_planes = 0;

	if (primedata) {
		// we have no DRM objects yet, so return
		if (!primedata->nb_objects) {
			LOGWARNING("SetupFB: No primedata objects available!");
			return 1;
		}

		AVDRMLayerDescriptor *layer = &primedata->layers[0];

		buf->pix_fmt = layer->format;
		buf->num_planes = layer->nb_planes;
		buf->nb_objects = primedata->nb_objects;

		LOGDEBUG2(L_DRM, "SetupFB: PRIMEDATA %d x %d, pix_fmt %4.4s nb_planes %d nb_objects %d",
			buf->width, buf->height, (char *)&buf->pix_fmt, buf->num_planes, buf->nb_objects);

		// create handles for PrimeFDs
		for (int object = 0; object < primedata->nb_objects; object++) {
			if (drmPrimeFDToHandle(fd_drm,
				primedata->objects[object].fd, &buf->primehandle[object])) {

				LOGERROR("SetupFB: PRIMEDATA Failed to retrieve the Prime Handle %i size %zu (%d): %m",
					primedata->objects[object].fd,
					primedata->objects[object].size, errno);
				return -errno;
			}
			buf->fd_prime[object] = primedata->objects[object].fd;
			buf->size[object] = primedata->objects[object].size;
			LOGDEBUG2(L_DRM, "SetupFB: PRIMEDATA create handle for PrimeFD (%d|%i): PrimeFD %i ToHandle %i size %zu modifier %" PRIx64 "",
				object, primedata->nb_objects, primedata->objects[object].fd, buf->primehandle[object],
				primedata->objects[object].size, primedata->objects[object].format_modifier);
		}

		// fill the planes
		for (int plane = 0; plane < layer->nb_planes; plane++) {
			int object = layer->planes[plane].object_index;
			uint32_t handle = buf->primehandle[object];
			if (handle) {
				buf->handle[plane] = handle;
				buf->pitch[plane] = layer->planes[plane].pitch;
				buf->offset[plane] = layer->planes[plane].offset;
				buf->obj_index[plane] = object;
				if (primedata->objects[object].format_modifier)
					modifier[plane] = primedata->objects[object].format_modifier;

				LOGDEBUG2(L_DRM, "SetupFB: PRIMEDATA fill plane %d: handle %d object_index %i pitch %d offset %d size %d modifier %" PRIx64 " (buf->plane not mapped!)",
					plane, buf->handle[plane], buf->obj_index[plane], buf->pitch[plane], buf->offset[plane], buf->size[plane], modifier[plane]);
			}
		}
		if (modifier[0] && modifier[0] != DRM_FORMAT_MOD_INVALID)
			mod_flags = DRM_MODE_FB_MODIFIERS;
	} else {
		const struct format_info *format_info = find_format(buf->pix_fmt);
		if (!format_info) {
			LOGERROR("SetupFB: No suitable format found!");
			return 1;
		}

		buf->num_planes = format_info->num_planes;

		LOGDEBUG2(L_DRM, "SetupFB:  %d x %d, pix_fmt %4.4s nb_planes %d",
			buf->width, buf->height, (char *)&buf->pix_fmt, buf->num_planes);

		for (int plane = 0; plane < format_info->num_planes; plane++) {
			const struct format_plane_info *plane_info = &format_info->planes[plane];

			struct drm_mode_create_dumb creq;
			creq.height = buf->height / plane_info->ysub;
			creq.width = buf->width / plane_info->xsub;
			creq.bpp = plane_info->bitspp;

			if (drmIoctl(fd_drm, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0){
				LOGERROR("SetupFB: cannot create dumb buffer %dx%d@%d (%d): %m",
					creq.width, creq.height, creq.bpp, errno);
				return -errno;
			}

			buf->handle[plane] = creq.handle;
			buf->pitch[plane] = creq.pitch;
			buf->size[plane] = creq.size;

			struct drm_mode_map_dumb mreq;
			memset(&mreq, 0, sizeof(struct drm_mode_map_dumb));
			mreq.handle = buf->handle[plane];

			if (drmIoctl(fd_drm, DRM_IOCTL_MODE_MAP_DUMB, &mreq)){
				LOGERROR("SetupFB: cannot prepare dumb buffer for mapping (%d): %m", errno);
				return -errno;
			}

			buf->plane[plane] = (uint8_t *)mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_drm, mreq.offset);

			if (buf->plane[plane] == MAP_FAILED) {
				LOGERROR("SetupFB: cannot map dumb buffer (%d): %m", errno);
				return -errno;
			}

			memset(buf->plane[plane], 0, buf->size[plane]);

			LOGDEBUG2(L_DRM, "SetupFB: fill plane %d: handle %d pitch %d offset %d size %d address %p",
				plane, buf->handle[plane], buf->pitch[plane], buf->offset[plane], buf->size[plane], buf->plane[plane]);
		}
	}

	int ret = -1;
	ret = drmModeAddFB2WithModifiers(fd_drm, buf->width, buf->height, buf->pix_fmt,
					 buf->handle, buf->pitch, buf->offset, modifier, &buf->fb_id, mod_flags);

	if (ret) {
		if (mod_flags)
			LOGERROR("SetupFB: cannot create modifiers framebuffer (%d): %m", errno);

		ret = drmModeAddFB2(fd_drm, buf->width, buf->height, buf->pix_fmt,
			buf->handle, buf->pitch, buf->offset, &buf->fb_id, 0);
	}

	if (ret)
		LOGFATAL("SetupFB: cannot create framebuffer (%d): %m", errno);

	LOGDEBUG2(L_DRM, "SetupFB: Added %sFB fb_id %d width %d height %d pix_fmt %4.4s",
		primedata ? "primedata " : "", buf->fb_id, buf->width, buf->height, (char *)&buf->pix_fmt);

	buf->dirty = 1;
	return 0;
}

void cVideoRender::DestroyFB(struct drm_buf *buf)
{
	struct drm_mode_destroy_dumb dreq;

	LOGDEBUG2(L_DRM, "DestroyFB: destroy FB %d", buf->fb_id);

	for (int i = 0; i < buf->num_planes; i++) {
		if (buf->plane[i]) {
			if (munmap(buf->plane[i], buf->size[i]))
				LOGERROR("DestroyFB: failed unmap FB (%d): %m", errno);
		}
	}

	if (drmModeRmFB(fd_drm, buf->fb_id) < 0)
		LOGERROR("DestroyFB: cannot rm FB (%d): %m", errno);

	if (buf->fd_prime[0] && buf->swbuffer) {
		if (close(buf->fd_prime[0]))
			LOGERROR("DestroyFB: error closing prime fd %d (%d): %m", buf->fd_prime[0], errno);
		buf->fd_prime[0] = 0;
	}

	for (int i = 0; i < buf->num_planes; i++) {
		if (buf->plane[i]) {
			memset(&dreq, 0, sizeof(dreq));
			dreq.handle = buf->handle[i];

			if (drmIoctl(fd_drm, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq) < 0)
				LOGERROR("DestroyFB: cannot destroy dumb buffer (%d): %m", errno);
			buf->handle[i] = 0;

		}

		buf->plane[i] = 0;
		buf->size[i] = 0;
		buf->pitch[i] = 0;
		buf->offset[i] = 0;
		buf->obj_index[i] = 0;
	}

	for (int i = 0; i < buf->nb_objects; i++) {
		if (buf->primehandle[i]) {
		// this can happen, when we SetPlayMode 0 while in trickspeed
		// does not show negative effects, but its not nice though -> TODO
			if (drmIoctl(fd_drm, DRM_IOCTL_GEM_CLOSE, &buf->primehandle[i]) < 0)
				LOGERROR("DestroyFB: cannot close handle %d FB %d GEM (%d): %m", buf->primehandle[i], buf->fb_id, errno);
		}
	}

	buf->width = 0;
	buf->height = 0;
	buf->fb_id = 0;
	buf->trickspeed = 0;
	buf->dirty = 0;
	buf->swbuffer = 0;
	buf->num_planes = 0;
}

///
///	Grabbing thread.
///
///	TODO: different threads for video and osd
///
void *cVideoRender::GrabHandlerThread(void *arg)
{
//	cVideoRender *render = (cVideoRender *)arg;

	LOGDEBUG2(L_GRAB, "GrabHandlerThread: thread started");
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

	struct drm_buf *videobuf = grabvideo->buf;
	struct drm_buf *osdbuf = grabosd->buf;

	VideoGrab(grabvideo, videobuf, &grabvideoready, 0);
	VideoGrab(grabosd, osdbuf, &grabosdready, 1);

	if (videobuf) {
		for (int plane = 0; plane < videobuf->num_planes; plane++) {
			if (videobuf->size[plane]) {
				LOGDEBUG2(L_GRAB, "GrabHandlerThread: free videobuf %p (plane %d)", videobuf->plane[plane], plane);
				free(videobuf->plane[plane]);
			}
		}
		free(videobuf);
	}

	if (osdbuf) {
		for (int plane = 0; plane < osdbuf->num_planes; plane++) {
			if (osdbuf->size[plane]) {
				LOGDEBUG2(L_GRAB, "GrabHandlerThread: free osdbuf %p (plane %d)", osdbuf->plane[plane], plane);
				free(osdbuf->plane[plane]);
			}
		}
		free(osdbuf);
	}

	LOGDEBUG2(L_GRAB, "GrabHandlerThread: thread ended");
	pthread_exit((void *)pthread_self());
}

///
///	Clone drm buffer
///
///	@param dst[out]		dst video buffer
///	@param src[in]		src video buffer
///
static void VideoCloneBuf(struct drm_buf **dst, struct drm_buf *src)
{
	struct drm_buf *buf = (struct drm_buf *)malloc(sizeof(struct drm_buf));

	buf->width = src->width;
	buf->height = src->height;
	buf->fb_id = src->fb_id;
	buf->pix_fmt = src->pix_fmt;
	buf->num_planes = src->num_planes;
	buf->trickspeed = src->trickspeed;
	buf->swbuffer = src->swbuffer;
	buf->nb_objects = src->nb_objects;

	for (int object = 0; object < buf->nb_objects; object++) {
		buf->fd_prime[object] = src->fd_prime[object];
	}

	for (int i = 0; i < src->num_planes; i++) {
		buf->size[i] = src->size[i];
		buf->pitch[i] = src->pitch[i];
		buf->handle[i] = src->handle[i];
		buf->offset[i] = src->offset[i];
		buf->obj_index[i] = src->obj_index[i];
	}

	void *src_buffer = NULL;
	void *dst_buffer = NULL;

	// planes aren't mmapped, do it (PRIME)
	if (!src->plane[0]) {
		for (int object = 0; object < buf->nb_objects; object++) {
			// memcpy mmapped data
			dst_buffer = malloc(src->size[object]);
			src_buffer = mmap(NULL, src->size[object], PROT_READ, MAP_PRIVATE, src->fd_prime[object], 0);
			if (src_buffer == MAP_FAILED) {
				LOGERROR("VideoCloneBufB: cannot map buffer (%d): %m", errno);
				return;
			}

			LOGDEBUG2(L_GRAB, "VideoCloneBuf: Copy %p to %p", src_buffer, dst_buffer);
			memcpy(dst_buffer, src_buffer, src->size[object]);
			munmap(src_buffer, src->size[object]);
			for (int plane = 0; plane < buf->num_planes; plane++) {
				if (buf->obj_index[plane] == object) {
					buf->plane[plane] = (uint8_t *)dst_buffer;
					LOGDEBUG2(L_GRAB, "VideoCloneBuf: buf->plane[%d] gets %p (object %d)", plane, dst_buffer, object);
				}
			}
		}
	} else {
		for (int plane = 0; plane < buf->num_planes; plane++) {
			dst_buffer = malloc(buf->size[plane]);
			memcpy(dst_buffer, src->plane[plane], src->size[plane]);
			buf->plane[plane] = (uint8_t *)dst_buffer;
		}
	}

	for (int plane = 0; plane < buf->num_planes; plane++) {
		LOGDEBUG2(L_GRAB, "VideoCloneBuf: Cloned plane %d address %p pitch %d offset %d handle %d size %d",
		       plane, buf->plane[plane], buf->pitch[plane], buf->offset[plane], buf->handle[plane], buf->size[plane]);
	}

	*dst = buf;
}

///
/// Clean DRM
///
void cVideoRender::CleanDisplayThread(void)
{
	AVFrame *frame;
	int i;

	pthread_mutex_lock(&WaitCleanMutex);
	// first wait for FilterThread to be closed
	while (FilterThread) {
		int timeout = 20;
		usleep(1000);
		if (!timeout--) {
			LOGERROR("CleanDisplayThread: Wait for filter thread ending -- TIMEOUT");
			break;
		}
	}

	pthread_mutex_lock(&DisplayQueue);
dequeue:
	if (atomic_read(&FramesFilled)) {
		frame = FramesRb[FramesRead];
		FramesRead = (FramesRead + 1) % VIDEO_SURFACES_MAX;
		atomic_dec(&FramesFilled);
		av_frame_free(&frame);
		goto dequeue;
	}
	pthread_mutex_unlock(&DisplayQueue);

	if (Closing && lastframe->frame) {
		av_frame_free(&lastframe->frame);
		lastframe->trickspeed = 0;
	}

	// Destroy FBs
	for (i = 0; i < RENDERBUFFERS; ++i) {
		if (bufs[i].dirty == 0)
			continue;

		if (Closing || (bufs[i].fb_id != lastframe->buf->fb_id))
			DestroyFB(&bufs[i]);
	}

	buffers = 0;
	enqueue_buffer = 0;

	pthread_cond_signal(&WaitCleanCondition);
	if (Flushing)
		FlushLast = 1;
	Flushing = 0;
	Closing = 0;
	FilterClosing = 0;
	FilterDeintDisabled = ConfigFilterDeintDisabled;
	pthread_mutex_unlock(&WaitCleanMutex);

	LOGDEBUG("CleanDisplayThread: DRM cleaned.");
}

///
///	Commit the frame to the hardware
///
//	retval 2	VIDEO and OSD modesetting and commit was done, need to process outstanding DRM events
//	retval 1	VIDEO only modesetting and commit was done, need to process outstanding DRM events
//	retval 0	OSD only modesetting and commit was done, need to process outstanding DRM events
//	retval -1	no modesetting and commit was done
//	retval -2	something went wrong, no modesetting was done
//
///
int cVideoRender::VideoDrmCommit(struct drm_buf *buf, int skip_video)
{
	int dirty = 0; // 0: no commit, 1: osd only, 2: video only, 3: both
	AVFrame *frame = NULL;

	uint64_t DispWidth;
	uint64_t DispHeight;
	uint64_t DispX;
	uint64_t DispY;
	uint64_t PicWidth;
	uint64_t PicHeight;

	drmModeAtomicReqPtr ModeReq;
	uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
	if (!(ModeReq = drmModeAtomicAlloc())) {
		LOGERROR("Frame2Display: cannot allocate atomic request (%d): %m", errno);
		return -2;
	}

	if (skip_video)
		goto skip_video;

	if (buf)
		frame = buf->frame;

	// handle the video plane
	// Get video size and position and set crtc rect
	DispWidth = mode.hdisplay;
	DispHeight = mode.vdisplay;
	DispX = 0;
	DispY = 0;

	if (video.is_scaled) {
		DispWidth = (uint64_t)video.width;
		DispHeight = (uint64_t)video.height;
		DispX = (uint64_t)video.x;
		DispY = (uint64_t)video.y;
	}

	PicWidth = DispWidth;
	PicHeight = DispHeight;

	// resize frame to fit into video area/ screen and keep the aspect ratio
	if (frame) {
		// use frame->sample_aspect_ratio of 1.0f if undefined (0.0f), otherwise we have division by 0
		double frame_sar = av_q2d(frame->sample_aspect_ratio) ? av_q2d(frame->sample_aspect_ratio) : 1.0f;

		// frame b*h < display b*h, e.g. fit a 4:3 frame into 16:9 display or area
		if (1000 * DispWidth / DispHeight > 1000 * frame->width / frame->height * frame_sar) {
			PicWidth = DispHeight * frame->width / frame->height * frame_sar;
			if (PicWidth <= 0 || PicWidth > DispWidth) {
				PicWidth = DispWidth;
			}
		// frame b*h >= display b*h, e.g. fit a 16:9 frame into 4:3 display or area
		} else {
			PicHeight = DispWidth * frame->height / frame->width / frame_sar;
			if (PicHeight <= 0 || PicHeight > DispHeight) {
				PicHeight = DispHeight;
			}
		}
	}

	planes[VIDEO_PLANE]->properties.crtc_id = crtc_id;
	planes[VIDEO_PLANE]->properties.fb_id = buf->fb_id;
	planes[VIDEO_PLANE]->properties.crtc_x = DispX + (DispWidth - PicWidth) / 2;
	planes[VIDEO_PLANE]->properties.crtc_y = DispY + (DispHeight - PicHeight) / 2;
	planes[VIDEO_PLANE]->properties.crtc_w = PicWidth;
	planes[VIDEO_PLANE]->properties.crtc_h = PicHeight;
	planes[VIDEO_PLANE]->properties.src_x = 0;
	planes[VIDEO_PLANE]->properties.src_y = 0;
	planes[VIDEO_PLANE]->properties.src_w = buf->width;
	planes[VIDEO_PLANE]->properties.src_h = buf->height;

	SetPlane(ModeReq, planes[VIDEO_PLANE]);
	dirty += 2;

	if (startgrab) {
		LOGDEBUG2(L_GRAB, "Frame2Display: Trigger video grab arrived");
		VideoCloneBuf(&grabvideo->buf, buf);
		// should be the size on screen
		grabvideo->x = DispX + (DispWidth - PicWidth) / 2;
		grabvideo->y = DispY + (DispHeight - PicHeight) / 2;
		grabvideo->width = PicWidth;
		grabvideo->height = PicHeight;
	}

skip_video:
	// handle the osd plane
	// We had draw activity on the osd buffer
	if (buf_osd && buf_osd->dirty) {
		if (use_zpos) {
			planes[VIDEO_PLANE]->properties.zpos = OsdShown ? zpos_primary : zpos_overlay;
			planes[OSD_PLANE]->properties.zpos = OsdShown ? zpos_overlay : zpos_primary;
			SetPlaneZpos(ModeReq, planes[VIDEO_PLANE]);
			SetPlaneZpos(ModeReq, planes[OSD_PLANE]);

			LOGDEBUG2(L_DRM, "Frame2Display: SetPlaneZpos: video->plane_id %d -> zpos %" PRIu64 ", osd->plane_id %d -> zpos %" PRIu64 "",
				planes[VIDEO_PLANE]->plane_id, planes[VIDEO_PLANE]->properties.zpos,
				planes[OSD_PLANE]->plane_id, planes[OSD_PLANE]->properties.zpos);
		}

		planes[OSD_PLANE]->properties.crtc_id = crtc_id;
		planes[OSD_PLANE]->properties.fb_id = buf_osd->fb_id;
		planes[OSD_PLANE]->properties.crtc_x = 0;
		planes[OSD_PLANE]->properties.crtc_y = 0;
		planes[OSD_PLANE]->properties.crtc_w = OsdShown ? buf_osd->width : 0;
		planes[OSD_PLANE]->properties.crtc_h = OsdShown ? buf_osd->height : 0;
		planes[OSD_PLANE]->properties.src_x = 0;
		planes[OSD_PLANE]->properties.src_y = 0;
		planes[OSD_PLANE]->properties.src_w = OsdShown ? buf_osd->width : 0;
		planes[OSD_PLANE]->properties.src_h = OsdShown ? buf_osd->height : 0;

		SetPlane(ModeReq, planes[OSD_PLANE]);
		dirty += 1;
		LOGDEBUG2(L_DRM, "Frame2Display: SetPlane OSD %d (fb = %" PRIu64 ")", OsdShown, planes[OSD_PLANE]->properties.fb_id);
		buf_osd->dirty = 0;
	}

	if (startgrab) {
		if (buf_osd && OsdShown) {
			LOGDEBUG2(L_GRAB, "Frame2Display: Trigger osd grab arrived");
			VideoCloneBuf(&grabosd->buf, buf_osd);
			// should be the size on screen
			grabosd->x = 0;
			grabosd->y = 0;
			grabosd->width = buf_osd->width;
			grabosd->height = buf_osd->height;
		}
// TODO
//		pthread_create(&GrabbingThread, NULL, GrabHandlerThread, render);
		pthread_create(&GrabbingThread, NULL, GrabHandlerThread, this);
		pthread_setname_np(GrabbingThread, "grabbing thread");
		startgrab = 0;
	}

	// return without an atomic commit (no video frame and osd activity)
	if (!dirty) {
		drmModeAtomicFree(ModeReq);
		return -1;
	}

	// do the atomic commit
	if (drmModeAtomicCommit(fd_drm, ModeReq, flags, NULL) != 0) {
		DumpPlaneProperties(planes[OSD_PLANE]);
		if (dirty > 1)
			DumpPlaneProperties(planes[VIDEO_PLANE]);

		drmModeAtomicFree(ModeReq);
		LOGERROR("Frame2Display: page flip failed (%d): %m", errno);
		return -2;
	}

	drmModeAtomicFree(ModeReq);

	return dirty - 1;
}

///
///	Sync the frames
///
//	retval 1	close or flush requested, skip video or show black frame
//	retval 0	nothing to sync or paused
//	retval -1	drop frame
//
///
int cVideoRender::VideoSync(AVFrame *frame, int *skip_video, struct drm_buf **buf)
{
	int64_t audio_pts;
	int64_t video_pts;

	video_pts = frame->pts * 1000 * av_q2d(*timebase);

	if(!StartCounter && !Closing) {
		LOGDEBUG("VideoSync: start PTS %s", Timestamp2String(video_pts));
		Audio->AudioSkipInTrickSpeed(frame->pts, 0);
avready:
		if (Audio->AudioVideoReady(video_pts)) {
			usleep(10000);

			// check for close/flush request or pause
			if (Closing) {
				LOGDEBUG2(L_DRM, "Frame2Display: closing while sync, set a black FB");
				*buf = &buf_black;
				return 1;
			} else if (Flushing) {
				LOGDEBUG2(L_DRM, "Frame2Display: flushing while sync, skip video");
				*skip_video = 1;
				return 1;
			} else if (VideoIsPaused()) {
				return 0;
			}

			goto avready;
		}
	}

audioclock:
	// check for close/flush request or pause
	if (Closing) {
		LOGDEBUG2(L_DRM, "Frame2Display: closing while sync, set a black FB");
		*buf = &buf_black;
		return 1;
	} else if (Flushing) {
		LOGDEBUG2(L_DRM, "Frame2Display: flushing while sync, skip video");
		*skip_video = 1;
		return 1;
	} else if (VideoIsPaused()) {
		return 0;
	}

	audio_pts = Audio->AudioGetClock();

	if (audio_pts == (int64_t)AV_NOPTS_VALUE) {
		usleep(20000);
		goto audioclock;
	}

	int diff = video_pts - audio_pts - Device->GetVideoAudioDelay();

	if (abs(diff) > 5000) {	// more than 5s
		LOGDEBUG2(L_AV_SYNC, "More then 5s Pkts %d deint %d Frames %d AudioUsedBytes %d audio %s video %s Delay %dms diff %dms",
			Device->VideoStream->GetPackets(), atomic_read(&FramesDeintFilled),
			atomic_read(&FramesFilled), Audio->AudioUsedBytes(), Timestamp2String(audio_pts),
			Timestamp2String(video_pts), Device->GetVideoAudioDelay(), diff);

	}

	if (diff < -5 && !(abs(diff) > 5000)) {	// video is more than 5ms behind audio, drop video frame
		LOGDEBUG2(L_AV_SYNC, "FrameDropped (drop %d, dup %d) Pkts %d deint %d Frames %d AudioUsedBytes %d audio %s video %s Delay %dms diff %dms",
			FramesDropped, FramesDuped,
			Device->VideoStream->GetPackets(), atomic_read(&FramesDeintFilled),
			atomic_read(&FramesFilled), Audio->AudioUsedBytes(), Timestamp2String(audio_pts),
			Timestamp2String(video_pts), Device->GetVideoAudioDelay(), diff);

		if (!StartCounter)
			StartCounter++;

		FramesDropped++;
		return -1;
	}

	if (diff > 35 && !(abs(diff) > 5000)) {	// audio is more than 35ms behind video, duplicate video frame
		LOGDEBUG2(L_AV_SYNC, "FrameDuped (drop %d, dup %d) Pkts %d deint %d Frames %d AudioUsedBytes %d audio %s video %s Delay %dms diff %dms",
			FramesDropped, FramesDuped,
			Device->VideoStream->GetPackets(), atomic_read(&FramesDeintFilled),
			atomic_read(&FramesFilled), Audio->AudioUsedBytes(), Timestamp2String(audio_pts),
			Timestamp2String(video_pts), Device->GetVideoAudioDelay(), diff);

		FramesDuped++;
		usleep(20000);
		goto audioclock;
	}

	return 0;
}

///
///	Get next video frame
///
//	retval 0	received frame with PTS value
//	retval 1	received frame without PTS value
//
///
int cVideoRender::VideoGetFrame(AVFrame **frame)
{
	AVFrame *pframe = NULL;

	pframe = FramesRb[FramesRead];
	FramesRead = (FramesRead + 1) % VIDEO_SURFACES_MAX;
	atomic_dec(&FramesFilled);

	*frame = pframe;

	if (pframe->pts == AV_NOPTS_VALUE)
		return 1;

	return 0;
}

///
///	Get suitable framebuffer for frame
///
//	retval 0	got a buffer
//	retval 1	sth went wrong
//
///
int cVideoRender::VideoGetBuffer(AVFrame *frame, struct drm_buf **buf)
{
	struct drm_buf *pbuf = NULL;
	int i;

	AVDRMFrameDescriptor *primedata = (AVDRMFrameDescriptor *)frame->data[0];
	FrameData *fd = (FrameData *)frame->opaque_ref->data;

	// search for a made fd / FB combination
	for (i = 0; i < RENDERBUFFERS; i++) {
		if (FlushLast && !bufs[i].swbuffer)
			break;

		if (bufs[i].trickspeed && !bufs[i].swbuffer)
			break;

		if (bufs[i].dirty == 0)
			continue;

		if (bufs[i].fd_prime[0] == primedata->objects[0].fd) {
			pbuf = &bufs[i];
			break;
		}
	}

	// search for a "free" buffer
	if (pbuf == 0) {
		for (i = 0; i < RENDERBUFFERS; i++) {
			if (bufs[i].dirty == 0)
				break;
		}
		if (bufs[i].dirty) {
			LOGDEBUG("VideoGetBuffer: SHOULD NOT HAPPEN! no free buffer available!");
			return 1;
		}

		pbuf = &bufs[i];;

		pbuf->width = (uint32_t)frame->width;
		pbuf->height = (uint32_t)frame->height;

		if (SetupFB(pbuf, primedata))
			return 1;

		bufs[i].swbuffer = 0;
	}

	if (pbuf == 0) {
		LOGDEBUG("VideoGetBuffer: SHOULD NOT HAPPEN! failed, no buffer found!");
		return 1;
	}

	pbuf->trickspeed = fd->flags & FRAME_FLAG_TRICKSPEED;

	*buf = pbuf;
	return 0;
}

///
///	Check if we should close or flush
///
///	retval 0	nothing to do
///	retval 1	needs closing/flushing: set black screen if closing, skip video if flushing
///
///
int cVideoRender::check_closing(int *skip_video, struct drm_buf **buf) {
	if (Closing) {
		LOGDEBUG2(L_DRM, "Frame2Display: closing, set a black FB");
		*buf = &buf_black;
		return 1;
	}

	if (Flushing) {
		LOGDEBUG2(L_DRM, "Frame2Display: flushing, skip video");
		*skip_video = 1;
		return 1;
	}

	return 0;
}

///
///	Check if video is paused
///
///	retval 0	not paused
///	retval 1	paused, skip the video
///
///
int cVideoRender::check_pausing(int *skip_video) {
	if (!VideoIsPaused())
		return 0;

	*skip_video = 1;
	usleep(10000);
//	LOGDEBUG2(L_DRM, "Frame2Display: paused, skip video");
	return 1;
}

// ab hier weiter
// threads fehlen auch noch

///
///	Check if video is paused and wait for video to come up with audio before pausing
///
///	retval 0	either not paused or waiting for video to sync with audio
///	retval 1	paused, skip the video
///
///
int cVideoRender::check_pausing_with_sync(int *skip_video) {
	if (!VideoIsPaused())
		return 0;

	int64_t audio_pts = Audio->AudioGetClock();
	int64_t video_pts = VideoGetClock() * 1000 * av_q2d(*timebase);
	if (video_pts == AV_NOPTS_VALUE || audio_pts == AV_NOPTS_VALUE) {
		usleep(10000);
		*skip_video = 1;
		return 1;
	}

	int diff = video_pts - audio_pts - Device->GetVideoAudioDelay();
	// audio is behind video, so pause - otherwise let video come up with audio before "real" pause
	if (diff > 0) {
		usleep(10000);
		*skip_video = 1;
		return 1;
	}

	return 0;
}

///
///	Draw a video frame.
///
//	retval 0	modesetting and commit was done, need to process outstanding DRM events
//	retval 1	no new frames or OSD, no modesetting was done, don't process outstanding DRM events
//
///
int cVideoRender::Frame2Display(void)
{
	struct drm_buf *buf = NULL;
	AVFrame *frame = NULL;
	int skip_video = 0;
	int timeout; // ms
	int ret;
	FrameData *fd;

	// early skips
	if (check_closing(&skip_video, &buf))
		goto page_flip;

dequeue:
	timeout = 25; // ms
	// wait for a frame in the ringbuffer
	while (!atomic_read(&FramesFilled)) {
		if (check_closing(&skip_video, &buf) ||
		    check_pausing(&skip_video))
			goto page_flip;

		// wait max. 25ms in case we have an osd
		if (buf_osd && buf_osd->dirty && !timeout--) {
			skip_video = 1;
			LOGDEBUG2(L_DRM, "Frame2Display: no video but osd, skip video");
			goto page_flip;
		}

		usleep(1000);
	}

	if (check_pausing_with_sync(&skip_video))
		goto page_flip;

	// advance frame
	if (VideoGetFrame(&frame)) {
		FrameData *fd = (FrameData *)frame->opaque_ref->data;
		if (fd->flags & FRAME_FLAG_STILLPICTURE) {
			LOGDEBUG2(L_STILL, "Frame2Display: Stillpicture has AV_NOPTS_VALUE, skip sync ...");
			goto skip_sync;
		} else {
			// TODO: fast/soft sync
			LOGDEBUG2(L_DRM, "Frame2Display: no AV_NOPTS_VALUE, use next frame ...");
			av_frame_free(&frame);
			return 1;
		}
	}

	fd = (FrameData *)frame->opaque_ref->data;

	// skip old audio in trickspeed
	if (fd->flags & FRAME_FLAG_TRICKSPEED) {
		Audio->AudioSkipInTrickSpeed(frame->pts, 0);
		goto skip_sync;
	}

	// skip old audio after trickspeed
	if (lastframe->frame && lastframe->trickspeed) {
		Audio->AudioSkipInTrickSpeed(frame->pts, 1);
		goto skip_sync;
	}

	// sync audio/video
	ret = VideoSync(frame, &skip_video, &buf);

	if (ret < 0) { // drop frame (dup is handled within VideoSync())
		av_frame_free(&frame);
		goto dequeue;
	} else if (ret) { // close or flush (black buffer or skip video)
		av_frame_free(&frame);
		goto page_flip;
	}

	StartCounter++;

skip_sync:
	// get suitable framebuffer
	if (VideoGetBuffer(frame, &buf)) {
		av_frame_free(&frame);
		return 1;
	}

	buf->frame = frame;

page_flip:
	// no modesetting was done
	if (VideoDrmCommit(buf, skip_video) < 0) {
		if (frame)
			av_frame_free(&frame);
		return 1;
	}

	// only osd was set
	if (skip_video)
		return 0;

	// now, that we had a successful commit, set the STC if we have a frame
	if (frame)
		VideoSetClock(frame->pts);

	if (frame)
		LOGDEBUG2(L_PACKET, "Frame2Display:                 PTS %s", Timestamp2String(frame->pts / 90));

	// new video frame was sent, rotate the frames
	if (lastframe->frame) {
		// if the lastframe was a trickframe or a flush is forced, destroy the FB
		if (FlushLast || lastframe->trickspeed) {
			DestroyFB(lastframe->buf);
			lastframe->trickspeed = 0;
			FlushLast = 0;
		}
		av_frame_free(&lastframe->frame);
	}

	if (buf && buf->fb_id != buf_black.fb_id) {
		lastframe->frame = buf->frame;
		lastframe->buf = buf;
		lastframe->trickspeed = buf->trickspeed;
	}

	return 0;
}

int cVideoRender::DrmHandleEvent(void)
{
    return drmHandleEvent(fd_drm, &ev);
}

//----------------------------------------------------------------------------
//	OSD
//----------------------------------------------------------------------------

///
///	Clear the OSD.
///
///
void cVideoRender::VideoOsdClear(void)
{
#ifdef USE_GLES
	if (DisableOglOsd) {
		memset((void *)buf_osd->plane[0], 0,
			(size_t)(buf_osd->pitch[0] * buf_osd->height));
	} else {
		struct drm_buf *buf;

		EGL_CHECK(eglSwapBuffers(eglDisplay, eglSurface));
		next_bo = gbm_surface_lock_front_buffer(gbm_surface);
		assert(next_bo);

		buf = drm_get_buf_from_bo(next_bo);
		if (!buf) {
			LOGERROR("Failed to get GL buffer");
			return;
		}

		buf_osd = buf;

		// release old buffer for writing again
		if (bo)
			gbm_surface_release_buffer(gbm_surface, bo);

		// rotate bos and create and keep bo as old_bo to make it free'able
		old_bo = bo;
		bo = next_bo;

		LOGDEBUG2(L_OPENGL, "VideoOsdClear(GL): eglSwapBuffers eglDisplay %p eglSurface %p (%i x %i, %i)", eglDisplay, eglSurface, buf->width, buf->height, buf->pitch[0]);
	}
#else
	memset((void *)buf_osd->plane[0], 0,
		(size_t)(buf_osd->pitch[0] * buf_osd->height));
#endif

	buf_osd->dirty = 1;
	OsdShown = 0;
}

#define MIN(a, b) ((a) < (b) ? (a) : (b))

///
///	Draw an OSD ARGB image.
///
///	@param xi	x-coordinate in argb image
///	@param yi	y-coordinate in argb image
///	@paran height	height in pixel in argb image
///	@paran width	width in pixel in argb image
///	@param pitch	pitch of argb image
///	@param argb	32bit ARGB image data
///	@param x	x-coordinate on screen of argb image
///	@param y	y-coordinate on screen of argb image
///
void cVideoRender::VideoOsdDrawARGB(int xi, int yi,
		int width, int height, int pitch,
		const uint8_t * argb, int x, int y)
{
#ifdef USE_GLES
	if (DisableOglOsd) {
		LOGDEBUG2(L_OSD, "VideoOsdDrawARGB width %d height %d pitch %d argb %p x %d y %d pitch buf %d xi %d yi %d",
		       width, height, pitch, argb, x, y, buf_osd->pitch[0], xi, yi);
		for (int i = 0; i < height; ++i) {
			memcpy(buf_osd->plane[0] + x * 4 + (i + y) * buf_osd->pitch[0],
				argb + i * pitch, MIN((size_t)pitch, buf_osd->pitch[0]));
		}
	} else {
		struct drm_buf *buf;

		EGL_CHECK(eglSwapBuffers(eglDisplay, eglSurface));
		next_bo = gbm_surface_lock_front_buffer(gbm_surface);
		assert(next_bo);

		buf = drm_get_buf_from_bo(next_bo);
		if (!buf) {
			LOGERROR("Failed to get GL buffer");
			return;
		}

		buf_osd = buf;

		// release old buffer for writing again
		if (bo)
			gbm_surface_release_buffer(gbm_surface, bo);

		// rotate bos and create and keep bo as old_bo to make it free'able
		old_bo = bo;
		bo = next_bo;

		LOGDEBUG2(L_OPENGL, "VideoOsdDrawARGB(GL): eglSwapBuffers eglDisplay %p eglSurface %p (%i x %i, %i)", eglDisplay, eglSurface, buf->width, buf->height, buf->pitch[0]);
	}
#else
	for (int i = 0; i < height; ++i) {
		memcpy(buf_osd->plane[0] + x * 4 + (i + y) * buf_osd->pitch[0],
			argb + i * pitch, (size_t)pitch);
	}
#endif
	buf_osd->dirty = 1;
	OsdShown = 1;
}

//----------------------------------------------------------------------------
//	Thread
//----------------------------------------------------------------------------

///
///	Exit and cleanup video threads.
///
void cVideoRender::VideoThreadExit(void)
{
	void *retval;

	LOGDEBUG("VideoThreadExit: cancel decoding and display thread");

	if (DecodeThread->Active()) {
		LOGDEBUG("VideoThreadExit: cancel decode thread");
		DecodeThread->Stop();
	}

	if (DisplayThread->Active()) {
		LOGDEBUG("VideoThreadExit: cancel display thread");
		DisplayThread->Stop();
	}
}

///
///	Video display wakeup.
///
///	New video arrived, wakeup video thread.
///
void cVideoRender::VideoThreadWakeup(int decoder, int display)
{
	LOGDEBUG("VideoThreadWakeup: VideoThreadWakeup");

	if (decoder && !DecodeThread->Active()) {
		LOGDEBUG("DisplayThreadWakeup: wakeup decoding thread");
		DecodeThread->Start();
	}

	if (display && !DisplayThread->Active()) {
		LOGDEBUG("VideoThreadWakeup: wakeup display thread");
		DisplayThread->Start();
	}
}

//----------------------------------------------------------------------------
//	Video API
//----------------------------------------------------------------------------

///
///	Allocate new video hw render.
///
///	@param stream	video stream
///
///	@returns a new initialized video hardware render.
///

cVideoRender::cVideoRender(cSoftHdDevice *device)
{
	Device = device;
	Audio = Device->Audio;
	DecodeThread = new cDecodingThread(Device);

	atomic_set(&FramesFilled, 0);
	atomic_set(&FramesDeintFilled, 0);
	Closing = 0;
	Flushing = 0;
	FlushLast = 0;
	FilterClosing = 0;
	FilterDeintDisabled = ConfigFilterDeintDisabled;
	enqueue_buffer = 0;
	lastframe = (struct lastFrame *)calloc(1, sizeof(struct lastFrame));
	VideoResume();
}

///
///	Destroy a video render.
///
///	@param render	video render
///
cVideoRender::~cVideoRender(void)
{
	delete DecodeThread;
//	if (!pthread_equal(pthread_self(), DecodeThread)) {
//		LOGDEBUG("video: should only be called from inside the thread");
//	}
	free(lastframe);
}

void cVideoRender::EnqueueFB(AVFrame *inframe)
{
	// inframe->format is always NV12!
	struct drm_buf *buf = 0;

	AVDRMFrameDescriptor * primedata;
	AVFrame *frame;
	int i;
	FrameData *ifd = (FrameData *)inframe->opaque_ref->data;

	if (ifd->flags & FRAME_FLAG_TRICKSPEED) {	// if we have trickspeed, always use a free buffer, because its destroyed in Frame2Display after rendering
		for (i = 0; i < RENDERBUFFERS; i++) {
			if (bufs[i].dirty == 0)
				break;
		}
		if (bufs[i].dirty)
			LOGFATAL("EnqueueFB: SHOULD NOT HAPPEN! no free buffer available!");

		buf = &bufs[i];
		buf->width = (uint32_t)inframe->width;
		buf->height = (uint32_t)inframe->height;
		buf->pix_fmt = DRM_FORMAT_NV12;

		if (SetupFB(buf, NULL)) {
			LOGERROR("EnqueueFB: SetupFB FB %i x %i failed",
				buf->width, buf->height);
		}
		if (drmPrimeHandleToFD(fd_drm, buf->handle[0], DRM_CLOEXEC | DRM_RDWR, &buf->fd_prime[0]))
			LOGERROR("EnqueueFB: Failed to retrieve the Prime FD (%d): %m", errno);
	} else {
		// create some buffers up to VIDEO_SURFACES_MAX + 2
get_buffer:
		buf = &bufs[enqueue_buffer];
		if (buffers < VIDEO_SURFACES_MAX + 2) {
			if (buf->dirty) {
				// skip the buffer, because it is either referenced by lastframe or is already setup
				// this should be safe, because we only have 1 lastframe, which should be destroyed as soon as
				// a new buffer arrives in Frame2Display
				// after that destroy, we should be able to setup 0, 1, 2, ..., VIDEO_SURFACES_MAX + 2 framebuffers
				enqueue_buffer = (enqueue_buffer + 1) % (VIDEO_SURFACES_MAX + 2);

				goto get_buffer;
			}

			buf->width = (uint32_t)inframe->width;
			buf->height = (uint32_t)inframe->height;
			buf->pix_fmt = DRM_FORMAT_NV12;

			if (SetupFB(buf, NULL)) {
				LOGERROR("EnqueueFB: SetupFB FB %i x %i failed",
					buf->width, buf->height);
			}

			buffers++;

			if (drmPrimeHandleToFD(fd_drm, buf->handle[0], DRM_CLOEXEC | DRM_RDWR, &buf->fd_prime[0]))
				LOGERROR("EnqueueFB: Failed to retrieve the Prime FD (%d): %m", errno);
		}
	}

	// mark this buffer as a software decoded buffer
	buf->swbuffer = 1;

	for (i = 0; i < inframe->height; ++i) {
		memcpy(buf->plane[0] + i * buf->pitch[0],
			inframe->data[0] + i * inframe->linesize[0], inframe->linesize[0]);
	}
	for (i = 0; i < inframe->height / 2; ++i) {
		memcpy(buf->plane[1] + i * buf->pitch[1],
			inframe->data[1] + i * inframe->linesize[1], inframe->linesize[1]);
	}

	frame = av_frame_alloc();
	frame->pts = inframe->pts;
	frame->width = inframe->width;
	frame->height = inframe->height;
	frame->format = AV_PIX_FMT_DRM_PRIME;
	frame->sample_aspect_ratio.num = inframe->sample_aspect_ratio.num;
	frame->sample_aspect_ratio.den = inframe->sample_aspect_ratio.den;

	FrameData *fd;
	if (!frame->opaque_ref) {
		frame->opaque_ref = av_buffer_allocz(sizeof(*fd));
		if (!frame->opaque_ref) {
			LOGFATAL("EnqueueFB: cannot allocate private frame data");
		}
	}
	fd = (FrameData *)frame->opaque_ref->data;
	fd->flags = ifd->flags;

	primedata = (AVDRMFrameDescriptor *)av_mallocz(sizeof(AVDRMFrameDescriptor));
	primedata->objects[0].fd = buf->fd_prime[0];
	frame->data[0] = (uint8_t *)primedata;
	frame->buf[0] = av_buffer_create((uint8_t *)primedata, sizeof(*primedata),
				ReleaseFrame, NULL, AV_BUFFER_FLAG_READONLY);

	if (Closing || Flushing) {
		av_frame_free(&inframe);
		av_frame_free(&frame);
		return;
	}

	pthread_mutex_lock(&DisplayQueue);
	FramesRb[FramesWrite] = frame;
	FramesWrite = (FramesWrite + 1) % VIDEO_SURFACES_MAX;
	atomic_inc(&FramesFilled);
	pthread_mutex_unlock(&DisplayQueue);

	if (!(ifd->flags & FRAME_FLAG_TRICKSPEED))
		enqueue_buffer = (enqueue_buffer + 1) % (VIDEO_SURFACES_MAX + 2);

	av_frame_free(&inframe);
}

//TODO
///
//	Filter thread.
//
void *cVideoRender::FilterHandlerThread(void * arg)
{
//	cVideoRender *render = (cVideoRender *)arg;

	AVFrame *frame = 0;
	int ret = 0;

	LOGDEBUG("video: video filter thread started");

	while (1) {
		while (!atomic_read(&FramesDeintFilled) && !FilterClosing) {
			usleep(10000);
		}
getinframe:
		if (atomic_read(&FramesDeintFilled)) {
			frame = FramesDeintRb[FramesDeintRead];
			FramesDeintRead = (FramesDeintRead + 1) % VIDEO_SURFACES_MAX;
			atomic_dec(&FramesDeintFilled);
			int interlaced;
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
			interlaced = frame->interlaced_frame;
#else
			interlaced = frame->flags & AV_FRAME_FLAG_INTERLACED;
#endif
			if (interlaced) {
				Filter_Frames += 2;
			} else {
				Filter_Frames++;
			}
		}
		if (FilterClosing) {
			if (frame) {
				av_frame_free(&frame);
			}
			if (atomic_read(&FramesDeintFilled)) {
				goto getinframe;
			}
			frame = NULL;
		}

		if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
			LOGWARNING("FilterHandlerThread: can't add_frame.");
		} else {
			av_frame_free(&frame);
		}

		while (1) {
			AVFrame *filt_frame = av_frame_alloc();
			ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);

			if (ret == AVERROR(EAGAIN)) {
				av_frame_free(&filt_frame);
				break;
			}
			if (ret == AVERROR_EOF) {
				av_frame_free(&filt_frame);
				goto closing;
			}
			if (ret < 0) {
				LOGERROR("FilterHandlerThread: can't get filtered frame: %s",
					av_err2str(ret));
				av_frame_free(&filt_frame);
				break;
			}
fillframe:
			if (FilterClosing) {
				av_frame_free(&filt_frame);
				break;
			}

			FrameData *fd;
			if (!filt_frame->opaque_ref) {
				filt_frame->opaque_ref = av_buffer_allocz(sizeof(*fd));
				if (!filt_frame->opaque_ref)
					LOGFATAL("FilterHandlerThread: cannot allocate private frame data");
			}
			fd = (FrameData *)filt_frame->opaque_ref->data;
			// set trickspeed flag of the filtered frame (scale filter and AV_PIX_FMT_YUV420P)
			if (Filter_Trick)
				fd->flags |= FRAME_FLAG_TRICKSPEED;
			if (Filter_Still) {
				fd->flags |= FRAME_FLAG_STILLPICTURE;
				filt_frame->pts = AV_NOPTS_VALUE;
			}

			pthread_mutex_lock(&DisplayQueue);
			if (atomic_read(&FramesFilled) < VIDEO_SURFACES_MAX) {
				if (filt_frame->format == AV_PIX_FMT_NV12) {
					// scale filter or sw deinterlacer, no prime data, always returns NV12
					// -> go through EnqueueFB
					if (Filter_Bug)
						filt_frame->pts = filt_frame->pts / 2;	// ffmpeg bug
					Filter_Frames--;
					pthread_mutex_unlock(&DisplayQueue);
					EnqueueFB(filt_frame);
				} else {
					// hw deinterlacers, we received prime data
					// -> put the frame into render Rb
					FramesRb[FramesWrite] = filt_frame;
					FramesWrite = (FramesWrite + 1) % VIDEO_SURFACES_MAX;
					atomic_inc(&FramesFilled);
					pthread_mutex_unlock(&DisplayQueue);
					Filter_Frames--;
				}
			} else {
				pthread_mutex_unlock(&DisplayQueue);
				usleep(5000);
				goto fillframe;
			}
		}
	}

closing:
	avfilter_graph_free(&filter_graph);
	Filter_Frames = 0;
	Filter_Trick = 0;
	Filter_Still = 0;
	pthread_cleanup_push(ThreadExitHandler, NULL);
	pthread_cleanup_pop(1);
	LOGDEBUG("video: video filter thread stopped");
	pthread_exit((void *)pthread_self());
}

///
//	Filter init.
//
//	@retval 0	filter initialised
//	@retval	-1	filter initialise failed
//
int cVideoRender::VideoFilterInit(const AVCodecContext * video_ctx,
		AVFrame * frame)
{
	int ret;
	char args[512];
	const char *filter_descr = NULL;
	filter_graph = avfilter_graph_alloc();
	if (!filter_graph) {
		LOGERROR("VideoFilterInit: Cannot alloc filter graph");
		return -1;
	}

	const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
	const AVFilter *buffersink = avfilter_get_by_name("buffersink");
	Filter_Bug = 0;

	int interlaced;
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
	interlaced = frame->interlaced_frame;
#else
	interlaced = frame->flags & AV_FRAME_FLAG_INTERLACED;
#endif

	if (video_ctx->framerate.num > 0) {
		if (video_ctx->framerate.num / video_ctx->framerate.den > 30)
			interlaced = 0;
		else
			interlaced = 1;
	}

	if (video_ctx->codec_id == AV_CODEC_ID_HEVC)
		interlaced = 0;

	if (FilterDeintDisabled) {
		if (interlaced)
			LOGDEBUG2(L_CODEC, "VideoFilterInit: Deinterlacer wanted, but disabled in setup!");
		interlaced = 0;
	}

	FrameData *fd;
	if (!frame->opaque_ref) {
		frame->opaque_ref = av_buffer_allocz(sizeof(*fd));
		if (!frame->opaque_ref)
			LOGFATAL("FilterHandlreThread: cannot allocate private frame data");
	}
	fd = (FrameData *)frame->opaque_ref->data;

	// interlaced and non-trickspeed AV_PIX_FMT_DRM_PRIME (hardware decoded) -> hardware deinterlacer
	// interlaced and non-trickspeed AV_PIX_FMT_YUV420P (software decoded) -> software deinterlacer
	// progressive and trickspeed AV_PIX_FMT_YUV420P (software decoded) -> scale filter (for NV12 output)
	// progressive and trickspeed AV_PIX_FMT_DRM_PRIME (hardware decoded) doesn't get to the FilterHandlerThread
	Filter_Trick = 0;
	Filter_Still = 0;
	if (interlaced && !(fd->flags & FRAME_FLAG_TRICKSPEED || fd->flags & FRAME_FLAG_STILLPICTURE)) {
		if (frame->format == AV_PIX_FMT_DRM_PRIME) {
			filter_descr = "deinterlace_v4l2m2m";
		} else if (frame->format == AV_PIX_FMT_YUV420P) {
			filter_descr = "bwdif=1:-1:0";
			Filter_Bug = 1;
		}
	} else if (frame->format == AV_PIX_FMT_YUV420P) {
		filter_descr = "scale";
		if (fd->flags & FRAME_FLAG_TRICKSPEED)
			Filter_Trick = 1;
		if (fd->flags & FRAME_FLAG_STILLPICTURE)
			Filter_Still = 1;
	}
#if LIBAVFILTER_VERSION_INT < AV_VERSION_INT(7,16,100)
	avfilter_register_all();
#endif

	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		video_ctx->width, video_ctx->height, frame->format,
		video_ctx->pkt_timebase.num ? video_ctx->pkt_timebase.num : 1,
		video_ctx->pkt_timebase.num ? video_ctx->pkt_timebase.den : 1,
		video_ctx->sample_aspect_ratio.num != 0 ? video_ctx->sample_aspect_ratio.num : 1,
		video_ctx->sample_aspect_ratio.num != 0 ? video_ctx->sample_aspect_ratio.den : 1);

	LOGDEBUG2(L_CODEC, "VideoFilterInit: filter=\"%s\" args=\"%s\"",
		filter_descr, args);

	ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
		args, NULL, filter_graph);
	if (ret < 0) {
		LOGERROR("VideoFilterInit: Cannot create buffer source (%d)", ret);
		avfilter_graph_free(&filter_graph);
		return -1;
	}

	AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
	memset(par, 0, sizeof(*par));
	par->format = AV_PIX_FMT_NONE;
	par->hw_frames_ctx = frame->hw_frames_ctx;
	ret = av_buffersrc_parameters_set(buffersrc_ctx, par);
	if (ret < 0) {
		LOGERROR("VideoFilterInit: Cannot av_buffersrc_parameters_set (%d)", ret);
		av_free(par);
		avfilter_graph_free(&filter_graph);
		return -1;
	}

	av_free(par);

	ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
		NULL, NULL, filter_graph);
	if (ret < 0) {
		LOGERROR("VideoFilterInit: Cannot create buffer sink (%d)", ret);
		avfilter_graph_free(&filter_graph);
		return -1;
	}

	if (frame->format != AV_PIX_FMT_DRM_PRIME) {
		enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_NV12, AV_PIX_FMT_NONE };
		ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
			AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
		if (ret < 0) {
			LOGERROR("VideoFilterInit: Cannot set output pixel format (%d)", ret);
			avfilter_graph_free(&filter_graph);
			return -1;
		}
	}

	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs  = avfilter_inout_alloc();

	outputs->name       = av_strdup("in");
	outputs->filter_ctx = buffersrc_ctx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;

	inputs->name       = av_strdup("out");
	inputs->filter_ctx = buffersink_ctx;
	inputs->pad_idx    = 0;
	inputs->next       = NULL;

	ret = avfilter_graph_parse_ptr(filter_graph, filter_descr,
		&inputs, &outputs, NULL);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	if (ret < 0) {
		LOGERROR("VideoFilterInit: avfilter_graph_parse_ptr failed (%d)", ret);
		avfilter_graph_free(&filter_graph);
		return -1;
	}

	ret = avfilter_graph_config(filter_graph, NULL);
	if (ret < 0) {
		LOGERROR("VideoFilterInit: avfilter_graph_config failed (%d)", ret);
		avfilter_graph_free(&filter_graph);
		return -1;
	}

	return 0;
}

///
///	Display a ffmpeg frame
///
///	@param render	video render
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///	@retval 0	success or error, return (frame is freed or moved to the render ringbuffer)
///	@retval	-1	ringbuffer full, try again
///
int cVideoRender::VideoRenderFrame(AVCodecContext * video_ctx, AVFrame * frame, int flags)
{
	int interlaced;

	if (!StartCounter) {
		timebase = &video_ctx->pkt_timebase;
	}

	if (frame->decode_error_flags || frame->flags & AV_FRAME_FLAG_CORRUPT) {
		LOGWARNING("VideoRenderFrame: error_flag or FRAME_FLAG_CORRUPT");
	}

	FrameData *fd;
	if (!frame->opaque_ref) {
		frame->opaque_ref = av_buffer_allocz(sizeof(*fd));
		if (!frame->opaque_ref)
			LOGFATAL("VideoRenderFrame: cannot allocate private frame data");
	}
	fd = (FrameData *)frame->opaque_ref->data;
	fd->flags = flags;

	if (Closing || Flushing) {
		av_frame_free(&frame);
		return 0;
	}

#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
	interlaced = frame->interlaced_frame;
#else
	interlaced = !!(frame->flags & AV_FRAME_FLAG_INTERLACED);
#endif
	// we can't trust frame->interlaced_frame ...
	if (!(fd->flags & FRAME_FLAG_TRICKSPEED || fd->flags & FRAME_FLAG_STILLPICTURE) && video_ctx->framerate.num > 0) {
		if (video_ctx->framerate.num / video_ctx->framerate.den > 30)
			interlaced = 0;
		else
			interlaced = 1;
	}
	if (!(fd->flags & FRAME_FLAG_TRICKSPEED || fd->flags & FRAME_FLAG_STILLPICTURE) && (video_ctx->codec_id == AV_CODEC_ID_HEVC))
		interlaced = 0;

// TODO render
	if (!(fd->flags & FRAME_FLAG_TRICKSPEED || fd->flags & FRAME_FLAG_STILLPICTURE))
		Device->VideoStream->SetInterlaced(interlaced);

	if (frame->format == AV_PIX_FMT_YUV420P ||
	   (frame->format == AV_PIX_FMT_DRM_PRIME && interlaced && !((HardwareQuirks & QUIRK_NO_HW_DEINT) || FilterDeintDisabled))) {
		// use deinterlace/scale filter
		// AV_PIX_FMT_YUV420P, interlaced -> software deinterlacer (bwdif filter)
		// AV_PIX_FMT_YUV420P, progressive -> scale filter to get NV12 frames
		// AV_PIX_FMT_DRM_PRIME, interlaced, hw deinterlacer available -> hw deinterlacer
		// -> put the frame into filter Rb
		if (FilterClosing) {
			av_frame_free(&frame);
			return 0;
		}

		if (!FilterThread) {
			LOGDEBUG("VideoRenderFrame: wakeup filter thread");
			if (VideoFilterInit(video_ctx, frame)) {
				av_frame_free(&frame);
				return 0;
			} else {
				pthread_create(&FilterThread, NULL, FilterHandlerThread, this);
				pthread_setname_np(FilterThread, "filter thread");
			}
		}

		if (atomic_read(&FramesDeintFilled) < VIDEO_SURFACES_MAX) {
			FramesDeintRb[FramesDeintWrite] = frame;
			FramesDeintWrite = (FramesDeintWrite + 1) % VIDEO_SURFACES_MAX;
			atomic_inc(&FramesDeintFilled);
		} else {
			usleep(10000);
			return -1;
		}
	} else {
		// don't use deinterlace/scale filter
		if (frame->format == AV_PIX_FMT_DRM_PRIME) {
			// AV_PIX_FMT_DRM_PRIME, interlaced, hw deinterlacer not available
			// AV_PIX_FMT_DRM_PRIME, progressive
			// -> put the frame directly in render Rb
			pthread_mutex_lock(&DisplayQueue);
			if (Closing || Flushing) {
				av_frame_free(&frame);
				return 0;
			}
			if (atomic_read(&FramesFilled) < VIDEO_SURFACES_MAX && !Filter_Frames) {
				FramesRb[FramesWrite] = frame;
				FramesWrite = (FramesWrite + 1) % VIDEO_SURFACES_MAX;
				atomic_inc(&FramesFilled);
				pthread_mutex_unlock(&DisplayQueue);
			} else {
				pthread_mutex_unlock(&DisplayQueue);
				usleep(1000);
				return -1;
			}
		} else {
			// AV_PIX_FMT_DRM_NV12 ?
			// -> go through EnqueueFB
			pthread_mutex_lock(&DisplayQueue);
			if (atomic_read(&FramesFilled) < VIDEO_SURFACES_MAX) {
				pthread_mutex_unlock(&DisplayQueue);
				EnqueueFB(frame);
			} else {
				pthread_mutex_unlock(&DisplayQueue);
				usleep(5000);
				return -1;
			}
		}
	}
	return 0;
}

///
///	Set video clock.
///
void cVideoRender::VideoSetClock(int64_t ppts)
{
	pthread_mutex_lock(&VideoClockMutex);
	pts = ppts;
	pthread_mutex_unlock(&VideoClockMutex);
}

///
///	Get video clock.
///
///	@param hw_decoder	video hardware decoder
///
///	@note this isn't monoton, decoding reorders frames, setter keeps it
///	monotonic
///
int64_t cVideoRender::VideoGetClock(void)
{
	int64_t PTS;
	pthread_mutex_lock(&VideoClockMutex);
	PTS = pts;
	pthread_mutex_unlock(&VideoClockMutex);
	return PTS;
}

///
///	send start condition to video thread.
///
///	@param hw_render	video hardware render
///
void cVideoRender::StartVideo(void)
{
	VideoResume();
	StartCounter = 0;
	LOGDEBUG("StartVideo: reset StartCounter %d Closing %d TrickSpeed %d",
		StartCounter, Closing, VideoGetTrickSpeed());
}

///
///	Close the renderer and clear frames (and) framebuffers if needed
///
///	@param hw_render	video hardware render
///	@param black		true, if should set a black fb and clear the last framebuffer,
///				otherwise wait for the clear until the next frame arrives
///
void cVideoRender::VideoSetClosing(int black)
{
	LOGDEBUG("VideoSetClosing: StartCounter %d", StartCounter);
	if (!DisplayThread->Active())
		return;

	pthread_mutex_lock(&WaitCleanMutex);
	FilterClosing = 1;
	Flushing = !black;
	Closing = black;

	if (VideoIsPaused())
		VideoResume();

	LOGDEBUG("VideoSetClosing: pthread_cond_wait");
	pthread_cond_wait(&WaitCleanCondition, &WaitCleanMutex);
	pthread_mutex_unlock(&WaitCleanMutex);
	LOGDEBUG("VideoSetClosing: NACH pthread_cond_wait");

	StartCounter = 0;
	FramesDuped = 0;
	FramesDropped = 0;
	if (black)
		VideoSetTrickSpeed(0, 1);
}

///
//	Pause video.
//
void cVideoRender::VideoPause(void)
{
	LOGDEBUG("VideoPause:");
	pthread_mutex_lock(&PlaybackMutex);
	VideoPaused = 1;
	pthread_mutex_unlock(&PlaybackMutex);
}

///
//	Resume video.
//
void cVideoRender::VideoResume(void)
{
	LOGDEBUG("VideoResume:");
	pthread_mutex_lock(&PlaybackMutex);
	VideoPaused = 0;
	pthread_mutex_unlock(&PlaybackMutex);
}

///
//	Check pause status
//
int cVideoRender::VideoIsPaused(void)
{
	int ret;
	pthread_mutex_lock(&PlaybackMutex);
	ret = VideoPaused;
	pthread_mutex_unlock(&PlaybackMutex);
	return ret;
}

///
///	Set trick play speed, start video if paused
///
///	@param hw_render	video hardware render
///	@param speed		trick speed (0 = normal)
///	@param forward		1 if forward trick speed
///
void cVideoRender::VideoTrickSpeed(int speed, int forward)
{
	VideoSetTrickSpeed(speed, forward);

	if (VideoIsPaused())
		StartVideo();
}

///
///	Simply set trick play speed values
///
///	@param hw_render	video hardware render
///	@param speed		trick speed (0 = normal)
///	@param forward		1 if forward trick speed
///
void cVideoRender::VideoSetTrickSpeed(int speed, int forward)
{
	LOGDEBUG2(L_TRICK, "VideoSetTrickSpeed: set trick speed %d %s", speed, forward ? "forward" : "backward");
	pthread_mutex_lock(&TrickSpeedMutex);
	TrickSpeed = speed;
	TrickCounter = speed;
	TrickForward = forward;
	pthread_mutex_unlock(&TrickSpeedMutex);
}

///
///	Return the current trick speed mode
///
int cVideoRender::VideoGetTrickSpeed(void)
{
	int speed;
	pthread_mutex_lock(&TrickSpeedMutex);
	speed = TrickSpeed;
	pthread_mutex_unlock(&TrickSpeedMutex);
	return speed;
}

///
///	Return the current trick speed direction
///
int cVideoRender::VideoGetTrickForward(void)
{
	int dir;
	pthread_mutex_lock(&TrickSpeedMutex);
	dir = TrickForward;
	pthread_mutex_unlock(&TrickSpeedMutex);
	return dir;
}

///
///	Return the current trick counter
///
int cVideoRender::VideoGetTrickCounter(void)
{
	int counter;
	pthread_mutex_lock(&TrickSpeedMutex);
	counter = TrickCounter;
	pthread_mutex_unlock(&TrickSpeedMutex);
	return counter;
}

///
///	Set the trick counter
///
void cVideoRender::VideoSetTrickCounter(int counter)
{
	pthread_mutex_lock(&TrickSpeedMutex);
	TrickCounter = counter;
	pthread_mutex_unlock(&TrickSpeedMutex);
}

///
///	Decrease the trick counter
///
int cVideoRender::VideoDecTrickCounter(void)
{
	int counter;
	pthread_mutex_lock(&TrickSpeedMutex);
	TrickCounter--;
	counter = TrickCounter;
	pthread_mutex_unlock(&TrickSpeedMutex);
	return counter;
}

///
//	Play video.
//
void cVideoRender::VideoPlay(void)
{
	LOGDEBUG("VideoPlay:");
	if (VideoGetTrickSpeed()) {
		VideoSetTrickSpeed(0, 1);
	}

	StartVideo();
}

///
///	Grab full screen image
///
///	@param grabimage[out]	the struct to grab in
///	@param buf[in]		current video buffer
///	@param ready[out]	ready is set true if we finished
///	@param is_osd		is this an osd grab? (just for logs)
///
void cVideoRender::VideoGrab(struct grabimage *grabimage, struct drm_buf *buf, int *ready, int is_osd)
{
	// early return if buf = NULL
	if (!buf) {
		grabimage->result = NULL;
		grabimage->size = 0;
		*ready = 1;
		return;
	}

	int size;
	for (int plane = 0; plane < buf->num_planes; plane++) {
		LOGDEBUG2(L_GRAB, "VideoGrab: %s plane %d address %p pitch %d offset %d handle %d size %d", is_osd ? "OSD" : "VIDEO",
		       plane, buf->plane[plane], buf->pitch[plane], buf->offset[plane], buf->handle[plane], buf->size[plane]);
	}
	// result's width and height are original dimensions how buffer is presented on the screen
	grabimage->result = buf2rgb(buf, &size, grabimage->width, grabimage->height, is_osd ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGB24);
	grabimage->size = size;
	*ready = 1;
}

///
///	Trigger grabbing in render thread
///
///	@param hw_render	video hardware render
///
void cVideoRender::VideoTriggerGrab(void)
{
	grabinwork = 1;
	grabvideo = (struct grabimage *)malloc(sizeof(struct grabimage));
	grabosd = (struct grabimage *)malloc(sizeof(struct grabimage));
	grabvideo->buf = NULL;
	grabosd->buf = NULL;

	grabvideoready = 0;
	grabosdready = 0;
	startgrab = 1;
}

///
///	Clear grabbing struct
///
///	@param hw_render	video hardware render
///
void cVideoRender::VideoClearGrab(void)
{
	free(grabvideo);
	free(grabosd);
	grabvideoready = 0;
	grabosdready = 0;
	startgrab = 0;
	grabinwork = 0;
}

///
///	Check, if the grabbed image is ready
///
///	@param hw_render	video hardware render
///
///	@retval 0		grab is not ready
///	@retval 1		grab is ready
///
int cVideoRender::VideoGrabReady(void)
{
	return (grabvideoready && grabosdready);
}

///
///	Check, if the grab is in work
///
///	@param hw_render	video hardware render
///
///	@retval 0		grab is not in work
///	@retval 1		grab is in work
///
int cVideoRender::VideoGrabInWork(void)
{
	return grabinwork;
}

///
///	Get the grabbed image
///
///	@param hw_render	video hardware render
///	@param[out] size	returns output size (memory)
///	@param[out] width	returns output width
///	@param[out] height	returns output height
///	@param[in] is_osd	is this an osd grab?
///
///	@returns the pointer to the image data
///
uint8_t *cVideoRender::VideoGetGrab(int *size, int *width, int *height, int *x, int *y, int is_osd)
{
	struct grabimage *grab;
	if (is_osd)
		grab = grabosd;
	else
		grab = grabvideo;

	LOGDEBUG2(L_GRAB, "VideoGetGrab: %s size %d %dx%d at %d|%x %p", is_osd ? "OSD" : "VIDEO", grab->size, grab->width, grab->height, grab->x, grab->y, grab->result);
	if (!grab->size)
		return NULL;

	if (size)
		*size = grab->size;
	if (width)
		*width = grab->width;
	if (height)
		*height = grab->height;
	if (x)
		*x = grab->x;
	if (y)
		*y = grab->y;

	return grab->result;
}

///
///	Get render statistics.
///
///	@param hw_render	video hardware render
///	@param[out] duped	duped frames
///	@param[out] dropped	dropped frames
///	@param[out] count	number of decoded frames
///
void cVideoRender::VideoGetStats(int *duped, int *dropped, int *counter)
{
    *duped = FramesDuped;
    *dropped = FramesDropped;
    *counter = StartCounter;
}

//----------------------------------------------------------------------------
//	Setup
//----------------------------------------------------------------------------

///
///	Get screen size.
///
///	@param[out] width	video stream width
///	@param[out] height	video stream height
///	@param[out] aspect_num	video stream aspect numerator
///	@param[out] aspect_den	video stream aspect denominator
///
void cVideoRender::VideoGetScreenSize(int *width, int *height, double *pixel_aspect)
{
	*width = mode.hdisplay;
	*height = mode.vdisplay;
	*pixel_aspect = (double)16 / (double)9;
}

///
///	Initialize video output module.
///
void cVideoRender::VideoInit(void)
{
	unsigned int i;

	if (FindDevice()){
		LOGFATAL("VideoInit: FindDevice() failed");
	}

	ReadHWPlatform();

	bufs[0].width = bufs[1].width = 0;
	bufs[0].height = bufs[1].height = 0;
	bufs[0].pix_fmt = bufs[1].pix_fmt = DRM_FORMAT_NV12;

	// osd FB
#ifndef USE_GLES
	if (!buf_osd)
		buf_osd = calloc(1, sizeof(struct drm_buf));
	buf_osd->pix_fmt = DRM_FORMAT_ARGB8888;
	buf_osd->width = mode.hdisplay;
	buf_osd->height = mode.vdisplay;
	if (SetupFB(buf_osd, NULL)){
		LOGFATAL("VideoOsdInit: SetupFB FB OSD failed!");
	}
#else
	if (DisableOglOsd) {
		if (!buf_osd)
			buf_osd = (struct drm_buf *)calloc(1, sizeof(struct drm_buf));
		buf_osd->pix_fmt = DRM_FORMAT_ARGB8888;
		buf_osd->width = mode.hdisplay;
		buf_osd->height = mode.vdisplay;
		if (SetupFB(buf_osd, NULL)){
			LOGFATAL("VideoOsdInit: SetupFB FB OSD failed!");
		}
	}
#endif

	// black fb
	buf_black.pix_fmt = DRM_FORMAT_NV12;
	buf_black.width = mode.hdisplay;
	buf_black.height = mode.vdisplay;
	LOGDEBUG2(L_DRM, "Videoinit: Try to create a black FB");
	if (SetupFB(&buf_black, NULL))
		LOGERROR("VideoInit: SetupFB black FB %i x %i failed",
			buf_black.width, buf_black.height);

	for (i = 0; i < buf_black.width * buf_black.height; ++i) {
		buf_black.plane[0][i] = 0x10;
		if (i < buf_black.width * buf_black.height / 2)
		buf_black.plane[1][i] = 0x80;
	}

	// save actual modesetting
	saved_crtc = drmModeGetCrtc(fd_drm, crtc_id);

	drmModeAtomicReqPtr ModeReq;
	const uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	uint32_t modeID = 0;

	if (drmModeCreatePropertyBlob(fd_drm, &mode, sizeof(mode), &modeID) != 0)
		LOGERROR("Failed to create mode property blob.");
	if (!(ModeReq = drmModeAtomicAlloc()))
		LOGERROR("cannot allocate atomic request (%d): %m", errno);

	SetPropertyRequest(ModeReq, fd_drm, crtc_id,
						DRM_MODE_OBJECT_CRTC, "MODE_ID", modeID);
	SetPropertyRequest(ModeReq, fd_drm, connector_id,
						DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", crtc_id);
	SetPropertyRequest(ModeReq, fd_drm, crtc_id,
						DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);

	// Osd plane
	// We don't have the buf_osd for OpenGL yet, so we can't set anything. Set src and FbId later when osd was drawn,
	// but initially move the OSD behind the VIDEO
#ifndef USE_GLES
	planes[OSD_PLANE]->properties.crtc_id = crtc_id;
	planes[OSD_PLANE]->properties.fb_id = buf_osd->fb_id;
	planes[OSD_PLANE]->properties.crtc_x = 0;
	planes[OSD_PLANE]->properties.crtc_y = 0;
	planes[OSD_PLANE]->properties.crtc_w = mode.hdisplay;
	planes[OSD_PLANE]->properties.crtc_h = mode.vdisplay;
	planes[OSD_PLANE]->properties.src_x = 0;
	planes[OSD_PLANE]->properties.src_y = 0;
	planes[OSD_PLANE]->properties.src_w = buf_osd->width;
	planes[OSD_PLANE]->properties.src_h = buf_osd->height;

	SetPlane(ModeReq, planes[OSD_PLANE]);
#else
	if (DisableOglOsd) {
		planes[OSD_PLANE]->properties.crtc_id = crtc_id;
		planes[OSD_PLANE]->properties.fb_id = buf_osd->fb_id;
		planes[OSD_PLANE]->properties.crtc_x = 0;
		planes[OSD_PLANE]->properties.crtc_y = 0;
		planes[OSD_PLANE]->properties.crtc_w = mode.hdisplay;
		planes[OSD_PLANE]->properties.crtc_h = mode.vdisplay;
		planes[OSD_PLANE]->properties.src_x = 0;
		planes[OSD_PLANE]->properties.src_y = 0;
		planes[OSD_PLANE]->properties.src_w = buf_osd->width;
		planes[OSD_PLANE]->properties.src_h = buf_osd->height;

		SetPlane(ModeReq, planes[OSD_PLANE]);
	}
#endif
	if (use_zpos) {
		planes[VIDEO_PLANE]->properties.zpos = zpos_overlay;
		SetPlaneZpos(ModeReq, planes[VIDEO_PLANE]);
#ifdef USE_GLES
		planes[OSD_PLANE]->properties.zpos = zpos_primary;
		SetPlaneZpos(ModeReq, planes[OSD_PLANE]);
#endif
	}

	planes[VIDEO_PLANE]->properties.crtc_id = crtc_id;
	planes[VIDEO_PLANE]->properties.fb_id = buf_black.fb_id;
	planes[VIDEO_PLANE]->properties.crtc_x = 0;
	planes[VIDEO_PLANE]->properties.crtc_y = 0;
	planes[VIDEO_PLANE]->properties.crtc_w = mode.hdisplay;
	planes[VIDEO_PLANE]->properties.crtc_h = mode.vdisplay;
	planes[VIDEO_PLANE]->properties.src_x = 0;
	planes[VIDEO_PLANE]->properties.src_y = 0;
	planes[VIDEO_PLANE]->properties.src_w = buf_black.width;
	planes[VIDEO_PLANE]->properties.src_h = buf_black.height;

	// Black Buffer for video plane
	SetPlane(ModeReq, planes[VIDEO_PLANE]);

	if (drmModeAtomicCommit(fd_drm, ModeReq, flags, NULL) != 0) {
#ifndef USE_GLES
		DumpPlaneProperties(planes[OSD_PLANE]);
#endif
		DumpPlaneProperties(planes[VIDEO_PLANE]);

		drmModeAtomicFree(ModeReq);
		LOGFATAL("VideoInit: cannot set atomic mode (%d): %m", errno);
	}

	drmModeAtomicFree(ModeReq);

	OsdShown = 0;

	// init variables page flip
	memset(&ev, 0, sizeof(ev));
	ev.version = 2;

	// Wakeup DisplayHandlerThread
	VideoThreadWakeup(0, 1);
}

///
///	Cleanup video output module.
///
void cVideoRender::VideoExit(void)
{
	VideoThreadExit();

	// restore saved CRTC configuration
	if (saved_crtc){
		drmModeSetCrtc(fd_drm, saved_crtc->crtc_id, saved_crtc->buffer_id,
			saved_crtc->x, saved_crtc->y, &connector_id, 1, &saved_crtc->mode);
		drmModeFreeCrtc(saved_crtc);
	}

	for (int i = 0; i < MAX_PLANES; i++) {
		if (planes[i]) {
			free_properties(planes[i]);
			free(planes[i]);
		}
	}

	DestroyFB(&buf_black);
#ifdef USE_GLES
	if (DisableOglOsd) {
		if (buf_osd) {
			DestroyFB(buf_osd);
			free(buf_osd);
		}
	} else {
		if (next_bo)
			gbm_bo_destroy(next_bo);
		if (old_bo)
			gbm_bo_destroy(old_bo);
	}
#else
	if (buf_osd) {
		DestroyFB(buf_osd);
		free(buf_osd);
	}
#endif

	close(fd_drm);
}

///
///	Set size and position of the video.
///
void cVideoRender::VideoSetOutputPosition(int x, int y, int width, int height)
{
	video.x = x;
	video.y = y;
	video.width = width;
	video.height = height;

	if (video.x == 0 &&
	    video.y == 0 &&
	    video.width == 0 &&
	    video.height == 0)
		video.is_scaled = 0;
	else
		video.is_scaled = 1;

	LOGDEBUG("VideoSetOutputPosition %d %d %d %d%s", x, y, width, height, video.is_scaled ? ", video is scaled" : "");
}

int cVideoRender::VideoCodecMode(void)
{
	return CodecMode;
}

void cVideoRender::VideoSetDisableDeint(int disable)
{
	ConfigFilterDeintDisabled = disable;
}

void cVideoRender::VideoSetDisableOglOsd(void)
{
	DisableOglOsd = 1;
}
