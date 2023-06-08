//
// Created by admin on 2023/6/2.
//
#include "../main.h"

enum DECODE_STATE{
    INIT,
    WAIT,
    STOP
};

void printError(const char *msg, int err) {
    char errStr[256] = {0};
    if (err < 0) {
        av_strerror(err, errStr, sizeof(errStr));
    }
    std::cout << msg << " : " << errStr << std::endl;
}

class Decoder {
public:
    explicit Decoder(const char *url, AVMediaType avMediaType) : m_url(url), m_avMediaType(avMediaType) {}

    ~Decoder() {
        std::cout << "~Decoder" << std::endl;
        if (m_fmt_ctx) {
            avformat_free_context(m_fmt_ctx);
        }

        if (m_cdc_ctx) {
            avcodec_free_context(&m_cdc_ctx);
        }

        if (m_pkt) {
            av_packet_free(&m_pkt);
        }

        if (m_frm) {
            av_frame_free(&m_frm);
        }

        if (m_sws_ctx) {
            sws_freeContext(m_sws_ctx);
        }

        if (m_swr_ctx) {
            swr_free(&m_swr_ctx);
        }
    }

    Decoder(Decoder const &) = delete;

    Decoder &operator=(Decoder const &) = delete;

    void start();

private:
    const char *m_url;
    AVMediaType m_avMediaType;
    int m_stream_idx;

    volatile int m_decode_state = INIT;

    AVFormatContext *m_fmt_ctx = nullptr;
    AVCodecContext *m_cdc_ctx = nullptr;
    const AVCodec *m_cdc = nullptr;
    AVPacket *m_pkt = nullptr;
    AVFrame *m_frm = nullptr;
    SwsContext *m_sws_ctx = nullptr;
    SwrContext *m_swr_ctx = nullptr;
};

class Wrapper {
public:
    explicit Wrapper(Decoder *decoder) : m_decoder(decoder) {}

    ~Wrapper() {
        std::cout << "~Wrapper" << std::endl;
        if (m_thread) {
            m_thread->join();
            delete m_thread;
            m_thread = nullptr;
        }

        if (m_decoder) {
            delete m_decoder;
            m_decoder = nullptr;
        }
    }

    Wrapper(Wrapper const &) = delete;

    Wrapper &operator=(Wrapper const &) = delete;

    void start();


private:
    Decoder *m_decoder;
    std::thread *m_thread;

    static void run(Wrapper *wrapper);
};

void Wrapper::start() {
    m_thread = new std::thread(run, this);
}

void Wrapper::run(Wrapper *wrapper) {
    std::cout << "run" << std::endl;
    wrapper->m_decoder->start();
}

void Decoder::start() {
    int ret = 0;

    do {
        m_fmt_ctx = avformat_alloc_context();

        ret = avformat_open_input(&m_fmt_ctx, m_url, nullptr, nullptr);

        if (ret < 0) {
            printError("avformat_open_input", ret);
            break;
        }

        avformat_find_stream_info(m_fmt_ctx, nullptr);

        m_stream_idx = av_find_best_stream(m_fmt_ctx, m_avMediaType, -1, -1, nullptr, 0);

        m_cdc = avcodec_find_decoder(m_fmt_ctx->streams[m_stream_idx]->codecpar->codec_id);

        m_cdc_ctx = avcodec_alloc_context3(m_cdc);

        avcodec_parameters_to_context(m_cdc_ctx, m_fmt_ctx->streams[m_stream_idx]->codecpar);

        ret = avcodec_open2(m_cdc_ctx, m_cdc, nullptr);
        if (ret < 0) {
            printError("avcodec_open2", ret);
            break;
        }

        av_dump_format(m_fmt_ctx, 0, m_url, 0);

        m_pkt = av_packet_alloc();
        m_frm = av_frame_alloc();

        bool loop_flag = true;
        while (true) {
            if (!loop_flag) {
                break;
            }

            ret = av_read_frame(m_fmt_ctx, m_pkt);
            if (ret == AVERROR_EOF) {
                loop_flag = false;
                avcodec_send_packet(m_cdc_ctx, nullptr);
                //av_packet_unref(m_pkt);
            } else if (m_pkt->stream_index != m_stream_idx) {
                av_packet_unref(m_pkt);
                continue;
            } else if (ret >= 0) {
                avcodec_send_packet(m_cdc_ctx, m_pkt);
                av_packet_unref(m_pkt);
            } else {
                loop_flag = false;
                printError("av_read_frame", ret);
                break;
            }

            while (true) {
                ret = avcodec_receive_frame(m_cdc_ctx, m_frm);
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
                }
            }
        }

    } while (false);
}

int main() {
    const char *url = "../asset/Iron_Man-Trailer_HD.mp4";
    Wrapper v_wrapper(new Decoder(url, AVMEDIA_TYPE_VIDEO));
    Wrapper a_wrapper(new Decoder(url, AVMEDIA_TYPE_AUDIO));

    v_wrapper.start();
    a_wrapper.start();
    return 0;
}