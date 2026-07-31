// view_matrix - Singularity VR mod, spike 9
//
// GOES AFTER THE ONE BLOCKING PROBLEM: camera POSITION, which gates both stereo (a per-eye
// offset IS a position offset) and 6-DOF.
//
// Three attempts to reach position through the engine's object model all failed (spike 8 and
// ENGINE_NOTES): writing every candidate field did nothing, a hardware write breakpoint found
// only generic actor-move plumbing shared by every actor in the game, and FUN_0104e420 turned
// out to be rotation. Unlike rotation, position has no single interceptable source here.
//
// So stop asking the engine and intercept the result instead. UE3 is shader-based: the
// world->clip matrix reaches the GPU as vertex shader constants, and moving the camera is
// just a change to that matrix. This is how vorpX and 3Dmigoto do stereo, it bypasses the
// object model entirely, and it solves stereo AND 6-DOF with one mechanism.
//
// TWO STEPS, BOTH IN THIS BUILD:
//
// 1. FIND the matrix (F7). Every SetVertexShaderConstantF call is scanned for a 4-register
//    window that behaves like a world->clip matrix. The test is geometric, not a guess: for
//    such a matrix, clip.w is the view-space depth of the point, so the CAMERA ITSELF must
//    land on the w = 0 plane. We know the camera's world position from the object model, so
//    substitute it and keep the windows where |w| ~ 0 while the camera is ~20,000 UU from the
//    origin. That is a very hard test to pass by accident.
//
// 2. MOVE it (F3). Offset the camera by pre-multiplying a world translation into the matrix
//    (row-vector convention: row3 -= o.x*row0 + o.y*row1 + o.z*row2). Exact for any offset,
//    and no knowledge of the projection is needed. If the view slides, position is solved.
//
// The injection re-runs the detection test on every call rather than trusting a register
// number, because the same register is reused by shadow and post passes with different
// matrices. Only the block the camera actually sits on gets modified.
//
// -------- inherited from spike 8 (working, unchanged) --------
// Head ROTATION via a detour on FUN_0104e390, the view-rotation source, plus a direct write
// of yaw to the live controller. The frame reaches the headset by
// backbuffer -> GetRenderTargetData -> SYSTEMMEM -> D3D11 -> XR swapchain.
//
// Controls:
//   F7  scan one frame for the view matrix, then report
//   F3  inject a +300 UU world-Y camera offset into it (the proof)
//   F2  constant-pitch test (+8000 UU, ignores the headset)
//   F9  head tracking on/off      F8  recentre      F5/F4  flip yaw / pitch sign
//
// Log: %LOCALAPPDATA%\SingularityVR\view_matrix.log

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <d3d9.h>
#include <d3d11.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "MinHook.h"

#include <cstdio>
#include <cstdint>
#include <cmath>

#pragma comment(lib, "d3d11.lib")

namespace {

// ---------------------------------------------------------------- logging
CRITICAL_SECTION g_lock;
char g_logPath[MAX_PATH] = {};
bool g_logReady = false;

// Declared up here only so LogInit can initialise it alongside g_lock - it belongs to the
// D3D9Ex / D3DPOOL_MANAGED shadow table far below, and guards that table's entries.
CRITICAL_SECTION g_stageLock;
bool g_stageLockReady = false;

// ---- ⚠️ truncating on attach destroys the run you are comparing against (run 31) ----
//
// Any measurement worth making here is an A/B across launches - two ini settings, two runs, two
// numbers. Truncating at attach means launch B erases launch A, so by the time you have the
// second number you no longer have the first. Run 31 lost two of its three tests to exactly
// that, and the loss is silent: the file looks complete, it is simply the wrong run.
//
// So the previous logs are rotated rather than overwritten. Three back is enough for the
// three-launch comparisons this project actually runs.
const int kLogKeep = 3;

void LogInit() {
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_stageLock);   // guards the MANAGED->DEFAULT shadow table
    g_stageLockReady = true;
    char base[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) {
        strcat_s(base, "\\SingularityVR");
        CreateDirectoryA(base, nullptr);
        sprintf_s(g_logPath, "%s\\view_matrix.log", base);
        // Oldest first, so nothing is overwritten before it has been moved along.
        char from[MAX_PATH], to[MAX_PATH];
        for (int i = kLogKeep; i >= 1; --i) {
            sprintf_s(to, "%s\\view_matrix.prev%d.log", base, i);
            if (i == 1) sprintf_s(from, "%s", g_logPath);
            else        sprintf_s(from, "%s\\view_matrix.prev%d.log", base, i - 1);
            DeleteFileA(to);
            MoveFileA(from, to);        // fails harmlessly when the run does not exist yet
        }
        FILE* f = nullptr;
        if (fopen_s(&f, g_logPath, "w") == 0 && f) fclose(f);
        g_logReady = true;
    }
}
// _Printf_format_string_ is what makes MSVC apply printf checking to a custom variadic function.
// Without it the compiler has no idea this is a format string, which is why a nine-conversion /
// ten-argument call sat in the perf line for four runs and presented as an unexplained startup
// hang. Verified: the annotation alone is not enough either - /W1 through /W4 stay silent, and it
// takes /analyze to read the annotation and report C6067. build.ps1 now runs that pass and fails
// the build on it.
void Log(_Printf_format_string_ const char* fmt, ...) {
    if (!g_logReady) return;
    EnterCriticalSection(&g_lock);
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") == 0 && f) {
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f); fclose(f);
    }
    LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------- the view rotation hook
struct FRotator { int32_t pitch, yaw, roll; };

// FUN_0104e390 is __thiscall. MSVC will not let a free function be __thiscall, so declare it
// __fastcall with an unused EDX slot - that produces the identical register/stack layout
// (this in ECX, everything else on the stack). Arg count MUST match: the callee cleans the
// stack, so one arg too few or too many corrupts it.
//
// From the prologue, five stack args are read: [EBP+8] .. [EBP+0x18], the last a float.
// The caller pushes a pointer to a local FRotator as the first one and uses the returned
// EAX as that rotator, so arg 1 is the output buffer.
typedef FRotator* (__fastcall* PFN_CalcViewRotation)(
    void* self, void* edx_unused,
    FRotator* outRot, uint32_t a2, uint32_t a3, uint32_t a4, float a5);

const uintptr_t kCalcViewRotationAddr = 0x0104e390;   // no ASLR: file address == runtime
PFN_CalcViewRotation g_origCalcViewRotation = nullptr;

struct FVec3 { float x, y, z; };

volatile LONG g_enabled = 0;      // head tracking - cold start is flat; BACKSPACE = all three at once
volatile LONG g_constTest = 0;    // constant-pitch proof
const int32_t kConstPitch = 8000; // ~44 degrees up - unmissable

int32_t g_wantPitch = 0, g_wantYaw = 0;
// Our own last yaw write, so anything else that moved yaw since can be told apart from it and
// folded in rather than overwritten. See the note at the write site.
int32_t g_lastWrittenYaw = 0;
bool    g_haveLastWrittenYaw = false;
bool    g_haveWant = false;
int     g_hookHits = 0;

float g_vrFovDeg = 0.0f;             // what ApplyVrFov decided to ask the engine for
uintptr_t g_lastCam = 0;             // camera pointer, cached by ApplyVrFov

FRotator* __fastcall Hook_CalcViewRotation(void* self, void* edx,
        FRotator* outRot, uint32_t a2, uint32_t a3, uint32_t a4, float a5) {
    FRotator* r = g_origCalcViewRotation(self, edx, outRot, a2, a3, a4, a5);
    if (!r) return r;

    if (++g_hookHits <= 3)
        Log("hook hit #%d: engine returned pitch=%d yaw=%d roll=%d", g_hookHits, r->pitch, r->yaw, r->roll);

    if (InterlockedCompareExchange(&g_constTest, 0, 0)) {
        r->pitch = kConstPitch;
    } else if (InterlockedCompareExchange(&g_enabled, 0, 0) && g_haveWant) {
        r->pitch = g_wantPitch;
        r->yaw   = g_wantYaw;
    }

    // NOT re-asserting the FOV here. Tried in run 20 on the theory that this seam - the camera
    // update - sits upstream of culling, and it failed on its own measurements: the value was
    // already correct at this point in all but ~0.06 corrections per frame, the engine still hit
    // its 65 deg default just as often, and the 10th-percentile FOV got WORSE (114.6 -> 65.0).
    // Writing FOV here also runs during MENUS, which is a plausible part of the menu corruption.
    //
    // So the engine resets FOV somewhere downstream of this function, and finding that seam needs
    // a write breakpoint on the FOV field rather than another guess at which hook looks upstream.
    return r;
}

// ---------------------------------------------------------------- engine objects (yaw path)
// The detour owns PITCH: the call site stores only [EAX] (pitch) into AActor::Rotation, while
// yaw goes to a local and is handled further along. Direct writes to controller+0x64 were
// already proven to steer yaw, so drive each axis where it actually works.
const uintptr_t kGNamesTArray   = 0x01CD4A1C;
const uintptr_t kGObjectsTArray = 0x01CD4A4C;
const int OBJ_OUTER = 0x28, OBJ_NAME = 0x2C, OBJ_CLASS = 0x34;
const int ACTOR_ROTATION = 0x0060;    // pitch, yaw, roll
const int CTL_ROTATION_2 = 0x05D4;

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
    if (!nd || idx < 0 || idx >= nc || !Readable((void*)(nd + idx*4), 4)) return false;
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

uintptr_t g_ctlClass = 0, g_controller = 0;
uintptr_t g_camClass = 0, g_camera = 0;

// Read-only now: these are what the matrix detector substitutes into the candidate matrices.
// Writing them was proven useless in spike 8 - the render does not read them back.
const int ACTOR_LOCATION   = 0x0054;   // FVector, 3 floats
const int CAM_POV_LOC      = 0x0420;   // mCurrentPOV location
const int CAM_POV_FOV      = 0x0438;   // mCurrentPOV FOV - proven to steer the projection
const int CAM_DESIRED_FOV  = 0x0470;   // mDesiredPOV FOV, the interpolation target
const int CAM_CURRENT_FOV  = 0x0490;   // mCurrentFOV

// ---- ⚠️ a FAILED scan is enormously expensive, and it repeats every frame (run 35) ----
//
// Both finders cache their object and return instantly while it stays valid. That hides how bad
// the miss path is: it walks every entry in GObjects - tens of thousands in UE3 - calling
// Readable() two or three times per object, and Readable() is VirtualQuery, a kernel transition.
// Hundreds of thousands of syscalls, per frame.
//
// It only costs anything when the object is ABSENT, because then the cache can never warm and the
// full walk runs again every single frame. Absent is exactly: main menu, level load, and quitting.
// Which is precisely where the game was reported freezing for seconds at a time, with "our work"
// measured at 69-82 ms a frame against a normal 6.
//
// So a failed scan arms a backoff. Acquisition is delayed by at most this long once the object
// does appear, which is imperceptible, and the menu cost drops by ~15x.
const ULONGLONG kFailedScanBackoffMs = 250;

inline bool ScanAllowed(ULONGLONG& gate) {
    const ULONGLONG now = GetTickCount64();
    if (now < gate) return false;
    // Armed up front rather than on the failure path, so every early `return 0` below is covered
    // without having to find them all. The success path never reaches here again anyway - the
    // cache short-circuits above it.
    gate = now + kFailedScanBackoffMs;
    return true;
}

uintptr_t FindController() {
    if (g_controller && g_ctlClass && Readable((void*)g_controller, 0x600) &&
        *reinterpret_cast<uintptr_t*>(g_controller + OBJ_CLASS) == g_ctlClass) return g_controller;
    g_controller = 0;
    static ULONGLONG s_gate = 0;
    if (!ScanAllowed(s_gate)) return 0;
    if (!Readable((void*)kGObjectsTArray, 12)) return 0;
    uintptr_t data = *reinterpret_cast<uintptr_t*>(kGObjectsTArray);
    int32_t count = *reinterpret_cast<int32_t*>(kGObjectsTArray + 4);
    if (!data || count < 100) return 0;
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
        if (!g_ctlClass) return 0;
    }
    for (int i = 0; i < count; ++i) {
        if (!Readable((void*)(data + i*4), 4)) continue;
        uintptr_t o = objs[i];
        if (!o || !Readable((void*)o, 0x600)) continue;
        if (*reinterpret_cast<uintptr_t*>(o + OBJ_CLASS) != g_ctlClass) continue;
        if (!NameOf(o, nm, sizeof(nm)) || strncmp(nm, "Default__", 9) == 0) continue;
        g_controller = o;
        return o;
    }
    return 0;
}

uintptr_t FindCamera() {
    if (g_camera && g_camClass && Readable((void*)g_camera, 0x700) &&
        *reinterpret_cast<uintptr_t*>(g_camera + OBJ_CLASS) == g_camClass) return g_camera;
    g_camera = 0;
    static ULONGLONG s_gate = 0;
    if (!ScanAllowed(s_gate)) return 0;
    if (!Readable((void*)kGObjectsTArray, 12)) return 0;
    uintptr_t data = *reinterpret_cast<uintptr_t*>(kGObjectsTArray);
    int32_t count = *reinterpret_cast<int32_t*>(kGObjectsTArray + 4);
    if (!data || count < 100) return 0;
    auto* objs = reinterpret_cast<uintptr_t*>(data);
    char nm[128], on[128];
    if (!g_camClass) {
        for (int i = 0; i < count; ++i) {
            if (!Readable((void*)(data + i*4), 4)) continue;
            uintptr_t o = objs[i];
            if (!o || !Readable((void*)o, 0x40)) continue;
            if (!NameOf(o, nm, sizeof(nm)) || _stricmp(nm, "RvPlayerCamera") != 0) continue;
            uintptr_t ou = *reinterpret_cast<uintptr_t*>(o + OBJ_OUTER);
            if (ou && Readable((void*)ou, 0x40) && NameOf(ou, on, sizeof(on)) && !_stricmp(on, "RvGame")) {
                g_camClass = o; break;
            }
        }
        if (!g_camClass) return 0;
    }
    for (int i = 0; i < count; ++i) {
        if (!Readable((void*)(data + i*4), 4)) continue;
        uintptr_t o = objs[i];
        if (!o || !Readable((void*)o, 0x700)) continue;
        if (*reinterpret_cast<uintptr_t*>(o + OBJ_CLASS) != g_camClass) continue;
        if (!NameOf(o, nm, sizeof(nm)) || strncmp(nm, "Default__", 9) == 0) continue;
        g_camera = o;
        return o;
    }
    return 0;
}

// ================================================================ the view matrix probe
//
// A world->clip matrix has one property we can test without knowing anything about the
// engine's conventions: clip.w is the view-space depth of the transformed point, so the
// camera itself transforms to w = 0. Substitute the camera's known world position into every
// candidate 4-register window and keep the ones that come out at zero. With the camera
// ~20,000 UU from the world origin, landing within a few units of zero by chance is
// vanishingly unlikely - the near-cancellation IS the identification.
//
// Two storage conventions are both common in D3D9, so test both:
//   ROW  registers are the rows of a row-vector matrix M (clip = v * M)
//        -> clip.w = v.x*r0.w + v.y*r1.w + v.z*r2.w + r3.w
//   COL  registers are the columns of M (the matrix was transposed before upload, which is
//        what D3DXMatrixTranspose + mul(M, v) produces)
//        -> clip.w = dot(r3.xyz, v) + r3.w
//
// The xyz part of that w term is the camera's forward axis, so it should be close to unit
// length. Reporting its length separately is a useful sanity check: a window that passes the
// zero test with a degenerate (near-zero) forward vector is passing trivially, not really a
// view matrix.

struct Reg4 { float x, y, z, w; };

enum MatConv { CONV_ROW = 0, CONV_COL = 1 };

// Which point was substituted to get w ~ 0. The last one matters: UE3 of this vintage often
// uploads a TRANSLATED-WORLD to clip matrix, pre-subtracting the view origin on the CPU to
// protect float precision at large world coordinates. In that space the camera sits at the
// origin, so the world-position test would be blind to it. Probing (0,0,0) as well costs
// nothing and covers that case in the same scan.
const char* const kPointName[3] = { "camActor", "mCurrentPOV", "origin(translated-world)" };

struct MatHit {
    UINT  startReg;      // absolute register the 4-vector block begins at
    int   conv;          // MatConv
    int   pt;            // index into kPointName
    float w;             // clip.w for that point - the closer to 0 the better
    float fwdLen;        // length of the w-term xyz, ~1.0 for a real view matrix
    float dotFwd;        // agreement with the camera's real facing - the decisive test
    int   count;         // how many times this window matched during the armed frame
    bool  shape;         // passed the tangent + orthogonality tests (see ViewMatrixShape)
};

const int kMaxHits = 24;
MatHit g_hits[kMaxHits];
int    g_hitCount = 0;

volatile LONG g_scanArmed  = 0;   // F7: scan this frame
volatile LONG g_injectOn   = 0;   // F3: offset the camera through the matrix
volatile LONG g_scanReport = 0;   // set by the scan, consumed by Present

// Camera world position, refreshed once per frame in Present so the hot path never walks
// GObjects. Two sources: the camera actor's Location and mCurrentPOV's location. They should
// agree; testing both means a wrong guess about which one the renderer uses cannot mask a hit.
float g_camPos[3] = {0,0,0};
float g_povPos[3] = {0,0,0};
volatile LONG g_camPosValid = 0;

// The camera's own axes, from its FRotator. These are what make the origin case decidable -
// see the direction test below.
float g_camFwd[3]   = {1,0,0};
float g_camRight[3] = {0,1,0};

// Injection magnitude, cycled at runtime with F11. 300 UU is unmissable and proves the
// mechanism; a real interpupillary distance is only a few UU, which is what stereo will
// actually use, so being able to dial down and confirm the artifacts shrink matters.
const float kOffsetSteps[] = { 300.0f, 100.0f, 30.0f, 10.0f, 3.0f };
const int   kOffsetStepCount = 5;
int   g_offsetStep = 0;
float g_offsetUU = 300.0f;

// ---- 6-DOF: the headset's real position, fed in as the offset ----
// UE3 scale is 16 units per foot, so 1 UU is 1.905 cm and a metre is 52.5 UU. If head movement
// comes out feeling too large or too small in game, this is the number to adjust.
const float kMetresToUU = 52.5f;
volatile LONG g_sixDof = 0;        // F10 - cold start is flat; BACKSPACE = all three at once
float g_hmdOffset[3] = {0,0,0};    // game-world offset from the recentre point, in UU
float g_centrePos[3] = {0,0,0};    // headset position captured at recentre, in XR metres
bool  g_haveCentrePos = false;

// ---- VR-correct projection (F6) ----
//
// Until now the projection layer claimed the game's image spanned the HEADSET's field of view.
// That is a lie whenever the game renders a different one, and it is why nothing in the
// headset could be measured against anything: everything was uniformly stretched, so head
// movement, world scale and object size were all wrong together and none of them could be
// judged against the others.
//
// The fix has two halves that have to travel together:
//   1. Tell OpenXR the truth - derive the frustum the game ACTUALLY rendered and submit that.
//   2. Ask the game for a field of view that covers the headset, so the truth is also a
//      full-coverage image rather than a small correct rectangle floating in black.
//
// The truth is read from the matrix rather than assumed, in the same spirit as everything
// else here: the x and y columns of a world->clip matrix are the projection scales times unit
// world axes, so their lengths ARE the projection scales, and tan(halfFov) is the reciprocal.
// That works without knowing whether UE3's FOVAngle is horizontal or vertical, or how it
// folds in aspect ratio - we never ask, we just read what came out.
volatile LONG g_vrProjection = 1;      // on by default: it is the correct behaviour
UINT  g_lockedReg  = 0;
int   g_lockedConv = CONV_ROW;
bool  g_haveLock   = false;
// ---- the lock has to be revocable (run 15) ----
//
// Locking onto a register was added as a pure optimisation, and it quietly became the single
// biggest fragility in the design: once set, it was never re-examined. Run 15 showed what that
// costs. At 4K the lock ended up on something that is not the scene matrix - the captured
// projection read 180x180 deg (degenerate columns) and 90x90 deg (a cube-map face) rather than
// the ~128x98 a real one gives - and because the modify path only ever tests the locked window,
// recognition never recovered. Result: 0 of ~2900 draws got parallax, every draw was flat, and
// the forced projection silently kept reporting a stale success from before the lock went bad.
//
// So: verify the lock every frame and drop it when it stops holding, which makes the next frame
// re-scan from scratch. A wrong lock now costs a second, not a session.
bool  g_lockVerified   = false;
int   g_lockMissFrames = 0;
float g_projTanX = 0.0f, g_projTanY = 0.0f;   // tan of half FOV, as OBSERVED (diagnostic)
float g_targetTanX = 0.0f, g_targetTanY = 0.0f;   // what we FORCE into the matrix, and submit
float g_forcedTanX = 0.0f, g_forcedTanY = 0.0f;   // read back AFTER the edit, to prove it landed

// Ask the engine for more FOV than we actually render with. Its value now only governs CPU
// culling, and culling wider than we draw costs a little geometry and guarantees no bare edges
// even on the frames where its interpolation dips well below what we asked for.
//
// ---- why 1.15 was far too little (run 9) ----
//
// The engine's FOV is HORIZONTAL at the game's 16:9 aspect, so its culling frustum is wide and
// short. Ours is the headset's: narrow and TALL. Coverage on the vertical axis is therefore the
// binding constraint, and 1.15 did not come close to meeting it.
//
// Measured, asking 133.9 deg: the engine actually held ~124 deg, dipping to 105.8. Vertical
// culling FOV = 2*atan(tan(H/2)/1.778), so:
//
//     124.0 deg horizontal -> 93.2 deg vertical    (we render 98.0 - SHORT by ~5 deg)
//     105.8 deg horizontal -> 73.2 deg vertical    (SHORT by ~25 deg)
//
// Anything in that shortfall band is culled before a draw call is ever issued, so our hook
// never sees it and duplication cannot help: the object is missing from BOTH eyes. That is
// exactly the reported behaviour of ammo and the gun vanishing when the head tilts down (ground
// objects entering the bottom shortfall), and of the manhole decal - which was also flickering
// BEFORE duplication existed, because this shortfall predates it.
//
// The headroom multiplies the tangent, so it must be large enough that even a 25-30 deg dip
// still leaves the vertical covered. 2.5 asks ~160 deg, which holds >=130 deg through the
// observed dips - comfortably past the ~128 deg needed for 98 deg vertical.
// Runtime-selectable, because the headroom is a genuine trade rather than a value to get right
// once. A wide engine FOV buys culling coverage, but the engine also derives texture-streaming
// mip selection and mesh LOD from projected screen size - so telling it the field is 158 deg
// makes everything read as far away, which is a plausible cause of the texture flicker still
// being reported. PAGE UP cycles this so the trade can be measured instead of argued about.
// ---- run 28: the headroom was buying nothing and costing everything ----
//
// 2.5 was chosen in run 9 to cover a VERTICAL CULLING SHORTFALL. Run 28 measured whether that
// shortfall is actually occurring, from the engine's own arriving matrix rather than from the
// mCurrentPOV proxy, and it is not:
//
//     engine's own matrix on arrival: 147.4 x 125.1 deg   (we render 98.0 vertical)
//                                     150.8 x 130.2 deg
//                                     153.8 x 135.0 deg
//
// The engine culls 112-135 deg vertical against our 98. Covered, every frame, with margin. The
// 51.1 deg reading that suggested otherwise came from a pre-gameplay frame.
//
// Meanwhile the cost is severe and measured: asking 157.9 deg is x2.43 native, which makes
// everything project ~8x smaller than the engine was tuned for. UE3 picks draw distance and mesh
// LOD from projected screen size, so objects cull about 8x closer than vanilla - which is
// precisely the control run 21 ran: in the unmodified game those objects are visible from much
// further away.
//
// So the ladder now runs DOWNWARD from 1.0. The interesting direction is toward native, and these
// steps span the inflation range end to end at 100% render:
//
//     headroom  ask     inflation  objects project
//        2.50   157.9     x2.43      8.0x smaller
//        1.00   127.8     x1.97      3.2x smaller
//        0.70   110.0     x1.69      2.2x smaller
//        0.50    91.3     x1.40      1.6x smaller
//        0.35    71.1     x1.09      1.0x  <- native LOD, no distortion at all
//
// Walk it down until either the artifacts stop (the mechanism is confirmed and the operating
// point is found) or the ground-truth culling line starts reporting SHORTFALL and the edges of
// the view begin dropping out. Those are the two ends of a real trade, and for the first time
// both ends are instrumented.
const float kFovHeadroomSteps[] = { 1.0f, 0.70f, 0.50f, 0.35f, 1.30f, 1.75f, 2.5f };
const int   kFovHeadroomCount = 7;
int   g_fovHeadroomStep = 0;
float kFovHeadroom = 1.0f;

// How much of the headset's field of view we actually render, as a fraction of its half-angle.
//
// This is the other half of the same trade as the headroom above, and the more useful end of it.
// The engine has to cull at least as wide as we draw, and because its FOV is horizontal at 16:9
// while ours is tall and narrow, covering our 98 deg vertical forces its reported FOV up to
// ~128 deg before any safety margin - which is what drags streaming and LOD off. Rendering a
// little less vertical FOV lowers that requirement much faster than it costs immersion:
//
//     100% -> 98.0 deg vertical, needs the engine at ~128 deg
//      85% -> 83.3 deg vertical, needs the engine at ~112 deg
//      70% -> 68.6 deg vertical, needs the engine at ~96 deg
//
// The submitted OpenXR frustum is derived from the same numbers, so narrowing this stays
// geometrically honest - the image simply subtends less of the eye, with black beyond it, rather
// than being wrong.
// The 0.40 step exists purely as a DECISIVE TEST, not as a playable setting.
//
// The culling theory says the flicker happens because our rendered frustum is taller than the
// engine's culling frustum, so geometry is culled before a draw call exists. The engine's worst
// observed case is 65 deg horizontal at 16:9, which is only ~39.4 deg VERTICAL. At 0.40 we render
// 49 * 0.40 * 2 = 39.2 deg vertical - just inside that worst case, so no shortfall can occur on any
// frame.
//
// Which makes it falsifiable: if the flicker vanishes completely at 0.40, culling is the cause. If
// it persists, culling is innocent and the last three theories were all wrong. It will look like a
// narrow letterbox and be unplayable - that is fine, it is a measurement.
const float kRenderFovSteps[] = { 1.0f, 0.85f, 0.70f, 0.55f, 0.40f };
const int   kRenderFovCount = 5;
int   g_renderFovStep = 0;
float g_renderFovScale = 1.0f;

// Per-frame diagnostics, reset each Present. The spread catches a case the single captured
// value cannot: c0 is written ~50 times a frame by different passes, and if two of them carry
// genuinely different projections then "the" FOV depends on which one happened to write last -
// a second, independent way to produce exactly the flicker seen in run 4.
float g_capTanMin = 1e9f, g_capTanMax = -1e9f;
// The VERTICAL tangent of the engine's own arriving matrix, tracked across a whole report window
// rather than a frame. This is the number that governs culling, and until now the shortfall
// warning was computed from mCurrentPOV instead - a proxy that the log shows disagreeing with it
// badly (mCurrentPOV read 157.9 while the matrix the engine actually built carried 80.7 x 51.1).
float g_capTanYMin = 1e9f;
int   g_injCount = 0;

// The projection scales, from the x and y columns. See ClipW for how the conventions differ.
inline void ProjTangents(const Reg4* r, int conv, float* tanX, float* tanY) {
    float xc[3], yc[3];
    if (conv == CONV_ROW) {
        xc[0] = r[0].x; xc[1] = r[1].x; xc[2] = r[2].x;
        yc[0] = r[0].y; yc[1] = r[1].y; yc[2] = r[2].y;
    } else {
        xc[0] = r[0].x; xc[1] = r[0].y; xc[2] = r[0].z;
        yc[0] = r[1].x; yc[1] = r[1].y; yc[2] = r[1].z;
    }
    float lx = sqrtf(xc[0]*xc[0] + xc[1]*xc[1] + xc[2]*xc[2]);
    float ly = sqrtf(yc[0]*yc[0] + yc[1]*yc[1] + yc[2]*yc[2]);
    *tanX = (lx > 1e-6f) ? 1.0f / lx : 0.0f;
    *tanY = (ly > 1e-6f) ? 1.0f / ly : 0.0f;
}

// ---- structural tests, independent of where the camera is pointing (run 27) ----
//
// The facing test (dotFwd) was the only real discriminator on the hot path, and it is the one
// quantity here that goes STALE: g_camFwd is read at Present from the camera ACTOR, a frame away
// from the constants being tested and from a different object than the one driving the render. At
// 37 fps a brisk turn covers more than the ~8 degrees of slack kMinDotFwd allows.
//
// And a rejection is not cheap. c0 carries one matrix per frame, so losing it loses the forced
// projection AND the per-eye split for the WHOLE frame - a full-frame flash keyed to head
// movement, which is exactly the symptom that has survived six theories. Worth noting that the
// run-23 elimination test was performed with head tracking OFF, which would hide this completely.
//
// Two tests buy back what the tight facing gate was providing, with nothing stale in them:
//
//   * the projection tangents must be sane. Already used to guard the diagnostic; now a gate.
//   * the basis must be orthogonal. For M = V*P with a row-vector V and a diagonal-ish P, column 0
//     of M is sx*right, column 1 is sy*up and the w column's xyz is forward - the camera's own
//     axes, scaled. Normalised they must be mutually perpendicular. Skinning palettes, UI
//     transforms and the float3x4 blocks the sliding window straddles do not satisfy that.
//
// With those in place the facing test can be loosened to its actual job - rejecting a matrix that
// is shaped like a view but points somewhere else entirely (a shadow cascade from a light, a
// cube-map face, a mirror) - which needs nothing like 8 degrees of precision.
inline void AxisCols(const Reg4* r, int conv, float* xc, float* yc, float* fc) {
    if (conv == CONV_ROW) {
        xc[0] = r[0].x; xc[1] = r[1].x; xc[2] = r[2].x;
        yc[0] = r[0].y; yc[1] = r[1].y; yc[2] = r[2].y;
        fc[0] = r[0].w; fc[1] = r[1].w; fc[2] = r[2].w;
    } else {
        xc[0] = r[0].x; xc[1] = r[0].y; xc[2] = r[0].z;
        yc[0] = r[1].x; yc[1] = r[1].y; yc[2] = r[1].z;
        fc[0] = r[3].x; fc[1] = r[3].y; fc[2] = r[3].z;
    }
}

inline bool NormaliseVec3(float* v) {
    float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (!(l > 1e-6f)) return false;          // also rejects NaN
    v[0] /= l; v[1] /= l; v[2] /= l;
    return true;
}

