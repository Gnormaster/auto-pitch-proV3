#include "PitchEngine.h"

#include <algorithm>
#include <cmath>

namespace apc
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    struct Range { float minF, maxF; };
    const Range kRanges[4] = { { 160.0f, 1200.0f },    // soprano
                               {  90.0f,  700.0f },    // alto / tenor
                               {  60.0f,  400.0f },    // low male
                               {  70.0f, 1200.0f } };  // instrument

    const float kIntervals[8] = { -12.0f, -7.0f, -5.0f, -3.0f, 3.0f, 5.0f, 7.0f, 12.0f };

    inline double clampd (double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
    inline int    clampi (int v, int lo, int hi)          { return v < lo ? lo : (v > hi ? hi : v); }

    // Snap a fractional MIDI note to the chromatic grid or the selected scale.
    float quantise (float note, int key, int scale)
    {
        if (scale == 0)
            return std::floor (note + 0.5f);

        static const int major[7] = { 0, 2, 4, 5, 7, 9, 11 };
        static const int minor[7] = { 0, 2, 3, 5, 7, 8, 10 };
        const int* degrees = (scale == 1) ? major : minor;

        const int n   = (int) std::floor (note);
        const int pc  = ((n % 12) + 12) % 12;
        const int rel = (((pc - key) % 12) + 12) % 12;

        int bestDist = 99, bestDeg = 0;
        for (int d = 0; d < 7; ++d)
        {
            const int dist = std::abs (degrees[d] - rel);
            if (dist < bestDist) { bestDist = dist; bestDeg = degrees[d]; }
        }
        return (float) (n + (bestDeg - rel));
    }
}

int Engine::latencyFor (double sampleRate, int inputType)
{
    const Range r = kRanges[clampi (inputType, 0, 3)];
    return (int) std::ceil (3.6 * sampleRate / r.minF) + 8;
}

void Engine::prepare (double sampleRate)
{
    sr = sampleRate;

    inL.assign ((size_t) kRing, 0.0f);
    inR.assign ((size_t) kRing, 0.0f);
    inM.assign ((size_t) kRing, 0.0f);
    olaL.assign ((size_t) kRing, 0.0f);
    olaR.assign ((size_t) kRing, 0.0f);
    marks.assign ((size_t) kMaxMarks, Mark());

    decim   = sr > 100000.0 ? 4 : (sr > 50000.0 ? 2 : 1);
    winDec  = (int) (sr * 0.0464) / decim;
    winSize = winDec * decim;
    hop     = std::max (64, (int) (sr * 0.01));

    xd.assign ((size_t) winDec, 0.0f);
    nsdf.assign ((size_t) winDec + 2, 0.0f);
    pre.assign ((size_t) winDec + 1, 0.0);

    latency = latencyFor (sr, 1);
    reset();
}

void Engine::reset()
{
    std::fill (inL.begin(), inL.end(), 0.0f);
    std::fill (inR.begin(), inR.end(), 0.0f);
    std::fill (inM.begin(), inM.end(), 0.0f);
    std::fill (olaL.begin(), olaL.end(), 0.0f);
    std::fill (olaR.begin(), olaR.end(), 0.0f);

    inCount = outCount = 0;
    periodEst = 0.0;
    voiced = false;
    missCount = 0;
    markCount = 0;
    haveMark = false;
    lastMarkPos = 0.0;
    mainActive = false;
    smoothFreq = curTarget = 220.0;
    vibPhase = 0.0;
    vibRatio = 1.0;
    voicedGain = 0.0f;
    for (auto& v : voices) { v.nextSynth = 0.0; v.active = false; }
    voices[0].active = true;

    meters.detectedMidi.store (-1.0f);
    meters.targetMidi.store (-1.0f);
    meters.cents.store (0.0f);
}

// ---------------------------------------------------------------- detection

