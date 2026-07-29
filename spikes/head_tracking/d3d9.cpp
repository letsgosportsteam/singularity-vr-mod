// head_tracking - Singularity VR mod, spike 6
//
// FIRST REAL VR BEHAVIOUR: the headset's orientation drives the in-game view.
//
// Deliberately does NOT render to the headset yet. The frame stays on the monitor; only the
// pose is consumed. That isolates the quaternion -> FRotator maths (which has sign and
// axis-convention traps) from the D3D9->D3D11->OpenXR submission path. Both halves are
// already proven separately:
//   spikes/openxr_chain   - 32-bit OpenXR session on VirtualDesktopXR, frame submitted
//   spikes/camera_write   - writing controller+0x60 turns the rendered view
//
// Controls:
//   F9  enable/disable head tracking (starts OFF; enabling recentres, so no view snap)
//   F8  recentre - treat the current head yaw as "straight ahead"
//   F5  cycle yaw sign      (axis conventions are easier to find than to derive)
//   F4  cycle pitch sign
//   F10 log current pose + what is being written
//
// Log: %LOCALAPPDATA%\SingularityVR\head_tracking.log

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

#include <cstdio>
#include <cstdint>
#include <cmath>

#pragma comment(lib, "d3d11.lib")

namespace {

// ---------------------------------------------------------------- logging
CRITICAL_SECTION g_lock;
char g_logPath[MAX_PATH] = {};
bool g_logReady = false;

void LogInit() {
    InitializeCriticalSection(&g_lock);
    char base[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) {
        strcat_s(base, "\\SingularityVR");
        CreateDirectoryA(base, nullptr);
        sprintf_s(g_logPath, "%s\\head_tracking.log", base);
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

// ---------------------------------------------------------------- engine access
const uintptr_t kGNamesTArray   = 0x01CD4A1C;
const uintptr_t kGObjectsTArray = 0x01CD4A4C;
const int OBJ_OUTER = 0x28, OBJ_NAME = 0x2C, OBJ_CLASS = 0x34;
const int ACTOR_ROTATION = 0x0060;     // FRotator {pitch, yaw, roll}
const int CAM_POV_FOV    = 0x0438;     // mCurrentPOV + 24

// The F6 diff found a SECOND rotator on the controller at +0x05D4 holding the same values
// as +0x0060. Yaw written to +0x0060 sticks but pitch does not, which fits UE3 applying yaw
// incrementally (our absolute write survives, mouse deltas add to it) and pitch absolutely
// from an accumulator (our write is overwritten every frame). +0x05D4 is the prime suspect
// for that accumulator.
const int CTL_ROTATION_2 = 0x05D4;
const int CAM_POV_PITCH  = 0x042C;   // mCurrentPOV + 12 (rotator inside the POV struct)

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

uintptr_t FindLive(const char* className, uintptr_t& cachedClass, uintptr_t& cached) {
    if (cached && cachedClass && Readable((void*)cached, 0x40) &&
        *reinterpret_cast<uintptr_t*>(cached + OBJ_CLASS) == cachedClass) return cached;
    cached = 0;
    if (!Readable((void*)kGObjectsTArray, 12)) return 0;
    uintptr_t data = *reinterpret_cast<uintptr_t*>(kGObjectsTArray);
    int32_t count = *reinterpret_cast<int32_t*>(kGObjectsTArray + 4);
    if (!data || count < 100) return 0;
    auto* objs = reinterpret_cast<uintptr_t*>(data);
    char nm[128], on[128];
    if (!cachedClass) {
        for (int i = 0; i < count; ++i) {
            if (!Readable((void*)(data + i*4), 4)) continue;
            uintptr_t o = objs[i];
            if (!o || !Readable((void*)o, 0x40)) continue;
            if (!NameOf(o, nm, sizeof(nm)) || _stricmp(nm, className) != 0) continue;
            uintptr_t ou = *reinterpret_cast<uintptr_t*>(o + OBJ_OUTER);
            if (ou && Readable((void*)ou, 0x40) && NameOf(ou, on, sizeof(on)) && !_stricmp(on, "RvGame")) {
                cachedClass = o; break;
            }
        }
        if (!cachedClass) return 0;
    }
    for (int i = 0; i < count; ++i) {
        if (!Readable((void*)(data + i*4), 4)) continue;
        uintptr_t o = objs[i];
        if (!o || !Readable((void*)o, 0x500)) continue;
        if (*reinterpret_cast<uintptr_t*>(o + OBJ_CLASS) != cachedClass) continue;
        if (!NameOf(o, nm, sizeof(nm)) || strncmp(nm, "Default__", 9) == 0) continue;
        cached = o;
        return o;
    }
    return 0;
}

// ---------------------------------------------------------------- OpenXR
XrInstance g_xrInstance = XR_NULL_HANDLE;
XrSession  g_xrSession  = XR_NULL_HANDLE;
XrSpace    g_xrSpace    = XR_NULL_HANDLE;
XrSystemId g_xrSystem   = XR_NULL_SYSTEM_ID;
ID3D11Device* g_dev11 = nullptr;
ID3D11DeviceContext* g_ctx11 = nullptr;
bool g_xrReady = false, g_xrRunning = false;
XrViewConfigurationType kViewCfg = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

bool InitXR() {
    const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };

    // VirtualDesktopXR implements OpenXR 1.0 only and rejects a 1.1 instance with
    // -4 INITIALIZATION_FAILED (which looks exactly like "no headset"). Ask for 1.0 first.
    XrVersion versions[] = { XR_MAKE_VERSION(1,0,34), XR_CURRENT_API_VERSION };
    XrResult r = XR_ERROR_RUNTIME_FAILURE;
    for (XrVersion v : versions) {
        XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
        strcpy_s(ici.applicationInfo.applicationName, "SingularityVR");
        ici.applicationInfo.apiVersion = v;
        ici.enabledExtensionCount = 1;
        ici.enabledExtensionNames = exts;
        r = xrCreateInstance(&ici, &g_xrInstance);
        if (XR_SUCCEEDED(r)) { Log("xrCreateInstance ok (api %llu.%llu)",
                                   XR_VERSION_MAJOR(v), XR_VERSION_MINOR(v)); break; }
        Log("xrCreateInstance rejected api %llu.%llu (%d)", XR_VERSION_MAJOR(v), XR_VERSION_MINOR(v), (int)r);
    }
    if (XR_FAILED(r)) { Log("no OpenXR instance - is the headset connected?"); return false; }

    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(g_xrInstance, &sgi, &g_xrSystem))) { Log("xrGetSystem failed"); return false; }

