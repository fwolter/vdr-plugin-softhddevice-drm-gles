extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
}

#include "logger.h"
#include "vdr/thread.h"
#include "threads.h"
#include "video.h"
#include "audio.h"

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
}

cDecodingThread::~cDecodingThread(void)
{
}

void cDecodingThread::Action(void)
{
    LOGDEBUG("video: decoding thread started");
    while(Running()) {
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
    while(Running()) {
	int ret = Render->DisplayFrame();

	if (!ret) {
	    if (Render->DrmHandleEvent() != 0)
		LOGERROR("DisplayHandlerThread: drmHandleEvent failed!");
	}

	if (Render->ShouldClose() || Render->ShouldFlush())
	    Render->CleanUp();

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
    Mutex.Lock();
    while (Running()) {
	if (!Audio->IsPaused()) {
//	    LOGDEBUG2(L_SOUND, "audio: AudioPlayHandlerThread: => FlushAlsaBuffers");
	    Audio->FlushAlsaBuffers();
	    Audio->ResetCompressor();
	    Audio->ResetNormalizer();
	}
	Audio->SetRunning(0);
	Audio->StartAlsaPlayer();

	// wait for sync start, if audio isn't running
	if (!Audio->IsRunning()) {
	    LOGDEBUG2(L_SOUND, "audio: wait on start condition");
	    StartWait.Wait(Mutex);
	}

	LOGDEBUG("audio: audio handler thread started");
	while(Running()) {
	    if (!Audio->AlsaPlayerRunning())
		break;

	    if (Audio->IsPaused()) {
		usleep(10000);
		continue;
	    }

	    if (Audio->GetUsedBytes()) {
		// try to play some samples
		Audio->PlayWithAlsa();
	    } else {
//		LOGDEBUG2(L_SOUND, "AudioPlayHandlerThread: ring buffer is empty");
		usleep(5000);
	    }
	}
    }
    LOGDEBUG("audio: audio handler thread stopped");
}

void cAudioHandlerThread::Stop(void)
{
    LOGDEBUG("audio: Stopping audio handler thread");
    Audio->SetRunning(1);
    Audio->StopAlsaPlayer();
    StartWait.Broadcast();
    Cancel(2);
}

void cAudioHandlerThread::SendStartSignal(void)
{
    StartWait.Broadcast();
}

///
///	Filter thread
///

cFilterThread::cFilterThread(cVideoRender *render) : cThread("filter thread")
{
    Render = render;
}

cFilterThread::~cFilterThread(void)
{
}

///
//	Filter init.
//
//	@retval 0	filter initialised
//	@retval	-1	filter initialise failed
//
int cFilterThread::Init(const AVCodecContext *VideoCtx, AVFrame *Frame, int disabled)
{
    int ret;
    char args[512];
    const char *filter_descr = NULL;
    filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
	LOGERROR("VideoFilterInit: Cannot alloc filter graph");
	return -1;
    }

    atomic_set(&FramesDeintFilled, 0);
    FramesDeintRead = 0;
    FramesDeintWrite =  0;

    Render->ClearFramesToFilter();
    FilterBug = 0;
    FilterTrick = 0;
    FilterStill = 0;
    m_isInterlaceFilter = 0;

    const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");

    int interlaced;
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
    interlaced = Frame->interlaced_frame;
#else
    interlaced = Frame->flags & AV_FRAME_FLAG_INTERLACED;
#endif

    if (VideoCtx->framerate.num > 0) {
	if (VideoCtx->framerate.num / VideoCtx->framerate.den > 30)
	    interlaced = 0;
	else
	    interlaced = 1;
    }

    if (VideoCtx->codec_id == AV_CODEC_ID_HEVC)
	interlaced = 0;

    if (disabled) {
	if (interlaced)
	    LOGDEBUG2(L_CODEC, "VideoFilterInit: Deinterlacer wanted, but disabled in setup!");
	interlaced = 0;
    }

    FrameData *fd;
    if (!Frame->opaque_ref) {
	Frame->opaque_ref = av_buffer_allocz(sizeof(*fd));
	if (!Frame->opaque_ref)
	    LOGFATAL("FilterHandlreThread: cannot allocate private frame data");
    }
    fd = (FrameData *)Frame->opaque_ref->data;

    // interlaced and non-trickspeed AV_PIX_FMT_DRM_PRIME (hardware decoded) -> hardware deinterlacer
    // interlaced and non-trickspeed AV_PIX_FMT_YUV420P (software decoded) -> software deinterlacer
    // progressive and trickspeed AV_PIX_FMT_YUV420P (software decoded) -> scale filter (for NV12 output)
    // progressive and trickspeed AV_PIX_FMT_DRM_PRIME (hardware decoded) doesn't get to the FilterHandlerThread
    if (interlaced && !(fd->flags & FRAME_FLAG_TRICKSPEED || fd->flags & FRAME_FLAG_STILLPICTURE)) {
	if (Frame->format == AV_PIX_FMT_DRM_PRIME) {
	    filter_descr = "deinterlace_v4l2m2m";
	    m_isInterlaceFilter = 1;
	} else if (Frame->format == AV_PIX_FMT_YUV420P) {
	    filter_descr = "bwdif=1:-1:0";
	    FilterBug = 1;
	    m_isInterlaceFilter = 1;
	}
    } else if (Frame->format == AV_PIX_FMT_YUV420P) {
	filter_descr = "scale";
	if (fd->flags & FRAME_FLAG_TRICKSPEED)
	    FilterTrick = 1;
	if (fd->flags & FRAME_FLAG_STILLPICTURE)
	    FilterStill = 1;
    }
#if LIBAVFILTER_VERSION_INT < AV_VERSION_INT(7,16,100)
    avfilter_register_all();
#endif

    // if we have a 576i stream without a valid sample_aspect_ratio (0/1) force it to be 64/45
    // wich "streches" a 576i stream to 1920/1080 size
    int sarNum = VideoCtx->sample_aspect_ratio.num != 0 ? VideoCtx->sample_aspect_ratio.num : (VideoCtx->height == 576 ? 64 : 1);
    int sarDen = VideoCtx->sample_aspect_ratio.num != 0 ? VideoCtx->sample_aspect_ratio.den : (VideoCtx->height == 576 ? 45 : 1);

    snprintf(args, sizeof(args),
	"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
	VideoCtx->width, VideoCtx->height, Frame->format,
	VideoCtx->pkt_timebase.num ? VideoCtx->pkt_timebase.num : 1,
	VideoCtx->pkt_timebase.num ? VideoCtx->pkt_timebase.den : 1,
	sarNum,
	sarDen);

    LOGDEBUG2(L_CODEC, "VideoFilterInit: filter=\"%s\" args=\"%s\"", filter_descr, args);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);
    if (ret < 0) {
	LOGERROR("VideoFilterInit: Cannot create buffer source (%d)", ret);
	avfilter_graph_free(&filter_graph);
	return -1;
    }

    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
    memset(par, 0, sizeof(*par));
    par->format = AV_PIX_FMT_NONE;
    par->hw_frames_ctx = Frame->hw_frames_ctx;
    ret = av_buffersrc_parameters_set(buffersrc_ctx, par);
    if (ret < 0) {
	LOGERROR("VideoFilterInit: Cannot av_buffersrc_parameters_set (%d)", ret);
	av_free(par);
	avfilter_graph_free(&filter_graph);
	return -1;
    }

    av_free(par);

    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph);
    if (ret < 0) {
	LOGERROR("VideoFilterInit: Cannot create buffer sink (%d)", ret);
	avfilter_graph_free(&filter_graph);
	return -1;
    }

    if (Frame->format != AV_PIX_FMT_DRM_PRIME) {
	enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_NV12, AV_PIX_FMT_NONE };
	ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
	    LOGERROR("VideoFilterInit: Cannot set output pixel format (%d)", ret);
	    avfilter_graph_free(&filter_graph);
	    return -1;
	}
    }

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    ret = avfilter_graph_parse_ptr(filter_graph, filter_descr, &inputs, &outputs, NULL);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    if (ret < 0) {
	LOGERROR("VideoFilterInit: avfilter_graph_parse_ptr failed (%d)", ret);
	avfilter_graph_free(&filter_graph);
	return -1;
    }

    ret = avfilter_graph_config(filter_graph, NULL);
    if (ret < 0) {
	LOGERROR("VideoFilterInit: avfilter_graph_config failed (%d)", ret);
	avfilter_graph_free(&filter_graph);
	return -1;
    }

    return 0;
}

