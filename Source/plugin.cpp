// VST 2.4 effect plugin wrapper around the pitch engine.
#include "PitchEngine.h"
#include "vst2_abi.h"
#include "editor_if.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
  #define APC_EXPORT extern "C" __declspec(dllexport)
#else
  #define APC_EXPORT extern "C" __attribute__((visibility("default")))
#endif

using namespace vst2;

namespace
{
    enum Kind { kFloat, kChoice, kInt, kBool };

    struct Spec
    {
        const char* name;
        const char* unit;
        Kind kind;
        float lo, hi, def;
        int count;                 // choices
        const char* const* items;
    };

    const char* const kKeys[12]    = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const char* const kScales[3]   = { "Chromatic", "Major", "Minor" };
    const char* const kInputs[4]   = { "Soprano", "Alto/Tenor", "Low Male", "Instrument" };
    const char* const kInterval[8] = { "-12 st", "-7 st", "-5 st", "-3 st", "+3 st", "+5 st", "+7 st", "+12 st" };

    constexpr int kNumParams = 26;
    enum { pRetune, pFlex, pHuman, pVibrato, pKey, pScale, pInput, pTracking, pTranspose,
           pDetune, pFormant, pMix, pClassic, pHarmony, pHarmonyBase };

    std::vector<Spec> makeSpecs()
    {
        static const char* const hNames[12] = { "H1 Interval", "H1 Level", "H1 Pan", "H2 Interval", "H2 Level", "H2 Pan",
                                                "H3 Interval", "H3 Level", "H3 Pan", "H4 Interval", "H4 Level", "H4 Pan" };
        const float defInt[4] = { 7, 6, 2, 0 };
        const float defPan[4] = { -50, 50, -100, 100 };

        std::vector<Spec> s;
        s.push_back ({ "Retune Speed",    "",   kFloat,  0, 100, 20, 0, nullptr });
        s.push_back ({ "Flex Tune",       "",   kFloat,  0, 100, 0,  0, nullptr });
        s.push_back ({ "Humanize",        "",   kFloat,  0, 100, 0,  0, nullptr });
        s.push_back ({ "Natural Vibrato", "st", kFloat, -12, 12, 0,  0, nullptr });
        s.push_back ({ "Key",             "",   kChoice, 0, 11, 0,  12, kKeys });
        s.push_back ({ "Scale",           "",   kChoice, 0, 2,  1,  3,  kScales });
        s.push_back ({ "Input Type",      "",   kChoice, 0, 3,  1,  4,  kInputs });
        s.push_back ({ "Tracking",        "",   kFloat,  0, 100, 50, 0, nullptr });
        s.push_back ({ "Transpose",       "st", kInt,   -24, 24, 0,  0, nullptr });
        s.push_back ({ "Detune",          "Hz", kFloat, 420, 460, 440, 0, nullptr });
        s.push_back ({ "Formant",         "%",  kFloat,  0, 100, 100, 0, nullptr });
        s.push_back ({ "Mix",             "%",  kFloat,  0, 100, 100, 0, nullptr });
        s.push_back ({ "Classic Mode",    "",   kBool,   0, 1,  0,  0, nullptr });
        s.push_back ({ "Harmony On",      "",   kBool,   0, 1,  0,  0, nullptr });
        for (int i = 0; i < 4; ++i)
        {
            s.push_back ({ hNames[i * 3 + 0], "",   kChoice, 0, 7, defInt[i], 8, kInterval });
            s.push_back ({ hNames[i * 3 + 1], "dB", kFloat, -60, 6, -6, 0, nullptr });
            s.push_back ({ hNames[i * 3 + 2], "",   kFloat, -100, 100, defPan[i], 0, nullptr });
        }
        return s;
    }

    const std::vector<Spec>& specs() { static const std::vector<Spec> s = makeSpecs(); return s; }

    float realValue (int i, float n)
    {
        const Spec& s = specs()[(size_t) i];
        n = n < 0 ? 0 : (n > 1 ? 1 : n);
        switch (s.kind)
        {
            case kFloat:  return s.lo + n * (s.hi - s.lo);
            case kChoice: return (float) std::floor (n * (float) (s.count - 1) + 0.5f);
            case kInt:    return s.lo + (float) std::floor (n * (s.hi - s.lo) + 0.5f);
            case kBool:   return n >= 0.5f ? 1.0f : 0.0f;
        }
        return 0.0f;
    }