const float kMaxAxisDot = 0.10f;             // ~6 degrees off perpendicular

inline bool IsOrthogonalBasis(const Reg4* r, int conv) {
    float xc[3], yc[3], fc[3];
    AxisCols(r, conv, xc, yc, fc);
    if (!NormaliseVec3(xc) || !NormaliseVec3(yc) || !NormaliseVec3(fc)) return false;
    const float xy = xc[0]*yc[0] + xc[1]*yc[1] + xc[2]*yc[2];
    const float xf = xc[0]*fc[0] + xc[1]*fc[1] + xc[2]*fc[2];
    const float yf = yc[0]*fc[0] + yc[1]*fc[1] + yc[2]*fc[2];
    return fabsf(xy) < kMaxAxisDot && fabsf(xf) < kMaxAxisDot && fabsf(yf) < kMaxAxisDot;
}

// Sane projection scales. Run 15 captured 180x180 deg (x column length ~0) and 90x90 deg (unit
// columns - a cube-map face), neither of which is a scene world->clip matrix.
const float kMinProjTan = 0.15f, kMaxProjTan = 8.0f;

inline bool ViewMatrixShape(const Reg4* r, int conv, float* tanX, float* tanY) {
    ProjTangents(r, conv, tanX, tanY);
    if (!(*tanX > kMinProjTan && *tanX < kMaxProjTan)) return false;
    if (!(*tanY > kMinProjTan && *tanY < kMaxProjTan)) return false;
    return IsOrthogonalBasis(r, conv);
}

// clip.w for point p under the given convention.
inline float ClipW(const Reg4* r, int conv, const float* p) {
    if (conv == CONV_ROW) return p[0]*r[0].w + p[1]*r[1].w + p[2]*r[2].w + r[3].w;
    else                  return p[0]*r[3].x + p[1]*r[3].y + p[2]*r[3].z + r[3].w;
}
// The xyz of the w term. For a world->clip matrix clip.w is view depth, so this is the
// camera's forward axis expressed in world axes.
inline void FwdVec(const Reg4* r, int conv, float* out) {
    if (conv == CONV_ROW) { out[0] = r[0].w; out[1] = r[1].w; out[2] = r[2].w; }
    else                  { out[0] = r[3].x; out[1] = r[3].y; out[2] = r[3].z; }
}
inline float FwdLen(const Reg4* r, int conv) {
    float f[3]; FwdVec(r, conv, f);
    return sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
}

// Does this window transform the camera to w ~ 0?
//
// The tolerance is absolute in world units and deliberately loose. The camera position we
// substitute is read at Present, so it is up to one frame older than the constants being
// scanned; a player moving at speed covers ~10 UU in that frame. 25 UU absorbs that and is
// still three orders of magnitude below the ~20,000 UU the camera sits from the origin, so a
// chance match remains vanishingly unlikely. Standing still while scanning tightens it
// further, and the reported w shows how well it really cancelled.
//
// The forward-length window rejects degenerate blocks whose w term is all zeros - those pass
// the zero test for every point and mean nothing.
//
// ---- why the zero test alone is not enough, learned from the first run ----
//
// The first scan matched 18 windows, all of them at the ORIGIN probe point (so this engine
// does use translated-world space - confirmed). But in that case the test degenerates badly:
// at p = (0,0,0) the clip.w expression collapses to r[3].w under BOTH conventions, so
//
//   * ROW and COL become indistinguishable - every origin hit is reported twice, and
//   * any matrix at all with a near-zero w constant qualifies, which is most of them.
//
// That is why w values of exactly +1.0000 showed up alongside the real one. The zero test
// only carries real information when the probe point is far from the origin.
//
// The fix is an independent second test: for a genuine world->clip matrix the xyz of the w
// term IS the camera's forward axis, and we know where the camera is actually facing from its
// FRotator. Requiring those to agree is decisive - it rejects unrelated matrices AND picks the
// correct convention, because ROW and COL read the forward vector from different components.
// ---- tolerance depends on WHICH probe point matched ----
//
// The loose tolerance exists only to absorb the one-frame staleness of the camera position we
// substitute. That staleness applies to the two world-position probes; it does NOT apply to
// the origin probe, where the test reduces to "is r[3].w near zero" - pure matrix content,
// with no camera reading involved and therefore nothing to go stale.
//
// Run 2 showed why this matters. Under a flat 25 UU tolerance, three extra windows passed
// (c8/c11/c14 COL, w around -10.5, 4 uploads each) alongside the real matrix at w = -0.0020
// with 50 uploads. Their 3-register spacing suggests a block of float3x4 transforms that the
// sliding 4-register window straddles. Injecting into those would corrupt something unrelated
// for no benefit. A tight origin tolerance separates -0.002 from -10.5 decisively.
const float kHitTolUU       = 25.0f;    // world-position probes: staleness-dominated
const float kHitTolOriginUU = 1.0f;     // origin probe: no staleness, so demand a real zero
const float kNearTolUU      = 5000.0f;
const float kMinDotFwd      = 0.99f;    // ~8 degrees of slack - for LOCKING only, see below
// ---- and why the per-call test uses a much looser one ----
//
// kMinDotFwd is right for the SCAN: it runs on a frame the player chose, it only has to pick a
// register once, and being strict there is what stops a weapon or reflection pass being locked.
//
// It is wrong for the per-call test that runs on every upload thereafter. There, g_camFwd is a
// frame stale (see the note by AxisCols), a single rejection costs the whole frame its projection
// and its split, and the shape tests now carry the discrimination. All this gate still has to do
// is reject a matrix pointing somewhere else entirely, which 37 degrees does comfortably.
const float kTrackDotFwd    = 0.80f;    // ~37 degrees - immune to a frame of rotation lag

inline float TolFor(int pt) { return (pt == 2) ? kHitTolOriginUU : kHitTolUU; }

// A failed scan should still yield evidence, so the closest non-qualifying window is kept.
// Without this, "nothing matched" cannot be told apart from "matched at 30 UU".
float g_nearBestW   = 1e30f;
float g_nearBestLen = 0.0f;
UINT  g_nearBestReg = 0;
int   g_nearBestConv = 0;
bool  g_haveNear = false;

const float g_zeroPos[3] = { 0.0f, 0.0f, 0.0f };

inline void NoteNearMiss(UINT reg, int conv, float w, float len) {
    if (fabsf(w) < fabsf(g_nearBestW)) {
        g_nearBestW = w; g_nearBestLen = len;
        g_nearBestReg = reg; g_nearBestConv = conv; g_haveNear = true;
    }
}

void RecordHit(UINT reg, int conv, int pt, float w, float len, float dot, bool shape) {
    for (int i = 0; i < g_hitCount; ++i) {
        if (g_hits[i].startReg == reg && g_hits[i].conv == conv) {
            g_hits[i].count++;
            if (fabsf(w) < fabsf(g_hits[i].w)) {
                g_hits[i].w = w; g_hits[i].fwdLen = len; g_hits[i].pt = pt; g_hits[i].dotFwd = dot;
                g_hits[i].shape = shape;
            }
            return;
        }
    }
    if (g_hitCount < kMaxHits) g_hits[g_hitCount++] = { reg, conv, pt, w, len, dot, 1, shape };
}

// Test one window against all three probe points and return the best-cancelling result,
// together with how well its forward axis agrees with where the camera is really facing.
// Returns false if the window has no plausible forward vector at all.
inline bool TestWindow(const Reg4* r, int conv, const float* const pts[3],
                       float* outW, float* outLen, int* outPt, float* outDot, bool* outShape) {
    float f[3]; FwdVec(r, conv, f);
    float len = sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    if (!(len > 0.3f && len < 3.0f)) return false;
    bool any = false;
    for (int i = 0; i < 3; ++i) {
        float w = ClipW(r, conv, pts[i]);
        if (!(w > -kNearTolUU && w < kNearTolUU)) continue;      // also rejects NaN
        if (!any || fabsf(w) < fabsf(*outW)) { *outW = w; *outPt = i; any = true; }
    }
    if (!any) return false;
    *outLen = len;
    *outDot = (f[0]*g_camFwd[0] + f[1]*g_camFwd[1] + f[2]*g_camFwd[2]) / len;
    float tx = 0, ty = 0;
    *outShape = ViewMatrixShape(r, conv, &tx, &ty);
    return true;
}

// A window is a candidate for LOCKING only if it passes all three tests. The shape test is what
// would have rejected the 180x180 and 90x90 windows that run 15 locked onto at 4K, and the
// fwdLen=1.4142 window sitting in the scan report with more uploads than the real matrix.
inline bool IsViewMatrix(float w, float dot, int pt, bool shape) {
    return fabsf(w) <= TolFor(pt) && dot >= kMinDotFwd && shape;
}

// The per-call test, shared by the verify path and the modify path so those two can never
// disagree about what "this window is still the scene view-projection" means. They used to apply
// visibly different tests to the same question.
//
// Note there is no probe point here. The engine is confirmed translated-world, so the camera sits
// at the origin and clip.w for it collapses to r[3].w under both conventions - pure matrix
// content, with no camera reading in it and therefore nothing to go stale. That is the whole
// reason this path no longer needs a valid camera POSITION at all.
inline bool TrackedViewMatrix(const Reg4* r, int conv, float* tanX, float* tanY, float* outDot) {
    float f[3]; FwdVec(r, conv, f);
    const float len = sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    *outDot = -2.0f;
    if (!(len > 0.3f && len < 3.0f)) return false;
    if (!(fabsf(r[3].w) <= kHitTolOriginUU)) return false;
    if (!ViewMatrixShape(r, conv, tanX, tanY)) return false;
    *outDot = (f[0]*g_camFwd[0] + f[1]*g_camFwd[1] + f[2]*g_camFwd[2]) / len;
    return *outDot >= kTrackDotFwd;
}

// Pre-multiply a world-space translation into the matrix: M' = T(-o) * M, which is exactly
// "move the camera by o" and is correct for any offset, including forward, with no knowledge
// of the projection needed.
//   ROW: row3 -= o.x*row0 + o.y*row1 + o.z*row2
//   COL: the same identity written against the transpose - each register's .w component is
//        the row3 entry for that column.
// Force the frustum to an exact target by rescaling the matrix's x and y columns.
//
// Run 4 proved the engine will accept a wide FOV but will not HOLD it: the value the matrix
// reported wobbled 123.8-128.7 degrees frame to frame and once fell to 80.7, against a steady
// 127.9 requested. That is the engine's own camera update interpolating back toward its
// default between our Present-time writes, with the size of each step set by frame duration.
// Asking more politely cannot fix it - the engine gets the last word before rendering.
//
// So stop relying on the request and set the projection where nothing can argue: in the
// matrix, on its way to the GPU, every single upload. The engine's FOV then matters only for
// CULLING, where being roughly right (and deliberately too wide) is entirely good enough.
//
// Scaling the x/y columns is exactly a clip-space x/y scale, which is exactly an FOV change,
// and it composes correctly after the positional offset.
void ApplyProjection(Reg4* r, int conv, float curTanX, float curTanY,
                     float wantTanX, float wantTanY) {
    if (!(curTanX > 0 && curTanY > 0 && wantTanX > 0 && wantTanY > 0)) return;
    const float sx = curTanX / wantTanX;
    const float sy = curTanY / wantTanY;
    if (conv == CONV_ROW) {
        for (int i = 0; i < 4; ++i) { r[i].x *= sx; r[i].y *= sy; }
    } else {
        r[0].x *= sx; r[0].y *= sx; r[0].z *= sx; r[0].w *= sx;
        r[1].x *= sy; r[1].y *= sy; r[1].z *= sy; r[1].w *= sy;
    }
}

// Squeeze the image into one half of the frame in CLIP SPACE, leaving the viewport alone.
//
// This replaces viewport-halving, and the reason is run 11. Halving the viewport breaks every
// UE3 shader that derives texture coordinates from screen position - deferred decals (a manhole
// IS a decal), projected ground effects, SSAO. Those shaders compute
//
//     UV = (clipPos.xy / clipPos.w) * ScreenPositionScaleBias.xy + ScreenPositionScaleBias.wz
//
// where that constant was computed for the FULL render target: NDC [-1,1] -> UV [0,1]. With a
// half viewport the geometry lands in half the pixels while the shader still samples the whole
// texture. The mismatch differs per half, which is exactly why a ground texture showed up in one
// eye and not the other.
//
// Remapping in clip space instead keeps the two mappings consistent. Scale x by 0.5 and shift by
// s (-0.5 left, +0.5 right) and the left eye occupies NDC x in [-1,0]; the full-width viewport
// puts that in pixels [0, W/2]; and the shader's own maths gives UV [0, 0.5] - the same half.
// Geometry and texture lookups agree again, because the viewport never lied to the shader.
//
// The offset must be scaled by w: clip coordinates are homogeneous, so a constant shift has to
// ride on the w term to survive the perspective divide.
void ApplyEyeRemap(Reg4* r, int conv, float s) {
    if (conv == CONV_ROW) {
        // clip.x = sum_i v_i * r[i].x + r[3].x, clip.w likewise from the .w column.
        for (int i = 0; i < 4; ++i) r[i].x = 0.5f * r[i].x + s * r[i].w;
    } else {
        // Register 0 IS the x column, register 3 the w column.
        r[0].x = 0.5f * r[0].x + s * r[3].x;
        r[0].y = 0.5f * r[0].y + s * r[3].y;
        r[0].z = 0.5f * r[0].z + s * r[3].z;
        r[0].w = 0.5f * r[0].w + s * r[3].w;
    }
}

void ApplyOffset(Reg4* r, int conv, const float* o) {
    if (conv == CONV_ROW) {
        float* r3 = &r[3].x;
        for (int k = 0; k < 4; ++k)
            r3[k] -= o[0]*(&r[0].x)[k] + o[1]*(&r[1].x)[k] + o[2]*(&r[2].x)[k];
    } else {
        for (int k = 0; k < 4; ++k)
            r[k].w -= o[0]*r[k].x + o[1]*r[k].y + o[2]*r[k].z;
    }
}

// ---------------------------------------------------------------- OpenXR
XrInstance g_xrInstance = XR_NULL_HANDLE;
XrSession  g_xrSession  = XR_NULL_HANDLE;
XrSpace    g_xrSpace    = XR_NULL_HANDLE;
XrSystemId g_xrSystem   = XR_NULL_SYSTEM_ID;
ID3D11Device* g_dev11 = nullptr;
ID3D11DeviceContext* g_ctx11 = nullptr;
bool g_xrReady = false, g_xrRunning = false;

// What the runtime recommends per eye, once known. Drives the resolution guidance now and the
// automatic sizing later.
uint32_t g_recEyeW = 0, g_recEyeH = 0;

bool InitXR() {
    const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    // VirtualDesktopXR is OpenXR 1.0 only and rejects a 1.1 instance with -4.
    XrVersion versions[] = { XR_MAKE_VERSION(1,0,34), XR_CURRENT_API_VERSION };
    XrResult r = XR_ERROR_RUNTIME_FAILURE;
    for (XrVersion v : versions) {
        XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
        strcpy_s(ici.applicationInfo.applicationName, "SingularityVR");
        ici.applicationInfo.apiVersion = v;
        ici.enabledExtensionCount = 1;
        ici.enabledExtensionNames = exts;
        r = xrCreateInstance(&ici, &g_xrInstance);
        if (XR_SUCCEEDED(r)) break;
    }
    if (XR_FAILED(r)) { Log("no OpenXR instance (headset not connected?) - detour still usable via F2"); return false; }

    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(g_xrInstance, &sgi, &g_xrSystem))) { Log("no HMD"); return false; }

    // Ask the runtime what resolution it actually wants per eye.
    //
    // This is the number the shipped mod must drive the game from, and it is the right one for a
    // reason worth recording: the recommended rect ALREADY includes whatever render-scale the user
    // set in SteamVR or the Meta app. So honouring it gives the behaviour every other VR title
    // has - resolution inherited from the headset, adjustable through the platform's own slider -
    // with no per-user command line anywhere.
    //
    // Side-by-side stereo needs twice the width in one frame, hence the doubling.
    {
        uint32_t viewCount = 0;
        xrEnumerateViewConfigurationViews(g_xrInstance, g_xrSystem,
                                          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                          0, &viewCount, nullptr);
        if (viewCount >= 1 && viewCount <= 4) {
            XrViewConfigurationView vcv[4]{};
            for (uint32_t i = 0; i < viewCount; ++i) vcv[i] = { XR_TYPE_VIEW_CONFIGURATION_VIEW };
            if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(
                    g_xrInstance, g_xrSystem, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                    viewCount, &viewCount, vcv))) {
                g_recEyeW = vcv[0].recommendedImageRectWidth;
                g_recEyeH = vcv[0].recommendedImageRectHeight;
                Log("headset wants %ux%u per eye (max %ux%u) - includes your runtime's"
                    " render-scale setting", g_recEyeW, g_recEyeH,
                    vcv[0].maxImageRectWidth, vcv[0].maxImageRectHeight);
                Log("  => for side-by-side stereo, launch the game with:"
                    "  -ResX=%u -ResY=%u -windowed", g_recEyeW * 2, g_recEyeH);
            }
        }
    }

    PFN_xrGetD3D11GraphicsRequirementsKHR pfn = nullptr;
    xrGetInstanceProcAddr(g_xrInstance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&pfn);
    if (!pfn) return false;
    XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    if (XR_FAILED(pfn(g_xrInstance, g_xrSystem, &req))) return false;

    IDXGIFactory1* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) return false;
    IDXGIAdapter1* ad = nullptr; IDXGIAdapter1* chosen = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{}; ad->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == req.adapterLuid.LowPart &&
            d.AdapterLuid.HighPart == req.adapterLuid.HighPart) { chosen = ad; break; }
        ad->Release();
    }
    fac->Release();
    if (!chosen) return false;
    D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_0 }, got{};
    if (FAILED(D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, lv, 1, D3D11_SDK_VERSION, &g_dev11, &got, &g_ctx11))) {
        chosen->Release(); return false;
    }
    chosen->Release();

    XrGraphicsBindingD3D11KHR bind{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    bind.device = g_dev11;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &bind; sci.systemId = g_xrSystem;
    if (XR_FAILED(xrCreateSession(g_xrInstance, &sci, &g_xrSession))) return false;

    XrReferenceSpaceCreateInfo rs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    if (XR_FAILED(xrCreateReferenceSpace(g_xrSession, &rs, &g_xrSpace))) return false;

    Log("OpenXR ready");
    g_xrReady = true;
    return true;
}

void PumpXR() {
    XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(g_xrInstance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_xrSession, &bi))) { g_xrRunning = true; Log("session running"); }
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_xrSession); g_xrRunning = false;
            }
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// ---------------------------------------------------------------- VR frame submission
// One swapchain per eye. In mono only [0] is ever filled and both eyes reference it; stereo
// fills them alternately. See the ALTERNATE-EYE note by g_stereo.
XrSwapchain g_swapchain[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
uint32_t g_scWidth = 0, g_scHeight = 0;
ID3D11Texture2D** g_scImages[2] = { nullptr, nullptr };
uint32_t g_scImageCount[2] = { 0, 0 };

// ---- ALTERNATE-EYE STEREO (F12) ----
//
// The game renders once per frame and there is no way to make it render twice from a D3D9
// shim, so the cheap route to two images is to alternate: this frame is drawn from the left
// eye, the next from the right, and each eye's swapchain holds the last image drawn for it.
// Both are submitted every frame, so one eye is always showing an image one frame old.
//
// The cost is real - each eye effectively updates at half the game's frame rate, and during
// fast head motion the two eyes disagree - which is why this is a toggle and not the default.
// The gain is that it is genuinely stereo, with correct occlusion, because each image really
// was rendered from that eye's position rather than reprojected.
//
// Almost no new maths: an eye offset IS a head position offset, so each eye's pose is fed
// through exactly the same XR->game mapping 6-DOF already uses. Interpupillary distance never
// appears as its own quantity.
volatile LONG g_stereo = 0;
int     g_renderEye = 0;             // which eye the frame now being drawn belongs to
XrPosef g_renderPose[2]{};           // the pose each eye's stored image was rendered from
bool    g_eyeFilled[2] = { false, false };

// ---- TRUE STEREO BY DRAW-CALL DUPLICATION (F1) ----
//
// Alternate-eye's flaw is temporal: the two eyes show the world at different instants, which
// no amount of reprojection can reconcile, and head rotation makes it obvious. The fix is to
// draw both eyes from the SAME instant, which means issuing every scene draw twice.
//
// The prototype needs no double-wide render target. Split the existing backbuffer down the
// middle with the VIEWPORT: left half gets the left-eye matrix, right half the right-eye
// matrix, and OpenXR is handed each half as a subImage rect. Half horizontal resolution per
// eye, but the technique is complete and end-to-end - if it works here, widening the target
// later is an optimisation rather than a question.
//
// Viewport switching rather than render-target switching is what keeps this affordable: two
// extra API calls and one extra draw per scene draw, no target rebinds.
//
// Known-imperfect by design: full-screen passes (post-processing, HUD) do not have the camera
// matrix bound, so StereoPair duplicates them into both halves WITHOUT an offset - each half
// gets an identical, un-parallaxed copy. For most such passes that is close to correct (they
// sample screen-space UVs relative to whatever viewport is active, so each half is filled
// properly); the visible compromise is that they carry no depth/stereo cue, not that they are
// missing or wrong-side. That is a smaller problem than the one this used to have - see the
// comment on StereoPair for what that was.
// Cold start is flat, as before - see SetVrMode / BACKSPACE for the one-key way to reach head
// tracking + 6-DOF + true stereo without three separate presses.
volatile LONG g_dupDraws = 0;

// ---- diagnostics for "visible in one eye only" (run 9) ----
//
// Run 9 reported rocks off to the LEFT that are missing from the LEFT eye - the eye NEARER to
// them - and stay missing regardless of view direction. That is inverted from what frustum
// clipping predicts. For an object off to the left, the RIGHT eye sits further from it and so
// sees it at the LARGER off-axis angle, meaning the right eye should lose it first. Observing
// the opposite says either the half-to-eye mapping is reversed somewhere, or the cause is not
// clipping at all. Reading the code did not find a swap, and this is the second wrong guess in
// a row, so: measure instead.
//
// INSERT blanks one half - whichever eye goes black identifies the mapping unambiguously.
// DELETE swaps the assignment, so if it is reversed the fix can be confirmed on the spot.
volatile LONG g_eyeDebug = 0;        // 0 = both halves, 1 = left half only, 2 = right half only
volatile LONG g_swapEyes = 0;        // reverse which half feeds which eye

// ---- HOME: the scissor kill-switch, and why it is decisive (run 28) ----
//
// After run 27 the numbers say every scene draw is split, every one is parallaxed, identification
// never lapses and only the two real scene targets are touched - and objects still vanish. Two
// mechanisms survive that, and they differ in one respect that no counter here can see:
//
//   * the object IS drawn, and our scissor clips it away, because we could not remap its
//     transform and it therefore still holds full-frame coordinates. The centre of the frame is
//     the seam, so a head-on object is discarded in BOTH halves.
//   * the object is NEVER drawn, because the engine culled it. Measured and real: its own
//     arriving matrix carries 51.1 deg vertical while we render 98.0.
//
// Turning the scissor off separates them in one keypress. Geometry we DID remap is unaffected -
// the clip-space remap already places it inside its half, so the scissor was never what put it
// there. Geometry we did NOT remap reappears, spanning the full frame and duplicated, which is
// wrong-looking but unmistakable.
//
//   object comes back -> it was being drawn and we clipped it. Draw classification is the fix.
//   object stays gone -> no draw call ever existed. Culling is the fix.
//
// This is the discriminator the last six theories all lacked: they argued about which mechanism
// it was without any test that could tell drawn-then-clipped from never-drawn.
volatile LONG g_noScissor = 0;

// ---- END: collapse c0, and read off what is left (run 29) ----
//
// The question nothing here has ever answered: does the geometry that vanishes actually take its
// transform from the register we remap? Run 18 said no, STATUS records that as refuted by
// "register count measured 1, ever", and run 28 showed that counter is capped at 1 by
// construction. So the theory stands untested, and the scan cannot settle it either - it looks for
// VIEW matrices, and a composed local-to-clip matrix can never pass its camera-at-origin test.
//
// This settles it without parsing a single shader. Upload a c0 that forces clip.w negative, so
// every triangle driven by it is discarded by the hardware before rasterisation. Whatever is
// STILL VISIBLE is, by construction, geometry we do not remap - and therefore geometry the
// scissor is clipping at the seam while it still holds full-frame coordinates.
//
//   the chest is sitting there when everything else collapses  -> run 18 was right all along
//   the screen goes essentially empty                          -> run 18 is finally dead
//
// The blanking is applied before the matrix is cached, so StereoPair's per-eye uploads inherit it
// and there is no path by which an unblanked copy reaches the GPU.
volatile LONG g_blankCamMat = 0;

float g_eyeDeltaUU[3] = {0,0,0};     // left eye -> right eye, in game world units
bool  g_camMatrixBound = false;      // is the camera's view matrix the one currently set?

// ---- every register currently holding a camera view-projection, not just one ----
//
// Run 18: a monster vanished when directly in FRONT of the player. Dead centre of the view is dead
// centre of the full-width frame, which is precisely the seam between the two eye halves - so
// geometry that never gets remapped into a half straddles the seam and the scissor discards it in
// BOTH. Off to one side part of it survives in one half; centred, it goes completely.
//
// The cause is that a single locked register cannot cover the frame. Skeletal meshes lay their
// constants out differently from static meshes, because a bone palette occupies a block of
// registers, so characters take their view-projection from somewhere else entirely. Tracking one
// register meant characters were never remapped.
//
// So track a small set. Each slot holds the finished LEFT-eye matrix for one register; the draw
// hook derives both eyes from every valid slot. Slots are invalidated as soon as their register is
// seen carrying something that is not a camera matrix, so a stale entry can never be re-uploaded
// over live engine state.
struct CamMatSlot { UINT reg; int conv; bool valid; float m[16]; };
const int kMaxCamMats = 8;
CamMatSlot g_camMats[kMaxCamMats]{};
int g_camMatUsed = 0;                // high-water mark of slots ever allocated
int g_camMatValidPeak = 0;           // diagnostic: how many were live in a frame
int   g_drawsTotal = 0, g_drawsDuped = 0, g_drawsDupedOffset = 0, g_drawsOffscreen = 0;
// Draws that had no live camera matrix and so were drawn once, unsplit. These are the ones that
// used to be scissored-but-unremapped and therefore vanished at the seam.
int   g_drawsMonoFallback = 0;

// ---- a slot that stays valid without a fresh upload behind it is a live corruption ----
//
// StereoPair re-uploads g_camMats[s].m for every draw while the slot is valid. If the constant
// hook stops re-validating that slot but leaves it set, every draw of the frame is forced through
// a view-projection captured on some EARLIER frame - the whole scene rendered from a frozen
// camera, then unfrozen, which is indistinguishable from the flicker being chased.
//
// Three paths used to do exactly that, all of them early returns in Hook_SetVSConstF that skipped
// the invalidation block at the bottom of the function: no valid camera position (retried only
// every 30 frames, so up to half a second of frozen frames), no lock, and an upload that
// overlapped the tracked window without covering it. The lock-drop path in Present was a fourth.
//
// Rule from here on: any path that leaves without positively re-validating must invalidate.
inline void InvalidateCamMats() {
    for (int s = 0; s < g_camMatUsed; ++s) g_camMats[s].valid = false;
}

// ---- how often identification fails, which nothing measured before ----
//
// c0 carries one matrix per frame, so a rejection is a whole-frame event: no forced projection and
// no split for any draw in it. If that is happening even occasionally it is a violent artefact,
// and the counters below are the only way to tell it apart from the other candidate mechanisms.
// g_bestRejectDot is the diagnostic that names the cause: a rejection at 0.985 is rotation lag, a
// rejection at -0.1 is a genuinely different matrix.
int   g_lockUploads = 0, g_lockRejects = 0;      // per frame, reset in Present
int   g_framesNoIdent = 0;                       // per report window: frames that lost it entirely
float g_bestRejectDot = -2.0f;                   // per report window

ID3D11Texture2D*   g_uploadTex  = nullptr;   // D3D11 side, CPU-writable
UINT g_srcW = 0, g_srcH = 0;
D3DFORMAT g_srcFmt = D3DFMT_UNKNOWN;
float g_headFovDeg = 0.0f;
XrPosef g_eyePose[2]{};
XrFovf  g_eyeFov[2]{};
bool    g_haveEyes = false;
int     g_framesSubmitted = 0;

// ---------------------------------------------------------------- the frame copy, measured
//
// "The frame copy is a full CPU round-trip" has been this project's performance headline since
// spike 2, and it has never once been broken down. It is three separate costs with three
// different fixes, and only one of them is the one D3D9Ex addresses:
//
//   GetRenderTargetData   GPU -> system memory. D3D9 has no asynchronous readback, so this
//                         blocks; and it is issued at Present, when the GPU queue is deepest,
//                         so it pays for a full pipeline drain on top of the transfer itself.
//   lock + memcpy         system memory -> the D3D11 upload texture. Pure CPU bandwidth,
//                         ~14 MB at 1440p, and nothing about D3D9Ex makes it cheaper.
//   upload + CopyResource CPU -> GPU again, into the XR swapchain image.
//
// This matters for sequencing. The D3D9Ex + D3DPOOL_MANAGED wrapper is a large, invasive piece
// of work - 10,454 MANAGED allocations were measured in spike 2, every one of which needs a
// backing store we own - and it is justified by the assumption that the readback dominates.
// That assumption has never been tested. If the drain is the cost, deferring the readback by a
// frame recovers most of it for almost nothing. If the memcpy is the cost, D3D9Ex would not
// have helped at all. Measure first, then spend.
double g_msGRTD = 0.0, g_msLock = 0.0, g_msLockCopy = 0.0, g_msUpload = 0.0;   // over the perf window
int    g_copyFrames = 0;

// ---- and the other half of the frame, which turned out to be all of it (run 31) ----
//
// xrWaitFrame blocks until the compositor is ready for the next frame. That is the whole point
// of it - it is how an OpenXR app is paced to the display - but it means a frame spent waiting
// there is a frame that finished its work early and is now idle. Time inside it is NOT our cost,
// and every ms attributed to it is a ms that optimising our own code cannot recover.
//
// Without this split, "residual" lumps genuine work together with deliberate idling, and the two
// point at opposite conclusions.
double g_msWaitFrame = 0.0;
int    g_waitFrames = 0;
double g_displayPeriodMs = 0.0;   // from XrFrameState, so the pacing target is measured not guessed

// ---- ⚠️ the copy timing conflates two costs, and only one of them D3D9Ex removes ----
//
// GetRenderTargetData does not just transfer - it SYNCHRONISES. The call cannot return until the
// GPU has finished rendering the frame, so the time attributed to `copy` is:
//
//     (wait for the GPU to finish the frame)  +  (the actual GPU->CPU transfer)
//
// Only the second is the round-trip. The first is real rendering work that a zero-copy path would
// still have to wait for - it would simply happen inside the compositor instead of inside us.
//
// This matters enormously for the D3D9Ex decision. At 4096x2160 `copy` measures ~12.5 ms. If that
// is mostly transfer, removing it takes the frame from 16 ms to ~4 and the wrapper is transformative.
// If it is mostly GPU wait, the wrapper buys almost nothing and run 31's cancelled verdict was
// accidentally right. The two cannot be told apart from the current number, and the entire case for
// weeks of wrapper work rests on which it is.
//
// So: issue a D3DQUERYTYPE_EVENT fence and block on it FIRST. That wait is the GPU finishing.
// Whatever GetRenderTargetData costs afterwards is transfer, with the sync already paid.
//
// This does not add work - GetRenderTargetData was going to wait for the GPU regardless. It only
// moves the wait somewhere it can be timed. Total frame time should be unchanged, and if it is
// not, this probe is lying (see the pipelining lesson: a stall that leaves the instrument has not
// left the frame).
// The GObjects walks (FindCamera / FindController), which are free on a cache hit and brutal on a
// miss. Broken out of "our work" so a menu stall can be attributed rather than guessed at.
double g_msEngineObjects = 0.0;
int    g_engineObjectFrames = 0;

IDirect3DQuery9* g_gpuFence = nullptr;
double g_msGpuWait = 0.0;
int    g_gpuWaitFrames = 0;
int    g_gpuFenceMode = 1;        // ini GpuFenceProbe: 1 = split the copy, 0 = leave it alone

inline LARGE_INTEGER Now() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t; }

