///
///	@file ringbuffer.h	@brief Ringbuffer module header file
///
///	Copyright (c) 2009, 2011 by Johns.  All Rights Reserved.
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

/// @addtogroup Ringbuffer
/// @{

#ifndef __DEVICE_RINGBUFFER_H
#define __DEVICE_RINGBUFFER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iatomic.h"

//----------------------------------------------------------------------------
//	Ringbuffer
//----------------------------------------------------------------------------

/**
**	cRingDeviceBuffer - RingBuffer class
*/

class cDeviceRingbuffer
{
private:
    char *Buffer;			///< ring buffer data
    const char *BufferEnd;		///< end of buffer
    size_t Size;			///< bytes in buffer (for faster calc)
    const char *ReadPointer;		///< only used by reader
    char *WritePointer;			///< only used by writer

    /// The only thing modified by both
    atomic_t Filled;			///< how many of the buffer is used
public:
    cDeviceRingbuffer(size_t);
    virtual ~cDeviceRingbuffer(void);
    void Reset(void);
	///< reset ring buffer pointers
    size_t Write(const void *, size_t);
	///< write into ring buffer
    size_t GetWritePointer(void **);
	///< get write pointer of ring buffer
    size_t WriteAdvance(size_t);
	///< advance write pointer of ring buffer
    size_t Read(void *, size_t);
	///< read from ring buffer
    size_t GetReadPointer(const void **);
	///< get read pointer of ring buffer
    size_t ReadAdvance(size_t);
	///< advance read pointer of ring buffer
    size_t FreeBytes(void);
	///< free bytes ring buffer
    size_t UsedBytes(void);
	///< used bytes ring buffer
};
#endif

/// @}