void Engine::analyse (const EngineParams& p)
{
    const Range rg = kRanges[clampi (p.inputType, 0, 3)];
    const int   Nd = winDec;

    double energy = 0.0;
    for (int j = 0; j < Nd; ++j)
    {
        double s = 0.0;
        for (int k = 0; k < decim; ++k)
            s += inM[(size_t) ((inCount - winSize + (int64_t) j * decim + k) & kMask)];
        xd[(size_t) j] = (float) (s / decim);
        energy += (double) xd[(size_t) j] * xd[(size_t) j];
    }

    const double rms = std::sqrt (energy / Nd);
    bool   voicedNow = false;
    double periodNow = 0.0;

    if (rms > 0.004)
    {
        pre[0] = 0.0;
        for (int j = 0; j < Nd; ++j)
            pre[(size_t) j + 1] = pre[(size_t) j] + (double) xd[(size_t) j] * xd[(size_t) j];

        const double srd    = sr / decim;
        const int    lagMin = std::max (2, (int) std::floor (srd / rg.maxF));
        const int    lagMax = std::min (Nd / 2 - 3, (int) std::ceil (srd / rg.minF));

        float best = -1.0f;
        for (int lag = lagMin - 1; lag <= lagMax + 1; ++lag)
        {
            const int lim = Nd - lag;
            float r = 0.0f;
            for (int j = 0; j < lim; j += 2)
                r += xd[(size_t) j] * xd[(size_t) (j + lag)];

            const double m = pre[(size_t) lim] + (pre[(size_t) Nd] - pre[(size_t) lag]);
            const float  v = m > 1.0e-9 ? (float) (4.0 * (double) r / m) : 0.0f;
            nsdf[(size_t) lag] = v;
            if (lag >= lagMin && lag <= lagMax && v > best)
                best = v;
        }

        if (best > 0.0f)
        {
            const float thr = 0.9f * best;
            int pick = -1;
            for (int lag = lagMin; lag <= lagMax; ++lag)
            {
                const float v = nsdf[(size_t) lag];
                if (v > nsdf[(size_t) lag - 1] && v >= nsdf[(size_t) lag + 1] && v >= thr)
                {
                    pick = lag;
                    break;
                }
            }

            if (pick > 0)
            {
                const double a = nsdf[(size_t) pick - 1], b = nsdf[(size_t) pick], c = nsdf[(size_t) pick + 1];
                const double den = a - 2.0 * b + c;
                const double shift = std::fabs (den) > 1.0e-9 ? clampd (0.5 * (a - c) / den, -1.0, 1.0) : 0.0;
                periodNow = (pick + shift) * decim;

                const double clarityThr = 0.85 - 0.004 * clampd (p.tracking, 0.0, 100.0);
                voicedNow = b >= clarityThr;
            }
        }
    }

    if (voicedNow)
    {
        missCount = 0;
        const double alpha = 0.3 + 0.5 * clampd (p.tracking, 0.0, 100.0) / 100.0;
        if (!voiced || periodEst <= 0.0 || std::fabs (periodNow / periodEst - 1.0) > 0.2)
            periodEst = periodNow;
        else
            periodEst += alpha * (periodNow - periodEst);
        voiced = true;
    }
    else if (++missCount >= 3)
    {
        voiced = false;
    }
}

// ------------------------------------------------------------ tuning logic

float Engine::tuneFor (float inputHz, const EngineParams& p) const
{
    const float note = inputHz <= 20.0f ? 60.0f
                                        : 69.0f + 12.0f * std::log2 (inputHz / p.refHz);

    float target = quantise (note, clampi (p.key, 0, 11), clampi (p.scale, 0, 2));

    if (p.flexTune > 0.0f && std::fabs (note - target) < p.flexTune * 0.005f)
        target = note;

    if (p.humanize > 0.0f)
        target += (note - target) * p.humanize * 0.002f;

    return p.refHz * std::pow (2.0f, (target + (float) p.transpose - 69.0f) / 12.0f);
}

// ------------------------------------------------------------- pitch marks

double Engine::findPeak (double centre, double halfSpan) const
{
    const int64_t lo = std::max<int64_t> (1, (int64_t) std::ceil (centre - halfSpan));
    const int64_t hi = std::min<int64_t> (inCount - 2, (int64_t) std::floor (centre + halfSpan));

    double bestVal = -1.0e30;
    int64_t bestPos = (int64_t) centre;
    for (int64_t i = lo; i <= hi; ++i)
    {
        const double v = (double) inM[(size_t) ((i - 1) & kMask)]
                       + 2.0 * (double) inM[(size_t) (i & kMask)]
                       + (double) inM[(size_t) ((i + 1) & kMask)];
        if (v > bestVal) { bestVal = v; bestPos = i; }
    }
    return (double) bestPos;
}

