#ifndef __GRAB_H
#define __GRAB_H

#include "drm_buf.h"

//----------------------------------------------------------------------------
//	Grab
//----------------------------------------------------------------------------

///
///	cSoftHdGrab class
///
class cSoftHdGrab
{
private:
    uint8_t *result;		///< pointer to grabbed image
    struct drm_buf *buf;		///< pointer to original buffer
    int size;			///< returned grabbed data size
    int width;			///< returned grab width
    int height;			///< returned grab height
    int x;				///< x coord in screenshot
    int y;				///< y coord in screenshot

public:
    cSoftHdGrab(void);
    virtual ~cSoftHdGrab(void);

    void SetX(int _x) { x = _x; };
    void SetY(int _y) { y = _y; };
    void SetWidth(int _width) { width = _width; };
    void SetHeight(int _height) { height = _height; };

    int GetX(void) { return y; };
    int GetY(void) { return x; };
    int GetWidth(void) { return width; };
    int GetHeight(void) { return height; };

    void SetData(uint8_t *_result) { result = _result; };
    void SetSize(int _size) { size = _size; };
    uint8_t *GetData(void) { return result; };
    int GetSize(void) { return size; };

    void SetBuf(struct drm_buf *_buf) { buf = _buf; };
    struct drm_buf *GetBuf(void) { return buf; };
    void FreeBuf(void);
};

#endif