    PFN_xrGetD3D11GraphicsRequirementsKHR pfn = nullptr;
    xrGetInstanceProcAddr(g_xrInstance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&pfn);
    if (!pfn) { Log("no D3D11 graphics requirements fn"); return false; }
    XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    if (XR_FAILED(pfn(g_xrInstance, g_xrSystem, &req))) { Log("graphics requirements failed"); return false; }

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
    if (!chosen) { Log("no adapter matching runtime LUID"); return false; }

    D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_0 }, got{};
    if (FAILED(D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, lv, 1, D3D11_SDK_VERSION, &g_dev11, &got, &g_ctx11))) {
        Log("D3D11CreateDevice failed"); chosen->Release(); return false;
    }
    chosen->Release();

    XrGraphicsBindingD3D11KHR bind{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    bind.device = g_dev11;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &bind; sci.systemId = g_xrSystem;
    if (XR_FAILED(xrCreateSession(g_xrInstance, &sci, &g_xrSession))) { Log("xrCreateSession failed"); return false; }

    XrReferenceSpaceCreateInfo rs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    if (XR_FAILED(xrCreateReferenceSpace(g_xrSession, &rs, &g_xrSpace))) { Log("reference space failed"); return false; }

    Log("OpenXR ready - head tracking available (F9 to enable)");
    g_xrReady = true;
    return true;
}

