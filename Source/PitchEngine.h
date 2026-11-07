#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

// Pitch correction engine. No framework dependencies, so it can be built and
// tested on its own (see tests/engine_test.cpp).
//
// Signal flow:
//   1. Pitch detection on a 46 ms window every 10 ms (normalised
//      autocorrelation with parabolic peak interpolation).
//   2. Pitch marks are placed one period apart on the input signal. Each mark
//      stores the corrected target frequency for that point in time.
//   3. Output is built with TD-PSOLA: windowed two-period grains are taken at
//      the marks and overlap-added at the corrected period. The grain read
//      speed is controlled by the Formant parameter.
//   4. Harmony voices run the same synthesis with their own clocks.
//
// The output is delayed by latencySamples() relative to the input. The dry
// signal used for the Mix control is delayed by the same amount.

namespace apc
{

struct EngineParams
{
    float retune    = 20.0f;   // 0..100
    float flexTune  = 0.0f;    // 0..100
    float humanize  = 0.0f;    // 0..100
    float vibrato   = 0.0f;    // -12..12
    int   key       = 0;       // 0..11
    int   scale     = 1;       // 0 chromatic, 1 major, 2 minor
    int   inputType = 1;       // 0 soprano, 1 alto/tenor, 2 low male, 3 instrument
    float tracking  = 50.0f;   // 0..100
    int   transpose = 0;       // semitones
    float refHz     = 440.0f;  // reference pitch of A4
    float formant   = 1.0f;    // 0..1, 1 = formants preserved
    float mix       = 1.0f;    // 0..1
    bool  classic   = false;
    bool  harmonyOn = false;

    int   hInterval[4] = { 7, 6, 2, 0 };                    // index into the interval table
    float hLevelDb[4]  = { -6.0f, -6.0f, -6.0f, -6.0f };
    float hPan[4]      = { -0.5f, 0.5f, -1.0f, 1.0f };      // -1..1
};

struct Meters
{
    std::atomic<float> detectedMidi { -1.0f };  // fractional MIDI note of the input, < 0 when unvoiced
    std::atomic<float> targetMidi   { -1.0f };  // corrected MIDI note
    std::atomic<float> cents        { 0.0f };   // applied correction in cents (-100..100)
};

class Engine
{
public:
    void prepare (double sampleRate);
    void process (float* left, float* right, int numSamples, const EngineParams& params);

    int latencySamples() const noexcept { return latency; }
    static int latencyFor (double sampleRate, int inputType);

    Meters meters;

private:
    struct Mark  { double pos = 0.0; float period = 0.0f; float targetHz = 0.0f; };
    struct Voice { double nextSynth = 0.0; bool active = false; };

    static constexpr int kRing     = 1 << 16;
    static constexpr int kMask     = kRing - 1;
    static constexpr int kMaxMarks = 1024;
    static constexpr int kVoices   = 5;   // main + 4 harmonies

    void   reset();
    void   analyse (const EngineParams& p);
    void   generateMarks (const EngineParams& p);
    double findPeak (double centre, double halfSpan) const;
    float  tuneFor (float inputHz, const EngineParams& p) const;
    const Mark* nearestMark (double pos, double tolerance) const;
    void   makeGrain (int voice, const Mark& mark, double centre, const EngineParams& p,
                      double hcap, bool& produced);

    double sr = 44100.0;
    int    latency = 0;

    std::vector<float> inL, inR, inM, olaL, olaR;
    int64_t inCount  = 0;
    int64_t outCount = 0;

    // detector
    int decim = 1, winSize = 0, winDec = 0, hop = 441;
    std::vector<float>  xd, nsdf;
    std::vector<double> pre;
    double periodEst = 0.0;
    bool   voiced = false;
    int    missCount = 0;

    // marks
    std::vector<Mark> marks;
    int64_t markCount = 0;
    bool    haveMark = false;
    double  lastMarkPos = 0.0;

    // synthesis
    Voice  voices[kVoices];
    bool   mainActive = false;
    double smoothFreq = 220.0;
    double curTarget  = 220.0;
    double vibPhase   = 0.0;
    double vibRatio   = 1.0;
    float  voicedGain = 0.0f;
};

} // namespace apc