void cFilterThread::Action(void)
{
    AVFrame *frame = 0;
    int ret = 0;
    int enqueued = 0;

    LOGDEBUG("video: video filter thread started");

    while (Running()) {
	if (!GetFramesDeintFilled()) {
	    usleep(10000);
	    continue;
	}

// filter frame
	frame = RbGetFrame();

	int interlaced;
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58,7,100)
	interlaced = frame->interlaced_frame;
#else
	interlaced = frame->flags & AV_FRAME_FLAG_INTERLACED;
#endif
	if (interlaced) {
		Render->IncFramesToFilter();
		Render->IncFramesToFilter();
	} else {
		Render->IncFramesToFilter();
	}

	if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
	    LOGWARNING("FilterHandlerThread: can't add_frame.");
	else
	    av_frame_free(&frame);

	while (Running()) {
		AVFrame *filt_frame = av_frame_alloc();
		ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);

		if (ret == AVERROR(EAGAIN)) {
		    av_frame_free(&filt_frame);
		    break;
		} else if (ret == AVERROR_EOF) {
		    av_frame_free(&filt_frame);
		    break;
		} else if (ret < 0) {
		    LOGERROR("FilterHandlerThread: can't get filtered frame: %s",
			av_err2str(ret));
		    av_frame_free(&filt_frame);
		    break;
		}

// set flag of the filtered frame (scale filter and AV_PIX_FMT_YUV420P)
		FrameData *fd;
		if (!filt_frame->opaque_ref) {
		    filt_frame->opaque_ref = av_buffer_allocz(sizeof(*fd));
		    if (!filt_frame->opaque_ref)
			LOGFATAL("FilterHandlerThread: cannot allocate private frame data");
		}
		fd = (FrameData *)filt_frame->opaque_ref->data;
		if (FilterTrick)
		    fd->flags |= FRAME_FLAG_TRICKSPEED;
		if (FilterStill) {
		    fd->flags |= FRAME_FLAG_STILLPICTURE;
		    filt_frame->pts = AV_NOPTS_VALUE;
		}

// put frame into display queue
		enqueued = 0;
		while (Running()) {
		    Render->FramesRbLock();
		    if (Render->GetFramesFilled() < VIDEO_SURFACES_MAX) {
			if (filt_frame->format == AV_PIX_FMT_NV12) {
			    // scale filter or sw deinterlacer, no prime data, always returns NV12
			    // -> go through EnqueueFB
			    if (FilterBug)
				filt_frame->pts = filt_frame->pts / 2;	// ffmpeg bug
			    Render->DecFramesToFilter();
			    Render->FramesRbUnlock();
			    Render->EnqueueFB(filt_frame);
			} else {
			    // hw deinterlacers, we received prime data
			    // -> put the frame into render Rb
			    Render->RbPushFrame(filt_frame);
			    Render->DecFramesToFilter();
			    Render->FramesRbUnlock();
			}
			enqueued = 1;
			break;
		    } else {
			Render->FramesRbUnlock();
			usleep(1000);
			continue;
		    }
		}

		if (!enqueued)
		    av_frame_free(&filt_frame);
	}

    }
    LOGDEBUG("video: filter thread stopped");
}

int cFilterThread::GetFramesDeintFilled(void)
{
    return atomic_read(&FramesDeintFilled);
}

AVFrame *cFilterThread::RbGetFrame(void)
{
    AVFrame *frame = FramesDeintRb[FramesDeintRead];
    FramesDeintRead = (FramesDeintRead + 1) % VIDEO_SURFACES_MAX;
    atomic_dec(&FramesDeintFilled);

    return frame;
}

void cFilterThread::RbPushFrame(AVFrame *frame)
{
    FramesDeintRb[FramesDeintWrite] = frame;
    FramesDeintWrite = (FramesDeintWrite + 1) % VIDEO_SURFACES_MAX;
    atomic_inc(&FramesDeintFilled);
}

void cFilterThread::Stop(void)
{
    LOGDEBUG("video: Stopping filter thread");
    Cancel(2);
    FilterBug = 0;
    FilterTrick = 0;
    FilterStill = 0;
    Render->ClearFramesToFilter();

    while (GetFramesDeintFilled()) {
	AVFrame *frame = RbGetFrame();
	av_frame_free(&frame);
    }

    avfilter_graph_free(&filter_graph);
}