void PumpXREvents() {
    XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(g_xrInstance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = kViewCfg;
                if (XR_SUCCEEDED(xrBeginSession(g_xrSession, &bi))) { g_xrRunning = true; Log("session running"); }
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_xrSession); g_xrRunning = false; Log("session stopped");
            }
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// Returns head orientation as XR quaternion. Frames must be pumped for poses to update.
bool GetHeadPose(XrQuaternionf& outQ, float& outFovDeg) {
    if (!g_xrReady || !g_xrRunning) return false;
    XrFrameState fs{ XR_TYPE_FRAME_STATE };
    if (XR_FAILED(xrWaitFrame(g_xrSession, nullptr, &fs))) return false;
    xrBeginFrame(g_xrSession, nullptr);

    bool ok = false;
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t n = 0;
    XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
    vli.viewConfigurationType = kViewCfg;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = g_xrSpace;
    XrViewState vs{ XR_TYPE_VIEW_STATE };
    if (XR_SUCCEEDED(xrLocateViews(g_xrSession, &vli, &vs, 2, &n, views)) && n >= 1 &&
        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        outQ = views[0].pose.orientation;
        // total horizontal FOV of one eye, in degrees
        outFovDeg = (fabsf(views[0].fov.angleLeft) + fabsf(views[0].fov.angleRight)) * 57.2957795f;
        ok = true;
    }

    // Submit nothing - we only want poses this spike. A frame must still be ended.
    XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = 0;
    xrEndFrame(g_xrSession, &fei);
    return ok;
}

// ---------------------------------------------------------------- pose maths
// OpenXR: right-handed, +X right, +Y up, -Z forward.
// UE3:    +X forward, +Y right, +Z up. FRotator is int32, 65536 == 360 degrees.
// Signs are notoriously easy to get backwards, so they are runtime-flippable (F5/F4).
const float kRadToUU = 65536.0f / 6.2831853f;

void QuatToPitchYaw(const XrQuaternionf& q, float& pitchRad, float& yawRad) {
    // yaw about XR +Y, pitch about XR +X
    float sinp = 2.0f * (q.w * q.x - q.y * q.z);
    if (sinp > 1.0f) sinp = 1.0f;
    if (sinp < -1.0f) sinp = -1.0f;
    pitchRad = asinf(sinp);
    yawRad = atan2f(2.0f * (q.w * q.y + q.z * q.x),
                    1.0f - 2.0f * (q.x * q.x + q.y * q.y));
}

// defined below with the pitch-hunt code
uintptr_t FindLivePawn();
// the BeginScene hook writes camera rotation; ApplyHeadTracking fills these in
extern int32_t g_wantPitch, g_wantYaw;
extern bool g_haveWant;
void WriteCameraRotationNow();

volatile LONG g_enabled = 0;
int  g_yawSign = -1, g_pitchSign = 1;
// UU-of-pitch-error to mouse-counts. Unknown until measured; proportional control tolerates
// a wrong value (too small = laggy, too large = oscillation). F3/F7 adjust it live.
float g_mouseGain = 0.02f;
// Pitch write targets. Run 2 showed our pitch sometimes SURVIVES in controller+0x60 yet the
// view still never tilts, so the camera is not reading pitch from there at all. Test every
// plausible sink, one at a time, with a big constant value (F2) so the answer is unmissable.
int  g_pitchTarget = 0;
const char* kPitchTargetName[] = {
    "ctl+0x60", "ctl+0x5D4", "cam+0x60", "cam POV+0x42C", "ALL",
};
const int kNumPitchTargets = 5;

// F2: ignore the head and slam a large constant pitch. ~+8000 UU is about 44 degrees up -
// impossible to miss if any of it reaches the view.
volatile LONG g_constPitch = 0;
const int32_t kConstPitchUU = 8000;
bool g_haveCentre = false;
float g_centreYaw = 0.0f;
int32_t g_baseGameYaw = 0;
int g_tick = 0;

