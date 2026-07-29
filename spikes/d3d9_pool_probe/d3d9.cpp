// d3d9_pool_probe - Singularity VR mod, spike 2
//
// A drop-in d3d9.dll shim that changes NOTHING about the game's behaviour. It forwards
// every export to the real system d3d9.dll and only *observes* which memory pool the
// engine asks for when it creates resources.
//
// Why: spike 1 proved D3D9 -> D3D11 sharing works, but only from a D3D9Ex device, and
// D3D9Ex has no D3DPOOL_MANAGED (CreateTexture returns 0x8876086C). UE3 may or may not
// rely on MANAGED. If it never asks for it, we can upgrade the game's device to Ex for
// free. If it asks constantly, the wrapper has to translate MANAGED -> DEFAULT and own
// the backing store. Guessing costs a week; measuring costs one run of the game.
//
// Deliberately observation-only: the device handed back is the ordinary non-Ex device the
// game asked for, so a bad result here cannot break the install.
//
// Install: build x86, copy d3d9.dll next to Singularity.exe (Binaries\). Run the game,
// reach gameplay, quit. Read the log, then DELETE the dll to uninstall.
// Log: %LOCALAPPDATA%\SingularityVR\d3d9_pool_probe.log

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------
namespace {

CRITICAL_SECTION g_logLock;
char g_logPath[MAX_PATH] = {};
bool g_logReady = false;

void LogInit() {
    InitializeCriticalSection(&g_logLock);
    char base[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) {
        strcat_s(base, "\\SingularityVR");
        CreateDirectoryA(base, nullptr);
        sprintf_s(g_logPath, "%s\\d3d9_pool_probe.log", base);
        // truncate previous run
        FILE* f = nullptr;
        if (fopen_s(&f, g_logPath, "w") == 0 && f) fclose(f);
        g_logReady = true;
    }
}

void Log(const char* fmt, ...) {
    if (!g_logReady) return;
    EnterCriticalSection(&g_logLock);
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") == 0 && f) {
        va_list ap; va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

const char* PoolName(D3DPOOL p) {
    switch (p) {
        case D3DPOOL_DEFAULT:   return "DEFAULT";
        case D3DPOOL_MANAGED:   return "MANAGED";
        case D3DPOOL_SYSTEMMEM: return "SYSTEMMEM";
        case D3DPOOL_SCRATCH:   return "SCRATCH";
        default:                return "?";
    }
}

// ---------------------------------------------------------------------------
// tallies - the actual output of this experiment
// ---------------------------------------------------------------------------
struct Counts { volatile LONG def = 0, managed = 0, sysmem = 0, scratch = 0, other = 0; };
Counts g_tex, g_vb, g_ib, g_cube, g_vol;

void Tally(Counts& c, D3DPOOL p) {
    switch (p) {
        case D3DPOOL_DEFAULT:   InterlockedIncrement(&c.def); break;
        case D3DPOOL_MANAGED:   InterlockedIncrement(&c.managed); break;
        case D3DPOOL_SYSTEMMEM: InterlockedIncrement(&c.sysmem); break;
        case D3DPOOL_SCRATCH:   InterlockedIncrement(&c.scratch); break;
        default:                InterlockedIncrement(&c.other); break;
    }
}

void DumpCounts(const char* label, const Counts& c) {
    Log("  %-14s DEFAULT=%-6ld MANAGED=%-6ld SYSTEMMEM=%-6ld SCRATCH=%-6ld other=%ld",
        label, c.def, c.managed, c.sysmem, c.scratch, c.other);
}

void DumpSummary() {
    Log("");
    Log("================ RESOURCE POOL SUMMARY ================");
    DumpCounts("CreateTexture", g_tex);
    DumpCounts("CreateVertexBuf", g_vb);
    DumpCounts("CreateIndexBuf", g_ib);
    DumpCounts("CreateCubeTex", g_cube);
    DumpCounts("CreateVolumeTex", g_vol);
    const LONG totalManaged = g_tex.managed + g_vb.managed + g_ib.managed +
                              g_cube.managed + g_vol.managed;
    Log("");
    Log("  TOTAL D3DPOOL_MANAGED allocations: %ld", totalManaged);
    if (totalManaged == 0) {
        Log("  VERDICT: no MANAGED usage seen -> a transparent D3D9Ex device upgrade");
        Log("           should be safe. This is the good outcome.");
    } else {
        Log("  VERDICT: engine DOES use MANAGED -> upgrading to D3D9Ex verbatim would");
        Log("           break it. Wrapper must translate MANAGED -> DEFAULT (+ own the");
        Log("           backing store), or fall back to the sysmem copy path.");
    }
    Log("======================================================");
}

// ---------------------------------------------------------------------------
// vtable patching. COM objects share a vtable per class, so patching once covers
// every instance. Far less code than wrapping 119 IDirect3DDevice9 methods.
// Indices are fixed ABI, taken from the declaration order in <d3d9.h>.
// ---------------------------------------------------------------------------
enum : int {
    kIDirect3D9_CreateDevice          = 16,
    kIDirect3DDevice9_CreateTexture       = 23,
    kIDirect3DDevice9_CreateVolumeTexture = 24,
    kIDirect3DDevice9_CreateCubeTexture   = 25,
    kIDirect3DDevice9_CreateVertexBuffer  = 26,
    kIDirect3DDevice9_CreateIndexBuffer   = 27,
};

void* PatchVTable(void* obj, int index, void* replacement) {
    void** vt = *reinterpret_cast<void***>(obj);
    DWORD oldProt = 0;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &oldProt)) return nullptr;
    void* original = vt[index];
    vt[index] = replacement;
    VirtualProtect(&vt[index], sizeof(void*), oldProt, &oldProt);
    return original;
}

