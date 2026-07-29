// camera_write - Singularity VR mod, spike 5
//
// FIRST SPIKE THAT WRITES TO ENGINE STATE. Everything before this only observed.
//
// Question: can we drive the camera by writing RvPlayerCamera::mCurrentPOV (+0x0420), and
// does that write SURVIVE the engine's own per-frame camera update? Deliberately does not
// inject a real HMD pose - a constant FOV change and a slow yaw sweep are unambiguous on a
// flat monitor, and isolate "does the write land" from all the VR maths.
//
// The important measurement is the read-back: each frame we record what we wrote, then on
// the NEXT frame check whether the engine clobbered it. That answers the write-timing and
// interpolation questions with data instead of guesswork.
//
// Controls (in game):
//   F9  toggle override on/off
//   F7  cycle mode: FOV only -> yaw sweep -> both
//   F10 dump current camera state to the log
//
// Log: %LOCALAPPDATA%\SingularityVR\camera_write.log

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>
#include <cmath>

namespace {

// ---- verified engine addresses (image base 0x400000, no ASLR) ----
const uintptr_t kGNamesTArray   = 0x01CD4A1C;
const uintptr_t kGObjectsTArray = 0x01CD4A4C;

// UObject layout
const int OBJ_OUTER = 0x28, OBJ_NAME = 0x2C, OBJ_CLASS = 0x34;
// RvPlayerCamera
const int POV_CURRENT = 0x0420;   // TPOV: FVector(12) + FRotator(12) + FLOAT fov
const int POV_DESIRED = 0x0458;
const int CUR_FOV     = 0x0490;

struct TPOV {
    float   x, y, z;
    int32_t pitch, yaw, roll;   // FRotator, 65536 == 360 degrees
    float   fov;
};
static_assert(sizeof(TPOV) == 28, "TPOV must be 28 bytes");

CRITICAL_SECTION g_lock;
char g_logPath[MAX_PATH] = {};
bool g_logReady = false;

void LogInit() {
    InitializeCriticalSection(&g_lock);
    char base[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) {
        strcat_s(base, "\\SingularityVR");
        CreateDirectoryA(base, nullptr);
        sprintf_s(g_logPath, "%s\\camera_write.log", base);
        FILE* f = nullptr;
        if (fopen_s(&f, g_logPath, "w") == 0 && f) fclose(f);
        g_logReady = true;
    }
}
void Log(const char* fmt, ...) {
    if (!g_logReady) return;
    EnterCriticalSection(&g_lock);
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") == 0 && f) {
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f); fclose(f);
    }
    LeaveCriticalSection(&g_lock);
}

bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return ((uintptr_t)p + n) <= ((uintptr_t)mbi.BaseAddress + mbi.RegionSize);
}

bool NameOf(uintptr_t obj, char* buf, size_t cap) {
    buf[0] = 0;
    if (!Readable((void*)kGNamesTArray, 12) || !Readable((void*)obj, 0x40)) return false;
    uintptr_t nd = *reinterpret_cast<uintptr_t*>(kGNamesTArray);
    int32_t nc = *reinterpret_cast<int32_t*>(kGNamesTArray + 4);
    int32_t idx = *reinterpret_cast<int32_t*>(obj + OBJ_NAME);
    if (!nd || idx < 0 || idx >= nc) return false;
    if (!Readable((void*)(nd + idx * 4), 4)) return false;
    uintptr_t e = reinterpret_cast<uintptr_t*>(nd)[idx];
    if (!e || !Readable((void*)e, 0x40)) return false;
    bool wide = (*reinterpret_cast<uint32_t*>(e + 8)) & 1;
    size_t n = 0;
    if (wide) { auto* w = reinterpret_cast<const wchar_t*>(e + 0x10);
                for (; n < cap-1 && w[n]; ++n) buf[n] = (char)(w[n] < 127 ? w[n] : '?'); }
    else      { auto* a = reinterpret_cast<const char*>(e + 0x10);
                for (; n < cap-1 && a[n]; ++n) buf[n] = a[n]; }
    buf[n] = 0;
    return n > 0;
}

// ---------------------------------------------------------------- find the camera
uintptr_t g_camClass = 0;   // UClass RvPlayerCamera
uintptr_t g_camera   = 0;   // the live instance

bool LooksLikeOurCamera(uintptr_t o) {
    if (!o || !g_camClass || !Readable((void*)o, 0x40)) return false;
    return *reinterpret_cast<uintptr_t*>(o + OBJ_CLASS) == g_camClass;
}