void ApplyHeadTracking() {
    if (!g_xrReady) return;
    PumpXREvents();

    XrQuaternionf q{}; float fovDeg = 0.0f;
    if (!GetHeadPose(q, fovDeg)) return;
    if (!InterlockedCompareExchange(&g_enabled, 0, 0)) return;

    uintptr_t ctl = FindLive("RvPlayerController", g_ctlClass, g_controller);
    if (!ctl || !Readable((void*)(ctl + ACTOR_ROTATION), 12)) return;
    auto* rot = reinterpret_cast<int32_t*>(ctl + ACTOR_ROTATION);

    float pitchRad, yawRad;
    QuatToPitchYaw(q, pitchRad, yawRad);

    // Recentring: remember where the head and the game were pointing when enabled, then
    // apply head movement as a DELTA. Without this, enabling would snap the view.
    if (!g_haveCentre) {
        g_centreYaw = yawRad;
        g_baseGameYaw = rot[1];
        g_haveCentre = true;
        Log("recentred: head yaw %.1f deg, game yaw %d", yawRad * 57.2958f, g_baseGameYaw);
    }

    float dYaw = yawRad - g_centreYaw;
    while (dYaw >  3.14159265f) dYaw -= 6.2831853f;
    while (dYaw < -3.14159265f) dYaw += 6.2831853f;

    int32_t yawUU   = g_baseGameYaw + (int32_t)(g_yawSign   * dYaw     * kRadToUU);
    int32_t pitchUU =                 (int32_t)(g_pitchSign * pitchRad * kRadToUU);

    // UE3 clamps view pitch; stay inside the usual limits to avoid the engine fighting us.
    if (pitchUU >  16000) pitchUU =  16000;
    if (pitchUU < -16000) pitchUU = -16000;

    // Record what the engine left there BEFORE we write, so the periodic log shows whether
    // it is overwriting us (and with what) rather than us guessing.
    int32_t enginePitch = rot[0], engineYaw = rot[1];
    int32_t engine2Pitch = 0, engine2Yaw = 0;
    bool have2 = Readable((void*)(ctl + CTL_ROTATION_2), 12);
    if (have2) {
        auto* r2 = reinterpret_cast<int32_t*>(ctl + CTL_ROTATION_2);
        engine2Pitch = r2[0]; engine2Yaw = r2[1];
    }

    if (InterlockedCompareExchange(&g_constPitch, 0, 0)) pitchUU = kConstPitchUU;

    // The pitch diff showed +0x60 and +0x5D4 move in LOCKSTEP under mouse input
    // (both 0 -> 7680), and the earlier failure showed them DIVERGING when we wrote only
    // +0x60 (ours 8000, engine's 64576) - i.e. the engine keeps authority through whichever
    // one we leave alone. So always write both. Same for yaw, for consistency.
    rot[0] = pitchUU;
    rot[1] = yawUU;
    // roll deliberately left alone - UE3 view roll is often ignored or causes artefacts
    if (have2) {
        auto* r2 = reinterpret_cast<int32_t*>(ctl + CTL_ROTATION_2);
        r2[0] = pitchUU;
        r2[1] = yawUU;
    }

    // Pitch has now failed from the controller alone across several runs even when the value
    // provably persisted. Rather than cycle candidates behind a hotkey (which got cycled past
    // too fast to observe last time), write EVERY known pitch sink every frame. If the view
    // still will not tilt, no memory location we have found feeds it and the answer has to
    // come from Ghidra instead of memory hunting.
    g_wantPitch = pitchUU;
    g_wantYaw   = yawUU;
    g_haveWant  = true;
    WriteCameraRotationNow();   // keeps the menu case working

    // ---- pitch by SYNTHETIC MOUSE instead of by memory write ----
    // Seven attempts to set pitch directly all failed: the value lands and persists, but the
    // camera recomputes pitch from an upstream source in gameplay. The hardware breakpoint
    // showed the actual writer is generic AActor::SetRotation plumbing fed from a caller we
    // would have to chase a long way.
    //
    // The engine moves pitch perfectly well when the MOUSE asks it to - through clamping,
    // smoothing and whatever else it does. So drive that path rather than fight it: measure
    // the error between the head's pitch and the game's, and inject a relative mouse delta
    // to close it. Proportional control converges even with an imprecise gain.
    int32_t curPitch = enginePitch;
    int32_t err = pitchUU - curPitch;
    while (err >  32768) err -= 65536;
    while (err < -32768) err += 65536;

    // Only inject during actual gameplay. In menus the mouse drives a CURSOR, and the camera
    // stops updating so the error never closes - unchecked, that would pin the cursor to the
    // top or bottom of the screen and make menus unusable. The game hides the cursor during
    // gameplay and shows it in menus, which is a cheap and reliable discriminator.
    CURSORINFO ci{}; ci.cbSize = sizeof(ci);
    bool cursorVisible = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);

    // Failsafe: if the error refuses to shrink, we are not actually steering anything
    // (paused, cutscene, menu we failed to detect). Back off rather than spray input.
    static int stuckFrames = 0;
    static int lastAbsErr = 0;
    int absErr = err < 0 ? -err : err;
    if (absErr > 400 && absErr >= lastAbsErr - 8) stuckFrames++; else stuckFrames = 0;
    lastAbsErr = absErr;
    if (stuckFrames > 90) {
        if (stuckFrames == 91) Log("pitch injection backing off - error not closing (menu/paused?)");
        if (stuckFrames > 300) stuckFrames = 0;      // retry occasionally
    }

    bool mayInject = !cursorVisible && stuckFrames <= 90;
    if (mayInject) {
        int dy = (int)(err * g_mouseGain * g_pitchSign);
        if (dy >  200) dy =  200;      // keep single steps sane
        if (dy < -200) dy = -200;
        if (dy != 0) {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = MOUSEEVENTF_MOVE;   // relative
            in.mi.dx = 0;
            in.mi.dy = dy;
            SendInput(1, &in, sizeof(INPUT));
        }
    }

    // FOV from the headset (proven writable on the camera's POV)
    uintptr_t cam = FindLive("RvPlayerCamera", g_camClass, g_camera);
    if (cam && fovDeg > 30.0f && fovDeg < 160.0f && Readable((void*)(cam + CAM_POV_FOV), 4))
        *reinterpret_cast<float*>(cam + CAM_POV_FOV) = fovDeg;

    if (++g_tick % 120 == 0)
        Log("head p%.1f y%.1f -> want p%d y%d | engine had +0x60 p%d y%d, +0x5D4 p%d y%d | target=%s",
            pitchRad*57.2958f, yawRad*57.2958f, pitchUU, yawUU,
            enginePitch, engineYaw, engine2Pitch, engine2Yaw, kPitchTargetName[g_pitchTarget]);
}

