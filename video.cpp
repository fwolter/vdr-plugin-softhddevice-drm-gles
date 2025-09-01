/**
 * @file video.cpp
 * @brief Rendering module
 *
 * Copyright: (c) 2009 - 2015 by Johns.  All Rights Reserved.
 * Copyright: (c) 2018 by zille.  All Rights Reserved.
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

#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <stdbool.h>
#include <unistd.h>

#include <inttypes.h>

#include <libintl.h>

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
}
#include "buf2rgb.h"

#include "video.h"
#include "audio.h"
#include "drm.h"
#include "threads.h"
#include "grab.h"

//----------------------------------------------------------------------------
//	Helper functions
//----------------------------------------------------------------------------

/*****************************************************************************
 * cVideoRender class
 ****************************************************************************/

static void ReleaseFrame( __attribute__ ((unused)) void *opaque, uint8_t *data)
{
	AVDRMFrameDescriptor *primedata = (AVDRMFrameDescriptor *)data;

	av_free(primedata);
}

static int GetPropertyValue(int m_fdDrm, uint32_t objectID,
		     uint32_t objectType, const char *propName, uint64_t *value)
{
	uint32_t i;
	int found = 0;
	drmModePropertyPtr Prop;
	drmModeObjectPropertiesPtr objectProps =
		drmModeObjectGetProperties(m_fdDrm, objectID, objectType);

	for (i = 0; i < objectProps->count_props; i++) {
		if ((Prop = drmModeGetProperty(m_fdDrm, objectProps->props[i])) == NULL)
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

	if (objectID == m_pPlanes[VIDEO_PLANE]->plane_id)
		obj = m_pPlanes[VIDEO_PLANE];
	else if (objectID == m_pPlanes[OSD_PLANE]->plane_id)
		obj = m_pPlanes[OSD_PLANE];

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

static int SetPropertyRequest(drmModeAtomicReqPtr ModeReq, int m_fdDrm,
					uint32_t objectID, uint32_t objectType,
					const char *propName, uint64_t value)
{
	uint32_t i;
	uint64_t id = 0;
	drmModePropertyPtr Prop;
	drmModeObjectPropertiesPtr objectProps =
		drmModeObjectGetProperties(m_fdDrm, objectID, objectType);

	for (i = 0; i < objectProps->count_props; i++) {
		if ((Prop = drmModeGetProperty(m_fdDrm, objectProps->props[i])) == NULL)
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
	m_hardwareQuirks = 0;

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
			m_hardwareQuirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
			break;
		}
		if (strstr(read_ptr, "bcm2711")) {
			LOGDEBUG2(L_DRM, "ReadHWPlatform: bcm2711 (Raspberry Pi 4 Model B, Compute Module 4, Pi 400) found");
			m_hardwareQuirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
			break;
		}
		if (strstr(read_ptr, "bcm2712")) {
			LOGDEBUG2(L_DRM, "ReadHWPlatform: bcm2712 (Raspberry Pi 5, Compute Module 5, Pi 500) found");
			m_hardwareQuirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
			break;
		}
		if (strstr(read_ptr, "amlogic")) {
			LOGDEBUG2(L_DRM, "ReadHWPlatform: amlogic found, disable HW deinterlacer");
			m_hardwareQuirks |= QUIRK_CODEC_NEEDS_EXT_INIT
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

	if (drmModeAtomicCommit(m_fdDrm, ModeReq, flags, NULL) != 0) {
		LOGDEBUG2(L_DRM, "CheckZpos: cannot set atomic mode (%d), don't use zpos change: %m", errno);
		m_useZpos = 0;
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

EGLConfig cVideoRender::GetEGLConfig(void)
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
    eglGetConfigs(m_eglDisplay, NULL, 0, &count);
//    EGL_CHECK(eglGetConfigs(m_eglDisplay, NULL, 0, &count));
    if (count < 1) {
        LOGFATAL("no EGL configs to choose from");
    }

    LOGDEBUG2(L_OPENGL, "%d EGL configs found", count);

    configs = (EGLConfig *)malloc(count * sizeof(*configs));
    if (!configs)
        LOGFATAL("can't allocate space for EGL configs");

    eglChooseConfig(m_eglDisplay, config_attribute_list, configs, count, &matched);
//    EGL_CHECK(eglChooseConfig(m_eglDisplay, config_attribute_list, configs, count, &matched));
    if (!matched) {
        LOGFATAL("no EGL configs with appropriate attributes");
    }

    LOGDEBUG2(L_OPENGL, "%d appropriate EGL configs found, which match attributes", matched);

    for (int i = 0; i < matched; ++i) {
        EGLint gbm_format;
        eglGetConfigAttrib(m_eglDisplay, configs[i], EGL_NATIVE_VISUAL_ID, &gbm_format);
//        EGL_CHECK(eglGetConfigAttrib(m_eglDisplay, configs[i], EGL_NATIVE_VISUAL_ID, &gbm_format));

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

void cVideoRender::SetDisplayResolution(const char* resolution)
{
	sscanf(resolution, "%dx%d@%d", &m_userReqDisplayWidth, &m_userReqDisplayHeight, &m_userReqDisplayRefreshRate);
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
		drmModeEncoder *encoder = drmModeGetEncoder(m_fdDrm, encoder_id);

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
	m_pGbmDevice = gbm_create_device(m_fdDrm);
	if (!m_pGbmDevice) {
		LOGERROR("FindDevice: failed to create gbm device!");
		return -1;
	}

	m_pGbmSurface = gbm_surface_create(m_pGbmDevice, w, h, format, modifier);
	if (!m_pGbmSurface) {
		LOGERROR("FindDevice: failed to create %d x %d surface bo", w, h);
		return -1;
	}

	return 0;
}

int cVideoRender::InitEGL(void)
{
	EGLint iMajorVersion, iMinorVersion;

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	assert(get_platform_display != NULL);
	PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC get_platform_surface = (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
	assert(get_platform_surface != NULL);

	m_eglDisplay = get_platform_display(EGL_PLATFORM_GBM_KHR, m_pGbmDevice, NULL);
//	EGL_CHECK(m_eglDisplay = get_platform_display(EGL_PLATFORM_GBM_KHR, m_pGbmDevice, NULL));
	if (!m_eglDisplay) {
		LOGERROR("FindDevice: failed to get eglDisplay");
		return -1;
	}

	if (!eglInitialize(m_eglDisplay, &iMajorVersion, &iMinorVersion)) {
		LOGERROR("FindDevice: eglInitialize failed");
		return -1;
	}

	LOGDEBUG2(L_OPENGL, "FindDevice: Using display %p with EGL version %d.%d", m_eglDisplay, iMajorVersion, iMinorVersion);
	EGL_CHECK(LOGDEBUG2(L_OPENGL, "  EGL Version: \"%s\"", eglQueryString(m_eglDisplay, EGL_VERSION)));
	EGL_CHECK(LOGDEBUG2(L_OPENGL, "  EGL Vendor: \"%s\"", eglQueryString(m_eglDisplay, EGL_VENDOR)));
	EGL_CHECK(LOGDEBUG2(L_OPENGL, "  EGL Extensions: \"%s\"", eglQueryString(m_eglDisplay, EGL_EXTENSIONS)));
	EGL_CHECK(LOGDEBUG2(L_OPENGL, "  EGL APIs: \"%s\"", eglQueryString(m_eglDisplay, EGL_CLIENT_APIS)));

	EGLConfig eglConfig = GetEGLConfig();

	EGL_CHECK(eglBindAPI(EGL_OPENGL_ES_API));
	EGL_CHECK(m_eglContext = eglCreateContext(m_eglDisplay, eglConfig, EGL_NO_CONTEXT, context_attribute_list));
	if (!m_eglContext) {
		LOGERROR("FindDevice: failed to create eglContext");
		return -1;
	}

	EGL_CHECK(m_eglSurface = get_platform_surface(m_eglDisplay, eglConfig, m_pGbmSurface, NULL));
	if (m_eglSurface == EGL_NO_SURFACE) {
		LOGERROR("FindDevice: failed to create eglSurface");
		return -1;
	}

	EGLint s_width, s_height;
	EGL_CHECK(eglQuerySurface(m_eglDisplay, m_eglSurface, EGL_WIDTH, &s_width));
	EGL_CHECK(eglQuerySurface(m_eglDisplay, m_eglSurface, EGL_HEIGHT, &s_height));

	LOGDEBUG2(L_OPENGL, "FindDevice: EGLSurface %p on EGLDisplay %p for %d x %d BO created", m_eglSurface, m_eglDisplay, s_width, s_height);

	m_glInitiated = 1;
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
	m_fdDrm = find_drm_device(&resources);
	if (m_fdDrm < 0) {
		LOGERROR("FindDevice: Could not open device!");
		return -1;
	}

	LOGDEBUG2(L_DRM, "FindDevice: DRM have %i connectors, %i crtcs, %i encoders",
		resources->count_connectors, resources->count_crtcs,
		resources->count_encoders);

	// find a connector
	connector = find_drm_connector(m_fdDrm, resources);
	if (!connector) {
		LOGERROR("FindDevice: cannot retrieve DRM connector (%d): %m", errno);
		return -errno;
	}
	m_connectorId = connector->connector_id;

	// find a user requested mode
	if (m_userReqDisplayWidth) {
		for (i = 0; i < connector->count_modes; i++) {
			drmModeModeInfo *current_mode = &connector->modes[i];
			if(current_mode->hdisplay == m_userReqDisplayWidth && current_mode->vdisplay == m_userReqDisplayHeight &&
			   current_mode->vrefresh == m_userReqDisplayRefreshRate && !(current_mode->flags & DRM_MODE_FLAG_INTERLACE)) {
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

	memcpy(&m_drmModeInfo, drmmode, sizeof(drmModeModeInfo));

	// find encoder
	for (i = 0; i < resources->count_encoders; i++) {
		encoder = drmModeGetEncoder(m_fdDrm, resources->encoders[i]);
		if (encoder->encoder_id == connector->encoder_id)
			break;
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}

	if (encoder) {
		m_crtcId = encoder->crtc_id;
		LOGDEBUG2(L_DRM, "FindDevice: have encoder, m_crtcId %d", m_crtcId);
	} else {
		int32_t crtc_id = find_crtc_for_connector(resources, connector);
		if (crtc_id == -1) {
			LOGERROR("FindDevice: No crtc found!");
			return -errno;
		}

		m_crtcId = crtc_id;
		LOGDEBUG2(L_DRM, "FindDevice: have no encoder, m_crtcId %d", m_crtcId);
	}

	for (i = 0; i < resources->count_crtcs; i++) {
		if (resources->crtcs[i] == m_crtcId) {
			m_crtcIndex = i;
			break;
		}
	}

	LOGINFO("FindDevice: Using Monitor Mode %dx%d@%d, m_crtcId %d crtc_idx %d",
		m_drmModeInfo.hdisplay, m_drmModeInfo.vdisplay, m_drmModeInfo.vrefresh, m_crtcId, m_crtcIndex);

	drmModeFreeConnector(connector);


	// find planes
	if ((plane_res = drmModeGetPlaneResources(m_fdDrm)) == NULL) {
		LOGERROR("FindDevice: cannot retrieve PlaneResources (%d): %m", errno);
		return -1;
	}

	// allocate local plane structs
	for (i = 0; i < MAX_PLANES; i++) {
		m_pPlanes[i] = (struct plane *)calloc(1, sizeof(struct plane));
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
		plane = drmModeGetPlane(m_fdDrm, plane_res->planes[j]);

		if (plane == NULL) {
			LOGERROR("FindDevice: cannot query DRM-KMS plane %d", j);
			continue;
		}

		uint64_t type;
		uint64_t zpos;
		char pixelformats[256];

		if (plane->possible_crtcs & (1 << m_crtcIndex)) {
			if (GetPropertyValue(m_fdDrm, plane_res->planes[j],
					     DRM_MODE_OBJECT_PLANE, "type", &type)) {
				LOGDEBUG2(L_DRM, "FindDevice: Failed to get property 'type'");
			}
			if (GetPropertyValue(m_fdDrm, plane_res->planes[j],
					     DRM_MODE_OBJECT_PLANE, "zpos", &zpos)) {
				LOGDEBUG2(L_DRM, "FindDevice: Failed to get property 'zpos'");
			} else {
				m_useZpos = 1;
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
		m_pPlanes[VIDEO_PLANE]->plane_id = best_primary_video_plane.plane_id;
		m_pPlanes[VIDEO_PLANE]->type = best_primary_video_plane.type;
		m_pPlanes[VIDEO_PLANE]->properties.zpos = m_zposPrimary = best_primary_video_plane.properties.zpos;
		m_pPlanes[OSD_PLANE]->plane_id = best_overlay_osd_plane.plane_id;
		m_pPlanes[OSD_PLANE]->type = best_overlay_osd_plane.type;
		m_pPlanes[OSD_PLANE]->properties.zpos = m_zposOverlay = best_overlay_osd_plane.properties.zpos;
	} else if (best_overlay_video_plane.plane_id && best_primary_osd_plane.plane_id) {
		m_pPlanes[VIDEO_PLANE]->plane_id = best_overlay_video_plane.plane_id;
		m_pPlanes[VIDEO_PLANE]->type = best_overlay_video_plane.type;
		m_pPlanes[VIDEO_PLANE]->properties.zpos = m_zposOverlay = best_overlay_video_plane.properties.zpos;
		m_pPlanes[OSD_PLANE]->plane_id = best_primary_osd_plane.plane_id;
		m_pPlanes[OSD_PLANE]->type = best_primary_osd_plane.type;
		m_pPlanes[OSD_PLANE]->properties.zpos = m_zposPrimary = best_primary_osd_plane.properties.zpos;
		m_useZpos = 1;
	} else {
		LOGERROR("FindDevice: No suitable planes found!");
		return -1;
	}

	// fill the plane's properties to speed up SetPropertyRequest later
	get_properties(m_fdDrm, m_pPlanes[VIDEO_PLANE]->plane_id, m_pPlanes[VIDEO_PLANE]);
	get_properties(m_fdDrm, m_pPlanes[OSD_PLANE]->plane_id, m_pPlanes[OSD_PLANE]);

	// Check, if we can set z-order (meson and rpi have fixed z-order, which cannot be changed)
	if (m_useZpos && CheckZpos(m_pPlanes[VIDEO_PLANE], m_pPlanes[VIDEO_PLANE]->properties.zpos)) {
		m_useZpos = 0;
	}
	if (m_useZpos && CheckZpos(m_pPlanes[OSD_PLANE], m_pPlanes[OSD_PLANE]->properties.zpos)) {
		m_useZpos = 0;
	}

	// m_useZpos was set, if Video is on OVERLAY, and Osd is on PRIMARY
	// Check if the OVERLAY plane really got a higher zpos than the PRIMARY plane
	// If not, change their zpos values or hardcode them to
	// 1 OVERLAY (Video)
	// 0 PRIMARY (Osd)
	if (m_useZpos && m_zposOverlay <= m_zposPrimary) {
		char str_zpos[256];
		strcpy(str_zpos, "FindDevice: zpos values are wrong, so ");
		if (m_zposOverlay == m_zposPrimary) {
			// is this possible?
			strcat(str_zpos, "hardcode them to 0 and 1, because they are equal");
			m_zposPrimary = 0;
			m_zposOverlay = 1;
		} else {
			strcat(str_zpos, "switch them");
			uint64_t zpos_tmp = m_zposPrimary;
			m_zposPrimary = m_zposOverlay;
			m_zposOverlay = zpos_tmp;
		}
		LOGDEBUG2(L_DRM, "%s", str_zpos);
	}
	drmModeFreePlaneResources(plane_res);
	drmModeFreeEncoder(encoder);
	drmModeFreeResources(resources);

	LOGINFO("FindDevice: DRM setup - CRTC: %i video_plane: %i (%s %" PRIu64 ") osd_plane: %i (%s %" PRIu64 ") m_useZpos: %d",
		m_crtcId, m_pPlanes[VIDEO_PLANE]->plane_id,
		m_pPlanes[VIDEO_PLANE]->type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY",
		m_pPlanes[VIDEO_PLANE]->properties.zpos,
		m_pPlanes[OSD_PLANE]->plane_id,
		m_pPlanes[OSD_PLANE]->type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "OVERLAY",
		m_pPlanes[OSD_PLANE]->properties.zpos,
		m_useZpos);

#ifdef USE_GLES
	if (m_disableOglOsd)
		return 0;

	// init gbm
	int w, h;
	double pixel_aspect;
	GetScreenSize(&w, &h, &pixel_aspect);

	if (init_gbm(w, h, DRM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING)) {
		LOGERROR("FindDevice: failed to init gbm device and surface!");
		return -1;
	}

	// init egl
	if (InitEGL()) {
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
		ret = drmModeAddFB2WithModifiers(m_fdDrm, buf->width, buf->height, buf->pix_fmt,
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
		ret = drmModeAddFB2(m_fdDrm, buf->width, buf->height, buf->pix_fmt,
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
			if (drmPrimeFDToHandle(m_fdDrm,
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

			if (drmIoctl(m_fdDrm, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0){
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

			if (drmIoctl(m_fdDrm, DRM_IOCTL_MODE_MAP_DUMB, &mreq)){
				LOGERROR("SetupFB: cannot prepare dumb buffer for mapping (%d): %m", errno);
				return -errno;
			}

			buf->plane[plane] = (uint8_t *)mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fdDrm, mreq.offset);

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
	ret = drmModeAddFB2WithModifiers(m_fdDrm, buf->width, buf->height, buf->pix_fmt,
					 buf->handle, buf->pitch, buf->offset, modifier, &buf->fb_id, mod_flags);

	if (ret) {
		if (mod_flags)
			LOGERROR("SetupFB: cannot create modifiers framebuffer (%d): %m", errno);

		ret = drmModeAddFB2(m_fdDrm, buf->width, buf->height, buf->pix_fmt,
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

	if (drmModeRmFB(m_fdDrm, buf->fb_id) < 0)
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

			if (drmIoctl(m_fdDrm, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq) < 0)
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
			if (drmIoctl(m_fdDrm, DRM_IOCTL_GEM_CLOSE, &buf->primehandle[i]) < 0)
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
void cVideoRender::CleanUp(void)
{
	AVFrame *frame;
	int i;

	// first wait for m_pFilterThread to be closed
	if (m_pFilterThread->Active()) {
		LOGDEBUG("CleanUp: cancel filter thread");
		m_pFilterThread->Stop();
	}

	FramesRbLock();
	while (atomic_read(&m_framesFilled)) {
		frame = RbGetFrame();
		av_frame_free(&frame);
	}
	FramesRbUnlock();

	if (m_closing && m_pLastFrame->frame) {
		av_frame_free(&m_pLastFrame->frame);
		m_pLastFrame->trickspeed = 0;
	}

	// Destroy FBs
	for (i = 0; i < RENDERBUFFERS; ++i) {
		if (m_buffer[i].dirty == 0)
			continue;

		if (m_closing || (m_buffer[i].fb_id != m_pLastFrame->buf->fb_id))
			DestroyFB(&m_buffer[i]);
	}

	m_numBuffers = 0;
	m_enqueueBufferIdx = 0;

	m_waitCleanCondition.Signal();
	if (m_flushing)
		m_flushLastFrame = 1;
	m_flushing = 0;
	m_closing = 0;
	m_deintDisabled = m_configDeintDisabled;

	LOGDEBUG("CleanUp: DRM cleaned (m_framesFilled %d m_numFramesToFilter %d)", atomic_read(&m_framesFilled), atomic_read(&m_numFramesToFilter));
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
int cVideoRender::CommitBuffer(struct drm_buf *buf, int skip_video)
{
	int dirty = 0; // 0: no commit, 1: osd only, 2: video only, 3: both
	AVFrame *frame = NULL;

	uint64_t DispWidth = m_drmModeInfo.hdisplay;;
	uint64_t DispHeight = m_drmModeInfo.vdisplay;
	uint64_t DispX = 0;
	uint64_t DispY = 0;
	uint64_t PicWidth = 0;
	uint64_t PicHeight = 0;

	drmModeAtomicReqPtr ModeReq;
	uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
	if (!(ModeReq = drmModeAtomicAlloc())) {
		LOGERROR("DisplayFrame: cannot allocate atomic request (%d): %m", errno);
		return -2;
	}

	if (skip_video)
		goto skip_video;

	if (buf)
		frame = buf->frame;

	// handle the video plane
	// Get video size and position and set crtc rect
	if (m_videoParam.is_scaled) {
		DispWidth = (uint64_t)m_videoParam.width;
		DispHeight = (uint64_t)m_videoParam.height;
		DispX = (uint64_t)m_videoParam.x;
		DispY = (uint64_t)m_videoParam.y;
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

	m_pPlanes[VIDEO_PLANE]->properties.crtc_id = m_crtcId;
	m_pPlanes[VIDEO_PLANE]->properties.fb_id = buf->fb_id;
	m_pPlanes[VIDEO_PLANE]->properties.crtc_x = DispX + (DispWidth - PicWidth) / 2;
	m_pPlanes[VIDEO_PLANE]->properties.crtc_y = DispY + (DispHeight - PicHeight) / 2;
	m_pPlanes[VIDEO_PLANE]->properties.crtc_w = PicWidth;
	m_pPlanes[VIDEO_PLANE]->properties.crtc_h = PicHeight;
	m_pPlanes[VIDEO_PLANE]->properties.src_x = 0;
	m_pPlanes[VIDEO_PLANE]->properties.src_y = 0;
	m_pPlanes[VIDEO_PLANE]->properties.src_w = buf->width;
	m_pPlanes[VIDEO_PLANE]->properties.src_h = buf->height;

	// set dimensions for grab early, because we might skip this at the next frame
	m_grabVideo.SetX(DispX + (DispWidth - PicWidth) / 2);
	m_grabVideo.SetY(DispY + (DispHeight - PicHeight) / 2);
	m_grabVideo.SetWidth(PicWidth);
	m_grabVideo.SetHeight(PicHeight);

	SetPlane(ModeReq, m_pPlanes[VIDEO_PLANE]);
	dirty += 2;


skip_video:
	// handle the osd plane
	// We had draw activity on the osd buffer
	if (m_pBufOsd && m_pBufOsd->dirty) {
		if (m_useZpos) {
			m_pPlanes[VIDEO_PLANE]->properties.zpos = m_osdShown ? m_zposPrimary : m_zposOverlay;
			m_pPlanes[OSD_PLANE]->properties.zpos = m_osdShown ? m_zposOverlay : m_zposPrimary;
			SetPlaneZpos(ModeReq, m_pPlanes[VIDEO_PLANE]);
			SetPlaneZpos(ModeReq, m_pPlanes[OSD_PLANE]);

			LOGDEBUG2(L_DRM, "DisplayFrame: SetPlaneZpos: video->plane_id %d -> zpos %" PRIu64 ", osd->plane_id %d -> zpos %" PRIu64 "",
				m_pPlanes[VIDEO_PLANE]->plane_id, m_pPlanes[VIDEO_PLANE]->properties.zpos,
				m_pPlanes[OSD_PLANE]->plane_id, m_pPlanes[OSD_PLANE]->properties.zpos);
		}

		m_pPlanes[OSD_PLANE]->properties.crtc_id = m_crtcId;
		m_pPlanes[OSD_PLANE]->properties.fb_id = m_pBufOsd->fb_id;
		m_pPlanes[OSD_PLANE]->properties.crtc_x = 0;
		m_pPlanes[OSD_PLANE]->properties.crtc_y = 0;
		m_pPlanes[OSD_PLANE]->properties.crtc_w = m_osdShown ? m_pBufOsd->width : 0;
		m_pPlanes[OSD_PLANE]->properties.crtc_h = m_osdShown ? m_pBufOsd->height : 0;
		m_pPlanes[OSD_PLANE]->properties.src_x = 0;
		m_pPlanes[OSD_PLANE]->properties.src_y = 0;
		m_pPlanes[OSD_PLANE]->properties.src_w = m_osdShown ? m_pBufOsd->width : 0;
		m_pPlanes[OSD_PLANE]->properties.src_h = m_osdShown ? m_pBufOsd->height : 0;

		SetPlane(ModeReq, m_pPlanes[OSD_PLANE]);
		dirty += 1;
		LOGDEBUG2(L_DRM, "DisplayFrame: SetPlane OSD %d (fb = %" PRIu64 ")", m_osdShown, m_pPlanes[OSD_PLANE]->properties.fb_id);
		m_pBufOsd->dirty = 0;
	}

	if (m_startgrab) {
		if (m_pBufOsd && m_osdShown) {
			LOGDEBUG2(L_GRAB, "DisplayFrame: Trigger osd grab arrived");
			struct drm_buf *osdBuf = NULL;
			VideoCloneBuf(&osdBuf, m_pBufOsd);
			m_grabOsd.SetBuf(osdBuf);
			// should be the size on screen
			m_grabOsd.SetX(0);
			m_grabOsd.SetY(0);
			m_grabOsd.SetWidth(m_pBufOsd->width);
			m_grabOsd.SetHeight(m_pBufOsd->height);
		}

		struct drm_buf *pbuf = buf ? buf : (m_pLastFrame->buf ? m_pLastFrame->buf : NULL);
		if (pbuf) {
			LOGDEBUG2(L_GRAB, "DisplayFrame: Trigger video grab arrived");
			struct drm_buf *videoBuf = NULL;
			VideoCloneBuf(&videoBuf, pbuf);
			m_grabVideo.SetBuf(videoBuf);
			// dimensions have already been set earlier
		}
		m_grabCond.Signal();
	}


	// return without an atomic commit (no video frame and osd activity)
	if (!dirty) {
		drmModeAtomicFree(ModeReq);
		return -1;
	}

	// do the atomic commit
	if (drmModeAtomicCommit(m_fdDrm, ModeReq, flags, NULL) != 0) {
		DumpPlaneProperties(m_pPlanes[OSD_PLANE]);
		if (dirty > 1)
			DumpPlaneProperties(m_pPlanes[VIDEO_PLANE]);

		drmModeAtomicFree(ModeReq);
		LOGERROR("DisplayFrame: page flip failed (%d): %m", errno);
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
int cVideoRender::Sync(AVFrame *frame, int *skip_video, struct drm_buf **buf)
{
	int64_t audio_pts;
	int64_t video_pts;

	video_pts = frame->pts * 1000 * av_q2d(*m_timebase);

	if(!m_startCounter && !m_closing) {
		LOGDEBUG("Sync: start PTS %s", Timestamp2String(video_pts));
		m_pAudio->Skip(frame->pts, 0);
avready:
		if (m_pAudio->VideoReady(video_pts)) {
			usleep(10000);

			// check for close/flush request or pause
			if (m_closing) {
				LOGDEBUG2(L_DRM, "DisplayFrame: closing while sync, set a black FB");
				*buf = &m_bufBlack;
				return 1;
			} else if (m_flushing) {
				LOGDEBUG2(L_DRM, "DisplayFrame: flushing while sync, skip video");
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
	if (m_closing) {
		LOGDEBUG2(L_DRM, "DisplayFrame: closing while sync, set a black FB");
		*buf = &m_bufBlack;
		return 1;
	} else if (m_flushing) {
		LOGDEBUG2(L_DRM, "DisplayFrame: flushing while sync, skip video");
		*skip_video = 1;
		return 1;
	} else if (VideoIsPaused()) {
		return 0;
	}

	audio_pts = m_pAudio->GetClock();

	if (audio_pts == (int64_t)AV_NOPTS_VALUE) {
		usleep(20000);
		goto audioclock;
	}

	int diff = video_pts - audio_pts - m_pDevice->GetVideoAudioDelay();

	if (abs(diff) > 5000) {	// more than 5s
		LOGDEBUG2(L_AV_SYNC, "More then 5s Pkts %d deint %d, Frames %d UsedBytes %d audio %s video %s Delay %dms diff %dms",
			m_pDevice->VideoStream->GetPacketsFilled(), m_pFilterThread->GetFramesDeintFilled(),
			atomic_read(&m_framesFilled), m_pAudio->GetUsedBytes(), Timestamp2String(audio_pts),
			Timestamp2String(video_pts), m_pDevice->GetVideoAudioDelay(), diff);
	}

	if (diff < -5 && !(abs(diff) > 5000)) {	// video is more than 5ms behind audio, drop video frame
		LOGDEBUG2(L_AV_SYNC, "FrameDropped (drop %d, dup %d) Pkts %d deint %d Frames %d UsedBytes %d audio %s video %s Delay %dms diff %dms",
			m_framesDropped, m_framesDuped,
			m_pDevice->VideoStream->GetPacketsFilled(), m_pFilterThread->GetFramesDeintFilled(),
			atomic_read(&m_framesFilled), m_pAudio->GetUsedBytes(), Timestamp2String(audio_pts),
			Timestamp2String(video_pts), m_pDevice->GetVideoAudioDelay(), diff);

		if (!m_startCounter)
			m_startCounter++;

		m_framesDropped++;
		return -1;
	}

	if (diff > 35 && !(abs(diff) > 5000)) {	// audio is more than 35ms behind video, duplicate video frame
		LOGDEBUG2(L_AV_SYNC, "FrameDuped (drop %d, dup %d) Pkts %d deint %d Frames %d UsedBytes %d audio %s video %s Delay %dms diff %dms",
			m_framesDropped, m_framesDuped,
			m_pDevice->VideoStream->GetPacketsFilled(), m_pFilterThread->GetFramesDeintFilled(),
			atomic_read(&m_framesFilled), m_pAudio->GetUsedBytes(), Timestamp2String(audio_pts),
			Timestamp2String(video_pts), m_pDevice->GetVideoAudioDelay(), diff);

		m_framesDuped++;
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
int cVideoRender::GetFrame(AVFrame **frame)
{
	AVFrame *pframe = NULL;

	pframe = RbGetFrame();
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
int cVideoRender::GetBuffer(AVFrame *frame, struct drm_buf **buf)
{
	struct drm_buf *pbuf = NULL;
	int i;

	AVDRMFrameDescriptor *primedata = (AVDRMFrameDescriptor *)frame->data[0];
	FrameData *fd = (FrameData *)frame->opaque_ref->data;

	// search for a made fd / FB combination
	for (i = 0; i < RENDERBUFFERS; i++) {
		if (m_flushLastFrame && !m_buffer[i].swbuffer)
			break;

		if (m_buffer[i].trickspeed && !m_buffer[i].swbuffer)
			break;

		if (m_buffer[i].dirty == 0)
			continue;

		if (m_buffer[i].fd_prime[0] == primedata->objects[0].fd) {
			pbuf = &m_buffer[i];
			break;
		}
	}

	// search for a "free" buffer
	if (pbuf == 0) {
		for (i = 0; i < RENDERBUFFERS; i++) {
			if (m_buffer[i].dirty == 0)
				break;
		}
		if (m_buffer[i].dirty) {
			LOGDEBUG("GetBuffer: SHOULD NOT HAPPEN! no free buffer available!");
			return 1;
		}

		pbuf = &m_buffer[i];;

		pbuf->width = (uint32_t)frame->width;
		pbuf->height = (uint32_t)frame->height;

		if (SetupFB(pbuf, primedata))
			return 1;

		m_buffer[i].swbuffer = 0;
	}

	if (pbuf == 0) {
		LOGDEBUG("GetBuffer: SHOULD NOT HAPPEN! failed, no buffer found!");
		return 1;
	}

	pbuf->trickspeed = fd->flags & FRAME_FLAG_TRICKSPEED;

	*buf = pbuf;
	return 0;
}

///
///	Check if we should wait for audio to come up with video
///
///	retval 0	wait for video to sync with audio
///	retval 1	wait for audio to sync with video
///
///
int cVideoRender::ShouldWaitForAudio(void) {
	int64_t audio_pts = m_pAudio->GetClock();
	int64_t video_pts = GetVideoClock() * 1000 * av_q2d(*m_timebase);
	if (video_pts == AV_NOPTS_VALUE || audio_pts == AV_NOPTS_VALUE)
		return 1;

	int diff = video_pts - audio_pts - m_pDevice->GetVideoAudioDelay();
	// audio is behind video, wait for audio
	if (diff > 0)
		return 1;

	// video is behind audio, so don't wait
	return 0;
}

///
///	Draw a video frame.
///
//	retval 0	modesetting and commit was done, need to process outstanding DRM events
//	retval 1	no new frames or OSD, no modesetting was done, don't process outstanding DRM events
//
///
int cVideoRender::DisplayFrame(void)
{
	struct drm_buf *buf = NULL;
	AVFrame *frame = NULL;
	int skip_video = 0;
	int timeout; // ms
	int ret;
	FrameData *fd;

	if (ShouldClose()) {
		LOGDEBUG2(L_DRM, "DisplayFrame: closing, set a black FB");
		buf = &m_bufBlack;
		goto page_flip;
	}

	if (ShouldFlush()) {
		LOGDEBUG2(L_DRM, "DisplayFrame: flushing, just skip video");
		skip_video = 1;
		goto page_flip;
	}

dequeue:
	timeout = 15; // ms
	// wait for a frame in the ringbuffer
	while (!atomic_read(&m_framesFilled)) {
		if (m_exitThread) {
			LOGDEBUG2(L_DRM, "DisplayFrame -> Exit Thread");
			return 1;
		}

		if (ShouldClose()) {
			LOGDEBUG2(L_DRM, "DisplayFrame: closing, set a black FB");
			buf = &m_bufBlack;
			goto page_flip;
		}

		if (ShouldFlush()) {
			LOGDEBUG2(L_DRM, "DisplayFrame: flushing, just skip video");
			skip_video = 1;
			goto page_flip;
		}

		if (VideoIsPaused()) {
			usleep(10000);
			// LOGDEBUG2(L_DRM, "DisplayFrame: paused, skip video");
			skip_video = 1;
			goto page_flip;			
		}

		// wait max. 15ms in case we have an osd
		if (m_pBufOsd && m_pBufOsd->dirty && !timeout--) {
			skip_video = 1;
			LOGDEBUG2(L_DRM, "DisplayFrame: no video but osd, skip video");
			goto page_flip;
		}

		usleep(1000);
	}

	// if the video is paused, lets wait all remaining audio
	// this is necessary for a correct Play() after Pause()
	if (VideoIsPaused() && ShouldWaitForAudio()) {
		usleep(10000);
		skip_video = 1;
		goto page_flip;
	}

	// advance frame
	if (GetFrame(&frame)) {
		FrameData *fd = (FrameData *)frame->opaque_ref->data;
		if (fd->flags & FRAME_FLAG_STILLPICTURE) {
			LOGDEBUG2(L_STILL, "DisplayFrame: Stillpicture has AV_NOPTS_VALUE, skip sync ...");
			goto skip_sync;
		} else {
			// TODO: fast/soft sync
			LOGDEBUG2(L_DRM, "DisplayFrame: no AV_NOPTS_VALUE, use next frame ...");
			av_frame_free(&frame);
			return 1;
		}
	}

	fd = (FrameData *)frame->opaque_ref->data;

	// skip old audio in trickspeed
	if (fd->flags & FRAME_FLAG_TRICKSPEED) {
		m_pAudio->Skip(frame->pts, 0);
		goto skip_sync;
	}

	// skip old audio after trickspeed
	if (m_pLastFrame->frame && m_pLastFrame->trickspeed) {
		m_pAudio->Skip(frame->pts, 1);
		goto skip_sync;
	}

	// sync audio/video
	ret = Sync(frame, &skip_video, &buf);

	if (ret < 0) { // drop frame (dup is handled within Sync())
		av_frame_free(&frame);
		goto dequeue;
	} else if (ret) { // close or flush (black buffer or skip video)
		av_frame_free(&frame);
		goto page_flip;
	}

	m_startCounter++;

skip_sync:
	// get suitable framebuffer
	if (GetBuffer(frame, &buf)) {
		av_frame_free(&frame);
		return 1;
	}

	buf->frame = frame;

page_flip:
	// no modesetting was done
	if (CommitBuffer(buf, skip_video) < 0) {
		if (frame)
			av_frame_free(&frame);
		return 1;
	}

	// only osd was set
	if (skip_video)
		return 0;

	// now, that we had a successful commit, set the STC if we have a frame
	if (frame)
		SetVideoClock(frame->pts);

	if (frame)
		LOGDEBUG2(L_PACKET, "DisplayFrame:                 PTS %s", Timestamp2String(frame->pts / 90));

	// new video frame was sent, rotate the frames
	if (m_pLastFrame->frame) {
		// if the m_pLastFrame was a trickframe or a flush is forced, destroy the FB
		if (m_flushLastFrame || m_pLastFrame->trickspeed) {
			DestroyFB(m_pLastFrame->buf);
			m_pLastFrame->trickspeed = 0;
			m_flushLastFrame = 0;
		}
		av_frame_free(&m_pLastFrame->frame);
	}

	if (buf && buf->fb_id != m_bufBlack.fb_id) {
		m_pLastFrame->frame = buf->frame;
		m_pLastFrame->buf = buf;
		m_pLastFrame->trickspeed = buf->trickspeed;
	}

	return 0;
}

int cVideoRender::DrmHandleEvent(void)
{
    return drmHandleEvent(m_fdDrm, &m_drmEventCtx);
}

//----------------------------------------------------------------------------
//	OSD
//----------------------------------------------------------------------------

///
///	Clear the OSD.
///
///
void cVideoRender::OsdClear(void)
{
#ifdef USE_GLES
	if (m_disableOglOsd) {
		memset((void *)m_pBufOsd->plane[0], 0,
			(size_t)(m_pBufOsd->pitch[0] * m_pBufOsd->height));
	} else {
		struct drm_buf *buf;

		EGL_CHECK(eglSwapBuffers(m_eglDisplay, m_eglSurface));
		m_pNextBo = gbm_surface_lock_front_buffer(m_pGbmSurface);
		assert(m_pNextBo);

		buf = drm_get_buf_from_bo(m_pNextBo);
		if (!buf) {
			LOGERROR("Failed to get GL buffer");
			return;
		}

		m_pBufOsd = buf;

		// release old buffer for writing again
		if (m_bo)
			gbm_surface_release_buffer(m_pGbmSurface, m_bo);

		// rotate bos and create and keep bo as m_pOldBo to make it free'able
		m_pOldBo = m_bo;
		m_bo = m_pNextBo;

		LOGDEBUG2(L_OPENGL, "OsdClear(GL): eglSwapBuffers m_eglDisplay %p eglSurface %p (%i x %i, %i)", m_eglDisplay, m_eglSurface, buf->width, buf->height, buf->pitch[0]);
	}
#else
	memset((void *)m_pBufOsd->plane[0], 0,
		(size_t)(m_pBufOsd->pitch[0] * m_pBufOsd->height));
#endif

	m_pBufOsd->dirty = 1;
	m_osdShown = 0;
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
void cVideoRender::OsdDrawARGB(int xi, int yi,
		int width, int height, int pitch,
		const uint8_t * argb, int x, int y)
{
#ifdef USE_GLES
	if (m_disableOglOsd) {
		LOGDEBUG2(L_OSD, "VideoOsdDrawARGB width %d height %d pitch %d argb %p x %d y %d pitch buf %d xi %d yi %d",
		       width, height, pitch, argb, x, y, m_pBufOsd->pitch[0], xi, yi);
		for (int i = 0; i < height; ++i) {
			memcpy(m_pBufOsd->plane[0] + x * 4 + (i + y) * m_pBufOsd->pitch[0],
				argb + i * pitch, MIN((size_t)pitch, m_pBufOsd->pitch[0]));
		}
	} else {
		struct drm_buf *buf;

		EGL_CHECK(eglSwapBuffers(m_eglDisplay, m_eglSurface));
		m_pNextBo = gbm_surface_lock_front_buffer(m_pGbmSurface);
		assert(m_pNextBo);

		buf = drm_get_buf_from_bo(m_pNextBo);
		if (!buf) {
			LOGERROR("Failed to get GL buffer");
			return;
		}

		m_pBufOsd = buf;

		// release old buffer for writing again
		if (m_bo)
			gbm_surface_release_buffer(m_pGbmSurface, m_bo);

		// rotate bos and create and keep bo as m_pOldBo to make it free'able
		m_pOldBo = m_bo;
		m_bo = m_pNextBo;

		LOGDEBUG2(L_OPENGL, "OsdDrawARGB(GL): eglSwapBuffers eglDisplay %p eglSurface %p (%i x %i, %i)", m_eglDisplay, m_eglSurface, buf->width, buf->height, buf->pitch[0]);
	}
#else
	for (int i = 0; i < height; ++i) {
		memcpy(m_pBufOsd->plane[0] + x * 4 + (i + y) * m_pBufOsd->pitch[0],
			argb + i * pitch, (size_t)pitch);
	}
#endif
	m_pBufOsd->dirty = 1;
	m_osdShown = 1;
}

//----------------------------------------------------------------------------
//	Thread
//----------------------------------------------------------------------------

///
///
///
void cVideoRender::ExitDecodingThread(void)
{
	LOGDEBUG("%s", __FUNCTION__);

	if (m_pDecodingThread->Active())
		m_pDecodingThread->Stop();
}

///
///
///
void cVideoRender::ExitDisplayThread(void)
{
	LOGDEBUG("%s", __FUNCTION__);

	SetClosing(1);
	if (m_pDisplayThread->Active()) {
		m_exitThread = 1;
		m_pDisplayThread->Stop();
	}
}

///
///	Wakeup display thread
///
///	New video arrived, wakeup video thread.
///
void cVideoRender::WakeupDisplayThread(void)
{
	LOGDEBUG("%s", __FUNCTION__);
	if (!m_pDisplayThread->Active())
		m_pDisplayThread->Start();
}

///
///	Wakeup decoding thread
///
///	New video arrived, wakeup video thread.
///
void cVideoRender::WakeupDecodingThread(void)
{
	LOGDEBUG("%s", __FUNCTION__);
	if (!m_pDecodingThread->Active())
		m_pDecodingThread->Start();
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
    m_pDevice = device;
    m_pAudio = m_pDevice->Audio;
}

///
///	Destroy a video render.
///
///	@param render	video render
///
cVideoRender::~cVideoRender(void)
{
	LOGDEBUG2(L_DRM, "~cVideoRender");
	if (m_pFilterThread)
		delete m_pFilterThread;
	if (m_pDisplayThread)
		delete m_pDisplayThread;
	if (m_pDecodingThread)
		delete m_pDecodingThread;
	free(m_pLastFrame);
	LOGDEBUG2(L_DRM, "~cVideoRender deleted");
}

void cVideoRender::StartThreads(void)
{
	m_pDecodingThread = new cDecodingThread(m_pDevice);
	m_pDisplayThread = new cDisplayThread(this);
	m_pFilterThread = new cFilterThread(this);

	atomic_set(&m_framesFilled, 0);
	m_closing = 0;
	m_flushing = 0;
	m_flushLastFrame = 0;
	m_deintDisabled = m_configDeintDisabled;
	m_enqueueBufferIdx = 0;
	m_pLastFrame = (struct lastFrame *)calloc(1, sizeof(struct lastFrame));
	ResumeVideo();
}


void cVideoRender::EnqueueFB(AVFrame *inframe)
{
	// inframe->format is always NV12!
	struct drm_buf *buf = 0;

	AVDRMFrameDescriptor * primedata;
	AVFrame *frame;
	int i;
	FrameData *ifd = (FrameData *)inframe->opaque_ref->data;

	if (ifd->flags & FRAME_FLAG_TRICKSPEED) {	// if we have trickspeed, always use a free buffer, because its destroyed in DisplayFrame after rendering
		for (i = 0; i < RENDERBUFFERS; i++) {
			if (m_buffer[i].dirty == 0)
				break;
		}
		if (m_buffer[i].dirty)
			LOGFATAL("EnqueueFB: SHOULD NOT HAPPEN! no free buffer available!");

		buf = &m_buffer[i];
		buf->width = (uint32_t)inframe->width;
		buf->height = (uint32_t)inframe->height;
		buf->pix_fmt = DRM_FORMAT_NV12;

		if (SetupFB(buf, NULL)) {
			LOGERROR("EnqueueFB: SetupFB FB %i x %i failed",
				buf->width, buf->height);
		}
		if (drmPrimeHandleToFD(m_fdDrm, buf->handle[0], DRM_CLOEXEC | DRM_RDWR, &buf->fd_prime[0]))
			LOGERROR("EnqueueFB: Failed to retrieve the Prime FD (%d): %m", errno);
	} else {
		// create some buffers up to VIDEO_SURFACES_MAX + 2
get_buffer:
		buf = &m_buffer[m_enqueueBufferIdx];
		if (m_numBuffers < VIDEO_SURFACES_MAX + 2) {
			if (buf->dirty) {
				// skip the buffer, because it is either referenced by m_pLastFrame or is already setup
				// this should be safe, because we only have 1 m_pLastFrame, which should be destroyed as soon as
				// a new buffer arrives in DisplayFrame
				// after that destroy, we should be able to setup 0, 1, 2, ..., VIDEO_SURFACES_MAX + 2 framebuffers
				m_enqueueBufferIdx = (m_enqueueBufferIdx + 1) % (VIDEO_SURFACES_MAX + 2);

				goto get_buffer;
			}

			buf->width = (uint32_t)inframe->width;
			buf->height = (uint32_t)inframe->height;
			buf->pix_fmt = DRM_FORMAT_NV12;

			if (SetupFB(buf, NULL)) {
				LOGERROR("EnqueueFB: SetupFB FB %i x %i failed",
					buf->width, buf->height);
			}

			m_numBuffers++;

			if (drmPrimeHandleToFD(m_fdDrm, buf->handle[0], DRM_CLOEXEC | DRM_RDWR, &buf->fd_prime[0]))
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

	if (m_closing || m_flushing) {
		av_frame_free(&inframe);
		av_frame_free(&frame);
		return;
	}

	FramesRbLock();
	RbPushFrame(frame);
	FramesRbUnlock();

	if (!(ifd->flags & FRAME_FLAG_TRICKSPEED))
		m_enqueueBufferIdx = (m_enqueueBufferIdx + 1) % (VIDEO_SURFACES_MAX + 2);

	av_frame_free(&inframe);
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
int cVideoRender::RenderFrame(AVCodecContext * video_ctx, AVFrame * frame, int flags)
{
	int interlaced;

	if (!m_startCounter) {
		m_timebase = &video_ctx->pkt_timebase;
	}

	if (frame->decode_error_flags || frame->flags & AV_FRAME_FLAG_CORRUPT) {
		LOGWARNING("RenderFrame: error_flag or FRAME_FLAG_CORRUPT");
	}

	FrameData *fd;
	if (!frame->opaque_ref) {
		frame->opaque_ref = av_buffer_allocz(sizeof(*fd));
		if (!frame->opaque_ref)
			LOGFATAL("RenderFrame: cannot allocate private frame data");
	}
	fd = (FrameData *)frame->opaque_ref->data;
	fd->flags = flags;

	if (m_closing || m_flushing) {
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
		m_pDevice->VideoStream->SetInterlaced(interlaced);

	if (frame->format == AV_PIX_FMT_YUV420P ||
	   (frame->format == AV_PIX_FMT_DRM_PRIME && interlaced && !((m_hardwareQuirks & QUIRK_NO_HW_DEINT) || m_deintDisabled))) {
		// use deinterlace/scale filter
		// AV_PIX_FMT_YUV420P, interlaced -> software deinterlacer (bwdif filter)
		// AV_PIX_FMT_YUV420P, progressive -> scale filter to get NV12 frames
		// AV_PIX_FMT_DRM_PRIME, interlaced, hw deinterlacer available -> hw deinterlacer
		// -> put the frame into filter Rb
		if (!m_pFilterThread->Active()) {
			LOGDEBUG("RenderFrame: wakeup filter thread");
			if (m_pFilterThread->Init(video_ctx, frame, m_deintDisabled)) {
				av_frame_free(&frame);
				return 0;
			} else {
				m_pFilterThread->Start();
			}
		}

		if (m_pFilterThread->GetFramesDeintFilled() < VIDEO_SURFACES_MAX) {
			m_pFilterThread->RbPushFrame(frame);
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
			if (m_closing || m_flushing) {
				av_frame_free(&frame);
				return 0;
			}
			FramesRbLock();
			if (atomic_read(&m_framesFilled) < VIDEO_SURFACES_MAX && !m_numFramesToFilter) {
				RbPushFrame(frame);
				FramesRbUnlock();
			} else {
				FramesRbUnlock();
				usleep(1000);
				return -1;
			}
		} else {
			// AV_PIX_FMT_DRM_NV12 ?
			// -> go through EnqueueFB
			FramesRbLock();
			if (atomic_read(&m_framesFilled) < VIDEO_SURFACES_MAX) {
				FramesRbUnlock();
				EnqueueFB(frame);
			} else {
				FramesRbUnlock();
				usleep(5000);
				return -1;
			}
		}
	}
	return 0;
}

void cVideoRender::FramesRbLock(void) {
	m_displayQueue.Lock();
}

void cVideoRender::FramesRbUnlock(void) {
	m_displayQueue.Unlock();
}

void cVideoRender::RbPushFrame(AVFrame * frame) {
	m_framesRb[m_framesWrite] = frame;
	m_framesWrite = (m_framesWrite + 1) % VIDEO_SURFACES_MAX;
	atomic_inc(&m_framesFilled);
}

AVFrame *cVideoRender::RbGetFrame(void) {
	AVFrame *frame = m_framesRb[m_framesRead];
	m_framesRead = (m_framesRead + 1) % VIDEO_SURFACES_MAX;
	atomic_dec(&m_framesFilled);

	return frame;
}

///
///	Set video clock.
///
void cVideoRender::SetVideoClock(int64_t pts)
{
	m_videoClockMutex.Lock();
	m_pts = pts;
	m_videoClockMutex.Unlock();
}

///
///	Get video clock.
///
///	@param hw_decoder	video hardware decoder
///
///	@note this isn't monoton, decoding reorders frames, setter keeps it
///	monotonic
///
int64_t cVideoRender::GetVideoClock(void)
{
	int64_t pts;
	m_videoClockMutex.Lock();
	pts = m_pts;
	m_videoClockMutex.Unlock();
	return pts;
}

///
///	send start condition to video thread.
///
///	@param hw_render	video hardware render
///
void cVideoRender::StartVideo(void)
{
	ResumeVideo();
	m_startCounter = 0;
	LOGDEBUG("StartVideo: reset m_startCounter %d Closing %d TrickSpeed %d",
		m_startCounter, m_closing, GetTrickSpeed());
}

///
///	Close the renderer and clear frames (and) framebuffers if needed
///
///	@param hw_render	video hardware render
///	@param black		true, if should set a black fb and clear the last framebuffer,
///				otherwise wait for the clear until the next frame arrives
///
void cVideoRender::SetClosing(int black)
{
	LOGDEBUG("SetClosing: m_startCounter %d%s", m_startCounter, black ? " closing": " flushing");
	if (!m_pDisplayThread->Active())
		return;

	if (m_pFilterThread->Active())
		m_pFilterThread->Stop();
	m_flushing = !black;
	m_closing = black;

	if (VideoIsPaused())
		ResumeVideo();

	LOGDEBUG("SetClosing: wait for cleanup");
	if (!m_waitCleanCondition.Wait(2000)) {
		LOGERROR("%s: timeout while waiting for cleanup");
	}

	m_startCounter = 0;
	m_framesDuped = 0;
	m_framesDropped = 0;
	if (black)
		SetTrickSpeed(0, 1);
}

///
//	Pause video.
//
void cVideoRender::PauseVideo(void)
{
	LOGDEBUG("PauseVideo:");
	m_playbackMutex.Lock();
	m_videoIsPaused = 1;
	m_playbackMutex.Unlock();
}

///
//	Resume video.
//
void cVideoRender::ResumeVideo(void)
{
	LOGDEBUG("ResumeVideo:");
	m_playbackMutex.Lock();
	m_videoIsPaused = 0;
	m_playbackMutex.Unlock();
}

///
//	Check pause status
//
int cVideoRender::VideoIsPaused(void)
{
	int ret;
	m_playbackMutex.Lock();
	ret = m_videoIsPaused;
	m_playbackMutex.Unlock();
	return ret;
}

///
///	Simply set trick play speed values
///
///	@param hw_render	video hardware render
///	@param speed		trick speed (0 = normal)
///	@param forward		1 if forward trick speed
///
void cVideoRender::SetTrickSpeed(int speed, int forward)
{
	LOGDEBUG2(L_TRICK, "SetTrickSpeed: set trick speed %d %s", speed, forward ? "forward" : "backward");
	m_trickspeedMutex.Lock();
	m_trickSpeed = speed;
	m_trickCounter = speed;
	m_trickForward = forward;
	m_trickspeedMutex.Unlock();
}

///
///	Return the current trick speed mode
///
int cVideoRender::GetTrickSpeed(void)
{
	int speed;
	m_trickspeedMutex.Lock();
	speed = m_trickSpeed;
	m_trickspeedMutex.Unlock();
	return speed;
}

///
///	Return the current trick speed direction
///
int cVideoRender::GetTrickForward(void)
{
	int dir;
	m_trickspeedMutex.Lock();
	dir = m_trickForward;
	m_trickspeedMutex.Unlock();
	return dir;
}

///
///	Return the current trick counter
///
int cVideoRender::GetTrickCounter(void)
{
	int counter;
	m_trickspeedMutex.Lock();
	counter = m_trickCounter;
	m_trickspeedMutex.Unlock();
	return counter;
}

///
///	Set the trick counter
///
void cVideoRender::SetTrickCounter(int counter)
{
	m_trickspeedMutex.Lock();
	m_trickCounter = counter;
	m_trickspeedMutex.Unlock();
}

///
///	Decrease the trick counter
///
int cVideoRender::DecTrickCounter(void)
{
	int counter;
	m_trickspeedMutex.Lock();
	m_trickCounter--;
	counter = m_trickCounter;
	m_trickspeedMutex.Unlock();
	return counter;
}

///
///	Trigger grabbing in render thread
///
///
void cVideoRender::TriggerGrab(cCondWait *wait)
{
	m_startgrab = 1;
	m_grabCond.Wait(2000);
	m_startgrab = 0;
	wait->Signal();
}

///
///	Grab full screen image
///
///	@param grabimage[out]	the struct to grab in
///	@param buf[in]		current video buffer
///	@param ready[out]	ready is set true if we finished
///	@param is_osd		is this an osd grab? (just for logs)
///
void cVideoRender::ConvertVideoBufToRgb(void)
{
	int size;
	cSoftHdGrab *grab = &m_grabVideo;
	struct drm_buf *buf = grab->GetBuf();

	// early return if buf = NULL
	if (!buf) {
		grab->SetData(NULL);
		grab->SetSize(0);
		return;
	}

	for (int plane = 0; plane < buf->num_planes; plane++) {
		LOGDEBUG2(L_GRAB, "ConvertVideoBufToRgb: VIDEO plane %d address %p pitch %d offset %d handle %d size %d",
		       plane, buf->plane[plane], buf->pitch[plane], buf->offset[plane], buf->handle[plane], buf->size[plane]);
	}
	// result's width and height are original dimensions how buffer is presented on the screen
	uint8_t * result = buf2rgb(buf, &size, grab->GetWidth(), grab->GetHeight(), AV_PIX_FMT_RGB24);
	grab->SetData(result);
	grab->SetSize(size);
	grab->FreeBuf();

	return;
}

void cVideoRender::ConvertOsdBufToRgb(void)
{
	int size;
	cSoftHdGrab *grab = &m_grabOsd;
	struct drm_buf *buf = grab->GetBuf();

	// early return if buf = NULL
	if (!buf) {
		grab->SetData(NULL);
		grab->SetSize(0);
		return;
	}

	for (int plane = 0; plane < buf->num_planes; plane++) {
		LOGDEBUG2(L_GRAB, "VideoGrab: OSD plane %d address %p pitch %d offset %d handle %d size %d",
		       plane, buf->plane[plane], buf->pitch[plane], buf->offset[plane], buf->handle[plane], buf->size[plane]);
	}
	// result's width and height are original dimensions how buffer is presented on the screen
	uint8_t * result = buf2rgb(buf, &size, grab->GetWidth(), grab->GetHeight(), AV_PIX_FMT_BGRA);
	grab->SetData(result);
	grab->SetSize(size);
	grab->FreeBuf();

	return;
}

///
///	Clear grab buffers
///
///
void cVideoRender::ClearGrab(void)
{
	if (m_grabOsd.GetBuf())
		m_grabOsd.FreeBuf();
	if (m_grabVideo.GetBuf())
		m_grabVideo.FreeBuf();
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
cSoftHdGrab *cVideoRender::GetGrab(int *size, int *width, int *height, int *x, int *y, int is_osd)
{
	cSoftHdGrab *grab;
	if (is_osd)
		grab = &m_grabOsd;
	else
		grab = &m_grabVideo;

	LOGDEBUG2(L_GRAB, "GetGrab: %s size %d %dx%d at %d|%d %p", is_osd ? "OSD" : "VIDEO", grab->GetSize(), grab->GetWidth(), grab->GetHeight(), grab->GetX(), grab->GetY(), grab->GetData());

	if (size)
		*size = grab->GetSize();
	if (width)
		*width = grab->GetWidth();
	if (height)
		*height = grab->GetHeight();
	if (x)
		*x = grab->GetX();
	if (y)
		*y = grab->GetY();

	return grab;
}

///
///	Get render statistics.
///
///	@param hw_render	video hardware render
///	@param[out] duped	duped frames
///	@param[out] dropped	dropped frames
///	@param[out] count	number of decoded frames
///
void cVideoRender::GetStats(int *duped, int *dropped, int *counter)
{
    *duped = m_framesDuped;
    *dropped = m_framesDropped;
    *counter = m_startCounter;
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
void cVideoRender::GetScreenSize(int *width, int *height, double *pixel_aspect)
{
	*width = m_drmModeInfo.hdisplay;
	*height = m_drmModeInfo.vdisplay;
	*pixel_aspect = (double)16 / (double)9;
}

///
///	Initialize video output module.
///
void cVideoRender::Init(void)
{
	unsigned int i;

	if (FindDevice()){
		LOGFATAL("VideoInit: FindDevice() failed");
	}

	ReadHWPlatform();

	m_buffer[0].width = m_buffer[1].width = 0;
	m_buffer[0].height = m_buffer[1].height = 0;
	m_buffer[0].pix_fmt = m_buffer[1].pix_fmt = DRM_FORMAT_NV12;

	// osd FB
#ifndef USE_GLES
	if (!m_pBufOsd)
		m_pBufOsd = calloc(1, sizeof(struct drm_buf));
	m_pBufOsd->pix_fmt = DRM_FORMAT_ARGB8888;
	m_pBufOsd->width = m_drmModeInfo.hdisplay;
	m_pBufOsd->height = m_drmModeInfo.vdisplay;
	if (SetupFB(m_pBufOsd, NULL)){
		LOGFATAL("VideoOsdInit: SetupFB FB OSD failed!");
	}
#else
	if (m_disableOglOsd) {
		if (!m_pBufOsd)
			m_pBufOsd = (struct drm_buf *)calloc(1, sizeof(struct drm_buf));
		m_pBufOsd->pix_fmt = DRM_FORMAT_ARGB8888;
		m_pBufOsd->width = m_drmModeInfo.hdisplay;
		m_pBufOsd->height = m_drmModeInfo.vdisplay;
		if (SetupFB(m_pBufOsd, NULL)){
			LOGFATAL("VideoOsdInit: SetupFB FB OSD failed!");
		}
	}
#endif

	// black fb
	m_bufBlack.pix_fmt = DRM_FORMAT_NV12;
	m_bufBlack.width = m_drmModeInfo.hdisplay;
	m_bufBlack.height = m_drmModeInfo.vdisplay;
	LOGDEBUG2(L_DRM, "Videoinit: Try to create a black FB");
	if (SetupFB(&m_bufBlack, NULL))
		LOGERROR("VideoInit: SetupFB black FB %i x %i failed",
			m_bufBlack.width, m_bufBlack.height);

	for (i = 0; i < m_bufBlack.width * m_bufBlack.height; ++i) {
		m_bufBlack.plane[0][i] = 0x10;
		if (i < m_bufBlack.width * m_bufBlack.height / 2)
		m_bufBlack.plane[1][i] = 0x80;
	}

	// save actual modesetting
	m_drmModeCrtcSaved = drmModeGetCrtc(m_fdDrm, m_crtcId);

	drmModeAtomicReqPtr ModeReq;
	const uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	uint32_t modeID = 0;

	if (drmModeCreatePropertyBlob(m_fdDrm, &m_drmModeInfo, sizeof(m_drmModeInfo), &modeID) != 0)
		LOGERROR("Failed to create mode property blob.");
	if (!(ModeReq = drmModeAtomicAlloc()))
		LOGERROR("cannot allocate atomic request (%d): %m", errno);

	SetPropertyRequest(ModeReq, m_fdDrm, m_crtcId,
						DRM_MODE_OBJECT_CRTC, "MODE_ID", modeID);
	SetPropertyRequest(ModeReq, m_fdDrm, m_connectorId,
						DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", m_crtcId);
	SetPropertyRequest(ModeReq, m_fdDrm, m_crtcId,
						DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);

	// Osd plane
	// We don't have the m_pBufOsd for OpenGL yet, so we can't set anything. Set src and FbId later when osd was drawn,
	// but initially move the OSD behind the VIDEO
#ifndef USE_GLES
	m_pPlanes[OSD_PLANE]->properties.crtc_id = m_crtcId;
	m_pPlanes[OSD_PLANE]->properties.fb_id = m_pBufOsd->fb_id;
	m_pPlanes[OSD_PLANE]->properties.crtc_x = 0;
	m_pPlanes[OSD_PLANE]->properties.crtc_y = 0;
	m_pPlanes[OSD_PLANE]->properties.crtc_w = m_drmModeInfo.hdisplay;
	m_pPlanes[OSD_PLANE]->properties.crtc_h = m_drmModeInfo.vdisplay;
	m_pPlanes[OSD_PLANE]->properties.src_x = 0;
	m_pPlanes[OSD_PLANE]->properties.src_y = 0;
	m_pPlanes[OSD_PLANE]->properties.src_w = m_pBufOsd->width;
	m_pPlanes[OSD_PLANE]->properties.src_h = m_pBufOsd->height;

	SetPlane(ModeReq, m_pPlanes[OSD_PLANE]);
#else
	if (m_disableOglOsd) {
		m_pPlanes[OSD_PLANE]->properties.crtc_id = m_crtcId;
		m_pPlanes[OSD_PLANE]->properties.fb_id = m_pBufOsd->fb_id;
		m_pPlanes[OSD_PLANE]->properties.crtc_x = 0;
		m_pPlanes[OSD_PLANE]->properties.crtc_y = 0;
		m_pPlanes[OSD_PLANE]->properties.crtc_w = m_drmModeInfo.hdisplay;
		m_pPlanes[OSD_PLANE]->properties.crtc_h = m_drmModeInfo.vdisplay;
		m_pPlanes[OSD_PLANE]->properties.src_x = 0;
		m_pPlanes[OSD_PLANE]->properties.src_y = 0;
		m_pPlanes[OSD_PLANE]->properties.src_w = m_pBufOsd->width;
		m_pPlanes[OSD_PLANE]->properties.src_h = m_pBufOsd->height;

		SetPlane(ModeReq, m_pPlanes[OSD_PLANE]);
	}
#endif
	if (m_useZpos) {
		m_pPlanes[VIDEO_PLANE]->properties.zpos = m_zposOverlay;
		SetPlaneZpos(ModeReq, m_pPlanes[VIDEO_PLANE]);
#ifdef USE_GLES
		m_pPlanes[OSD_PLANE]->properties.zpos = m_zposPrimary;
		SetPlaneZpos(ModeReq, m_pPlanes[OSD_PLANE]);
#endif
	}

	m_pPlanes[VIDEO_PLANE]->properties.crtc_id = m_crtcId;
	m_pPlanes[VIDEO_PLANE]->properties.fb_id = m_bufBlack.fb_id;
	m_pPlanes[VIDEO_PLANE]->properties.crtc_x = 0;
	m_pPlanes[VIDEO_PLANE]->properties.crtc_y = 0;
	m_pPlanes[VIDEO_PLANE]->properties.crtc_w = m_drmModeInfo.hdisplay;
	m_pPlanes[VIDEO_PLANE]->properties.crtc_h = m_drmModeInfo.vdisplay;
	m_pPlanes[VIDEO_PLANE]->properties.src_x = 0;
	m_pPlanes[VIDEO_PLANE]->properties.src_y = 0;
	m_pPlanes[VIDEO_PLANE]->properties.src_w = m_bufBlack.width;
	m_pPlanes[VIDEO_PLANE]->properties.src_h = m_bufBlack.height;

	// Black Buffer for video plane
	SetPlane(ModeReq, m_pPlanes[VIDEO_PLANE]);

	if (drmModeAtomicCommit(m_fdDrm, ModeReq, flags, NULL) != 0) {
#ifndef USE_GLES
		DumpPlaneProperties(m_pPlanes[OSD_PLANE]);
#endif
		DumpPlaneProperties(m_pPlanes[VIDEO_PLANE]);

		drmModeAtomicFree(ModeReq);
		LOGFATAL("VideoInit: cannot set atomic mode (%d): %m", errno);
	}

	drmModeAtomicFree(ModeReq);

	m_osdShown = 0;

	// init variables page flip
	memset(&m_drmEventCtx, 0, sizeof(m_drmEventCtx));
	m_drmEventCtx.version = 2;

	// Wakeup DisplayHandlerThread
	WakeupDisplayThread();
}

///
///	Cleanup video output module.
///
void cVideoRender::Exit(void)
{
	ExitDecodingThread();
	ExitDisplayThread();

	// restore saved CRTC configuration
	if (m_drmModeCrtcSaved){
		drmModeSetCrtc(m_fdDrm, m_drmModeCrtcSaved->crtc_id, m_drmModeCrtcSaved->buffer_id,
			m_drmModeCrtcSaved->x, m_drmModeCrtcSaved->y, &m_connectorId, 1, &m_drmModeCrtcSaved->mode);
		drmModeFreeCrtc(m_drmModeCrtcSaved);
	}

	for (int i = 0; i < MAX_PLANES; i++) {
		if (m_pPlanes[i]) {
			free_properties(m_pPlanes[i]);
			free(m_pPlanes[i]);
		}
	}

	DestroyFB(&m_bufBlack);
#ifdef USE_GLES
	if (m_disableOglOsd) {
		if (m_pBufOsd) {
			DestroyFB(m_pBufOsd);
			free(m_pBufOsd);
		}
	} else {
		if (m_pNextBo)
			gbm_bo_destroy(m_pNextBo);
		if (m_pOldBo)
			gbm_bo_destroy(m_pOldBo);
	}
#else
	if (m_pBufOsd) {
		DestroyFB(m_pBufOsd);
		free(m_pBufOsd);
	}
#endif

	close(m_fdDrm);
}

///
///	Set size and position of the video.
///
void cVideoRender::SetVideoOutputPosition(int x, int y, int width, int height)
{
	m_videoParam.x = x;
	m_videoParam.y = y;
	m_videoParam.width = width;
	m_videoParam.height = height;

	if (m_videoParam.x == 0 &&
	    m_videoParam.y == 0 &&
	    m_videoParam.width == 0 &&
	    m_videoParam.height == 0)
		m_videoParam.is_scaled = 0;
	else
		m_videoParam.is_scaled = 1;

	LOGDEBUG("SetVideoOutputPosition %d %d %d %d%s", x, y, width, height, m_videoParam.is_scaled ? ", video is scaled" : "");
}

void cVideoRender::DisableDeint(int disable)
{
	m_configDeintDisabled = disable;
}

void cVideoRender::DisableOglOsd(void)
{
	m_disableOglOsd = 1;
}

int cVideoRender::DecodingThreadIsActive(void) {
	return m_pDecodingThread->Active();
};