void Engine::generateMarks (const EngineParams& p)
{
    if (!voiced || periodEst <= 0.0)
    {
        haveMark = false;
        return;
    }

    const double T0 = periodEst;
    auto push = [&] (double pos)
    {
        Mark& m = marks[(size_t) (markCount % kMaxMarks)];
        m.pos = pos;
        m.period = (float) T0;
        m.targetHz = tuneFor ((float) (sr / T0), p);
        ++markCount;
        lastMarkPos = pos;
    };

    if (!haveMark)
    {
        const double back  = std::min (6.0 * T0, 0.5 * winSize);
        const double start = (double) (inCount - 1) - 0.3 * T0 - std::max (back, 1.3 * T0);
        if (start < 2.0 * T0)
            return;
        push (findPeak (start, 0.5 * T0));
        haveMark = true;
    }

    while (true)
    {
        const double pred = lastMarkPos + T0;
        if (pred + 0.3 * T0 + 2.0 > (double) (inCount - 1))
            break;

        const double m = findPeak (pred, 0.25 * T0);
        if (m <= lastMarkPos + 0.5 * T0)
        {
            lastMarkPos = pred;   // avoid duplicates, keep the grid moving
            continue;
        }
        push (m);
    }
}

const Engine::Mark* Engine::nearestMark (double pos, double tolerance) const
{
    const Mark* best = nullptr;
    double bestD = tolerance;
    const int64_t first = std::max<int64_t> (0, markCount - kMaxMarks + 1);

    for (int64_t i = markCount - 1; i >= first; --i)
    {
        const Mark& m = marks[(size_t) (i % kMaxMarks)];
        const double d = std::fabs (m.pos - pos);
        if (d < bestD) { bestD = d; best = &m; }
        else if (m.pos < pos - tolerance) break;
    }
    return best;
}

// --------------------------------------------------------------- synthesis

void Engine::makeGrain (int v, const Mark& mk, double sc, const EngineParams& p,
                        double /*hcap*/, bool& produced)
{
    produced = false;

    const double period = mk.period;
    const double fin    = sr / period;

    if (!mainActive)
    {
        smoothFreq = fin;
        curTarget  = mk.targetHz;
        mainActive = true;
    }
    if (v == 0)
        curTarget = mk.targetHz;

    const double ratioMain = clampd (smoothFreq / fin, 0.5, 2.0) * vibRatio;
    double ratio = clampd (ratioMain, 0.5, 2.0);
    if (v > 0)
        ratio = clampd (ratio * std::pow (2.0, kIntervals[clampi (p.hInterval[v - 1], 0, 7)] / 12.0), 0.25, 4.0);

    const double Tout = period / ratio;
    const double f    = clampd (std::pow (ratio, 1.0 - clampd (p.formant, 0.0, 1.0)), 0.7, 1.43);
    double Hout = std::max (period / f, Tout);
    Hout = std::max (4.0, std::min (Hout, 1.5 * period));
    const double Hin = Hout * f;

    // the grain must lie entirely inside the received input
    if (mk.pos + Hin + 3.0 >= (double) inCount || mk.pos - Hin - 3.0 < 0.0)
        return;

    double gl = 1.0, gr = 1.0;
    bool mono = false;
    if (v > 0)
    {
        const double level = std::pow (10.0, p.hLevelDb[v - 1] / 20.0);
        const double ang   = (clampd (p.hPan[v - 1], -1.0, 1.0) + 1.0) * kPi * 0.25;
        gl = level * std::cos (ang);
        gr = level * std::sin (ang);
        mono = true;
    }

    const double g = Tout / Hout;
    const int t0 = (int) std::floor (sc - Hout);
    const int t1 = (int) std::ceil  (sc + Hout);

    for (int t = t0; t <= t1; ++t)
    {
        if ((int64_t) t < outCount)
            continue;

        const double d = (double) t - sc;
        if (std::fabs (d) > Hout)
            continue;

        const double w  = 0.5 * (1.0 + std::cos (kPi * d / Hout)) * g;
        const double rp = mk.pos + d * f;
        const int64_t i0 = (int64_t) std::floor (rp);
        const float   fr = (float) (rp - (double) i0);

        const size_t a = (size_t) (i0 & kMask), b = (size_t) ((i0 + 1) & kMask);
        float sL = inL[a] + (inL[b] - inL[a]) * fr;
        float sR = inR[a] + (inR[b] - inR[a]) * fr;

        if (mono)
        {
            const float m = 0.5f * (sL + sR);
            sL = m;
            sR = m;
        }

        olaL[(size_t) (t & kMask)] += (float) (sL * w * gl);
        olaR[(size_t) (t & kMask)] += (float) (sR * w * gr);
    }

    produced = true;

    if (v == 0)
    {
        const float detMidi = 69.0f + 12.0f * std::log2 ((float) fin / p.refHz);
        const float tgtMidi = 69.0f + 12.0f * std::log2 ((float) (smoothFreq * vibRatio) / p.refHz) - (float) p.transpose;
        meters.detectedMidi.store (detMidi);
        meters.targetMidi.store (tgtMidi + (float) p.transpose);
        meters.cents.store ((float) clampd ((double) (tgtMidi - detMidi) * 100.0, -100.0, 100.0));
    }
}

