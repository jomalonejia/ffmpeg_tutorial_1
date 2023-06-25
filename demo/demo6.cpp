//
// Created by admin on 2023/6/2.
//
#include "../main.h"

const char program_name[] = "demo6";
const int program_birth_year = 2023;

enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

enum ShowMode {
    SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
};

static bool CONFIG_AVFILTER = false;

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
    int abort_request = 0;
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

    ShowMode show_mode = SHOW_MODE_NONE;

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
static bool show_status = false;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static int fast = 0;
static int genpts = 0;
static int lowres = 0;
static int decoder_reorder_pts = -1;
static bool autoexit = true;
static int exit_on_keydown;
static int exit_on_mousedown;
static int loop = 1;
static int framedrop = -1;
static bool infinite_buffer = false;
static ShowMode show_mode = SHOW_MODE_NONE;
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

static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar) {
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop + y;
    rect->w = FFMAX((int) width, 1);
    rect->h = FFMAX((int) height, 1);
}

static void set_default_window_size(int width, int height, AVRational sar) {
    SDL_Rect rect;
    int max_width = screen_width ? screen_width : INT_MAX;
    int max_height = screen_height ? screen_height : INT_MAX;
    if (max_width == INT_MAX && max_height == INT_MAX)
        max_height = height;
    calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
    default_width = rect.w;
    default_height = rect.h;
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

double get_clock(Clock *c) {
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

void sync_clock_to_slave(Clock *c, Clock *slave) {
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
double get_master_clock(VideoState *is) {
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            val = get_clock(&is->extclk);
            break;
    }
    return val;
}

/* packet queue handling */
int packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
    if (!q->pkt_list)
        return AVERROR(ENOMEM);
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_log(nullptr, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_log(nullptr, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->abort_request = true;
    return 0;
}

void packet_queue_start(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    q->abort_request = false;
    q->serial++;
    SDL_UnlockMutex(q->mutex);
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

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial) {
    MyAVPacketList pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
            q->nb_packets--;
            q->size -= pkt1.pkt->size + sizeof(pkt1);
            q->duration -= pkt1.pkt->duration;
            av_packet_move_ref(pkt, pkt1.pkt);
            if (serial)
                *serial = pkt1.serial;
            av_packet_free(&pkt1.pkt);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

void packet_queue_destroy(PacketQueue *q) {
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkt_list);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

int packet_queue_put_private(PacketQueue *q, AVPacket *pkt) {
    MyAVPacketList pkt1;
    int ret;

    if (q->abort_request)
        return -1;


    pkt1.pkt = pkt;
    pkt1.serial = q->serial;

    ret = av_fifo_write(q->pkt_list, &pkt1, 1);
    if (ret < 0)
        return ret;
    q->nb_packets++;
    q->size += pkt1.pkt->size + sizeof(pkt1);
    q->duration += pkt1.pkt->duration;
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);
    return 0;
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacket *pkt1;
    int ret;

    pkt1 = av_packet_alloc();
    if (!pkt1) {
        av_packet_unref(pkt);
        return -1;
    }
    av_packet_move_ref(pkt1, pkt);

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt1);
    SDL_UnlockMutex(q->mutex);

    if (ret < 0) {
        av_packet_free(&pkt1);
    }
    return ret;
}

int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index) {
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
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

static void frame_queue_push(FrameQueue *f) {
    if (++f->windex == f->max_size)
        f->windex = 0;
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
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


Frame *frame_queue_peek_readable(FrameQueue *f) {
    /* wait until we have a readable a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0 &&
           !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

void frame_queue_unref_item(Frame *vp) {
    av_frame_unref(vp->frame);
    avsubtitle_free(&vp->sub);
}

void frame_queue_next(FrameQueue *f) {
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    SDL_LockMutex(f->mutex);
    f->size--;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

Frame *frame_queue_peek_writable(FrameQueue *f) {
    /* wait until we have space to put a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size >= f->max_size &&
           !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[f->windex];
}

static int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->pkt = av_packet_alloc();
    if (!d->pkt)
        return AVERROR(ENOMEM);
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->pkt_serial = -1;
    return 0;
}

int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void *arg) {
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);
    if (!d->decoder_tid) {
        av_log(nullptr, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

int decode_interrupt_cb(void *ctx) {
    return static_cast<VideoState *>(ctx)->abort_request;
}

void decoder_destroy(Decoder *d) {
    if (d->pkt) {
        av_packet_free(&d->pkt);
    }
    if (d->avctx) {
        avcodec_free_context(&d->avctx);
    }
}

int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request)
                    return -1;

                switch (d->avctx->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            if (decoder_reorder_pts == -1) {
                                frame->pts = frame->best_effort_timestamp;
                            } else if (!decoder_reorder_pts) {
                                frame->pts = frame->pkt_dts;
                            }
                        }
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            AVRational tb = (AVRational) {1, frame->sample_rate};
                            if (frame->pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                            else if (d->next_pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                            if (frame->pts != AV_NOPTS_VALUE) {
                                d->next_pts = frame->pts + frame->nb_samples;
                                d->next_pts_tb = tb;
                            }
                        }
                        break;
                }
                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (d->queue->nb_packets == 0)
                SDL_CondSignal(d->empty_queue_cond);
            if (d->packet_pending) {
                d->packet_pending = 0;
            } else {
                int old_serial = d->pkt_serial;
                if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0)
                    return -1;
                if (old_serial != d->pkt_serial) {
                    avcodec_flush_buffers(d->avctx);
                    d->finished = 0;
                    d->next_pts = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            }
            if (d->queue->serial == d->pkt_serial)
                break;
            av_packet_unref(d->pkt);
        } while (1);

        if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            int got_frame = 0;
            ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, d->pkt);
            if (ret < 0) {
                ret = AVERROR(EAGAIN);
            } else {
                if (got_frame && !d->pkt->data) {
                    d->packet_pending = 1;
                }
                ret = got_frame ? 0 : (d->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
            }
            av_packet_unref(d->pkt);
        } else {
            if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN)) {
                av_log(d->avctx, AV_LOG_ERROR,
                       "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                d->packet_pending = 1;
            } else {
                av_packet_unref(d->pkt);
            }
        }
    }
}


/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */
static int synchronize_audio(VideoState *is, int nb_samples) {
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int) (diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
                av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                       diff, avg_diff, wanted_nb_samples - nb_samples,
                       is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum = 0;
        }
    }

    return wanted_nb_samples;
}

int audio_decode_frame(VideoState *is) {
    int data_size, resampled_data_size;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    if (is->paused)
        return -1;

    do {
/*#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep (1000);
        }
#endif*/
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return -1;
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    data_size = av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels,
                                           af->frame->nb_samples,
                                           static_cast<AVSampleFormat>(af->frame->format), 1);

    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format != is->audio_src.fmt ||
        av_channel_layout_compare(&af->frame->ch_layout, &is->audio_src.ch_layout) ||
        af->frame->sample_rate != is->audio_src.freq ||
        (wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx)) {
        swr_free(&is->swr_ctx);
        swr_alloc_set_opts2(&is->swr_ctx,
                            &is->audio_tgt.ch_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                            &af->frame->ch_layout, static_cast<AVSampleFormat>(af->frame->format),
                            af->frame->sample_rate,
                            0, NULL);
        if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                   af->frame->sample_rate, av_get_sample_fmt_name(static_cast<AVSampleFormat>(af->frame->format)),
                   af->frame->ch_layout.nb_channels,
                   is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.ch_layout.nb_channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        if (av_channel_layout_copy(&is->audio_src.ch_layout, &af->frame->ch_layout) < 0)
            return -1;
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = static_cast<AVSampleFormat>(af->frame->format);
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **) af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int64_t) wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
        int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.ch_layout.nb_channels, out_count,
                                                  is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq /
                                                  af->frame->sample_rate,
                                     wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;
#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif
    return resampled_data_size;
}

