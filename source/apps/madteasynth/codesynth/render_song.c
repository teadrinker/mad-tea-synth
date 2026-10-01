// Renders song.c to a 16-bit PCM WAV. Used by render_song.bat in-tree and by the
// Wav export targets. Writes the WAV itself: tinywavwrap.h doesn't compile as C.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Included directly so song_SongState's per-export size is visible.
#include <song.c>

// Fallbacks for a generated pair without song_config.h or SONG_OUTPUT_CHANNELS,
// like teatime's song.c.
#ifndef SONG_SAMPLE_RATE
#define SONG_SAMPLE_RATE 44100
#endif
#ifndef SONG_OUTPUT_CHANNELS
#define SONG_OUTPUT_CHANNELS 1
#endif

static void write_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void write_u16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

// `frames` counts frames; stereo is interleaved L,R as song_render_audio_batch
// produces it.
static int wav_save_pcm16(const char* path, int rate, int channels,
                          const short int* samples, int frames)
{
    FILE* f = fopen(path, "wb");
    if (!f) return -1;

    uint32_t dataBytes = (uint32_t)frames * (uint32_t)channels * sizeof(short int);
    fwrite("RIFF", 1, 4, f);
    write_u32(f, 36 + dataBytes);
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    write_u32(f, 16);                                    // fmt chunk size
    write_u16(f, 1);                                     // PCM
    write_u16(f, (uint16_t)channels);
    write_u32(f, (uint32_t)rate);
    write_u32(f, (uint32_t)rate * (uint32_t)channels * 2); // byte rate
    write_u16(f, (uint16_t)(channels * 2));              // block align
    write_u16(f, 16);                                    // bits per sample

    fwrite("data", 1, 4, f);
    write_u32(f, dataBytes);
    fwrite(samples, sizeof(short int), (size_t)frames * (size_t)channels, f);

    int ok = !ferror(f);
    fclose(f);
    return ok ? 0 : -1;
}

// Usage: render_song [seconds] [out.wav]
// Seconds omitted or <= 0 renders the whole song (song_total_samples()).
//   render_song                  whole song -> song.wav
//   render_song 0 mysong.wav     whole song -> mysong.wav
//   render_song 8 preview.wav    first 8s   -> preview.wav
int main(int argc, char** argv)
{
    const char* outPath = (argc > 2) ? argv[2] : "song.wav";

    double seconds = (argc > 1) ? atof(argv[1]) : 0.0;
    int totalFrames = (seconds > 0.0) ? (int)(seconds * (double)SONG_SAMPLE_RATE)
                                      : song_total_samples();
    if (totalFrames < 0) totalFrames = 0;
    // Rounded up to a whole BATCHSIZE.
    const int kRenderBatchSize = BATCHSIZE;
    totalFrames = ((totalFrames + kRenderBatchSize - 1) / kRenderBatchSize) * kRenderBatchSize;
    if (totalFrames < kRenderBatchSize) totalFrames = kRenderBatchSize;

    // frames * channels: a mono-sized buffer would overrun on a stereo song.
    const size_t sampleCount = (size_t)totalFrames * (size_t)SONG_OUTPUT_CHANNELS;
    short int* pcm = (short int*)malloc(sampleCount * sizeof(short int));
    if (!pcm) {
        fprintf(stderr, "Out of memory allocating %d frames x %d channel(s).\n",
                totalFrames, SONG_OUTPUT_CHANNELS);
        return 1;
    }

    song_SongState state = {0};
    for (int frameId = 0; frameId < totalFrames; frameId += BATCHSIZE)
        song_render_audio_batch(frameId, &state, pcm + (size_t)frameId * SONG_OUTPUT_CHANNELS);

    if (wav_save_pcm16(outPath, SONG_SAMPLE_RATE, SONG_OUTPUT_CHANNELS, pcm, totalFrames) != 0) {
        fprintf(stderr, "Failed to write %s\n", outPath);
        free(pcm);
        return 1;
    }

    printf("Wrote %s: %d frames (%.2fs) at %d Hz, %s.\n",
           outPath, totalFrames, (double)totalFrames / (double)SONG_SAMPLE_RATE,
           SONG_SAMPLE_RATE, SONG_OUTPUT_CHANNELS == 2 ? "stereo" : "mono");

    free(pcm);
    return 0;
}