void Engine::process (float* left, float* right, int numSamples, const EngineParams& p)
{
    const int type = clampi (p.inputType, 0, 3);
    latency = latencyFor (sr, type);
    const double hcap = 1.5 * sr / kRanges[type].minF;
    const double tol  = std::max (32.0, 0.8 * (periodEst > 0.0 ? periodEst : 128.0));

    // retune time constant in ms
    const double tauMs = (p.classic && p.retune <= 5.0f) ? 0.1 : 1.0 + 0.04 * (double) p.retune * p.retune;
    const double coef  = std::exp (-1000.0 / (tauMs * sr));

    for (int n = 0; n < numSamples; ++n)
    {
        const float l = left[n];
        const float r = right[n];

        const size_t w = (size_t) (inCount & kMask);
        inL[w] = l;
        inR[w] = r;
        inM[w] = 0.5f * (l + r);
        ++inCount;

        if (inCount >= winSize && (inCount % hop) == 0)
            analyse (p);

        generateMarks (p);

        // vibrato
        if (p.vibrato != 0.0f)
        {
            vibPhase += 2.0 * kPi * 5.5 / sr;
            if (vibPhase >= 2.0 * kPi) vibPhase -= 2.0 * kPi;
        }
        vibRatio = std::pow (2.0, std::sin (vibPhase) * (double) p.vibrato * 0.08 / 12.0);

        if (mainActive)
            smoothFreq += (curTarget - smoothFreq) * (1.0 - coef);

        // voice activation
        for (int v = 1; v < kVoices; ++v)
        {
            if (p.harmonyOn && !voices[v].active)
            {
                voices[v].active = true;
                voices[v].nextSynth = (double) outCount + hcap + 1.0;
            }
            else if (!p.harmonyOn)
                voices[v].active = false;
        }

        // schedule grains
        for (int v = 0; v < kVoices; ++v)
        {
            if (!voices[v].active)
                continue;

            int guard = 0;
            while (voices[v].nextSynth - hcap <= (double) outCount + 1.0 && ++guard < 64)
            {
                const double sc = voices[v].nextSynth;
                const Mark* mk = nearestMark (sc - latency, tol);
                bool produced = false;
                double step = 64.0;

                if (mk != nullptr)
                {
                    const Mark m = *mk;
                    makeGrain (v, m, sc, p, hcap, produced);
                    if (produced)
                    {
                        const double fin = sr / (double) m.period;
                        double ratio = clampd (smoothFreq / fin, 0.5, 2.0) * vibRatio;
                        ratio = clampd (ratio, 0.5, 2.0);
                        if (v > 0)
                            ratio = clampd (ratio * std::pow (2.0, kIntervals[clampi (p.hInterval[v - 1], 0, 7)] / 12.0), 0.25, 4.0);
                        step = (double) m.period / ratio;
                    }
                }

                if (!produced && v == 0)
                    mainActive = false;

                voices[v].nextSynth += std::max (4.0, step);
            }
        }

        // output
        const int64_t di = outCount - latency;
        const float dryL = di >= 0 ? inL[(size_t) (di & kMask)] : 0.0f;
        const float dryR = di >= 0 ? inR[(size_t) (di & kMask)] : 0.0f;

        const bool hasMark = nearestMark ((double) di, tol) != nullptr;
        voicedGain += ((hasMark ? 1.0f : 0.0f) - voicedGain) * 0.01f;

        const size_t oi = (size_t) (outCount & kMask);
        const float wetL = olaL[oi] + (1.0f - voicedGain) * dryL;
        const float wetR = olaR[oi] + (1.0f - voicedGain) * dryR;
        olaL[oi] = 0.0f;
        olaR[oi] = 0.0f;

        float oL = dryL + (wetL - dryL) * p.mix;
        float oR = dryR + (wetR - dryR) * p.mix;
        if (!std::isfinite (oL)) oL = 0.0f;
        if (!std::isfinite (oR)) oR = 0.0f;

        left[n]  = std::max (-1.0f, std::min (1.0f, oL));
        right[n] = std::max (-1.0f, std::min (1.0f, oR));
        ++outCount;

        if (!voiced)
        {
            meters.detectedMidi.store (-1.0f);
            meters.targetMidi.store (-1.0f);
            meters.cents.store (0.0f);
        }
    }
}

} // namespace apc
