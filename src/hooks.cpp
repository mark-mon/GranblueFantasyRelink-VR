#include <intrin.h>
// Hooks IDXGISwapChain::Present / Present1 / ResizeBuffers by reading the vtable of a
// throwaway swapchain (all DXGI swapchains share the implementation in dxgi.dll).
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#pragma comment(lib, "dxgi.lib")
#include "MinHook.h"
#include "globals.h"
#include "camera.h"

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static PresentFn oPresent = nullptr;
static Present1Fn oPresent1 = nullptr;
static ResizeFn oResize = nullptr;
static thread_local bool t_inPresent = false;

// The game presents its (mirror) window with vsync: on a 60 Hz monitor that alone caps the game at 60 fps
// even though the headset runs at 90 Hz or more. While VR runs, present the mirror without waiting for
// the monitor (desktop_vsync = false); SteamVR's frame pacing (xrWaitFrame) sets the frame rate instead.
bool VR_Active();
static UINT FixSync(UINT sync) {
    static UINT last = 0xffffffff;
    if (sync != last) { last = sync; LOGI("Game presents with sync interval %u%s", sync, (!g_cfg.desktopVsync && sync && VR_Active()) ? " -> 0 (desktop_vsync = false)" : ""); }
    if (!g_cfg.desktopVsync && VR_Active()) return 0;
    return sync;
}
static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    Camera_NotePresent();
    sync = FixSync(sync);
    if (!t_inPresent && !(flags & DXGI_PRESENT_TEST)) {
        t_inPresent = true;
        VR_OnPresent(sc);
        HRESULT hr = oPresent(sc, sync, flags);
        t_inPresent = false;
        return hr;
    }
    return oPresent(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    Camera_NotePresent();
    sync = FixSync(sync);
    if (!t_inPresent && !(flags & DXGI_PRESENT_TEST)) {
        t_inPresent = true;
        VR_OnPresent(sc);
        HRESULT hr = oPresent1(sc, sync, flags, p);
        t_inPresent = false;
        return hr;
    }
    return oPresent1(sc, sync, flags, p);
}

// Back-buffer size override (VR render resolution); returns true if changed.
bool VR_RenderSizeOverride(UINT* w, UINT* h);
static HRESULT STDMETHODCALLTYPE hkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    LOGI("ResizeBuffers(%u, %ux%u, fmt %d, flags 0x%x) from exe+0x%llx", n, w, h, (int)f, fl, (unsigned long long)((uintptr_t)_ReturnAddress() - (uintptr_t)GetModuleHandleW(nullptr)));
    if (VR_RenderSizeOverride(&w, &h)) LOGI("  -> render size override %ux%u", w, h);
    VR_OnResize(sc);
    return oResize(sc, n, w, h, f, fl);
}

using CreateSCFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSCHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
static CreateSCFn oCreateSC = nullptr; static CreateSCHwndFn oCreateSCHwnd = nullptr;
static HRESULT STDMETHODCALLTYPE hkCreateSC(IDXGIFactory* f, IUnknown* d, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    if (desc) {
        LOGI("CreateSwapChain %ux%u", desc->BufferDesc.Width, desc->BufferDesc.Height);
        DXGI_SWAP_CHAIN_DESC c = *desc; UINT w = c.BufferDesc.Width, h = c.BufferDesc.Height;
        if (VR_RenderSizeOverride(&w, &h)) { c.BufferDesc.Width = w; c.BufferDesc.Height = h; LOGI("  -> render size override %ux%u", w, h); return oCreateSC(f, d, &c, out); }
    }
    return oCreateSC(f, d, desc, out);
}
static HRESULT STDMETHODCALLTYPE hkCreateSCHwnd(IDXGIFactory2* f, IUnknown* d, HWND hw, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out) {
    if (desc) {
        LOGI("CreateSwapChainForHwnd %ux%u scaling %d", desc->Width, desc->Height, (int)desc->Scaling);
        DXGI_SWAP_CHAIN_DESC1 c = *desc; UINT w = c.Width, h = c.Height;
        if (VR_RenderSizeOverride(&w, &h)) { c.Width = w; c.Height = h; c.Scaling = DXGI_SCALING_STRETCH; LOGI("  -> render size override %ux%u", w, h); return oCreateSCHwnd(f, d, hw, &c, fs, o, out); }
    }
    return oCreateSCHwnd(f, d, hw, desc, fs, o, out);
}