/* copy samples for viewing in editor window */
void update_sample_display(VideoState *is, short *samples, int samples_size) {
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        if (len > size)
            len = size;
        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
        samples += len;
        is->sample_array_index += len;
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
            is->sample_array_index = 0;
        size -= len;
    }
}


/* prepare a new audio buffer */
void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    VideoState *is = (VideoState *) opaque;
    int audio_size, len1;

    audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_size = audio_decode_frame(is);
            if (audio_size < 0) {
                /* if error, just output silence */
                is->audio_buf = nullptr;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            } else {
                if (is->show_mode != SHOW_MODE_VIDEO)
                    update_sample_display(is, (int16_t *) is->audio_buf, audio_size);
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len) {
            len1 = len;
        }
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME) {
            memcpy(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, len1);
        } else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf) {
                SDL_MixAudioFormat(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, AUDIO_S16SYS, len1,
                                   is->audio_volume);
            }
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        set_clock_at(&is->audclk, is->audio_clock - (double) (2 * is->audio_hw_buf_size + is->audio_write_buf_size) /
                                                    is->audio_tgt.bytes_per_sec, is->audio_clock_serial,
                     audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

static int audio_open(void *opaque, AVChannelLayout *wanted_channel_layout, int wanted_sample_rate,
                      struct AudioParams *audio_hw_params) {
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
    int wanted_nb_channels = wanted_channel_layout->nb_channels;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    wanted_nb_channels = wanted_channel_layout->nb_channels;
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(nullptr, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq) {
        next_sample_rate_idx--;
    }
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE,
                                2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    while (!(audio_dev = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec,
                                             SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        av_log(nullptr, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n", wanted_spec.channels,
               wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(nullptr, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        av_channel_layout_default(wanted_channel_layout, wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(nullptr, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, spec.channels);
        if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
            av_log(nullptr, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_channel_layout) < 0)
        return -1;
    audio_hw_params->frame_size = av_samples_get_buffer_size(nullptr, audio_hw_params->ch_layout.nb_channels, 1,
                                                             audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(nullptr, audio_hw_params->ch_layout.nb_channels,
                                                                audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }
    return spec.size;
}


int audio_thread(void *arg) {
    VideoState *is = (VideoState *) arg;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
#if CONFIG_AVFILTER
    int last_serial = -1;
    int reconfigure;
#endif
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame) {
        return AVERROR(ENOMEM);
    }

    do {
        if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0) {
            goto the_end;
        }

        if (got_frame) {
            tb = (AVRational) {1, frame->sample_rate};

#if CONFIG_AVFILTER
            reconfigure =
                cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.ch_layout.nb_channels,
                               frame->format, frame->ch_layout.nb_channels)    ||
                av_channel_layout_compare(&is->audio_filter_src.ch_layout, &frame->ch_layout) ||
                is->audio_filter_src.freq           != frame->sample_rate ||
                is->auddec.pkt_serial               != last_serial;

            if (reconfigure) {
                char buf1[1024], buf2[1024];
                av_channel_layout_describe(&is->audio_filter_src.ch_layout, buf1, sizeof(buf1));
                av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));
                av_log(NULL, AV_LOG_DEBUG,
                       "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                       is->audio_filter_src.freq, is->audio_filter_src.ch_layout.nb_channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                       frame->sample_rate, frame->ch_layout.nb_channels, av_get_sample_fmt_name(frame->format), buf2, is->auddec.pkt_serial);

                is->audio_filter_src.fmt            = frame->format;
                ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &frame->ch_layout);
                if (ret < 0)
                    goto the_end;
                is->audio_filter_src.freq           = frame->sample_rate;
                last_serial                         = is->auddec.pkt_serial;

                if ((ret = configure_audio_filters(is, afilters, 1)) < 0)
                    goto the_end;
            }

        if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
            goto the_end;

        while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
            tb = av_buffersink_get_time_base(is->out_audio_filter);
#endif
            if (!(af = frame_queue_peek_writable(&is->sampq)))
                goto the_end;

            af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            af->pos = frame->pkt_pos;
            af->serial = is->auddec.pkt_serial;
            af->duration = av_q2d((AVRational) {frame->nb_samples, frame->sample_rate});

            av_frame_move_ref(af->frame, frame);
            frame_queue_push(&is->sampq);

#if CONFIG_AVFILTER
            if (is->audioq.serial != is->auddec.pkt_serial)
                break;
        }
        if (ret == AVERROR_EOF)
            is->auddec.finished = is->auddec.pkt_serial;
#endif
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&is->agraph);
#endif
    av_frame_free(&frame);
    return ret;
}


/* open a given stream. Return 0 if OK */
int stream_component_open(VideoState *is, int stream_index) {

    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx = nullptr;
    AVChannelLayout ch_layout = {};
    AVDictionary *opts = nullptr;
    const AVCodec *codec = nullptr;
    const char *forced_codec_name = nullptr;
    int stream_lowres = lowres;
    int sample_rate = 0;


    int ret = 0;

    do {
        if (stream_index < 0 || stream_index >= ic->nb_streams) {
            return -1;
        }

        avctx = avcodec_alloc_context3(nullptr);
        if (!avctx) {
            return AVERROR(ENOMEM);
        }

        ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
        if (ret < 0) {
            break;
        }

        avctx->pkt_timebase = ic->streams[stream_index]->time_base;
        codec = avcodec_find_decoder(avctx->codec_id);

        switch (avctx->codec_type) {
            case AVMEDIA_TYPE_AUDIO   :
                is->last_audio_stream = stream_index;
                forced_codec_name = audio_codec_name;
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                is->last_subtitle_stream = stream_index;
                forced_codec_name = subtitle_codec_name;
                break;
            case AVMEDIA_TYPE_VIDEO   :
                is->last_video_stream = stream_index;
                forced_codec_name = video_codec_name;
                break;
        }

        if (forced_codec_name) {
            codec = avcodec_find_decoder_by_name(forced_codec_name);
        }

        if (!codec) {
            if (forced_codec_name) {
                av_log(nullptr, AV_LOG_WARNING,
                       "No codec could be found with name '%s'\n", forced_codec_name);
            } else {
                av_log(nullptr, AV_LOG_WARNING,
                       "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
            }
            ret = AVERROR(EINVAL);
            break;
        }


        avctx->codec_id = codec->id;

        //分辨率
        if (stream_lowres > codec->max_lowres) {
            av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
                   codec->max_lowres);
            stream_lowres = codec->max_lowres;
        }
        avctx->lowres = stream_lowres;

        if (fast) {
            avctx->flags2 |= AV_CODEC_FLAG2_FAST;
        }

        if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
            break;
        }

        is->eof = false;

        ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;

        switch (avctx->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
                {
                AVFilterContext *sink;

                is->audio_filter_src.freq           = avctx->sample_rate;
                ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &avctx->ch_layout);
                if (ret < 0)
                    goto fail;
                is->audio_filter_src.fmt            = avctx->sample_fmt;
                if ((ret = configure_audio_filters(is, afilters, 0)) < 0)
                    goto fail;
                sink = is->out_audio_filter;
                sample_rate    = av_buffersink_get_sample_rate(sink);
                ret = av_buffersink_get_ch_layout(sink, &ch_layout);
                if (ret < 0)
                    goto fail;
            }
#else
                sample_rate = avctx->sample_rate;
                ret = av_channel_layout_copy(&ch_layout, &avctx->ch_layout);
                if (ret < 0) {
                    break;
                }
#endif
                /* prepare audio output */
                if ((ret = audio_open(is, &ch_layout, sample_rate, &is->audio_tgt)) < 0) {
                    break;
                }
                is->audio_hw_buf_size = ret;
                is->audio_src = is->audio_tgt;
                is->audio_buf_size = 0;
                is->audio_buf_index = 0;

                /* init averaging filter */
                is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
                is->audio_diff_avg_count = 0;
                /* since we do not have a precise anough audio FIFO fullness,
                   we correct audio sync only if larger than this threshold */
                is->audio_diff_threshold = (double) (is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

                is->audio_stream = stream_index;
                is->audio_st = ic->streams[stream_index];

                if ((ret = decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread)) < 0) { break; }

                if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) &&
                    !is->ic->iformat->read_seek) {
                    is->auddec.start_pts = is->audio_st->start_time;
                    is->auddec.start_pts_tb = is->audio_st->time_base;
                }
                if ((ret = decoder_start(&is->auddec, audio_thread, "audio_decoder", is)) < 0) {
                    ret = 0;
                    break;
                }
                SDL_PauseAudioDevice(audio_dev, 0);

                break;
            case AVMEDIA_TYPE_VIDEO:
                break;
            default :
                break;
        }
    } while (0);

    if (ret < 0) {
        avcodec_free_context(&avctx);
    }

    av_channel_layout_uninit(&ch_layout);
    av_dict_free(&opts);

    return ret;
}

