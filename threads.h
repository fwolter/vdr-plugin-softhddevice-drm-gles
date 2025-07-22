#ifndef __THREADS_H
#define __THREADS_H

#include "vdr/thread.h"

//----------------------------------------------------------------------------
//	Thread
//----------------------------------------------------------------------------

///
///	Decoding thread
///
class cSoftHdDevice;

class cDecodingThread : public cThread
{
private:
    cSoftHdDevice *Device;
public:
    cDecodingThread(cSoftHdDevice *);
    virtual ~cDecodingThread(void);
    void Stop(void);
protected:
    virtual void Action(void);
};

///
///	Audio handler thread
///

class cSoftHdAudio;

class cAudioHandlerThread : public cThread
{
private:
    cSoftHdAudio *Audio;
    cCondWait StartWait;
public:
    cAudioHandlerThread(cSoftHdAudio *);
    virtual ~cAudioHandlerThread(void);
    void Stop(void);
    void SendStartSignal(void);
protected:
    virtual void Action(void);
};

#endif
