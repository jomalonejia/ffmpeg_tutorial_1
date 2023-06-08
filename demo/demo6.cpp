//
// Created by admin on 2023/6/2.
//
#include "../main.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

const char program_name[] = "demo6";
const int program_birth_year = 2023;

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

static unsigned sws_flags = SWS_BICUBIC;

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

class MyAVPacketList {
public:
    AVPacket *pkt;
    int serial;
};

class AudioParams {
public:
    int freq;
    AVChannelLayout ch_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
};

class Clock {
public:
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int serial;           /* clock is based on a packet with this serial */
    int paused;
    int *queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
};

/* Common struct for handling all types of decoded data and allocated render buffers. */
class Frame {
public:
    AVFrame *frame;
    AVSubtitle sub;
    int serial;
    double pts;           /* presentation timestamp for the frame */
    double duration;      /* estimated duration of the frame */
    int64_t pos;          /* byte position of the frame in the input file */
    int width;
    int height;
    int format;
    AVRational sar;
    int uploaded;
    int flip_v;
};

class PacketQueue {
public:
    AVFifo *pkt_list;
    int nb_packets;
    int size;
    int64_t duration;
    bool abort_request;
    int serial;
    SDL_mutex *mutex;
    SDL_cond *cond;
};

class FrameQueue {
public:
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    int max_size;
    int keep_last;
    int rindex_shown;
    SDL_mutex *mutex;
    SDL_cond *cond;
    PacketQueue *pktq;
};

class Decoder {
public:
    AVPacket *pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    int pkt_serial;
    int finished;
    int packet_pending;
    SDL_cond *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;
};

class VideoState {
public:
    VideoState() = default;

    SDL_Thread *read_tid;
    const AVInputFormat *iformat;
    int abort_request;
    int force_refresh;
    int paused;
    int last_paused;
    int queue_attachments_req;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    AVFormatContext *ic;
    int realtime;

    Clock audclk;
    Clock vidclk;
    Clock extclk;

    FrameQueue pictq;
    FrameQueue subpq;
    FrameQueue sampq;

    Decoder auddec;
    Decoder viddec;
    Decoder subdec;

    int audio_stream;

    int av_sync_type;

    double audio_clock;
    int audio_clock_serial;
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    AVStream *audio_st;
    PacketQueue audioq;
    int audio_hw_buf_size;
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size; /* in bytes */
    unsigned int audio_buf1_size;
    int audio_buf_index; /* in bytes */
    int audio_write_buf_size;
    int audio_volume;
    bool muted;
    AudioParams audio_src;
    AudioParams audio_filter_src;
    AudioParams audio_tgt;
    struct SwrContext *swr_ctx;
    int frame_drops_early;
    int frame_drops_late;

    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    int last_i_start;
    RDFTContext *rdft;
    int rdft_bits;
    FFTSample *rdft_data;
    int xpos;
    double last_vis_time;
    SDL_Texture *vis_texture;
    SDL_Texture *sub_texture;
    SDL_Texture *vid_texture;

    int subtitle_stream;
    AVStream *subtitle_st;
    PacketQueue subtitleq;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;
    AVStream *video_st;
    PacketQueue videoq;
    double max_frame_duration;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    struct SwsContext *img_convert_ctx;
    struct SwsContext *sub_convert_ctx;
    bool eof;

    char *filename;
    int width, height, xleft, ytop;
    int step;

    int vfilter_idx;
    AVFilterContext *in_video_filter;   // the first filter in the video chain
    AVFilterContext *out_video_filter;  // the last filter in the video chain
    AVFilterContext *in_audio_filter;   // the first filter in the audio chain
    AVFilterContext *out_audio_filter;  // the last filter in the audio chain
    AVFilterGraph *agraph;              // audio filter graph

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_cond *continue_read_thread;
};

void printError(const char *msg, int err) {
    char errStr[256] = {0};
    if (err < 0) {
        av_strerror(err, errStr, sizeof(errStr));
    }
    std::cout << msg << " : " << errStr << std::endl;
    exit(-1);
}

enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

enum ShowMode {
    SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
};