// --- device-level hooks ----------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateTexture)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateVolumeTexture)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateCubeTexture)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateVertexBuffer)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateIndexBuffer)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);

PFN_CreateTexture       g_origCreateTexture       = nullptr;
PFN_CreateVolumeTexture g_origCreateVolumeTexture = nullptr;
PFN_CreateCubeTexture   g_origCreateCubeTexture   = nullptr;
PFN_CreateVertexBuffer  g_origCreateVertexBuffer  = nullptr;
PFN_CreateIndexBuffer   g_origCreateIndexBuffer   = nullptr;

// Only the first handful of each kind get an individual line; after that we just tally,
// so a long play session does not produce a gigabyte of log.
const LONG kVerboseFirstN = 12;

HRESULT STDMETHODCALLTYPE Hook_CreateTexture(IDirect3DDevice9* self, UINT w, UINT h, UINT levels,
        DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared) {
    if (g_tex.def + g_tex.managed + g_tex.sysmem + g_tex.scratch < kVerboseFirstN)
        Log("CreateTexture       %ux%u lv=%u usage=0x%08lX fmt=%d pool=%s", w, h, levels, usage, (int)fmt, PoolName(pool));
    Tally(g_tex, pool);
    return g_origCreateTexture(self, w, h, levels, usage, fmt, pool, out, shared);
}

HRESULT STDMETHODCALLTYPE Hook_CreateVolumeTexture(IDirect3DDevice9* self, UINT w, UINT h, UINT d,
        UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9** out, HANDLE* shared) {
    if (g_vol.def + g_vol.managed + g_vol.sysmem + g_vol.scratch < kVerboseFirstN)
        Log("CreateVolumeTexture %ux%ux%u pool=%s", w, h, d, PoolName(pool));
    Tally(g_vol, pool);
    return g_origCreateVolumeTexture(self, w, h, d, levels, usage, fmt, pool, out, shared);
}

HRESULT STDMETHODCALLTYPE Hook_CreateCubeTexture(IDirect3DDevice9* self, UINT edge, UINT levels,
        DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9** out, HANDLE* shared) {
    if (g_cube.def + g_cube.managed + g_cube.sysmem + g_cube.scratch < kVerboseFirstN)
        Log("CreateCubeTexture   edge=%u pool=%s", edge, PoolName(pool));
    Tally(g_cube, pool);
    return g_origCreateCubeTexture(self, edge, levels, usage, fmt, pool, out, shared);
}

HRESULT STDMETHODCALLTYPE Hook_CreateVertexBuffer(IDirect3DDevice9* self, UINT len, DWORD usage,
        DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* shared) {
    if (g_vb.def + g_vb.managed + g_vb.sysmem + g_vb.scratch < kVerboseFirstN)
        Log("CreateVertexBuffer  len=%u usage=0x%08lX pool=%s", len, usage, PoolName(pool));
    Tally(g_vb, pool);
    return g_origCreateVertexBuffer(self, len, usage, fvf, pool, out, shared);
}

HRESULT STDMETHODCALLTYPE Hook_CreateIndexBuffer(IDirect3DDevice9* self, UINT len, DWORD usage,
        D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE* shared) {
    if (g_ib.def + g_ib.managed + g_ib.sysmem + g_ib.scratch < kVerboseFirstN)
        Log("CreateIndexBuffer   len=%u usage=0x%08lX pool=%s", len, usage, PoolName(pool));
    Tally(g_ib, pool);
    return g_origCreateIndexBuffer(self, len, usage, fmt, pool, out, shared);
}