// ---------------------------------------------------------------- pitch hunt
// Five candidate sinks all failed, including writing every one at once with an unmissable
// +8000 UU constant. So pitch reaches the view from somewhere we have not looked. Find it
// the way yaw was found: snapshot, move the MOUSE vertically only, diff.
//
// Snapshots the controller, camera and pawn. Mouse-only vertical look keeps the diff clean:
// anything that changes is on the pitch path.
const int kSnapBytes = 0x900;
uint8_t g_snapCtl[kSnapBytes], g_snapCam[kSnapBytes], g_snapPawn[kSnapBytes];
bool g_haveSnap = false;
uintptr_t g_pawnClass = 0, g_pawn = 0;

// The last diff reported "pawn: not found" - the exact class name is not RvPawn. Find any
// live actor whose CLASS name contains "Pawn" instead of guessing the full name.
uintptr_t FindLivePawn() {
    if (g_pawn && Readable((void*)g_pawn, 0x40)) return g_pawn;
    if (!Readable((void*)kGObjectsTArray, 12)) return 0;
    uintptr_t data = *reinterpret_cast<uintptr_t*>(kGObjectsTArray);
    int32_t count = *reinterpret_cast<int32_t*>(kGObjectsTArray + 4);
    if (!data) return 0;
    auto* objs = reinterpret_cast<uintptr_t*>(data);
    char nm[128], cn[128], on[128];
    for (int i = 0; i < count; ++i) {
        if (!Readable((void*)(data + i*4), 4)) continue;
        uintptr_t o = objs[i];
        if (!o || !Readable((void*)o, kSnapBytes)) continue;
        if (!NameOf(o, nm, sizeof(nm)) || strncmp(nm, "Default__", 9) == 0) continue;
        uintptr_t ou = *reinterpret_cast<uintptr_t*>(o + OBJ_OUTER);
        if (!ou || !Readable((void*)ou, 0x40) || !NameOf(ou, on, sizeof(on))) continue;
        if (_stricmp(on, "PersistentLevel") != 0) continue;     // a live actor
        uintptr_t cls = *reinterpret_cast<uintptr_t*>(o + OBJ_CLASS);
        if (!cls || !Readable((void*)cls, 0x40) || !NameOf(cls, cn, sizeof(cn))) continue;
        if (!strstr(cn, "Pawn") && !strstr(cn, "pawn")) continue;
        g_pawn = o;
        Log("live pawn found: 0x%08X name=%s class=%s", o, nm, cn);
        return o;
    }
    Log("no live pawn (class containing 'Pawn') found");
    return 0;
}