inline double MsSince(const LARGE_INTEGER& t0) {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    return double(now.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
}

// ---- pipelining the readback, and why the pose has to travel with the pixels ----
//
// Issuing GetRenderTargetData and locking its result in the same breath is the worst available
// ordering: the CPU waits for the GPU to finish everything queued, and then waits again for the
// transfer. With two slots we issue this frame's readback and consume the one issued last
// frame, so the transfer has a whole frame of GPU work to hide behind.
//
// Whether the driver actually lets it overlap is precisely what the phase timings above will
// say, which is why this is a switch rather than simply the new behaviour. `FrameCopyMode=1`
// restores the old ordering for an A/B in the same session.
//
// The price is one frame of latency, and it is paid honestly rather than ignored: each slot
// carries the eye, the head pose and the projection its pixels were actually drawn with, and
// those are what get submitted. Handing the compositor a fresh pose for a stale image is how
// reprojection turns latency into visible swim - the image would be corrected towards a
// position the frame never rendered from.
const int kCopySlots = 2;

struct CopySlot {
    IDirect3DSurface9* surf = nullptr;   // D3D9 SYSTEMMEM copy target
    bool    pending = false;             // holds pixels captured but not yet consumed
    int     eye = 0;                     // g_renderEye at capture (alternate-eye mode)
    bool    stereo = false;              // alternate-eye active for this frame
    bool    dup = false;                 // draw duplication active for this frame
    XrPosef pose[2]{};                   // where the head was when these pixels were drawn
    XrFovf  fov[2]{};
    bool    useDerived = false;
    XrFovf  derived{};
};
CopySlot g_slot[kCopySlots];
int g_slotCur = 0;

// ---- ⚠️ measured run 32: pipelining LOSES. Immediate is the default. ----
//
// Matched by scene (same draw counts, both at 120 Hz with SSW off):
//
//     draws   pipelined            immediate
//       367   107.0 fps (9.34ms)   109.7 fps (9.12ms)
//       444   111.0 fps (9.01ms)   111.3 fps (8.98ms)
//       138   114.5 fps (8.74ms)   117.5 fps (8.51ms)
//
// Immediate is equal or slightly faster every time, and it does not cost a frame of latency.
//
// The phase split says why, and it is worth understanding before anyone tries this again:
// pipelining did not remove the readback stall, it RELOCATED it. Pipelined reports copy 0.89 ms
// and "our work" 9.4 ms; immediate reports copy 4.2 ms and "our work" 3.7 ms. Same total. Issuing
// GetRenderTargetData without locking its result lets the call return immediately, so the phase
// timer sees nothing - but the driver still has to do the transfer, and it surfaces later inside
// work we do not measure. Moving a stall out of an instrument's view is not an optimisation.
//
// Numbers kept stable rather than renumbered, so old logs and STATUS entries still read correctly.
int g_copyMode = 1;                      // 0 = pipelined, 1 = immediate  <- default, measured

// ---------------------------------------------------------------- STAGE 2: the zero-copy path
//
// What the whole D3D9Ex exercise was for. Instead of dragging the frame through system memory:
//
//   now      GetRenderTargetData (GPU->sysmem, blocks) + LockRect (DMA lands) + memcpy 35 MB
//            + Map + CopyResource                                        ~9.8 ms at 4096x2160
//   stage 2  StretchRect backbuffer -> shared RT (GPU->GPU)
//            + CopyResource shared -> XR swapchain (GPU->GPU)            target: well under 1 ms
//
// The frame never touches the CPU. Spike 1 proved every link of this in a 32-bit process: a
// D3D9Ex texture created with a shared handle, opened by ID3D11Device::OpenSharedResource, with
// the pixels arriving intact.
//
// ---- synchronisation, which is the only genuinely new problem ----
//
// D3D9 has no counterpart to D3D11's keyed mutex, so nothing makes the D3D11 read wait for the
// D3D9 write. A D3DQUERYTYPE_EVENT fence issued AFTER the StretchRect and waited on before the
// D3D11 copy is what spike 1 used, and it is what is used here.
//
// Note the fence moves relative to the CPU path. There it is issued BEFORE the readback and
// measures "GPU finished the frame". Here it must come AFTER the StretchRect is queued, so the
// one wait covers both the frame and the copy into the shared target.
//
// That wait is not overhead we are adding - run 35 measured it at 2-4 ms and it is genuine
// rendering time. The frame cannot be shown before it has been drawn. What disappears is the
// ~9.4 ms of transfer and memcpy stacked on top of it.
// Set when CreateDeviceEx succeeds, down in the D3DPOOL_MANAGED section. Declared up here because
// the frame path needs it well before that code appears, and because zero-copy is meaningless
// without it: a shared surface cannot be created on a plain D3D9 device.
bool g_deviceIsEx = false;

int g_zeroCopy = 1;                      // ini ZeroCopy - only ever active when the device is Ex
bool g_zeroCopyActive = false;           // resolved at resource-creation time, per resolution

HANDLE              g_sharedHandle = nullptr;
IDirect3DTexture9*  g_sharedTex9   = nullptr;   // D3DUSAGE_RENDERTARGET, DEFAULT, shared
IDirect3DSurface9*  g_sharedSurf9  = nullptr;   // its level 0, the StretchRect destination
ID3D11Texture2D*    g_sharedTex11  = nullptr;   // the same memory, opened by D3D11
ID3D11Texture2D*    g_frameSrc11   = nullptr;   // what the submit block copies into the swapchain

// Create the XR swapchain lazily, sized to the game's backbuffer - we are pasting the game's
// own image, so matching its dimensions avoids any rescale on the way in.
bool EnsureSwapchain(UINT w, UINT h) {
    if (g_swapchain[0] != XR_NULL_HANDLE && w == g_scWidth && h == g_scHeight) return true;
    for (int e = 0; e < 2; ++e) {
        if (g_swapchain[e] != XR_NULL_HANDLE) { xrDestroySwapchain(g_swapchain[e]); g_swapchain[e] = XR_NULL_HANDLE; }
        if (g_scImages[e]) { delete[] g_scImages[e]; g_scImages[e] = nullptr; }
        g_eyeFilled[e] = false;
    }

    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(g_xrSession, 0, &fmtCount, nullptr);
    int64_t* fmts = new int64_t[fmtCount ? fmtCount : 1];
    xrEnumerateSwapchainFormats(g_xrSession, fmtCount, &fmtCount, fmts);
    // Colour space matters here. The game writes sRGB-ENCODED pixels into its backbuffer. If
    // the swapchain is declared linear (plain _UNORM), the compositor assumes the values are
    // linear and applies its own linear->sRGB conversion, encoding them a second time - which
    // looks washed out and low-contrast. Declaring an _SRGB format tells the compositor the
    // data is already encoded, so it decodes before compositing.
    //
    // B8G8R8A8_UNORM_SRGB is bit-identical to the D3D9 A8R8G8B8 layout, and shares a type
    // group with B8G8R8A8_UNORM, so CopyResource from the upload texture is still legal.
    const int64_t prefer[] = {
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM,
    };
    int64_t chosen = fmtCount ? fmts[0] : 0;
    bool picked = false;
    for (int p = 0; p < 3 && !picked; ++p)
        for (uint32_t i = 0; i < fmtCount; ++i)
            if (fmts[i] == prefer[p]) { chosen = fmts[i]; picked = true; break; }
    Log("swapchain format %lld %s", (long long)chosen,
        (chosen == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || chosen == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            ? "(sRGB - correct for the game's output)" : "(LINEAR - expect washed out colours)");
    delete[] fmts;

    for (int e = 0; e < 2; ++e) {
        XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format = chosen;
        sci.sampleCount = 1;
        sci.width = w; sci.height = h;
        sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(g_xrSession, &sci, &g_swapchain[e]))) {
            Log("xrCreateSwapchain failed for eye %d", e); return false;
        }
        xrEnumerateSwapchainImages(g_swapchain[e], 0, &g_scImageCount[e], nullptr);
        XrSwapchainImageD3D11KHR* imgs = new XrSwapchainImageD3D11KHR[g_scImageCount[e]];
        for (uint32_t i = 0; i < g_scImageCount[e]; ++i) imgs[i] = { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR };
        xrEnumerateSwapchainImages(g_swapchain[e], g_scImageCount[e], &g_scImageCount[e],
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs));
        g_scImages[e] = new ID3D11Texture2D*[g_scImageCount[e]];
        for (uint32_t i = 0; i < g_scImageCount[e]; ++i) g_scImages[e][i] = imgs[i].texture;
        delete[] imgs;
    }

    g_scWidth = w; g_scHeight = h;
    Log("XR swapchains (2) %ux%u fmt=%lld images=%u each", w, h, (long long)chosen, g_scImageCount[0]);
    return true;
}

// D3D9 SYSTEMMEM surfaces + matching CPU-writable D3D11 texture, all sized to the backbuffer.
bool EnsureCopyResources(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt) {
    if (g_slot[0].surf && g_uploadTex && w == g_srcW && h == g_srcH && fmt == g_srcFmt) return true;
    for (int i = 0; i < kCopySlots; ++i) {
        if (g_slot[i].surf) { g_slot[i].surf->Release(); g_slot[i].surf = nullptr; }
        g_slot[i].pending = false;
    }
    g_slotCur = 0;
    if (g_uploadTex)  { g_uploadTex->Release();  g_uploadTex = nullptr; }

    // GetRenderTargetData demands identical size AND format on both surfaces. Take the format
    // from the backbuffer rather than assuming: this build reports 21 = D3DFMT_A8R8G8B8, and
    // hardcoding X8R8G8B8 (22) is what made the call return D3DERR_INVALIDCALL.
    for (int i = 0; i < kCopySlots; ++i) {
        if (FAILED(dev->CreateOffscreenPlainSurface(w, h, fmt, D3DPOOL_SYSTEMMEM,
                                                    &g_slot[i].surf, nullptr))) {
            Log("CreateOffscreenPlainSurface %d failed for fmt=%d", i, (int)fmt); return false;
        }
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev11->CreateTexture2D(&td, nullptr, &g_uploadTex))) {
        Log("CreateTexture2D(upload) failed"); return false;
    }
    // One fence, reused. EVENT queries pass straight through Hook_QueryGetData - it only rewrites
    // D3DQUERYTYPE_OCCLUSION - so this cannot collide with the run-30 override.
    if (g_gpuFenceMode && !g_gpuFence) {
        if (FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_gpuFence))) {
            g_gpuFence = nullptr;
            Log("GPU fence unavailable - copy timing will keep GPU wait and transfer merged");
        }
    }

    // ---- the shared render target: the whole point of the D3D9Ex upgrade ----
    //
    // Created at the backbuffer's own format so StretchRect needs no conversion, one mip level and
    // DEFAULT pool because that is all a shared surface may be, and with a non-null pSharedHandle,
    // which is the part a plain D3D9 device refuses (spike 1 confirmed the refusal).
    //
    // This passes through our own Hook_CreateTexture untouched - the pool is already DEFAULT, so
    // the MANAGED branch is never taken.
    if (g_sharedSurf9) { g_sharedSurf9->Release(); g_sharedSurf9 = nullptr; }
    if (g_sharedTex9)  { g_sharedTex9->Release();  g_sharedTex9 = nullptr; }
    if (g_sharedTex11) { g_sharedTex11->Release(); g_sharedTex11 = nullptr; }
    g_sharedHandle = nullptr;
    g_zeroCopyActive = false;

    if (g_zeroCopy && g_deviceIsEx) {
        HRESULT hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT,
                                        &g_sharedTex9, &g_sharedHandle);
        if (FAILED(hr) || !g_sharedTex9 || !g_sharedHandle) {
            Log("*** shared render target creation FAILED hr=0x%08lX - falling back to the CPU copy ***", hr);
        } else if (FAILED(g_sharedTex9->GetSurfaceLevel(0, &g_sharedSurf9)) || !g_sharedSurf9) {
            Log("*** GetSurfaceLevel on the shared RT failed - falling back to the CPU copy ***");
        } else if (FAILED(g_dev11->OpenSharedResource(g_sharedHandle, __uuidof(ID3D11Texture2D),
                                                      (void**)&g_sharedTex11)) || !g_sharedTex11) {
            Log("*** OpenSharedResource FAILED - D3D11 could not open the D3D9 texture,"
                " falling back to the CPU copy ***");
        } else {
            D3D11_TEXTURE2D_DESC sd{};
            g_sharedTex11->GetDesc(&sd);
            g_zeroCopyActive = true;
            Log("*** ZERO-COPY ACTIVE: shared RT %ux%u, D3D11 sees fmt=%d - the frame no longer"
                " touches the CPU ***", sd.Width, sd.Height, (int)sd.Format);
        }
        if (!g_zeroCopyActive) {
            if (g_sharedSurf9) { g_sharedSurf9->Release(); g_sharedSurf9 = nullptr; }
            if (g_sharedTex9)  { g_sharedTex9->Release();  g_sharedTex9 = nullptr; }
            if (g_sharedTex11) { g_sharedTex11->Release(); g_sharedTex11 = nullptr; }
            g_sharedHandle = nullptr;
        }
    } else if (g_zeroCopy && !g_deviceIsEx) {
        static bool said = false;
        if (!said) { said = true;
            Log("ZeroCopy requested but the device is not D3D9Ex - needs D3D9ExMode=1. Using the CPU copy."); }
    }

    g_srcW = w; g_srcH = h; g_srcFmt = fmt;
    Log("copy resources ready for %ux%u fmt=%d (%d readback slots, %s, %s)", w, h, (int)fmt,
        kCopySlots, g_copyMode == 1 ? "immediate" : "pipelined one frame",
        g_zeroCopyActive ? "ZERO-COPY" : "CPU round-trip");
    return true;
}

// ---------------------------------------------------------------- culling band measurement
//
// Offsetting the camera reveals geometry the game culled on the CPU against its ORIGINAL
// frustum, so it was never submitted as a draw call and comes out as a dead strip down the
// edge of the frame. How wide that strip is decides whether the FOV-widening work is needed
// at stereo-relevant offsets, or whether it can be skipped entirely.
//
// Measuring it beats eyeballing it, and not only for precision: the artefact lives in the
// GAME's frame, so judging it through the headset would mean judging it through the aspect
// mismatch and the lens distortion as well - neither of which will be there in the final
// product. Reading the backbuffer directly sidesteps all of that, and reports the same number
// whether or not anyone is wearing the headset.
//
// The scan walks in from each edge counting columns that are uniformly near-black. Sampling
// every 16th row is plenty: the strip spans the full height, so a partial sample cannot miss
// it, and a bright frame stops the scan on its first column.
int g_bandLeft = 0, g_bandRight = 0;

void MeasureCullBand(const uint8_t* bits, int pitch, UINT w, UINT h) {
    const int kDark = 12;           // out of 255, per channel - "nothing was drawn here"
    const UINT kRowStep = 16;
    const UINT kMaxBand = w / 2;

    auto columnIsDark = [&](UINT x) {
        for (UINT y = 0; y < h; y += kRowStep) {
            const uint8_t* px = bits + (size_t)y * pitch + (size_t)x * 4;   // BGRA
            if (px[0] > kDark || px[1] > kDark || px[2] > kDark) return false;
        }
        return true;
    };

    UINT l = 0; while (l < kMaxBand && columnIsDark(l)) ++l;
    UINT r = 0; while (r < kMaxBand && columnIsDark(w - 1 - r)) ++r;
    g_bandLeft = (int)l; g_bandRight = (int)r;

    // Report periodically with the offset that produced it, so the log correlates the two
    // without needing timestamps or the user to remember which step they were on.
    static int n = 0;
    if (++n % 60 == 0) {
        if (InterlockedCompareExchange(&g_dupDraws, 0, 0)) {
            // A real interpupillary distance is a few UU (kMetresToUU * ~0.063 m ~= 3-4 UU).
            // Logging it directly settles, rather than guesses at, whether "visible in only
            // one eye no matter where you look" is a real magnitude/sign bug in this vector
            // versus a genuine (and much smaller) parallax/occlusion effect.
            float mag = sqrtf(g_eyeDeltaUU[0]*g_eyeDeltaUU[0] + g_eyeDeltaUU[1]*g_eyeDeltaUU[1] +
                              g_eyeDeltaUU[2]*g_eyeDeltaUU[2]);
            Log("cull band: left=%u right=%u of %u px (%.2f%% / %.2f%%)  eye delta "
                "(%.2f, %.2f, %.2f) UU, |%.2f|%s", l, r, w, 100.0 * l / w, 100.0 * r / w,
                g_eyeDeltaUU[0], g_eyeDeltaUU[1], g_eyeDeltaUU[2], mag,
                (mag > 20.0f) ? "  <-- FAR outside real IPD range, suspect a bug" : "");
        } else if (InterlockedCompareExchange(&g_sixDof, 0, 0)) {
            Log("cull band: left=%u right=%u of %u px (%.2f%% / %.2f%%)  6DOF offset "
                "(%.1f, %.1f, %.1f) UU", l, r, w, 100.0 * l / w, 100.0 * r / w,
                g_hmdOffset[0], g_hmdOffset[1], g_hmdOffset[2]);
        } else {
            bool on = InterlockedCompareExchange(&g_injectOn, 0, 0) != 0;
            Log("cull band: left=%u right=%u of %u px (%.2f%% / %.2f%%)  offset=%s%.0f UU",
                l, r, w, 100.0 * l / w, 100.0 * r / w, on ? "" : "OFF, was ", g_offsetUU);
        }
    }
}

// Stamp the slot with the state its pixels were drawn under. Everything the submission needs is
// captured at the moment of capture, so a frame consumed later cannot be submitted with a pose or
// a projection it was never rendered from. Shared by both the CPU and zero-copy paths - the two
// must never disagree about what a captured frame means.
void StampCaptureState(CopySlot& cap) {
    cap.stereo = InterlockedCompareExchange(&g_stereo, 0, 0) != 0;
    cap.dup    = InterlockedCompareExchange(&g_dupDraws, 0, 0) != 0;
    // The backbuffer just captured was drawn with the offset chosen at the END of the previous
    // frame, so it belongs to g_renderEye - not to whichever eye is about to be switched to.
    cap.eye    = cap.stereo ? g_renderEye : 0;
    for (int i = 0; i < 2; ++i) {
        cap.pose[i] = cap.stereo ? g_renderPose[i] : g_eyePose[i];
        cap.fov[i]  = g_eyeFov[i];
    }
    // Submit the frustum we FORCED, not the one we observed. Following the observation was the
    // run-5 flicker: the engine's FOV drifts between our writes, so a submitted frustum that
    // tracked it inherited every wobble.
    cap.useDerived = InterlockedCompareExchange(&g_vrProjection, 0, 0) != 0 &&
                     g_targetTanX > 0.0f && g_targetTanY > 0.0f;
    if (cap.useDerived) {
        cap.derived.angleLeft  = -atanf(g_targetTanX);
        cap.derived.angleRight =  atanf(g_targetTanX);
        cap.derived.angleUp    =  atanf(g_targetTanY);
        cap.derived.angleDown  = -atanf(g_targetTanY);
    }
}

