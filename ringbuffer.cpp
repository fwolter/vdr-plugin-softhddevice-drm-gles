///
///	@file ringbuffer.cpp	@brief Ringbuffer module
///
///	Copyright (c) 2009, 2011, 2014	by Johns.  All Rights Reserved.
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
///	@defgroup Ringbuffer The ring buffer module.
///
///	Lock free ring buffer with only one writer and one reader.
///

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iatomic.h"
#include "ringbuffer.h"

#include "logger.h"

/******************************************************************************
**	cDeviceRingbuffer class
******************************************************************************/

/**
**	cDeviceRingbuffer constructor
**
**	Allocate a new ring buffer
**
**	@param size	Size of the ring buffer.
**
**	@returns	Allocated ring buffer, must be freed with
**			RingBufferDel(), NULL for out of memory.
*/
cDeviceRingbuffer::cDeviceRingbuffer(size_t size)
{
    if (!(Buffer = (char *)malloc(size)))	// allocate buffer
	LOGFATAL("cDeviceRinbuffer::cDeviceRingbuffer: can't allocate memory for ringbuffer");

    Size = size;
    BufferEnd = Buffer + size;
    ReadPointer = Buffer;
    WritePointer = Buffer;
    atomic_set(&Filled, 0);
}

/**
**	cDeviceRingbuffer destructor
*/
cDeviceRingbuffer::~cDeviceRingbuffer(void)
{
    free(Buffer);
}

/**
**	Reset ring buffer pointers.
*/
void cDeviceRingbuffer::Reset(void)
{
    ReadPointer = Buffer;
    WritePointer = Buffer;
    atomic_set(&Filled, 0);
}

/**
**	Advance write pointer in ring buffer.
**
**	@param cnt	Number of bytes to be adavanced.
**
**	@returns	Number of bytes that could be advanced in ring buffer.
*/
size_t cDeviceRingbuffer::WriteAdvance(size_t cnt)
{
    size_t n;

    n = Size - atomic_read(&Filled);
    if (cnt > n) {			// not enough space
	cnt = n;
    }
    //
    //	Hitting end of buffer?
    //
    n = BufferEnd - WritePointer;
    if (n > cnt) {			// don't cross the end
	WritePointer += cnt;
    } else {				// reached or cross the end
	WritePointer = Buffer;
	if (n < cnt) {
	    n = cnt - n;
	    WritePointer += n;
	}
    }

    //
    //	Only atomic modification!
    //
    atomic_add(cnt, &Filled);
    return cnt;
}

/**
**	Write to a ring buffer.
**
**	@param buf	Buffer of @p cnt bytes.
**	@param cnt	Number of bytes in buffer.
**
**	@returns	The number of bytes that could be placed in the ring
**			buffer.
*/
size_t cDeviceRingbuffer::Write(const void *buf, size_t cnt)
{
    size_t n;

    n = Size - atomic_read(&Filled);
    if (cnt > n) {			// not enough space
	cnt = n;
    }
    //
    //	Hitting end of buffer?
    //
    n = BufferEnd - WritePointer;
    if (n > cnt) {			// don't cross the end
	memcpy(WritePointer, buf, cnt);
	WritePointer += cnt;
    } else {				// reached or cross the end
	memcpy(WritePointer, buf, n);
	WritePointer = Buffer;
	if (n < cnt) {
	    buf = (uint8_t *)buf + n;
	    n = cnt - n;
	    memcpy(WritePointer, buf, n);
	    WritePointer += n;
	}
    }

    //
    //	Only atomic modification!
    //
    atomic_add(cnt, &Filled);
    return cnt;
}

/**
**	Get write pointer and free bytes at this position of ring buffer.
**
**	@param[out] wp	Write pointer is placed here
**
**	@returns	The number of bytes that could be placed in the ring
**			buffer at the write pointer.
*/
size_t cDeviceRingbuffer::GetWritePointer(void **wp)
{
    size_t n;
    size_t cnt;

    //	Total free bytes available in ring buffer
    cnt = Size - atomic_read(&Filled);

    *wp = WritePointer;

    //
    //	Hitting end of buffer?
    //
    n = BufferEnd - WritePointer;
    if (n <= cnt) {			// reached or cross the end
	return n;
    }
    return cnt;
}

/**
**	Advance read pointer in ring buffer.
**
**	@param cnt	Number of bytes to be advanced.
**
**	@returns	Number of bytes that could be advanced in ring buffer.
*/
size_t cDeviceRingbuffer::ReadAdvance(size_t cnt)
{
    size_t n;

    n = atomic_read(&Filled);
    if (cnt > n) {			// not enough filled
	cnt = n;
    }
    //
    //	Hitting end of buffer?
    //
    n = BufferEnd - ReadPointer;
    if (n > cnt) {			// don't cross the end
	ReadPointer += cnt;
    } else {				// reached or cross the end
	ReadPointer = Buffer;
	if (n < cnt) {
	    n = cnt - n;
	    ReadPointer += n;
	}
    }

    //
    //	Only atomic modification!
    //
    atomic_sub(cnt, &Filled);
    return cnt;
}

/**
**	Read from a ring buffer.
**
**	@param buf	Buffer of @p cnt bytes.
**	@param cnt	Number of bytes to be read.
**
**	@returns	Number of bytes that could be read from ring buffer.
*/
size_t cDeviceRingbuffer::Read(void *buf, size_t cnt)
{
    size_t n;

    n = atomic_read(&Filled);
    if (cnt > n) {			// not enough filled
	cnt = n;
    }
    //
    //	Hitting end of buffer?
    //
    n = BufferEnd - ReadPointer;
    if (n > cnt) {			// don't cross the end
	memcpy(buf, ReadPointer, cnt);
	ReadPointer += cnt;
    } else {				// reached or cross the end
	memcpy(buf, ReadPointer, n);
	ReadPointer = Buffer;
	if (n < cnt) {
	    buf = (uint8_t *)buf + n;
	    n = cnt - n;
	    memcpy(buf, ReadPointer, n);
	    ReadPointer += n;
	}
    }

    //
    //	Only atomic modification!
    //
    atomic_sub(cnt, &Filled);
    return cnt;
}

/**
**	Get read pointer and used bytes at this position of ring buffer.
**
**	@param[out] rp	Read pointer is placed here
**
**	@returns	The number of bytes that could be read from the ring
**			buffer at the read pointer.
*/
size_t cDeviceRingbuffer::GetReadPointer(const void **rp)
{
    size_t n;
    size_t cnt;

    //	Total used bytes in ring buffer
    cnt = atomic_read(&Filled);

    *rp = ReadPointer;

    //
    //	Hitting end of buffer?
    //
    n = BufferEnd - ReadPointer;
    if (n <= cnt) {			// reached or cross the end
	return n;
    }
    return cnt;
}

/**
**	Get free bytes in ring buffer.
**
**	@returns	Number of bytes free in buffer.
*/
size_t cDeviceRingbuffer::FreeBytes(void)
{
    return Size - atomic_read(&Filled);
}

/**
**	Get used bytes in ring buffer.
**
**	@returns	Number of bytes used in buffer.
*/
size_t cDeviceRingbuffer::UsedBytes(void)
{
    return atomic_read(&Filled);
}