volatile LONG g_devicePatched = 0;

void PatchDevice(IDirect3DDevice9* dev) {
    if (InterlockedExchange(&g_devicePatched, 1) != 0) return;  // vtable is shared; once is enough
    g_origCreateTexture       = (PFN_CreateTexture)      PatchVTable(dev, kIDirect3DDevice9_CreateTexture,       (void*)&Hook_CreateTexture);
    g_origCreateVolumeTexture = (PFN_CreateVolumeTexture)PatchVTable(dev, kIDirect3DDevice9_CreateVolumeTexture, (void*)&Hook_CreateVolumeTexture);
    g_origCreateCubeTexture   = (PFN_CreateCubeTexture)  PatchVTable(dev, kIDirect3DDevice9_CreateCubeTexture,   (void*)&Hook_CreateCubeTexture);
    g_origCreateVertexBuffer  = (PFN_CreateVertexBuffer) PatchVTable(dev, kIDirect3DDevice9_CreateVertexBuffer,  (void*)&Hook_CreateVertexBuffer);
    g_origCreateIndexBuffer   = (PFN_CreateIndexBuffer)  PatchVTable(dev, kIDirect3DDevice9_CreateIndexBuffer,   (void*)&Hook_CreateIndexBuffer);
    Log("device vtable patched (CreateTexture/Volume/Cube/VB/IB)");
}

// --- IDirect3D9::CreateDevice hook ----------------------------------------
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
PFN_CreateDevice g_origCreateDevice = nullptr;

HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type,
        HWND focus, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    Log("CreateDevice adapter=%u type=%d flags=0x%08lX", adapter, (int)type, flags);
    if (pp) {
        Log("  backbuffer %ux%u fmt=%d count=%u windowed=%d msaa=%d msaaQ=%lu autoDepth=%d depthFmt=%d",
            pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->BackBufferFormat, pp->BackBufferCount,
            (int)pp->Windowed, (int)pp->MultiSampleType, pp->MultiSampleQuality,
            (int)pp->EnableAutoDepthStencil, (int)pp->AutoDepthStencilFormat);
        if (pp->MultiSampleType != D3DMULTISAMPLE_NONE)
            Log("  NOTE: backbuffer is MULTISAMPLED - shared surfaces cannot be MSAA; will need a resolve.");
    }
    HRESULT hr = g_origCreateDevice(self, adapter, type, focus, flags, pp, out);
    Log("  -> hr=0x%08lX", (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) PatchDevice(*out);
    return hr;
}

// ---------------------------------------------------------------------------
// real d3d9.dll passthrough
// ---------------------------------------------------------------------------
HMODULE g_real = nullptr;

typedef IDirect3D9*  (WINAPI *PFN_Direct3DCreate9)(UINT);
typedef HRESULT      (WINAPI *PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);
typedef int          (WINAPI *PFN_D3DPERF_BeginEvent)(D3DCOLOR, LPCWSTR);
typedef int          (WINAPI *PFN_D3DPERF_EndEvent)(void);
typedef DWORD        (WINAPI *PFN_D3DPERF_GetStatus)(void);
typedef void         (WINAPI *PFN_D3DPERF_SetMarker)(D3DCOLOR, LPCWSTR);
typedef void         (WINAPI *PFN_D3DPERF_SetRegion)(D3DCOLOR, LPCWSTR);
typedef BOOL         (WINAPI *PFN_D3DPERF_QueryRepeatFrame)(void);
typedef void         (WINAPI *PFN_D3DPERF_SetOptions)(DWORD);
typedef void         (WINAPI *PFN_DebugSetMute)(void);
typedef void*        (WINAPI *PFN_Generic)(void);

PFN_Direct3DCreate9          p_Direct3DCreate9 = nullptr;
PFN_Direct3DCreate9Ex        p_Direct3DCreate9Ex = nullptr;
PFN_D3DPERF_BeginEvent       p_D3DPERF_BeginEvent = nullptr;
PFN_D3DPERF_EndEvent         p_D3DPERF_EndEvent = nullptr;
PFN_D3DPERF_GetStatus        p_D3DPERF_GetStatus = nullptr;
PFN_D3DPERF_SetMarker        p_D3DPERF_SetMarker = nullptr;
PFN_D3DPERF_SetRegion        p_D3DPERF_SetRegion = nullptr;
PFN_D3DPERF_QueryRepeatFrame p_D3DPERF_QueryRepeatFrame = nullptr;
PFN_D3DPERF_SetOptions       p_D3DPERF_SetOptions = nullptr;
PFN_DebugSetMute             p_DebugSetMute = nullptr;