static const char *input_filename = "../asset/juren-30s.mp4";
static const AVInputFormat *file_iformat;
static const char *window_title;
static int default_width = 640;
static int default_height = 480;
static int screen_width = 0;
static int screen_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;
static int audio_disable;
static int video_disable;
static int subtitle_disable;
static const char *wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
static int seek_by_bytes = -1;
static float seek_interval = 10;
static int display_disable;
static int borderless;
static int alwaysontop;
static int startup_volume = 100;
static int show_status = -1;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static int fast = 0;
static int genpts = 0;
static int lowres = 0;
static int decoder_reorder_pts = -1;
static int autoexit;
static int exit_on_keydown;
static int exit_on_mousedown;
static int loop = 1;
static int framedrop = -1;
static int infinite_buffer = -1;
static int show_mode = SHOW_MODE_NONE;
static const char *audio_codec_name;
static const char *subtitle_codec_name;
static const char *video_codec_name;
double rdftspeed = 0.02;
static int64_t cursor_last_shown;
static int cursor_hidden = 0;
static const char **vfilters_list = nullptr;
static int nb_vfilters = 0;
static char *afilters = nullptr;
static int autorotate = 1;
static int find_stream_info = 1;
static int filter_nbthreads = 0;

/* current context */
static int is_full_screen;
static int64_t audio_callback_time;

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

SDL_Window *window;
SDL_Renderer *renderer;
SDL_RendererInfo renderer_info = {0};
SDL_AudioDeviceID audio_dev;

const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
        {AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332},
        {AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444},
        {AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555},
        {AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555},
        {AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565},
        {AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565},
        {AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24},
        {AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24},
        {AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888},
        {AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888},
        {AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888},
        {AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888},
        {AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888},
        {AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888},
        {AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888},
        {AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888},
        {AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV},
        {AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2},
        {AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY},
        {AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN},
};

int opt_add_vfilter(void *optctx, const char *opt, const char *arg) {
    //GROW_ARRAY(vfilters_list, nb_vfilters);
    vfilters_list[nb_vfilters - 1] = arg;
    return 0;
}