// Pull the game's finished frame across to D3D11. On the CPU path this is the expensive part: a
// full GPU->CPU readback followed by a CPU->GPU upload, ~14 MB each way at 1440p and ~35 MB at 4K.
// Under zero-copy it is two GPU-side copies and a fence.
//
// Returns the slot whose pixels now sit in g_uploadTex, or nullptr if there is nothing to submit.
// In pipelined mode the very first frame returns nullptr because nothing has been captured yet -
// that is normal startup, not a failure.
const CopySlot* CopyBackbufferToD3D11(IDirect3DDevice9* dev) {
    IDirect3DSurface9* back = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return nullptr;
    D3DSURFACE_DESC sd{};
    back->GetDesc(&sd);
    if (!EnsureCopyResources(dev, sd.Width, sd.Height, sd.Format)) { back->Release(); return nullptr; }

    CopySlot& cap = g_slot[g_slotCur];

    // ---- zero-copy: the frame goes GPU -> GPU and never enters system memory ----
    if (g_zeroCopyActive && g_sharedSurf9) {
        LARGE_INTEGER tS = Now();
        // Same size, same format, no filtering. Queued, not executed - the fence below is what
        // makes it safe for D3D11 to read.
        HRESULT hrS = dev->StretchRect(back, nullptr, g_sharedSurf9, nullptr, D3DTEXF_NONE);
        back->Release();
        g_msGRTD += MsSince(tS);
        if (FAILED(hrS)) {
            static bool once = false;
            if (!once) { once = true;
                Log("*** StretchRect to the shared RT failed hr=0x%08lX - dropping to the CPU copy"
                    " for the rest of this session ***", hrS); }
            g_zeroCopyActive = false;
            return nullptr;
        }
        // The fence goes AFTER the copy is queued, not before it as on the CPU path: this one wait
        // has to cover the frame being drawn AND the StretchRect landing, because D3D9 offers
        // nothing like a keyed mutex to make the D3D11 read wait on its own.
        if (g_gpuFence) {
            g_gpuFence->Issue(D3DISSUE_END);
            LARGE_INTEGER tf = Now();
            BOOL done = FALSE;
            while (g_gpuFence->GetData(&done, sizeof(done), D3DGETDATA_FLUSH) == S_FALSE) {
                if (MsSince(tf) > 100.0) { Log("GPU fence did not signal within 100 ms - abandoning the wait"); break; }
                YieldProcessor();
            }
            g_msGpuWait += MsSince(tf);
            ++g_gpuWaitFrames;
        }
        // ---- ⚠️ no pipelining here, and it is not an oversight ----
        //
        // FrameCopyMode's ring exists because a sysmem readback can be deferred a frame. There is
        // exactly ONE shared render target, and StretchRect has just overwritten it with THIS
        // frame - so consuming an older slot's metadata would submit these pixels with the
        // previous frame's pose. That is the reprojection-swim failure the capture stamping exists
        // to prevent, arrived at from the opposite direction.
        //
        // A dedicated slot rather than the ring, so the two paths cannot interfere if the mode is
        // switched mid-session.
        static CopySlot zc;
        StampCaptureState(zc);
        zc.pending = false;
        g_frameSrc11 = g_sharedTex11;
        ++g_copyFrames;
        // MeasureCullBand is unavailable here by construction - there are no CPU-readable pixels
        // any more. Said once so a missing diagnostic is never mistaken for a zero reading.
        static bool saidBand = false;
        if (!saidBand) { saidBand = true;
            Log("note: the cull-band measurement is inert under zero-copy - the frame is never"
                " mapped to the CPU. Set ZeroCopy=0 to measure it."); }
        return &zc;
    }

    // Pay the GPU sync here, where it can be measured, instead of anonymously inside
    // GetRenderTargetData. See the note on g_gpuFence: this is the number the D3D9Ex decision
    // turns on, because only the transfer half of `copy` is what a zero-copy path removes.
    if (g_gpuFence) {
        g_gpuFence->Issue(D3DISSUE_END);
        LARGE_INTEGER tf = Now();
        BOOL done = FALSE;
        // D3DGETDATA_FLUSH so the command buffer is actually submitted rather than sat on.
        //
        // Bounded. An unbounded spin on a query that never completes - a lost device, a driver
        // hiccup - would wedge the game with no way out but the task manager, and this is a
        // measurement, not a feature. Bailing early only costs one frame's accuracy.
        while (g_gpuFence->GetData(&done, sizeof(done), D3DGETDATA_FLUSH) == S_FALSE) {
            if (MsSince(tf) > 100.0) { Log("GPU fence did not signal within 100 ms - abandoning the wait"); break; }
            YieldProcessor();
        }
        g_msGpuWait += MsSince(tf);
        ++g_gpuWaitFrames;
    }

    LARGE_INTEGER t0 = Now();
    HRESULT hr = dev->GetRenderTargetData(back, cap.surf);
    g_msGRTD += MsSince(t0);
    back->Release();
    if (FAILED(hr)) {
        static bool once = false;
        if (!once) {
            Log("GetRenderTargetData failed hr=0x%08lX (backbuffer %ux%u fmt=%d msaa=%d)",
                hr, sd.Width, sd.Height, (int)sd.Format, (int)sd.MultiSampleType);
            once = true;
        }
        return nullptr;
    }

    StampCaptureState(cap);
    cap.pending = true;

    // Which slot we consume is the entire difference between the two modes.
    const int consumeIdx = (g_copyMode == 1) ? g_slotCur : (g_slotCur + 1) % kCopySlots;
    g_slotCur = (g_slotCur + 1) % kCopySlots;

    CopySlot& use = g_slot[consumeIdx];
    if (!use.pending) return nullptr;          // pipelined, first frame - nothing captured yet

    // ---- ⚠️ LockRect is where the transfer actually surfaces (run 35) ----
    //
    // GetRenderTargetData times at 0.00 ms once the GPU fence is paid, which does NOT mean the
    // transfer is free - the driver defers it, and LockRect is what blocks until the DMA lands.
    // Timing lock and memcpy as one bucket therefore labelled ~7 ms of GPU->CPU transfer as
    // "memcpy", which is wrong in a way that matters: memcpy is CPU bandwidth and transfer is bus
    // latency, and they have different fixes. Both are removed by a zero-copy path, so the
    // RECOVERABLE total was right even while its breakdown was not.
    LARGE_INTEGER tLock = Now();
    D3DLOCKED_RECT lr{};
    if (FAILED(use.surf->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return nullptr;
    g_msLock += MsSince(tLock);

    t0 = Now();
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(g_ctx11->Map(g_uploadTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        use.surf->UnlockRect(); return nullptr;
    }
    const uint8_t* s = static_cast<const uint8_t*>(lr.pBits);
    uint8_t* d = static_cast<uint8_t*>(ms.pData);
    const size_t rowBytes = (size_t)g_srcW * 4;
    // One memcpy when both pitches are tight, which they usually are. 1440 separate row copies
    // per frame is pure call overhead when the memory is contiguous on both sides.
    if ((size_t)lr.Pitch == rowBytes && (size_t)ms.RowPitch == rowBytes) {
        memcpy(d, s, rowBytes * g_srcH);
    } else {
        for (UINT y = 0; y < g_srcH; ++y)
            memcpy(d + (size_t)y * ms.RowPitch, s + (size_t)y * lr.Pitch, rowBytes);
    }
    MeasureCullBand(s, lr.Pitch, g_srcW, g_srcH);
    g_ctx11->Unmap(g_uploadTex, 0);
    use.surf->UnlockRect();
    g_msLockCopy += MsSince(t0);
    ++g_copyFrames;

    use.pending = false;
    g_frameSrc11 = g_uploadTex;
    return &use;
}

const float kRadToUU = 65536.0f / 6.2831853f;
int   g_yawSign = -1, g_pitchSign = 1;
bool  g_haveCentre = false;
float g_centreYaw = 0.0f;
int32_t g_baseYaw = 0;
int   g_tick = 0;

void UpdateFromHeadset(IDirect3DDevice9* gameDev) {
    if (!g_xrReady) return;
    PumpXR();
    if (!g_xrRunning) return;

    XrFrameState fs{ XR_TYPE_FRAME_STATE };
    LARGE_INTEGER tWait = Now();
    if (XR_FAILED(xrWaitFrame(g_xrSession, nullptr, &fs))) return;
    g_msWaitFrame += MsSince(tWait);
    ++g_waitFrames;
    // The pacing target, measured rather than assumed. A frame time that sits at an exact
    // multiple of this is the signature of missing the deadline and waiting out a whole extra
    // display period - a very different problem from being uniformly slow, and the fix for it
    // is to get under the period rather than to shave a few percent off anything.
    if (g_displayPeriodMs == 0.0 && fs.predictedDisplayPeriod > 0) {
        g_displayPeriodMs = double(fs.predictedDisplayPeriod) / 1.0e6;   // ns -> ms
        Log("headset display period %.2f ms (%.1f Hz) - frame times at 2x this are a missed deadline",
            g_displayPeriodMs, 1000.0 / g_displayPeriodMs);
    }
    xrBeginFrame(g_xrSession, nullptr);

    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t n = 0;
    XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = g_xrSpace;
    XrViewState vs{ XR_TYPE_VIEW_STATE };
    if (XR_SUCCEEDED(xrLocateViews(g_xrSession, &vli, &vs, 2, &n, views)) && n >= 1 &&
        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        // keep the per-eye poses/FOVs for the projection layer submitted below
        for (uint32_t i = 0; i < n && i < 2; ++i) { g_eyePose[i] = views[i].pose; g_eyeFov[i] = views[i].fov; }
        g_haveEyes = (n >= 2);
        g_headFovDeg = (fabsf(views[0].fov.angleLeft) + fabsf(views[0].fov.angleRight)) * 57.2957795f;

        const XrQuaternionf& q = views[0].pose.orientation;
        float sinp = 2.0f * (q.w * q.x - q.y * q.z);
        if (sinp > 1.0f) sinp = 1.0f;
        if (sinp < -1.0f) sinp = -1.0f;
        float pitchRad = asinf(sinp);
        float yawRad = atan2f(2.0f * (q.w * q.y + q.z * q.x),
                              1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        // Anchor to the GAME's current yaw, not our own previous output. Seeding from
        // g_wantYaw (initially 0) made enabling snap the view to world yaw 0.
        if (!g_haveCentre) {
            g_centreYaw = yawRad;
            uintptr_t ctl = FindController();
            g_baseYaw = (ctl && Readable((void*)(ctl + ACTOR_ROTATION), 12))
                        ? reinterpret_cast<int32_t*>(ctl + ACTOR_ROTATION)[1] : 0;
            // Midpoint of the eyes, so neither stereo eye starts biased against the centre.
            const bool haveBoth = (n >= 2);
            g_centrePos[0] = haveBoth ? (views[0].pose.position.x + views[1].pose.position.x) * 0.5f
                                      : views[0].pose.position.x;
            g_centrePos[1] = haveBoth ? (views[0].pose.position.y + views[1].pose.position.y) * 0.5f
                                      : views[0].pose.position.y;
            g_centrePos[2] = haveBoth ? (views[0].pose.position.z + views[1].pose.position.z) * 0.5f
                                      : views[0].pose.position.z;
            g_haveCentrePos = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
            g_haveCentre = true;
            Log("recentred: head yaw %.1f, game yaw %d, head pos (%.3f, %.3f, %.3f)%s",
                yawRad * 57.2958f, g_baseYaw,
                g_centrePos[0], g_centrePos[1], g_centrePos[2],
                g_haveCentrePos ? "" : " [POSITION NOT TRACKED]");
        }

        // ---- headset position -> game-world offset ----
        //
        // The mapping is fixed at RECENTRE, not taken from where the camera points now. That
        // distinction is the whole correctness argument: an XR position is expressed in the
        // XR world frame, so turning the head does not change it. If the offset were mapped
        // through the camera's current facing, rotating the head would swing a stationary
        // body offset around the world - leaning right, then turning, would send the view
        // somewhere it should not be.
        //
        // XR is Y-up, right-handed, forward = -Z, metres. The game is Z-up, X forward,
        // Y right, UU. So decompose the XR offset onto the head's centre axes, then rebuild
        // it on the game's axes at the yaw the game had when we recentred.
        // Positional tracking can drop while orientation keeps working - the IMU carries
        // rotation, but position needs the cameras. Run 3 ended with the offset frozen at
        // (29.8, -30.8, -27.7) UU, repeated identically frame after frame: roughly 0.9 m of
        // permanent displacement with no way back. Freezing is the wrong failure mode, so
        // decay to neutral instead. The view drifts home over about a second and recovers by
        // itself when tracking returns.
        if (!(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
            static bool warned = false;
            if (!warned && InterlockedCompareExchange(&g_sixDof, 0, 0)) {
                Log("positional tracking LOST - decaying the 6-DOF offset to neutral");
                warned = true;
            }
            for (int i = 0; i < 3; ++i) g_hmdOffset[i] *= 0.94f;
        }
        else if (g_haveCentrePos) {
            // An eye offset IS a head position offset, so stereo needs no separate
            // interpupillary maths: map the position of whichever eye the NEXT frame will be
            // drawn for, and the separation falls out of the same transform 6-DOF uses. In
            // mono, use the midpoint of the two eyes so the view is not biased half an IPD to
            // the left.
            const bool stereoNow = InterlockedCompareExchange(&g_stereo, 0, 0) != 0;
            const bool dupNow = InterlockedCompareExchange(&g_dupDraws, 0, 0) != 0;
            const int nextEye = stereoNow ? (1 - g_renderEye) : 0;
            XrVector3f hp = views[nextEye].pose.position;
            // Duplication draws the LEFT eye through the constant hook and derives the right
            // from it, so the base offset must be eye 0 exactly - not the midpoint.
            if (!stereoNow && !dupNow && n >= 2) {
                hp.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
                hp.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
                hp.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
            }
            // Remember where this eye's upcoming image will have been rendered from, so it can
            // be submitted with its true pose next frame even though by then it is stale.
            g_renderPose[nextEye] = views[nextEye].pose;

            float ox = hp.x - g_centrePos[0];
            float oy = hp.y - g_centrePos[1];
            float oz = hp.z - g_centrePos[2];

            const float sc = sinf(g_centreYaw), cc = cosf(g_centreYaw);
            float fwdAmt   = -(ox * sc) - (oz * cc);   // along the head's centre forward
            float rightAmt =  (ox * cc) - (oz * sc);   // along the head's centre right
            float upAmt    =  oy;

            const float baseYawRad = g_baseYaw * (6.2831853f / 65536.0f);
            const float bc = cosf(baseYawRad), bs = sinf(baseYawRad);
            g_hmdOffset[0] = (bc * fwdAmt - bs * rightAmt) * kMetresToUU;
            g_hmdOffset[1] = (bs * fwdAmt + bc * rightAmt) * kMetresToUU;
            g_hmdOffset[2] = upAmt * kMetresToUU;

            // Left eye -> right eye, through the identical transform. The mapping is linear,
            // so it can be applied to the separation vector directly; interpupillary distance
            // still never needs naming.
            if (n >= 2) {
                float dx = views[1].pose.position.x - views[0].pose.position.x;
                float dy = views[1].pose.position.y - views[0].pose.position.y;
                float dz = views[1].pose.position.z - views[0].pose.position.z;
                float dFwd   = -(dx * sc) - (dz * cc);
                float dRight =  (dx * cc) - (dz * sc);
                g_eyeDeltaUU[0] = (bc * dFwd - bs * dRight) * kMetresToUU;
                g_eyeDeltaUU[1] = (bs * dFwd + bc * dRight) * kMetresToUU;
                g_eyeDeltaUU[2] = dy * kMetresToUU;
            }
        }
        float dYaw = yawRad - g_centreYaw;
        while (dYaw >  3.14159265f) dYaw -= 6.2831853f;
        while (dYaw < -3.14159265f) dYaw += 6.2831853f;

        int32_t p = (int32_t)(g_pitchSign * pitchRad * kRadToUU);
        if (p >  16000) p =  16000;
        if (p < -16000) p = -16000;
        g_wantPitch = p;
        g_wantYaw   = g_baseYaw + (int32_t)(g_yawSign * dYaw * kRadToUU);
        g_haveWant  = true;

        if (++g_tick % 180 == 0)
            Log("head pitch %.1f yaw %.1f -> want pitch %d yaw %d (hook hits %d)",
                pitchRad*57.2958f, yawRad*57.2958f, g_wantPitch, g_wantYaw, g_hookHits);
    }

    // ---- copy the game's frame into the swapchain and submit it ----
    XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView projViews[2]{};
    const XrCompositionLayerBaseHeader* layers[1] = { nullptr };
    uint32_t layerCount = 0;

    const CopySlot* frame = (fs.shouldRender && g_haveEyes && gameDev)
                          ? CopyBackbufferToD3D11(gameDev) : nullptr;

    if (frame && EnsureSwapchain(g_srcW, g_srcH)) {
        // Every one of these was captured with the pixels, not read live. In pipelined mode the
        // image is a frame old, and reading `g_stereo` or `g_eyePose` here would describe the
        // frame being drawn now rather than the one being submitted.
        const bool stereo = frame->stereo;
        const int filled = frame->eye;

        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_swapchain[filled], &ai, &idx))) {
            XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = XR_INFINITE_DURATION;
            if (XR_SUCCEEDED(xrWaitSwapchainImage(g_swapchain[filled], &wi))) {
                // Either the CPU-uploaded staging texture or, under zero-copy, the shared surface
                // the GPU already wrote - both are D3D11 textures of a compatible type group, so
                // the copy into the swapchain image is identical either way.
                LARGE_INTEGER tUp = Now();
                if (g_frameSrc11) g_ctx11->CopyResource(g_scImages[filled][idx], g_frameSrc11);
                g_msUpload += MsSince(tUp);
                XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(g_swapchain[filled], &ri);
                g_eyeFilled[filled] = true;
                if (!stereo) { g_renderPose[0] = frame->pose[0]; g_renderPose[1] = frame->pose[1]; }

                // Both eyes reference the SAME image - this is the MonoTracked rung, not
                // stereo. It reads as depth-free but correctly oriented, because the game
                // already rendered from wherever the head was pointing.
                // Submit the frustum the game ACTUALLY rendered, not the headset's. Claiming
                // the headset's FOV for an image drawn with a different one stretches
                // everything uniformly, which is precisely what made scale unjudgeable.
                // Submit the frustum we FORCED, not the one we observed. Following the
                // observation was the flicker: the engine's FOV drifts between our writes, so
                // a submitted frustum that tracked it inherited every wobble. Forcing the
                // matrix and submitting the same constant means the two agree by construction.
                // In mono both eyes reference the one filled swapchain, as before. In stereo
                // each eye gets its own, submitted with the pose its image was actually
                // rendered from - which for the eye not drawn this frame is a frame old. That
                // stale pose is the honest one: telling the compositor where the image really
                // came from lets it reproject correctly instead of mismatching the two eyes.
                const bool dupNow = frame->dup;
                for (int i = 0; i < 2; ++i) {
                    const int src = stereo ? i : 0;
                    projViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                    projViews[i].pose = frame->pose[i];
                    projViews[i].fov  = frame->useDerived ? frame->derived : frame->fov[i];
                    projViews[i].subImage.swapchain = g_swapchain[src];
                    if (dupNow) {
                        // Both eyes live in one image, side by side. Hand each eye its half by
                        // rect - no extra copy, and both were drawn in the same instant.
                        const int32_t halfW = (int32_t)g_scWidth / 2;
                        const bool swapEyes = InterlockedCompareExchange(&g_swapEyes, 0, 0) != 0;
                        // Named `half`, not `src`: it shadowed the enclosing `src` (which selects
                        // the SWAPCHAIN) with a different meaning (which HALF of the frame), and
                        // both are live in this block. That is how an eye-swap bug gets written.
                        const int32_t half = swapEyes ? (1 - i) : i;
                        projViews[i].subImage.imageRect.offset = { half * halfW, 0 };
                        projViews[i].subImage.imageRect.extent = { halfW, (int32_t)g_scHeight };
                    } else {
                        projViews[i].subImage.imageRect.offset = { 0, 0 };
                        projViews[i].subImage.imageRect.extent = { (int32_t)g_scWidth, (int32_t)g_scHeight };
                    }
                }
                // Nothing is submitted until BOTH eyes hold an image, otherwise the first
                // stereo frame would show one eye a swapchain that has never been written.
                if (!stereo || (g_eyeFilled[0] && g_eyeFilled[1])) {
                    layer.space = g_xrSpace;
                    layer.viewCount = 2;
                    layer.views = projViews;
                    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
                    layerCount = 1;
                    if (++g_framesSubmitted == 1) Log("*** FIRST FRAME SUBMITTED TO THE HEADSET ***");
                }
            } else {
                XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(g_swapchain[filled], &ri);
            }
        }
    }

    // Hand the next frame to the other eye. Done after submission so the block above still
    // refers to the eye the just-finished frame was drawn for.
    if (InterlockedCompareExchange(&g_stereo, 0, 0)) g_renderEye = 1 - g_renderEye;

    XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount;
    fei.layers = layerCount ? layers : nullptr;
    xrEndFrame(g_xrSession, &fei);
}

// ---------------------------------------------------------------- d3d9 plumbing
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
PFN_Present g_origPresent = nullptr;
volatile LONG g_initTried = 0;

// ---------------------------------------------------------------- SetVertexShaderConstantF
// Vtable slot 94. This is a very hot call - thousands per frame - so the disarmed path must
// cost two interlocked reads and a jump, nothing more.
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetVSConstF)(IDirect3DDevice9*, UINT, const float*, UINT);
PFN_SetVSConstF g_origSetVSConstF = nullptr;

// Large uploads (skinning palettes and the like) run to hundreds of registers. Scanning every
// window in those would cost more than it is worth and they are not view matrices anyway, so
// cap how far into a single call we look.
const UINT kMaxScanWindows = 32;
const UINT kScratchVecs    = 256;   // vs_3_0's full float-constant file

HRESULT STDMETHODCALLTYPE Hook_SetVSConstF(IDirect3DDevice9* dev, UINT startReg,
                                           const float* data, UINT vec4Count) {
    bool scan = InterlockedCompareExchange(&g_scanArmed, 0, 0) != 0;
    // Duplication needs the final left-eye matrix cached, which only the modify path produces.
    const bool dup = InterlockedCompareExchange(&g_dupDraws, 0, 0) != 0;
    // Stereo and 6-DOF share one path: g_hmdOffset is the mapped position of the eye this
    // frame belongs to, which already contains both the head's movement and the eye's own
    // displacement. They cannot be separated here, so stereo necessarily brings positional
    // head tracking with it - which is the behaviour we want anyway.
    //
    // Draw duplication belongs in this list too, and its absence was a real asymmetry: the left
    // eye is produced HERE and the right by adding g_eyeDeltaUU in StereoPair, so leaving this
    // off put the left eye at the engine's own camera and the right at +IPD. The pair was biased
    // half an interpupillary distance, and head translation did nothing under F1 unless F10
    // happened to be on as well.
    const bool posOffset = InterlockedCompareExchange(&g_sixDof, 0, 0) != 0 ||
                           InterlockedCompareExchange(&g_stereo, 0, 0) != 0 || dup;
    const bool inject = posOffset || InterlockedCompareExchange(&g_injectOn, 0, 0) != 0;

    // Nothing below may touch `data` before this. The verify block used to dereference it three
    // lines above the null check that guards it.
    if (!data || vec4Count < 4) return g_origSetVSConstF(dev, startReg, data, vec4Count);

    // Cheap always-on capture of the projection the game is really using. Once the matrix has
    // been located we know exactly which register to look at, so this is a bounds check on
    // most calls and a single window test on the few that carry it - affordable every frame,
    // unlike the full scan. Verified per call rather than trusted, because the same register
    // is reused by other passes.
    const bool coversLock = g_haveLock && startReg <= g_lockedReg &&
                            g_lockedReg + 4 <= startReg + vec4Count;
    bool  lockOk = false;
    float lockTanX = 0.0f, lockTanY = 0.0f;
    if (coversLock) {
        const Reg4* r = reinterpret_cast<const Reg4*>(data + (g_lockedReg - startReg) * 4);
        float tx = 0, ty = 0, dot = -2.0f;
        ++g_lockUploads;
        lockOk = TrackedViewMatrix(r, g_lockedConv, &tx, &ty, &dot);
        if (lockOk) {
            lockTanX = tx; lockTanY = ty;
            g_projTanX = tx; g_projTanY = ty;
            g_lockVerified = true;
            if (g_projTanX < g_capTanMin) g_capTanMin = g_projTanX;
            if (g_projTanX > g_capTanMax) g_capTanMax = g_projTanX;
            if (g_projTanY < g_capTanYMin) g_capTanYMin = g_projTanY;
        } else {
            // Something else has taken this register - a shadow or post pass. Draws issued now
            // are not scene geometry and must not be duplicated.
            ++g_lockRejects;
            if (dot > g_bestRejectDot) g_bestRejectDot = dot;
            g_camMatrixBound = false;
        }
    }
    const bool forceProj = InterlockedCompareExchange(&g_vrProjection, 0, 0) != 0 &&
                           g_targetTanX > 0.0f && g_targetTanY > 0.0f;
    const bool modify = inject || forceProj || dup;

    // The SCAN genuinely needs a live camera position - its world-position probes are meaningless
    // without one, and it is the only thing here that reads g_camPos at all.
    //
    // The MODIFY path does not, and gating it on the same flag is what turned a camera-pointer
    // miss - retried only every 30 frames - into up to half a second in which no constant was
    // examined, no slot was invalidated, and StereoPair went on re-uploading a frozen matrix over
    // live engine state. Every test the modify path applies is now either pure matrix content or
    // a loose facing check that is allowed to be stale, so it runs regardless.
    if (!InterlockedCompareExchange(&g_camPosValid, 0, 0)) scan = false;
    if (!scan && !modify) return g_origSetVSConstF(dev, startReg, data, vec4Count);

    if (scan) {
        const float* const pts[3] = { g_camPos, g_povPos, g_zeroPos };
        const UINT windows = (vec4Count - 4 < kMaxScanWindows) ? vec4Count - 4 : kMaxScanWindows;
        for (UINT j = 0; j <= windows; ++j) {
            const Reg4* r = reinterpret_cast<const Reg4*>(data + j * 4);
            for (int c = 0; c < 2; ++c) {
                float w = 0, len = 0, dot = 0; int pt = 0; bool shape = false;
                if (!TestWindow(r, c, pts, &w, &len, &pt, &dot, &shape)) continue;
                // Everything inside the w tolerance is recorded, passing or not, so the report
                // can show WHY the rejects were rejected rather than hiding them.
                if (fabsf(w) <= kHitTolUU) RecordHit(startReg + j, c, pt, w, len, dot, shape);
                else                       NoteNearMiss(startReg + j, c, w, len);
            }
        }
    }

    if (modify) {
        // Copy-on-match: the same register slot is reused by shadow and post passes with
        // completely different matrices, so trusting a register number would corrupt those.
        // Re-testing each call scopes the edit to the block the camera actually sits on.
        //
        // A world-space offset is the same vector in translated-world space - the translation
        // moves the origin, not the axes - so one injection path covers both cases.
        // vs_3_0 tops out at 256 float constants, so a legitimate call always fits. Bail
        // rather than forward a truncated copy if that assumption ever breaks - dropping
        // constants would corrupt the frame in a way that looks like a detection failure.
        static thread_local float scratch[kScratchVecs * 4];
        const UINT copyVecs = vec4Count;
        if (copyVecs > kScratchVecs) return g_origSetVSConstF(dev, startReg, data, vec4Count);

        // ---- every exit from here on must leave the slot honest ----
        //
        // Does this upload touch the tracked window at all, and does it COVER it?
        const bool touchesLock = g_haveLock && startReg < g_lockedReg + 4 &&
                                 g_lockedReg < startReg + vec4Count;
        if (!coversLock) {
            // A partial overlap leaves the tracked block holding a mixture of what the engine
            // just wrote and what we cached; re-uploading our copy over that in StereoPair would
            // clobber the engine's own constants. Having no lock at all is the same situation
            // with a wider blast radius. Both used to return here silently, leaving the slot set.
            if (dup && (touchesLock || !g_haveLock)) InvalidateCamMats();
            return g_origSetVSConstF(dev, startReg, data, vec4Count);
        }
        if (!lockOk) {
            // The register now holds something that is not the scene view-projection - a shadow
            // or post pass, or a frame where identification lapsed. Either way, pushing the
            // cached copy over it for the rest of the frame is exactly the frozen-camera failure.
            if (dup) InvalidateCamMats();
            return g_origSetVSConstF(dev, startReg, data, vec4Count);
        }

        // 6-DOF wins when both are on: it is the real thing, the F3 constant is only a probe.
        // Otherwise offset along the camera's RIGHT axis, not a world axis - that is the
        // actual stereo primitive, and it keeps the effect the same whichever way the player
        // happens to be facing.
        float o[3] = {0,0,0};
        if (inject) {
            if (posOffset) { o[0] = g_hmdOffset[0]; o[1] = g_hmdOffset[1]; o[2] = g_hmdOffset[2]; }
            else           { o[0] = g_camRight[0] * g_offsetUU;
                             o[1] = g_camRight[1] * g_offsetUU;
                             o[2] = g_camRight[2] * g_offsetUU; }
        }

        // REVERTED to locked-register-only (run 20). Scanning every window was introduced on the
        // theory that characters used a second register; the diagnostic then measured **1 register,
        // ever**, so it bought nothing and cost plenty:
        //
        //   * frame rate fell from 37 to 28 fps median - a full scan of up to 32 windows against 3
        //     probe points, on every constant upload, is not free, and
        //   * it corrupted the MAIN MENU. The origin probe needs no valid camera position, so with
        //     more windows examined, menu and UI matrices got spurious matches and were mangled.
        //     Menus were fine before this change and broke with it.
        //
        // The lesson worth keeping: a wider net is not free when the test can produce false
        // positives, and this test can - the origin probe cannot tell a UI matrix from a view one.
        const UINT j = g_lockedReg - startReg;
        const int  c = g_lockedConv;

        ++g_injCount;
        memcpy(scratch, data, copyVecs * 4 * sizeof(float));
        Reg4* m = reinterpret_cast<Reg4*>(scratch + j * 4);
        if (inject) ApplyOffset(m, c, o);
        if (forceProj) {
            // lockTanX/Y were measured from the arriving matrix by the verify block above, so the
            // two paths cannot disagree about what the engine sent.
            ApplyProjection(m, c, lockTanX, lockTanY, g_targetTanX, g_targetTanY);
            // Measure the RESULT, not just the input. The observed-FOV capture at the top of
            // this function reads the engine's matrix before we touch it, so it reports the
            // engine's drift and says nothing about whether our forcing arithmetic is right.
            // Reading it back after the edit closes that loop.
            ProjTangents(m, c, &g_forcedTanX, &g_forcedTanY);
        }
        // END: collapse everything this register drives, so whatever survives on screen is
        // geometry we do not remap. Applied before the cache below, so the draw hook's per-eye
        // copies inherit it too. See the note by g_blankCamMat.
        if (InterlockedCompareExchange(&g_blankCamMat, 0, 0)) {
            // CLIP it out, do not collapse it. Run 29 scaled the x/y columns down by 1000, which
            // piles every triangle in the world onto a few pixels at the centre of the frame -
            // catastrophic overdraw, which bloom and tonemapping then smear across everything.
            // The result was a rainbow with nothing legible in it, so the test destroyed its own
            // readability and the run was wasted.
            //
            // Forcing clip.w negative makes the hardware discard the geometry before
            // rasterisation instead. Nothing is drawn, nothing is overdrawn, and post-processing
            // has an ordinary near-empty frame to work on.
            //
            // For a zeroed block clip.w reduces to r[3].w under BOTH conventions - ROW sums
            // v_i*r[i].w + r[3].w, COL takes dot(r[3].xyz, v) + r[3].w - so one assignment covers
            // the pair, and every vertex fails the -w <= x <= w test.
            float* f = reinterpret_cast<float*>(m);
            for (int k = 0; k < 16; ++k) f[k] = 0.0f;
            f[15] = -1.0f;
        }
        // Record the finished left-eye matrix. The draw hook derives both eyes from it, so the
        // eye offset and the half-frame remap compose on top of the forced projection instead of
        // fighting it.
        if (dup) {
            int slot = -1;
            for (int s = 0; s < g_camMatUsed; ++s)
                if (g_camMats[s].reg == g_lockedReg) { slot = s; break; }
            if (slot < 0 && g_camMatUsed < kMaxCamMats) slot = g_camMatUsed++;
            // Slots are only ever allocated per distinct locked register, so exhausting eight of
            // them takes eight rescans onto different registers - remote, but the failure mode is
            // the worst one there is: nothing gets cached, StereoPair sees nSlots == 0 for every
            // draw, and stereo silently switches itself off. Reuse a dead slot instead.
            if (slot < 0)
                for (int s = 0; s < g_camMatUsed; ++s)
                    if (!g_camMats[s].valid) { slot = s; break; }
            if (slot >= 0) {
                g_camMats[slot].reg  = g_lockedReg;
                g_camMats[slot].conv = c;
                g_camMats[slot].valid = true;
                memcpy(g_camMats[slot].m, m, sizeof(g_camMats[slot].m));
            }
            g_camMatrixBound = true;
        }
        return g_origSetVSConstF(dev, startReg, scratch, copyVecs);
    }

    return g_origSetVSConstF(dev, startReg, data, vec4Count);
}

// ---------------------------------------------------------------- draw-call duplication
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawPrim)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawIndexedPrim)(IDirect3DDevice9*, D3DPRIMITIVETYPE,
                                                         INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawPrimUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT,
                                                    const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawIndexedPrimUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE,
                                                           UINT, UINT, UINT, const void*,
                                                           D3DFORMAT, const void*, UINT);
PFN_DrawPrim           g_origDrawPrim = nullptr;
PFN_DrawIndexedPrim    g_origDrawIndexedPrim = nullptr;
PFN_DrawPrimUP         g_origDrawPrimUP = nullptr;
PFN_DrawIndexedPrimUP  g_origDrawIndexedPrimUP = nullptr;
int g_drawsUP = 0;           // how many draws came through the user-pointer forms
bool g_hookUserPtrDraws = false;  // ini-switchable; see LoadIniSettings for why it must be
int  g_userPtrHookLevel = 0;      // 0 off, 1 patch only, 2 +counter, 3 full - a bisection ladder

// ---- only split draws aimed at the SCENE, never at offscreen targets (run 9) ----
//
// Run 9's decisive clue: with both halves drawn, distant building textures drop out and return
// when you look away and back; with only ONE half drawn (INSERT) they are mostly stable. So the
// damage comes from doing two draws, not from the matrices or the eye mapping - which the same
// run confirmed is correct (left half -> left eye).
//
// The cause is that the run-7 fix went too far. Splitting EVERY draw also split draws aimed at
// render targets that have nothing to do with the eyes: shadow maps above all. Rendering the
// shadow scene twice into two halves of a shadow map leaves it holding two squashed copies,
// each covering half its width. Objects then sample garbage shadow data, which reads exactly as
// distant surfaces going dark and recovering when the view changes and the cascade is rebuilt.
// One half is less wrong than two, hence "mostly stable" with INSERT.
//
// Fix: track the bound render target and only split when it is scene-sized. Shadow maps,
// reflection buffers and half-res post buffers all differ in size and are left alone.
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
PFN_SetRenderTarget g_origSetRenderTarget = nullptr;
volatile LONG g_rtIsScene = 1;
int g_splitColourOnly = 0;    // ini SplitColourTargetsOnly - see the note in Hook_SetRenderTarget
// One-shot budget for the shadow-pass constant dump. Only fires while SplitColourTargetsOnly=1,
// because that is what routes those draws down the excluded path where they can be caught.
volatile LONG g_dumpShadowConsts = 0;

// ---- render-target census (run 11) ----
//
// The size test above caught only 11 draws of ~1640, so shadow maps are NOT being separated by
// size - either this build allocates them at scene resolution (UE3 does that for whole-scene
// dominant shadows) or they never pass through SetRenderTarget(0) the way assumed. Either way
// the theory was wrong, and rather than guess a fourth time, enumerate: every distinct target,
// its size and format, and how many draws land on it. That says exactly what is being split.
//
// ---- and why it now keys on the SURFACE, not on (width, height, format) (run 27) ----
//
// The census merged every target sharing a descriptor into one row, so it could not distinguish
// one scene colour target from several distinct surfaces of identical size and format. That is
// exactly the case it needed to resolve: g_rtIsScene splits on size equality alone, so any
// full-resolution offscreen target - a post ping-pong buffer, or the whole-scene dominant shadow
// map UE3 allocates at scene resolution - is being split as if it were the scene, and the census
// as written could never show it. Two 2560x1440 A8R8G8B8 surfaces read as one row of ~27000
// draws, and run 11's conclusion that shadow maps "are not being separated by size" rested on it.
//
// Keying on the pointer can double-count if a surface is released and its address reused. For a
// once-a-second census that is acceptable; the row simply appears twice, which is far less
// misleading than two different targets appearing as one.
struct RtInfo { IDirect3DSurface9* surf; UINT w, h; D3DFORMAT fmt; int draws; };
RtInfo g_rtSeen[16]{};
int    g_rtSeenCount = 0;
int    g_rtCurrent = -1;

const char* FmtName(D3DFORMAT f) {
    switch (f) {
        case D3DFMT_A8R8G8B8:   return "A8R8G8B8";
        case D3DFMT_X8R8G8B8:   return "X8R8G8B8";
        case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
        case D3DFMT_R32F:       return "R32F";
        case D3DFMT_G16R16F:    return "G16R16F";
        case D3DFMT_A2B10G10R10: return "A2B10G10R10";
        case D3DFMT_D24S8:      return "D24S8";
        case D3DFMT_D16:        return "D16";
        case D3DFMT_L8:         return "L8";
        case D3DFMT_A8:         return "A8";
        case D3DFMT_G16R16:     return "G16R16";
        case D3DFMT_A16B16G16R16: return "A16B16G16R16";
        case D3DFMT_R16F:       return "R16F";
        case D3DFMT_G32R32F:    return "G32R32F";
        case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
        case D3DFMT_A8B8G8R8:   return "A8B8G8R8";
        case D3DFMT_R5G6B5:     return "R5G6B5";
        case D3DFMT_D24X8:      return "D24X8";
        case D3DFMT_D32:        return "D32";
        default: {
            // "other" was hiding the identity of a scene-sized surface being split 240 times a
            // frame - the leading suspect for the one-eye shadow corruption. A name we cannot
            // read is not a diagnostic. FOURCC formats print as their four characters, everything
            // else as its numeric D3DFORMAT.
            static char buf[32];
            const DWORD v = (DWORD)f;
            if (v > 0x20202020) {
                sprintf_s(buf, "FOURCC '%c%c%c%c'",
                          (char)(v & 0xFF), (char)((v >> 8) & 0xFF),
                          (char)((v >> 16) & 0xFF), (char)((v >> 24) & 0xFF));
            } else {
                sprintf_s(buf, "fmt#%lu", (unsigned long)v);
            }
            return buf;
        }
    }
}

HRESULT STDMETHODCALLTYPE Hook_SetRenderTarget(IDirect3DDevice9* dev, DWORD idx,
                                              IDirect3DSurface9* surf) {
    HRESULT hr = g_origSetRenderTarget(dev, idx, surf);
    if (idx == 0) {
        // Before the backbuffer size is known, assume scene - the initial target IS the
        // backbuffer, and refusing to split would silently disable stereo instead of failing
        // in a way anyone would notice.
        bool scene = true;
        g_rtCurrent = -1;
        if (surf) {
            D3DSURFACE_DESC d{};
            if (SUCCEEDED(surf->GetDesc(&d))) {
                if (g_srcW && g_srcH) scene = (d.Width == g_srcW && d.Height == g_srcH);
                // ---- the discriminator for the one-eye shadow corruption (run 40) ----
                //
                // The scene test above is SIZE EQUALITY ALONE, so every scene-sized surface gets
                // split - and the census has been flagging a THIRD one for several runs, taking
                // 240 draws a frame, that is neither of the two colour targets it should be.
                //
                // A screen-space effect that reconstructs world position from depth does so with
                // an inverse-projection constant we do NOT remap. Feed it depth from geometry we
                // DID remap and the reconstruction lands somewhere else entirely - differently per
                // eye, since each eye's remap differs. That is exactly "the shadow appears in one
                // eye and is wrong in that eye".
                //
                // This restricts splitting to the two known scene COLOUR formats, which excludes
                // that third surface. If the shadows change, it is implicated; if they do not, the
                // cause is elsewhere and this costs one relaunch to find out.
                if (scene && g_splitColourOnly &&
                    !(d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8 ||
                      d.Format == D3DFMT_A16B16G16R16F)) {
                    scene = false;
                }
                for (int i = 0; i < g_rtSeenCount; ++i)
                    if (g_rtSeen[i].surf == surf) { g_rtCurrent = i; break; }
                if (g_rtCurrent < 0 && g_rtSeenCount < 16) {
                    g_rtSeen[g_rtSeenCount] = { surf, d.Width, d.Height, d.Format, 0 };
                    g_rtCurrent = g_rtSeenCount++;
                }
            }
        }
        InterlockedExchange(&g_rtIsScene, scene ? 1 : 0);
    }
    return hr;
}

