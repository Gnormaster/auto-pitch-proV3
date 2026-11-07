#pragma once
#include <string>

namespace apc_ui
{
    // Callbacks from the editor into the plugin.
    struct PluginLink
    {
        void* ctx = nullptr;
        int numParams = 0;
        float (*getNorm)(void* ctx, int index) = nullptr;
        void  (*setFromUi)(void* ctx, int index, float norm) = nullptr;
        void  (*getTelemetry)(void* ctx, float* out6) = nullptr;
        std::string* customState = nullptr;
    };

    void  scaledSize (int* width, int* height);
    void* openEditor (void* parentWindow, const PluginLink& link);
    void  closeEditor (void* editor);
}