// Instances are recreated on level load, so re-enumerate whenever the cached one goes stale.
bool FindCamera() {
    if (LooksLikeOurCamera(g_camera)) return true;
    g_camera = 0;
    if (!Readable((void*)kGObjectsTArray, 12)) return false;
    uintptr_t data = *reinterpret_cast<uintptr_t*>(kGObjectsTArray);
    int32_t count  = *reinterpret_cast<int32_t*>(kGObjectsTArray + 4);
    if (!data || count < 100) return false;
    auto* objs = reinterpret_cast<uintptr_t*>(data);
    char nm[128], on[128];

    if (!g_camClass) {
        for (int i = 0; i < count; ++i) {
            if (!Readable((void*)(data + i*4), 4)) continue;
            uintptr_t o = objs[i];
            if (!o || !Readable((void*)o, 0x40)) continue;
            if (!NameOf(o, nm, sizeof(nm)) || _stricmp(nm, "RvPlayerCamera") != 0) continue;
            uintptr_t outer = *reinterpret_cast<uintptr_t*>(o + OBJ_OUTER);
            if (outer && Readable((void*)outer, 0x40) && NameOf(outer, on, sizeof(on))
                && _stricmp(on, "RvGame") == 0) { g_camClass = o; Log("UClass RvPlayerCamera @ 0x%08X", o); break; }
        }
        if (!g_camClass) return false;
    }

    for (int i = 0; i < count; ++i) {
        if (!Readable((void*)(data + i*4), 4)) continue;
        uintptr_t o = objs[i];
        if (!o || !Readable((void*)o, POV_CURRENT + 64)) continue;
        if (*reinterpret_cast<uintptr_t*>(o + OBJ_CLASS) != g_camClass) continue;
        if (!NameOf(o, nm, sizeof(nm)) || strncmp(nm, "Default__", 9) == 0) continue;
        g_camera = o;
        Log("live RvPlayerCamera @ 0x%08X (%s)", o, nm);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- override state
// AActor layout, found by the F6 rotation hunt: Location +0x54, Rotation +0x60.
// The POV structs at +0x328/+0x350/+0x420/+0x458 all mirror the same values, i.e. they are
// downstream COPIES - which is exactly why writing mCurrentPOV.Rotation changed nothing.
const int ACTOR_LOCATION = 0x0054;
const int ACTOR_ROTATION = 0x0060;   // FRotator {pitch, yaw, roll} int32

// defined further down with the rotation-hunt code
bool FindController();
extern uintptr_t g_controller;

volatile LONG g_enabled = 0;
int  g_mode = 0;
const char* kModeName[] = {
    "FOV only",                    // 0 - known to work
    "yaw via mCurrentPOV",         // 1 - known NOT to work (POV is a copy)
    "FOV + POV yaw",               // 2
    "yaw via CAMERA Actor.Rotation",      // 3 - new
    "yaw via CONTROLLER Actor.Rotation",  // 4 - new
};
const int kNumModes = 5;

TPOV g_lastWritten{};
bool g_haveLastWritten = false;
int  g_survived = 0, g_clobbered = 0;
int  g_reportTick = 0;

void ApplyOverride() {
    if (!FindCamera()) return;
    uintptr_t p = g_camera + POV_CURRENT;
    if (!Readable((void*)p, sizeof(TPOV))) return;
    TPOV* pov = reinterpret_cast<TPOV*>(p);

    // --- read back: did last frame's write survive the engine's update? ---
    if (g_haveLastWritten) {
        bool fovKept = fabsf(pov->fov - g_lastWritten.fov) < 0.01f;
        bool yawKept = (pov->yaw == g_lastWritten.yaw);
        bool kept = (g_mode == 0) ? fovKept : (g_mode == 1 ? yawKept : (fovKept && yawKept));
        if (kept) g_survived++; else g_clobbered++;

        if (++g_reportTick % 120 == 0) {
            Log("readback: survived=%d clobbered=%d   now fov=%.2f yaw=%d (wrote fov=%.2f yaw=%d)",
                g_survived, g_clobbered, pov->fov, pov->yaw, g_lastWritten.fov, g_lastWritten.yaw);
            g_survived = g_clobbered = 0;
        }
    }

    if (!InterlockedCompareExchange(&g_enabled, 0, 0)) { g_haveLastWritten = false; return; }

    // --- write something unmistakable ---
    static float t = 0.0f;
    t += 1.0f / 60.0f;
    const int32_t sweepYaw = (int32_t)(fmodf(t / 8.0f, 1.0f) * 65536.0f);  // full turn / 8s

    if (g_mode == 0 || g_mode == 2) {
        pov->fov = 110.0f;                                       // vs the normal 65
        if (Readable((void*)(g_camera + POV_DESIRED), sizeof(TPOV)))
            reinterpret_cast<TPOV*>(g_camera + POV_DESIRED)->fov = pov->fov;
        if (Readable((void*)(g_camera + CUR_FOV), 4))
            *reinterpret_cast<float*>(g_camera + CUR_FOV) = pov->fov;
    }
    if (g_mode == 1 || g_mode == 2) {
        pov->yaw = sweepYaw;
        if (Readable((void*)(g_camera + POV_DESIRED), sizeof(TPOV)))
            reinterpret_cast<TPOV*>(g_camera + POV_DESIRED)->yaw = sweepYaw;
    }
    if (g_mode == 3 && Readable((void*)(g_camera + ACTOR_ROTATION), 12)) {
        reinterpret_cast<int32_t*>(g_camera + ACTOR_ROTATION)[1] = sweepYaw;   // yaw
    }
    if (g_mode == 4 && FindController() && Readable((void*)(g_controller + ACTOR_ROTATION), 12)) {
        reinterpret_cast<int32_t*>(g_controller + ACTOR_ROTATION)[1] = sweepYaw;
    }

    g_lastWritten = *pov;
    g_haveLastWritten = true;
}

// ---------------------------------------------------------------- rotation hunt
// mCurrentPOV.Rotation writes persist (clobbered=0) but do not steer the view, so the
// renderer takes rotation from elsewhere. Find it by watching which dwords actually change
// while the player looks around: snapshot, wait for mouse movement, diff.
//
// A live FRotator yaw is a dword that changes smoothly and stays inside +/-65536-ish.
const int kSnapBytes = 0x900;
uint8_t g_snapCam[kSnapBytes], g_snapCtl[kSnapBytes];
bool    g_haveSnap = false;
uintptr_t g_controller = 0;
uintptr_t g_ctlClass = 0;

bool FindController() {
    if (g_controller && Readable((void*)g_controller, 0x40) && g_ctlClass &&
        *reinterpret_cast<uintptr_t*>(g_controller + OBJ_CLASS) == g_ctlClass) return true;
    g_controller = 0;
    if (!Readable((void*)kGObjectsTArray, 12)) return false;
    uintptr_t data = *reinterpret_cast<uintptr_t*>(kGObjectsTArray);
    int32_t count  = *reinterpret_cast<int32_t*>(kGObjectsTArray + 4);
    if (!data) return false;
    auto* objs = reinterpret_cast<uintptr_t*>(data);
    char nm[128], on[128];
    if (!g_ctlClass) {
        for (int i = 0; i < count; ++i) {
            if (!Readable((void*)(data + i*4), 4)) continue;
            uintptr_t o = objs[i];
            if (!o || !Readable((void*)o, 0x40)) continue;
            if (!NameOf(o, nm, sizeof(nm)) || _stricmp(nm, "RvPlayerController") != 0) continue;
            uintptr_t ou = *reinterpret_cast<uintptr_t*>(o + OBJ_OUTER);
            if (ou && Readable((void*)ou, 0x40) && NameOf(ou, on, sizeof(on)) && !_stricmp(on, "RvGame")) {
                g_ctlClass = o; break;
            }
        }
        if (!g_ctlClass) return false;
    }
    for (int i = 0; i < count; ++i) {
        if (!Readable((void*)(data + i*4), 4)) continue;
        uintptr_t o = objs[i];
        if (!o || !Readable((void*)o, kSnapBytes)) continue;
        if (*reinterpret_cast<uintptr_t*>(o + OBJ_CLASS) != g_ctlClass) continue;
        if (!NameOf(o, nm, sizeof(nm)) || strncmp(nm, "Default__", 9) == 0) continue;
        g_controller = o;
        Log("live RvPlayerController @ 0x%08X", o);
        return true;
    }
    return false;
}

void RotationSnapshot() {
    if (!FindCamera() || !FindController()) { Log("F6: camera or controller missing"); return; }
    if (!Readable((void*)g_camera, kSnapBytes) || !Readable((void*)g_controller, kSnapBytes)) return;
    memcpy(g_snapCam, (void*)g_camera, kSnapBytes);
    memcpy(g_snapCtl, (void*)g_controller, kSnapBytes);
    g_haveSnap = true;
    Log("F6: snapshot taken. NOW LOOK AROUND (mouse), then press F6 again to see what changed.");
}

void RotationDiff() {
    if (!g_haveSnap) { RotationSnapshot(); return; }
    if (!FindCamera() || !FindController()) return;
    Log("");
    Log("=== fields that changed while looking around ===");
    struct Src { const char* tag; uintptr_t base; uint8_t* snap; };
    Src srcs[] = { { "camera",     g_camera,     g_snapCam },
                   { "controller", g_controller, g_snapCtl } };
    for (auto& s : srcs) {
        if (!Readable((void*)s.base, kSnapBytes)) continue;
        Log("  -- %s @ 0x%08X --", s.tag, s.base);
        for (int off = 0; off + 12 <= kSnapBytes; off += 4) {
            int32_t oldv = *reinterpret_cast<int32_t*>(s.snap + off);
            int32_t newv = *reinterpret_cast<int32_t*>(s.base + off);
            if (oldv == newv) continue;
            // a rotator component: plausible angle magnitude, and an integer not a float
            float asF = *reinterpret_cast<float*>(s.base + off);
            bool rotLike = (newv > -200000 && newv < 200000);
            bool floatLike = (asF > -100000.0f && asF < 100000.0f && asF != 0.0f && fabsf(asF) > 0.001f);
            const char* hint = "";
            if (rotLike && !floatLike) hint = "  <-- ROTATOR-like (int angle)";
            else if (floatLike)        hint = "  (float)";
            Log("     +0x%04X  %11d -> %11d   (as float %.3f)%s", off, oldv, newv, asF, hint);
        }
    }
    Log("=== end diff - press F6 to snapshot again ===");
    g_haveSnap = false;
}

void DumpState() {
    if (!FindCamera()) { Log("F10: no live camera found"); return; }
    if (!Readable((void*)(g_camera + POV_CURRENT), sizeof(TPOV))) return;
    TPOV* c = reinterpret_cast<TPOV*>(g_camera + POV_CURRENT);
    TPOV* d = reinterpret_cast<TPOV*>(g_camera + POV_DESIRED);
    Log("F10 state: camera=0x%08X override=%s mode=%s", g_camera,
        InterlockedCompareExchange(&g_enabled,0,0) ? "ON" : "off", kModeName[g_mode]);
    Log("   mCurrentPOV loc(%.1f, %.1f, %.1f) rot(%d, %d, %d) fov %.2f",
        c->x, c->y, c->z, c->pitch, c->yaw, c->roll, c->fov);
    Log("   mDesiredPOV loc(%.1f, %.1f, %.1f) rot(%d, %d, %d) fov %.2f",
        d->x, d->y, d->z, d->pitch, d->yaw, d->roll, d->fov);
}

// ---------------------------------------------------------------- hooks
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
PFN_Present g_origPresent = nullptr;

HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* s, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    static bool p9 = false, p7 = false, p10 = false, p6 = false;
    bool k9  = (GetAsyncKeyState(VK_F9)  & 0x8000) != 0;
    bool k7  = (GetAsyncKeyState(VK_F7)  & 0x8000) != 0;
    bool k10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    bool k6  = (GetAsyncKeyState(VK_F6)  & 0x8000) != 0;
    if (k6 && !p6) RotationDiff();
    p6 = k6;

    if (k9 && !p9) {
        LONG now = InterlockedExchange(&g_enabled, InterlockedCompareExchange(&g_enabled,0,0) ? 0 : 1);
        Log("F9: override now %s (mode %s)", now ? "OFF" : "ON", kModeName[g_mode]);
        g_haveLastWritten = false;
    }
    if (k7 && !p7) { g_mode = (g_mode + 1) % kNumModes; Log("F7: mode -> %d %s", g_mode, kModeName[g_mode]); g_haveLastWritten = false; }
    if (k10 && !p10) DumpState();
    p9 = k9; p7 = k7; p10 = k10;

    ApplyOverride();
    return g_origPresent(s, a, b, c, d);
}

void* PatchVTable(void* obj, int index, void* repl) {
    void** vt = *reinterpret_cast<void***>(obj);
    DWORD old = 0;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    void* orig = vt[index];
    vt[index] = repl;
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
    return orig;
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
PFN_CreateDevice g_origCreateDevice = nullptr;
volatile LONG g_patched = 0;

HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* self, UINT ad, D3DDEVTYPE t, HWND w, DWORD f,
                                            D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    HRESULT hr = g_origCreateDevice(self, ad, t, w, f, pp, out);
    if (SUCCEEDED(hr) && out && *out && InterlockedExchange(&g_patched, 1) == 0) {
        g_origPresent = (PFN_Present)PatchVTable(*out, 17, (void*)&Hook_Present);
        Log("Present hooked. F9 = toggle override, F7 = cycle mode, F10 = dump state.");
    }
    return hr;
}

// ---------------------------------------------------------------- passthrough
HMODULE g_real = nullptr;
typedef IDirect3D9* (WINAPI *PFN_C9)(UINT);
typedef HRESULT (WINAPI *PFN_C9Ex)(UINT, IDirect3D9Ex**);
typedef int (WINAPI *PFN_icw)(D3DCOLOR, LPCWSTR); typedef int (WINAPI *PFN_iv)(void);
typedef DWORD (WINAPI *PFN_dv)(void); typedef void (WINAPI *PFN_vcw)(D3DCOLOR, LPCWSTR);
typedef BOOL (WINAPI *PFN_bv)(void); typedef void (WINAPI *PFN_vd)(DWORD); typedef void (WINAPI *PFN_vv)(void);
PFN_C9 p_C9=nullptr; PFN_C9Ex p_C9Ex=nullptr; PFN_icw p_Begin=nullptr; PFN_iv p_End=nullptr;
PFN_dv p_Status=nullptr; PFN_vcw p_Marker=nullptr,p_Region=nullptr; PFN_bv p_Repeat=nullptr;
PFN_vd p_Options=nullptr; PFN_vv p_Mute=nullptr;

bool LoadReal() {
    char sys[MAX_PATH] = {};
    if (!GetSystemDirectoryA(sys, MAX_PATH)) return false;
    strcat_s(sys, "\\d3d9.dll");
    g_real = LoadLibraryA(sys);
    if (!g_real) return false;
    p_C9=(PFN_C9)GetProcAddress(g_real,"Direct3DCreate9");
    p_C9Ex=(PFN_C9Ex)GetProcAddress(g_real,"Direct3DCreate9Ex");
    p_Begin=(PFN_icw)GetProcAddress(g_real,"D3DPERF_BeginEvent");
    p_End=(PFN_iv)GetProcAddress(g_real,"D3DPERF_EndEvent");
    p_Status=(PFN_dv)GetProcAddress(g_real,"D3DPERF_GetStatus");
    p_Marker=(PFN_vcw)GetProcAddress(g_real,"D3DPERF_SetMarker");
    p_Region=(PFN_vcw)GetProcAddress(g_real,"D3DPERF_SetRegion");
    p_Repeat=(PFN_bv)GetProcAddress(g_real,"D3DPERF_QueryRepeatFrame");
    p_Options=(PFN_vd)GetProcAddress(g_real,"D3DPERF_SetOptions");
    p_Mute=(PFN_vv)GetProcAddress(g_real,"DebugSetMute");
    return p_C9 != nullptr;
}

}  // namespace

