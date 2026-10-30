// AMD FSR Frame Generation through the FidelityFX API. The signed runtime
// (amd_fidelityfx_loader_dx12.dll + amd_fidelityfx_framegeneration_dx12.dll, FSR SDK 2.4.0) sits
// next to the game exe.
//
// AMD's order, every frame: ffxConfigure (frame generation description: the swapchain, a callback
// that dispatches frame generation, the picture before the HUD, frame id + 1), then the prepare
// pass (depth, motion vectors, camera), then the swapchain's Present runs the callback. The
// configure and prepare happen right before the game's Present (framegen.cpp's present hook): all
// the frame's command lists are submitted by then, so the prepare pass goes in a command list of
// our own, submitted on the game's queue after them.
//
// Lifetime: the frame generation context is destroyed (after disabling it, which waits for
// presents) before FSR's swapchain context, and both before the game's last reference to the
// swapchain goes (framegen.cpp hooks the swapchain's Release).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "common.hpp"
#include "fsrfg.hpp"

#ifndef _WINDOWS
#define _WINDOWS
#endif
#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_framegeneration_dx12.h>

#include <cstdio>
#include <mutex>

namespace fsrfg {
namespace {

// AMD: context create/destroy, the prepare dispatch and the swapchain's present must not overlap.
std::recursive_mutex g_mutex;
HMODULE g_module = nullptr;
ffxFunctions g_ffx = {};
bool g_loadTried = false, g_loaded = false;
std::string g_loadError;

ffxContext g_swapCtx = nullptr;   // FSR's swapchain
IDXGISwapChain4* g_proxy = nullptr;  // not a counted reference of ours: the game owns it
ULONG g_ownRefs = 0;
ID3D12CommandQueue* g_queue = nullptr;  // the game's (referenced)
ID3D12Device* g_device = nullptr;       // referenced

ffxContext g_fgCtx = nullptr;  // frame generation
unsigned g_ctxW = 0, g_ctxH = 0;
DXGI_FORMAT g_ctxFormat = DXGI_FORMAT_UNKNOWN;
bool g_ctxHdr = false;
uint64_t g_frameId = 0;
bool g_enabled = false;  // what the last configure said
std::string g_status = "not started", g_version;

FrameInputs g_last;  // the last frame's inputs, reused (with reset) for a frame without any
bool g_haveLast = false;

// Our command lists for the prepare pass: three allocators, in turn, behind a fence.
ID3D12CommandAllocator* g_alloc[3] = {};
uint64_t g_allocFence[3] = {};
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
uint64_t g_fenceValue = 0;
int g_allocNext = 0;
HANDLE g_event = nullptr;

LARGE_INTEGER g_freq = {}, g_lastPresent = {};
float g_frameMs = 33.3f;

std::string Code(ffxReturnCode_t r) {
    switch (r) {
        case FFX_API_RETURN_OK: return "ok";
        case FFX_API_RETURN_ERROR: return "error";
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown description type";
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "runtime error";
        case FFX_API_RETURN_NO_PROVIDER: return "no provider (GPU, driver or DLL missing)";
        case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
        case FFX_API_RETURN_ERROR_PARAMETER: return "bad parameter";
        case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "runtime too old";
        default: return "code " + std::to_string(r);
    }
}

ffxReturnCode_t DispatchCallback(ffxDispatchDescFrameGeneration* params, void* user) {
    return g_ffx.Dispatch(static_cast<ffxContext*>(user), &params->header);
}

bool CommandObjects() {
    if (g_list) return true;
    for (ID3D12CommandAllocator*& a : g_alloc)
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list))))
        return false;
    g_list->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g_event != nullptr;
}

void WaitFence(uint64_t value) {
    if (!g_fence || g_fence->GetCompletedValue() >= value) return;
    g_fence->SetEventOnCompletion(value, g_event);
    WaitForSingleObject(g_event, 2000);
}

void ReleaseCommandObjects() {
    WaitFence(g_fenceValue);
    for (ID3D12CommandAllocator*& a : g_alloc)
        if (a) a->Release(), a = nullptr;
    if (g_list) g_list->Release(), g_list = nullptr;
    if (g_fence) g_fence->Release(), g_fence = nullptr;
    if (g_event) CloseHandle(g_event), g_event = nullptr;
}

