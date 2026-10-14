/* The miniaudio implementation (phase 9), configured by vd_miniaudio's definitions (engine/CMakeLists.txt), and the
   C entry points of its MP3 frame decoder (audio/Mp3Frame.h). */
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "audio/Mp3Frame.h"

size_t vdMp3DecoderSize(void) {
    return sizeof(ma_dr_mp3dec);
}

void vdMp3DecoderInit(void* decoder) {
    ma_dr_mp3dec_init((ma_dr_mp3dec*)decoder);
}

int vdMp3DecodeFrame(void* decoder, const unsigned char* data, int size, short* pcm, int* consumed, int* channels,
                     int* sampleRate) {
    ma_dr_mp3dec_frame_info info = {0};
    int samples = ma_dr_mp3dec_decode_frame((ma_dr_mp3dec*)decoder, data, size, pcm, &info);
    *consumed = info.frame_bytes;
    *channels = info.channels;
    *sampleRate = info.sample_rate;
    return samples;
}
