/*
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
}
*/
//#include "misc.h"
//#include "buf2rgb.h"
//#include <libavutil/hwcontext_drm.h>
//#include <libavutil/pixdesc.h>
/*
#include "drm.h"
//#include "openglosd.h"
*/
#include "logger.h"
#include "grab.h"
/*
#include "vdr/thread.h"
#include "threads.h"
#include "video.h"
#include "audio.h"
*/
//----------------------------------------------------------------------------
//	Grab
//----------------------------------------------------------------------------

///
///	cSoftHdGrab class
///
cSoftHdGrab::cSoftHdGrab(void)
{
    buf = NULL;
    result = NULL;

    x = 0;
    y = 0;
    width = 0;
    height = 0;
    size = 0;
}

cSoftHdGrab::~cSoftHdGrab(void)
{
}

void cSoftHdGrab::FreeBuf(void)
{
	if (!buf)
	    return;

	for (int plane = 0; plane < buf->num_planes; plane++) {
	    if (buf->size[plane]) {
		LOGDEBUG2(L_GRAB, "FreeBuf: free buf %p (plane %d)", buf->plane[plane], plane);
		free(buf->plane[plane]);
	    }
	}
	free(buf);
}