void ReportRenderTargets() {
    Log("render-target census (draws this second, scene is %ux%u; one row per SURFACE):",
        g_srcW, g_srcH);
    int sceneSurfaces = 0;
    for (int i = 0; i < g_rtSeenCount; ++i) {
        if (!g_rtSeen[i].draws) continue;
        // ---- ⚠️ this label used to lie, and it lied about the exact surface under suspicion ----
        //
        // It was computed from size equality alone, so with SplitColourTargetsOnly=1 the G16R16
        // target still printed "<- SPLIT" while it was in fact being left alone. A census that
        // reports the rule instead of the decision is worse than no census: run 40 nearly
        // concluded the switch had not worked from this line.
        const bool sceneSized = (g_rtSeen[i].w == g_srcW && g_rtSeen[i].h == g_srcH);
        const bool colourFmt = (g_rtSeen[i].fmt == D3DFMT_A8R8G8B8 ||
                                g_rtSeen[i].fmt == D3DFMT_X8R8G8B8 ||
                                g_rtSeen[i].fmt == D3DFMT_A16B16G16R16F);
        const bool isScene = sceneSized && (!g_splitColourOnly || colourFmt);
        if (sceneSized) ++sceneSurfaces;
        Log("    %p  %5ux%-5u %-14s %6d draws  %s", (void*)g_rtSeen[i].surf,
            g_rtSeen[i].w, g_rtSeen[i].h,
            FmtName(g_rtSeen[i].fmt), g_rtSeen[i].draws,
            isScene ? "<- SPLIT" : (sceneSized ? "<- scene-sized, EXCLUDED by ini" : "left alone"));
        g_rtSeen[i].draws = 0;
    }
    // The scene test is size equality, so every one of these is being split. Two is expected
    // (LDR + HDR scene colour). More than that means something which is not scene colour is
    // being split as if it were - which is the shadow-map corruption theory, now visible.
    if (sceneSurfaces > 2)
        Log("    ^ %d distinct SCENE-SIZED surfaces are being split. Only the LDR and HDR scene"
            " colour targets should be - check what the extras are.", sceneSurfaces);
}

// Issue one scene draw twice: left half of the viewport, then right. When the draw's matrix
// is confirmed as the tracked camera, the right half gets it shifted by the eye offset - true
// parallax. Otherwise the SAME unmodified state is used for both halves.
//
// Run 7 found the bug in getting this backwards. The original version skipped the whole split
// for any draw whose matrix didn't pass the camera identity check (decals, per-object
// transforms, screen-space effects), drawing it ONCE at full un-split width. Since the
// finished backbuffer is sliced in half at submission - left half to the left eye, right half
// to the right - a full-width draw only survives the crop in whichever half it happened to
// land in. That one mechanism produced every symptom reported: geometry present in only one
// eye, an ammo pickup flickering as its alignment crossed the detection threshold while the
// head tilted, a manhole decal missing outright, and a screen-space effect that never got a
// second offset copy reading as sliding with the view instead of staying put.
//
// The fix is to stop gating the SPLIT on camera identification at all - split unconditionally
// whenever duplication is on, and use identification only to decide whether the right half
// gets an offset or an exact copy. A flat, non-parallaxed duplicate in both eyes is a small
// visual compromise; a duplicate missing from one eye outright is not.
//
// ---- run 11 rework: clip-space remap + SCISSOR, never the viewport ----
//
// The viewport is now left exactly as the game set it, because changing it was silently breaking
// every screen-space-sampling shader (see ApplyEyeRemap for the full argument). Each eye is
// placed in its half by remapping clip space in the matrix instead, which keeps the shader's
// NDC->UV mapping and the hardware's NDC->pixel mapping in agreement.
//
// The scissor rectangle does the clipping the halved viewport used to do. Crucially, scissor
// discards pixels WITHOUT altering the NDC->pixel transform, so it costs nothing in shader
// correctness - it just stops draws we could not remap (their transform comes from a register we
// do not track) from spilling across the seam into the other eye.
template <typename DrawFn>
HRESULT StereoPair(IDirect3DDevice9* dev, DrawFn&& draw) {
    ++g_drawsTotal;
    if (g_rtCurrent >= 0 && g_rtCurrent < 16) ++g_rtSeen[g_rtCurrent].draws;
    if (!InterlockedCompareExchange(&g_dupDraws, 0, 0)) return draw();
    // Never split a draw aimed at a shadow map or other offscreen target - see the note on
    // Hook_SetRenderTarget for what that corrupts.
    // ---- run 41: dump the constants of the shadow pass, now that it is down to ~2 draws ----
    //
    // The G16R16 scene-sized target takes ~240 draws a SECOND - about 2 a frame - and excluding it
    // from splitting moved the artefact from one eye to both, which is exactly what a fullscreen
    // pass does when it stops being remapped. So it is implicated, and the haystack is two draws
    // rather than nineteen hundred.
    //
    // What we are looking for is the constant that maps clip space to a texture coordinate.
    // Stage 4A predicted its signature precisely: derived from the render-target size, so near
    // (0.5, -0.5, 0.5+halfpixel, 0.5+halfpixel). A shader sampling the scene with that constant
    // built for the FULL target reads the wrong half once we remap geometry into halves - which is
    // the whole mechanism. Finding it is the same job the F7 scan did for the view matrix, on a
    // far smaller search space.
    //
    // Read rather than hooked: GetPixelShaderConstantF needs no patch, and this runs a handful of
    // times total.
    if (g_dumpShadowConsts > 0 && InterlockedCompareExchange(&g_rtIsScene, 0, 0) == 0 &&
        // Bound against the ARRAY size, not just g_rtSeenCount - the existing draw counter at the
        // top of this function uses `< 16` for the same reason, and /analyze is right to insist.
        g_rtCurrent >= 0 && g_rtCurrent < 16 &&
        g_rtSeen[g_rtCurrent].w == g_srcW && g_rtSeen[g_rtCurrent].h == g_srcH) {
        --g_dumpShadowConsts;
        Log("--- constants for a draw into the EXCLUDED scene-sized target (%s) ---",
            FmtName(g_rtSeen[g_rtCurrent].fmt));
        // ---- ⚠️ ScreenPositionScaleBias is NOT the bug here, worked through in run 42 ----
        //
        // ps c1 = (0.5, -0.5, 0.50023, 0.50012) is exactly UE3's ScreenPositionScaleBias - the
        // constant Stage 4A predicted, confirmed by arithmetic against 4096x2160. But patching it
        // would BREAK this pass rather than fix it.
        //
        // A fullscreen quad spans NDC x in [-1,+1] across the FULL target. Scissored to the left
        // half, only pixels with NDC x in [-1,0] are written, and UV = ndc*0.5 + 0.500122 puts
        // those at UV [0, 0.5] - the left half of the source. Already self-consistent. The quad is
        // not remapped by c0 (its vertex shader never reads the view matrix), so nothing has moved
        // out from under its UV maths.
        //
        // What IS suspect: this pass reads DEPTH and reconstructs world position from it. The
        // depth was rendered with our REMAPPED projection - clip x halved and shifted per eye -
        // while the reconstruction constant still describes the original full-frame projection.
        // Reconstruct with the wrong inverse and every world position lands somewhere else,
        // differently per eye because the two halves were remapped differently.
        //
        // So: look for a MATRIX, in either shader stage, not another scale/bias pair.
        float c[4];
        for (UINT r = 0; r < 64; ++r) {
            if (FAILED(dev->GetPixelShaderConstantF(r, c, 1))) break;
            if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f) continue;
            const bool scaleBias =
                (fabsf(fabsf(c[0]) - 0.5f) < 0.02f && fabsf(fabsf(c[1]) - 0.5f) < 0.02f) ||
                (fabsf(c[2] - 0.5f) < 0.02f && fabsf(c[3] - 0.5f) < 0.02f);
            Log("    ps c%-2u = (%9.5f, %9.5f, %9.5f, %9.5f)%s", r, c[0], c[1], c[2], c[3],
                scaleBias ? "   <-- screen scale/bias (NOT the bug - see note)" : "");
        }
        // The reconstruction matrix is more likely here: UE3 puts per-view transforms in vertex
        // constants, and our own view matrix already lives at vs c0.
        for (UINT r = 0; r < 96; ++r) {
            if (FAILED(dev->GetVertexShaderConstantF(r, c, 1))) break;
            if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f) continue;
            Log("    vs c%-2u = (%10.4f, %10.4f, %10.4f, %10.4f)", r, c[0], c[1], c[2], c[3]);
        }
    }
    if (!InterlockedCompareExchange(&g_rtIsScene, 0, 0)) { ++g_drawsOffscreen; return draw(); }

    D3DVIEWPORT9 full{};
    if (FAILED(dev->GetViewport(&full)) || full.Width < 4) return draw();

    const LONG dbg = InterlockedCompareExchange(&g_eyeDebug, 0, 0);
    const LONG halfW = (LONG)(full.Width / 2);

    // Collect every register currently holding a camera matrix. Each needs its own per-eye version
    // uploaded, or geometry driven by an untouched register keeps full-frame coordinates and gets
    // scissored away at the seam - the run-18 vanishing-monster bug.
    int slots[kMaxCamMats]; int nSlots = 0;
    if (g_origSetVSConstF)
        for (int s = 0; s < g_camMatUsed && nSlots < kMaxCamMats; ++s)
            if (g_camMats[s].valid) slots[nSlots++] = s;
    if (nSlots > g_camMatValidPeak) g_camMatValidPeak = nSlots;

    // If we cannot remap this draw, do NOT split it either - draw it once, full frame, untouched.
    //
    // This is the flicker, found by elimination in run 22/23. With head tracking off, the chest was
    // visible in alternate-eye mode and gone in duplication mode, which put the cause squarely in
    // this function rather than in rotation, FOV or culling.
    //
    // The bug: splitting was unconditional (made so in run 7, to stop objects vanishing from ONE
    // eye) while the remap is conditional on having a live camera matrix. Under the clip-space
    // scheme those two must travel together. Scissoring geometry we did not remap clips it to a
    // half while it still holds full-frame coordinates - and the centre of the frame IS the seam,
    // so a head-on object is discarded in both halves at once. Off to one side it partly survives.
    //
    // The camera slot goes briefly invalid whenever c0 is written with something that is not a view
    // matrix, which happens repeatedly within a frame, so this fired intermittently - which is
    // exactly what "flickering" was. Falling back to one unsplit draw costs that object its
    // parallax for a frame; it does not cost its existence.
    if (nSlots == 0) { ++g_drawsMonoFallback; return draw(); }

    // ---- "with parallax" has to mean something ----
    //
    // This used to be `const bool remapKnown = true;`, incremented unconditionally a few lines
    // below - so the perf line's `split N (N with parallax, 0 flat)` was a tautology and could
    // never read anything else. Run 16's "906 with parallax, 0 flat - the stereo mechanism is
    // validated" was that constant, not a measurement.
    //
    // What actually decides whether the right half gains real depth is the eye separation vector,
    // which is only non-zero while positional tracking is live. Zero here means both halves are
    // the same image and the result is mono wearing a stereo costume - worth seeing.
    const float ed2 = g_eyeDeltaUU[0]*g_eyeDeltaUU[0] + g_eyeDeltaUU[1]*g_eyeDeltaUU[1] +
                      g_eyeDeltaUU[2]*g_eyeDeltaUU[2];
    const bool haveParallax = ed2 > 0.01f;      // 0.1 UU ~ 2 mm; a real IPD is 3-4 UU

    // Save the scissor state so the game's own setup is restored untouched.
    // HOME suppresses the scissor entirely - see the note by g_noScissor for what that decides.
    const bool useScissor = !InterlockedCompareExchange(&g_noScissor, 0, 0);
    RECT  oldRect{};
    DWORD oldScissorOn = FALSE;
    if (useScissor) {
        dev->GetScissorRect(&oldRect);
        dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &oldScissorOn);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    }

    float mat[16];
    HRESULT hr = S_OK;

    if (dbg != 2) {
        RECT lr = { (LONG)full.X, (LONG)full.Y,
                    (LONG)full.X + halfW, (LONG)(full.Y + full.Height) };
        if (useScissor) dev->SetScissorRect(&lr);
        for (int i = 0; i < nSlots; ++i) {
            const CamMatSlot& cs = g_camMats[slots[i]];
            memcpy(mat, cs.m, sizeof(mat));
            ApplyEyeRemap(reinterpret_cast<Reg4*>(mat), cs.conv, -0.5f);
            g_origSetVSConstF(dev, cs.reg, mat, 4);
        }
        hr = draw();
    }

    if (dbg != 1) {
        RECT rr = { (LONG)full.X + halfW, (LONG)full.Y,
                    (LONG)(full.X + full.Width), (LONG)(full.Y + full.Height) };
        if (useScissor) dev->SetScissorRect(&rr);
        for (int i = 0; i < nSlots; ++i) {
            const CamMatSlot& cs = g_camMats[slots[i]];
            memcpy(mat, cs.m, sizeof(mat));
            // Eye offset first, then the half-frame remap - the offset is a world-space camera
            // move and must happen before clip space is rescaled.
            ApplyOffset(reinterpret_cast<Reg4*>(mat), cs.conv, g_eyeDeltaUU);
            ApplyEyeRemap(reinterpret_cast<Reg4*>(mat), cs.conv, +0.5f);
            g_origSetVSConstF(dev, cs.reg, mat, 4);
        }
        HRESULT hr2 = draw();
        if (dbg == 2) hr = hr2;
    }

    // Put back exactly what the engine had set, for every register we disturbed.
    for (int i = 0; i < nSlots; ++i)
        g_origSetVSConstF(dev, g_camMats[slots[i]].reg, g_camMats[slots[i]].m, 4);
    if (useScissor) {
        dev->SetScissorRect(&oldRect);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, oldScissorOn);
    }
    ++g_drawsDuped;
    if (haveParallax) ++g_drawsDupedOffset;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_DrawPrim(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t,
                                        UINT start, UINT count) {
    return StereoPair(dev, [&] { return g_origDrawPrim(dev, t, start, count); });
}
HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrim(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t,
                                               INT baseVertex, UINT minIndex, UINT numVertices,
                                               UINT startIndex, UINT primCount) {
    return StereoPair(dev, [&] {
        return g_origDrawIndexedPrim(dev, t, baseVertex, minIndex, numVertices, startIndex, primCount);
    });
}

// ---- ⚠️ the run-24/25 startup hang was file I/O in a draw hook ----
//
// The previous attempt logged "first user-pointer draw seen" gated on `g_drawsUP == 1`. But that
// counter is RESET EVERY FRAME for the perf line, so the condition was true once per frame, not
// once ever - the log shows it 119 times. Every one of those did a full fopen/fwrite/fclose from
// inside a draw call.
//
// That is almost certainly the hang, and the reason is that it is the ONLY behavioural difference
// between these two hooks and the DrawPrimitive/DrawIndexedPrimitive hooks that have worked all
// along: those never log. Per-frame synchronous file I/O inside a draw call - on a path UE3 may
// well be driving from its movie-playback thread during splash screens, while our Log() also takes
// a critical section - is exactly the kind of thing that stalls startup.
//
// Lesson: a diagnostic that is cheap "once" is not cheap if the thing gating it resets. Anything
// logging from the hot path needs a one-shot that cannot be reset by unrelated bookkeeping.
// ---- the user-pointer draws are on ANOTHER THREAD (run 26) ----
//
// The ladder localised it precisely: level 1 (patch, forward immediately) reaches the menus; level 2
// (identical, plus `++g_drawsUP`) does not. A single non-atomic increment cannot hang anything by
// itself, so the meaningful difference is not the arithmetic - it is that level 2 WRITES TO SHARED
// STATE on this path and level 1 does not.
//
// That is the signature of these calls arriving on a different thread from the render loop. UE3
// plays its splash movies on a separate thread, and a hot cross-thread write to a global that the
// main thread also touches costs a cache-line transfer every time. If the movie thread issues these
// in bulk, throughput collapses far enough to look like a hang - and every counter inside StereoPair
// carries the same exposure, along with the far worse problem of issuing D3D state changes from a
// thread that does not own the device.
//
// So: do nothing at all unless we are on the thread that drives Present. During splash movies that
// means pure pass-through; during gameplay, where these draws come from the render thread, the
// per-eye treatment still applies.
//
// ---- run 27: the hang had a much simpler cause, and the theory above was never measured ----
//
// The perf log line had nine printf conversions and ten arguments; %s was consuming an int. That
// is harmless while the user-pointer counter is zero and a fault inside a held critical section -
// i.e. a hang - the moment it is not, which is exactly at level 2. See the note at that Log call.
//
// The thread guard is KEPT regardless: issuing SetVertexShaderConstantF, SetScissorRect and a
// draw from a thread that does not own the device is wrong whether or not it explains the hang.
// But it was never established that these calls arrive off the render thread at all - that was
// inferred. g_upOffThread settles it for the cost of one write, ever.
DWORD g_mainThreadId = 0;        // set on the first Present
volatile LONG g_upOffThread = 0; // set once if a user-pointer draw ever arrives off that thread

inline bool OnRenderThread() {
    return g_mainThreadId != 0 && GetCurrentThreadId() == g_mainThreadId;
}

// True if this user-pointer draw must be forwarded untouched. Deliberately write-once on the
// off-thread flag: after the first sighting the line stays shared-read, so the hot cross-thread
// write the run-26 note warns about never happens.
inline bool UserPtrInert() {
    if (g_userPtrHookLevel <= 1) return true;
    if (!OnRenderThread()) {
        // Deliberately a plain volatile read, not an Interlocked one (hence the suppression): a
        // locked operation on every off-thread draw is the exact cost run 26 was worried about,
        // and this flag only ever goes 0 -> 1, which an aligned LONG load reads atomically.
#pragma warning(suppress: 28112)
        if (g_upOffThread == 0) InterlockedExchange(&g_upOffThread, 1);
        return true;
    }
    return false;
}

void NoteFirstUserPtrDraw(const char* which) {
    static bool logged = false;      // genuinely once per process, not once per frame
    if (logged) return;
    logged = true;
    Log("first user-pointer draw seen (%s)", which);
}

// ---- the OTHER two draw entry points (run 24) ----
//
// Only DrawPrimitive and DrawIndexedPrimitive were hooked. D3D9 has two more, the user-pointer
// forms, and UE3 uses them for dynamic geometry - particles, decals, sprites, anything built on the
// CPU each frame rather than living in a static vertex buffer.
//
// Anything drawn through them was never split, so it kept FULL-FRAME coordinates. The centre of the
// frame is the seam between the eye halves, so a centred object drawn this way lands exactly there
// and is effectively invisible, while an off-centre one survives in one half. Same seam mechanism as
// run 23, different cause: not "we chose not to split it" but "we never saw it at all".
//
// The mono-fallback count showing 0 is what pointed here: the camera matrix was live and everything
// we HOOKED was being split correctly, so whatever was vanishing had to be a draw we never saw.
HRESULT STDMETHODCALLTYPE Hook_DrawPrimUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t,
                                          UINT primCount, const void* vtxData, UINT vtxStride) {
    if (!g_origDrawPrimUP) return D3DERR_INVALIDCALL;
    // Touch nothing off the render thread - see the note above OnRenderThread.
    if (UserPtrInert()) return g_origDrawPrimUP(dev, t, primCount, vtxData, vtxStride);
    ++g_drawsUP;
    if (g_userPtrHookLevel == 2) return g_origDrawPrimUP(dev, t, primCount, vtxData, vtxStride);
    NoteFirstUserPtrDraw("DrawPrimitiveUP");
    return StereoPair(dev, [&] { return g_origDrawPrimUP(dev, t, primCount, vtxData, vtxStride); });
}

HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t,
                                                UINT minVtxIndex, UINT numVertices, UINT primCount,
                                                const void* idxData, D3DFORMAT idxFormat,
                                                const void* vtxData, UINT vtxStride) {
    if (!g_origDrawIndexedPrimUP) return D3DERR_INVALIDCALL;
    if (UserPtrInert())
        return g_origDrawIndexedPrimUP(dev, t, minVtxIndex, numVertices, primCount,
                                       idxData, idxFormat, vtxData, vtxStride);
    ++g_drawsUP;
    if (g_userPtrHookLevel == 2)
        return g_origDrawIndexedPrimUP(dev, t, minVtxIndex, numVertices, primCount,
                                       idxData, idxFormat, vtxData, vtxStride);
    NoteFirstUserPtrDraw("DrawIndexedPrimitiveUP");
    return StereoPair(dev, [&] {
        return g_origDrawIndexedPrimUP(dev, t, minVtxIndex, numVertices, primCount,
                                       idxData, idxFormat, vtxData, vtxStride);
    });
}

// ---- occlusion queries: the mechanism nothing here has ever looked at (run 29) ----
//
// UE3 draws a bounding box per primitive inside a D3DQUERYTYPE_OCCLUSION query, reads the pixel
// count back, and skips drawing that primitive on a later frame if it came back zero. When that
// goes wrong the symptom is objects popping in and out, worst when looked at directly, on
// individual props rather than on world geometry - which is the reported behaviour to the letter,
// and the list of offenders (a chest, a locker door, a corpse, ammo) is exactly the set of things
// that get their own occlusion query.
//
// It has never been tested here, and by run 29 it is one of only two surviving candidates.
//
// The test is to take the feature away: refusing to create an occlusion query makes UE3 fall back
// to no occlusion culling, which is the same path it follows on hardware that does not support
// the feature - a supported configuration, not an unexplored one. If the objects come back, the
// mechanism is named. If nothing changes, it is ruled out for good.
//
// ---- ⚠️ mode 2 (refuse to create) CRASHES this build - run 30 ----
//
// D3DERR_NOTAVAILABLE is the documented answer for an unsupported query type, and it is the path
// UE3 follows on hardware without occlusion query support. This build takes it straight into a
// crash between the splash screens and the main menu - consistent with the unchecked-NULL-from-a-
// factory habit already on record for Raven's code in ENGINE_NOTES.
//
// So do not take the feature away. Mode 1 lets every query be created and used exactly as the
// engine expects, and only changes the ANSWER: GetData reports a large pixel count, so nothing is
// ever judged occluded. Same experiment, none of the risk - the engine's control flow is
// untouched and only the culling decision changes.
//
// The device's query vtable is shared by every query it creates, so patching GetData once covers
// all of them; the type check inside the hook keeps D3DQUERYTYPE_EVENT fences (which UE3 uses for
// frame pacing) reading their true values.
//
// ---- ✅ CONFIRMED run 30: this was the flicker, after seven eliminated mechanisms ----
//
// With results forced to "visible", draws per frame went from ~1000 to 2100-3800 and every
// reported object came back solid. The engine was discarding 50-70% of its own draw calls as
// occluded when they were not.
//
// Reporting "visible" is safe in the sense that matters - it can never wrongly delete geometry,
// only fail to save work. But it IS brute force, and a proper fix exists. See STATUS.md, "Why the
// root cause was never pinned down".
//
// Do not repeat the claim this comment used to make, that single-view occlusion culling is "not
// well defined in stereo" and therefore has no correct answer. The engine issues one query, but we
// draw the box TWICE - once into each eye-half - so the returned count is the SUM, which is
// exactly the union of what the two eyes can see. That is the correct conservative answer for
// stereo, so correct culling is achievable and this override is a stand-in for it.
//
// Why the real mechanism is still unknown: occlusion boxes are drawn with colour writes disabled,
// so they are invisible by construction and every visual diagnostic in this file was blind to
// them. The leading hypothesis is that they arrive via DrawPrimitiveUP, which is unhooked, so they
// are drawn at FULL-FRAME coordinates against a depth buffer holding half-remapped geometry.
// Testable by hooking IDirect3DQuery9::Issue (slot 6) and counting how many draws inside each
// query window were never remapped.
//
// The cost is small here and measured: frame rate held at 37-40 fps with peaks of 70, unchanged
// from before, because this build is bottlenecked on the CPU frame copy rather than on draw calls.
// That masking will end with the D3D9Ex work - 3,806 split draws is ~7,600 real draw calls a
// frame, ~290k/sec, which is a lot for D3D9. Re-measure then, and fix it properly if it matters.
//
// Default is AUTO: overridden while duplication is on, left alone in mono, so the safe fallback
// mode keeps the engine's own culling and its speed.
//   0 = auto - report visible while draw duplication is on, normal otherwise   <- default
//   1 = always report visible
//   2 = never override; the engine's occlusion culling is always live
//   3 = refuse to create them  ** CRASHES THIS BUILD, kept only so it is not retried **
int g_occlusionMode = 0;
int g_occlusionForced = 0;

// Defined further down with the other d3d9 plumbing; needed here to patch the query vtable.
void* PatchVTable(void* obj, int index, void* repl);

typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateQuery)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_QueryGetData)(IDirect3DQuery9*, void*, DWORD, DWORD);
PFN_CreateQuery  g_origCreateQuery = nullptr;
PFN_QueryGetData g_origQueryGetData = nullptr;

// A pixel count large enough that no visibility threshold treats it as occluded.
const DWORD kPretendVisiblePixels = 0x00010000;

// Auto mode overrides only while duplication is on - that is the configuration whose split frame
// invalidates the query, and mono has no reason to pay for it.
inline bool OverrideOcclusion() {
    if (g_occlusionMode == 1) return true;
    return g_occlusionMode == 0 && InterlockedCompareExchange(&g_dupDraws, 0, 0) != 0;
}

