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

#ifndef __SOFTHDDEV_H
#define __SOFTHDDEV_H

extern "C" {
#include <libavcodec/avcodec.h>
}

//----------------------------------------------------------------------------
//	Typedefs
//----------------------------------------------------------------------------
    /// Video output stream typedef
    typedef struct __video_stream__ VideoStream; 		// in softhddev.h ?

//----------------------------------------------------------------------------
//	Prototypes
//----------------------------------------------------------------------------

    /// Decode video input buffers.
    extern int VideoDecodeInput(VideoStream *);
    /// Get number of input buffers.
    extern int VideoGetPackets(void);

    /// Get parsed width and height
    extern void ParseResolutionH264(int *, int *);

    /// C plugin get video render
    extern void *GetVideoRender(void);
    /// Set interlaced stream flag
    extern void SetInterlacedStream(int);

#endif
