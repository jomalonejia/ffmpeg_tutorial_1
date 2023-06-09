//
// Created by admin on 2023/6/2.
//
#include "../main.h"

void printError(const char *msg, int err) {
    char errStr[256] = {0};
    if (err < 0) {
        av_strerror(err, errStr, sizeof(errStr));
    }
    std::cout << msg << " : " << errStr << std::endl;
    exit(-1);
}

class demo4 {
public:
    demo4() = default;

    demo4(const char *url) : url_in(url) {}

    ~demo4();

    void init_decoder();

    void init_video_render();

    void init_audio_render();

    void process_packet_video();

    void process_packet_audio();

    void run();

private:

    const char *url_in = "";

    AVMediaType m_avMediaType_v = AVMEDIA_TYPE_VIDEO;
    AVMediaType m_avMediaType_a = AVMEDIA_TYPE_AUDIO;

    AVFormatContext *fmt_ctx = nullptr;
    AVCodecContext *cdc_ctx_a = nullptr;
    AVCodecContext *cdc_ctx_v = nullptr;
    const AVCodec *cdc_a = nullptr;
    const AVCodec *cdc_v = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frm = nullptr;
    AVFrame *frm_render_video = nullptr;
    SwsContext *sws_ctx = nullptr;
    SwrContext *swr_ctx = nullptr;
    int m_stream_idx_v = -1;
    int m_stream_idx_a = -1;

    int ret = 0;


    //video
    AVPixelFormat pix_fmt_out = AV_PIX_FMT_YUV420P;

    SDL_Renderer *sdl_renderer = nullptr;
    SDL_Texture *sdl_texture = nullptr;

    uint8_t *video_buffer = nullptr;


    //audio

    AVChannelLayout in_ch_layout;
    enum AVSampleFormat in_sample_fmt;
    int in_sample_rate = 0;
    int in_nb_samples = 0;

    AVChannelLayout out_ch_layout;
    enum AVSampleFormat out_sample_fmt;
    int out_sample_rate = 0;
    int out_nb_samples = 0;
    int max_out_nb_samples = 0;

    uint8_t **audio_buffer = nullptr;
};

demo4::~demo4() {
    if (fmt_ctx) {
        avformat_free_context(fmt_ctx);
    }

    if (cdc_ctx_v) {
        avcodec_free_context(&cdc_ctx_v);
    }

    if (cdc_ctx_a) {
        avcodec_free_context(&cdc_ctx_a);
    }

    if (pkt) {
        av_packet_free(&pkt);
    }

    if (frm) {
        av_frame_free(&frm);
    }

    if (frm_render_video) {
        av_frame_free(&frm_render_video);
    }

    if (video_buffer) {
        delete video_buffer;
        video_buffer = nullptr;
    }
    if (audio_buffer) {
        delete audio_buffer;
        audio_buffer = nullptr;
    }

    if (sws_ctx) {
        sws_freeContext(sws_ctx);
    }

    if (swr_ctx) {
        swr_free(&swr_ctx);
    }

    if (sdl_renderer) {
        SDL_DestroyRenderer(sdl_renderer);
        SDL_Quit();
    }
}

void demo4::init_decoder() {
    do {
        fmt_ctx = avformat_alloc_context();

        ret = avformat_open_input(&fmt_ctx, url_in, nullptr, nullptr);

        if (ret < 0) {
            printError("avformat_open_input", ret);
            break;
        }

        avformat_find_stream_info(fmt_ctx, nullptr);

        m_stream_idx_v = av_find_best_stream(fmt_ctx, m_avMediaType_v, -1, -1, nullptr, 0);

        m_stream_idx_a = av_find_best_stream(fmt_ctx, m_avMediaType_a, -1, -1, nullptr, 0);

        cdc_v = avcodec_find_decoder(fmt_ctx->streams[m_stream_idx_v]->codecpar->codec_id);

        cdc_a = avcodec_find_decoder(fmt_ctx->streams[m_stream_idx_a]->codecpar->codec_id);

        cdc_ctx_v = avcodec_alloc_context3(cdc_v);

        cdc_ctx_a = avcodec_alloc_context3(cdc_a);

        avcodec_parameters_to_context(cdc_ctx_v, fmt_ctx->streams[m_stream_idx_v]->codecpar);

        avcodec_parameters_to_context(cdc_ctx_a, fmt_ctx->streams[m_stream_idx_a]->codecpar);

        ret = avcodec_open2(cdc_ctx_v, cdc_v, nullptr);
        if (ret < 0) {
            printError("avcodec_open2 video", ret);
            break;
        }

        ret = avcodec_open2(cdc_ctx_a, cdc_a, nullptr);
        if (ret < 0) {
            printError("avcodec_open2 audio", ret);
            break;
        }

        pkt = av_packet_alloc();
        frm = av_frame_alloc();
        frm_render_video = av_frame_alloc();

        av_dump_format(fmt_ctx, 0, url_in, 0);
    } while (false);
}