// Frame generation off and its context gone (configure first: it waits for the presents using it).
void DestroyFrameGen() {
    if (!g_fgCtx) return;
    ffxConfigureDescFrameGeneration c = {};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = g_proxy;
    c.frameGenerationEnabled = false;
    c.frameID = ++g_frameId;
    g_ffx.Configure(&g_fgCtx, &c.header);
    g_ffx.DestroyContext(&g_fgCtx, nullptr);
    g_fgCtx = nullptr;
    g_enabled = false;
    rd::Log("FSR frame generation: context released");
}

bool CreateFrameGen(unsigned w, unsigned h, DXGI_FORMAT format, bool hdr) {
    ffxCreateContextDescFrameGeneration d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    d.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE |
              (hdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0u);
    d.displaySize = {w, h};
    d.maxRenderSize = {w, h};
    d.backBufferFormat = ffxApiGetSurfaceFormatDX12(format);
    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = g_device;
    ffxCreateContextDescFrameGenerationVersion version = {};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    version.version = FFX_FRAMEGENERATION_VERSION;
    d.header.pNext = &backend.header;
    backend.header.pNext = &version.header;
    const ffxReturnCode_t r = g_ffx.CreateContext(&g_fgCtx, &d.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        g_fgCtx = nullptr;
        g_status = "frame generation context creation failed (" + Code(r) + ")";
        rd::Log("FSR frame generation: %s (%ux%u, format %u, %s)", g_status.c_str(), w, h, static_cast<unsigned>(format),
                hdr ? "HDR" : "SDR");
        return false;
    }
    g_ctxW = w, g_ctxH = h, g_ctxFormat = format, g_ctxHdr = hdr;
    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (g_ffx.Query(&g_fgCtx, &pv.header) == FFX_API_RETURN_OK && pv.versionName) {
        g_version = pv.versionName;
        if (g_version.rfind("FSR", 0) != 0) g_version = "FSR FG " + g_version;
    }
    g_status = "ready";
    rd::Log("FSR frame generation: context created, %ux%u, format %u, %s, %s", w, h, static_cast<unsigned>(format),
            hdr ? "HDR" : "SDR", g_version.empty() ? "version unknown" : g_version.c_str());
    return true;
}

FfxApiResource Res(ID3D12Resource* r, uint32_t state) { return ffxApiGetResourceDX12(r, state); }

void Prepare(const FrameInputs& in, bool reset) {
    if (!CommandObjects()) {
        g_status = "couldn't create command lists";
        return;
    }
    const int i = g_allocNext;
    g_allocNext = (g_allocNext + 1) % 3;
    WaitFence(g_allocFence[i]);
    g_alloc[i]->Reset();
    g_list->Reset(g_alloc[i], nullptr);

    ffxDispatchDescFrameGenerationPrepareV2 p = {};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    p.frameID = g_frameId;
    p.commandList = g_list;
    p.renderSize = {in.renderWidth, in.renderHeight};
    p.jitterOffset = {in.jitterX, in.jitterY};
    p.motionVectorScale = {1, 1};  // our motion vectors are in render pixels
    p.frameTimeDelta = g_frameMs;
    p.reset = reset;
    p.cameraNear = in.cameraNear;
    p.cameraFar = 1e8f;  // infinite far plane (flag set)
    p.cameraFovAngleVertical = in.cameraFovY;
    p.viewSpaceToMetersFactor = 0.01f;  // UE units are centimetres
    // Depth copy in depth read + shader resource, motion in non-pixel shader resource: both
    // readable as is, so FSR adds no barriers.
    p.depth = Res(in.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    p.motionVectors = Res(in.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    for (int k = 0; k < 3; ++k) {
        p.cameraPosition[k] = in.position[k];
        p.cameraUp[k] = in.up[k];
        p.cameraRight[k] = in.right[k];
        p.cameraForward[k] = in.forward[k];
    }
    const ffxReturnCode_t r = g_ffx.Dispatch(&g_fgCtx, &p.header);
    g_list->Close();
    if (r != FFX_API_RETURN_OK) {
        static int logged = 0;
        if (logged++ < 10) rd::Log("FSR frame generation: prepare failed (%s)", Code(r).c_str());
        g_status = "prepare failed (" + Code(r) + ")";
        return;
    }
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);
    g_allocFence[i] = g_fenceValue;
}

}  // namespace