void set_clock_at(Clock *c, double pts, int serial, double time) {
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

void set_clock(Clock *c, double pts, int serial) {
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

void init_clock(Clock *c, int *queue_serial) {
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

/* packet queue handling */
int packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
    if (!q->pkt_list)
        return AVERROR(ENOMEM);
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->abort_request = true;
    return 0;
}

void packet_queue_flush(PacketQueue *q) {
    MyAVPacketList pkt1;

    SDL_LockMutex(q->mutex);
    while (q->pkt_list && (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0)) {
        av_packet_free((&pkt1.pkt));
    }
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    q->serial++;
    SDL_UnlockMutex(q->mutex);
}

void packet_queue_destroy(PacketQueue *q) {
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkt_list);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last) {
    int i;
    memset(f, 0, sizeof(FrameQueue));
    if (!(f->mutex = SDL_CreateMutex())) {
        av_log(nullptr, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    if (!(f->cond = SDL_CreateCond())) {
        av_log(nullptr, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

void frame_queue_destory(FrameQueue *f) {
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        av_frame_unref(vp->frame);
        avsubtitle_free(&vp->sub);
        av_frame_free(&vp->frame);
    }
    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
}

void stream_close(VideoState *is) {
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    SDL_WaitThread(is->read_tid, nullptr);

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    /* free all pictures */
    frame_queue_destory(&is->pictq);
    frame_queue_destory(&is->sampq);
    frame_queue_destory(&is->subpq);

    SDL_DestroyCond(is->continue_read_thread);
    av_free(is->filename);

    //av_free(is);  //malloc
    delete is;
    is = nullptr;
}

int decode_interrupt_cb(void *ctx) {
    return static_cast<VideoState *>(ctx)->abort_request;
}

/* this thread gets the stream from the disk or the network */
int read_thread(VideoState *is) {

    int err, i, ret;
    SDL_mutex *wait_mutex = nullptr;
    AVFormatContext *ic = nullptr;
    AVPacket *pkt = nullptr;
    int st_index[AVMEDIA_TYPE_NB];

    do {
        wait_mutex = SDL_CreateMutex();
        if (!wait_mutex) {
            av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
            break;
        }

        if (ic && !is->ic) {
            avformat_close_input(&ic);
        }

        memset(st_index, -1, sizeof(st_index));
        is->eof = false;

        pkt = av_packet_alloc();
        if (!pkt) {
            av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
            ret = AVERROR(ENOMEM);
            break;
        }
        ic = avformat_alloc_context();
        if (!ic) {
            av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
            ret = AVERROR(ENOMEM);
            break;
        }

        ic->interrupt_callback.callback = decode_interrupt_cb;
        ic->interrupt_callback.opaque = is;

        err = avformat_open_input(&ic, is->filename, is->iformat, nullptr);
        if (err < 0) {
            printError("avformat_open_input", ret);
            break;
        }

        is->ic = ic;
        av_format_inject_global_side_data(ic);

    } while (0);


    av_packet_free(&pkt);
    if (ret != 0) {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    SDL_DestroyMutex(wait_mutex);
    return 0;
}

VideoState *stream_open(const char *filename,
                        const AVInputFormat *iformat) {
    VideoState *is = nullptr;
    do {
        is = new VideoState();
        is->last_video_stream = is->video_stream = -1;
        is->last_audio_stream = is->audio_stream = -1;
        is->last_subtitle_stream = is->subtitle_stream = -1;

        is->filename = av_strdup(filename);
        if (!is->filename) {
            break;
        }

        is->iformat = iformat;
        is->ytop = 0;
        is->xleft = 0;

        /* start video display */
        if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
            break;
        if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
            break;
        if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
            break;

        if (packet_queue_init(&is->videoq) < 0 ||
            packet_queue_init(&is->audioq) < 0 ||
            packet_queue_init(&is->subtitleq) < 0) {
            break;
        }

        if (!(is->continue_read_thread = SDL_CreateCond())) {
            av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
            break;
        }

        init_clock(&is->vidclk, &is->videoq.serial);
        init_clock(&is->audclk, &is->audioq.serial);
        init_clock(&is->extclk, &is->extclk.serial);


        is->audio_clock_serial = -1;
        if (startup_volume < 0)
            av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
        if (startup_volume > 100)
            av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
        startup_volume = av_clip(startup_volume, 0, 100);
        startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
        is->audio_volume = startup_volume;

        is->muted = false;
        is->av_sync_type = av_sync_type;

        is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
        if (!is->read_tid) {
            av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
            break;
        }
        return is;

    } while (0);

    stream_close(is);
    return nullptr;
}

static void do_exit(VideoState *is) {
    if (is) {
        stream_close(is);
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }

    if (window) {
        SDL_DestroyWindow(window);
    }
    if (&vfilters_list) {
        av_freep(&vfilters_list);
    }
    avformat_network_deinit();
    if (show_status) {
        printf("\n");
    }
    SDL_Quit();
    av_log(nullptr, AV_LOG_QUIET, "%s", "");
    exit(0);
}

int main(int argv, char **args) {

    avformat_network_init();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        av_log(nullptr, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(nullptr, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    int flags;
    //flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    if (borderless) {
        flags |= SDL_WINDOW_BORDERLESS;
    } else {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    flags |= SDL_WINDOW_OPENGL;
    flags |= SDL_WINDOW_ALLOW_HIGHDPI;

    window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width,
                              default_height, flags);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (window) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED
                                                  | SDL_RENDERER_PRESENTVSYNC
                                                  | SDL_RENDERER_TARGETTEXTURE);
        if (!renderer) {
            av_log(nullptr, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n",
                   SDL_GetError());
            renderer = SDL_CreateRenderer(window, -1, 0);
        }
        if (renderer) {
            if (!SDL_GetRendererInfo(renderer, &renderer_info))
                av_log(nullptr, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
        }
    }
    if (!window || !renderer || !renderer_info.num_texture_formats) {
        av_log(nullptr, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
        do_exit(nullptr);
    }

    VideoState *is = stream_open(input_filename, file_iformat);
    if (!is) {
        av_log(nullptr, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        do_exit(nullptr);
    }

    do_exit(is);
    return 0;
}