void demo4::init_video_render() {
    do {
        ret = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
        if (0 != ret) {
            printf("Could not initialize SDL - %s\n.", SDL_GetError());
        }

        SDL_Window *sdl_window = SDL_CreateWindow("jj", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                                  cdc_ctx_v->width / 2, cdc_ctx_v->height / 2,
                                                  SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);

        if (!sdl_window) {
            printf("SDL: could not set video mode - exiting.\n");
            break;
        }

        //
        SDL_GL_SetSwapInterval(1);

        // A structure that contains a rendering state.
        // Use this function to create a 2D rendering context for a window.
        sdl_renderer = SDL_CreateRenderer(sdl_window, -1,
                                          SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC |
                                          SDL_RENDERER_TARGETTEXTURE);   // [3]

        // A structure that contains an efficient, driver-specific representation
        // of pixel data.
        // Use this function to create a texture for a rendering context.
        sdl_texture = SDL_CreateTexture(
                sdl_renderer,
                SDL_PIXELFORMAT_YV12,
                SDL_TEXTUREACCESS_STREAMING,
                cdc_ctx_v->width,
                cdc_ctx_v->height
        );

        if (!sws_ctx) {
            int bufferSize = av_image_get_buffer_size(pix_fmt_out, cdc_ctx_v->width,
                                                      cdc_ctx_v->height, 1);
            video_buffer = (uint8_t *) malloc(bufferSize * sizeof(uint8_t));

            av_image_fill_arrays(frm_render_video->data, frm_render_video->linesize,
                                 video_buffer, pix_fmt_out,
                                 cdc_ctx_v->width, cdc_ctx_v->height, 1);

            sws_ctx = sws_getContext(cdc_ctx_v->width, cdc_ctx_v->height, cdc_ctx_v->pix_fmt,
                                     cdc_ctx_v->width, cdc_ctx_v->height, pix_fmt_out,
                                     SWS_BILINEAR,
                                     nullptr, nullptr, nullptr);
        }
    } while (false);
}

void demo4::init_audio_render() {
    do {
        if (!swr_ctx) {
            swr_ctx = swr_alloc();

            av_channel_layout_copy(&in_ch_layout, &cdc_ctx_a->ch_layout);
            av_channel_layout_copy(&out_ch_layout, &in_ch_layout);

            in_sample_fmt = cdc_ctx_a->sample_fmt;
            out_sample_fmt = in_sample_fmt;

            in_sample_rate = cdc_ctx_a->sample_rate;
            out_sample_rate = cdc_ctx_a->sample_rate;

            ret = swr_alloc_set_opts2(&swr_ctx,
                                      &out_ch_layout,
                                      in_sample_fmt,
                                      in_sample_rate,
                                      &in_ch_layout,
                                      out_sample_fmt,
                                      out_sample_rate,
                                      0,
                                      nullptr);

            if (ret < 0) {
                printError("swr_alloc_set_opts2 ", ret);
                break;
            }

            ret = swr_init(swr_ctx);
            if (ret < 0) {
                printError("swr_init ", ret);
                break;
            }
        }
    } while (0);
}

void demo4::process_packet_video() {
    while (true) {
        ret = avcodec_receive_frame(cdc_ctx_v, frm);
        if (ret == AVERROR(EAGAIN)) {
            break;
        } else if (ret == AVERROR_EOF) {
            //loop_flag = false;
            break;
        } else if (ret < 0) {
            break;
        } else {

            sws_scale(sws_ctx, frm->data, frm->linesize, 0, cdc_ctx_v->height,
                      frm_render_video->data, frm_render_video->linesize);

            double fps = av_q2d(fmt_ctx->streams[m_stream_idx_v]->r_frame_rate);
            //std::cout << "fps:" << fps << " frame_rate" << std::endl;

            // get clip sleep time
            double sleep_time = 1.0 / (double) fps;

            // sleep: usleep won't work when using SDL_CreateWindow
            // usleep(sleep_time);
            // Use SDL_Delay in milliseconds to allow for cpu scheduling
            SDL_Delay((1000 * sleep_time) - 10);    // [5]

            // The simplest struct in SDL. It contains only four shorts. x, y which
            // holds the position and w, h which holds width and height.It's important
            // to note that 0, 0 is the upper-left corner in SDL. So a higher y-value
            // means lower, and the bottom-right corner will have the coordinate x + w,
            // y + h.
            SDL_Rect rect;
            rect.x = 0;
            rect.y = 0;
            rect.w = cdc_ctx_v->width;
            rect.h = cdc_ctx_v->height;

            // Use this function to update a rectangle within a planar
            // YV12 or IYUV texture with new pixel data.
            SDL_UpdateYUVTexture(
                    sdl_texture,            // the texture to update
                    &rect,              // a pointer to the rectangle of pixels to update, or NULL to update the entire texture
                    frm_render_video->data[0],      // the raw pixel data for the Y plane
                    frm_render_video->linesize[0],  // the number of bytes between rows of pixel data for the Y plane
                    frm_render_video->data[1],      // the raw pixel data for the U plane
                    frm_render_video->linesize[1],  // the number of bytes between rows of pixel data for the U plane
                    frm_render_video->data[2],      // the raw pixel data for the V plane
                    frm_render_video->linesize[2]   // the number of bytes between rows of pixel data for the V plane
            );

            // clear the current rendering target with the drawing color
            SDL_RenderClear(sdl_renderer);

            // copy a portion of the texture to the current rendering target
            SDL_RenderCopy(
                    sdl_renderer,   // the rendering context
                    sdl_texture,    // the source texture
                    NULL,       // the source SDL_Rect structure or NULL for the entire texture
                    NULL        // the destination SDL_Rect structure or NULL for the entire rendering
                    // target; the texture will be stretched to fill the given rectangle
            );

            // update the screen with any rendering performed since the previous call
            SDL_RenderPresent(sdl_renderer);

        }
    }
}