HRESULT STDMETHODCALLTYPE Hook_QueryGetData(IDirect3DQuery9* q, void* data, DWORD size, DWORD flags) {
    HRESULT hr = g_origQueryGetData(q, data, size, flags);
    // S_FALSE means "not ready yet" and carries no data - only rewrite a completed result.
    if (OverrideOcclusion() && hr == S_OK && data && size >= sizeof(DWORD) &&
        q->GetType() == D3DQUERYTYPE_OCCLUSION) {
        *reinterpret_cast<DWORD*>(data) = kPretendVisiblePixels;
        ++g_occlusionForced;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateQuery(IDirect3DDevice9* dev, D3DQUERYTYPE type,
                                           IDirect3DQuery9** out) {
    if (g_occlusionMode == 3 && type == D3DQUERYTYPE_OCCLUSION) {
        if (out) *out = nullptr;
        return D3DERR_NOTAVAILABLE;
    }
    HRESULT hr = g_origCreateQuery(dev, type, out);
    // Patch GetData on the first real query we see. One patch covers every query from this
    // device, since they all share a vtable. Auto mode patches too - the hook decides per call.
    if (g_occlusionMode <= 1 && SUCCEEDED(hr) && out && *out && !g_origQueryGetData) {
        g_origQueryGetData = (PFN_QueryGetData)PatchVTable(*out, 7, (void*)&Hook_QueryGetData);
        Log("occlusion queries will report FULLY VISIBLE (GetData patched, orig=%p)",
            (void*)g_origQueryGetData);
        if (!g_origQueryGetData) { Log("*** GetData patch failed - occlusion test inactive ***");
                                   g_occlusionMode = 0; }
    }
    return hr;
}

// ================================================================ D3D9Ex + D3DPOOL_MANAGED
//
// Why this exists: OpenXR has no D3D9 graphics binding, so every frame has to reach D3D11. Doing
// that without a CPU round-trip needs a SHARED SURFACE, shared surfaces need a D3D9**Ex** device,
// and D3D9Ex rejects D3DPOOL_MANAGED outright (spike 1: D3DERR_INVALIDCALL). Spike 2 counted
// 10,454 MANAGED allocations in one session against 47 DEFAULT, so handing UE3 an Ex device
// unmodified fails nearly every resource creation at startup.
//
// So the pool has to be translated, and whatever MANAGED was providing has to be provided by us.
// MANAGED gives the application three things:
//
//   1. Lock() works at any time, because there is a system-memory copy behind the resource
//   2. that copy is uploaded to VRAM automatically
//   3. it survives device loss
//
// (3) is free here, and it is the reason this is safe rather than merely possible: **D3D9Ex does
// not lose D3DPOOL_DEFAULT resources on Reset.** That is one of the headline differences of Ex,
// and it removes what would otherwise be the biggest objection to this whole approach - on a plain
// D3D9 device, translating MANAGED to DEFAULT would mean rebuilding every resource on every
// device-lost event.
//
// (1) and (2) split by resource type, and the split is what keeps this tractable:
//
//   VERTEX / INDEX BUFFERS (7,149 of the 10,454) - **pool swap only, no shadow copy.**
//     D3D9 permits Lock() on a DEFAULT vertex or index buffer. It stalls the pipeline for a
//     static one, which is why it is discouraged, but it is legal and it is correct - and these
//     are locked once at load, not per frame. Two thirds of the problem needs no machinery at all.
//
//   TEXTURES / CUBE / VOLUME (3,305) - **pool swap + SYSTEMMEM shadow.**
//     LockRect on a DEFAULT texture genuinely fails (only D3DUSAGE_DYNAMIC textures can be
//     locked), so these need the system-memory copy MANAGED used to keep. We create the DEFAULT
//     texture the game renders from, create a SYSTEMMEM shadow alongside it, hand the game the
//     DEFAULT one, and redirect its Lock/Unlock to the shadow - uploading with UpdateTexture when
//     the shadow is unlocked after being written.
//
// ---- why vtable patching rather than COM proxy objects ----
//
// The obvious implementation is a wrapper class per resource type, forwarding every method. That
// also means unwrapping at every device entry point that accepts a resource - SetTexture,
// SetStreamSource, SetIndices, UpdateTexture, StretchRect, GetTexture - and one missed site is a
// crash.
//
// Patching the vtable avoids all of it. The game is handed the REAL DEFAULT texture, so every
// device call that consumes it works untouched; only Lock/Unlock/Release are intercepted. And
// D3D9 resources of a type share one vtable, so a single patch covers every texture the device
// will ever create - exactly the trick already proven here on IDirect3DQuery9::GetData for the
// run-30 occlusion fix.
//
// The cost is that the patch is global: Lock on a texture we did NOT translate lands in our hook
// too. That is a side-table miss and an immediate forward, which is why the table lookup has to be
// cheap rather than convenient.
//
// ---- vtable slots, verified against the SDK header rather than remembered ----
//
// Extracted from d3d9.h by enumerating each interface's STDMETHOD declarations in order. The same
// extraction reproduces every slot this project already relies on - Present 17, SetRenderTarget
// 37, DrawPrimitive 81, SetVertexShaderConstantF 94, CreateQuery 118 - so the ones below are
// confirmed by the same method that the working hooks vouch for.
//
//   IDirect3DDevice9        CreateTexture 23, CreateVolumeTexture 24, CreateCubeTexture 25,
//                           CreateVertexBuffer 26, CreateIndexBuffer 27, UpdateTexture 31
//   IDirect3DTexture9       LockRect 19, UnlockRect 20
//   IDirect3DCubeTexture9   LockRect 19, UnlockRect 20
//   IDirect3DVolumeTexture9 LockBox  19, UnlockBox  20
//   IDirect3DVertexBuffer9  Lock 11, Unlock 12
//   IDirect3DIndexBuffer9   Lock 11, Unlock 12
//   IUnknown                Release 2
//
// Off by default. This replaces the resource management of a shipping engine; it is one relaunch
// away from off precisely because the failure mode is "the game does not start".
int  g_d3d9ExMode = 0;                 // ini D3D9ExMode: 0 = plain D3D9 (default), 1 = Ex + translate
// g_deviceIsEx ("did we actually end up with an Ex device") is declared up beside the zero-copy
// globals - the frame path consults it long before this section appears.
IDirect3D9Ex*     g_d3d9ex = nullptr;  // kept so CreateDevice can call CreateDeviceEx
IDirect3DDevice9* g_gameDev = nullptr; // for UpdateTexture

LONG g_xlatTex = 0, g_xlatVB = 0, g_xlatIB = 0, g_xlatCube = 0, g_xlatVol = 0;
LONG g_stageCount = 0, g_stageFailed = 0, g_xlatFailed = 0;
double g_stageMB = 0.0;

// ---- the side table: DEFAULT resource -> its SYSTEMMEM shadow ----
//
// Open addressing with linear probing, fixed size, no allocation. Sized ~5x the 3,305 textures
// spike 2 counted so the table stays sparse and probes stay short - this is consulted on every
// texture Lock in the process, including ones we never translated.
struct StageEntry {
    IDirect3DBaseTexture9* key;    // what the game holds (DEFAULT)
    IDirect3DBaseTexture9* shadow; // what it actually locks (SYSTEMMEM)
    bool  dirty;                   // written since the last upload
};
const int kStageSlots = 16384;                 // power of two
StageEntry g_stageTab[kStageSlots];
// g_stageLock / g_stageLockReady are declared beside g_lock at the top so LogInit can initialise
// both in one place.

inline int StageHash(const void* p) {
    // Pointers are at least 16-byte aligned in practice; drop the dead low bits before masking.
    return (int)(((uintptr_t)p >> 4) & (kStageSlots - 1));
}

void StageAdd(IDirect3DBaseTexture9* key, IDirect3DBaseTexture9* shadow) {
    EnterCriticalSection(&g_stageLock);
    int i = StageHash(key);
    for (int n = 0; n < kStageSlots; ++n) {
        StageEntry& e = g_stageTab[i];
        if (!e.key || e.key == key) {
            e.key = key; e.shadow = shadow; e.dirty = false;
            LeaveCriticalSection(&g_stageLock);
            return;
        }
        i = (i + 1) & (kStageSlots - 1);
    }
    LeaveCriticalSection(&g_stageLock);
    Log("*** staging table FULL at %d entries - texture %p left unshadowed ***", kStageSlots, (void*)key);
}

StageEntry* StageFind(IDirect3DBaseTexture9* key) {
    int i = StageHash(key);
    for (int n = 0; n < kStageSlots; ++n) {
        StageEntry& e = g_stageTab[i];
        if (!e.key) return nullptr;          // empty slot ends the probe chain
        if (e.key == key) return &e;
        i = (i + 1) & (kStageSlots - 1);
    }
    return nullptr;
}

// Tombstone-free removal is wrong with linear probing, so removed slots keep a dead marker: the
// key is cleared but the chain is preserved by leaving `shadow` set to a sentinel. Simpler here to
// just leave the entry with a null key AND rehash nothing - textures are created far more often
// than destroyed during load, and the table is sized 5x. Accepting a slow leak of dead slots is
// safer than getting probe-chain repair subtly wrong.
void StageRemove(IDirect3DBaseTexture9* key) {
    EnterCriticalSection(&g_stageLock);
    StageEntry* e = StageFind(key);
    if (e) {
        if (e->shadow) e->shadow->Release();
        e->shadow = nullptr;
        e->dirty = false;
        // Key deliberately left in place as a tombstone so probe chains through this slot survive.
        // StageFind still matches it, but the null shadow makes every path treat it as absent.
    }
    LeaveCriticalSection(&g_stageLock);
}

// Rough, for the log only. The point is to notice if shadows become a memory problem in a 32-bit
// process, not to be exact. MANAGED kept a system copy too, so this should be roughly a wash.
double ApproxTexMB(UINT w, UINT h, UINT levels, D3DFORMAT fmt) {
    double bpp = 4.0;
    switch ((DWORD)fmt) {
        case MAKEFOURCC('D','X','T','1'): bpp = 0.5; break;
        case MAKEFOURCC('D','X','T','2'): case MAKEFOURCC('D','X','T','3'):
        case MAKEFOURCC('D','X','T','4'): case MAKEFOURCC('D','X','T','5'): bpp = 1.0; break;
        case D3DFMT_A8: case D3DFMT_L8: bpp = 1.0; break;
        case D3DFMT_R5G6B5: case D3DFMT_A1R5G5B5: case D3DFMT_A8L8: bpp = 2.0; break;
        default: break;
    }
    double base = (double)w * h * bpp;
    double total = (levels <= 1) ? base : base * 1.34;   // full mip chain converges on 4/3
    return total / (1024.0 * 1024.0);
}

// ---- Lock/Unlock redirection ----
typedef HRESULT (STDMETHODCALLTYPE *PFN_TexLockRect)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_TexUnlockRect)(IDirect3DTexture9*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CubeLockRect)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CubeUnlockRect)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_VolLockBox)(IDirect3DVolumeTexture9*, UINT, D3DLOCKED_BOX*, const D3DBOX*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_VolUnlockBox)(IDirect3DVolumeTexture9*, UINT);
typedef ULONG   (STDMETHODCALLTYPE *PFN_Release)(IUnknown*);

PFN_TexLockRect    g_origTexLock = nullptr;
PFN_TexUnlockRect  g_origTexUnlock = nullptr;
PFN_Release        g_origTexRelease = nullptr;
PFN_CubeLockRect   g_origCubeLock = nullptr;
PFN_CubeUnlockRect g_origCubeUnlock = nullptr;
PFN_Release        g_origCubeRelease = nullptr;
PFN_VolLockBox     g_origVolLock = nullptr;
PFN_VolUnlockBox   g_origVolUnlock = nullptr;
PFN_Release        g_origVolRelease = nullptr;

// Push the shadow's newly-dirtied regions up to the DEFAULT resource. SYSTEMMEM textures track
// dirty rects themselves across LockRect, and UpdateTexture clears them as it copies, so calling
// this per unlock moves only what that unlock touched rather than the whole chain each time.
inline void FlushShadow(StageEntry* e) {
    if (!e || !e->shadow || !e->dirty || !g_gameDev) return;
    HRESULT hr = g_gameDev->UpdateTexture(e->shadow, e->key);
    e->dirty = false;
    if (FAILED(hr)) {
        static int once = 0;
        if (once < 5) { ++once; Log("UpdateTexture failed hr=0x%08lX for %p", hr, (void*)e->key); }
    }
}

HRESULT STDMETHODCALLTYPE Hook_TexLockRect(IDirect3DTexture9* self, UINT level, D3DLOCKED_RECT* out,
                                           const RECT* rect, DWORD flags) {
    EnterCriticalSection(&g_stageLock);
    StageEntry* e = StageFind(self);
    IDirect3DTexture9* shadow = (e && e->shadow) ? (IDirect3DTexture9*)e->shadow : nullptr;
    if (shadow && !(flags & D3DLOCK_READONLY)) e->dirty = true;
    LeaveCriticalSection(&g_stageLock);
    if (!shadow) return g_origTexLock(self, level, out, rect, flags);   // not ours - forward
    return g_origTexLock(shadow, level, out, rect, flags);
}

HRESULT STDMETHODCALLTYPE Hook_TexUnlockRect(IDirect3DTexture9* self, UINT level) {
    EnterCriticalSection(&g_stageLock);
    StageEntry* e = StageFind(self);
    IDirect3DTexture9* shadow = (e && e->shadow) ? (IDirect3DTexture9*)e->shadow : nullptr;
    HRESULT hr = shadow ? g_origTexUnlock(shadow, level) : S_OK;
    if (shadow) FlushShadow(e);
    LeaveCriticalSection(&g_stageLock);
    if (!shadow) return g_origTexUnlock(self, level);
    return hr;
}

ULONG STDMETHODCALLTYPE Hook_TexRelease(IUnknown* self) {
    ULONG n = g_origTexRelease(self);
    // Only once the object is actually gone. The pointer is used as a key here, never dereferenced.
    if (n == 0) StageRemove((IDirect3DBaseTexture9*)self);
    return n;
}

// ---- ⚠️ the known gap, instrumented rather than hoped about ----
//
// Redirection only covers Lock on the TEXTURE. An engine can instead call GetSurfaceLevel and lock
// the returned IDirect3DSurface9, which bypasses all of this - and locking a surface belonging to
// a DEFAULT texture fails, so the upload would silently do nothing and the texture would render as
// whatever uninitialised VRAM it was given.
//
// Hooking surface Lock is not a clean fix: surfaces from textures share a vtable with render-target
// and depth surfaces, and the shadow's surfaces would need pairing level by level. Before building
// that, find out whether UE3 takes this path at all. If this counter stays at zero the gap is
// theoretical; if it climbs, that is the next piece of work and the reason any textures look wrong.
typedef HRESULT (STDMETHODCALLTYPE *PFN_TexGetSurfaceLevel)(IDirect3DTexture9*, UINT, IDirect3DSurface9**);
PFN_TexGetSurfaceLevel g_origTexGetSurfaceLevel = nullptr;
LONG g_surfLevelOnShadowed = 0;

HRESULT STDMETHODCALLTYPE Hook_TexGetSurfaceLevel(IDirect3DTexture9* self, UINT level,
                                                  IDirect3DSurface9** out) {
    EnterCriticalSection(&g_stageLock);
    StageEntry* e = StageFind(self);
    const bool shadowed = (e && e->shadow);
    LeaveCriticalSection(&g_stageLock);
    if (shadowed) {
        LONG n = InterlockedIncrement(&g_surfLevelOnShadowed);
        if (n <= 3)
            Log("*** GetSurfaceLevel on a SHADOWED texture (#%ld) - if the engine LOCKS this"
                " surface the write bypasses the shadow and fails on DEFAULT. This is the known"
                " gap in the wrapper; suspect it first if textures look wrong. ***", n);
    }
    return g_origTexGetSurfaceLevel(self, level, out);
}

HRESULT STDMETHODCALLTYPE Hook_CubeLockRect(IDirect3DCubeTexture9* self, D3DCUBEMAP_FACES face,
                                            UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) {
    EnterCriticalSection(&g_stageLock);
    StageEntry* e = StageFind(self);
    IDirect3DCubeTexture9* shadow = (e && e->shadow) ? (IDirect3DCubeTexture9*)e->shadow : nullptr;
    if (shadow && !(flags & D3DLOCK_READONLY)) e->dirty = true;
    LeaveCriticalSection(&g_stageLock);
    if (!shadow) return g_origCubeLock(self, face, level, out, rect, flags);
    return g_origCubeLock(shadow, face, level, out, rect, flags);
}

HRESULT STDMETHODCALLTYPE Hook_CubeUnlockRect(IDirect3DCubeTexture9* self, D3DCUBEMAP_FACES face, UINT level) {
    EnterCriticalSection(&g_stageLock);
    StageEntry* e = StageFind(self);
    IDirect3DCubeTexture9* shadow = (e && e->shadow) ? (IDirect3DCubeTexture9*)e->shadow : nullptr;
    HRESULT hr = shadow ? g_origCubeUnlock(shadow, face, level) : S_OK;
    if (shadow) FlushShadow(e);
    LeaveCriticalSection(&g_stageLock);
    if (!shadow) return g_origCubeUnlock(self, face, level);
    return hr;
}

ULONG STDMETHODCALLTYPE Hook_CubeRelease(IUnknown* self) {
    ULONG n = g_origCubeRelease(self);
    if (n == 0) StageRemove((IDirect3DBaseTexture9*)self);
    return n;
}

HRESULT STDMETHODCALLTYPE Hook_VolLockBox(IDirect3DVolumeTexture9* self, UINT level,
                                          D3DLOCKED_BOX* out, const D3DBOX* box, DWORD flags) {
    EnterCriticalSection(&g_stageLock);
    StageEntry* e = StageFind(self);
    IDirect3DVolumeTexture9* shadow = (e && e->shadow) ? (IDirect3DVolumeTexture9*)e->shadow : nullptr;
    if (shadow && !(flags & D3DLOCK_READONLY)) e->dirty = true;
    LeaveCriticalSection(&g_stageLock);
    if (!shadow) return g_origVolLock(self, level, out, box, flags);
    return g_origVolLock(shadow, level, out, box, flags);
}

HRESULT STDMETHODCALLTYPE Hook_VolUnlockBox(IDirect3DVolumeTexture9* self, UINT level) {
    EnterCriticalSection(&g_stageLock);
    StageEntry* e = StageFind(self);
    IDirect3DVolumeTexture9* shadow = (e && e->shadow) ? (IDirect3DVolumeTexture9*)e->shadow : nullptr;
    HRESULT hr = shadow ? g_origVolUnlock(shadow, level) : S_OK;
    if (shadow) FlushShadow(e);
    LeaveCriticalSection(&g_stageLock);
    if (!shadow) return g_origVolUnlock(self, level);
    return hr;
}

ULONG STDMETHODCALLTYPE Hook_VolRelease(IUnknown* self) {
    ULONG n = g_origVolRelease(self);
    if (n == 0) StageRemove((IDirect3DBaseTexture9*)self);
    return n;
}

// ---- the Create* hooks ----
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateTexture)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateVolumeTexture)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateCubeTexture)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateVertexBuffer)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateIndexBuffer)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);

PFN_CreateTexture       g_origCreateTexture = nullptr;
PFN_CreateVolumeTexture g_origCreateVolumeTexture = nullptr;
PFN_CreateCubeTexture   g_origCreateCubeTexture = nullptr;
PFN_CreateVertexBuffer  g_origCreateVertexBuffer = nullptr;
PFN_CreateIndexBuffer   g_origCreateIndexBuffer = nullptr;

// D3DUSAGE_DYNAMIC never legally coexists with MANAGED, and a DEFAULT texture that IS dynamic can
// be locked directly - so a translated texture only needs a shadow when it is not dynamic.
inline bool NeedsShadow(DWORD usage) { return (usage & D3DUSAGE_DYNAMIC) == 0; }

// Defined below - the vtable for a resource type can only be patched once an instance exists, so
// each Create* hook arms its own type on first success.
void PatchTextureVTableOnce(IDirect3DTexture9* t);
void PatchCubeVTableOnce(IDirect3DCubeTexture9* t);
void PatchVolVTableOnce(IDirect3DVolumeTexture9* t);

HRESULT STDMETHODCALLTYPE Hook_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels,
                                             DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                             IDirect3DTexture9** out, HANDLE* share) {
    if (!g_deviceIsEx || pool != D3DPOOL_MANAGED)
        return g_origCreateTexture(dev, w, h, levels, usage, fmt, pool, out, share);

    HRESULT hr = g_origCreateTexture(dev, w, h, levels, usage, fmt, D3DPOOL_DEFAULT, out, share);
    if (FAILED(hr) || !out || !*out) {
        InterlockedIncrement(&g_xlatFailed);
        static int once = 0;
        if (once < 5) { ++once; Log("CreateTexture DEFAULT failed hr=0x%08lX (%ux%u lv=%u usage=0x%lX fmt=%d)",
                                    hr, w, h, levels, usage, (int)fmt); }
        return hr;
    }
    InterlockedIncrement(&g_xlatTex);
    PatchTextureVTableOnce(*out);
    if (!NeedsShadow(usage)) return hr;

    // Take the level count from the texture that was actually created rather than from `levels`,
    // which is 0 for "full chain" and 1 under D3DUSAGE_AUTOGENMIPMAP.
    const UINT realLevels = (*out)->GetLevelCount();
    IDirect3DTexture9* shadow = nullptr;
    HRESULT hs = g_origCreateTexture(dev, w, h, realLevels, 0, fmt, D3DPOOL_SYSTEMMEM, &shadow, nullptr);
    if (FAILED(hs) || !shadow) {
        InterlockedIncrement(&g_stageFailed);
        static int once = 0;
        if (once < 5) { ++once; Log("shadow CreateTexture failed hr=0x%08lX (%ux%u lv=%u fmt=%d)"
                                    " - this texture will not be lockable", hs, w, h, realLevels, (int)fmt); }
        return hr;   // the DEFAULT texture is still valid; only Lock will misbehave
    }
    StageAdd(*out, shadow);
    InterlockedIncrement(&g_stageCount);
    g_stageMB += ApproxTexMB(w, h, realLevels, fmt);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCubeTexture(IDirect3DDevice9* dev, UINT edge, UINT levels,
                                                 DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                 IDirect3DCubeTexture9** out, HANDLE* share) {
    if (!g_deviceIsEx || pool != D3DPOOL_MANAGED)
        return g_origCreateCubeTexture(dev, edge, levels, usage, fmt, pool, out, share);

    HRESULT hr = g_origCreateCubeTexture(dev, edge, levels, usage, fmt, D3DPOOL_DEFAULT, out, share);
    if (FAILED(hr) || !out || !*out) { InterlockedIncrement(&g_xlatFailed); return hr; }
    InterlockedIncrement(&g_xlatCube);
    PatchCubeVTableOnce(*out);
    if (!NeedsShadow(usage)) return hr;

    const UINT realLevels = (*out)->GetLevelCount();
    IDirect3DCubeTexture9* shadow = nullptr;
    if (SUCCEEDED(g_origCreateCubeTexture(dev, edge, realLevels, 0, fmt, D3DPOOL_SYSTEMMEM, &shadow, nullptr))
        && shadow) {
        StageAdd(*out, shadow);
        InterlockedIncrement(&g_stageCount);
        g_stageMB += ApproxTexMB(edge, edge, realLevels, fmt) * 6.0;
    } else {
        InterlockedIncrement(&g_stageFailed);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateVolumeTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT depth,
                                                   UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                   IDirect3DVolumeTexture9** out, HANDLE* share) {
    if (!g_deviceIsEx || pool != D3DPOOL_MANAGED)
        return g_origCreateVolumeTexture(dev, w, h, depth, levels, usage, fmt, pool, out, share);

    HRESULT hr = g_origCreateVolumeTexture(dev, w, h, depth, levels, usage, fmt, D3DPOOL_DEFAULT, out, share);
    if (FAILED(hr) || !out || !*out) { InterlockedIncrement(&g_xlatFailed); return hr; }
    InterlockedIncrement(&g_xlatVol);
    PatchVolVTableOnce(*out);
    if (!NeedsShadow(usage)) return hr;

    const UINT realLevels = (*out)->GetLevelCount();
    IDirect3DVolumeTexture9* shadow = nullptr;
    if (SUCCEEDED(g_origCreateVolumeTexture(dev, w, h, depth, realLevels, 0, fmt, D3DPOOL_SYSTEMMEM, &shadow, nullptr))
        && shadow) {
        StageAdd(*out, shadow);
        InterlockedIncrement(&g_stageCount);
        g_stageMB += ApproxTexMB(w, h, realLevels, fmt) * depth;
    } else {
        InterlockedIncrement(&g_stageFailed);
    }
    return hr;
}

// Buffers need no shadow - see the header comment. Pool swap only.
HRESULT STDMETHODCALLTYPE Hook_CreateVertexBuffer(IDirect3DDevice9* dev, UINT len, DWORD usage,
                                                  DWORD fvf, D3DPOOL pool,
                                                  IDirect3DVertexBuffer9** out, HANDLE* share) {
    if (g_deviceIsEx && pool == D3DPOOL_MANAGED) { pool = D3DPOOL_DEFAULT; InterlockedIncrement(&g_xlatVB); }
    return g_origCreateVertexBuffer(dev, len, usage, fvf, pool, out, share);
}

HRESULT STDMETHODCALLTYPE Hook_CreateIndexBuffer(IDirect3DDevice9* dev, UINT len, DWORD usage,
                                                 D3DFORMAT fmt, D3DPOOL pool,
                                                 IDirect3DIndexBuffer9** out, HANDLE* share) {
    if (g_deviceIsEx && pool == D3DPOOL_MANAGED) { pool = D3DPOOL_DEFAULT; InterlockedIncrement(&g_xlatIB); }
    return g_origCreateIndexBuffer(dev, len, usage, fmt, pool, out, share);
}

// Resource vtables can only be patched once an instance of that type exists, so each Create* hook
// arms its own type the first time it succeeds. One patch covers every instance from the device.
void PatchTextureVTableOnce(IDirect3DTexture9* t) {
    static LONG done = 0;
    if (!t || InterlockedExchange(&done, 1)) return;
    g_origTexLock    = (PFN_TexLockRect)PatchVTable(t, 19, (void*)&Hook_TexLockRect);
    g_origTexUnlock  = (PFN_TexUnlockRect)PatchVTable(t, 20, (void*)&Hook_TexUnlockRect);
    g_origTexRelease = (PFN_Release)PatchVTable(t, 2, (void*)&Hook_TexRelease);
    g_origTexGetSurfaceLevel = (PFN_TexGetSurfaceLevel)PatchVTable(t, 18, (void*)&Hook_TexGetSurfaceLevel);
    Log("texture vtable patched (LockRect=%p UnlockRect=%p Release=%p GetSurfaceLevel=%p)",
        (void*)g_origTexLock, (void*)g_origTexUnlock, (void*)g_origTexRelease,
        (void*)g_origTexGetSurfaceLevel);
}

void PatchCubeVTableOnce(IDirect3DCubeTexture9* t) {
    static LONG done = 0;
    if (!t || InterlockedExchange(&done, 1)) return;
    g_origCubeLock    = (PFN_CubeLockRect)PatchVTable(t, 19, (void*)&Hook_CubeLockRect);
    g_origCubeUnlock  = (PFN_CubeUnlockRect)PatchVTable(t, 20, (void*)&Hook_CubeUnlockRect);
    g_origCubeRelease = (PFN_Release)PatchVTable(t, 2, (void*)&Hook_CubeRelease);
    Log("cube texture vtable patched");
}

void PatchVolVTableOnce(IDirect3DVolumeTexture9* t) {
    static LONG done = 0;
    if (!t || InterlockedExchange(&done, 1)) return;
    g_origVolLock    = (PFN_VolLockBox)PatchVTable(t, 19, (void*)&Hook_VolLockBox);
    g_origVolUnlock  = (PFN_VolUnlockBox)PatchVTable(t, 20, (void*)&Hook_VolUnlockBox);
    g_origVolRelease = (PFN_Release)PatchVTable(t, 2, (void*)&Hook_VolRelease);
    Log("volume texture vtable patched");
}

void ReportPoolTranslation() {
    Log("D3D9Ex pool translation: %ld textures, %ld cube, %ld volume, %ld vertex buf, %ld index buf"
        " | shadows %ld (~%.0f MB), shadow failures %ld, create failures %ld,"
        " GetSurfaceLevel-on-shadowed %ld",
        g_xlatTex, g_xlatCube, g_xlatVol, g_xlatVB, g_xlatIB,
        g_stageCount, g_stageMB, g_stageFailed, g_xlatFailed, g_surfLevelOnShadowed);
}

// Refresh the camera position the detector substitutes. Runs once per frame in Present.
void RefreshCameraPosition() {
    // FindCamera is cheap once it has a live instance cached - it just revalidates the
    // pointer - but a MISS walks all ~112,000 GObjects with a VirtualQuery per entry. That
    // happens on every menu frame, so retry the search only occasionally while there is no
    // camera. Once found, the per-frame revalidation keeps it current for free.
    static int missTick = 0;
    if (!g_camera && (++missTick % 30) != 1) { InterlockedExchange(&g_camPosValid, 0); return; }
    uintptr_t cam = FindCamera();
    if (!cam) { InterlockedExchange(&g_camPosValid, 0); return; }
    bool ok = false;
    if (Readable((void*)(cam + ACTOR_LOCATION), 12)) {
        memcpy(g_camPos, (void*)(cam + ACTOR_LOCATION), 12);
        ok = true;
    }
    if (Readable((void*)(cam + CAM_POV_LOC), 12)) memcpy(g_povPos, (void*)(cam + CAM_POV_LOC), 12);
    else memcpy(g_povPos, g_camPos, 12);

    // Camera axes from its FRotator. UE3 convention: X forward, Y right, Z up, angles as
    // int32 with 65536 = 360 degrees. This is what makes the origin (translated-world) case
    // decidable - the position test alone cannot tell a view matrix from any other matrix
    // there, but the facing direction can.
    if (Readable((void*)(cam + ACTOR_ROTATION), 12)) {
        const int32_t* rot = reinterpret_cast<const int32_t*>(cam + ACTOR_ROTATION);
        const float kUUToRad = 6.2831853f / 65536.0f;
        float pitch = rot[0] * kUUToRad, yaw = rot[1] * kUUToRad;
        float cp = cosf(pitch), sp = sinf(pitch), cy = cosf(yaw), sy = sinf(yaw);
        g_camFwd[0]   = cp * cy; g_camFwd[1]   = cp * sy; g_camFwd[2]   = sp;
        g_camRight[0] = -sy;     g_camRight[1] = cy;      g_camRight[2] = 0.0f;
    }
    // A camera sitting exactly at the origin would make the w test meaningless - every matrix
    // would pass on its constant term alone. Wait for a real position before scanning.
    float d2 = g_camPos[0]*g_camPos[0] + g_camPos[1]*g_camPos[1] + g_camPos[2]*g_camPos[2];
    InterlockedExchange(&g_camPosValid, (ok && d2 > 1000.0f) ? 1 : 0);
}

// Ask the game for a field of view wide enough to cover the headset.
//
// Match the headset's VERTICAL FOV rather than its horizontal. The game renders 16:9 (wider
// than tall) while a per-eye view is nearly square, so equal vertical FOV leaves the game's
// horizontal field comfortably wider than the headset needs - full coverage on both axes,
// with the surplus simply falling outside the eye. Matching horizontally instead would leave
// the top and bottom short, which is the visible half of the current aspect mismatch.
//
// Written every frame because the engine recomputes it, and to all three FOV fields so the
// interpolator does not drag it back. mCurrentPOV's FOV is the one proven to steer the
// projection; the other two stop it fighting.
void ApplyVrFov() {
    if (!InterlockedCompareExchange(&g_vrProjection, 0, 0)) return;
    if (!g_haveEyes || g_srcW == 0 || g_srcH == 0) return;
    uintptr_t cam = FindCamera();
    if (!cam) return;

    float vHalf = (fabsf(g_eyeFov[0].angleUp) + fabsf(g_eyeFov[0].angleDown)) * 0.5f;
    if (!(vHalf > 0.05f && vHalf < 1.5f)) return;
    // Render only the requested fraction of the headset's field. Scaling the ANGLE rather than
    // the tangent keeps the steps behaving the way they read.
    vHalf *= g_renderFovScale;
    float aspect = (float)g_srcW / (float)g_srcH;
    float hFovDeg = 2.0f * atanf(tanf(vHalf) * aspect) * 57.2957795f;
    if (!(hFovDeg > 30.0f && hFovDeg < 170.0f)) return;      // never write nonsense

    // What we FORCE into the matrix and submit to OpenXR - one value, so the two cannot
    // disagree and nothing can wobble. Vertical is the headset's own, horizontal follows from
    // the backbuffer's aspect, which keeps the forced frustum consistent with the 16:9 image
    // we are actually pasting into the eye.
    // With duplication on, each eye occupies half the backbuffer's width, so the per-eye
    // frustum is half as wide. Getting this wrong would stretch both eyes horizontally by 2x.
    const bool dupOn = InterlockedCompareExchange(&g_dupDraws, 0, 0) != 0;
    const float prevTanX = g_targetTanX;
    g_targetTanY = tanf(vHalf);
    g_targetTanX = g_targetTanY * (dupOn ? aspect * 0.5f : aspect);
    // If the target moved, discard the readback of the previous one. Run 15 reported "matrix
    // measures 127.9 (want 91.3)" for a long stretch - the 127.9 was a leftover from before F1
    // changed the target, so a total failure to force anything looked like a partial success.
    if (fabsf(g_targetTanX - prevTanX) > 1e-4f) g_forcedTanX = g_forcedTanY = 0.0f;

    // The request to the engine is deliberately wider: it now governs only culling.
    float askDeg = 2.0f * atanf(tanf(vHalf) * aspect * kFovHeadroom) * 57.2957795f;
    if (askDeg > 170.0f) askDeg = 170.0f;
    // The headroom now goes below 1.0, and PAGE DOWN scales vHalf on top of it, so the two
    // together can drive this under the 30 deg floor the sanity check above enforces on the
    // un-headroomed value. 40 deg is below the game's own 65 and still a legal field.
    if (askDeg < 40.0f) askDeg = 40.0f;
    hFovDeg = askDeg;

    // ---- read BEFORE writing: does our value survive the engine's own camera update? ----
    //
    // Run 4 showed the rendered image pulsing between two fields of view - a flash on the
    // monitor, black bars flicking in and out at the top and bottom in the headset. The
    // suspicion is that the engine recomputes FOV each tick and interpolates back toward its
    // own default, so our Present-time write survives some frames and not others, and the
    // sawtooth is what the eye sees. Reading the field back before overwriting it settles
    // that: if the write sticks, this reads our own value every frame; if the engine is
    // fighting us, the min/max spread shows the shape of the fight.
    float povBefore = Readable((void*)(cam + CAM_POV_FOV), 4)
                      ? *reinterpret_cast<float*>(cam + CAM_POV_FOV) : -1.0f;
    float desBefore = Readable((void*)(cam + CAM_DESIRED_FOV), 4)
                      ? *reinterpret_cast<float*>(cam + CAM_DESIRED_FOV) : -1.0f;
    float curBefore = Readable((void*)(cam + CAM_CURRENT_FOV), 4)
                      ? *reinterpret_cast<float*>(cam + CAM_CURRENT_FOV) : -1.0f;

    static float povMin = 1e9f, povMax = -1e9f;
    if (povBefore > 0) { if (povBefore < povMin) povMin = povBefore;
                         if (povBefore > povMax) povMax = povBefore; }

    if (Readable((void*)(cam + CAM_POV_FOV), 4))     *reinterpret_cast<float*>(cam + CAM_POV_FOV) = hFovDeg;
    if (Readable((void*)(cam + CAM_DESIRED_FOV), 4)) *reinterpret_cast<float*>(cam + CAM_DESIRED_FOV) = hFovDeg;
    if (Readable((void*)(cam + CAM_CURRENT_FOV), 4)) *reinterpret_cast<float*>(cam + CAM_CURRENT_FOV) = hFovDeg;

    // Hand the value and the camera to the camera-update seam, which re-asserts it there.
    g_vrFovDeg = hFovDeg;
    g_lastCam  = cam;

    static int n = 0;
    if (++n % 60 == 1) {
        Log("VR FOV: forcing %.1f x %.1f deg into the matrix; asked the engine for %.1f deg"
            " (culling headroom x%.2f)",
            2.0f * atanf(g_targetTanX) * 57.2957795f,
            2.0f * atanf(g_targetTanY) * 57.2957795f, hFovDeg, kFovHeadroom);
        // The vertical figure is the one that matters: the engine's FOV is horizontal at 16:9, so
        // its vertical culling frustum is much smaller, and OUR render is tall and narrow. When
        // this drops below what we draw, geometry is culled before a draw call exists - which is
        // invisible in the horizontal numbers alone and was the run-19 flicker.
        const float engVertMin = 2.0f * atanf(tanf(povMin * 0.5f / 57.2957795f) /
                                              ((float)g_srcW / (float)g_srcH)) * 57.2957795f;
        Log("    engine FOV read back before write: mCurrentPOV %.1f (range %.1f-%.1f over the"
            " last 60), mDesiredPOV %.1f, mCurrentFOV %.1f",
            povBefore, povMin, povMax, desBefore, curBefore);
        // NB this is the WORST frame in the window, so it flags a shortfall if any single frame
        // dipped - which is the right sensitivity for a flicker that only needs one bad frame to
        // be visible, but do not read it as "75% of the time".
        // How far we have inflated the engine's FOV above the game's native 65 deg, and what that
        // does to projected size. UE3 culls and picks LOD by screen-space size, so an object at a
        // given distance appears (tan(native/2) / tan(asked/2)) times smaller than the engine was
        // tuned for - which makes it cull things far closer than vanilla does. Run 21 caught this
        // by checking the unmodified game: objects that vanish in the mod are visible from much
        // further away without it, so the mod is culling early, not the game.
        //
        // This is the direct cost of culling headroom, and it pulls AGAINST the vertical coverage
        // the headroom exists to buy. Both numbers have to be read together.
        {
            const float nativeTanHalf = tanf(32.5f / 57.2957795f);      // 65 deg native
            const float askedTanHalf  = tanf(hFovDeg * 0.5f / 57.2957795f);
            Log("    FOV inflation vs native 65 deg: x%.2f -> objects project %.1fx smaller than"
                " the engine expects (early distance culling)",
                hFovDeg / 65.0f, askedTanHalf / nativeTanHalf);
        }
        const float renderedVert = 2.0f * atanf(g_targetTanY) * 57.2957795f;
        Log("    worst engine VERTICAL cull this window, from mCurrentPOV: %.1f deg vs %.1f deg"
            " rendered -> %s", engVertMin, renderedVert,
            engVertMin + 1.0f < renderedVert ? "SHORTFALL, geometry culled" : "covered");
        // ---- and the same thing measured, not inferred (run 27) ----
        //
        // mCurrentPOV is a proxy, and the log shows it disagreeing with reality badly: it reads
        // back the 157.9 we wrote while the matrix the engine actually built for the same frame
        // carries 80.7 x 51.1 deg. The matrix is the frustum the engine really rendered, so it is
        // also the one its culling was computed from. Every shortfall number until now came from
        // the proxy.
        if (g_capTanYMin < 1e8f) {
            const float engVertMeasured = 2.0f * atanf(g_capTanYMin) * 57.2957795f;
            Log("    worst engine VERTICAL cull this window, from the ARRIVING MATRIX (ground"
                " truth): %.1f deg vs %.1f deg rendered -> %s%s",
                engVertMeasured, renderedVert,
                engVertMeasured + 1.0f < renderedVert ? "SHORTFALL, geometry culled" : "covered",
                (engVertMeasured + 1.0f < renderedVert && engVertMin + 1.0f >= renderedVert)
                    ? "   <-- the proxy said 'covered' and was WRONG" : "");
        }
        g_capTanYMin = 1e9f;
        Log("    after forcing, the matrix measures %.1f x %.1f deg (want %.1f x %.1f)",
            g_forcedTanX > 0 ? 2.0f * atanf(g_forcedTanX) * 57.2957795f : 0.0f,
            g_forcedTanY > 0 ? 2.0f * atanf(g_forcedTanY) * 57.2957795f : 0.0f,
            2.0f * atanf(g_targetTanX) * 57.2957795f,
            2.0f * atanf(g_targetTanY) * 57.2957795f);
        // NB the figure below is the ENGINE's matrix as it arrived, before our edit - it is a
        // measure of the engine's drift, not of our output. The count is per DRAW CALL using
        // the camera matrix, not per distinct matrix; ~50-60 a frame is normal.
        Log("    engine's own matrix on arrival: %.1f x %.1f deg (spread within the frame"
            " %.1f-%.1f deg horiz, %d draw call(s) modified)",
            g_projTanX > 0 ? 2.0f * atanf(g_projTanX) * 57.2957795f : 0.0f,
            g_projTanY > 0 ? 2.0f * atanf(g_projTanY) * 57.2957795f : 0.0f,
            g_capTanMin < 1e8f ? 2.0f * atanf(g_capTanMin) * 57.2957795f : 0.0f,
            g_capTanMax > 0    ? 2.0f * atanf(g_capTanMax) * 57.2957795f : 0.0f,
            g_injCount);
        povMin = 1e9f; povMax = -1e9f;
    }
}

void ReportScan() {
    Log("--- scan complete: %d candidate window(s), camera (%.1f, %.1f, %.1f) POV (%.1f, %.1f, %.1f)",
        g_hitCount, g_camPos[0], g_camPos[1], g_camPos[2], g_povPos[0], g_povPos[1], g_povPos[2]);
    Log("    camera facing (%.3f, %.3f, %.3f), right (%.3f, %.3f, %.3f)",
        g_camFwd[0], g_camFwd[1], g_camFwd[2], g_camRight[0], g_camRight[1], g_camRight[2]);
    if (g_hitCount == 0) {
        if (g_haveNear)
            Log("    closest window: c%u %s w=%+.2f fwdLen=%.4f (tolerance was %.0f)",
                g_nearBestReg, g_nearBestConv == CONV_ROW ? "ROW" : "COL",
                g_nearBestW, g_nearBestLen, kHitTolUU);
        else
            Log("    no window even had a plausible forward vector.");
        Log("    All three probe points were tried, so this is not the translated-world case.");
        Log("    Next suspects: the matrix reaching the GPU via a shader constant BUFFER rather");
        Log("    than SetVertexShaderConstantF, or a block split across two calls.");
        return;
    }
    // Passing windows first, then by UPLOAD COUNT - the main scene view-projection is set once
    // per pass and used by most of the frame's draws, so it is uploaded far more often than a
    // weapon or reflection matrix that happens to share the register. The original scan showed
    // exactly that: 50 uploads for the real one against 4-9 for the impostors. Ranking by
    // agreement-with-facing put a rare pass first whenever its dot product was marginally
    // better, which is how run 16 ended up locking onto 36.7x21.1 deg matrices.
    for (int i = 0; i < g_hitCount; ++i)
        for (int j = i + 1; j < g_hitCount; ++j) {
            bool pi = IsViewMatrix(g_hits[i].w, g_hits[i].dotFwd, g_hits[i].pt, g_hits[i].shape);
            bool pj = IsViewMatrix(g_hits[j].w, g_hits[j].dotFwd, g_hits[j].pt, g_hits[j].shape);
            if ((pj && !pi) || (pj == pi && g_hits[j].count > g_hits[i].count)) {
                MatHit t = g_hits[i]; g_hits[i] = g_hits[j]; g_hits[j] = t;
            }
        }
    int passing = 0;
    for (int i = 0; i < g_hitCount; ++i) {
        bool pass = IsViewMatrix(g_hits[i].w, g_hits[i].dotFwd, g_hits[i].pt, g_hits[i].shape);
        if (pass) ++passing;
        Log("    %s c%-3u %s  w=%+.4f  fwdLen=%.4f  dotFwd=%+.4f  shape=%s  hits=%-5d  zero at %s",
            pass ? "**" : "  ", g_hits[i].startReg, g_hits[i].conv == CONV_ROW ? "ROW" : "COL",
            g_hits[i].w, g_hits[i].fwdLen, g_hits[i].dotFwd,
            g_hits[i].shape ? "ok " : "BAD", g_hits[i].count,
            kPointName[g_hits[i].pt]);
    }
    Log("    %d of %d windows pass ALL THREE tests (** marked). Only those are injected into.",
        passing, g_hitCount);
    // A rarely-uploaded window is not the scene matrix however well it scores otherwise, so
    // require it to carry a real share of the frame before trusting it.
    const int kMinUploadsToLock = 8;
    if (passing > 0 && IsViewMatrix(g_hits[0].w, g_hits[0].dotFwd, g_hits[0].pt, g_hits[0].shape)) {
        // ---- refuse to lock on anything but the translated-world probe ----
        //
        // TrackedViewMatrix, which every later upload is judged by, tests |r[3].w| ~ 0 - the
        // translated-world case, where the camera sits at the origin. That is deliberate: it is
        // pure matrix content, so nothing in it can go stale, which is what lets the per-call test
        // survive a frame of rotation lag. It is also confirmed for this engine.
        //
        // But it means a lock acquired on the camActor or mCurrentPOV probe could never verify:
        // the lock would be dropped after 60 frames, rescanned, relocked and dropped again, for
        // ever, with stereo off the whole time. Refuse it and say why, rather than loop silently.
        if (g_hits[0].pt != 2) {
            Log("    NOT locking c%u - it matched at %s, not at the translated-world origin."
                " The per-upload test only understands the origin case, so this lock could never"
                " verify. If this ever fires, the engine is not translated-world and"
                " TrackedViewMatrix needs the probe point carried with the lock.",
                g_hits[0].startReg, kPointName[g_hits[0].pt]);
        } else if (g_hits[0].count >= kMinUploadsToLock) {
            g_lockedReg = g_hits[0].startReg; g_lockedConv = g_hits[0].conv; g_haveLock = true;
            Log("    LOCKED on c%u %s (%d uploads this frame)", g_lockedReg,
                g_lockedConv == CONV_ROW ? "ROW" : "COL", g_hits[0].count);
        } else {
            Log("    best window c%u only had %d uploads (< %d) - not locking, will rescan",
                g_hits[0].startReg, g_hits[0].count, kMinUploadsToLock);
        }
    }
    Log("    -> F3 offsets the camera %.0f UU along its own right axis. F11 changes the amount.",
        g_offsetUU);
}

void InstallViewHook() {
    if (MH_Initialize() != MH_OK) { Log("MH_Initialize failed"); return; }
    void* target = reinterpret_cast<void*>(kCalcViewRotationAddr);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&Hook_CalcViewRotation),
                      reinterpret_cast<void**>(&g_origCalcViewRotation)) != MH_OK) {
        Log("MH_CreateHook failed at 0x%08X", kCalcViewRotationAddr); return;
    }
    if (MH_EnableHook(target) != MH_OK) { Log("MH_EnableHook failed"); return; }
    Log("view rotation detour installed at 0x%08X", kCalcViewRotationAddr);
}