extern "C" {
IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    if (!p_C9) return nullptr;
    IDirect3D9* d3d = p_C9(sdk);
    if (d3d) g_origCreateDevice = (PFN_CreateDevice)PatchVTable(d3d, 16, (void*)&Hook_CreateDevice);
    return d3d;
}
HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** o) { return p_C9Ex ? p_C9Ex(sdk,o) : E_NOTIMPL; }
int   WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n) { return p_Begin ? p_Begin(c,n) : 0; }
int   WINAPI D3DPERF_EndEvent(void)                    { return p_End ? p_End() : 0; }
DWORD WINAPI D3DPERF_GetStatus(void)                   { return p_Status ? p_Status() : 0; }
void  WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n)  { if (p_Marker) p_Marker(c,n); }
void  WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n)  { if (p_Region) p_Region(c,n); }
BOOL  WINAPI D3DPERF_QueryRepeatFrame(void)            { return p_Repeat ? p_Repeat() : FALSE; }
void  WINAPI D3DPERF_SetOptions(DWORD o)               { if (p_Options) p_Options(o); }
void  WINAPI DebugSetMute(void)                        { if (p_Mute) p_Mute(); }
}

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(m);
        LogInit();
        Log("=== camera_write attached - THIS SPIKE WRITES TO ENGINE STATE ===");
        Log("    override starts OFF. press F9 in game to enable.");
        if (!LoadReal()) { Log("FATAL: real d3d9.dll not loadable"); return FALSE; }
    }
    return TRUE;
}
