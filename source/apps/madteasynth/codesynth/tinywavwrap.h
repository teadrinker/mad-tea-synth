#ifndef TINYWAVWRAP_H
#define TINYWAVWRAP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tinywav.h"

#define TINYWAVWRAP_NUM_CHANNELS 2
#define TINYWAVWRAP_BLOCK_SIZE 480
#define TINYWAVWRAP_MAX_CHANNELS 64

int wav_save(const char* filename, int rate, int lengthInSamples, float* L, float* R)
  #ifdef TINYWAVWRAP_IMPL
{
  if (!filename || !L || !R || lengthInSamples <= 0 || rate <= 0)
  {
    fprintf(stderr, "Invalid arguments to wav_save.\n");
    return -1;
  }

  TinyWav tw;

  if (tinywav_open_write(&tw, TINYWAVWRAP_NUM_CHANNELS, rate, TW_FLOAT32, TW_SPLIT, filename))
  {
    fprintf(stderr, "Failed to open TinyWav for writing.\n");
    return -1;
  }

  for (int i = 0; i < lengthInSamples; i += TINYWAVWRAP_BLOCK_SIZE)
  {
    int blockSize = (i + TINYWAVWRAP_BLOCK_SIZE <= lengthInSamples) ? TINYWAVWRAP_BLOCK_SIZE : lengthInSamples - i;

    float* samplePtrs[TINYWAVWRAP_NUM_CHANNELS] = {L + i, R + i};

    tinywav_write_f(&tw, samplePtrs, blockSize);
  }

  tinywav_close_write(&tw);
  return 0;
}
#else
  ;
#endif

int wav_load(const char* filename, int* outRate, int* outLengthInSamples, float** outL, float** outR)
#ifdef TINYWAVWRAP_IMPL
{
    if (!filename || !outRate || !outLengthInSamples || !outL || !outR) {
        fprintf(stderr, "Invalid arguments to wav_load.\n");
        return -1;
    }

    *outL = NULL;
    *outR = NULL;

    TinyWav tw;

    // Read interleaved into scratch rather than TW_SPLIT: the split path writes
    // one pointer per channel in the file, so a 2-pointer array would leave R
    // uninitialised for mono and overrun for anything above stereo.
    if (tinywav_open_read(&tw, filename, TW_INTERLEAVED)) {
        fprintf(stderr, "Failed to open TinyWav for reading: %s\n", filename);
        return -1;
    }

    const int nch = tw.numChannels;
    const int length = tw.numFramesInHeader;
    if (nch < 1 || nch > TINYWAVWRAP_MAX_CHANNELS || length <= 0) {
        fprintf(stderr, "Unsupported WAV geometry (%d channels, %d frames): %s\n", nch, length, filename);
        tinywav_close_read(&tw);
        return -1;
    }

    *outRate = tw.h.SampleRate;
    *outLengthInSamples = length;

    *outL = (float*)malloc((size_t)length * sizeof(float));
    *outR = (float*)malloc((size_t)length * sizeof(float));
    float* scratch = (float*)malloc((size_t)nch * TINYWAVWRAP_BLOCK_SIZE * sizeof(float));
    if (!*outL || !*outR || !scratch) {
        fprintf(stderr, "Memory allocation failed.\n");
        tinywav_close_read(&tw);
        free(*outL); *outL = NULL;
        free(*outR); *outR = NULL;
        free(scratch);
        return -1;
    }

    // A mono file feeds both outputs; extra channels beyond stereo are dropped.
    const int rightCh = (nch > 1) ? 1 : 0;
    int filled = 0;
    while (filled < length) {
        int blockSize = (length - filled < TINYWAVWRAP_BLOCK_SIZE) ? length - filled : TINYWAVWRAP_BLOCK_SIZE;
        int got = tinywav_read_f(&tw, scratch, blockSize);
        if (got <= 0) break; // short file: keep what we got rather than leaving the tail uninitialised
        for (int j = 0; j < got; j++) {
            (*outL)[filled + j] = scratch[j * nch];
            (*outR)[filled + j] = scratch[j * nch + rightCh];
        }
        filled += got;
    }

    free(scratch);
    tinywav_close_read(&tw);

    if (filled == 0) {
        fprintf(stderr, "Error reading WAV data: %s\n", filename);
        free(*outL); *outL = NULL;
        free(*outR); *outR = NULL;
        return -1;
    }

    *outLengthInSamples = filled;
    return 0;
}
#else
  ;
#endif

#endif