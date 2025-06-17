#ifndef __GLHELPERS_H
#define __GLHELPERS_H

#include <stdio.h>
#include <stdlib.h>

#include <EGL/egl.h>
//#include <EGL/eglext.h>
//#include <EGL/eglplatform.h>
/* Hack:
 * xlib.h via eglplatform.h: #define Status int
 * X.h via eglplatform.h: #define CurrentTime 0L
 *
 * revert it, because it conflicts with vdr variables.
 */
#undef Status
#undef CurrentTime

#include <GLES2/gl2.h>
//#include <GLES2/gl2ext.h>

#include "logger.h"

/****************************************************************************************
* Helpers
****************************************************************************************/
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