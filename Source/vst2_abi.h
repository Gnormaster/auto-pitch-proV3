#pragma once
#include <cstdint>

// Minimal declarations of the VST 2.x plugin binary interface.
namespace vst2
{
    constexpr int32_t kMagic = 0x56737450;   // 'VstP'

    enum Opcode
    {
        effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4,
        effGetProgramName = 5, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
        effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12,
        effEditGetRect = 13, effEditOpen = 14, effEditClose = 15, effEditIdle = 19,
        effGetChunk = 23, effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26,
        effString2Parameter = 27, effGetProgramNameIndexed = 29, effGetPlugCategory = 35,
        effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
        effGetVendorVersion = 49, effCanDo = 51, effGetTailSize = 52, effGetVstVersion = 58
    };

    enum MasterOpcode { audioMasterAutomate = 0, audioMasterVersion = 1, audioMasterIOChanged = 13 };
    enum Flags { effFlagsHasEditor = 1 << 0, effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5 };

    struct ERect { int16_t top, left, bottom, right; };

    struct AEffect;
    using HostCallback = intptr_t (*)(AEffect*, int32_t, int32_t, intptr_t, void*, float);

    struct AEffect
    {
        int32_t magic;
        intptr_t (*dispatcher)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
        void (*process)(AEffect*, float**, float**, int32_t);
        void (*setParameter)(AEffect*, int32_t, float);
        float (*getParameter)(AEffect*, int32_t);
        int32_t numPrograms;
        int32_t numParams;
        int32_t numInputs;
        int32_t numOutputs;
        int32_t flags;
        intptr_t resvd1;
        intptr_t resvd2;
        int32_t initialDelay;
        int32_t realQualities;
        int32_t offQualities;
        float ioRatio;
        void* object;
        void* user;
        int32_t uniqueID;
        int32_t version;
        void (*processReplacing)(AEffect*, float**, float**, int32_t);
        void (*processDoubleReplacing)(AEffect*, double**, double**, int32_t);
        char future[56];
    };
}
