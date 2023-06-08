//
// Created by admin on 2023/6/2.
//
#include "../main.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

void printError(const char *msg, int err) {
    char errStr[256] = {0};
    if (err < 0) {
        av_strerror(err, errStr, sizeof(errStr));
    }
    std::cout << msg << " : " << errStr << std::endl;
}


int main(int argv, char** args) {

    const char *url_in = "../asset/small_bunny_1080p_60fps.mp4";
    const char *url_out = "../asset/output1.ppm";
    AVMediaType m_avMediaType_v = AVMEDIA_TYPE_VIDEO;
    AVPixelFormat pix_fmt_out = AV_PIX_FMT_YUV420P;

    AVFormatContext *fmt_ctx = nullptr;
    AVCodecContext *cdc_ctx_a = nullptr;
    AVCodecContext *cdc_ctx_v = nullptr;
    const AVCodec *cdc_a = nullptr;
    const AVCodec *cdc_v = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frm = nullptr;
    AVFrame *frm_out = nullptr;
    SwsContext *m_sws_ctx = nullptr;
    SwrContext *m_swr_ctx = nullptr;
    int ret = 0;
    uint8_t *buffer = nullptr;
    int m_stream_idx_v;

    do {
        fmt_ctx = avformat_alloc_context();

        ret = avformat_open_input(&fmt_ctx, url_in, nullptr, nullptr);

        if (ret < 0) {
            printError("avformat_open_input", ret);
            break;
        }

        avformat_find_stream_info(fmt_ctx, nullptr);

        m_stream_idx_v = av_find_best_stream(fmt_ctx, m_avMediaType_v, -1, -1, nullptr, 0);

        cdc_v = avcodec_find_decoder(fmt_ctx->streams[m_stream_idx_v]->codecpar->codec_id);

        cdc_ctx_v = avcodec_alloc_context3(cdc_v);

        avcodec_parameters_to_context(cdc_ctx_v, fmt_ctx->streams[m_stream_idx_v]->codecpar);

        ret = avcodec_open2(cdc_ctx_v, cdc_v, nullptr);
        if (ret < 0) {
            printError("avcodec_open2", ret);
            break;
        }

        av_dump_format(fmt_ctx, 0, url_in, 0);


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
        SDL_Renderer *sdl_renderer = SDL_CreateRenderer(sdl_window, -1,
                                                        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC |
                                                        SDL_RENDERER_TARGETTEXTURE);   // [3]

        // A structure that contains an efficient, driver-specific representation
        // of pixel data.
        // Use this function to create a texture for a rendering context.
        SDL_Texture *sdl_texture = SDL_CreateTexture(
                sdl_renderer,
                SDL_PIXELFORMAT_YV12,
                SDL_TEXTUREACCESS_STREAMING,
                cdc_ctx_v->width,
                cdc_ctx_v->height
        );

        pkt = av_packet_alloc();
        frm = av_frame_alloc();
        frm_out = av_frame_alloc();

        bool loop_flag = true;
        while (true) {
            if (!loop_flag) {
                break;
            }

            ret = av_read_frame(fmt_ctx, pkt);
            if (ret == AVERROR_EOF) {
                loop_flag = false;
                avcodec_send_packet(cdc_ctx_v, nullptr);
                //av_packet_unref(pkt);
            } else if (pkt->stream_index != m_stream_idx_v) {
                av_packet_unref(pkt);
                continue;
            } else if (ret >= 0) {
                avcodec_send_packet(cdc_ctx_v, pkt);
                av_packet_unref(pkt);
            } else {
                loop_flag = false;
                printError("av_read_frame", ret);
                break;
            }

            while (true) {
                ret = avcodec_receive_frame(cdc_ctx_v, frm);
                if (ret == AVERROR(EAGAIN)) {
                    break;
                } else if (ret == AVERROR_EOF) {
                    loop_flag = false;
                    break;
                } else if (ret < 0) {
                    loop_flag = false;
                    break;
                } else {
                    //std::cout << m_avMediaType << std::endl;
                    if (!m_sws_ctx) {
                        int bufferSize = av_image_get_buffer_size(pix_fmt_out, cdc_ctx_v->width,
                                                                  cdc_ctx_v->height, 1);
                        buffer = (uint8_t *) malloc(bufferSize * sizeof(uint8_t));

                        av_image_fill_arrays(frm_out->data, frm_out->linesize,
                                             buffer, pix_fmt_out,
                                             cdc_ctx_v->width, cdc_ctx_v->height, 1);

                        m_sws_ctx = sws_getContext(cdc_ctx_v->width, cdc_ctx_v->height, cdc_ctx_v->pix_fmt,
                                                   cdc_ctx_v->width, cdc_ctx_v->height, pix_fmt_out, SWS_BILINEAR,
                                                   nullptr, nullptr, nullptr);
                    }

                    sws_scale(m_sws_ctx, frm->data, frm->linesize, 0, cdc_ctx_v->height,
                              frm_out->data, frm_out->linesize);

                    double fps = av_q2d(fmt_ctx->streams[m_stream_idx_v]->r_frame_rate);
                    //std::cout << "fps:" << fps << " frame_rate" << std::endl;

                    // get clip sleep time
                    double sleep_time = 1.0/(double)fps;

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
                            frm_out->data[0],      // the raw pixel data for the Y plane
                            frm_out->linesize[0],  // the number of bytes between rows of pixel data for the Y plane
                            frm_out->data[1],      // the raw pixel data for the U plane
                            frm_out->linesize[1],  // the number of bytes between rows of pixel data for the U plane
                            frm_out->data[2],      // the raw pixel data for the V plane
                            frm_out->linesize[2]   // the number of bytes between rows of pixel data for the V plane
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
        if (fmt_ctx) {
            avformat_free_context(fmt_ctx);
        }

        if (cdc_ctx_v) {
            avcodec_free_context(&cdc_ctx_v);
        }

        if (pkt) {
            av_packet_free(&pkt);
        }

        if (frm) {
            av_frame_free(&frm);
        }

        if (frm_out) {
            av_frame_free(&frm_out);
        }

        if (buffer) {
            delete buffer;
            buffer = nullptr;
        }

        if (m_sws_ctx) {
            sws_freeContext(m_sws_ctx);
        }

        if (m_swr_ctx) {
            swr_free(&m_swr_ctx);
        }

        if(sdl_renderer){
            SDL_DestroyRenderer(sdl_renderer);
            SDL_Quit();
        }

    } while (false);
    return 0;
}