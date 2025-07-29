#ifndef __THREADS_H
#define __THREADS_H

#include "vdr/thread.h"
//#include "video.h"

#define VIDEO_SURFACES_MAX 3

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
///	Display thread
///
class cVideoRender;

class cDisplayThread : public cThread
{
private:
    cVideoRender *Render;
public:
    cDisplayThread(cVideoRender *);
    virtual ~cDisplayThread(void);
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

///
///	Filter thread
///

class cFilterThread : public cThread
{
private:
    cVideoRender *Render;

    AVFilterGraph *filter_graph;
    AVFilterContext *buffersrc_ctx;
    AVFilterContext *buffersink_ctx;

    int FilterBug;
    int FilterTrick;
    int FilterStill;

    AVFrame *FramesDeintRb[VIDEO_SURFACES_MAX];
    int FramesDeintFilled;
    int FramesDeintWrite;
    int FramesDeintRead;

    AVFrame *GetFrame(void);

public:
    cFilterThread(cVideoRender *);
    virtual ~cFilterThread(void);

    int Init(const AVCodecContext *, AVFrame *, int);
    void Stop(void);
    int GetFramesDeintFilled(void);
    void PushFrame(AVFrame *);

protected:
    virtual void Action(void);
};

#endif