void demo4::process_packet_audio() {
    while (true) {
        ret = avcodec_receive_frame(cdc_ctx_a, frm);
        if (ret == AVERROR(EAGAIN)) {
            break;
        } else if (ret == AVERROR_EOF) {
            //loop_flag = false;
            break;
        } else if (ret < 0) {
            break;
        } else {
            if (swr_ctx) {
                if (!audio_buffer) {

                    in_nb_samples = frm->nb_samples;

                    max_out_nb_samples = out_nb_samples = av_rescale_rnd(
                            in_nb_samples,
                            out_sample_rate,
                            in_sample_rate,
                            AV_ROUND_UP
                    );

                    // check rescaling was successful
                    if (max_out_nb_samples <= 0) {
                        printf("av_rescale_rnd error.\n");
                        exit(-1);
                    }

                    ret = av_samples_alloc_array_and_samples(
                            &audio_buffer,
                            nullptr,
                            out_ch_layout.nb_channels,
                            out_nb_samples,
                            out_sample_fmt,
                            1
                    );

                    if (ret < 0) {
                        printError("av_samples_alloc_array_and_samples", ret);
                        break;
                    }

                    int64_t delay = swr_get_delay(swr_ctx, in_sample_rate);

                    // retrieve output samples number taking into account the progressive delay
                    out_nb_samples = av_rescale_rnd(
                            swr_get_delay(swr_ctx, in_sample_rate) + in_nb_samples,
                            out_sample_rate,
                            in_sample_rate,
                            AV_ROUND_UP
                    );

                    // check output samples number was correctly retrieved
                    if (out_nb_samples <= 0) {
                        printf("av_rescale_rnd error\n");
                        exit(-1);
                    }

                    if (out_nb_samples > max_out_nb_samples) {
                        // free memory block and set pointer to NULL
                        av_free(audio_buffer[0]);

                        // Allocate a samples buffer for out_nb_samples samples
                        ret = av_samples_alloc(
                                audio_buffer,
                                nullptr,
                                out_ch_layout.nb_channels,
                                out_nb_samples,
                                out_sample_fmt,
                                1
                        );

                        // check samples buffer correctly allocated
                        if (ret < 0) {
                            printf("av_samples_alloc failed.\n");
                            exit(-1);
                        }

                        max_out_nb_samples = out_nb_samples;
                    }
                }

                // do the actual audio data resampling
                ret = swr_convert(
                        swr_ctx,
                        audio_buffer,
                        out_nb_samples,
                        (const uint8_t **) frm->data,
                        frm->nb_samples
                );

                // check audio conversion was successful
                if (ret < 0) {
                    printError("swr_convert", ret);
                    break;
                }

            } else {
                printf("swr_ctx null error.\n");
                exit(-1);
            }
        }
    }
}

void demo4::run() {
    do {

        init_decoder();

        init_video_render();
        init_audio_render();

        bool loop_flag = true;
        while (true) {

            SDL_Event sdl_event;
            SDL_PollEvent(&sdl_event);
            switch (sdl_event.type) {
                case SDL_QUIT:
                    loop_flag = false;
                    break;
            }

            if (!loop_flag) {
                break;
            }

            ret = av_read_frame(fmt_ctx, pkt);
            if (ret == AVERROR_EOF) {
                loop_flag = false;
                avcodec_send_packet(cdc_ctx_v, nullptr);
            } else if (ret >= 0) {
                if (pkt->stream_index == m_stream_idx_v) {
                    avcodec_send_packet(cdc_ctx_v, pkt);
                    av_packet_unref(pkt);
                    process_packet_video();
                } else if (pkt->stream_index == m_stream_idx_a) {
                    avcodec_send_packet(cdc_ctx_a, pkt);
                    av_packet_unref(pkt);
                    process_packet_audio();
                } else {
                    av_packet_unref(pkt);
                    continue;
                }
            } else {
                loop_flag = false;
                printError("av_read_frame", ret);
                break;
            }

        }

    } while (false);
}

int main(int argc, char **argv) {
    //const char *url_in = "../asset/small_bunny_1080p_60fps.mp4";
    const char *url_in = "../asset/juren-30s.mp4";

    demo4 *demo = new demo4(url_in);
    demo->run();
    av_freep(demo);
    delete demo;
    demo = nullptr;
    return 0;
}