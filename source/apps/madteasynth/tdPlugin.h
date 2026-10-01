
/*

	MINIMAL VST EXAMPLE:

		#include "tdPlugin.h"

		enum eMyGainParams { eMyClipParams_Gain_Level, eMyClipParams_Count };
		const char* sMyGainParamNames[1] = {"Gain Level"};

		class cMyGain : public cTDAudioPluginBase {
		public:	virtual void handleSamples (float **inputs, float **outputs, int sampleFrames) {
				for(int c = 0; c < fChannelCount; c++) {
					for(int i = 0; i < sampleFrames; i++) {
						outputs[c][i] = inputs[c][i] * fParam[eMyClipParams_Gain_Level];
					}
				}
			}
		};

		#define VSTPLUGIN_CLASSNAME cMyGain
		#define VSTPLUGIN_PARAMNAMES sMyGainParamNames
		#define VSTPLUGIN_PARAMCOUNT eMyClipParams_Count

		#include "tdPluginVST.h"

*/

// The base class of both synth backends.

#ifndef __tdPlugin__
#define __tdPlugin__

#define VSTPLUGIN_PARAMCOUNT ePluginParams_Count
#define VSTPLUGIN_CLASSNAME cPlugin
#define VSTPLUGIN_VENDOR "tdPlugin"
#define VSTPLUGIN_NAME "testPlugin"
#define VSTPLUGIN_MAGIC 'J5pV'

#define VSTPLUGIN_LATENCY 32
#define VSTPLUGIN_FORCEBLOCKSIZE 32

class cTDAudioPluginOwner {
	public: virtual void paramChangedFromDSP(int param) = 0;
};

class cTDAudioPluginBase {
public:
	cTDAudioPluginOwner* fOwner;

	int		fChannelCount;
	float*	fParam;
	bool*	fParamChanged;
	double	fSR;

	// Host song position in beats at the block start, and beats per sample; set by
	// SteepSynthEngine::ProcessBlock. cCodeSynth exposes it as `beat`.
	double	fSongBeatPos = 0.0;
	double	fSongBeatPerSample = 0.0;

	virtual void handleSamples      (float **inputs, float **outputs, int sampleFrames) = 0;

	virtual void handleMidi         (int frameOffset, int status, int data1, int data2)  { };

	virtual void init               () { } // set default parameters here, if not, they will all be 0.5  (50 %)
	virtual void sampleRateChanged  () { } // this should be ok for allocation
	virtual void getParamAsText     (int index, char* out) { out[0] = '@'; out[1] = 0; } ; // max 8 chars, (9 including zero term, out[8] = 0 is ok) fallbacks to 0 - 100
	virtual void getParamPostFix    (int index, char* out) { out[0] = '@'; out[1] = 0; } ; // max 8 chars, (9 including zero term, out[8] = 0 is ok) fallbacks to %
};
/*
cTDAudioPluginBase::cTDAudioPluginBase(int paramCount, int channelCount) {
	fParam = new float[paramCount];
	fParamChanged = new bool[paramCount];
	for(int i = 0; i < paramCount; i++) {
		fParam[i] = 0.5f;
		fParamChanged[i] = true;
	}
	fSR = 44100.f;
	fChannelCount = channelCount;
}

cTDAudioPluginBase::~cTDAudioPluginBase() {
	delete [] fParam;
	delete [] fParamChanged;
}
*/

static inline void copyArray(float* dst, float* src, int size)
{
	int i;
	for(i = 0; i < size; i++)
	{
		dst[i] = src[i];
	}
}
static inline void mixArray(float* dst_srcA, float* srcB, int size)
{
	int i;
	for(i = 0; i < size; i++)
	{
		dst_srcA[i] += srcB[i];
	}
}
static inline void gainArray(float* dst, float amp, int size)
{
	int i;
	for(i = 0; i < size; i++)
	{
		dst[i] *= amp;
	}
}
static inline void zeroArray(float* dst, int size)
{
	int i;
	for(i = 0; i < size; i++)
	{
		dst[i] = 0;
	}
}

static int exp2(int log2n) { return 1<<log2n; }
static int mini(int a, int b) { return a<b?a:b; }
static float lerp(float c, float a, float b) { return a+c*(b-a); }
static float max(float a, float b) { return a>b?a:b; }
static float min(float a, float b) { return a<b?a:b; }
static float clamp(float v, float mi=0, float ma=1.f) { return v<mi?mi: (v>ma?ma: v); }

#endif