void stream_component_close(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams) {
        return;
    }

    codecpar = ic->streams[stream_index]->codecpar;


    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            decoder_destroy(&is->auddec);
            break;
        case AVMEDIA_TYPE_VIDEO:
            decoder_destroy(&is->viddec);
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            decoder_destroy(&is->subdec);
            break;
        default:
            break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->audio_st = nullptr;
            is->audio_stream = -1;
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->video_st = nullptr;
            is->video_stream = -1;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subtitle_st = nullptr;
            is->subtitle_stream = -1;
            break;
        default:
            break;
    }
}

void stream_close(VideoState *is) {
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    SDL_WaitThread(is->read_tid, nullptr);

    if (is->audio_stream >= 0) {
        stream_component_close(is, is->audio_stream);
    }
    if (is->video_stream >= 0) {
        stream_component_close(is, is->video_stream);
    }
    if (is->subtitle_stream >= 0) {
        stream_component_close(is, is->subtitle_stream);
    }

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    /* free all pictures */
    frame_queue_destory(&is->pictq);
    frame_queue_destory(&is->sampq);
    frame_queue_destory(&is->subpq);

    SDL_DestroyCond(is->continue_read_thread);
    av_free(is->filename);

    //TODO: MOCK avformat_free_context
    avformat_free_context(is->ic);

    //av_free(is);  //malloc
    delete is;
    is = nullptr;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

/* this thread gets the stream from the disk or the network */
int read_thread(void *src) {
    VideoState *is = (VideoState *) src;
    int err, i, ret;
    SDL_mutex *wait_mutex = nullptr;
    AVFormatContext *ic = nullptr;
    AVPacket *pkt = nullptr;
    int st_index[AVMEDIA_TYPE_NB];
    int64_t stream_start_time;
    int64_t pkt_ts;
    bool pkt_in_play_range = false;
    bool error = false;

    do {
        wait_mutex = SDL_CreateMutex();
        if (!wait_mutex) {
            av_log(nullptr, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
            break;
        }

        if (ic && !is->ic) {
            avformat_close_input(&ic);
        }

        memset(st_index, -1, sizeof(st_index));
        is->eof = false;

        pkt = av_packet_alloc();
        if (!pkt) {
            av_log(nullptr, AV_LOG_FATAL, "Could not allocate packet.\n");
            ret = AVERROR(ENOMEM);
            break;
        }
        ic = avformat_alloc_context();
        if (!ic) {
            av_log(nullptr, AV_LOG_FATAL, "Could not allocate context.\n");
            ret = AVERROR(ENOMEM);
            break;
        }

        ic->interrupt_callback.callback = decode_interrupt_cb;
        ic->interrupt_callback.opaque = is;

        err = avformat_open_input(&ic, is->filename, is->iformat, nullptr);
        if (err < 0) {
            av_log(nullptr, AV_LOG_FATAL, "avformat_open_input.\n");
            break;
        }

        is->ic = ic;
        av_format_inject_global_side_data(ic);

        if (find_stream_info) {
            err = avformat_find_stream_info(ic, nullptr);
            if (err < 0) {
                av_log(nullptr, AV_LOG_WARNING,
                       "%s: could not find codec parameters\n", is->filename);
                ret = -1;
                break;
            }
        }

        if (ic->pb) {
            ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
        }


        //TODO: seek_by_bytes

        is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

        window_title = is->filename;


        //TODO: if seeking requested, we execute it

        //TODO: realtime

        if (show_status) {
            av_dump_format(ic, 0, is->filename, 0);
        }

        for (i = 0; i < ic->nb_streams; i++) {
            AVStream *st = ic->streams[i];
            enum AVMediaType type = st->codecpar->codec_type;
            st->discard = AVDISCARD_ALL;
            if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1) {
                if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0) {
                    st_index[type] = i;
                }
            }
        }
        for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
            if (wanted_stream_spec[i] && st_index[i] == -1) {
                av_log(nullptr, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n",
                       wanted_stream_spec[i],
                       av_get_media_type_string(static_cast<AVMediaType>(i)));
                st_index[i] = INT_MAX;
            }
        }

        if (!video_disable) {
            st_index[AVMEDIA_TYPE_VIDEO] =
                    av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                        st_index[AVMEDIA_TYPE_VIDEO], -1, nullptr, 0);
        }
        if (!audio_disable) {
            st_index[AVMEDIA_TYPE_AUDIO] =
                    av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                        st_index[AVMEDIA_TYPE_AUDIO],
                                        st_index[AVMEDIA_TYPE_VIDEO],
                                        nullptr, 0);
        }
        if (!video_disable && !subtitle_disable) {
            st_index[AVMEDIA_TYPE_SUBTITLE] =
                    av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                        st_index[AVMEDIA_TYPE_SUBTITLE],
                                        (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                         st_index[AVMEDIA_TYPE_AUDIO] :
                                         st_index[AVMEDIA_TYPE_VIDEO]),
                                        nullptr, 0);
        }

        is->show_mode = show_mode;
        if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
            AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
            AVCodecParameters *codecpar = st->codecpar;
            AVRational sar = av_guess_sample_aspect_ratio(ic, st, nullptr);
            if (codecpar->width)
                set_default_window_size(codecpar->width, codecpar->height, sar);
        }

        if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
            ret = stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
        }

        if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
            ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
        }
        if (is->show_mode == SHOW_MODE_NONE)
            is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

        if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
            stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
        }

        if (is->video_stream < 0 && is->audio_stream < 0) {
            av_log(nullptr, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
                   is->filename);
            ret = -1;
            break;
        }

        if (!infinite_buffer && is->realtime) {
            infinite_buffer = true;
        }


        while (true) {
            if (is->abort_request || error) {
                break;
            }

            //todo:: pause
            //todo:: filter
            //todo:: seek
            //todo:: attachment

            /* if the queue are full, no need to read more *//* if the queue are full, no need to read more */
            if (!infinite_buffer && (
                    is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
                    || (
                            stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
                            stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
                            stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)
                    )
            )) {
                SDL_LockMutex(wait_mutex);
                SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
                SDL_UnlockMutex(wait_mutex);
                continue;
            }

            //todo:: pause

            ret = av_read_frame(ic, pkt);
            if (ret < 0) {
                if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                    if (is->video_stream >= 0)
                        packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
                    if (is->audio_stream >= 0)
                        packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
                    if (is->subtitle_stream >= 0)
                        packet_queue_put_nullpacket(&is->subtitleq, pkt, is->subtitle_stream);
                    is->eof = true;
                }

                if (ic->pb && ic->pb->error) {
                    if (autoexit) {
                        error = true;
                    }
                    break;
                }

                SDL_LockMutex(wait_mutex);
                SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
                SDL_UnlockMutex(wait_mutex);
                continue;
            } else {
                is->eof = false;
            }

            /* check if packet is in play range specified by user, then queue, otherwise discard */
            stream_start_time = ic->streams[pkt->stream_index]->start_time;
            pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
            pkt_in_play_range = duration == AV_NOPTS_VALUE ||
                                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                                (double) (start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
                                <= ((double) duration / 1000000);
            if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
                packet_queue_put(&is->audioq, pkt);
            } else if (pkt->stream_index == is->video_stream && pkt_in_play_range) {
                packet_queue_put(&is->videoq, pkt);
            } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
                packet_queue_put(&is->subtitleq, pkt);
            } else {
                av_packet_unref(pkt);
            }
        }


    } while (0);

    if (ic && !is->ic) {
        avformat_close_input(&ic);
    }
    av_packet_free(&pkt);
    if (ret != 0) {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    SDL_DestroyMutex(wait_mutex);
    return ret;
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
        if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0) {
            break;
        }
        if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0) {
            break;
        }
        if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0) {
            break;
        }

        if (packet_queue_init(&is->videoq) < 0 ||
            packet_queue_init(&is->audioq) < 0 ||
            packet_queue_init(&is->subtitleq) < 0) {
            break;
        }

        if (!(is->continue_read_thread = SDL_CreateCond())) {
            av_log(nullptr, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
            break;
        }

        //TODO: init_clock

        init_clock(&is->vidclk, &is->videoq.serial);
        init_clock(&is->audclk, &is->audioq.serial);
        init_clock(&is->extclk, &is->extclk.serial);

        //is->audio_clock_serial = -1;

        //TODO: startup_volume
        /*if (startup_volume < 0)
            av_log(nullptr, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
        if (startup_volume > 100)
            av_log(nullptr, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
        startup_volume = av_clip(startup_volume, 0, 100);
        startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);*/
        is->audio_volume = startup_volume;

        is->muted = false;
        is->av_sync_type = av_sync_type;

        is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
        if (!is->read_tid) {
            av_log(nullptr, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
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

void event_loop(VideoState *cur_stream) {
    while (!cur_stream->abort_request) {
        SDL_Delay(100);
    }
}

int main(int argv, char **args) {

    //初始化网络
    avformat_network_init();

    //初始化SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        av_log(nullptr, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(nullptr, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    //忽略SDL事件
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

    //创建SDL窗口
    window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width,
                              default_height, flags);
    //设置SDL提示
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (window) {
        //创建SDL渲染
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

    event_loop(is);

    return 0;
}