// ---------------------------------------------------------------- VR mode, as one switch
//
// F9 (head tracking), F10 (6-DOF) and F1 (true stereo) are not really three features, they are the
// mod - but the cold start stays flat (unchanged, run 37 reverted the default-on tried in run 36),
// because starting in VR mode meant the game's own main menu was rendered split for the first
// time ever, untested. BACKSPACE reaches the working state in one press instead of three, without
// changing what the game looks like on launch.
//
// "Off" means the plain game: no head tracking, no positional offset, one mono image, and the
// engine's own projection restored - because the forced per-eye frustum is only correct while
// duplication is splitting the frame, and leaving it on would give a mono view stretched to 98
// degrees, which looks broken rather than standard.
//
// BACKSPACE because every F-key F1-F12 is taken, and it is large enough to find by feel while
// wearing a headset - which is the entire point of a one-press switch.
void SetVrMode(bool on) {
    InterlockedExchange(&g_enabled,      on ? 1 : 0);
    InterlockedExchange(&g_sixDof,       on ? 1 : 0);
    InterlockedExchange(&g_dupDraws,     on ? 1 : 0);
    InterlockedExchange(&g_vrProjection, on ? 1 : 0);
    // Alternate-eye is the other stereo method and is never part of this set. Clearing it stops
    // BACKSPACE from ever producing the both-modes-at-once state that F12-after-F1 produces.
    InterlockedExchange(&g_stereo, 0);
    // Same reasoning as F1: slots left from a previous session are stale by definition, and the
    // recentre stops the 6-DOF offset resuming from wherever the head was when it was switched off.
    InvalidateCamMats();
    g_haveCentre = false;
    Log("BACKSPACE: VR mode %s (head tracking, 6-DOF, true stereo, VR projection)",
        on ? "ON" : "OFF - plain game");
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* s, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    // Whoever calls Present owns the device. Recorded so the user-pointer draw hooks can tell
    // render-thread calls from movie-playback-thread ones and stay completely inert on the latter.
    g_mainThreadId = GetCurrentThreadId();
    if (InterlockedExchange(&g_initTried, 1) == 0) { InstallViewHook(); InitXR(); }

    static bool p9=false,p8=false,p5=false,p4=false,p2=false;
    bool k9=(GetAsyncKeyState(VK_F9)&0x8000)!=0, k8=(GetAsyncKeyState(VK_F8)&0x8000)!=0;
    bool k5=(GetAsyncKeyState(VK_F5)&0x8000)!=0, k4=(GetAsyncKeyState(VK_F4)&0x8000)!=0;
    bool k2=(GetAsyncKeyState(VK_F2)&0x8000)!=0;
    if (k9&&!p9) { LONG was=InterlockedCompareExchange(&g_enabled,0,0);
                   InterlockedExchange(&g_enabled, was?0:1); g_haveCentre=false;
                   Log("F9: head tracking %s", was?"OFF":"ON"); }
    if (k2&&!p2) { LONG was=InterlockedCompareExchange(&g_constTest,0,0);
                   InterlockedExchange(&g_constTest, was?0:1);
                   Log("F2: constant pitch test %s", was?"OFF":"ON"); }
    if (k8&&!p8) { g_haveCentre=false; Log("F8: recentre"); }
    if (k5&&!p5) { g_yawSign=-g_yawSign; g_haveCentre=false; Log("F5: yaw sign %+d", g_yawSign); }
    if (k4&&!p4) { g_pitchSign=-g_pitchSign; Log("F4: pitch sign %+d", g_pitchSign); }
    static bool p3 = false;
    bool k3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
    if (k3 && !p3) { LONG was = InterlockedCompareExchange(&g_injectOn,0,0);
                     InterlockedExchange(&g_injectOn, was?0:1);
                     Log("F3: matrix camera offset %s (%.0f UU along the camera's right axis)",
                         was?"OFF":"ON", g_offsetUU); }
    p3 = k3;
    static bool p6 = false;
    bool k6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (k6 && !p6) { LONG was = InterlockedCompareExchange(&g_vrProjection,0,0);
                     InterlockedExchange(&g_vrProjection, was?0:1);
                     Log("F6: VR-correct projection %s", was?"OFF (headset FOV claimed, as before)":"ON"); }
    p6 = k6;
    static bool p1 = false;
    bool k1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (k1 && !p1) { LONG was = InterlockedCompareExchange(&g_dupDraws,0,0);
                     InterlockedExchange(&g_dupDraws, was?0:1);
                     // The two stereo methods are alternatives, never both at once. Duplication
                     // also depends on the forced projection: each eye occupies half the
                     // width, and without the matching per-eye frustum both come out squashed
                     // horizontally by 2x. Turning it on here removes that broken combination.
                     if (!was) { InterlockedExchange(&g_stereo, 0);
                                 InterlockedExchange(&g_vrProjection, 1); }
                     // Slots are only maintained while duplication is on, so anything left over
                     // from a previous F1 session is by definition stale. Clearing here stops the
                     // first draws after re-enabling being pushed through a matrix from minutes
                     // ago.
                     InvalidateCamMats();
                     g_haveCentre = false;
                     Log("F1: draw-call duplication (TRUE stereo) %s", was?"OFF":"ON"); }
    p1 = k1;

    // BACKSPACE: the whole mod on or off in one key. See SetVrMode.
    static bool pBack = false;
    bool kBack = (GetAsyncKeyState(VK_BACK) & 0x8000) != 0;
    if (kBack && !pBack) SetVrMode(InterlockedCompareExchange(&g_dupDraws, 0, 0) == 0);
    pBack = kBack;

    // INSERT: blank one half. Whichever eye goes black tells us the half-to-eye mapping for
    // certain, which reading the code did not settle.
    static bool pIns = false;
    bool kIns = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (kIns && !pIns) {
        LONG next = (InterlockedCompareExchange(&g_eyeDebug,0,0) + 1) % 3;
        InterlockedExchange(&g_eyeDebug, next);
        Log("INSERT: eye debug %s", next == 0 ? "OFF (both halves drawn)"
            : next == 1 ? "drawing LEFT half only -> the RIGHT eye should go black"
                        : "drawing RIGHT half only -> the LEFT eye should go black");
    }
    pIns = kIns;

    // PAGE UP: cycle the culling headroom, to test whether the very wide engine FOV it asks for
    // is what is making textures flicker (streaming and LOD both key off projected size).
    static bool pPgUp = false;
    bool kPgUp = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
    if (kPgUp && !pPgUp) {
        g_fovHeadroomStep = (g_fovHeadroomStep + 1) % kFovHeadroomCount;
        kFovHeadroom = kFovHeadroomSteps[g_fovHeadroomStep];
        Log("PAGE UP: culling headroom now x%.2f%s", kFovHeadroom,
            kFovHeadroom < 1.05f ? " (no headroom - expect edge dropouts, but correct LOD)" : "");
    }
    pPgUp = kPgUp;

    // PAGE DOWN: how much of the headset's field we render. Lowers what the engine has to cull,
    // which is the same trade as PAGE UP approached from the other side.
    static bool pPgDn = false;
    bool kPgDn = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
    if (kPgDn && !pPgDn) {
        g_renderFovStep = (g_renderFovStep + 1) % kRenderFovCount;
        g_renderFovScale = kRenderFovSteps[g_renderFovStep];
        Log("PAGE DOWN: rendering %.0f%% of the headset's field of view%s",
            g_renderFovScale * 100.0f,
            g_renderFovScale < 0.45f
                ? "  <-- CULLING TEST: no shortfall is possible here. If the flicker survives"
                  " this, culling is not the cause." : "");
    }
    pPgDn = kPgDn;

    // HOME: suppress the scissor. The one test that separates "drawn, then clipped by us" from
    // "never drawn at all" - see the note by g_noScissor.
    static bool pHome = false;
    bool kHome = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    if (kHome && !pHome) {
        LONG was = InterlockedCompareExchange(&g_noScissor, 0, 0);
        InterlockedExchange(&g_noScissor, was ? 0 : 1);
        Log("HOME: eye-half scissor %s", was ? "ON (normal)"
            : "OFF  <-- DECISIVE TEST. Geometry we remapped is unaffected. Geometry we could NOT"
              " remap now REAPPEARS, full-frame and duplicated. If the vanishing object comes"
              " back, it was being drawn and we were clipping it - the fix is draw"
              " classification. If it stays gone, no draw call ever existed - the fix is culling.");
    }
    pHome = kHome;

    // END: collapse c0 - see the note by g_blankCamMat.
    static bool pEnd = false;
    bool kEnd = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    if (kEnd && !pEnd) {
        LONG was = InterlockedCompareExchange(&g_blankCamMat, 0, 0);
        InterlockedExchange(&g_blankCamMat, was ? 0 : 1);
        Log("END: camera matrix %s", was ? "restored"
            : "CLIPPED OUT  <-- everything driven by the tracked register is discarded before"
              " rasterisation, so the world should simply be GONE. WHATEVER IS STILL VISIBLE is"
              " geometry we cannot remap, and therefore geometry the scissor clips at the seam."
              " The HUD should survive - it never used this register - which doubles as proof the"
              " test is working rather than the frame having failed.");
    }
    pEnd = kEnd;

    // DELETE: reverse the mapping, so a confirmed swap can be fixed and verified immediately.
    static bool pDel = false;
    bool kDel = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
    if (kDel && !pDel) { LONG was = InterlockedCompareExchange(&g_swapEyes,0,0);
                         InterlockedExchange(&g_swapEyes, was?0:1);
                         Log("DELETE: eye/half assignment %s", was?"NORMAL":"SWAPPED"); }
    pDel = kDel;
    static bool p12 = false;
    bool k12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (k12 && !p12) { LONG was = InterlockedCompareExchange(&g_stereo,0,0);
                       InterlockedExchange(&g_stereo, was?0:1);
                       g_eyeFilled[0] = g_eyeFilled[1] = false;
                       g_renderEye = 0;
                       Log("F12: alternate-eye STEREO %s%s", was?"OFF":"ON",
                           was?"":" - each eye now updates at half the frame rate");
                       if (!was) Log("    stereo shares the position path, so it brings"
                                     " positional head tracking with it whether F10 is on or not");
                     }
    p12 = k12;
    static bool p10 = false;
    bool k10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (k10 && !p10) { LONG was = InterlockedCompareExchange(&g_sixDof,0,0);
                       InterlockedExchange(&g_sixDof, was?0:1);
                       g_haveCentre = false;      // recentre so the offset starts from zero
                       Log("F10: 6-DOF head position %s", was?"OFF":"ON"); }
    p10 = k10;
    static bool p11 = false;
    bool k11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (k11 && !p11) {
        g_offsetStep = (g_offsetStep + 1) % kOffsetStepCount;
        g_offsetUU = kOffsetSteps[g_offsetStep];
        Log("F11: offset now %.0f UU", g_offsetUU);
    }
    p11 = k11;
    p9=k9;p8=k8;p5=k5;p4=k4;p2=k2;

    // The camera position the detector substitutes must be this frame's, read before any of
    // the frame's draw calls - which is exactly what Present-time refresh gives us, since the
    // constants for the frame about to be presented were uploaded just before we got here.
    // Timed because "our work" hit 69-82 ms a frame in menus (run 34) with the copy behaving
    // normally, and both of these walk GObjects on a cache miss. Naming the cost beats assuming it.
    {
        LARGE_INTEGER tEng = Now();
        RefreshCameraPosition();
        ApplyVrFov();
        g_msEngineObjects += MsSince(tEng);
        ++g_engineObjectFrames;
    }
    // "Can a 2010 game afford twice the draw calls" is the whole question this spike exists to
    // answer, so measure it rather than reasoning about it. Frame time is wall clock between
    // Presents, averaged over a second.
    {
        static LARGE_INTEGER freq{}, last{};
        static int frames = 0; static double accum = 0.0;
        static int dupPeak = 0, totPeak = 0, offsetPeak = 0, offscreenPeak = 0, monoPeak = 0, upPeak = 0;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        if (last.QuadPart) accum += double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
        last = now;
        if (g_drawsTotal > totPeak) totPeak = g_drawsTotal;
        if (g_drawsDuped > dupPeak) dupPeak = g_drawsDuped;
        if (g_drawsDupedOffset > offsetPeak) offsetPeak = g_drawsDupedOffset;
        if (g_drawsOffscreen > offscreenPeak) offscreenPeak = g_drawsOffscreen;
        if (g_drawsMonoFallback > monoPeak) monoPeak = g_drawsMonoFallback;
        if (g_drawsUP > upPeak) upPeak = g_drawsUP;
        // A frame in which the tracked register was uploaded but never once passed the test is a
        // frame that got no forced projection and no per-eye split for ANY draw. Nothing measured
        // this before, and it is the single most likely explanation left for a flicker that
        // tracks head movement - which run 23's elimination test, run with head tracking off,
        // could not have seen.
        if (g_lockUploads > 0 && g_lockRejects == g_lockUploads) ++g_framesNoIdent;
        if (++frames >= 120) {
            // "duplicated" is every draw split across both eye halves; "with offset" is the
            // subset that also got true parallax. The gap between them is drawn flat/mono in
            // both eyes - visible now, not silently dropped from one eye as it was before.
            // monoFallback is the number that matters for the run-23 flicker: these draws had no
            // live camera matrix, so they are drawn once instead of being scissored into a half
            // they were never remapped into. A high or spiky count means the camera slot is going
            // invalid often, which costs parallax on those objects even though they now appear.
            // ---- ⚠️ THIS LINE IS WHY THE USER-POINTER HOOKS "HUNG" (found run 27) ----
            //
            // It had nine conversions and TEN arguments: upPeak was missing its %d, so %s
            // consumed an int and formatted it as a char*. The evidence is in every log from
            // run 23 onwards - "mono-fallback 0(null)", where (null) is upPeak == 0 printed as a
            // string, and where [TRUE STEREO ON] never once appeared because its argument was
            // being swallowed.
            //
            // Harmless only while upPeak stays zero. upPeak is non-zero exactly when
            // HookUserPointerDraws >= 2 - at which point %s dereferences a small integer, inside
            // Log(), inside EnterCriticalSection(&g_lock). The fault leaves the lock held, so
            // every subsequent Log() blocks forever. That is precisely the reported behaviour:
            // level 1 reaches the menus, level 2 does not, and it presents as a hang rather than
            // a crash.
            //
            // So the run-26 conclusion - that these calls arrive on a different thread and a
            // shared write on that path is ruinous - was inferred from a symptom this bug fully
            // explains. The thread guard is kept (issuing device state changes from a thread that
            // does not own the device is genuinely wrong), but it was not the fix.
            //
            // Lesson: printf argument mismatches are a latent crash a compiler will diagnose for
            // free - but only if it is told the function is printf-like AND asked to look. Log()
            // is now annotated _Printf_format_string_ and build.ps1 runs /analyze, which together
            // make exactly this mistake fail the build. Neither half works alone.
            Log("perf: %.1f fps (%.2f ms/frame) | draws/frame peak %d, split %d (%d with"
                " parallax, %d flat), offscreen %d, mono-fallback %d, user-ptr %d,"
                " frames with NO identification %d%s",
                frames / (accum > 0 ? accum : 1.0), (accum * 1000.0) / frames, totPeak, dupPeak,
                offsetPeak, dupPeak - offsetPeak, offscreenPeak, monoPeak, upPeak, g_framesNoIdent,
                InterlockedCompareExchange(&g_dupDraws, 0, 0) ? " [TRUE STEREO ON]" : "");
            // ---- the frame copy, broken into its three phases (run 31) ----
            //
            // This is the number that decides whether the D3D9Ex + MANAGED-pool wrapper is worth
            // writing. Only `readback` is the part D3D9Ex removes; lock/memcpy and upload survive
            // it untouched. If the residual - everything that is NOT the copy - dominates, then
            // the bottleneck was never the round-trip and the whole plan needs rethinking.
            //
            // The residual is also the meter for re-measuring the occlusion override: draw
            // submission lives in it, so running OcclusionQueryMode=1 against 2 and comparing
            // residuals prices ~7,600 draw calls a frame directly, without timing each one.
            if (g_copyFrames) {
                const double ms = (accum * 1000.0) / frames;
                const double rb = g_msGRTD / g_copyFrames;
                const double lk = g_msLock / g_copyFrames;
                const double lc = g_msLockCopy / g_copyFrames;
                const double up = g_msUpload / g_copyFrames;
                const double wf = g_waitFrames ? g_msWaitFrame / g_waitFrames : 0.0;
                const double gw = g_gpuWaitFrames ? g_msGpuWait / g_gpuWaitFrames : 0.0;
                const double copy = rb + lk + lc + up;
                Log("    frame copy [%s]: %s %.2f + lock(DMA lands) %.2f + memcpy %.2f"
                    " + upload %.2f = %.2f ms of %.2f (%.0f%%)",
                    g_zeroCopyActive ? "ZERO-COPY" : (g_copyMode == 1 ? "immediate" : "pipelined"),
                    g_zeroCopyActive ? "StretchRect" : "readback", rb, lk, lc, up,
                    copy, ms, ms > 0 ? 100.0 * copy / ms : 0.0);
                // The split that decides where any further effort goes. Time in xrWaitFrame is
                // the app sitting idle waiting for the compositor, so it is not ours to optimise;
                // "our work" is. Chasing the copy or the draw count while xrWaitFrame holds most
                // of the frame would be optimising the part that is already free.
                const double eng = g_engineObjectFrames ? g_msEngineObjects / g_engineObjectFrames : 0.0;
                Log("    frame budget: xrWaitFrame %.2f (idle) + copy %.2f + GObjects walk %.2f"
                    " + our work %.2f = %.2f ms (%.0f%% of the %.2f ms display period)",
                    wf, copy, eng, ms - wf - copy - eng, ms,
                    g_displayPeriodMs > 0.0 ? 100.0 * ms / g_displayPeriodMs : 0.0,
                    g_displayPeriodMs);
                // A cached hit is microseconds; anything here in milliseconds means the object is
                // absent and the full scan is repeating, which is the menu/level-load freeze.
                if (eng > 2.0)
                    Log("    *** GObjects walk cost %.2f ms this window - the camera or controller"
                        " is ABSENT, so the cache cannot warm and the full scan repeats. Expected"
                        " in menus and during level load; should be ~0 in gameplay. ***", eng);
                // ---- ⚠️ the SSW check, and the correction it needed one run later ----
                //
                // Run 31 measured 28 ms frames, concluded the app was missing a 72 Hz deadline,
                // and cancelled the D3D9Ex work on the strength of it. The real cause was
                // Synchronous Spacewarp: SSW pins the app at HALF the display rate on purpose
                // and synthesises the frames in between. With it off the same build runs at
                // 105-118 fps. Every performance number this project recorded before run 32 was
                // taken through that halving, which is why the frame copy looked like 3% - it was
                // 0.9 ms of a frame that had been padded out to 27.8 ms.
                //
                // ---- but frame time ALONE cannot identify SSW. Corrected run 34. ----
                //
                // The first version of this check warned on frame time ~2x the display period and
                // nothing else. That fires on any app honestly running at half the refresh - 60 fps
                // against 120 Hz is 2.00x by definition - and it duly cried wolf all through run
                // 33, which was genuinely CPU-bound at 16 ms with SSW switched off.
                //
                // The discriminator is xrWaitFrame, and it was already being collected. Under SSW
                // the runtime holds the app back, so it finishes early and BLOCKS - a large share
                // of the frame sits idle in xrWaitFrame. An app that is simply slow does not idle
                // at all; it arrives late and xrWaitFrame returns immediately.
                //
                //   ~2x period AND mostly idle   -> being held back. SSW.
                //   ~2x period AND barely idle   -> genuinely takes that long. Not SSW.
                if (g_displayPeriodMs > 0.0) {
                    const double mult = ms / g_displayPeriodMs;
                    const bool mostlyIdle = ms > 0.0 && (wf / ms) > 0.35;
                    if (mult > 1.85 && mult < 2.15 && mostlyIdle) {
                        static int warned = 0;
                        if (warned < 3) {
                            ++warned;
                            Log("    *** frame time is %.2fx the display period AND %.0f%% of it is"
                                " idle in xrWaitFrame - the app is being HELD BACK, not running"
                                " slow. Suspect SSW/ASW (Virtual Desktop 'Synchronous Spacewarp',"
                                " Meta 'ASW'). Turn it off before trusting these numbers. ***",
                                mult, 100.0 * wf / ms);
                        }
                    }
                }
                // The line the D3D9Ex decision turns on. `gpu wait` is rendering we would still
                // have to wait for with a zero-copy path; `transfer` + `lock/memcpy` + `upload` is
                // what it actually deletes. Read the RECOVERABLE figure as the ceiling on what the
                // MANAGED wrapper can buy - and note it is a ceiling, not a promise, since the
                // compositor still has to consume the shared surface.
                if (g_gpuWaitFrames) {
                    const double recoverable = copy;
                    Log("    round-trip split: gpu wait %.2f ms (rendering - NOT recoverable)"
                        " | transfer %.2f + memcpy %.2f + upload %.2f = %.2f ms RECOVERABLE"
                        " -> zero-copy ceiling %.2f ms/frame (%.0f fps)",
                        gw, rb + lk, lc, up, recoverable,
                        ms - recoverable, (ms - recoverable) > 0 ? 1000.0 / (ms - recoverable) : 0.0);
                }
            }
            g_msGRTD = g_msLock = g_msLockCopy = g_msUpload = 0.0;
            g_copyFrames = 0;
            g_msWaitFrame = 0.0; g_waitFrames = 0;
            g_msGpuWait = 0.0; g_gpuWaitFrames = 0;
            g_msEngineObjects = 0.0; g_engineObjectFrames = 0;
            // Once, after the level has loaded and the counts have stopped moving. The comparison
            // that matters is against spike 2's census: 3,276 textures / 6,258 VB / 891 IB / 26
            // cube / 3 volume. Numbers far below that mean allocations are escaping translation.
            if (g_deviceIsEx) {
                static int poolReports = 0;
                if (poolReports < 3) { ++poolReports; ReportPoolTranslation(); }
            }
            // The identification counters are the point of this run. c0 carries one matrix per
            // frame, so a rejection is a whole-frame event - no forced projection and no split for
            // any draw in it. If "frames with NO identification" is non-zero, that is a full-frame
            // flash, and g_bestRejectDot names the cause: near 0.99 is rotation lag against a
            // stale g_camFwd, far below it is a genuinely different matrix taking the register.
            if (g_framesNoIdent || g_bestRejectDot > -1.5f)
                Log("    identification: %d of %d frames lost the matrix entirely; best rejected"
                    " dotFwd this window %+.4f (gate %.2f)",
                    g_framesNoIdent, frames, g_bestRejectDot, kTrackDotFwd);
            g_framesNoIdent = 0;
            g_bestRejectDot = -2.0f;
            // Settles the run-26 theory rather than leaving it inferred. If this never appears
            // while the user-pointer hooks are on, those calls do NOT come from another thread
            // and the guard is costing us the very draws we hooked them to catch.
            {
                static bool saidOcclusion = false;
                if (!saidOcclusion && g_occlusionForced > 0) {
                    saidOcclusion = true;
                    Log("    occlusion results overridden to FULLY VISIBLE (%d so far) - this is"
                        " the run-30 fix. Single-view occlusion culling is not well defined in"
                        " stereo, so reporting visible is the conservative correct answer.",
                        g_occlusionForced);
                }
            }
            if (g_userPtrHookLevel >= 2) {
                static bool saidOffThread = false, saidOnThread = false;
                if (g_upOffThread && !saidOffThread) {
                    saidOffThread = true;
                    Log("    user-pointer draws DO arrive off the Present thread - those are being"
                        " forwarded untouched, as intended");
                } else if (!g_upOffThread && upPeak > 0 && !saidOnThread) {
                    saidOnThread = true;
                    Log("    user-pointer draws arrive ONLY on the Present thread (%d/frame) - the"
                        " run-26 threading explanation does not hold", upPeak);
                }
            }
            if (InterlockedCompareExchange(&g_dupDraws, 0, 0)) {
                // Stereo silently doing nothing is the worst failure mode there is - run 14 lost
                // a whole session to it. If duplication is on and no draw was split, say so
                // loudly and name the likely reason rather than leaving it to be inferred from
                // the census.
                if (dupPeak == 0 && totPeak > 100)
                    Log("*** WARNING: TRUE STEREO is ON but NOTHING was split (%d draws seen)."
                        " The scene target does not match the backbuffer %ux%u - stereo is"
                        " inactive. ***", totPeak, g_srcW, g_srcH);
                // ---- ⚠️ this line is NOT evidence about the run-18 theory (found run 28) ----
                //
                // It was recorded in STATUS as refuting it: "register count measured 1, ever".
                // But the modify path only ever caches g_lockedReg, so g_camMats can hold at most
                // one entry by construction. This counter cannot report anything else, and the
                // theory it supposedly killed was never tested. Third tautological diagnostic in
                // this file, after `remapKnown = true` and the perf line's swallowed argument.
                //
                // Worse, the scan cannot settle it either. It finds VIEW matrices by requiring the
                // camera to transform to w ~ 0. A pre-composed LOCAL-TO-CLIP matrix puts w at the
                // object's distance, not zero, so it can never pass - geometry drawn that way is
                // invisible to every instrument here, gets scissored while still holding
                // full-frame coordinates, and disappears at the seam. HOME is the test for it.
                Log("registers cached for the draw hook: %d live (of %d ever). NB this is capped"
                    " at 1 by construction - it says nothing about whether other registers carry"
                    " view-projections. Use F7 for that, and HOME for the composed-matrix case.",
                    g_camMatValidPeak, g_camMatUsed);
                for (int s = 0; s < g_camMatUsed; ++s)
                    Log("    c%-3u %s %s", g_camMats[s].reg,
                        g_camMats[s].conv == CONV_ROW ? "ROW" : "COL",
                        g_camMats[s].valid ? "live" : "(idle)");
                g_camMatValidPeak = 0;
                ReportRenderTargets();
            }
            frames = 0; accum = 0.0; dupPeak = 0; totPeak = 0; offsetPeak = 0; offscreenPeak = 0; monoPeak = 0; upPeak = 0;
        }
    }

    // Revoke a lock that has stopped holding, so the next frame re-scans instead of staying
    // stuck on the wrong register forever. See the note by g_lockVerified.
    if (g_haveLock) {
        if (g_lockVerified) {
            g_lockMissFrames = 0;
        } else if (++g_lockMissFrames > 60) {
            Log("*** lock on c%u %s stopped verifying for %d frames - DROPPING it and"
                " re-scanning ***", g_lockedReg, g_lockedConv == CONV_ROW ? "ROW" : "COL",
                g_lockMissFrames);
            g_haveLock = false;
            g_lockMissFrames = 0;
            g_camMatrixBound = false;
            g_projTanX = g_projTanY = 0.0f;      // stale readings must not look healthy
            g_forcedTanX = g_forcedTanY = 0.0f;
            // The fourth path that used to leave a slot valid with nothing behind it. Without
            // this, dropping the lock left StereoPair re-uploading the last matrix it ever
            // captured, over every draw, until a rescan happened to succeed.
            InvalidateCamMats();
        }
        g_lockVerified = false;
    }

    // Reset the per-frame diagnostics AFTER reporting them, so each report covers one frame.
    g_capTanMin = 1e9f; g_capTanMax = -1e9f; g_injCount = 0;
    g_lockUploads = 0; g_lockRejects = 0;
    g_drawsTotal = 0; g_drawsDuped = 0; g_drawsDupedOffset = 0; g_drawsOffscreen = 0; g_drawsMonoFallback = 0; g_drawsUP = 0;

    // The scan runs for exactly one frame. Arming it in Present means it covers the whole of
    // the NEXT frame's constant uploads, so the report is collected on the frame after that.
    static bool p7 = false;
    bool k7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (InterlockedCompareExchange(&g_scanArmed, 0, 0)) {
        InterlockedExchange(&g_scanArmed, 0);
        ReportScan();
    } else if (k7 && !p7) {
        if (!InterlockedCompareExchange(&g_camPosValid, 0, 0)) {
            Log("F7: no live camera yet - reach gameplay first");
        } else {
            g_hitCount = 0;
            InterlockedExchange(&g_scanArmed, 1);
            Log("F7: scanning one frame for the view matrix...");
        }
    } else if (!g_haveLock && InterlockedCompareExchange(&g_camPosValid, 0, 0) &&
               (InterlockedCompareExchange(&g_dupDraws, 0, 0) ||
                InterlockedCompareExchange(&g_vrProjection, 0, 0))) {
        // Nothing is modified without a lock now, so re-acquiring one has to be automatic -
        // needing an F7 press would leave stereo simply switched off. Arming here rather than
        // earlier in Present matters: the consume branch above runs first, so arming any sooner
        // would have the scan collected in the same call, before a single frame was scanned.
        // Rate-limited because a scan tests every window of every upload and is far too
        // expensive to leave running.
        static int rescanWait = 0;
        if (++rescanWait >= 20) {
            rescanWait = 0;
            g_hitCount = 0;
            InterlockedExchange(&g_scanArmed, 1);
        }
    }
    p7 = k7;

    UpdateFromHeadset(s);

    // Yaw is not carried by the detour's seam, so write it directly - the mechanism proven
    // in the earlier spike. Pitch deliberately left alone here; the detour owns it.
    //
    // ---- letting the mouse (and later, a thumbstick) coexist with head tracking ----
    //
    // Writing an ABSOLUTE yaw every frame silently discarded every other source of turning:
    // the engine would apply the mouse delta, then we would overwrite it, so the view visibly
    // snapped back. Reported in run 12 and it would have hit stick input in the shipped mod
    // just as hard - the same bug, not a testing-only inconvenience.
    //
    // The fix is to treat our own last write as the reference point. Anything that moved yaw
    // since then was somebody else's input, so fold that difference into the base the head
    // delta is measured from. Head tracking and mouse then add instead of fighting: yaw becomes
    // body-turn (mouse/stick) plus head-turn, which is the arrangement VR wants anyway.
    //
    // The int32 subtraction wraps correctly through the FRotator's 65536-per-turn space, so
    // crossing the wrap point needs no special case.
    if (InterlockedCompareExchange(&g_enabled, 0, 0) && g_haveWant) {
        uintptr_t ctl = FindController();
        if (ctl && Readable((void*)(ctl + ACTOR_ROTATION), 12)) {
            int32_t* rot = reinterpret_cast<int32_t*>(ctl + ACTOR_ROTATION);
            if (g_haveLastWrittenYaw) {
                int32_t externalDelta = rot[1] - g_lastWrittenYaw;
                if (externalDelta) {
                    g_baseYaw += externalDelta;
                    g_wantYaw += externalDelta;
                }
            }
            rot[1] = g_wantYaw;
            if (Readable((void*)(ctl + CTL_ROTATION_2), 12))
                reinterpret_cast<int32_t*>(ctl + CTL_ROTATION_2)[1] = g_wantYaw;
            g_lastWrittenYaw = g_wantYaw;
            g_haveLastWrittenYaw = true;
        }
    } else {
        g_haveLastWrittenYaw = false;   // stale reference once we stop steering
    }
    return g_origPresent(s, a, b, c, d);
}