    float normFromReal (int i, float v)
    {
        const Spec& s = specs()[(size_t) i];
        const float span = s.kind == kChoice ? (float) (s.count - 1) : (s.hi - s.lo);
        const float lo   = s.kind == kChoice ? 0.0f : s.lo;
        const float n = span != 0 ? (v - lo) / span : 0.0f;
        return n < 0 ? 0 : (n > 1 ? 1 : n);
    }

    void copyStr (void* dst, const char* src, size_t cap)
    {
        if (dst == nullptr) return;
        std::strncpy ((char*) dst, src, cap - 1);
        ((char*) dst)[cap - 1] = 0;
    }

    struct Plugin
    {
        AEffect effect {};
        HostCallback host = nullptr;
        std::atomic<float> norm[kNumParams];
        apc::Engine engine;
        double sampleRate = 44100.0;
        bool prepared = false;
        std::vector<float> chunk;
        std::string customState;          // state of the web user interface (JSON text)
        void* editor = nullptr;
        std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
        ERect rect { 0, 0, 640, 900 };

        float real (int i) const { return realValue (i, norm[i].load()); }

        apc::EngineParams params() const
        {
            apc::EngineParams p;
            p.retune    = real (pRetune);
            p.flexTune  = real (pFlex);
            p.humanize  = real (pHuman);
            p.vibrato   = real (pVibrato);
            p.key       = (int) real (pKey);
            p.scale     = (int) real (pScale);
            p.inputType = (int) real (pInput);
            p.tracking  = real (pTracking);
            p.transpose = (int) real (pTranspose);
            p.refHz     = real (pDetune);
            p.formant   = real (pFormant) * 0.01f;
            p.mix       = real (pMix) * 0.01f;
            p.classic   = real (pClassic) >= 0.5f;
            p.harmonyOn = real (pHarmony) >= 0.5f;
            for (int i = 0; i < 4; ++i)
            {
                p.hInterval[i] = (int) real (pHarmonyBase + i * 3 + 0);
                p.hLevelDb[i]  = real (pHarmonyBase + i * 3 + 1);
                p.hPan[i]      = real (pHarmonyBase + i * 3 + 2) * 0.01f;
            }
            return p;
        }

        void prepare (double sr)
        {
            sampleRate = sr;
            engine.prepare (sr);
            prepared = true;
            effect.initialDelay = apc::Engine::latencyFor (sr, (int) real (pInput));
        }

        void display (int i, char* out, size_t cap) const
        {
            const Spec& s = specs()[(size_t) i];
            const float v = real (i);
            char buf[48];
            switch (s.kind)
            {
                case kFloat:  std::snprintf (buf, sizeof buf, "%.1f", (double) v); break;
                case kInt:    std::snprintf (buf, sizeof buf, "%d", (int) v); break;
                case kChoice: std::snprintf (buf, sizeof buf, "%s", s.items[(int) v]); break;
                case kBool:   std::snprintf (buf, sizeof buf, "%s", v >= 0.5f ? "On" : "Off"); break;
            }
            copyStr (out, buf, cap);
        }
    };

    Plugin* self (AEffect* e) { return (Plugin*) e->object; }

    void setParameter (AEffect* e, int32_t index, float value)
    {
        if (index < 0 || index >= kNumParams) return;
        Plugin* p = self (e);
        p->norm[index].store (value < 0 ? 0 : (value > 1 ? 1 : value));

        if (index == pInput && p->prepared)
        {
            const int lat = apc::Engine::latencyFor (p->sampleRate, (int) p->real (pInput));
            if (lat != e->initialDelay)
            {
                e->initialDelay = lat;
                if (p->host) p->host (e, audioMasterIOChanged, 0, 0, nullptr, 0.0f);
            }
        }
    }

    float getParameter (AEffect* e, int32_t index)
    {
        if (index < 0 || index >= kNumParams) return 0.0f;
        return self (e)->norm[index].load();
    }

