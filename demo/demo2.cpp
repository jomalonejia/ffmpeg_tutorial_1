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
}


int main() {
    const char *url_in = "../asset/small_bunny_1080p_60fps.mp4";
    const char *url_out = "../asset/output1.ppm";
    AVMediaType m_avMediaType_v = AVMEDIA_TYPE_VIDEO;
    int pix_fmt_out = AV_PIX_FMT_YUV420P;

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
    uint8_t * buffer = nullptr;
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

        pkt = av_packet_alloc();
        frm = av_frame_alloc();
        frm_out = av_frame_alloc();

        FILE *file = fopen(url_out, "wb");
        if(!file){
            break;
        }

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
                    if(!m_sws_ctx){
                        int bufferSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, cdc_ctx_v->width,
                                                                  cdc_ctx_v->height, 1);
                        buffer = (uint8_t *) malloc(bufferSize * sizeof(uint8_t));

                        av_image_fill_arrays(frm_out->data,frm_out->linesize,
                                             buffer,AV_PIX_FMT_RGB24,
                                             cdc_ctx_v->width,cdc_ctx_v->height, 1);

                        m_sws_ctx = sws_getContext(cdc_ctx_v->width, cdc_ctx_v->height, cdc_ctx_v->pix_fmt,
                                                   cdc_ctx_v->width, cdc_ctx_v->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
                        sws_scale(m_sws_ctx, frm->data, frm->linesize, 0, cdc_ctx_v->height,
                                  frm_out->data, frm_out->linesize);

                        for (int i = 0; i < cdc_ctx_v->height; ++i){
                            fwrite(frm_out->data[0] + i * frm_out->linesize[0], 1, cdc_ctx_v->width * 3, file);
                        }
                    }


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

        if(buffer){
            delete buffer;
            buffer = nullptr;
        }

        if (m_sws_ctx) {
            sws_freeContext(m_sws_ctx);
        }

        if (m_swr_ctx) {
            swr_free(&m_swr_ctx);
        }
        if(file){
            fclose(file);
        }
    } while (false);
    return 0;
}