bool Load(std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_loadTried) {
        error = g_loadError;
        return g_loaded;
    }
    g_loadTried = true;
    QueryPerformanceFrequency(&g_freq);
    const std::wstring fg = rd::ExeDir() + L"amd_fidelityfx_framegeneration_dx12.dll";
    if (GetFileAttributesW(fg.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_loadError = "amd_fidelityfx_framegeneration_dx12.dll missing next to the game exe";
    } else {
        g_module = LoadLibraryW((rd::ExeDir() + L"amd_fidelityfx_loader_dx12.dll").c_str());
        if (!g_module) g_loadError = "couldn't load amd_fidelityfx_loader_dx12.dll";
    }
    if (g_module) {
        ffxLoadFunctions(&g_ffx, g_module);
        if (!g_ffx.CreateContext || !g_ffx.DestroyContext || !g_ffx.Configure || !g_ffx.Dispatch || !g_ffx.Query)
            g_loadError = "amd_fidelityfx_loader_dx12.dll is incomplete";
    }
    g_loaded = g_loadError.empty();
    g_status = g_loaded ? "loaded" : g_loadError;
    rd::Log("FSR frame generation: %s", g_loaded ? "runtime loaded" : g_loadError.c_str());
    error = g_loadError;
    return g_loaded;
}

namespace {

// Shared by both creation paths once FSR's swapchain exists.
void Adopt(ID3D12CommandQueue* queue, IDXGISwapChain4* sc) {
    g_proxy = sc;
    queue->AddRef();
    g_queue = queue;
    if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&g_device)))) {
        // References on the swapchain besides the game's one (FSR's own): counted now.
        sc->AddRef();
        const ULONG n = sc->Release();
        g_ownRefs = n > 0 ? n - 1 : 0;
        g_status = "swapchain created";
        rd::Log("FSR frame generation: the game's swapchain is FSR's (references: %lu, FSR's own %lu)", n, g_ownRefs);
    }
}

}  // namespace

HRESULT CreateSwapChainForHwnd(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGISwapChain1** out, std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_loaded || g_swapCtx) return error = g_loaded ? "a swapchain exists" : g_loadError, E_FAIL;
    DXGI_SWAP_CHAIN_DESC1 d1 = *desc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs = fullscreen ? *fullscreen : DXGI_SWAP_CHAIN_FULLSCREEN_DESC{};
    IDXGISwapChain4* sc = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 c = {};
    c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    c.swapchain = &sc;
    c.hwnd = hwnd;
    c.desc = &d1;
    c.fullscreenDesc = fullscreen ? &fs : nullptr;
    c.dxgiFactory = factory;
    c.gameQueue = queue;
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 v = {};
    v.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    v.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    c.header.pNext = &v.header;
    const ffxReturnCode_t r = g_ffx.CreateContext(&g_swapCtx, &c.header, nullptr);
    if (r != FFX_API_RETURN_OK || !sc) {
        g_swapCtx = nullptr;
        error = "FSR swapchain creation failed (" + Code(r) + ")";
        g_status = error;
        return E_FAIL;
    }
    Adopt(queue, sc);
    *out = sc;
    return S_OK;
}

HRESULT CreateSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out,
                        std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_loaded || g_swapCtx) return error = g_loaded ? "a swapchain exists" : g_loadError, E_FAIL;
    IDXGISwapChain4* sc = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainNewDX12 c = {};
    c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12;
    c.swapchain = &sc;
    c.desc = desc;
    c.dxgiFactory = factory;
    c.gameQueue = queue;
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 v = {};
    v.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    v.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    c.header.pNext = &v.header;
    const ffxReturnCode_t r = g_ffx.CreateContext(&g_swapCtx, &c.header, nullptr);
    if (r != FFX_API_RETURN_OK || !sc) {
        g_swapCtx = nullptr;
        error = "FSR swapchain creation failed (" + Code(r) + ")";
        g_status = error;
        return E_FAIL;
    }
    Adopt(queue, sc);
    *out = sc;
    return S_OK;
}

