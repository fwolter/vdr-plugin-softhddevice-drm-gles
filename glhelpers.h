/**
 * @file glhelpers.h
 * @brief Some helper functions for GL
 *
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

#ifndef __GLHELPERS_H
#define __GLHELPERS_H

#include <stdio.h>
#include <stdlib.h>

#include <EGL/egl.h>
/* Hack:
 * xlib.h via eglplatform.h: #define Status int
 * X.h via eglplatform.h: #define CurrentTime 0L
 *
 * revert it, because it conflicts with vdr variables.
 */
#undef Status
#undef CurrentTime

#include <GLES2/gl2.h>

#include "logger.h"

/****************************************************************************************
 * Helpers
 ***************************************************************************************/
static inline void glCheckError(const char *stmt, const char *fname, int line) {
	GLint err = glGetError();
	if (err != GL_NO_ERROR)
		LOGERROR("GL Error (0x%08x): %s failed at %s:%i", err, stmt, fname, line);
}

static inline void eglCheckError(const char *stmt, const char *fname, int line) {
	EGLint err = eglGetError();
	if (err != EGL_SUCCESS)
		LOGERROR("EGL ERROR (0x%08x): %s failed at %s:%i", err, stmt, fname, line);
}

#define GL_CHECK(stmt) do { \
		stmt; \
		glCheckError(#stmt, __FILE__, __LINE__); \
	} while (0)

#define EGL_CHECK(stmt) do { \
		stmt; \
		eglCheckError(#stmt, __FILE__, __LINE__); \
	} while (0)

#endif