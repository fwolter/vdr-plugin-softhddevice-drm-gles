/*

#include <stdbool.h>
#include <unistd.h>

#include <inttypes.h>

#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut

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
#include "buf2rgb.h"
}

#include "video.h"
#include "audio.h"
#include "drm.h"
//#include "openglosd.h"
*/
#include "vdr/thread.h"
#include "threas.h"

//----------------------------------------------------------------------------
//	Thread
//----------------------------------------------------------------------------

///
///	Decoding thread
///
cDecodingThread::cDecodingThread(cSoftHdDevice *device) : cThread("decoding thread")
{
    Device = device;
    Start();
    StartWait = new cCondWait();
}

cDecodingThread::~cDecodingThread(void)
{
    delete StartWait;
}

void cDecodingThread::Action(void)
{
    LOGDEBUG("video: decoding thread started");
    while(Running())
	// manage fill frame output ring buffer
	if (Device->VideoStream->DecodeInput()) {
	    usleep(10000);
	}
    }
    LOGDEBUG("video: decoding thread stopped");
}

void cDecodingThread::Stop(void)
{
    LOGDEBUG("video: Stopping decoding thread");
    Cancel(2);
}

///
///	Display thread
///
cDisplayThread::cDisplayThread(cVideoRender *render) : cThread("display thread")
{
    Render = render;
    Start();
}

cDisplayThread::~cDisplayThread(void)
{
}

void cDisplayThread::Action(void)
{
    LOGDEBUG("video: display thread started");
    while(Running())
	int ret = Render->Frame2Display();

	if (!ret) {
	    if (Render->DrmHandleEvent() != 0)
		LOGERROR("DisplayHandlerThread: drmHandleEvent failed!");
	}

	if (Render->ShouldClose() || Render->ShouldFlush())
	    Render->CleanDisplayThread();

    }
    LOGDEBUG("video: display thread stopped");
}

	// manage fill frame output ring buffer
//	if (Device->VideoStream->DecodeInput()) {
//	    usleep(10000);
	}
    }
    LOGDEBUG("video: display thread stopped");
}

void cDisplayThread::Stop(void)
{
    LOGDEBUG("video: Stopping display thread");
    Cancel(2);
}

///
///	Audio handler thread
///
cAudioHandlerThread::cAudioHandlerThread(cSoftHdAudio *audio) : cThread("audio handler thread")
{
    Audio = audio;
    Start();
}

cAudioHandlerThread::~cAudioHandlerThread(void)
{
}

void cAudioHandlerThread::Action(void)
{
    if (!Audio->AudioIsPaused()) {
//	LOGDEBUG2(L_SOUND, "audio: AudioPlayHandlerThread: => AlsaFlushBuffers");
	Audio->AlsaFlushBuffers();
	Audio->AudioResetCompressor();
	Audio->AudioResetNormalizer();
    }
    Audio->AudioSetRunning(0);
    Audio->AlsaPlayerSetStop(0);

    // wait for sync start, if audio isn't running
    LOGDEBUG2(L_SOUND, "audio: wait on start condition");
    if (!Audio->AudioIsRunning())
	StartWait.Wait();

//    LOGDEBUG2(L_SOUND, "audio: AudioPlayHandlerThread: nach pthread_cond_wait ----> %dms start", (Audio->AudioUsedBytes() * 1000)
//    	/ (!Audio->HwSampleRate + !Audio->HwChannels +
//    	Audio->HwSampleRate * Audio->HwChannels * Audio->AudioBytesProSample));

    LOGDEBUG("audio: audio handler thread started");
    while(Running())
	if (Audio->AudioRingBuffer->UsedBytes()) {
		// try to play some samples
		Audio->AlsaPlayer();
	} else {
//		LOGDEBUG2(L_SOUND, "AudioPlayHandlerThread: ring buffer is empty, HwSampleRate %d", HwSampleRate);
		usleep(5000);
	}

	if (Audio->AudioIsPaused())
		usleep(10000);

	if (Audio->AlsaPlayerIsStopped())
		break;

    }
    LOGDEBUG("audio: audio handler thread stopped");
}

void cAudioHandlerThread::Stop(void)
{
    Audio->AudioSetRunning(1);
    StartWait.Signal();
    LOGDEBUG("audio: Stopping audio handler thread");
    Cancel(2);
}

void cAudioHandlerThread::SendStartSignal(void)
{
    StartWait.Signal();
}
