#pragma once

// MPEG audio frame decoder: miniaudio's copy of minimp3, which it keeps in its implementation section, reached
// through these C entry points (audio/MiniaudioImpl.c). The decoder state is plain memory the caller owns.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Most samples per channel one frame decodes to.
#define VD_MP3_MAX_FRAME_SAMPLES 1152

// Bytes of a decoder's state; allocate it 16-byte aligned.
size_t vdMp3DecoderSize(void);
// Resets a decoder (before the first frame of a stream, or to start it over).
void vdMp3DecoderInit(void* decoder);
// Decodes the frame at `data` (junk before it is skipped) into `pcm`: up to VD_MP3_MAX_FRAME_SAMPLES interleaved
// 16-bit frames of `*channels` channels. Returns the frames decoded (0: none from this frame, e.g. the first frames
// filling the bit reservoir); `*consumed` = bytes used, 0 when no frame is left in `size` bytes.
int vdMp3DecodeFrame(void* decoder, const unsigned char* data, int size, short* pcm, int* consumed, int* channels,
                     int* sampleRate);

#ifdef __cplusplus
}
#endif