void PitchSnapshot() {
    uintptr_t ctl  = FindLive("RvPlayerController", g_ctlClass, g_controller);
    uintptr_t cam  = FindLive("RvPlayerCamera", g_camClass, g_camera);
    uintptr_t pawn = FindLivePawn();
    if (ctl  && Readable((void*)ctl,  kSnapBytes)) memcpy(g_snapCtl,  (void*)ctl,  kSnapBytes);
    if (cam  && Readable((void*)cam,  kSnapBytes)) memcpy(g_snapCam,  (void*)cam,  kSnapBytes);
    if (pawn && Readable((void*)pawn, kSnapBytes)) memcpy(g_snapPawn, (void*)pawn, kSnapBytes);
    g_haveSnap = true;
    Log("F6: snapshot (ctl=0x%08X cam=0x%08X pawn=0x%08X). Now look UP/DOWN with the MOUSE only, then F6 again.",
        ctl, cam, pawn);
}

void PitchDiff() {
    if (!g_haveSnap) { PitchSnapshot(); return; }
    uintptr_t ctl  = FindLive("RvPlayerController", g_ctlClass, g_controller);
    uintptr_t cam  = FindLive("RvPlayerCamera", g_camClass, g_camera);
    uintptr_t pawn = FindLivePawn();
    Log("");
    Log("=== PITCH DIFF (fields that changed while looking up/down) ===");
    struct S { const char* tag; uintptr_t base; uint8_t* snap; };
    S srcs[] = { { "controller", ctl, g_snapCtl }, { "camera", cam, g_snapCam }, { "pawn", pawn, g_snapPawn } };
    for (auto& s : srcs) {
        if (!s.base || !Readable((void*)s.base, kSnapBytes)) { Log("  -- %s: not found --", s.tag); continue; }
        Log("  -- %s @ 0x%08X --", s.tag, s.base);
        int shown = 0;
        for (int off = 0; off + 4 <= kSnapBytes && shown < 40; off += 4) {
            int32_t o = *reinterpret_cast<int32_t*>(s.snap + off);
            int32_t n = *reinterpret_cast<int32_t*>(s.base + off);
            if (o == n) continue;
            float f = *reinterpret_cast<float*>(s.base + off);
            // an FRotator pitch is an int that stays within roughly +/-16k, or wraps near 65536
            bool angleLike = (n > -20000 && n < 20000) || (n > 45000 && n < 65536);
            const char* hint = angleLike ? "  <-- ANGLE-like" : "";
            Log("     +0x%04X  %11d -> %11d  (float %.3f)%s", off, o, n, f, hint);
            shown++;
        }
    }
    Log("=== end pitch diff ===");
    g_haveSnap = false;
}

// ---------------------------------------------------------------- who writes pitch?
// Six guesses at the pitch source have all failed, so stop guessing: put a HARDWARE WRITE
// BREAKPOINT on the controller's pitch dword and let the CPU report the instruction that
// writes it. The resulting EIP maps straight to a function in the Ghidra database.
//
// Debug registers are per-thread, so the breakpoint is armed on every thread in the process.
// It disarms itself after a handful of unique hits to avoid any lasting overhead.
#include <tlhelp32.h>