bool LoadReal() {
    char sys[MAX_PATH] = {};
    // In a 32-bit process this resolves to SysWOW64, which is what we want.
    if (!GetSystemDirectoryA(sys, MAX_PATH)) return false;
    strcat_s(sys, "\\d3d9.dll");
    g_real = LoadLibraryA(sys);
    if (!g_real) return false;
    Log("loaded real d3d9 from %s", sys);

    p_Direct3DCreate9          = (PFN_Direct3DCreate9)         GetProcAddress(g_real, "Direct3DCreate9");
    p_Direct3DCreate9Ex        = (PFN_Direct3DCreate9Ex)       GetProcAddress(g_real, "Direct3DCreate9Ex");
    p_D3DPERF_BeginEvent       = (PFN_D3DPERF_BeginEvent)      GetProcAddress(g_real, "D3DPERF_BeginEvent");
    p_D3DPERF_EndEvent         = (PFN_D3DPERF_EndEvent)        GetProcAddress(g_real, "D3DPERF_EndEvent");
    p_D3DPERF_GetStatus        = (PFN_D3DPERF_GetStatus)       GetProcAddress(g_real, "D3DPERF_GetStatus");
    p_D3DPERF_SetMarker        = (PFN_D3DPERF_SetMarker)       GetProcAddress(g_real, "D3DPERF_SetMarker");
    p_D3DPERF_SetRegion        = (PFN_D3DPERF_SetRegion)       GetProcAddress(g_real, "D3DPERF_SetRegion");
    p_D3DPERF_QueryRepeatFrame = (PFN_D3DPERF_QueryRepeatFrame)GetProcAddress(g_real, "D3DPERF_QueryRepeatFrame");
    p_D3DPERF_SetOptions       = (PFN_D3DPERF_SetOptions)      GetProcAddress(g_real, "D3DPERF_SetOptions");
    p_DebugSetMute             = (PFN_DebugSetMute)            GetProcAddress(g_real, "DebugSetMute");
    return p_Direct3DCreate9 != nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// exports
// ---------------------------------------------------------------------------
extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    Log("Direct3DCreate9(sdk=%u) - the game is using the NON-Ex entry point", sdk);
    if (!p_Direct3DCreate9) return nullptr;
    IDirect3D9* d3d = p_Direct3DCreate9(sdk);
    if (d3d) {
        g_origCreateDevice = (PFN_CreateDevice)PatchVTable(d3d, kIDirect3D9_CreateDevice, (void*)&Hook_CreateDevice);
        Log("  IDirect3D9::CreateDevice hooked");
    }
    return d3d;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    Log("Direct3DCreate9Ex(sdk=%u)", sdk);
    if (!p_Direct3DCreate9Ex) return E_NOTIMPL;
    return p_Direct3DCreate9Ex(sdk, out);
}

int  WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n) { return p_D3DPERF_BeginEvent ? p_D3DPERF_BeginEvent(c, n) : 0; }
int  WINAPI D3DPERF_EndEvent(void)                    { return p_D3DPERF_EndEvent ? p_D3DPERF_EndEvent() : 0; }
DWORD WINAPI D3DPERF_GetStatus(void)                  { return p_D3DPERF_GetStatus ? p_D3DPERF_GetStatus() : 0; }
void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n)  { if (p_D3DPERF_SetMarker) p_D3DPERF_SetMarker(c, n); }
void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n)  { if (p_D3DPERF_SetRegion) p_D3DPERF_SetRegion(c, n); }
BOOL WINAPI D3DPERF_QueryRepeatFrame(void)            { return p_D3DPERF_QueryRepeatFrame ? p_D3DPERF_QueryRepeatFrame() : FALSE; }
void WINAPI D3DPERF_SetOptions(DWORD o)               { if (p_D3DPERF_SetOptions) p_D3DPERF_SetOptions(o); }
void WINAPI DebugSetMute(void)                        { if (p_DebugSetMute) p_DebugSetMute(); }

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        LogInit();
        Log("=== d3d9_pool_probe attached (observation only, game behaviour unchanged) ===");
        if (!LoadReal()) { Log("FATAL: could not load the real d3d9.dll"); return FALSE; }
    } else if (reason == DLL_PROCESS_DETACH) {
        DumpSummary();
        Log("=== detached ===");
    }
    return TRUE;
}
