//
// Created by admin on 2023/6/2.
//

#ifndef PROJ_1_MAIN_H
#define PROJ_1_MAIN_H

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>

extern "C" {
#include <libavutil/log.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
#include <libavutil/fifo.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/eval.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavutil/dict.h>
#include <libavutil/parseutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libavutil/bprint.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/avfft.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavfilter/avfilter.h>
}

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#endif //PROJ_1_MAIN_H