    void processReplacing (AEffect* e, float** in, float** out, int32_t n)
    {
        Plugin* p = self (e);
        if (p == nullptr || n <= 0) return;

        if (out[0] != in[0]) std::memcpy (out[0], in[0], sizeof (float) * (size_t) n);
        if (out[1] != in[1]) std::memcpy (out[1], in[1], sizeof (float) * (size_t) n);

        if (p->prepared)
        {
            p->engine.process (out[0], out[1], n, p->params());

            float mL = 0.0f, mR = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                mL = std::max (mL, std::fabs (out[0][i]));
                mR = std::max (mR, std::fabs (out[1][i]));
            }
            p->peakL.store (std::max (mL, p->peakL.load() * 0.85f));
            p->peakR.store (std::max (mR, p->peakR.load() * 0.85f));
        }
    }

    void processLegacy (AEffect* e, float** in, float** out, int32_t n)
    {
        // accumulating variant is not used by current hosts; behave like replacing
        processReplacing (e, in, out, n);
    }

    float linkGetNorm (void* ctx, int i) { return ((Plugin*) ctx)->norm[i].load(); }

    void linkSetFromUi (void* ctx, int i, float v)
    {
        Plugin* p = (Plugin*) ctx;
        setParameter (&p->effect, i, v);
        if (p->host) p->host (&p->effect, audioMasterAutomate, i, 0, nullptr, v);
    }

    void linkTelemetry (void* ctx, float* out)
    {
        Plugin* p = (Plugin*) ctx;
        const float det = p->engine.meters.detectedMidi.load();
        const float tgt = p->engine.meters.targetMidi.load();
        const bool voiced = det >= 0.0f;
        out[0] = voiced ? std::floor (det + 0.5f) : 0.0f;                  // detected note
        out[1] = voiced ? -p->engine.meters.cents.load() : 0.0f;          // deviation in cents
        out[2] = voiced ? std::floor (tgt + 0.5f) : 0.0f;                  // corrected note
        out[3] = voiced ? 1.0f : 0.0f;
        out[4] = p->peakL.load();
        out[5] = p->peakR.load();
        out[6] = voiced ? det : 0.0f;                                      // detected pitch (fractional note)
        out[7] = voiced ? tgt : 0.0f;                                      // corrected pitch (fractional note)
    }

    intptr_t dispatcher (AEffect* e, int32_t op, int32_t index, intptr_t value, void* ptr, float opt)
    {
        Plugin* p = self (e);

        switch (op)
        {
            case effOpen:
                p->prepare (44100.0);
                return 0;

            case effClose:
#if defined(_WIN32)
                if (p->editor) { apc_ui::closeEditor (p->editor); p->editor = nullptr; }
#endif
                delete p;
                return 0;

            case effSetSampleRate:
                if (opt > 1000.0f && (double) opt != p->sampleRate)
                    p->prepare ((double) opt);
                return 0;

            case effMainsChanged:
                if (value != 0)
                    p->prepare (p->sampleRate);
                return 0;

            case effGetProgram:          return 0;
            case effGetProgramName:      copyStr (ptr, "Default", 24); return 0;
            case effGetProgramNameIndexed: copyStr (ptr, "Default", 24); return 1;

            case effGetParamLabel:
                if (index >= 0 && index < kNumParams) copyStr (ptr, specs()[(size_t) index].unit, 24);
                return 0;
            case effGetParamDisplay:
                if (index >= 0 && index < kNumParams) p->display (index, (char*) ptr, 24);
                return 0;
            case effGetParamName:
                if (index >= 0 && index < kNumParams) copyStr (ptr, specs()[(size_t) index].name, 24);
                return 0;

            case effCanBeAutomated:
                return (index >= 0 && index < kNumParams) ? 1 : 0;

            case effString2Parameter:
            {
                if (index < 0 || index >= kNumParams || ptr == nullptr) return 0;
                const Spec& s = specs()[(size_t) index];
                const char* txt = (const char*) ptr;
                if (s.kind == kChoice)
                {
                    for (int k = 0; k < s.count; ++k)
                        if (std::strcmp (s.items[k], txt) == 0) { setParameter (e, index, normFromReal (index, (float) k)); return 1; }
                    return 0;
                }
                if (s.kind == kBool)
                {
                    setParameter (e, index, (std::strcmp (txt, "On") == 0 || std::atof (txt) >= 0.5) ? 1.0f : 0.0f);
                    return 1;
                }
                setParameter (e, index, normFromReal (index, (float) std::atof (txt)));
                return 1;
            }

            case effGetChunk:
            {
                const size_t nBytes = sizeof (float) * kNumParams;
                std::vector<float> tmp (kNumParams);
                for (int i = 0; i < kNumParams; ++i) tmp[(size_t) i] = p->norm[i].load();

                const uint32_t len = (uint32_t) p->customState.size();
                p->chunk.assign ((nBytes + sizeof (len) + len + 3) / 4, 0.0f);
                unsigned char* d = (unsigned char*) p->chunk.data();
                std::memcpy (d, tmp.data(), nBytes);
                std::memcpy (d + nBytes, &len, sizeof (len));
                if (len) std::memcpy (d + nBytes + sizeof (len), p->customState.data(), len);

                if (ptr) *(void**) ptr = p->chunk.data();
                return (intptr_t) (nBytes + sizeof (len) + len);
            }

            case effSetChunk:
            {
                if (ptr == nullptr) return 0;
                const size_t nBytes = sizeof (float) * kNumParams;
                const unsigned char* d = (const unsigned char*) ptr;
                const int count = (int) std::min<intptr_t> (kNumParams, value / (intptr_t) sizeof (float));
                for (int i = 0; i < count; ++i)
                {
                    float f;
                    std::memcpy (&f, d + sizeof (float) * (size_t) i, sizeof (f));
                    setParameter (e, i, f);
                }
                if ((size_t) value >= nBytes + sizeof (uint32_t))
                {
                    uint32_t len = 0;
                    std::memcpy (&len, d + nBytes, sizeof (len));
                    if ((size_t) value >= nBytes + sizeof (len) + len)
                        p->customState.assign ((const char*) d + nBytes + sizeof (len), len);
                }
                return 0;
            }

            case effEditGetRect:
#if defined(_WIN32)
            {
                int w = 900, h = 640;
                apc_ui::scaledSize (&w, &h);
                p->rect.right = (int16_t) w;
                p->rect.bottom = (int16_t) h;
            }
#endif
                if (ptr) *(ERect**) ptr = &p->rect;
                return 1;

            case effEditOpen:
            {
#if defined(_WIN32)
                apc_ui::PluginLink link;
                link.ctx = p;
                link.numParams = kNumParams;
                link.getNorm = linkGetNorm;
                link.setFromUi = linkSetFromUi;
                link.getTelemetry = linkTelemetry;
                link.customState = &p->customState;
                p->editor = apc_ui::openEditor (ptr, link);
                return 1;
#else
                return 0;
#endif
            }

            case effEditClose:
#if defined(_WIN32)
                if (p->editor) { apc_ui::closeEditor (p->editor); p->editor = nullptr; }
#endif
                return 0;

            case effEditIdle:
                return 0;

            case effGetEffectName:       copyStr (ptr, "Auto-Pitch Pro", 32); return 1;
            case effGetVendorString:     copyStr (ptr, "AutoPitchClone", 64); return 1;
            case effGetProductString:    copyStr (ptr, "Auto-Pitch Pro", 64); return 1;
            case effGetVendorVersion:    return 1000;
            case effGetPlugCategory:     return 1;      // effect
            case effGetVstVersion:       return 2400;
            case effGetTailSize:         return 0;
            case effProcessEvents:       return 0;

            case effCanDo:
                return 0;

            default:
                return 0;
        }
    }
}

