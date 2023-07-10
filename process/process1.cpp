#include "../main.h"


int process(const std::string &inputUrl, const std::string &inputUrl2,const std::string &outputUrl) {
    int ret = 0;

    // 打开音频流
    AVFormatContext *ifmt_ctx = NULL;
    ret = avformat_open_input(&ifmt_ctx, inputUrl.c_str(), NULL, NULL);
    if (ret != 0) {
        printf("Couldn't open input stream.\n");
        return -1;
    }
    if (avformat_find_stream_info(ifmt_ctx, NULL) < 0) {
        printf("Couldn't find stream information.\n");
        return -1;
    }
    av_dump_format(ifmt_ctx, 0, inputUrl.c_str(), 0);
    int audio_index = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    AVStream *st = ifmt_ctx->streams[audio_index];
    const AVCodec *codec = NULL;
    codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }
    AVCodecContext *codec_ctx = NULL;
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        exit(1);
    }
    avcodec_parameters_to_context(codec_ctx, ifmt_ctx->streams[audio_index]->codecpar);
    if ((ret = avcodec_open2(codec_ctx, codec, NULL) < 0)) {
        return -1;
    }
    AVPacket *pkt = av_packet_alloc();

    // 打开视频流
    AVFormatContext *ifmt_ctx2 = NULL;
    ret = avformat_open_input(&ifmt_ctx2, inputUrl2.c_str(), NULL, NULL);
    if (ret != 0) {
        printf("Couldn't open input stream %s.\n", inputUrl2.c_str());
        return -1;
    }
    if (avformat_find_stream_info(ifmt_ctx2, NULL) < 0) {
        printf("Couldn't find stream information.\n");
        return -1;
    }
    av_dump_format(ifmt_ctx2, 0, inputUrl2.c_str(), 0);
    int video_index = av_find_best_stream(ifmt_ctx2, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    AVStream *st2 = ifmt_ctx2->streams[video_index];
    const AVCodec *codec2 = NULL;
    codec2 = avcodec_find_decoder(st2->codecpar->codec_id);
    if (!codec2) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }
    AVCodecContext *codec_ctx2 = NULL;
    codec_ctx2 = avcodec_alloc_context3(codec2);
    if (!codec_ctx2) {
        exit(1);
    }
    avcodec_parameters_to_context(codec_ctx2, ifmt_ctx2->streams[video_index]->codecpar);
    if ((ret = avcodec_open2(codec_ctx2, codec2, NULL) < 0)) {
        return -1;
    }
    AVPacket *pkt2 = av_packet_alloc();


    // 打开输出流
    AVFormatContext *ofmt_ctx = NULL;
    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mpegts", outputUrl.c_str());
    if (!ofmt_ctx) {
        printf("Could not create output context\n");
        return -1;
    }
    const AVOutputFormat *ofmt = ofmt_ctx->oformat;
    {
        //创建输出音频流
        AVStream *out = avformat_new_stream(ofmt_ctx, NULL);
        if (!out) {
            return -1;
        }
        //复制配置信息
        AVCodecParameters *codecpar = ifmt_ctx->streams[audio_index]->codecpar;
        avcodec_parameters_copy(out->codecpar, codecpar);
        out->codecpar->codec_tag = 0;
    }
    {
        //创建输出视频流
        AVStream *out = avformat_new_stream(ofmt_ctx, NULL);
        if (!out) {
            return -1;
        }
        //复制配置信息
        AVCodecParameters *codecpar = ifmt_ctx2->streams[video_index]->codecpar;
        avcodec_parameters_copy(out->codecpar, codecpar);
        out->codecpar->codec_tag = 0;
    }
    av_dump_format(ofmt_ctx, 0, outputUrl.c_str(), 1);

    //Open output URL
    if (!(ofmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, outputUrl.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            printf("Could not open output URL '%s'", outputUrl.c_str());
            return -1;
        }
    }
    //Write file header
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        printf("Error occurred when opening output URL\n");
        return -1;
    }

    int duration_audio = 0;
    int duration_video = 0;
    int cur_pts_a = 0;
    int cur_pts_unit = 0;
    bool repeat = false;
    int i = 1;
    // 音视频合成
    while (1) {
        if (duration_audio < duration_video) {
            AVStream *in_stream, *out_stream;

            ret = av_read_frame(ifmt_ctx, pkt);
            cur_pts_a = pkt->pts > 0 ? pkt->pts : cur_pts_a;
            cur_pts_unit = cur_pts_unit > 0 ? cur_pts_unit : cur_pts_a;

            if (ret < 0) {
                break;
            }

            if (pkt->stream_index == audio_index) {
                in_stream = ifmt_ctx->streams[pkt->stream_index];

                out_stream = ofmt_ctx->streams[0];

                /* copy packet */
                av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
                pkt->pos = -1;
                duration_audio += pkt->duration;
                if (repeat) {
                    pkt->pts += cur_pts_a + (cur_pts_unit * i++);
                    pkt->dts = pkt->pts;
                }
                ret = av_interleaved_write_frame(ofmt_ctx, pkt);
                /* pkt is now blank (av_interleaved_write_frame() takes ownership of
                 * its contents and resets pkt), so that no unreferencing is necessary.
                 * This would be different if one used av_write_frame(). */
                if (ret < 0) {
                    fprintf(stderr, "Error muxing packet\n");
                    break;
                }
            } else {
                av_packet_unref(pkt);
            }
        } else {
            AVStream *in_stream, *out_stream;

            ret = av_read_frame(ifmt_ctx2, pkt2);
            if (ret < 0)
                break;

            if (pkt2->stream_index == video_index) {
                in_stream = ifmt_ctx2->streams[pkt2->stream_index];

                pkt2->stream_index = 1;
                out_stream = ofmt_ctx->streams[1];

                av_packet_rescale_ts(pkt2, in_stream->time_base, out_stream->time_base);
                pkt2->pos = -1;
                duration_video += pkt2->duration;
                ret = av_interleaved_write_frame(ofmt_ctx, pkt2);

                if (ret < 0) {
                    fprintf(stderr, "Error muxing packet\n");
                    break;
                }
            } else {
                av_packet_unref(pkt2);
            }

        }





        /*if (av_read_frame(ifmt_ctx2, pkt2) >= 0)
        {
            if (pkt2->stream_index == video_index)
            {
                int video = av_find_best_stream(ofmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
                     av_packet_rescale_ts(pkt2, st2->time_base, ofmt_ctx->streams[video]->time_base);
                av_interleaved_write_frame(ofmt_ctx, pkt2);
            }
        }
        else
        {
            break;
        }

        if (av_read_frame(ifmt_ctx, pkt) >= 0)
        {
            if (pkt->stream_index == audio_index)
            {
                int audio = av_find_best_stream(ofmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

                av_interleaved_write_frame(ofmt_ctx, pkt);
            }
        }*/
    }


    av_write_trailer(ofmt_ctx);


    return 0;
}

namespace fs = std::filesystem;

int main(int argc, char **argv) {
    /*for (auto &p: fs::directory_iterator("D:\\gg\\tmp")) {
        const std::filesystem::path &path = p.path();
        const std::string &filepath = p.path().string();
        const std::string &filename = p.path().filename().string();

        std::cout << filepath << filename << std::endl;
    }*/
    int audio_i = 1;
    for (auto &p: fs::directory_iterator("D:\\gg\\tmp")) {
        if(audio_i > 5){
            audio_i = 1;
        }
        //std::cout << p.path() << "\n";
        const std::string &filepath = p.path().string();
        const std::string &filename = p.path().filename().string();
        if(filename.contains(".mp4")){
            const std::string &outputName = "../asset1/new-"+filename;
            const std::string &audioName = "../asset/" + std::to_string(audio_i++) + ".mp3";
            std::cout << audioName << filepath << filename << outputName;
            process(audioName, filepath,outputName);
        }
    }

    return 0;
}
