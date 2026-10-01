#define PLUG_NAME "Mad Tea Synth"
#define PLUG_MFR "teadrinker"
#define PLUG_VERSION_HEX 0x00010000
#define PLUG_VERSION_STR "1.0.0"
#define PLUG_UNIQUE_ID 'MdTS'
#define PLUG_MFR_ID 'TdRk'
#define PLUG_URL_STR "https://github.com/teadrinker"
#define PLUG_EMAIL_STR "theteadrinker@gmail.com"
#define PLUG_COPYRIGHT_STR "Copyright 2026 teadrinker"
#define PLUG_CLASS_NAME SteepSynth

#define BUNDLE_NAME "madteasynth"
#define BUNDLE_MFR "teadrinker"
#define BUNDLE_DOMAIN "com"

#define SHARED_RESOURCES_SUBPATH "madteasynth"

#define PLUG_CHANNEL_IO "0-2"

#define PLUG_LATENCY 0
#define PLUG_TYPE 1
#define PLUG_DOES_MIDI_IN 1
#define PLUG_DOES_MIDI_OUT 0
#define PLUG_DOES_MPE 0
// The sounds live in the state chunk; this makes VST2/AUv2 chunk too.
#define PLUG_DOES_STATE_CHUNKS 1
#define PLUG_HAS_UI 1
// Leaves a usable editor beside the widest screen panel (microw8: 336 logical units).
#define PLUG_WIDTH 1356
#define PLUG_HEIGHT 702
#define PLUG_FPS 60
#define PLUG_SHARED_RESOURCES 0
#define PLUG_HOST_RESIZE 0

#define AUV2_ENTRY SteepSynth_Entry
#define AUV2_ENTRY_STR "SteepSynth_Entry"
#define AUV2_FACTORY SteepSynth_Factory
#define AUV2_VIEW_CLASS SteepSynth_View
#define AUV2_VIEW_CLASS_STR "SteepSynth_View"

#define AAX_TYPE_IDS 'IEF1', 'IEF2'
#define AAX_TYPE_IDS_AUDIOSUITE 'IEA1', 'IEA2'
#define AAX_PLUG_MFR_STR "teadrinker"
#define AAX_PLUG_NAME_STR "Mad Tea Synth\nMdTS"
#define AAX_PLUG_CATEGORY_STR "Synth"
#define AAX_DOES_AUDIOSUITE 1

#define VST3_SUBCATEGORY "Instrument Synth"
#define VST3_NUM_CC_CHANS 0

#define CLAP_MANUAL_URL "https://github.com/teadrinker/madteasynth/manual"
#define CLAP_SUPPORT_URL "https://github.com/teadrinker/madteasynth/support"
#define CLAP_DESCRIPTION "A software-rasterized textmode synth plugin"
#define CLAP_FEATURES "instrument", "synth"

#define APP_NUM_CHANNELS 2
#define APP_N_VECTOR_WAIT 0
#define APP_MULT 1
#define APP_COPY_AUV3 0
#define APP_SIGNAL_VECTOR_SIZE 64

#define ROBOTO_FN "Roboto-Regular.ttf"
