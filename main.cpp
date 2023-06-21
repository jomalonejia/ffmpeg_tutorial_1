#include "main.h"


int main(int argc, char **argv) {
    SDL_AudioSpec audioSpec;
    audioSpec.freq = 44100;
    audioSpec.format = AUDIO_S16SYS;
    audioSpec.channels = 2;
    audioSpec.silence = 0;
    audioSpec.samples = 1024;
    // 因为是推模式，所以这里为 nullptr
    audioSpec.callback = nullptr;

    SDL_AudioDeviceID deviceId;
    if ((deviceId = SDL_OpenAudioDevice(nullptr,0,&audioSpec, nullptr,SDL_AUDIO_ALLOW_ANY_CHANGE)) < 2){
        std::cout << "open audio device failed \n";
    }

    SDL_CloseAudioDevice(deviceId);
    return 0;
}