void Passes_OnShaderCreated(void* shader, char kind, const void* code, SIZE_T len);
using CreatePSFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using CreateCSFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**);
static CreatePSFn oCreatePS = nullptr; static CreateCSFn oCreateCS = nullptr;
static HRESULT STDMETHODCALLTYPE hkCreatePS(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* l, ID3D11PixelShader** out) {
    HRESULT hr = oCreatePS(d, code, len, l, out);
    if (SUCCEEDED(hr) && out && *out) Passes_OnShaderCreated(*out, 'P', code, len);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hkCreateCS(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* l, ID3D11ComputeShader** out) {
    HRESULT hr = oCreateCS(d, code, len, l, out);
    if (SUCCEEDED(hr) && out && *out) Passes_OnShaderCreated(*out, 'C', code, len);
    return hr;
}
bool InstallD3DHooks() {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GranblueVRDummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { LOGE("Dummy window creation failed (%lu)", GetLastError()); return false; }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; sd.BufferCount = 1;
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    }
    if (FAILED(hr)) { LOGE("Dummy D3D11 device/swapchain failed (0x%08lx)", hr); DestroyWindow(hwnd); return false; }

    void** vt = *(void***)sc;
    void* pPresent = vt[8];
    void* pResize = vt[13];
    void* pPresent1 = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1))) {
        pPresent1 = (*(void***)sc1)[22];
        sc1->Release();
    }
    void** dvt = *(void***)dev;
    void* pCreatePS = dvt[15]; void* pCreateCS = dvt[18];
    sc->Release(); ctx->Release(); dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (MH_Initialize() != MH_OK) { LOGE("MH_Initialize failed"); return false; }
    bool ok = true;
    if (MH_CreateHook(pPresent, (void*)hkPresent, (void**)&oPresent) != MH_OK) { LOGE("Hook Present failed"); ok = false; }
    if (MH_CreateHook(pResize, (void*)hkResize, (void**)&oResize) != MH_OK) { LOGE("Hook ResizeBuffers failed"); ok = false; }
    if (pPresent1 && MH_CreateHook(pPresent1, (void*)hkPresent1, (void**)&oPresent1) != MH_OK) LOGE("Hook Present1 failed");
    {
        IDXGIFactory2* fac = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&fac)) && fac) {
            void** fvt = *(void***)fac;
            if (MH_CreateHook(fvt[10], (void*)hkCreateSC, (void**)&oCreateSC) != MH_OK) LOGE("Hook CreateSwapChain failed");
            if (MH_CreateHook(fvt[15], (void*)hkCreateSCHwnd, (void**)&oCreateSCHwnd) != MH_OK) LOGE("Hook CreateSwapChainForHwnd failed");
            fac->Release();
        }
    }
    // shader creation (shared device implementation): hash bytecode so post passes can be identified
    if (MH_CreateHook(pCreatePS, (void*)hkCreatePS, (void**)&oCreatePS) != MH_OK) LOGE("Hook CreatePixelShader failed");
    if (MH_CreateHook(pCreateCS, (void*)hkCreateCS, (void**)&oCreateCS) != MH_OK) LOGE("Hook CreateComputeShader failed");
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { LOGE("MH_EnableHook failed"); ok = false; }
    LOGI("D3D hooks installed: Present=%p Present1=%p ResizeBuffers=%p", pPresent, pPresent1, pResize);
    return ok;
}