APC_EXPORT vst2::AEffect* VSTPluginMain (vst2::HostCallback host)
{
    if (host != nullptr && host (nullptr, audioMasterVersion, 0, 0, nullptr, 0.0f) == 0)
        return nullptr;

    Plugin* p = new Plugin();
    p->host = host;
    for (int i = 0; i < kNumParams; ++i)
        p->norm[i].store (normFromReal (i, specs()[(size_t) i].def));

    AEffect& e = p->effect;
    std::memset (&e, 0, sizeof (e));
    e.magic            = kMagic;
    e.dispatcher       = dispatcher;
    e.process          = processLegacy;
    e.setParameter     = setParameter;
    e.getParameter     = getParameter;
    e.numPrograms      = 1;
    e.numParams        = kNumParams;
    e.numInputs        = 2;
    e.numOutputs       = 2;
    e.flags            = effFlagsCanReplacing | effFlagsProgramChunks;
#if defined(_WIN32)
    e.flags           |= effFlagsHasEditor;
#endif
    e.initialDelay     = apc::Engine::latencyFor (44100.0, 1);
    e.ioRatio          = 1.0f;
    e.object           = p;
    e.uniqueID         = 0x41706350;      // 'ApcP'
    e.version          = 1000;
    e.processReplacing = processReplacing;
    return &e;
}
