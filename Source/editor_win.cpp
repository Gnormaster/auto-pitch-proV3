// Windows editor: hosts the web user interface in a WebView2 control.
// Protocol (iPlug2 style):
//   UI -> plugin : window.IPlugSendMsg({msg:"SPVFUI", paramIdx, value})   (value normalised 0..1)
//   plugin -> UI : window.SPVFD(paramIdx, value), window.__vstOnProcessorBuffers({telemetry:[...]})
#include "editor_if.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <WebView2.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace apc_ui
{
namespace
{
    const wchar_t* const kClassName = L"AutoPitchProEditor";
    constexpr int kWidth = 900, kHeight = 640;
    constexpr UINT_PTR kTimerId = 1;

    HMODULE thisModule()
    {
        HMODULE m = nullptr;
        GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR> (&thisModule), &m);
        return m;
    }

    // number formatting and parsing without locale dependence
    std::wstring fmt (double v)
    {
        if (!std::isfinite (v)) v = 0.0;
        const bool neg = v < 0.0;
        if (neg) v = -v;
        const long long iv = (long long) std::llround (v * 1000000.0);
        wchar_t buf[64];
        _snwprintf_s (buf, _TRUNCATE, L"%ls%lld.%06lld", neg ? L"-" : L"", iv / 1000000, iv % 1000000);
        return buf;
    }

    double parseNumber (const wchar_t* s, const wchar_t** end)
    {
        const wchar_t* p = s;
        bool neg = false;
        if (*p == L'-') { neg = true; ++p; } else if (*p == L'+') ++p;
        double v = 0.0;
        while (*p >= L'0' && *p <= L'9') { v = v * 10.0 + (double) (*p - L'0'); ++p; }
        if (*p == L'.')
        {
            ++p;
            double scale = 0.1;
            while (*p >= L'0' && *p <= L'9') { v += (double) (*p - L'0') * scale; scale *= 0.1; ++p; }
        }
        if (*p == L'e' || *p == L'E')
        {
            ++p;
            bool eneg = false;
            if (*p == L'-') { eneg = true; ++p; } else if (*p == L'+') ++p;
            int e = 0;
            while (*p >= L'0' && *p <= L'9') { e = e * 10 + (*p - L'0'); ++p; }
            v *= std::pow (10.0, eneg ? -e : e);
        }
        if (end) *end = p;
        return neg ? -v : v;
    }

    std::string toUtf8 (const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte (CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
        std::string s ((size_t) n, '\0');
        WideCharToMultiByte (CP_UTF8, 0, w.data(), (int) w.size(), &s[0], n, nullptr, nullptr);
        return s;
    }

    std::wstring fromUtf8 (const std::string& s)
    {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar (CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
        std::wstring w ((size_t) n, L'\0');
        MultiByteToWideChar (CP_UTF8, 0, s.data(), (int) s.size(), &w[0], n);
        return w;
    }

    std::wstring dataDir()
    {
        wchar_t buf[2 * MAX_PATH];
        const DWORD n = GetEnvironmentVariableW (L"LOCALAPPDATA", buf, (DWORD) std::size (buf));
        const std::wstring base = (n > 0 && n < std::size (buf)) ? std::wstring (buf) : std::wstring (L"C:\\Users\\Public");
        const std::wstring dir = base + L"\\AutoPitchPro";
        CreateDirectoryW (dir.c_str(), nullptr);
        return dir;
    }

    // Writes the embedded user interface to disk so WebView2 can serve it.
    std::wstring prepareUiFolder()
    {
        const std::wstring folder = dataDir() + L"\\ui";
        CreateDirectoryW (folder.c_str(), nullptr);

        HMODULE mod = thisModule();
        HRSRC res = FindResourceW (mod, MAKEINTRESOURCEW (1), RT_RCDATA);
        if (res != nullptr)
        {
            HGLOBAL g = LoadResource (mod, res);
            const void* data = g ? LockResource (g) : nullptr;
            const DWORD size = SizeofResource (mod, res);
            const std::wstring file = folder + L"\\index.html";

            WIN32_FILE_ATTRIBUTE_DATA info {};
            const bool same = GetFileAttributesExW (file.c_str(), GetFileExInfoStandard, &info)
                              && info.nFileSizeHigh == 0 && info.nFileSizeLow == size;
            if (data != nullptr && !same)
            {
                HANDLE f = CreateFileW (file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE)
                {
                    DWORD written = 0;
                    WriteFile (f, data, size, &written, nullptr);
                    CloseHandle (f);
                }
            }
        }
        return folder;
    }

    // Reference-counted COM callback object backed by a std::function.
    template <class Iface, class... Args>
    struct Handler : Iface
    {
        std::function<HRESULT (Args...)> fn;
        std::atomic<ULONG> refs { 1 };

        explicit Handler (std::function<HRESULT (Args...)> f) : fn (std::move (f)) {}

        HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** ppv) override
        {
            if (riid == IID_IUnknown || riid == __uuidof (Iface))
            {
                *ppv = static_cast<Iface*> (this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG r = --refs;
            if (r == 0) delete this;
            return r;
        }
        HRESULT STDMETHODCALLTYPE Invoke (Args... args) override { return fn (args...); }
    };

    struct Editor
    {
        PluginLink link;
        HWND hwnd = nullptr;
        ICoreWebView2Environment* env = nullptr;
        ICoreWebView2Controller* controller = nullptr;
        ICoreWebView2* web = nullptr;
        std::wstring folder;
        std::vector<float> sent;
        bool ready = false, failed = false, closed = false;

        void run (const std::wstring& js)
        {
            if (web != nullptr && ready)
                web->ExecuteScript (js.c_str(), nullptr);
        }

        std::wstring initScript() const
        {
            std::wstring s =
                L"window.IPlugSendMsg=function(m){try{var w=window.chrome.webview;"
                L"if(m.msg==='SPVFUI')w.postMessage('P|'+m.paramIdx+'|'+m.value);"
                L"else if(m.msg==='vstSetCustomState')w.postMessage('S|'+m.state);"
                L"else if(m.msg==='vstGetCustomState')w.postMessage('G|');"
                L"}catch(e){}};"
                L"window.__spaceMeta=Object.assign(window.__spaceMeta||{},{initialParams:{";
            for (int i = 0; i < link.numParams; ++i)
                s += std::to_wstring (i) + L":" + fmt (link.getNorm (link.ctx, i)) + L",";
            s += L"}});";
            if (link.customState != nullptr && !link.customState->empty())
                s += L"window.__vstCustomState=" + fromUtf8 (*link.customState) + L";";
            return s;
        }

        void onMessage (const wchar_t* m)
        {
            if (m[0] == L'P' && m[1] == L'|')
            {
                const wchar_t* end = nullptr;
                const int idx = (int) parseNumber (m + 2, &end);
                if (*end == L'|')
                {
                    const double v = parseNumber (end + 1, nullptr);
                    if (idx >= 0 && idx < link.numParams)
                    {
                        const float f = (float) std::fmin (1.0, std::fmax (0.0, v));
                        sent[(size_t) idx] = f;
                        link.setFromUi (link.ctx, idx, f);
                    }
                }
            }
            else if (m[0] == L'S' && m[1] == L'|')
            {
                if (link.customState != nullptr)
                    *link.customState = toUtf8 (std::wstring (m + 2));
            }
            else if (m[0] == L'G' && m[1] == L'|')
            {
                if (link.customState != nullptr && !link.customState->empty())
                    run (L"window.__vstOnCustomState&&window.__vstOnCustomState(" + fromUtf8 (*link.customState) + L");");
            }
        }

        void tick()
        {
            if (!ready || closed) return;

            std::wstring js;
            for (int i = 0; i < link.numParams; ++i)
            {
                const float v = link.getNorm (link.ctx, i);
                if (std::fabs (v - sent[(size_t) i]) > 1.0e-6f)
                {
                    sent[(size_t) i] = v;
                    js += L"window.SPVFD&&window.SPVFD(" + std::to_wstring (i) + L"," + fmt (v) + L");";
                }
            }

            float t[8] = {};
            link.getTelemetry (link.ctx, t);
            js += L"window.__vstOnProcessorBuffers&&window.__vstOnProcessorBuffers({telemetry:[";
            for (int i = 0; i < 8; ++i)
            {
                if (i) js += L",";
                js += fmt ((double) t[i]);
            }
            js += L"]});";
            run (js);
        }

        void fail()
        {
            failed = true;
            if (hwnd) InvalidateRect (hwnd, nullptr, TRUE);
        }

        HRESULT onController (HRESULT hr, ICoreWebView2Controller* c)
        {
            if (closed) return S_OK;
            if (FAILED (hr) || c == nullptr) { fail(); return S_OK; }

            controller = c;
            controller->AddRef();
            controller->get_CoreWebView2 (&web);
            if (web == nullptr) { fail(); return S_OK; }

            RECT rc;
            GetClientRect (hwnd, &rc);
            controller->put_Bounds (rc);
            controller->put_IsVisible (TRUE);

            ICoreWebView2Settings* settings = nullptr;
            if (SUCCEEDED (web->get_Settings (&settings)) && settings != nullptr)
            {
                settings->put_AreDefaultContextMenusEnabled (FALSE);
                settings->put_AreDevToolsEnabled (FALSE);
                settings->put_IsStatusBarEnabled (FALSE);
                settings->put_IsZoomControlEnabled (FALSE);
                settings->Release();
            }

            EventRegistrationToken token {};
            auto* msg = new Handler<ICoreWebView2WebMessageReceivedEventHandler,
                                    ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*> (
                [this] (ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
                {
                    LPWSTR text = nullptr;
                    if (!closed && SUCCEEDED (args->TryGetWebMessageAsString (&text)) && text != nullptr)
                    {
                        onMessage (text);
                        CoTaskMemFree (text);
                    }
                    return S_OK;
                });
            web->add_WebMessageReceived (msg, &token);
            msg->Release();

            auto* nav = new Handler<ICoreWebView2NavigationCompletedEventHandler,
                                    ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*> (
                [this] (ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT
                {
                    ready = true;
                    return S_OK;
                });
            web->add_NavigationCompleted (nav, &token);
            nav->Release();

            web->AddScriptToExecuteOnDocumentCreated (initScript().c_str(), nullptr);

            ICoreWebView2_3* web3 = nullptr;
            if (SUCCEEDED (web->QueryInterface (IID_PPV_ARGS (&web3))) && web3 != nullptr)
            {
                web3->SetVirtualHostNameToFolderMapping (L"apc.local", folder.c_str(),
                                                         COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                web3->Release();
            }
            web->Navigate (L"https://apc.local/index.html");
            return S_OK;
        }

        void start()
        {
            folder = prepareUiFolder();
            sent.assign ((size_t) link.numParams, -1.0f);
            const std::wstring userData = dataDir() + L"\\webview";

            auto* envHandler = new Handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                                           HRESULT, ICoreWebView2Environment*> (
                [this] (HRESULT hr, ICoreWebView2Environment* e) -> HRESULT
                {
                    if (closed) return S_OK;
                    if (FAILED (hr) || e == nullptr) { fail(); return S_OK; }

                    env = e;
                    env->AddRef();
                    auto* ctrl = new Handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                                             HRESULT, ICoreWebView2Controller*> (
                        [this] (HRESULT hr2, ICoreWebView2Controller* c) -> HRESULT { return onController (hr2, c); });
                    env->CreateCoreWebView2Controller (hwnd, ctrl);
                    ctrl->Release();
                    return S_OK;
                });

            const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions (nullptr, userData.c_str(), nullptr, envHandler);
            envHandler->Release();
            if (FAILED (hr)) fail();
        }
    };

    LRESULT CALLBACK wndProc (HWND h, UINT msg, WPARAM w, LPARAM l)
    {
        Editor* ed = reinterpret_cast<Editor*> (GetWindowLongPtrW (h, GWLP_USERDATA));
        switch (msg)
        {
            case WM_TIMER:
                if (ed != nullptr && w == kTimerId) ed->tick();
                return 0;

            case WM_SIZE:
                if (ed != nullptr && ed->controller != nullptr)
                {
                    RECT rc;
                    GetClientRect (h, &rc);
                    ed->controller->put_Bounds (rc);
                }
                return 0;

            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint (h, &ps);
                RECT rc;
                GetClientRect (h, &rc);
                HBRUSH brush = CreateSolidBrush (RGB (20, 22, 34));
                FillRect (dc, &rc, brush);
                DeleteObject (brush);
                if (ed != nullptr && ed->failed)
                {
                    SetTextColor (dc, RGB (220, 224, 238));
                    SetBkMode (dc, TRANSPARENT);
                    DrawTextW (dc, L"WebView2 Runtime nicht gefunden. Bitte von microsoft.com installieren.",
                               -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                EndPaint (h, &ps);
                return 0;
            }
        }
        return DefWindowProcW (h, msg, w, l);
    }
}

void scaledSize (int* width, int* height)
{
    UINT dpi = 96;
    HMODULE user = GetModuleHandleW (L"user32.dll");
    using GetDpiFn = UINT (WINAPI*) ();
    GetDpiFn fn = user ? reinterpret_cast<GetDpiFn> (reinterpret_cast<void*> (GetProcAddress (user, "GetDpiForSystem"))) : nullptr;
    if (fn) dpi = fn();
    *width  = MulDiv (kWidth, (int) dpi, 96);
    *height = MulDiv (kHeight, (int) dpi, 96);
}

void* openEditor (void* parentWindow, const PluginLink& link)
{
    int winW = kWidth, winH = kHeight;
    scaledSize (&winW, &winH);
    CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);   // result ignored: the host may already have initialised COM

    HMODULE mod = thisModule();

    WNDCLASSEXW wc {};
    wc.cbSize = sizeof (wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = mod;
    wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW (&wc);   // fails harmlessly when the class already exists

    Editor* ed = new Editor();
    ed->link = link;
    ed->hwnd = CreateWindowExW (0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                0, 0, winW, winH, (HWND) parentWindow, nullptr, mod, nullptr);
    if (ed->hwnd == nullptr)
    {
        ed->closed = true;
        return ed;
    }
    SetWindowLongPtrW (ed->hwnd, GWLP_USERDATA, (LONG_PTR) ed);
    ed->start();
    SetTimer (ed->hwnd, kTimerId, 33, nullptr);
    return ed;
}

void closeEditor (void* editor)
{
    // The Editor object is intentionally kept allocated: late WebView2 callbacks may still reference it.
    Editor* ed = static_cast<Editor*> (editor);
    if (ed == nullptr || ed->closed) return;
    ed->closed = true;
    ed->ready = false;

    if (ed->hwnd) KillTimer (ed->hwnd, kTimerId);
    if (ed->controller) { ed->controller->Close(); ed->controller->Release(); ed->controller = nullptr; }
    if (ed->web) { ed->web->Release(); ed->web = nullptr; }
    if (ed->env) { ed->env->Release(); ed->env = nullptr; }
    if (ed->hwnd) { SetWindowLongPtrW (ed->hwnd, GWLP_USERDATA, 0); DestroyWindow (ed->hwnd); ed->hwnd = nullptr; }
}
}
