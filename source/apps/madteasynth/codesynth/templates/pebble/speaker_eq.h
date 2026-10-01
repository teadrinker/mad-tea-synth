// The exported app's speaker-compensation EQ, included by song_config.h.
// Belongs to this target, tuned on the watch's speaker.
#ifndef SONG_SPEAKER_EQ_H
#define SONG_SPEAKER_EQ_H

// The compare slider's four fixed positions: references to A/B against, not
// presets. FILT_REF_* is also what the release build bakes in; FILT4_REF_* is
// no EQ.
#define FILT_REF_0 2563
#define FILT_REF_1 23739
#define FILT_REF_2 60289
#define FILT_REF_3 1448
#define FILT_REF_4 28253
#define FILT_REF_5 59387
#define FILT_REF_6 724
#define FILT_REF_7 34369
#define FILT_REF_8 48615
#define FILT_REF_PREGAIN 208

#define FILT2_REF_0 47756
#define FILT2_REF_1 65535
#define FILT2_REF_2 0
#define FILT2_REF_3 2589
#define FILT2_REF_4 12295
#define FILT2_REF_5 45481
#define FILT2_REF_6 1152
#define FILT2_REF_7 1140
#define FILT2_REF_8 44168
#define FILT2_REF_PREGAIN 3608

#define FILT3_REF_0 57566
#define FILT3_REF_1 65535
#define FILT3_REF_2 0
#define FILT3_REF_3 167
#define FILT3_REF_4 22281
#define FILT3_REF_5 0
#define FILT3_REF_6 18691
#define FILT3_REF_7 30371
#define FILT3_REF_8 0
#define FILT3_REF_PREGAIN 10311

// No EQ at all: 
#define FILT4_REF_0 0
#define FILT4_REF_1 0
#define FILT4_REF_2 32768
#define FILT4_REF_3 0
#define FILT4_REF_4 0
#define FILT4_REF_5 32768
#define FILT4_REF_6 0
#define FILT4_REF_7 0
#define FILT4_REF_8 32768
#define FILT4_REF_PREGAIN 3608

#endif // SONG_SPEAKER_EQ_H