IUnknown* Proxy() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_proxy;
}

ULONG OwnReferences() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_ownRefs;
}

void BeforePresent(bool enable, const FrameInputs* in, ID3D12Resource* hudless, unsigned bbWidth, unsigned bbHeight,
                   DXGI_FORMAT bbFormat, bool hdr) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_proxy || !g_device) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_freq.QuadPart && g_lastPresent.QuadPart) {
        const float ms = static_cast<float>(now.QuadPart - g_lastPresent.QuadPart) * 1000.0f / g_freq.QuadPart;
        g_frameMs = ms > 0.5f && ms < 500 ? ms : 33.3f;
    }
    g_lastPresent = now;

    if (in) g_last = *in, g_haveLast = true;
    const bool want = enable && g_haveLast && bbWidth && bbHeight;
    if (!want) {
        if (g_enabled && g_fgCtx) {  // pause (this waits for the presents in flight once)
            ffxConfigureDescFrameGeneration c = {};
            c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            c.swapChain = g_proxy;
            c.frameGenerationEnabled = false;
            c.frameID = ++g_frameId;
            g_ffx.Configure(&g_fgCtx, &c.header);
            g_enabled = false;
            rd::Log("FSR frame generation paused");
        }
        return;
    }
    // A new back buffer size, format or HDR mode: a new context.
    if (g_fgCtx && (g_ctxW != bbWidth || g_ctxH != bbHeight || g_ctxFormat != bbFormat || g_ctxHdr != hdr)) DestroyFrameGen();
    if (!g_fgCtx && !CreateFrameGen(bbWidth, bbHeight, bbFormat, hdr)) return;

    ffxConfigureDescFrameGeneration c = {};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = g_proxy;
    c.frameGenerationCallback = &DispatchCallback;
    c.frameGenerationCallbackUserContext = &g_fgCtx;
    c.frameGenerationEnabled = true;
    c.allowAsyncWorkloads = false;
    c.HUDLessColor = hudless ? Res(hudless, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ) : FfxApiResource{};
    c.generationRect = {0, 0, static_cast<int32_t>(bbWidth), static_cast<int32_t>(bbHeight)};
    c.frameID = ++g_frameId;
    const ffxReturnCode_t r = g_ffx.Configure(&g_fgCtx, &c.header);
    if (r != FFX_API_RETURN_OK) {
        static int logged = 0;
        if (logged++ < 10) rd::Log("FSR frame generation: configure failed (%s)", Code(r).c_str());
        g_status = "configure failed (" + Code(r) + ")";
        return;
    }
    if (!g_enabled) rd::Log("FSR frame generation on (%ux%u%s)", bbWidth, bbHeight, hudless ? ", with the picture before the HUD" : "");
    g_enabled = true;
    // A frame without inputs of its own (a hitch) reuses the last ones, as a reset.
    Prepare(g_last, in ? in->reset : true);
    g_status = "running";
}

void BeforeSwapchainRelease() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_swapCtx) return;
    DestroyFrameGen();
    ReleaseCommandObjects();
    // Cleared first: FSR may release its own references while its context goes, which comes back
    // through the hooked Release.
    ffxContext ctx = g_swapCtx;
    g_swapCtx = nullptr;
    g_proxy = nullptr;
    g_ffx.DestroyContext(&ctx, nullptr);
    g_haveLast = false;
    if (g_queue) g_queue->Release(), g_queue = nullptr;
    if (g_device) g_device->Release(), g_device = nullptr;
    g_status = "swapchain released";
    rd::Log("FSR frame generation: shut down with the game's swapchain");
}

void Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_swapCtx) BeforeSwapchainRelease();
    ReleaseCommandObjects();
}

bool Running() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_fgCtx && g_enabled;
}

std::string Status() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_status;
}

std::string Version() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_version.empty() ? "FSR frame generation" : g_version;
}

}  // namespace fsrfg