void* PatchVTable(void* obj, int index, void* repl) {   // forward-declared up by Hook_CreateQuery
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

// ---- ⚠️ forcing the BACKBUFFER size does NOT raise UE3's render resolution ----
//
// Tried in run 14 and it broke stereo completely. The census told the story:
//
//     render-target census (scene is 3840x2160):
//          2560x1440  A8R8G8B8       7103 draws  left alone
//          2560x1440  A16B16G16R16F 19946 draws  left alone
//
// Zero draws split. UE3 allocates its scene render targets from its OWN configured resolution,
// not from the backbuffer, so overriding the backbuffer only created a mismatch: the engine kept
// rendering 2560x1440 while the backbuffer became 3840x2160. Our scene test compares the bound
// target against the backbuffer, so nothing matched and duplication silently switched itself off.
// The frame was then a mono 2560x1440 image in a 3840x2160 buffer - which is exactly the reported
// symptom, since slicing that in half gives the right eye 640 px of picture and 1280 px of black.
// The cull band measured that black precisely: "right=1280 of 3840".
//
// So resolution must be changed where the ENGINE reads it - the in-game video options - which
// makes it allocate scene targets at the new size and keeps backbuffer and scene consistent.
//
// The ini is kept because the shipped mod needs one for mode selection, but it no longer touches
// resolution: there is no point offering a setting that cannot work.
void LoadIniSettings() {
    static bool done = false;
    if (done) return;
    done = true;

    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&LoadIniSettings, &self);
    if (!GetModuleFileNameA(self, path, MAX_PATH)) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "SingularityVR.ini");

    if (GetPrivateProfileIntA("Render", "ResX", 0, path)) {
        Log("ini: [Render] ResX/ResY are IGNORED - forcing the backbuffer does not change UE3's");
        Log("     render resolution, it only desynchronises it. Use the in-game video options.");
    }

    // The zero-copy prerequisite. OFF by default: this replaces the resource management of a
    // shipping engine, so its failure mode is "the game does not start" and it must be one
    // relaunch away from off. See the big comment above Hook_CreateTexture.
    g_d3d9ExMode = GetPrivateProfileIntA("Render", "D3D9ExMode", 0, path);
    if (g_d3d9ExMode < 0 || g_d3d9ExMode > 1) g_d3d9ExMode = 0;
    Log("ini: D3D9ExMode=%d (%s)", g_d3d9ExMode,
        g_d3d9ExMode ? "upgrade the device to D3D9Ex and translate D3DPOOL_MANAGED -> DEFAULT"
                     : "plain D3D9, MANAGED left alone - the known-good path");

    // Escape hatch. Hooking the user-pointer draw calls stopped the game reaching its menus once
    // (run 24), and the cause is still unexplained - the vtable indices are confirmed correct
    // against d3d9.h, and the hooks are pass-through until F1 is pressed, so nothing they DO can
    // be responsible. Since a bad interaction here blocks launching entirely, it must be
    // switchable without a rebuild.
    // Levels rather than on/off, so the hang can be BISECTED without a rebuild each time.
    //
    // Three explanations for it have now been wrong - bad vtable index, bad signature, and file I/O
    // in the hook - and both index and signature are since confirmed exact against d3d9.h. Rather
    // than guess a fourth time, each level adds back one thing:
    //
    //   0 = not hooked at all                     (known good)
    //   1 = hooked, forwards immediately          (tests whether the PATCH ALONE breaks it)
    //   2 = level 1 plus the counter              (tests touching our own globals)
    //   3 = full treatment, routed via StereoPair (tests the duplication path)
    //
    // Whichever level first fails names the culprit exactly, and costs one relaunch instead of a
    // round trip through me.
    // See the note above Hook_CreateQuery. Off by default: this removes a feature the engine
    // expects to have, so it is a measurement, not a setting.
    g_occlusionMode = GetPrivateProfileIntA("Render", "OcclusionQueryMode", 0, path);
    if (g_occlusionMode < 0 || g_occlusionMode > 3) g_occlusionMode = 0;
    Log("ini: OcclusionQueryMode=%d (%s)", g_occlusionMode,
        g_occlusionMode == 0 ? "auto - report visible while true stereo is on, normal in mono" :
        g_occlusionMode == 1 ? "always report visible" :
        g_occlusionMode == 2 ? "never override - the engine's own occlusion culling stays live"
                             : "refuse to create - WARNING, this crashed the game in run 30");

    // The A/B for the readback ordering. Pipelined is the default because it is the better
    // ordering on paper, but "on paper" is exactly what has been wrong six times in this project,
    // so the old ordering stays one relaunch away and the log prints the phase breakdown for
    // whichever is running.
    g_copyMode = GetPrivateProfileIntA("Render", "FrameCopyMode", 1, path);
    if (g_copyMode < 0 || g_copyMode > 1) g_copyMode = 0;
    Log("ini: FrameCopyMode=%d (%s)", g_copyMode,
        g_copyMode == 0 ? "pipelined - read back the frame captured last time, one frame of latency"
                        : "immediate - capture and consume in the same Present (pre-run-31)");

    // Splits the round-trip into GPU wait and actual transfer. Only the transfer is what a
    // zero-copy path could remove, and the whole case for the MANAGED wrapper turns on which of
    // the two the 12.5 ms at 4K actually is. Switchable because it forces the GPU sync slightly
    // earlier than the driver would, and that should be provable as harmless rather than assumed.
    // Stage 2. Only ever active on an Ex device, so leaving this at 1 with D3D9ExMode=0 changes
    // nothing - the pair is the A/B: D3D9ExMode=1/ZeroCopy=0 against D3D9ExMode=1/ZeroCopy=1.
    g_splitColourOnly = GetPrivateProfileIntA("Render", "SplitColourTargetsOnly", 0, path);
    if (g_splitColourOnly < 0 || g_splitColourOnly > 1) g_splitColourOnly = 0;
    // Dump a handful of draws' worth of constants from the excluded target, then stop. Costs
    // nothing after the budget is spent, and the log stays readable.
    if (g_splitColourOnly) InterlockedExchange(&g_dumpShadowConsts, 4);
    Log("ini: SplitColourTargetsOnly=%d (%s)", g_splitColourOnly,
        g_splitColourOnly ? "split ONLY the two scene colour targets - excludes the third,"
                            " unidentified scene-sized surface (shadow test)"
                          : "split every scene-sized target, as before");

    g_zeroCopy = GetPrivateProfileIntA("Render", "ZeroCopy", 1, path);
    if (g_zeroCopy < 0 || g_zeroCopy > 1) g_zeroCopy = 1;
    Log("ini: ZeroCopy=%d (%s)", g_zeroCopy,
        g_zeroCopy ? "shared surface, GPU->GPU - needs D3D9ExMode=1 to take effect"
                   : "CPU round-trip through system memory");

    g_gpuFenceMode = GetPrivateProfileIntA("Render", "GpuFenceProbe", 1, path);
    if (g_gpuFenceMode < 0 || g_gpuFenceMode > 1) g_gpuFenceMode = 1;
    Log("ini: GpuFenceProbe=%d (%s)", g_gpuFenceMode,
        g_gpuFenceMode ? "split the copy into gpu-wait vs transfer"
                       : "off - copy stays a single merged number");

    g_userPtrHookLevel = GetPrivateProfileIntA("Render", "HookUserPointerDraws", 0, path);
    if (g_userPtrHookLevel < 0 || g_userPtrHookLevel > 3) g_userPtrHookLevel = 0;
    g_hookUserPtrDraws = g_userPtrHookLevel > 0;
    Log("ini: HookUserPointerDraws=%d (%s)", g_userPtrHookLevel,
        g_userPtrHookLevel == 0 ? "not hooked - known good" :
        g_userPtrHookLevel == 1 ? "patch only, immediate forward" :
        g_userPtrHookLevel == 2 ? "patch + counter" : "full StereoPair treatment");
}

HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* self, UINT ad, D3DDEVTYPE t, HWND w, DWORD f,
                                            D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    LoadIniSettings();

    HRESULT hr;
    if (g_d3d9ExMode && g_d3d9ex) {
        // CreateDeviceEx wants a D3DDISPLAYMODEEX for exclusive fullscreen and NULL for windowed.
        // The dev command line uses -windowed, but a fullscreen launch must not be left to chance.
        D3DDISPLAYMODEEX dm{ sizeof(D3DDISPLAYMODEEX) };
        D3DDISPLAYMODEEX* dmp = nullptr;
        if (pp && !pp->Windowed) {
            dm.Width  = pp->BackBufferWidth;
            dm.Height = pp->BackBufferHeight;
            dm.RefreshRate = pp->FullScreen_RefreshRateInHz;
            dm.Format = pp->BackBufferFormat;
            dm.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
            dmp = &dm;
        }
        IDirect3DDevice9Ex* devEx = nullptr;
        hr = g_d3d9ex->CreateDeviceEx(ad, t, w, f, pp, dmp, &devEx);
        if (SUCCEEDED(hr) && devEx) {
            *out = devEx;                  // IDirect3DDevice9Ex derives from IDirect3DDevice9
            g_deviceIsEx = true;
            Log("*** D3D9Ex device created - D3DPOOL_MANAGED will be translated to DEFAULT ***");
            Log("    (Ex does not lose DEFAULT resources on Reset, which is what makes this safe)");
        } else {
            // Fall back rather than take the game down with us. Without Ex the pool translation
            // must stay off too, or every MANAGED allocation would be silently changed for no
            // reason - g_deviceIsEx gates all of it.
            Log("*** CreateDeviceEx FAILED hr=0x%08lX - falling back to a plain D3D9 device ***", hr);
            Log("    set D3D9ExMode=0 to stop trying; the mod runs normally on the CPU copy path");
            hr = g_origCreateDevice(self, ad, t, w, f, pp, out);
        }
    } else {
        hr = g_origCreateDevice(self, ad, t, w, f, pp, out);
    }

    if (SUCCEEDED(hr) && out && *out && InterlockedExchange(&g_patched, 1) == 0) {
        g_gameDev = *out;

        // ---- ⚠️ is the 4096 cap actually the HARDWARE's? Nobody has ever asked ----
        //
        // Run 33 found -ResX=4096 accepted and 4992 refused, and attributed it to the classic
        // D3D9-era 4096 maximum texture dimension. That was a hypothesis. It is also the entire
        // argument for "side-by-side can never reach parity, so per-eye render targets are
        // mandatory" - a large architectural rewrite resting on an untested premise, which is
        // exactly the shape of mistake that cancelled the D3D9Ex work on SSW-confounded data.
        //
        // One call settles it. If MaxTextureWidth comes back 8192 or 16384, the limit is UE3's own
        // resolution validation rather than the GPU's, the side-by-side path that already runs at
        // 115 fps only needs a bigger number, and per-eye targets go back to being optional.
        {
            D3DCAPS9 caps{};
            if (SUCCEEDED((*out)->GetDeviceCaps(&caps))) {
                Log("device caps: MaxTextureWidth=%lu MaxTextureHeight=%lu MaxTextureAspectRatio=%lu"
                    " MaxSimultaneousTextures=%lu",
                    (unsigned long)caps.MaxTextureWidth, (unsigned long)caps.MaxTextureHeight,
                    (unsigned long)caps.MaxTextureAspectRatio,
                    (unsigned long)caps.MaxSimultaneousTextures);
                // 5376 is what side-by-side needs for 2688 per eye, the size the headset asked for.
                const bool hwAllows = caps.MaxTextureWidth >= 5376 && caps.MaxTextureHeight >= 2880;
                Log("    -> the hardware %s a 5376x2880 side-by-side target. The 4096 refusal in"
                    " run 33 was therefore %s.",
                    hwAllows ? "ALLOWS" : "does NOT allow",
                    hwAllows ? "UE3's own limit, NOT the GPU's - per-eye render targets may be"
                               " unnecessary"
                             : "a genuine hardware cap - per-eye render targets are required");
            } else {
                Log("GetDeviceCaps failed - cannot tell whether 4096 is a hardware limit");
            }
        }
        if (g_deviceIsEx) {
            // 23/24/25/26/27, verified against d3d9.h by the same enumeration that reproduces
            // Present 17, SetRenderTarget 37, DrawPrimitive 81 and SetVertexShaderConstantF 94.
            g_origCreateTexture       = (PFN_CreateTexture)PatchVTable(*out, 23, (void*)&Hook_CreateTexture);
            g_origCreateVolumeTexture = (PFN_CreateVolumeTexture)PatchVTable(*out, 24, (void*)&Hook_CreateVolumeTexture);
            g_origCreateCubeTexture   = (PFN_CreateCubeTexture)PatchVTable(*out, 25, (void*)&Hook_CreateCubeTexture);
            g_origCreateVertexBuffer  = (PFN_CreateVertexBuffer)PatchVTable(*out, 26, (void*)&Hook_CreateVertexBuffer);
            g_origCreateIndexBuffer   = (PFN_CreateIndexBuffer)PatchVTable(*out, 27, (void*)&Hook_CreateIndexBuffer);
            const bool allOk = g_origCreateTexture && g_origCreateVolumeTexture && g_origCreateCubeTexture
                            && g_origCreateVertexBuffer && g_origCreateIndexBuffer;
            if (!allOk) {
                // A null original means forwarding would jump through a null pointer on the very
                // first allocation. Nothing can be salvaged from a partial patch here.
                Log("*** a Create* patch FAILED - disabling pool translation to stay safe ***");
                g_deviceIsEx = false;
            } else {
                Log("resource creation hooks installed (CreateTexture/Volume/Cube/VB/IB)");
            }
        }
        g_origPresent = (PFN_Present)PatchVTable(*out, 17, (void*)&Hook_Present);
        // Slot 94 = SetVertexShaderConstantF. Present at 17 already proved this vtable
        // indexing against the real interface, so 94 needs no separate verification.
        g_origSetVSConstF = (PFN_SetVSConstF)PatchVTable(*out, 94, (void*)&Hook_SetVSConstF);
        // 81 = DrawPrimitive, 82 = DrawIndexedPrimitive - the two the scene passes use.
        g_origDrawPrim        = (PFN_DrawPrim)PatchVTable(*out, 81, (void*)&Hook_DrawPrim);
        g_origDrawIndexedPrim = (PFN_DrawIndexedPrim)PatchVTable(*out, 82, (void*)&Hook_DrawIndexedPrim);
        // 83/84 = the user-pointer draw forms, VERIFIED against d3d9.h rather than from memory:
        // enumerating IDirect3DDevice9's declarations and anchoring on the slots already known to
        // work (Present 17, SetRenderTarget 37, DrawPrimitive 81, SetVertexShaderConstantF 94)
        // gives a consistent offset and puts these two at 83 and 84. So the indices were right the
        // first time and "wrong index" was NOT why startup broke - that remains unexplained, which
        // is exactly why this is switchable from the ini without a rebuild.
        if (g_hookUserPtrDraws) {
            g_origDrawPrimUP        = (PFN_DrawPrimUP)PatchVTable(*out, 83, (void*)&Hook_DrawPrimUP);
            g_origDrawIndexedPrimUP = (PFN_DrawIndexedPrimUP)PatchVTable(*out, 84, (void*)&Hook_DrawIndexedPrimUP);
            Log("user-pointer draw hooks installed (83=%p, 84=%p)",
                (void*)g_origDrawPrimUP, (void*)g_origDrawIndexedPrimUP);
            // A null original means the patch failed and forwarding would crash - drop the hook
            // rather than take the game down with it.
            if (!g_origDrawPrimUP || !g_origDrawIndexedPrimUP) {
                Log("*** one of the user-pointer patches FAILED - disabling to stay safe ***");
                g_hookUserPtrDraws = false;
            }
        } else {
            Log("user-pointer draw hooks DISABLED by ini");
        }
        // 37 = SetRenderTarget, so draws aimed at shadow maps can be told apart from scene draws.
        g_origSetRenderTarget = (PFN_SetRenderTarget)PatchVTable(*out, 37, (void*)&Hook_SetRenderTarget);
        // 118 = CreateQuery, the last method on IDirect3DDevice9. Counted forward from
        // SetVertexShaderConstantF at 94, which is independently confirmed working.
        g_origCreateQuery = (PFN_CreateQuery)PatchVTable(*out, 118, (void*)&Hook_CreateQuery);
        if (!g_origCreateQuery && g_occlusionMode) {
            Log("*** CreateQuery patch FAILED - leaving occlusion queries alone ***");
            g_occlusionMode = 0;
        }
        Log("Present + SetVertexShaderConstantF + Draw + SetRenderTarget hooks installed.");
        Log("F7 find the matrix | F1 TRUE stereo | F12 alternate-eye | F10 6-DOF | F9 head tracking");
    }
    return hr;
}

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
    // The ini has to be read HERE, not in CreateDevice as everything else is: the choice between
    // a plain and an Ex factory object is made before the device exists, and it cannot be revised
    // afterwards.
    LoadIniSettings();

    IDirect3D9* d3d = nullptr;
    if (g_d3d9ExMode && p_C9Ex) {
        IDirect3D9Ex* ex = nullptr;
        HRESULT hr = p_C9Ex(sdk, &ex);
        if (SUCCEEDED(hr) && ex) {
            // Spike 1 verified an IDirect3D9Ex* is usable as a plain IDirect3D9* - it derives from
            // it, so the game never has to know. CreateDevice at slot 16 is at the same offset in
            // both vtables for the same reason.
            g_d3d9ex = ex;
            d3d = ex;
            Log("Direct3DCreate9Ex succeeded - handing the game an Ex factory");
        } else {
            Log("Direct3DCreate9Ex FAILED hr=0x%08lX - using plain D3D9", hr);
            g_d3d9ExMode = 0;
        }
    }
    if (!d3d) d3d = p_C9(sdk);
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
        // Stamp the run so a log can be told apart from the previous one at a glance. The file is
        // truncated at attach, but without a date there is no way to know whether what you are
        // reading is this run or the last one - which matters when the log is read from disk
        // rather than pasted, and cost real confusion across earlier sessions.
        {
            SYSTEMTIME st{}; GetLocalTime(&st);
            Log("=== view_matrix attached %04u-%02u-%02u %02u:%02u:%02u - run starts here ===",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        }
        if (!LoadReal()) { Log("FATAL: real d3d9.dll not loadable"); return FALSE; }
    }
    return TRUE;
}