volatile LONG g_bpArmed = 0;
uintptr_t g_bpAddr = 0;
uintptr_t g_bpSeen[16];
int g_bpSeenCount = 0;
PVOID g_veh = nullptr;

void SetDebugRegsAllThreads(uintptr_t addr, bool enable) {
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    int done = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                                   FALSE, te.th32ThreadID);
            if (!th) continue;
            bool self = (te.th32ThreadID == GetCurrentThreadId());
            if (!self) SuspendThread(th);
            CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &ctx)) {
                if (enable) {
                    ctx.Dr0 = addr;
                    // L0 enable | RW0=01 (write) | LEN0=11 (4 bytes)
                    ctx.Dr7 = (ctx.Dr7 & ~0xFULL) | 0x1ULL | (0x1ULL << 16) | (0x3ULL << 18);
                } else {
                    ctx.Dr0 = 0;
                    ctx.Dr7 &= ~0xF0001ULL;
                }
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (SetThreadContext(th, &ctx)) done++;
            }
            if (!self) ResumeThread(th);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    Log("debug registers %s on %d threads (addr 0x%08X)", enable ? "ARMED" : "cleared", done, addr);
}

LONG NTAPI PitchVEH(PEXCEPTION_POINTERS ei) {
    if (ei->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (!(ei->ContextRecord->Dr6 & 0x1)) return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t eip = (uintptr_t)ei->ContextRecord->Eip;
    bool fresh = true;
    for (int i = 0; i < g_bpSeenCount; ++i) if (g_bpSeen[i] == eip) { fresh = false; break; }
    if (fresh && g_bpSeenCount < 16) {
        g_bpSeen[g_bpSeenCount++] = eip;
        int32_t nowVal = Readable((void*)g_bpAddr, 4) ? *reinterpret_cast<int32_t*>(g_bpAddr) : 0;
        Log("PITCH WRITER #%d: EIP=0x%08X  (value now %d)  <-- look this up in Ghidra",
            g_bpSeenCount, eip, nowVal);
        if (g_bpSeenCount >= 8) {
            Log("collected 8 distinct writers - disarming");
            InterlockedExchange(&g_bpArmed, 0);
            SetDebugRegsAllThreads(0, false);
        }
    }
    ei->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void TogglePitchBreakpoint() {
    if (InterlockedCompareExchange(&g_bpArmed, 0, 0)) {
        InterlockedExchange(&g_bpArmed, 0);
        SetDebugRegsAllThreads(0, false);
        Log("F2: pitch write breakpoint DISARMED");
        return;
    }
    uintptr_t ctl = FindLive("RvPlayerController", g_ctlClass, g_controller);
    if (!ctl) { Log("F2: no controller"); return; }
    g_bpAddr = ctl + ACTOR_ROTATION;   // pitch is the first dword of the rotator
    g_bpSeenCount = 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, PitchVEH);
    InterlockedExchange(&g_bpArmed, 1);
    SetDebugRegsAllThreads(g_bpAddr, true);
    Log("F2: pitch write breakpoint ARMED at 0x%08X - now look up/down with the MOUSE", g_bpAddr);
}

// ---------------------------------------------------------------- BeginScene write
// The menu-vs-gameplay asymmetry (menu: pitch works, yaw does not; gameplay: the reverse)
// showed that writing CAMERA rotation does steer the view - it is simply overwritten by the
// camera's own per-frame update, because Present runs AFTER the scene is drawn.
//
// BeginScene runs after the game update but before the scene renders, so a camera write
// there should land inside the same frame that consumes it. Yaw keeps going to the
// controller at Present (proven), and now camera yaw is written too, which the menu result
// says was the only reason menu yaw failed.
int32_t g_wantPitch = 0, g_wantYaw = 0;
bool    g_haveWant = false;

void WriteCameraRotationNow() {
    if (!g_haveWant) return;
    uintptr_t cam = FindLive("RvPlayerCamera", g_camClass, g_camera);
    if (!cam) return;
    if (Readable((void*)(cam + ACTOR_ROTATION), 12)) {
        auto* r = reinterpret_cast<int32_t*>(cam + ACTOR_ROTATION);
        r[0] = g_wantPitch;
        r[1] = g_wantYaw;
    }
    static const int kSinks[4] = { 0x0338, 0x035C, CAM_POV_PITCH, 0x0464 };
    for (int i = 0; i < 4; ++i) {
        if (!Readable((void*)(cam + kSinks[i]), 12)) continue;
        auto* r = reinterpret_cast<int32_t*>(cam + kSinks[i]);
        r[0] = g_wantPitch;
        r[1] = g_wantYaw;
    }
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_BeginScene)(IDirect3DDevice9*);
PFN_BeginScene g_origBeginScene = nullptr;

HRESULT STDMETHODCALLTYPE Hook_BeginScene(IDirect3DDevice9* s) {
    if (InterlockedCompareExchange(&g_enabled, 0, 0)) WriteCameraRotationNow();
    return g_origBeginScene(s);
}

// ---------------------------------------------------------------- hooks
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
PFN_Present g_origPresent = nullptr;
volatile LONG g_xrInitTried = 0;

HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* s, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    if (InterlockedExchange(&g_xrInitTried, 1) == 0) InitXR();

    static bool p9=false, p8=false, p5=false, p4=false, p10=false;
    bool k9 =(GetAsyncKeyState(VK_F9) &0x8000)!=0, k8 =(GetAsyncKeyState(VK_F8) &0x8000)!=0;
    bool k5 =(GetAsyncKeyState(VK_F5) &0x8000)!=0, k4 =(GetAsyncKeyState(VK_F4) &0x8000)!=0;
    bool k10=(GetAsyncKeyState(VK_F10)&0x8000)!=0;

    if (k9 && !p9) {
        LONG was = InterlockedCompareExchange(&g_enabled, 0, 0);
        InterlockedExchange(&g_enabled, was ? 0 : 1);
        g_haveCentre = false;
        Log("F9: head tracking %s", was ? "OFF" : "ON");
    }
    if (k8 && !p8) { g_haveCentre = false; Log("F8: recentre"); }
    if (k5 && !p5) { g_yawSign   = -g_yawSign;   g_haveCentre = false; Log("F5: yaw sign %+d", g_yawSign); }
    if (k4 && !p4) { g_pitchSign = -g_pitchSign; Log("F4: pitch sign %+d", g_pitchSign); }
    static bool p3 = false, p2 = false;
    bool k3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
    bool k2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (k3 && !p3) { g_mouseGain *= 1.6f; Log("F3: mouse gain -> %.4f", g_mouseGain); }
    static bool p7 = false;
    bool k7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (k7 && !p7) { g_mouseGain /= 1.6f; Log("F7: mouse gain -> %.4f", g_mouseGain); }
    p7 = k7;
    if (k2 && !p2) TogglePitchBreakpoint();
    p3 = k3; p2 = k2;
    static bool p6 = false;
    bool k6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (k6 && !p6) PitchDiff();
    p6 = k6;
    if (k10 && !p10) Log("state: enabled=%ld xrReady=%d running=%d yawSign=%+d pitchSign=%+d",
                         InterlockedCompareExchange(&g_enabled,0,0), (int)g_xrReady, (int)g_xrRunning,
                         g_yawSign, g_pitchSign);
    p9=k9; p8=k8; p5=k5; p4=k4; p10=k10;

    ApplyHeadTracking();
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
        // BeginScene is vtable slot 41 on IDirect3DDevice9 - after the game update, before
        // the scene draws. That is where a camera-rotation write can survive to be used.
        g_origBeginScene = (PFN_BeginScene)PatchVTable(*out, 41, (void*)&Hook_BeginScene);
        Log("Present + BeginScene hooked. F9 enable, F8 recentre, F5 yaw sign, F4 pitch sign.");
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
        Log("=== head_tracking attached - HMD orientation drives the in-game view ===");
        Log("    starts OFF. connect the headset via Virtual Desktop, then press F9 in game.");
        if (!LoadReal()) { Log("FATAL: real d3d9.dll not loadable"); return FALSE; }
    }
    return TRUE;
}
