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
#include <intrin.h>       // _ReturnAddress, for the XInput caller census

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

// ---- âš ï¸ truncating on attach destroys the run you are comparing against (run 31) ----
//
// Any measurement worth making here is an A/B across launches - two ini settings, two runs, two
// numbers. Truncating at attach means launch B erases launch A, so by the time you have the
// second number you no longer have the first. Run 31 lost two of its three tests to exactly
// that, and the loss is silent: the file looks complete, it is simply the wrong run.
//
// So the previous logs are rotated rather than overwritten. Three back is enough for the
// three-launch comparisons this project actually runs.
// ⚠️ Raised from 3 in run 136. Three spares is under an hour of testing, and it cost a result:
// the full-resolution half of a resolution A/B had rotated out by the time the pair was compared,
// leaving four logs that were all 1440p. An A/B whose two halves are separate launches needs
// enough history to survive the relaunches BETWEEN them plus whatever is run afterwards, and these
// are a few hundred KB each.
const int kLogKeep = 12;

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

// SingularityVR.ini, always beside this DLL rather than beside the exe or in the working directory
// - the mod is dropped in next to the game binary and nothing about it should depend on where the
// game was launched from.
bool IniPath(char* out /*MAX_PATH*/) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&IniPath, &self)) return false;
    if (!GetModuleFileNameA(self, out, MAX_PATH)) return false;
    char* slash = strrchr(out, '\\');
    if (!slash) return false;
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - out), "SingularityVR.ini");
    return true;
}

// ---------------------------------------------------------------- timing primitives
// Up here rather than beside the frame-copy code that first needed them: the XInput hook times
// itself now, and that runs long before any of the render timing does.
inline LARGE_INTEGER Now() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t; }

inline double MsSince(const LARGE_INTEGER& t0) {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    return double(now.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
}

// ================================================================ automatic resolution (run 59)
//
// Every other VR title inherits its resolution from the headset. This one has needed a hand-typed
// `-ResX=4992 -ResY=2688 -windowed` since run 17, and run 58 finally proved the engine ACCEPTS the
// parity size - which is what makes automating it worth doing rather than merely tidy.
//
// ---- why the command line and not the backbuffer ----
//
// Run 14 settled this: overriding the D3D9 backbuffer in CreateDevice does NOT raise UE3's render
// resolution. The engine sizes its scene targets from its own configured resolution, so forcing the
// backbuffer only desynchronises the two and breaks stereo. The resolution has to reach UE3's own
// config, and the command line is the route that does that.
//
// ---- why the timing works ----
//
// This DLL is a d3d9.dll proxy, so it is a STATIC import of the exe: the loader runs our DllMain
// before it calls the exe's entry point. The CRT then reads GetCommandLineW inside that entry
// point to build WinMain's lpCmdLine, and UE3 parses it from there. So a detour installed in
// DllMain is in place before anything has looked at the command line - but only if it is installed
// in DllMain. By the time our first D3D export is called, the engine has long since parsed it.
//
// ---- why a CACHED size and not a live XR query ----
//
// The honest option was to spin up a temporary OpenXR instance in DllMain and ask the runtime.
// xrEnumerateViewConfigurationViews needs only an instance and a system id - no session, no
// graphics device - so it is technically possible. It also puts OpenXR initialisation into the
// process's startup path, where a failure means THE GAME DOES NOT LAUNCH.
//
// So instead the size the mod already queries and logs every run is written back to the ini, and
// the NEXT launch injects it. Change your headset's render scale and the first run afterwards uses
// the old number while the next one is correct. A wrong resolution for one run is a far better
// failure than a game that will not start.
int  g_autoResMode = 1;          // ini AutoResolution
UINT g_autoResW = 0, g_autoResH = 0;
bool g_autoResInjected = false;
char  g_cmdLineA[1024]{};
WCHAR g_cmdLineW[1024]{};

typedef LPSTR  (WINAPI *PFN_GetCmdA)(void);
typedef LPWSTR (WINAPI *PFN_GetCmdW)(void);
PFN_GetCmdA g_origGetCmdA = nullptr;
PFN_GetCmdW g_origGetCmdW = nullptr;

// Case-insensitive search for "-ResX" / "/ResX" so an explicit choice can be detected.
bool HasResArg(const char* s) {
    for (const char* p = s; *p; ++p) {
        if ((p[0] == '-' || p[0] == '/') &&
            (p[1] == 'R' || p[1] == 'r') && (p[2] == 'e' || p[2] == 'E') &&
            (p[3] == 's' || p[3] == 'S') && (p[4] == 'X' || p[4] == 'x')) return true;
    }
    return false;
}

// Builds the augmented command line once. Returns false to mean "use the original untouched",
// which is the answer for every failure path here - see the note above.
bool BuildAutoCmdLine(const char* orig) {
    if (!g_autoResMode || !orig) return false;
    if (!g_autoResW || !g_autoResH) return false;                 // nothing cached yet
    if (g_autoResW < 640 || g_autoResH < 480 ||
        g_autoResW > 16384 || g_autoResH > 16384) return false;   // refuse nonsense
    // ⚠️ AN EXPLICIT -ResX ON THE COMMAND LINE ALWAYS WINS. Injecting a second one would leave the
    // engine parsing two, and whichever it picked would silently override a deliberate choice.
    if (HasResArg(orig)) return false;

    const int n = _snprintf_s(g_cmdLineA, sizeof(g_cmdLineA), _TRUNCATE,
                              "%s -ResX=%u -ResY=%u -windowed", orig, g_autoResW, g_autoResH);
    if (n <= 0) return false;   // truncated or failed - do not hand back a mangled command line
    if (MultiByteToWideChar(CP_ACP, 0, g_cmdLineA, -1, g_cmdLineW,
                            sizeof(g_cmdLineW) / sizeof(g_cmdLineW[0])) == 0) return false;
    return true;
}

// Write the size the headset just asked for back to the ini, so the NEXT launch can inject it.
// Only rewritten when it actually changes, so a render-scale change costs one file write rather
// than one per launch. Deliberately writes nothing else: this is the only value the mod ever
// authors, and a settings file the user edits by hand should not be churned.
void CacheAutoResolution(UINT sideBySideW, UINT h) {
    if (!sideBySideW || !h) return;
    if (g_autoResW == sideBySideW && g_autoResH == h) return;    // already correct
    char path[MAX_PATH]{};
    if (!IniPath(path)) return;
    char v[32]{};
    _snprintf_s(v, sizeof(v), _TRUNCATE, "%u", sideBySideW);
    WritePrivateProfileStringA("Render", "AutoResX", v, path);
    _snprintf_s(v, sizeof(v), _TRUNCATE, "%u", h);
    WritePrivateProfileStringA("Render", "AutoResY", v, path);
    Log("auto-resolution: cached %ux%u to the ini - the NEXT launch will use it automatically",
        sideBySideW, h);
}

LPSTR WINAPI Hook_GetCommandLineA(void) {
    LPSTR orig = g_origGetCmdA ? g_origGetCmdA() : nullptr;
    return (g_autoResInjected && orig) ? g_cmdLineA : orig;
}
LPWSTR WINAPI Hook_GetCommandLineW(void) {
    LPWSTR orig = g_origGetCmdW ? g_origGetCmdW() : nullptr;
    return (g_autoResInjected && orig) ? g_cmdLineW : orig;
}

// ================================================================ the gamepad path (run 60)
//
// Singularity.exe statically imports XINPUT1_3.dll - ordinals 2 and 3, which are XInputGetState
// and XInputSetState. It is a 2010 console port, so Raven's whole 360-pad path is already
// compiled in: analog move, analog look, buttons, rumble, and - the part worth the most here -
// MENU NAVIGATION.
//
// That reframes the last rung entirely. The mod does not have to teach UE3 about controllers,
// invent a laser pointer for menus that are currently inert, or synthesise keystrokes at the OS
// level (tried for view rotation in spike 5 and rejected: closed-loop, laggy, special-cased in
// menus). It has to impersonate ONE Xbox 360 pad and let the game's own input code do the rest -
// including its dead zones, its acceleration curves and its menu focus handling, none of which
// then have to be written or tuned here.
//
// ---- why detour the FUNCTION and not the import table ----
//
// The exe imports these BY ORDINAL. There is no "XInputGetState" string anywhere in its import
// table, so an IAT patch keyed on the name would have found nothing to patch and quietly reported
// success - the ninth tautological instrument this project would have built. MinHook rewrites the
// prologue inside XINPUT1_3.dll itself, which catches the caller however it resolved the address.
// GetProcAddress by name is still the way to FIND it: the DLL exports the name regardless of how
// the importer chose to ask.
//
// ---- what this probe settles, before any OpenXR input code exists ----
//
//   1. Is the poll live at all? If the engine asked once at startup, found no pad and gave up,
//      the call count stays at zero and this approach is dead. One run to find out.
//   2. Does the game accept a pad that is not there? NUMPAD3 says one is plugged in.
//   3. Does engine-driven turning coexist with head tracking? NUMPAD5 holds the look stick over
//      and the run-12 external-delta fold-in either absorbs it or fights it. That question has
//      to be answered before a thumbstick is wired to anything.
//
// Structs are declared here rather than included from <xinput.h> deliberately: the SDK header
// describes XInput 1.4, this is 1.3, and the two agree on these layouts only by convention. Two
// fixed-ABI structs are cheaper to read than that caveat.
struct XiGamepad {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX, sThumbLY;
    SHORT sThumbRX, sThumbRY;
};
struct XiState { DWORD dwPacketNumber; XiGamepad Gamepad; };
struct XiVibration { WORD wLeftMotorSpeed, wRightMotorSpeed; };
typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XiState*);
typedef DWORD (WINAPI *PFN_XInputSetState)(DWORD, XiVibration*);

const DWORD kXiOk           = 0;      // ERROR_SUCCESS
const DWORD kXiNotConnected = 1167;   // ERROR_DEVICE_NOT_CONNECTED

const WORD XI_DPAD_UP = 0x0001, XI_DPAD_DOWN = 0x0002, XI_DPAD_LEFT = 0x0004,
           XI_DPAD_RIGHT = 0x0008, XI_START = 0x0010, XI_BACK = 0x0020,
           XI_LTHUMB = 0x0040, XI_RTHUMB = 0x0080, XI_LSHOULDER = 0x0100,
           XI_RSHOULDER = 0x0200, XI_A = 0x1000, XI_B = 0x2000, XI_X = 0x4000, XI_Y = 0x8000;

PFN_XInputGetState g_origXInputGetState = nullptr;
PFN_XInputSetState g_origXInputSetState = nullptr;

// Probe state shared by both hooks. See the run-72 note further down for what they are for.
volatile LONG g_freezePacket = 0;    // numpad - : never advance dwPacketNumber
volatile LONG g_setStateCalls = 0;   // rumble path activity, counted per second

// The right trigger straight from OpenXR, 0-255, independent of whether the pad is currently
// plugged in. The script census splits on this so an unplugged pad cannot silently empty it.
volatile LONG g_rawTrigger = 0;

// Set once the OpenXR action set is attached. Lives here rather than with the input code because
// both halves read it: the input code to know it may publish, the test overrides to know whether
// anything else is writing the pad at all.
bool g_xrInputReady = false;

volatile LONG g_padEmu      = 0;   // NUMPAD3 - report a connected pad on port 0
volatile LONG g_padWalkTest = 0;   // NUMPAD4 - hold the move stick forward
volatile LONG g_padLookTest = 0;   // NUMPAD5 - hold the look stick right

// The published pad state, packed into three LONGs so the hook can read it without a lock.
// XInputGetState is called from whichever thread the engine ticks input on, which is not
// guaranteed to be the one Present runs on, and a critical section in a per-frame engine callback
// is a cost this project has already paid once for (run 24's file I/O in a draw hook). Tearing
// between the three words is one frame of mixed input at 120 Hz and is not observable; tearing
// WITHIN a word is what would matter, and interlocked access rules it out.
volatile LONG g_padBtns   = 0;   // low 16: wButtons          high 16: rTrigger<<8 | lTrigger
volatile LONG g_padLStick = 0;   // low 16: sThumbLX          high 16: sThumbLY
volatile LONG g_padRStick = 0;   // low 16: sThumbRX          high 16: sThumbRY

LONG PackStick(int16_t x, int16_t y) {
    return (LONG)(((uint32_t)(uint16_t)y << 16) | (uint16_t)x);
}

// ---- rumble, which Raven already authored and nothing was listening to (run 62) ----
//
// The exe imports ordinal 3 as well as ordinal 2, and the config names
// `WinDrv.XnaForceFeedbackManager` with a `bForceFeedbackEnabled` switch and
// `Engine.ForceFeedbackWaveform` assets. So every weapon, impact and scripted jolt in the game
// already has force feedback authored against it, aimed at a 360 pad that was never there.
//
// Forwarding it to the controllers costs one more hook and gets the whole authored set for free -
// the cheapest content in this project, because somebody else wrote all of it in 2010.
//
// 360 motors are asymmetric by design: LEFT is the large low-frequency weight, RIGHT the small
// high-frequency one. Sending each to the hand of the same name keeps that asymmetry meaningful
// rather than averaging it away.
volatile LONG g_rumbleL = 0, g_rumbleR = 0;   // 0..65535, as the game sends them

DWORD WINAPI Hook_XInputSetState(DWORD idx, XiVibration* v) {
    InterlockedIncrement(&g_setStateCalls);
    if (InterlockedCompareExchange(&g_padEmu, 0, 0) && idx == 0) {
        if (v) {
            InterlockedExchange(&g_rumbleL, v->wLeftMotorSpeed);
            InterlockedExchange(&g_rumbleR, v->wRightMotorSpeed);
        }
        // Answered here rather than passed on, for the same reason as GetState: there is no
        // physical pad on port 0, so the real call can only fail, and slowly.
        return kXiOk;
    }
    return g_origXInputSetState ? g_origXInputSetState(idx, v) : kXiNotConnected;
}

DWORD g_padPacket = 0;
XiGamepad g_padLast{};

// ---- ✅ run 70: SOLVED. It is the packet number, and the cause is stick NOISE ----
//
//     pad off, feed ON,  in hand : 119.1 fps | residual 4.02 | MISSED 0.7%
//     pad off, feed ON,  on desk : 119.8 fps | residual 3.31 | MISSED 0.6%
//     pad ON,  feed off, in hand : 106.7 fps | residual 4.24 | MISSED 2.2%
//     pad ON,  feed ON,  on desk :  91.5 fps | residual 5.97 | MISSED 8.4%
//     pad ON,  feed ON,  in hand :  76.6 fps | residual 10.88 | MISSED 11.5%
//
// Draw counts sit between 1163 and 1638 across every row, so the activity confound that ruined run
// 68 is controlled this time. The dominant variable is **the pad, not the controllers** - with the
// pad off it is 119 fps and 0.6% missed in BOTH controller states, which is the one comparison in
// the table where the controller position provably does not matter.
//
// And the rows are monotonic in HOW OFTEN THE PAD STATE CHANGES:
//
//   pad absent            -> no input processing at all        0.7% missed
//   pad present, frozen   -> state identical every frame       2.2% missed
//   pad present, on desk  -> some jitter                       8.4% missed
//   pad present, in hand  -> maximum jitter                   11.5% missed
//
// The mechanism: `dwPacketNumber` advances whenever the state differs, and a game uses that to
// skip re-processing an unchanged pad. Our stick values come straight from raw OpenXR thumbstick
// readings, which are NOISY - a resting controller still reports a few thousandths that wobble
// every frame. So the state differed every frame, the packet advanced every frame, and UE3 ran its
// full input path - fourteen chain buttons through UnrealScript - 120 times a second, for input
// nobody gave. A real 360 pad at rest reports a hard zero and the game skips all of it.
//
// This is also why "in hand" measured worse than "on desk": a held controller jitters more. The
// hand/desk correlation was real, and it was a proxy for noise the whole time.
//
// Fix: a radial deadzone, so a resting stick produces a BIT-IDENTICAL state frame to frame and the
// packet stops advancing. Not a comfort tweak - it is the difference between the engine skipping
// its input path and running it every frame.
DWORD g_padPacketAdvances = 0;    // per second: ~0 at rest is the whole point

// ---- ❌ run 71: the deadzone WORKED and the judder did NOT go away. Run 70 is retracted ----
//
// Packet advances fell from ~120/sec to 5-60/sec, so the noise diagnosis was right and the fix did
// what it claimed. The judder stayed. **Fixing the mechanism I blamed did not fix the symptom, so
// it was not the mechanism.** Keeping the deadzone regardless - it is correct on its own terms and
// matches Raven's own configured values - but it is not the answer.
//
// What the same log shows instead, and what nothing had been looking at:
//
//     xinput: 938 calls/sec ... at 27 fps      -> ~33 polls PER FRAME
//     xinput:  72 calls/sec ... at 63 fps      -> ~1 poll per frame
//
// The game polls XInput **thirty-three times a frame** during the bad stretches and once a frame
// when it is healthy. Run 60 established that one poll per frame is the normal, promoted state, so
// this is something else entirely.
//
// ⚠️ And 900 calls a second through this detour is perhaps 2 ms of CPU across a WHOLE second, so
// the polling itself cannot be the cost. It is a SYMPTOM - the visible edge of the game's input
// path running 33 times per frame, and whatever that path drags with it is where the milliseconds
// are. Chasing the poll count directly would be optimising the thermometer.
//
// So find out who is calling. The return address separates "one site in a loop" from "many
// subsystems each asking once", which are different bugs with different fixes, and it hands over an
// address that can be looked up in Ghidra rather than argued about.
struct XiCaller { uintptr_t addr; DWORD count; };
XiCaller g_xiCallers[12]{};
int   g_xiCallerN = 0;
double g_msInHook = 0.0;

// ---- ❌ run 72: the 900 polls/sec were the LOADING SCREEN. Run 71's lead is dead ----
//
// One caller, `0x011D4795`, always. And correlated against the frame log it reads:
//
//     912 polls/sec @ 27 fps, **0 draws**   <- loading screen, polling for "press any button"
//      77 polls/sec @ 72 fps, 1730 draws    <- gameplay, one poll per frame
//     122 polls/sec @ 120 fps, 1695 draws   <- gameplay, one poll per frame
//
// In gameplay it is one poll per frame throughout, exactly as run 60 established. The 33-per-frame
// figure was a loading screen doing what loading screens do, and it had nothing to do with the
// judder. Our hook costs 0.18 ms per SECOND, so it was never a candidate either.
//
// What the frame budget actually shows across the same transition, at a constant ~1700 draws:
//
//     72 fps : xrWaitFrame 0.03 idle, our work 13.7 ms, XR submit 0.09
//    120 fps : xrWaitFrame 3.90 idle, our work  3.4 ms, XR submit 1.10
//
// So the app is genuinely compute-bound in the bad stretch - not paced, not stalled on the link
// (submit is FASTER when we are slow, which is what an app arriving late looks like), and drawing
// the same amount. **10 ms a frame appears and disappears inside "our work" at constant draw
// count.** The display period never changes, so it is not a refresh-rate drop either.
//
// The one clean, draw-controlled comparison still standing is run 70's: pad OFF gives 119 fps and
// 0.6% missed in BOTH controller states; pad ON gives 76-91 fps. Since our own hook is free, the
// cost has to be what UE3 does when it believes a pad is present.
//
// Two candidates remain, and they split cleanly:
//   (a) INPUT PROCESSING - the engine re-runs its chain-button path whenever dwPacketNumber moves.
//   (b) MERE PRESENCE - `WinDrv.XnaForceFeedbackManager` runs a waveform evaluator every frame once
//       a pad exists, regardless of whether anything is pressed.
//
// Freezing the packet number tells them apart in one keypress: (a) collapses, (b) does not.
// (g_freezePacket and g_setStateCalls are declared up with the other hook state - both hooks
// touch them and Hook_XInputSetState is defined before this point.)

// ---- ❌ run 73: the packet number is not read at all, and the pad may be innocent ----
//
// Freezing dwPacketNumber changed nothing - **and the controls kept working.** The second half is
// the real result: if the engine still responded to sticks and buttons while being told the state
// never changed, then it does not consult the packet number. So run 70's whole theory was wrong
// twice over, not just once, and the deadzone stays only because it is right on its own terms.
//
// SetState/sec tracks calls/sec exactly (74 vs 76, 115 vs 117), so the force-feedback manager runs
// once a frame - present, but our hook answers it in nanoseconds.
//
// ---- ⭐ what the config says, and it explains the entire session ----
//
//     bSmoothFrameRate     = TRUE
//     MinSmoothedFrameRate = 22
//     MaxSmoothedFrameRate = 121
//
// UE3's frame-rate smoother keeps a history of recent frame times, picks a target from it, clamps
// to [Min, Max] - and then **SLEEPS to hold the frame rate down to that target.** It is not a cap
// at 121; it is a governor that follows recent history. One hitch drops the target, and the engine
// then actively holds you there until the history rolls over.
//
// That is the missing mechanism, and it fits every anomaly at once:
//
//   * **72.0 fps for four samples in a row, then 120.0** - locked values, no key pressed, draw
//     count unchanged at ~1700. Compute-bound apps do not produce round stable numbers; governors
//     do.
//   * **The sleep lands in "our work"** - it happens inside the game's own tick, between our
//     Present hooks, where xrWaitFrame is not waiting (0.03 ms idle) and nothing else can see it.
//   * **Every A/B this session has been noisy and contradictory**, including ones that should have
//     been clean, because the frame rate was being held by hysteresis that outlasted whatever was
//     being toggled. "I pressed X and it got better" and "I picked up the controllers and it got
//     better" are the same coincidence: the history rolled over.
//
// ⚠️ This is the run-56 pattern for a third time: an invisible external governor moving underneath
// every comparison. The instrument is the same shape too - do not redesign around it, MEASURE it.
// UE3's smoother sleeps through appSleep, which is ::Sleep on Win32, so a hook on Sleep reports the
// governor's cost directly and a switch on that hook tests the fix in the same run.
typedef void (WINAPI *PFN_Sleep)(DWORD);
PFN_Sleep g_origSleep = nullptr;
volatile LONG g_killSleep = 0;      // numpad + : neutralise the governor on the game thread
volatile LONG g_sleepCalls = 0;
volatile LONG g_sleepMs = 0;
DWORD g_mainThreadId = 0;           // set at the first Present - see Hook_Present

void WINAPI Hook_Sleep(DWORD ms) {
    // Only the thread that renders, and only short sleeps. A process-wide Sleep(0) would spin
    // every worker and streaming thread the game owns, which would cost far more than it saved and
    // would look exactly like the bug being chased. Long sleeps are somebody deliberately waiting.
    if (GetCurrentThreadId() == g_mainThreadId && ms > 0 && ms <= 50) {
        InterlockedIncrement(&g_sleepCalls);
        InterlockedAdd(&g_sleepMs, (LONG)ms);
        if (InterlockedCompareExchange(&g_killSleep, 0, 0)) { if (g_origSleep) g_origSleep(0); return; }
    }
    if (g_origSleep) g_origSleep(ms);
}

// Call statistics. These are the whole point of the first run: a zero here kills the approach.
volatile LONG g_xiCalls = 0;
volatile LONG g_xiCallsByIndex[4] = { 0, 0, 0, 0 };
ULONGLONG g_xiLastStatsTick = 0;
LONG      g_xiLastStatsCalls = 0;

// Set from Present each frame: the hand's aim in engine units, and the live controller object.
// The XInput hook cannot go looking for either - FindController walks GObjects, which is far too
// expensive for a function the engine polls every frame and may not even call on our thread.
volatile LONG g_aimYawForXi = 0, g_aimPitchForXi = 0;
volatile LONG g_ctlForXi = 0;

// Defined next to the Present-time rotation write it mirrors - the two are the same operation at
// two different instants in the tick, and keeping them apart is how they get out of step.
void WriteAimAtInputPoll();

DWORD WINAPI Hook_XInputGetState(DWORD idx, XiState* st) {
    LARGE_INTEGER tHook = Now();

    // ---- mode 5: write the hand at INPUT POLL time instead of at Present ----
    //
    // Mode 4 put the hand into AActor::Rotation at Present and let the camera update overwrite it
    // with the head. The shot followed the head, which locates the fire trace AFTER the camera
    // update in the tick - a real ordering fact, and the first one this project has had.
    //
    // So the write has to land later than Present does. This is the only other seam the mod owns
    // that runs INSIDE the tick: the engine polls XInput once a frame from a single call site
    // (0x011D4795, established run 72). If that poll happens after the camera update, the hand
    // survives to the fire trace while the camera has already taken the head - which is the whole
    // arrangement, reached without a symbol.
    //
    // ⚠️ Equally it may poll input BEFORE the camera update, in which case the head overwrites us
    // again and this is the end of the time-split idea. Binary, one run, and the two candidate
    // orderings are the only two there are.
    WriteAimAtInputPoll();
    InterlockedIncrement(&g_xiCalls);
    if (idx < 4) InterlockedIncrement(&g_xiCallsByIndex[idx]);

    // Who called us. MinHook patches the target's prologue and reaches the detour through a
    // trampoline, so the return address on this frame is the GAME's call site, not MinHook's.
    // Linear scan over at most twelve entries, which is cheaper than any structure that would
    // avoid it at this call count.
    {
        const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
        int slot = -1;
        for (int i = 0; i < g_xiCallerN; ++i) if (g_xiCallers[i].addr == ra) { slot = i; break; }
        if (slot < 0 && g_xiCallerN < 12) { slot = g_xiCallerN++; g_xiCallers[slot].addr = ra; }
        if (slot >= 0) ++g_xiCallers[slot].count;
    }

    const bool emu = InterlockedCompareExchange(&g_padEmu, 0, 0) != 0;

    // ⚠️ While emulating, the real function is NOT called - and that is a saving, not a shortcut.
    // XInputGetState against an absent device is the known-expensive case (it re-enumerates), and
    // the game is already paying it every frame today. Answering from our own state removes that
    // from the frame budget rather than adding to it. The real result is sampled once at install
    // time instead, which is all the diagnostic ever needed it for.
    if (!emu || idx != 0 || !st) {
        const DWORD r = (emu && idx != 0)
                      ? kXiNotConnected                 // exactly one pad, on port 0
                      : (g_origXInputGetState ? g_origXInputGetState(idx, st) : kXiNotConnected);
        g_msInHook += MsSince(tHook);
        return r;
    }

    const LONG b = InterlockedCompareExchange(&g_padBtns, 0, 0);
    const LONG l = InterlockedCompareExchange(&g_padLStick, 0, 0);
    const LONG r = InterlockedCompareExchange(&g_padRStick, 0, 0);

    XiGamepad g{};
    g.wButtons      = (WORD)(b & 0xFFFF);
    g.bLeftTrigger  = (BYTE)((b >> 16) & 0xFF);
    g.bRightTrigger = (BYTE)((b >> 24) & 0xFF);
    g.sThumbLX = (SHORT)(l & 0xFFFF);  g.sThumbLY = (SHORT)((l >> 16) & 0xFFFF);
    g.sThumbRX = (SHORT)(r & 0xFFFF);  g.sThumbRY = (SHORT)((r >> 16) & 0xFFFF);

    // dwPacketNumber must only advance when something actually changed. Games use it to skip
    // re-reading an unchanged pad, and one that increments every call defeats that; one that never
    // increments can make the game ignore us entirely. Both failure modes are silent.
    if (memcmp(&g, &g_padLast, sizeof(g)) != 0) {
        g_padLast = g;
        // Frozen deliberately when probing: the pad stays connected and keeps reporting live stick
        // and button values, but the engine's "has anything changed" test always says no. Controls
        // are expected to stop responding while this is on - that is the point, not a side effect.
        if (!InterlockedCompareExchange(&g_freezePacket, 0, 0)) ++g_padPacket;
        ++g_padPacketAdvances;
    }

    st->dwPacketNumber = g_padPacket;
    st->Gamepad = g;
    g_msInHook += MsSince(tHook);
    return kXiOk;
}

// Returns true once the hook is live. Called from DllMain first and retried from Present, because
// XINPUT1_3.dll and this DLL are both static imports of the exe and the loader's order between
// them is not ours to assume.
bool InstallXInputHook() {
    if (g_origXInputGetState) return true;

    HMODULE h = GetModuleHandleA("XINPUT1_3.dll");
    if (!h) return false;                     // not loaded yet - Present will try again

    void* target = (void*)GetProcAddress(h, "XInputGetState");
    if (!target) { Log("xinput: XInputGetState not exported - gamepad path unavailable"); return true; }

    // ON by default. UE3 enumerates ports at ~1/sec round-robin, so a pad that only appears on a
    // keypress is absent for the whole main menu - the one place it is most needed. See the note
    // above InitXRInput for why this is not the run-36 mistake repeated.
    {
        char path[MAX_PATH]{};
        if (IniPath(path))
            InterlockedExchange(&g_padEmu, GetPrivateProfileIntA("Input", "Gamepad", 1, path) ? 1 : 0);
        else
            InterlockedExchange(&g_padEmu, 1);
    }

    const MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
        Log("xinput: MH_Initialize failed (%d)", (int)mhInit); return true;
    }
    if (MH_CreateHook(target, (void*)&Hook_XInputGetState,
                      (void**)&g_origXInputGetState) != MH_OK) {
        Log("xinput: MH_CreateHook failed at %p", target); return true;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("xinput: MH_EnableHook failed"); g_origXInputGetState = nullptr; return true;
    }

    // The frame-rate governor's sleep. Installed here because MinHook is already up and this runs
    // once; a failure costs the measurement and nothing else.
    if (HMODULE k32 = GetModuleHandleA("kernel32.dll")) {
        if (void* sleepTarget = (void*)GetProcAddress(k32, "Sleep")) {
            if (MH_CreateHook(sleepTarget, (void*)&Hook_Sleep, (void**)&g_origSleep) == MH_OK &&
                MH_EnableHook(sleepTarget) == MH_OK) {
                Log("sleep: hooked kernel32!Sleep - UE3's frame-rate smoother sleeps through this."
                    " NUMPAD+ neutralises it on the render thread.");
            } else {
                g_origSleep = nullptr;
                Log("sleep: hook failed - the governor cannot be measured this run");
            }
        }
    }

    // Rumble. A failure here costs haptics and nothing else, so it does not gate the input path.
    if (void* setTarget = (void*)GetProcAddress(h, "XInputSetState")) {
        if (MH_CreateHook(setTarget, (void*)&Hook_XInputSetState,
                          (void**)&g_origXInputSetState) == MH_OK &&
            MH_EnableHook(setTarget) == MH_OK) {
            Log("xinput: hooked XInputSetState - the game's authored force feedback will drive"
                " controller haptics");
        } else {
            g_origXInputSetState = nullptr;
            Log("xinput: XInputSetState hook failed - no haptics, everything else unaffected");
        }
    }

    // Sample the real state of all four ports once, so "is a physical pad plugged in" is on the
    // record and cannot be confused later with our own synthetic one.
    for (DWORD i = 0; i < 4; ++i) {
        XiState s{};
        const DWORD rr = g_origXInputGetState(i, &s);
        if (rr == kXiOk) Log("xinput: port %u has a REAL pad attached - it will be overridden while"
                             " emulation is on", i);
    }
    Log("xinput: hooked XInputGetState at %p. NUMPAD3 = pretend a 360 pad is plugged in.", target);
    Log("xinput:   the game imports this BY ORDINAL, so watch the call count below - if it stays"
        " at 0 the engine is not polling and the gamepad route is dead.");
    return true;
}

// Once a second, and only while something is worth saying. The call count is the measurement this
// probe exists for, so it is reported whether or not emulation is on.
void LogXInputStats() {
    const ULONGLONG now = GetTickCount64();
    if (g_xiLastStatsTick && now - g_xiLastStatsTick < 1000) return;
    const LONG total = InterlockedCompareExchange(&g_xiCalls, 0, 0);
    if (g_xiLastStatsTick) {
        const LONG delta = total - g_xiLastStatsCalls;
        // Packet advances is the run-70 instrument. At rest it should be ~0: a nonzero rate with
        // nobody touching a stick means the state is still churning and UE3 is still running its
        // whole input path for nothing.
        Log("xinput: %ld calls/sec (total %ld), packet advances/sec %lu%s, SetState/sec %ld"
            " - port0 %ld port1 %ld port2 %ld port3 %ld, emulation %s",
            delta, total, (unsigned long)g_padPacketAdvances,
            InterlockedCompareExchange(&g_freezePacket, 0, 0) ? " [PACKET FROZEN]" : "",
            InterlockedExchange(&g_setStateCalls, 0),
            InterlockedCompareExchange(&g_xiCallsByIndex[0], 0, 0),
            InterlockedCompareExchange(&g_xiCallsByIndex[1], 0, 0),
            InterlockedCompareExchange(&g_xiCallsByIndex[2], 0, 0),
            InterlockedCompareExchange(&g_xiCallsByIndex[3], 0, 0),
            InterlockedCompareExchange(&g_padEmu, 0, 0) ? "ON" : "off");
    }
    // Who called, and what our own hook cost. If the hook total is a fraction of a millisecond
    // while the game is losing 6 ms a frame, the polls are a symptom and the call SITE is the lead.
    if (g_xiCallerN > 0) {
        char line[512]; int n = 0;
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
                         "xinput callers (hook total %.2f ms/sec):", g_msInHook);
        for (int i = 0; i < g_xiCallerN && n < 400; ++i)
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
                             "  0x%08X x%lu", (unsigned)g_xiCallers[i].addr,
                             (unsigned long)g_xiCallers[i].count);
        Log("%s", line);
        for (int i = 0; i < g_xiCallerN; ++i) g_xiCallers[i].count = 0;
    }
    g_msInHook = 0.0;

    g_xiLastStatsTick = now;
    g_xiLastStatsCalls = total;
    g_padPacketAdvances = 0;
}

void ClearPad() {
    InterlockedExchange(&g_padBtns, 0);
    InterlockedExchange(&g_padLStick, PackStick(0, 0));
    InterlockedExchange(&g_padRStick, PackStick(0, 0));
}

// ---- the button probe (run 61b): stop guessing Raven's console layout, measure it ----
//
// Run 61 mapped weapon switch to D-pad up and it used a healing item. That is the second guess at
// a layout nobody has written down, and guessing again would be the third. Raven's 360 mapping is
// a fact about the shipped game, it is fourteen buttons wide, and one press each settles it
// completely - so press them one at a time and write down what happens.
//
// Cheaper than reading the layout out of the binary, and it cannot be wrong about the build that
// is actually installed.
struct XiButtonName { WORD mask; const char* name; };
const XiButtonName kXiButtonNames[] = {
    { XI_A,           "A"                 },
    { XI_B,           "B"                 },
    { XI_X,           "X"                 },
    { XI_Y,           "Y"                 },
    { XI_LSHOULDER,   "LB"                },
    { XI_RSHOULDER,   "RB"                },
    { XI_LTHUMB,      "left stick click"  },
    { XI_RTHUMB,      "right stick click" },
    { XI_DPAD_UP,     "D-pad up"          },
    { XI_DPAD_DOWN,   "D-pad down"        },
    { XI_DPAD_LEFT,   "D-pad left"        },
    { XI_DPAD_RIGHT,  "D-pad right"       },
    // Last deliberately: START opens the pause menu, which interrupts whatever you were watching.
    { XI_START,       "START"             },
    { XI_BACK,        "BACK"              },
};
const int kXiButtonNameCount = (int)(sizeof(kXiButtonNames) / sizeof(kXiButtonNames[0]));

int  g_btnProbeIdx    = 0;
int  g_btnProbeFrames = 0;    // frames left in the pulse
WORD g_btnProbeMask   = 0;

// The look stick's vertical axis, which turn never uses. Up cycles the weapon (Raven has only a
// cycle, not next/prev), leaving down free for the health item - the one action with no natural
// home on a Touch controller once the TMD powers have taken the grips and the face buttons.
WORD g_stickUpMask   = XI_Y;         // ini StickUp   - CycleWeapon
WORD g_stickDownMask = XI_DPAD_UP;   // ini StickDown - Health

// The NUMPAD4/NUMPAD5 test inputs, laid OVER whatever the controllers just published.
//
// ⚠️ Ordering, and it is not incidental: this MUST run after SyncXRInput, or the real controller
// state overwrites the test every frame and the test silently does nothing. Both write the same
// three words. It is called from Hook_Present rather than from UpdateFromHeadset for exactly that
// reason - UpdateFromHeadset returns early on half a dozen paths, and a test that only works when
// the headset happens to be running is not a control.
void ApplyPadTestOverrides() {
    const bool walk = InterlockedCompareExchange(&g_padWalkTest, 0, 0) != 0;
    const bool look = InterlockedCompareExchange(&g_padLookTest, 0, 0) != 0;
    const bool probe = g_btnProbeFrames > 0;

    if (!walk && !look && !probe) {
        // Nothing else writes the pad when the controllers are not driving it, so a test switched
        // off would otherwise leave its last value latched and the player walking for ever.
        if (!g_xrInputReady) ClearPad();
        return;
    }
    // 24000 of 32767 - clear of any dead zone the engine applies, and short of the rim so a
    // stuck-at-maximum reading stays distinguishable from a deliberate one.
    if (walk) InterlockedExchange(&g_padLStick, PackStick(0, 24000));
    if (look) InterlockedExchange(&g_padRStick, PackStick(24000, 0));
    if (probe) {
        // OR'd in, and held for several frames. A single-frame press is not reliably seen: the
        // engine samples the pad on its own tick, which is not this one, and an action that needs
        // a press AND a release needs the button to have been down across at least one of them.
        InterlockedExchange(&g_padBtns,
            InterlockedCompareExchange(&g_padBtns, 0, 0) | (LONG)g_btnProbeMask);
        --g_btnProbeFrames;
    }
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
// ']' - absorb engine-side yaw changes as player input, or ignore them. ON is the shipped
// behaviour and the only reason mouse and stick turning coexist with head tracking. See the
// note at the fold-in itself for why a cutscene makes it turn on the head instead.
volatile LONG g_yawFoldIn = 1;
// How often the engine keeps the yaw we wrote, over the last report window. The cutscene detector
// candidate that replaced the dead ProcessEvent route - see the note at the measurement site.
volatile LONG g_yawHeld = 0, g_yawChecked = 0;
volatile LONG g_yawHeldPct = -1;    // -1 = not measured yet, so the readout can say so
// ']' - pin the view to the engine's own camera so a measurement is not at the mercy of how still
// someone can hold their head. See the note where it is applied.
volatile LONG g_freezeView = 0;
int32_t g_frozenYaw = 0, g_frozenPitch = 0;
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

// Split out of NameOf so a name index can be resolved on its own. The ProcessEvent census keeps
// indices rather than objects - a UFunction pointer is only valid for the instant of the call,
// while its FName index stays meaningful for the whole run.
bool NameFromIndex(int32_t idx, char* buf, size_t cap) {
    buf[0] = 0;
    if (!Readable((void*)kGNamesTArray, 12)) return false;
    uintptr_t nd = *reinterpret_cast<uintptr_t*>(kGNamesTArray);
    int32_t nc = *reinterpret_cast<int32_t*>(kGNamesTArray + 4);
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

// ================= ⭐ run 98: find EVERY field that holds the view rotation ====================
//
// Run 97 swapped Controller.Rotation, the camera actor's Rotation and mCurrentPOV.Rotation for the
// duration of FireAmmunition - the log confirms all three were SET, with the hand 98 degrees off -
// and the bullet still went down the crosshair. So the aim is read from a field nobody has named.
//
// UE3's GetPlayerViewPoint reads `PlayerCamera.CameraCache.POV.Rotation`, and CameraCache is a
// DIFFERENT member from Raven's mCurrentPOV. That is the leading candidate. But guessing its
// offset is exactly what has cost this project run after run, so it does not get guessed.
//
// Instead: scan both objects for every 3-int32 triple whose pitch and yaw match the view rotation
// this mod is itself writing. A field that holds the view rotation must track it. The filter is
// that it has to track it across SEVERAL DIFFERENT head orientations - a single sample would match
// dozens of offsets by coincidence, since small integers are everywhere, and requiring agreement
// at four orientations at least 5 degrees apart removes essentially all of them.
//
// This is the same discipline that finally worked for UFunction::Func: do not adopt a candidate
// that a wrong answer could imitate.
const int kRotFieldMax = 24;
int g_ctlRotOffs[kRotFieldMax], g_ctlRotCount = 0;
int g_camRotOffs[kRotFieldMax], g_camRotCount = 0;
// Odd while a fire window is open, even when closed. Present uses it to tell "the player turned"
// from "we wrote the hand for a microsecond" - see the fold-in guard.
// ---- 💥 run 109: DEPTH, not parity ----
//
// g_fireSeq is bumped once per window, and the fire windows NEST - StartFire five deep, then
// BeginFire, then FireAmmunition. So "odd means open" flips seven times during one shot and is
// meaningless by the time the trace runs. That is why aim mode 11 rotated nothing: the gate was
// effectively random, and it is also why spawn lines read "window closed" for emitters that are
// certainly inside the shot.
//
// Depth is the correct primitive for a nested window. The sequence counter stays, but only for
// what it was actually good for: letting Present detect that a window opened or closed ACROSS its
// read. Open-ness itself is now depth > 0.
// The hand-minus-head deviation in engine rotation units, measured directly from the two poses
// in the same XR frame - see run 112. This is what the trace rotation uses.
volatile LONG g_handDevYawUU = 0, g_handDevPitchUU = 0, g_handDevValid = 0;
volatile LONG g_handDevRollUU = 0;
volatile LONG g_handOffX = 0, g_handOffY = 0, g_handOffZ = 0;   // hand minus head, world UU
volatile LONG g_fireDepth = 0;
volatile LONG g_fireSeq = 0;

// ---- ⭐ run 100: which fields the fire window is allowed to touch ----
//
// Run 99 wrote all seven scanned fields and produced the first real result: the bullet followed
// the controller in PITCH. It also moved the camera on every shot, and the two are the same story.
// Yaw is almost certainly decoupling as well and cannot be seen, because the camera jumps to the
// hand's yaw at the instant of the shot - so the bullet lands on the crosshair because the
// CROSSHAIR MOVED. Pitch survives because the mod's fold-in is yaw-only and has no pitch
// equivalent to corrupt.
//
// The camera movement is not the fold-in race the seqlock guards - that was already fixed and the
// symptom is unchanged. It is a WRITE the engine acts on: ENGINE_NOTES records
// mDesiredRotationInterpRate on the camera, so a desired-rotation field written even for a
// microsecond makes the camera interpolate toward the hand over the following frames. No seqlock
// can prevent that, because it is not a race.
//
// Three of the seven scanned fields (camera +0x0338, +0x035C, +0x0464) have never been identified,
// and one of them is the obvious candidate. So narrow the set instead of guessing which:
//
//   0  controller only  - +0x0060 and +0x05D4, no camera writes at all      (DEFAULT)
//   1  controller + mCurrentPOV.Rotation
//   2  every scanned field                                    (run 99's behaviour)
//
// Set 0 is the default because it removes every camera write, and because +0x05D4 is a CONTROLLER
// field that run 99 added for the first time - run 97 wrote the camera fields WITHOUT it and got
// nothing, so the new information is more likely to be there than in the camera.
//
// ⚠️ Run 80 wrote a constant to +0x05D4 and saw no deflection, and that is NOT a contradiction: it
// wrote from Present, once per frame, so the engine had a whole tick to overwrite it before the
// shot. Inside the fire window there is no such gap.
LONG g_aimFieldSet = 0;
volatile LONG g_rotScanSamples = 0;
volatile LONG g_rotScanDone = 0;
int32_t g_rotScanLastYaw = 0;

static void ScanOne(uintptr_t obj, int limit, int* offs, int* count, bool first,
                    int32_t wantPitch, int32_t wantYaw) {
    const int32_t kTol = 200;          // ~1.1 degrees in the 65536-per-turn space
    if (first) {
        *count = 0;
        for (int o = 0; o + 12 <= limit && *count < kRotFieldMax; o += 4) {
            const int32_t* r = reinterpret_cast<const int32_t*>(obj + o);
            const int32_t dp = r[0] - wantPitch, dy = r[1] - wantYaw;
            if (dp < kTol && dp > -kTol && dy < kTol && dy > -kTol) offs[(*count)++] = o;
        }
        return;
    }
    int keep = 0;
    for (int i = 0; i < *count; ++i) {
        const int32_t* r = reinterpret_cast<const int32_t*>(obj + offs[i]);
        const int32_t dp = r[0] - wantPitch, dy = r[1] - wantYaw;
        if (dp < kTol && dp > -kTol && dy < kTol && dy > -kTol) offs[keep++] = offs[i];
    }
    *count = keep;
}

// Called from Present. Samples only when the head has actually MOVED - four samples taken at the
// same orientation would confirm nothing, and would look identical to four that had.
void ScanViewRotFields(uintptr_t ctl, uintptr_t cam, int32_t wantPitch, int32_t wantYaw) {
    if (InterlockedCompareExchange(&g_rotScanDone, 0, 0)) return;
    const LONG n = InterlockedCompareExchange(&g_rotScanSamples, 0, 0);
    if (n > 0) {
        const int32_t moved = wantYaw - g_rotScanLastYaw;
        if (moved < 910 && moved > -910) return;      // under 5 degrees - not a new orientation
    }
    if (!ctl || !cam || !Readable((void*)ctl, 0x600) || !Readable((void*)cam, 0x700)) return;
    g_rotScanLastYaw = wantYaw;

    const bool first = (n == 0);
    ScanOne(ctl, 0x600, g_ctlRotOffs, &g_ctlRotCount, first, wantPitch, wantYaw);
    ScanOne(cam, 0x700, g_camRotOffs, &g_camRotCount, first, wantPitch, wantYaw);
    const LONG got = InterlockedIncrement(&g_rotScanSamples);
    Log("view-rot scan %ld/4 (yaw %d): controller %d field(s), camera %d field(s) still matching",
        got, wantYaw, g_ctlRotCount, g_camRotCount);

    if (got >= 4) {
        InterlockedExchange(&g_rotScanDone, 1);
        for (int i = 0; i < g_ctlRotCount; ++i) Log("    controller +0x%04X", g_ctlRotOffs[i]);
        for (int i = 0; i < g_camRotCount; ++i) Log("    camera     +0x%04X", g_camRotOffs[i]);
        if (g_ctlRotCount + g_camRotCount == 0)
            Log("    *** NOTHING survived. The view rotation is not stored on either object in the"
                " form this mod writes it, and the fire window cannot reach it by swapping fields.");
    }
}

// Read-only now: these are what the matrix detector substitutes into the candidate matrices.
// Writing them was proven useless in spike 8 - the render does not read them back.
const int ACTOR_LOCATION   = 0x0054;   // FVector, 3 floats
const int CAM_POV_LOC      = 0x0420;   // mCurrentPOV location
// ---- ⭐ run 97: mCurrentPOV.Rotation, DERIVED rather than guessed ----
// UE3's FPOV is { vector Location; rotator Rotation; float FOV; } - 12 + 12 + 4 bytes. Location is
// known to be 0x0420 and FOV known to be 0x0438, and 0x0420 + 12 = 0x042C, 0x042C + 12 = 0x0438.
// The two offsets already proven bracket this one exactly, so it is the only value the struct can
// have. That is a different kind of claim from +0x14, which was one observation treated as a fact.
const int CAM_POV_ROT      = 0x042C;   // mCurrentPOV rotation - pitch, yaw, roll
const int CAM_POV_FOV      = 0x0438;   // mCurrentPOV FOV - proven to steer the projection
const int CAM_DESIRED_FOV  = 0x0470;   // mDesiredPOV FOV, the interpolation target
const int CAM_CURRENT_FOV  = 0x0490;   // mCurrentFOV

// ---- âš ï¸ a FAILED scan is enormously expensive, and it repeats every frame (run 35) ----
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
// ini ExtraCamReg: a second vertex-shader register to remap alongside the locked one. -1 = off.
// Run 42 found a view-projection at c13 that nothing was remapping; see the note where it is used.
int   g_extraCamReg = -1;
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
// ---- run 78: the top of this list is now the aim-decoupling lever ----
//
// Confirmed by report: with the engine aimed at the hand, "moving the gun down turned the ceiling
// black", worsening as the clamp widened. That is UE3 culling against the ENGINE's frustum, which
// now follows the weapon rather than the view - exactly the run-64 blocker, and this time
// unambiguous because the user can point the gun and watch a specific surface disappear.
//
// The lever already existed and nobody had pushed it: `askDeg` is the FOV handed to the engine and
// it governs ONLY culling, because ApplyProjection forces the rendered frustum independently. So
// widening it buys culling coverage without touching the image.
//
// The arithmetic that says how much is needed: at a rendered half-angle of ~50 deg, a headroom of
// k gives a culling half-angle of atan(k * tan 50). 2.5x reaches 71 deg - 21 deg of slack, short
// of a 30 deg clamp. 6x reaches 82 deg. `askDeg` is capped at 170 deg total, so ~9.6x is the
// ceiling and there is no point going past it.
//
// ⚠️ NOT free, and run 21 already priced it: inflating the engine's FOV makes objects project
// smaller than the engine expects, which pushes distant geometry down its LOD and draw-distance
// curves. That is the trade this test is really measuring - edge dropouts against pop-in - and it
// is why the low steps stay in the list rather than being replaced.
// ⚠️ Reordered run 90 to run UPWARDS first. The old order put 0.70/0.50/0.35 immediately after
// 1.0, so reaching 4x took eight presses and 6x took nine - in a headset, while trying to watch a
// ceiling for black patches. Mis-counting wastes the whole run. The shortfall-measuring values
// below 1.0 are still reachable, just moved to the end where they are no longer in the way.
const float kFovHeadroomSteps[] = { 1.0f, 1.30f, 1.75f, 2.5f, 4.0f, 6.0f, 0.70f, 0.50f, 0.35f };
const int   kFovHeadroomCount = 9;
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
// Draws whose matrix actually received a roll. Reported next to the peak angle so "roll is on"
// and "roll reached the GPU" stay separate claims - every counter in this project that could only
// report the good case has eventually misled someone.
int   g_rollApplied = 0;

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

// ---- head ROLL, applied in clip space ----
//
// Roll was the last axis of the head pose never written anywhere: F9 drove pitch and yaw only, so
// tilting your head sideways left the horizon glued to the headset instead of staying level.
//
// It does NOT go through the engine's FRotator. The detour owns pitch, yaw needs a direct write to
// the controller, and UE3 zeroes camera roll in several places - three seams to fight for an angle
// we already own outright, because the view matrix is ours by the time it reaches the GPU.
//
// ⚠️ And it CANNOT be left to the compositor, which is the trap here. The submitted projection
// layer already carries the head's full orientation, roll included, so it looks like the runtime
// should be rotating the image for us. It does not: a projection layer is reprojected by the
// DELTA between the pose we claim and the pose the display is at. Claim a rolled pose for an
// unrolled render while the head is at that same roll, the delta is zero, and the image is
// presented straight - which is exactly the reported symptom. Baking roll into the render is what
// makes the claimed pose true, and the two then agree by construction.
//
// Roll about the view's forward axis is a rotation of clip x/y, but the frustum is NOT square -
// tanX and tanY differ, and under duplication tanX is the half-width per-eye value. Rotating the
// raw clip columns would shear. So convert to the symmetric view-space direction, rotate, convert
// back:
//
//     x_v = clip.x * tanX,  y_v = clip.y * tanY      (view-space direction, aspect removed)
//     rotate by theta
//     clip.x' = x_v' / tanX,  clip.y' = y_v' / tanY
//
// which collapses to a mix of the two columns with the aspect ratio as the cross term.
//
// Order matters twice over, and both are satisfied by applying this in Hook_SetVSConstF right
// after ApplyProjection:
//
//   * AFTER the forced projection, so the tangents read here are the frustum the eye actually
//     sees rather than whatever the engine happened to send.
//   * BEFORE ApplyEyeRemap, which squashes clip x by half. Rolling after that squash would mix a
//     halved x with a full y and tilt by the wrong angle.
//
// It composes with ApplyOffset in either order: an offset is a pre-multiplication in world space,
// a roll is a post-multiplication in clip space, so they commute. The eye separation needs no
// special handling either - g_eyeDeltaUU is derived from the two real eye POSITIONS, which already
// swing about the head as it tilts.
void ApplyRoll(Reg4* r, int conv, float rollRad, float tanX, float tanY) {
    if (rollRad == 0.0f) return;
    if (!(tanX > 1e-6f && tanY > 1e-6f)) return;
    const float cs = cosf(rollRad), sn = sinf(rollRad);
    const float xy = (tanY / tanX) * sn;    // how much clip y feeds into clip x
    const float yx = (tanX / tanY) * sn;
    if (conv == CONV_ROW) {
        // The x column is r[0..3].x, the y column r[0..3].y - the same layout ApplyProjection scales.
        for (int i = 0; i < 4; ++i) {
            const float x = r[i].x, y = r[i].y;
            r[i].x = cs * x - xy * y;
            r[i].y = yx * x + cs * y;
        }
    } else {
        // Register 0 IS the x row, register 1 the y row.
        for (int k = 0; k < 4; ++k) {
            const float x = (&r[0].x)[k], y = (&r[1].x)[k];
            (&r[0].x)[k] = cs * x - xy * y;
            (&r[1].x)[k] = yx * x + cs * y;
        }
    }
}

// ---- head YAW and PITCH in the view matrix, the way roll already is (run 64) ----
//
// The last rung needs `PlayerController.Rotation` free to carry the WEAPON's aim, and today it
// carries the head's. Roll already proves the alternative works: run 59 put it in the matrix
// rather than the engine's FRotator, and it has been correct ever since. Yaw and pitch are the
// same move with two more axes.
//
// Unlike roll, this is NOT a clip-space rotation. Roll is about the view's forward axis, so it
// only mixes clip x and y and can be conjugated through the projection with two tangents. Yaw and
// pitch mix in depth, and conjugating those through a projection matrix is a projective transform
// involving the near and far planes - fiddly, and unnecessary, because there is a much better
// place to stand.
//
// The engine renders in TRANSLATED-WORLD space, so **the camera sits at the origin**. A rotation
// about the camera is therefore a rotation about the origin: a pure 3x3, no translation term, and
// it pre-multiplies the matrix on the world side where the projection cannot interfere.
//
//     clip = v * M,  M = V * P        (row-vector, ROW storage - the locked convention here)
//     camera basis rotates by R  =>   V' = R * V   =>   M' = R * M
//
// which for row storage is a linear combination of the first three rows and leaves row 3 - the
// translation - untouched. That last part matters: the camera POSITION must survive this, and it
// does so by construction rather than by care.
//
// R is built as B_desired * B_current^T rather than composed from separate yaw and pitch
// rotations. Composition order is the classic place to be subtly wrong here, and two bases
// multiplied together cannot be in the wrong order - the ordering of the basis columns cancels,
// so long as both use the same one.
//
// ⚠️ Order against ApplyOffset: rotation FIRST, offset second. We want the camera rotated about
// its own position and then displaced, which is M' = T(-o) * R * M. ApplyOffset already computes
// T(-o) * M, so calling it after this composes correctly. The reverse order would swing the 6-DOF
// offset around with the head instead of holding it in world space.
void CamBasis(float pitchRad, float yawRad, float b[3][3]) {
    // UE3: X forward, Y right, Z up - the same construction the camera-axis code uses, so the two
    // cannot drift apart.
    const float cp = cosf(pitchRad), sp = sinf(pitchRad);
    const float cy = cosf(yawRad),   sy = sinf(yawRad);
    const float fwd[3]   = { cp * cy, cp * sy, sp };
    const float right[3] = { -sy,     cy,      0.0f };
    // up = fwd x right, which comes out +Z at zero pitch - checked against the existing axes.
    const float up[3] = { fwd[1]*right[2] - fwd[2]*right[1],
                          fwd[2]*right[0] - fwd[0]*right[2],
                          fwd[0]*right[1] - fwd[1]*right[0] };
    for (int i = 0; i < 3; ++i) { b[i][0] = right[i]; b[i][1] = up[i]; b[i][2] = fwd[i]; }
}

void ApplyHeadRotation(Reg4* r, int conv,
                       float pitchCur, float yawCur, float pitchDes, float yawDes) {
    float bc[3][3], bd[3][3];
    CamBasis(pitchCur, yawCur, bc);
    CamBasis(pitchDes, yawDes, bd);

    float R[3][3];
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k)
            R[i][k] = bd[i][0]*bc[k][0] + bd[i][1]*bc[k][1] + bd[i][2]*bc[k][2];

    if (conv == CONV_ROW) {
        Reg4 out[3];
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 4; ++k)
                (&out[i].x)[k] = R[i][0]*(&r[0].x)[k]
                               + R[i][1]*(&r[1].x)[k]
                               + R[i][2]*(&r[2].x)[k];
        for (int i = 0; i < 3; ++i) r[i] = out[i];
    } else {
        // COL: registers are the rows of clip = M*v, so the same camera rotation lands on the
        // right as M * R^T. UNTESTED - every lock this project has ever taken has been ROW.
        for (int i = 0; i < 4; ++i) {
            const float x = r[i].x, y = r[i].y, z = r[i].z;
            for (int k = 0; k < 3; ++k)
                (&r[i].x)[k] = x*R[k][0] + y*R[k][1] + z*R[k][2];
        }
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

// Defined further down, next to g_baseYaw - snap turn steers that directly, so the input code
// lives beside it rather than being separated from the one variable it writes.
bool InitXRInput();

// Which OpenXR session state we are in. Actions only deliver data while FOCUSED, and the
// difference between "not focused" and "not working" is invisible without this.
XrSessionState g_xrSessionState = XR_SESSION_STATE_UNKNOWN;

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
                CacheAutoResolution(g_recEyeW * 2, g_recEyeH);
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

    // Must happen before the first xrSyncActions, and xrAttachSessionActionSets can only ever be
    // called once for a session - so this is the only place it can go. A failure here disables
    // controller input and nothing else; the head-tracked render path does not depend on it.
    InitXRInput();

    Log("OpenXR ready");
    g_xrReady = true;
    return true;
}

void PumpXR() {
    XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(g_xrInstance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            g_xrSessionState = s->state;
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
// xrBeginFrame + xrEndFrame, the frame submission. Broken out of "our work" in run 56 because it is
// the only part of this mod that talks to the wireless link, and the unmodded game runs clean.
double g_msXrSubmit = 0.0;
int    g_xrSubmitFrames = 0;
double g_msXrSubmitPeak = 0.0;
int    g_waitFrames = 0;
double g_displayPeriodMs = 0.0;   // from XrFrameState, so the pacing target is measured not guessed

// ---- âš ï¸ the copy timing conflates two costs, and only one of them D3D9Ex removes ----
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

// ---- âš ï¸ measured run 32: pipelining LOSES. Immediate is the default. ----
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

// ---- did the engine ACCEPT the resolution we asked for on the command line? ----
//
// Only two widths have ever been tried: 4096 accepted, 4992 refused (run 33). Run 40 then measured
// the GPU at 16384x16384, so the refusal is UE3's own validation rather than hardware - and nobody
// has bisected where it actually cuts off, or tried a TALLER frame at all. Vertical is independent
// of whatever limits the width, and the headset wants 2688 against the 2160 being rendered.
//
// Reading the command line is not the GetCommandLineW DETOUR that would inject these automatically
// - that is still unbuilt, and would be pointless until the engine's ceiling is known, since it
// could only ever inject a size the engine already accepts. This just reads what was asked so the
// log states the verdict instead of leaving it to be inferred from two numbers on different lines.
void ReportResolutionRequest(UINT gotW, UINT gotH) {
    const char* cl = GetCommandLineA();
    if (!cl) return;
    Log("command line: %s", cl);

    unsigned reqW = 0, reqH = 0;
    // -ResX= / -ResY=, case-insensitive, as UE3 itself parses them.
    for (const char* p = cl; *p; ++p) {
        if ((p[0] == '-' || p[0] == '/') &&
            (p[1] == 'R' || p[1] == 'r') && (p[2] == 'e' || p[2] == 'E') &&
            (p[3] == 's' || p[3] == 'S')) {
            const char axis = p[4];
            if (p[5] == '=') {
                const unsigned v = (unsigned)strtoul(p + 6, nullptr, 10);
                if (axis == 'X' || axis == 'x') reqW = v;
                if (axis == 'Y' || axis == 'y') reqH = v;
            }
        }
    }
    if (!reqW && !reqH) {
        Log("    no -ResX/-ResY on the command line - the engine used its own configured size,"
            " %ux%u", gotW, gotH);
        return;
    }

    const bool okW = (reqW == 0) || (reqW == gotW);
    const bool okH = (reqH == 0) || (reqH == gotH);
    Log("    requested %ux%u -> got %ux%u   *** %s ***", reqW, reqH, gotW, gotH,
        (okW && okH) ? "ACCEPTED"
        : (!okW && !okH) ? "REFUSED on BOTH axes - engine substituted its own size"
        : !okW ? "WIDTH REFUSED (height was fine) - the limit is horizontal"
               : "HEIGHT REFUSED (width was fine) - the limit is vertical");

    // What this means for the eyes, which is the number the whole exercise is about.
    if (g_recEyeW && g_recEyeH) {
        const double hPct = 100.0 * (gotW / 2.0) / g_recEyeW;
        const double vPct = 100.0 * gotH / g_recEyeH;
        Log("    per eye %ux%u against the headset's %ux%u -> %.0f%% horizontal, %.0f%% vertical"
            " (%.0f%% of the pixels)",
            gotW / 2, gotH, g_recEyeW, g_recEyeH, hPct, vPct, hPct * vPct / 100.0);
    }
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

    // ⚠️ NOTHING ELSE GETS ALLOCATED HERE WITHOUT BEING USED. The Stage 4B probe created a
    // scene-sized render target AND depth-stencil at this point - ~70 MB at 4096x2160 - gated
    // behind a hotkey that had to be pressed for them to be touched at all. Every run paid the
    // allocation from the first frame. Gating a diagnostic's WORK is not gating its COST.
    g_srcW = w; g_srcH = h; g_srcFmt = fmt;
    Log("copy resources ready for %ux%u fmt=%d (%d readback slots, %s, %s)", w, h, (int)fmt,
        kCopySlots, g_copyMode == 1 ? "immediate" : "pipelined one frame",
        g_zeroCopyActive ? "ZERO-COPY" : "CPU round-trip");
    ReportResolutionRequest(w, h);
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
        // ---- âš ï¸ no pipelining here, and it is not an oversight ----
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

    // ---- âš ï¸ LockRect is where the transfer actually surfaces (run 35) ----
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

// ---- head roll ----
//
// Kept in RADIANS, not UU: it never reaches an FRotator. See ApplyRoll for why the engine's
// rotation is the wrong place for this and the view matrix is the right one.
//
// Deliberately ABSOLUTE, with no recentre baseline. Yaw is anchored to whatever the game's yaw was
// when we recentred because the game's yaw is arbitrary; roll is not - the runtime measures it
// against gravity, and level head should mean level horizon. Anchoring it would bake a permanent
// tilt in whenever someone recentred with their head at an angle.
volatile LONG g_rollOn = 1;      // ini HeadRoll, NUMPAD1 - rides with head tracking
// -1, MEASURED in run 49: +1 tilted the horizon the wrong way in the headset and NUMPAD2 fixed it.
// Third axis, third time the handedness between OpenXR's frame and the game's has come out
// negative - g_yawSign is -1 for the same reason. Kept as a variable, and NUMPAD2 kept, because
// the next runtime or headset is not obliged to agree.
int   g_rollSign = -1;           // NUMPAD2 - the same escape hatch F4/F5 give pitch and yaw
float g_wantRollRad = 0.0f;

// ---- the view/engine rotation split (run 64, NUMPAD8) ----
//
// OFF: today's arrangement - the engine's rotation carries the head, and drives the view with it.
// ON:  the engine gets the BASE only, and the head's yaw and pitch are added in the view matrix.
//
// With the split on, the image should be IDENTICAL. That is the whole point of doing it as a
// separate step: it is a refactor with a visible no-op, so the mechanism can be proved before
// anything is decoupled. Once it holds, the engine's rotation is free to carry the weapon.
volatile LONG g_matrixRot = 0;

// ---- ⭐ run 133: the cutscene yaw test, as ONE key ----
//
// The two flags that matter for it are independent, so the honest test is their 2x2. Asking for
// that as two separate keys put the burden of tracking which cell you are in on someone wearing a
// headset - so NUMPAD8 walks the four combinations in order instead, and the readout names the
// cell. One key, four presses, back where you started.
//
// ⚠️ The mode number is DERIVED from the live flags, never stored. g_matrixRot has another owner
// (the aim-mode path sets it directly), and a remembered cycle index would keep confidently
// reporting a cell the flags had since left. Derived, a disagreement is impossible by
// construction: whatever the flags say IS the mode.
//
//   1  ROT ENGINE  FOLD ON    the shipped behaviour
//   2  ROT MATRIX  FOLD ON    the matrix carries rotation; the fold-in still cancels yaw
//   3  ROT MATRIX  FOLD OFF   predicted to be the one where looking left and right works
//   4  ROT ENGINE  FOLD OFF   isolates the fold-in from the matrix path
inline int CutsceneMode() {
    const bool mat  = InterlockedCompareExchange(&g_matrixRot, 0, 0) != 0;
    const bool fold = InterlockedCompareExchange(&g_yawFoldIn, 0, 0) != 0;
    if (!mat &&  fold) return 1;
    if ( mat &&  fold) return 2;
    if ( mat && !fold) return 3;
    return 4;
}

inline void SetCutsceneMode(int m) {
    InterlockedExchange(&g_matrixRot, (m == 2 || m == 3) ? 1 : 0);
    InterlockedExchange(&g_yawFoldIn, (m == 1 || m == 2) ? 1 : 0);
}

// NUMPAD . - point the engine's rotation at the right hand instead of the body. Needs g_matrixRot,
// since without the split the engine's rotation IS the view and aiming it at your hand would swing
// the camera with your hand.
volatile LONG g_aimDecouple = 0;

// NUMPAD . cycles this. 0 off · 1 engine follows the hand (run 76, needs the matrix split and
// breaks culling) · 2 constant +20 deg into the controller's SECOND rotation, as a probe ·
// 3 the hand written to that second rotation, which is the arrangement worth having.
//
// Modes 2 and 3 deliberately need NO matrix split: the view and the engine both stay on the head,
// which is the normal, truthful arrangement, and culling never diverges from what you can see.
volatile LONG g_aimMode = 0;
int32_t g_aimYawUU = 0, g_aimPitchUU = 0;   // the hand's aim, in the engine's own units
int32_t g_matCurYawUU = 0;      // what the engine is being told
int32_t g_matCurPitchUU = 0;
int32_t g_matDesYawUU = 0;      // where the head actually is
int32_t g_matDesPitchUU = 0;

// ---- the deviation clamp: how far the ENGINE's camera may point away from your view ----
//
// Run 64's test decoupled the engine's rotation all the way to the BODY's facing, and geometry
// disappeared behind the player. That is CPU frustum culling in UE3, and no hook here can reach
// it: the actors are culled before a draw call is ever issued, long before
// SetVertexShaderConstantF sees anything. The fix has to be upstream, in what the engine believes
// it is looking at.
//
// ⚠️ ENGINE_NOTES records "CPU frustum culling is a non-issue at stereo scale - a 1.4-2.9% band at
// 300 UU". That measurement is sound and completely inapplicable here. It was taken for an eye
// separation of a few UU; this is a rotation of up to ninety degrees. Same mechanism, different
// regime by three orders of magnitude - the project's own recurring trap, and worth naming rather
// than quietly re-measuring.
//
// So bound it instead of arguing about it. This clamp guarantees |engine - view| <= N:
//
//     N = 0    engine points exactly where you look   (today's behaviour, culling perfect)
//     N = 90   engine may be a quarter turn away      (run 64's test, culling visibly broken)
//
// Raise N until geometry pops, and the answer is a number rather than an opinion. That number
// decides the whole remaining design: the weapon aim only has to deviate from the view by as much
// as your hand does from your gaze, which is nothing like ninety degrees. If N holds to ~40 this
// approach ships. If it collapses at ~10, the fire trace has to be hooked instead.
//
// It is also the shipping mechanism, not only the instrument - clamping how far the weapon may
// stray from the view is a reasonable design in its own right.
const int kDevStepCount = 7;
const int kDevSteps[kDevStepCount] = { 0, 10, 20, 30, 45, 60, 90 };
volatile LONG g_devStep = 0;    // index into kDevSteps

// ---- ⚠️ why the clamp alone is NOT a valid test, and the flicker is (run 65) ----
//
// Cycling the clamp and looking for missing geometry asks the player to notice an ABSENCE. That is
// the one thing human vision is worst at, especially in the periphery where a small deviation puts
// the culled band - so "I saw nothing wrong at 30 degrees" is almost no evidence that nothing was
// culled at 30 degrees. The instrument could only ever report the bad case when it was already
// severe enough to be obvious, which is how it got set up to confirm whatever it was pointed at.
//
// This is the SAME defect as the button probe two runs ago, caught this time by the person holding
// the headset rather than by the person who wrote it. Tenth instance in this project.
//
// The fix converts the absence into a MOTION signal. Alternate the clamp between 0 and the test
// angle a few times a second. The rendered view is identical either way - that is the no-op
// property the split was built to have - so anything the engine culls at the test angle appears
// and disappears at ~4 Hz. Vision is superb at detecting flicker and hopeless at detecting
// absence, so this reads the same defect through the channel that can actually see it.
//
// It also makes a clean NEGATIVE possible: if nothing flickers, nothing visible was culled. That
// is a real result rather than an absence of evidence.
//
// The draw-count delta logged alongside is a SUPPORTING number, not the verdict. It counts the
// engine's whole frustum, which at a large clamp covers a different region of the world - so it
// includes geometry that was never in view and reads as an upper bound on what was lost.
volatile LONG g_devFlicker = 0;
int    g_devPhase = 1;          // 0 = baseline (clamp 0), 1 = the test angle
int    g_devPhaseAge = 999;
double g_devDraws[2] = { 0.0, 0.0 };
int    g_devDrawFrames[2] = { 0, 0 };

// ---- ⚠️ the flicker test was confounded by its own mechanism (run 66) ----
//
// Reported symptom: at a clamp of only 10 degrees EVERYTHING strobed, including geometry straight
// ahead, and it read as a misalignment whose ANGLE GREW WITH THE CLAMP. None of that is culling.
// Culling removes objects at the edges; it does not tilt the ones in front of you, and it does not
// scale its error with a knob.
//
// What it is: the split assumes the engine's camera adopts the rotation we write to it IMMEDIATELY.
// ENGINE_NOTES already flagged the reason it might not - `mDesiredRotationInterpRate` and friends,
// "will likely need neutralising so the engine does not smooth away a directly-written HMD pose".
// A smoothing filter is INVISIBLE for smooth input and glaring for step input, and flicking the
// clamp four times a second is a step input of exactly the clamp's size. So the test manufactured
// the artefact it then reported, and the amplitude tracking the clamp is the signature.
//
// The consequence is bigger than the test: the matrix must compensate for where the engine's camera
// ACTUALLY is, not for where we asked it to go. Those were the same thing while the head drove the
// rotation smoothly, and they stop being the same the moment anything steps it - which aim
// decoupling will do constantly, because a hand does not move like a head.
//
// So the camera's real rotation is now read once per frame AT RENDER TIME - at the first matched
// constant upload, by which point the engine's camera update for this frame has already run, so it
// is the value this frame is actually being drawn with rather than last frame's.
//
// The residual below is the instrument, and it can report the bad case: **at clamp 0 it should be
// ~0**, because there we ask for the same rotation the head already had. A nonzero residual there
// would mean the camera actor is not what the view matrix is built from, which is a premise this
// whole approach rests on and has never been checked directly.
volatile LONG g_camRotDirty = 1;
int32_t g_camYawUU = 0, g_camPitchUU = 0;
bool    g_camRotKnown = false;
double  g_residSum = 0.0, g_residMax = 0.0;
int     g_residN = 0;

int32_t ClampDev(int32_t d, int32_t maxAbs) {
    // The subtraction that produced `d` wrapped through the FRotator's 65536-per-turn space, so
    // normalise into (-32768, +32768] before comparing - otherwise a turn across the wrap point
    // reads as an enormous deviation and gets clamped the wrong way.
    while (d >  32768) d -= 65536;
    while (d < -32768) d += 65536;
    if (d >  maxAbs) d =  maxAbs;
    if (d < -maxAbs) d = -maxAbs;
    return d;
}
float g_maxRollDeg = 0.0f;       // largest magnitude seen this report window
bool  g_haveCentre = false;
float g_centreYaw = 0.0f;
int32_t g_baseYaw = 0;
int   g_tick = 0;

// ================================================================ OpenXR input -> the pad (run 61)
//
// The probe settled the route, and the log settled the mechanism with it. Before a pad was
// reported, UE3 polled at 2 calls/sec ROUND-ROBIN across all four ports - that is device
// ENUMERATION, not input. The moment port 0 answered ERROR_SUCCESS the rate on that port alone
// jumped to ~122/sec, matching the frame rate, while ports 1-3 stayed on the slow scan. So the
// engine promotes a port to per-frame polling on first successful read.
//
// Two consequences, both design-forcing:
//
//   1. Detection costs up to ~4 seconds (four ports, ~1/sec). A pad that only appears when a
//      hotkey is pressed is therefore ABSENT for the whole main menu, which is precisely where it
//      is needed most. So emulation defaults ON.
//   2. The per-port call rate is a free, non-tautological instrument: port0 at frame rate means
//      the engine is reading us as a real input device, and it genuinely reported the bad case
//      for the first two seconds of the probe run. It stays in the log.
//
// ⚠️ Defaulting ON changes the game before any key is pressed, which run 36 did with VR mode and
// run 37 reverted. The distinction that makes it safe here: this changes no RENDERING. The menu
// gains button prompts and a highlighted item - the arrangement a pad user would already see -
// and `Gamepad=0` in the ini restores the old behaviour without a rebuild.
//
// ---- what is deliberately NOT done ----
//
// No key synthesis, no menu cursor, no engine input hooks. Raven's own 360 path owns dead zones,
// acceleration curves and menu focus, and every one of those is a thing not written or tuned here.

XrActionSet g_actionSet = XR_NULL_HANDLE;
XrAction g_actMove = XR_NULL_HANDLE, g_actTurn = XR_NULL_HANDLE;
XrAction g_actFire = XR_NULL_HANDLE, g_actAltFire = XR_NULL_HANDLE;
XrAction g_actHaptic = XR_NULL_HANDLE;
XrPath   g_handSubPath[2] = { XR_NULL_PATH, XR_NULL_PATH };   // left, right

// ---- controller poses (run 63) ----
//
// The shared prerequisite for everything still outstanding: off-hand-relative movement, aim
// decoupling, a laser sight, and putting the weapon model in your hand. Built once, here, with a
// small consumer attached immediately so it is proven rather than merely present - if walking
// follows your left hand, the pose and its conventions are right, and that is a much better test
// than logging numbers nobody can check.
// ---- ⚠️ run 77: MOTION was the wrong signal, and the hardware has the right one ----
//
// Auto-pad unplugged and replugged NINE times in one session, each replug costing a second of
// frozen controls while UE3 re-enumerated. Cause: resting your hands still - listening to dialogue,
// lining up a shot, just holding the controllers - drops both below any motion threshold that can
// still tell a desk from a hand. I flagged this exact risk when choosing 3 seconds and picked
// wrong; `both controllers must be still` does not save it, because both hands rest together.
//
// Motion was always an INFERENCE about whether a controller is held. Touch controllers report it
// DIRECTLY: `thumbrest/touch` and `trigger/touch` are capacitive sensors that fire when a finger is
// on the controller and stop when it is put down. A hand resting perfectly still is still touching
// it; a controller on a desk is not. That is the actual question, asked of the hardware that
// already answers it, instead of reconstructed from position deltas.
//
// Both bindings go to ONE action deliberately: a boolean action with several bound sources reads
// as the OR of them, so a thumb on the rest or a finger on the trigger both count as held, and
// neither has to be special-cased.
//
// Motion stays as the fallback for a runtime that does not expose the touch sensors - but with the
// threshold dropped to 100 um, because it is now only being asked "did this move at all", not
// "did this move enough to be a hand".
XrAction g_actGrasp = XR_NULL_HANDLE;
bool g_graspAvailable = false;

XrAction g_actHandPose = XR_NULL_HANDLE;
XrSpace  g_handSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
bool     g_handPoseValid[2] = { false, false };
float    g_handYawRad[2] = { 0.0f, 0.0f };
float    g_handPitchRad[2] = { 0.0f, 0.0f };
float    g_handRollRad[2] = { 0.0f, 0.0f };

// The right hand's resting offset from the head, captured at recentre. See the note where it is
// captured - this is what makes the deviation clamp symmetric.
float g_handYawOffset = 0.0f, g_handPitchOffset = 0.0f;
bool  g_haveHandOffset = false;

// The head yaw from the PREVIOUS frame. xrLocateViews runs after input is synced, so this is one
// frame stale by construction. That is fine for the one thing it is used for: walk direction
// depends on the DIFFERENCE between hand and head yaw, and turning your body moves both together,
// so the difference changes slowly even when both are moving fast.
float g_headYawRad = 0.0f;
float g_headRollRad = 0.0f;
float g_headPosXR[3] = { 0.0f, 0.0f, 0.0f };

int g_walkDirection = 1;   // ini WalkDirection: 0 = head-relative, 1 = off-hand-relative
int g_walkSign      = 1;   // ini WalkSign - handedness has been guessed wrong on three axes here

// ---- ⚠️ THE CONTROLLERS THEMSELVES ARE A VARIABLE, AND EVERY TEST SO FAR RAN INSIDE IT ----
//
// Reported run 67: the image judders and the frame rate drops whenever the head moves, ALWAYS -
// and it stops the moment the controllers are picked up, and returns when they are set down and
// the keyboard is used instead. Touch controllers sleep after a few seconds of stillness, so
// "set down" and "asleep" are the same state.
//
// This is bad news for the last three runs, and it has to be said plainly: **every hotkey in this
// mod is on the keyboard, so pressing NUMPAD8, 9 or * REQUIRES putting a controller down.** Every
// test in this session was therefore performed in whichever state that produces, and the run-66
// conclusion - that the split is not a no-op and the engine is smoothing our writes - was drawn
// from a strobe that may have had nothing to do with the split at all.
//
// That conclusion is now UNSAFE and is not to be built on until this is separated. It is the run-56
// lesson word for word: validate the environment before bisecting yourself. Three conclusions were
// retracted last time for exactly this reason.
//
// So: instrument the controller state into the frame budget, and add the cheap control - a switch
// that turns ALL of this mod's XR input work off, so "is it us at all" is one keypress rather than
// a rebuild.
bool g_handTracked[2] = { false, false };

// ---- ⚠️ run 69: "TRACKED" was never the question, and it reads BACKWARDS ----
//
// Reported: on the desk = judder; in hand = fine; and a trigger firing by accident on the desk
// does NOT help. That last detail is the load-bearing one - it rules out "is the controller awake"
// and "is the runtime delivering input" outright, because an accidental trigger is both.
//
// It also explains why the table looked inverted. A controller sitting on a desk in front of the
// headset is in clear view of the cameras and reports ORIENTATION_TRACKED. A controller in your
// hands spends most of its time at your sides or in your lap, BELOW the camera cone, where the
// runtime keeps the pose VALID from the IMU but drops the TRACKED bit. So "TRACKED" was labelling
// *on the desk*, and the 76 fps bucket was the bad case all along - the data agreed with the
// report and the label disagreed with both.
//
// What actually distinguishes the two states is MOTION. A held controller never stops moving; a
// desk controller is still to within noise. So measure that instead of asking the runtime a
// question it was not being asked.
float g_handSpeed[2] = { 0.0f, 0.0f };      // smoothed metres/frame
bool  g_handHeld[2] = { false, false };
XrVector3f g_handLastPos[2]{};
bool  g_handHavePos[2] = { false, false };

// ---- ❌ run 74: the governor is dead too, and the approach changes ----
//
// NUMPAD+ neutralised every render-thread sleep and the frame rate did not move. So
// `bSmoothFrameRate` was not holding anything down either. That is four mechanisms proposed and
// four eliminated - packet churn, poll rate, force feedback, and now the governor - each one
// diagnosed from a symptom that turned out to be a coincidence of something else.
//
// The honest read: root-causing this from the outside is not converging, and the one durable,
// draw-controlled fact in the whole session is still run 70's - **pad OFF is 119 fps and 0.6%
// missed; pad ON is 76-91 fps.** Every attempt to explain WHY has failed; the correlation itself
// has not.
//
// So stop explaining and start using it. Turning the pad off when nobody is holding a controller
// is worth having regardless of the cause - a gamepad that is not in anyone's hands has no
// business being fed to the game - and it converts the outstanding question into a CONTINUOUS
// experiment instead of another one-shot A/B. If the judder tracks the auto-toggle over an entire
// session, the pad is the cause and this is the fix. If it does not, that is the cleanest evidence
// yet that the pad is innocent and run 70's table was confounded like everything else.
//
// ---- "set down but not asleep" is exactly what MOTION distinguishes ----
//
// Asleep is the wrong question, and the accidental-trigger observation already proved it: a
// controller can be wide awake, delivering input, and still be sitting on a desk. Motion is the
// signal that separates them - a held controller never stops moving, because a hand at rest still
// drifts by fractions of a millimetre every frame, and a desk does not drift at all.
//
// ⚠️ The threshold is a guess, and run 72's guess was wrong by enough to collapse a whole table.
// So the measured speeds are logged now, and the number can be set from data rather than from
// another guess.
int  g_autoPad = 1;                 // ini AutoPad
bool g_autoPadDisabled = false;     // did WE turn it off, or did the user with NUMPAD3
int  g_handStillFrames = 0;

// ⚠️ Re-detection is NOT instant. Run 60 measured UE3 enumerating ports round-robin at about one
// per second, so port 0 comes round every ~2 s and a pad only "appears" on the first successful
// read after that. Picking the controllers up will therefore cost a second or two before they
// work. That is why the disable is slow (3 s of stillness) and the enable is immediate - the
// asymmetry puts the whole latency where it is least annoying.
// ---- ✅ run 75 RESULT: it is the INTERACTION, and auto-pad removes it ----
//
//   pad off, feed ON, ON DESK : 109.3 fps | residual  4.13 | MISSED  2.1% | 1316 draws | 5193 fr
//   pad ON,  feed ON, in hand : 117.9 fps | residual  4.14 | MISSED  1.0% | 1563 draws | 2913 fr
//   pad ON,  feed ON, ON DESK :  71.9 fps | residual 10.40 | MISSED 18.7% | 1148 draws | 1176 fr
//
// **Neither factor is the cause on its own.** A connected pad while the controllers are in hand is
// fine (1.0% missed). Controllers on the desk with no pad is fine (2.1%). Only both together break
// it - 18.7% missed at 71.9 fps. Every earlier run tested one factor at a time and was therefore
// asking a question with no answer, which is why four consecutive mechanisms each looked plausible
// and each died.
//
// The activity confound is controlled the right way round this time: the bad row has the FEWEST
// draws (1148 against 1316 and 1563) and is still the slowest by 40 fps.
//
// The remaining 1176 bad frames are ~16 seconds, which is about three 3-second detection windows
// plus the re-enumeration after each - in other words, the exposure is now only the delay before
// auto-pad disengages, and the user reports no judder over a full session.
//
// The MECHANISM is still unknown. That is worth saying plainly rather than dressing up: five
// theories have died here, and this is a workaround built on a correlation that has survived every
// test rather than an explanation that has passed one.
//
// ---- why the timeout stays at 3 s and does not get shortened ----
//
// Tempting, because the 3 s window is now the whole remaining exposure. But the costs are
// asymmetric: a premature unplug costs ~2-3 s of DEAD CONTROLS mid-combat while the game
// re-enumerates, and that is far worse than 3 s of judder. Measured speeds leave room but not a
// lot - in hand runs 878 to 10359 um/frame, a resting desk reads 0, and one borderline sample came
// in at 389. Holding still to line up a shot is a real thing a player does.
//
// The `||` below is what makes 3 s safe: BOTH controllers have to be still. Aiming with one hand
// steady while the other moves keeps the pad alive.
// Raised to 5 s in run 77. With the capacitive sensors answering the question directly, putting a
// controller down is unambiguous, so waiting longer costs nothing and absorbs a momentary blip
// while re-gripping - which under the motion detector would have been another unplug/replug cycle.
int g_handStillFramesToDisable = 600;   // ~5 s at 120 fps; ini AutoPadStillFrames

void UpdateAutoPad() {
    if (!g_autoPad) return;
    if (!g_xrInputReady) return;

    const bool held = g_handHeld[0] || g_handHeld[1];
    if (held) g_handStillFrames = 0; else ++g_handStillFrames;

    const bool padOn = InterlockedCompareExchange(&g_padEmu, 0, 0) != 0;

    if (padOn && !held && g_handStillFrames >= g_handStillFramesToDisable) {
        InterlockedExchange(&g_padEmu, 0);
        ClearPad();
        g_autoPadDisabled = true;
        Log("auto-pad: controllers still for %.1f s -> gamepad UNPLUGGED from the game",
            g_handStillFramesToDisable / 120.0);
    } else if (!padOn && held && g_autoPadDisabled) {
        InterlockedExchange(&g_padEmu, 1);
        g_autoPadDisabled = false;
        Log("auto-pad: controller picked up -> gamepad plugged back in"
            " (the game re-enumerates, so give it a second or two)");
    }
}
volatile LONG g_xrInputOff = 0;     // numpad / - kill all controller input work
int g_syncFailFrames = 0;           // frames where xrSyncActions did not return XR_SUCCESS

// ---- ✅ run 67 RESULT: it is ours, and now it gets timed rather than guessed at ----
//
// Confirmed by the cheap control: controllers DOWN + input ON = juddering; controllers DOWN +
// input OFF (numpad /) = normal. So the cost is inside SyncXRInput, and it appears only while the
// controllers are asleep. The haptics gate added the same run did NOT fix it, which eliminates the
// most obvious candidate outright.
//
// ⚠️ SyncXRInput is currently one undifferentiated lump inside "our work" - which is precisely the
// situation run 56 spent a day inside when xrEndFrame hid a 189 ms stall in that same residual.
// STATUS says it in as many words: "our work" is not a measurement, it is the residual, and it
// will absorb any amount of someone else's latency while looking exactly like your own bug.
//
// So break it into its four phases before touching a line of it. There are ~16 runtime calls here
// per frame - one sync, thirteen action-state reads, two space locations - and any of them could be
// the one that goes slow on a sleeping controller. Guessing picks one in sixteen; timing names it.
double g_msSyncActions = 0.0, g_msActionStates = 0.0, g_msPoses = 0.0, g_msHaptics = 0.0;
double g_pkSyncActions = 0.0, g_pkActionStates = 0.0, g_pkPoses = 0.0, g_pkHaptics = 0.0;
int    g_syncSamples = 0;

// ---- the controller correlation, bucketed rather than remembered (run 68) ----
//
// Run 67's timing came back at under 0.1 ms peak across all four phases. That EXONERATES the input
// calls: whatever costs the judder, it is not xrSyncActions, the action-state reads, the pose
// locations or the haptics. Something real is happening - it was reproduced three times by hand -
// but the mechanism is not where it was looked for.
//
// It also came back from a run that did not contain the experiment: `NUMPAD/` was never pressed
// and the controllers were never BOTH asleep, so there was no controllers-down sample to compare
// against. Asking someone in a headset to hold a four-cell protocol in their head while playing is
// not a reasonable instrument, and this is the second run it has cost.
//
// ⚠️ And there is a 99 ms worst-frame XR submit in that same log, with 38 ms and 17 ms behind it.
// That is the documented WIRELESS LINK signature - STATUS: "If the worst frame is tens of ms, the
// dips are the streaming path and no change to the render code will help." The link is also
// exactly what burned this project in run 56, where three conclusions drawn from A/Bs taken across
// it all had to be retracted, and where the identical binary measured 4% and 84% of windows over
// 15 ms in one evening. **A by-hand controllers-up/down comparison is not safe against that.**
//
// So stop comparing two moments and compare two POPULATIONS. Every frame is bucketed by the state
// it was rendered in, and the buckets are reported side by side with their sample counts. Link
// noise lands in both buckets and averages out; a real effect does not.
// ---- ⚠️ run 68 came back BACKWARDS, and the reason is a confound, not a mystery ----
//
//     input ON,  controllers AWAKE  : 76.0 fps, residual 10.93 ms, 5171 frames
//     input ON,  controllers ASLEEP : 113.5 fps, residual  4.21 ms,  862 frames
//     input OFF, controllers ASLEEP : 119.2 fps, residual  3.81 ms, 1106 frames
//
// The slow case is controllers AWAKE - the opposite of the reported symptom. And the one clean
// comparison in there, asleep with input on vs off, differs by 0.4 ms, which is the input cost
// already measured plus noise. XR submit stays under 1.6 ms in every bucket, so the link is not
// involved either.
//
// The confound: **an awake controller means the player is playing.** Sticks live, walking through
// the level, fighting - which draws more, streams more and costs more, for reasons that have
// nothing to do with the controller being awake. A sleeping controller means standing still. The
// bucket labelled "controllers awake" is really labelled "busy scene", and it was never going to
// answer the question.
//
// The second problem is worse, and it is mine: **mean fps is not judder.** Judder is missed
// deadlines and frame-time spread. A run that averages 113 fps while dropping one frame in twenty
// looks fine in this table and awful in a headset, which is precisely the gap between what the
// numbers said and what was reported. So count what actually judders - frames over the display
// period, and the worst one - and carry draws/frame alongside so the activity confound is VISIBLE
// in the table rather than left to be argued about.
struct StateBucket {
    double frameMs, waitMs, xrSubmitMs, maxFrameMs, drawsSum;
    int frames, missed;
};
// Three binary conditions now, and all three have to be separable: is the game being fed a pad at
// all (NUMPAD3), are we publishing controller state into it (NUMPAD/), and are the controllers in
// a hand or on the desk (measured by motion). Eight rows is a small price for never again having
// two conditions welded together the way run 68 welded "awake" to "input running".
StateBucket g_bucket[8]{};   // [padEmu*4 + notFeeding*2 + onDesk]

const char* BucketName(int i) {
    static char buf[8][80];
    _snprintf_s(buf[i], sizeof(buf[i]), _TRUNCATE, "pad %s, feed %s, controllers %s",
                (i & 4) ? "ON " : "off",
                (i & 2) ? "off" : "ON ",
                (i & 1) ? "ON DESK" : "in hand");
    return buf[i];
}

// ---- Raven's 360 layout, read out of the game's own config (run 61b) ----
//
// `RvGame\CookedPC\Coalesced_INT.bin` is UE3's coalesced config archive, and it carries the
// shipped binding table as UTF-16 text. No guessing was needed after all:
//
//     RT  Fire          LT  Aim              A  Jump        B  Crouch
//     X   Use/reload    Y   CycleWeapon      LB AgeYoung    RB Impulse
//     LSt Dash          RSt AntiGrav         START ShowMenu BACK Scoreboard
//     D-pad up  HEALTH       D-pad down  Flashlight       D-pad left/right  unassigned
//
// Two facts that change the mapping rather than merely confirm it:
//
//   - **D-pad up is the health item.** That is what the run-61 weapon-switch mapping actually hit.
//   - **There is no next/prev weapon, only CYCLE** - one button, and the mouse wheel is bound to
//     the same command in both directions. A two-way stick gesture would have been wrong by
//     construction, so the stick's other direction is free and now carries health.
//
// ⚠️ Method note worth more than the table: the button probe could confirm a binding but could not
// refute one. X, Y, LB and the thumbstick clicks all read as "nothing happened" simply because
// Use, CycleWeapon and the TMD powers do nothing without a target, a second weapon or a charge -
// and D-pad up read as nothing at full health, having demonstrably worked ten minutes earlier.
// **The instrument could only ever report the positive case.** The config file was the better
// instrument and it was sitting in the install the whole time.
//
// Masks stay ini-editable regardless. Values are the standard XINPUT_GAMEPAD_* constants: A=0x1000
// B=0x2000 X=0x4000 Y=0x8000 START=0x0010 BACK=0x0020 LB=0x0100 RB=0x0200 LSTICK=0x0040
// RSTICK=0x0080 DPAD up/down/left/right = 0x0001/0x0002/0x0004/0x0008.
struct PadButtonAction {
    const char* name;        // OpenXR action name (lower case, no spaces - the spec requires it)
    const char* localised;
    const char* path;        // Touch-profile binding. Booleans on `squeeze/value` are legal:
                             // the runtime thresholds a float source for a boolean action.
    const char* iniKey;
    WORD        mask;
    XrAction    action;
    bool        isMenu;      // handled specially - see the long-press note in SyncXRInput
};

// ⚠️ `menu/click` exists on the LEFT hand only in the Touch profile, and `system/click` on the
// right is reserved by the runtime. Both have caught people out; neither is a runtime error, the
// binding is just silently ignored.
PadButtonAction g_padButtons[] = {
    { "jump",       "Jump",           "/user/hand/right/input/a/click",          "Jump",       XI_A,         XR_NULL_HANDLE, false },
    { "crouch",     "Crouch",         "/user/hand/right/input/b/click",          "Crouch",     XI_B,         XR_NULL_HANDLE, false },
    { "use",        "Use / reload",   "/user/hand/right/input/squeeze/value",    "Use",        XI_X,         XR_NULL_HANDLE, false },
    { "antigrav",   "Anti-gravity",   "/user/hand/right/input/thumbstick/click", "AntiGrav",   XI_RTHUMB,    XR_NULL_HANDLE, false },
    { "impulse",    "Impulse",        "/user/hand/left/input/squeeze/value",     "Impulse",    XI_RSHOULDER, XR_NULL_HANDLE, false },
    { "ageyoung",   "Age / renovate", "/user/hand/left/input/x/click",           "AgeYoung",   XI_LSHOULDER, XR_NULL_HANDLE, false },
    { "flashlight", "Flashlight",     "/user/hand/left/input/y/click",           "Flashlight", XI_DPAD_DOWN, XR_NULL_HANDLE, false },
    { "dash",       "Dash",           "/user/hand/left/input/thumbstick/click",  "Dash",       XI_LTHUMB,    XR_NULL_HANDLE, false },
    { "menu",       "Menu",           "/user/hand/left/input/menu/click",        "Menu",       XI_START,     XR_NULL_HANDLE, true  },
};
const int kPadButtonCount = (int)(sizeof(g_padButtons) / sizeof(g_padButtons[0]));

// Turn. Snap steps the yaw base directly rather than driving the look stick, because the engine's
// own turn ramp is exactly what snap must not have: instant and exact is the whole point. Smooth
// goes through the pad instead and inherits Raven's curve, which is what you want there.
int   g_turnMode     = 1;      // ini TurnMode: 0 smooth, 1 snap
float g_turnAngleDeg = 30.0f;  // ini TurnAngle
int   g_turnSign     = 1;      // ini TurnSign - one edit if right turns left
bool  g_snapArmed    = true;

int32_t g_padTurnAccum = 0;    // snap steps waiting to be folded in, in FRotator units

bool XrPathOf(const char* s, XrPath* out) {
    return XR_SUCCEEDED(xrStringToPath(g_xrInstance, s, out));
}

// Defined with the aim-pose code further down, next to the comment that explains what they mean.
// Declared here because the ini is read long before that point in the file.
extern LONG  g_aimPoseUUPer1;
extern LONG  g_aimFieldSet;
extern LONG  g_traceSignYaw, g_traceSignPitch;
extern LONG  g_gunSignYaw, g_gunSignPitch, g_gunSignRoll, g_gunFollowPos;
extern LONG  g_gunSignPosZ, g_gunAnchorFwd, g_gunAnchorRight, g_gunAnchorUp;
extern volatile LONG g_gunSignCombo;
extern float g_aimPoseSignX, g_aimPoseSignY;

bool InitXRInput() {
    if (g_xrInstance == XR_NULL_HANDLE || g_xrSession == XR_NULL_HANDLE) return false;

    {
        char path[MAX_PATH]{};
        if (IniPath(path)) {
            g_turnMode     = GetPrivateProfileIntA("Input", "TurnMode",  1, path);
            g_turnAngleDeg = (float)GetPrivateProfileIntA("Input", "TurnAngle", 30, path);
            g_turnSign     = GetPrivateProfileIntA("Input", "TurnSign",  1, path) >= 0 ? 1 : -1;
            g_stickUpMask   = (WORD)GetPrivateProfileIntA("Input", "StickUp",   g_stickUpMask,   path);
            g_stickDownMask = (WORD)GetPrivateProfileIntA("Input", "StickDown", g_stickDownMask, path);
            g_walkDirection = GetPrivateProfileIntA("Input", "WalkDirection", 1, path);
            g_walkSign      = GetPrivateProfileIntA("Input", "WalkSign", 1, path) >= 0 ? 1 : -1;
            g_autoPad       = GetPrivateProfileIntA("Input", "AutoPad", 1, path);
            // ---- aim mode 7's gun pose, the two numbers that need calibrating ----
            // UUPer1: engine rotation units per 1.0 of anim aim offset. 16384 = 90 deg, which is
            // the common authored range - a guess, and the log prints what it should have been.
            // Signs are separate because X and Y conventions are authored per AnimTree profile and
            // there is no way to read them from here.
            g_aimPoseUUPer1 = GetPrivateProfileIntA("Input", "AimPoseUUPer1", 16384, path);
            g_aimPoseSignX  = GetPrivateProfileIntA("Input", "AimPoseSignX", 1, path) >= 0 ? 1.0f : -1.0f;
            g_aimPoseSignY  = GetPrivateProfileIntA("Input", "AimPoseSignY", 1, path) >= 0 ? 1.0f : -1.0f;
            // Which fields aim mode 9 may write. 0 controller only (default), 1 + mCurrentPOV,
            // 2 every scanned field. See the note on g_aimFieldSet - set 2 moves the camera.
            g_aimFieldSet   = GetPrivateProfileIntA("Input", "AimFieldSet", 0, path);
            // Mode 11 trace rotation: flip if the shot deflects the wrong way.
            g_traceSignYaw   = GetPrivateProfileIntA("Input", "AimTraceSignYaw", 1, path) >= 0 ? 1 : -1;
            g_traceSignPitch = GetPrivateProfileIntA("Input", "AimTraceSignPitch", 1, path) >= 0 ? 1 : -1;
            // Aim mode gun visual: signs and whether the gun follows the hand POSITION as well as
            // its direction. Up/down came back reversed, hence the -1 default on pitch.
            g_gunSignYaw   = GetPrivateProfileIntA("Input", "GunSignYaw",   1, path) >= 0 ? 1 : -1;
            g_gunSignPitch = GetPrivateProfileIntA("Input", "GunSignPitch", -1, path) >= 0 ? 1 : -1;
            g_gunSignRoll  = GetPrivateProfileIntA("Input", "GunSignRoll",  1, path) >= 0 ? 1 : -1;
            g_gunFollowPos = GetPrivateProfileIntA("Input", "GunFollowPosition", 1, path);
            g_gunSignPosZ    = GetPrivateProfileIntA("Input", "GunSignPosZ", -1, path) >= 0 ? 1 : -1;
            // Where the gun sits relative to your eye, in engine units - forward, right, up. This
            // is the point it ROTATES ABOUT, so getting it near the grip is what stops the gun
            // swinging through an arc when you turn your wrist.
            g_gunAnchorFwd   = GetPrivateProfileIntA("Input", "GunAnchorFwd",   25, path);
            g_gunAnchorRight = GetPrivateProfileIntA("Input", "GunAnchorRight",  8, path);
            g_gunAnchorUp    = GetPrivateProfileIntA("Input", "GunAnchorUp",    -8, path);
            // Which sign combination to START in. Set from the ini rather than by changing the
            // sign defaults, because changing those RENUMBERS the combos - which is what made
            // "combo 2" mean two different things in two sessions and cost a run.
            InterlockedExchange(&g_gunSignCombo,
                GetPrivateProfileIntA("Input", "GunSignCombo", 0, path) & 15);
            g_handStillFramesToDisable =
                GetPrivateProfileIntA("Input", "AutoPadStillFrames", 600, path);
            for (int i = 0; i < kPadButtonCount; ++i)
                g_padButtons[i].mask = (WORD)GetPrivateProfileIntA(
                    "Input", g_padButtons[i].iniKey, g_padButtons[i].mask, path);
        }
    }

    XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(asci.actionSetName, "gameplay");
    strcpy_s(asci.localizedActionSetName, "Gameplay");
    if (XR_FAILED(xrCreateActionSet(g_xrInstance, &asci, &g_actionSet))) {
        Log("xrinput: xrCreateActionSet failed - controllers unavailable, gamepad probe still usable");
        return false;
    }

    auto mkAction = [&](const char* n, const char* loc, XrActionType t, XrAction* out) -> bool {
        XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO };
        strcpy_s(aci.actionName, n);
        strcpy_s(aci.localizedActionName, loc);
        aci.actionType = t;
        return XR_SUCCEEDED(xrCreateAction(g_actionSet, &aci, out));
    };

    XrPathOf("/user/hand/left",  &g_handSubPath[0]);
    XrPathOf("/user/hand/right", &g_handSubPath[1]);

    // One vibration action with two subaction paths, rather than two actions. The subaction path
    // is what xrApplyHapticFeedback selects on, so this is the shape the API wants.
    {
        XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO };
        strcpy_s(aci.actionName, "haptic");
        strcpy_s(aci.localizedActionName, "Haptic");
        aci.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
        aci.countSubactionPaths = 2;
        aci.subactionPaths = g_handSubPath;
        if (XR_FAILED(xrCreateAction(g_actionSet, &aci, &g_actHaptic)))
            g_actHaptic = XR_NULL_HANDLE;   // haptics only; input carries on
    }

    {
        XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO };
        strcpy_s(aci.actionName, "grasp");
        strcpy_s(aci.localizedActionName, "Holding the controller");
        aci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        aci.countSubactionPaths = 2;
        aci.subactionPaths = g_handSubPath;
        if (XR_FAILED(xrCreateAction(g_actionSet, &aci, &g_actGrasp)))
            g_actGrasp = XR_NULL_HANDLE;
    }

    {
        XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO };
        strcpy_s(aci.actionName, "handpose");
        strcpy_s(aci.localizedActionName, "Hand pose");
        aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
        aci.countSubactionPaths = 2;
        aci.subactionPaths = g_handSubPath;
        if (XR_FAILED(xrCreateAction(g_actionSet, &aci, &g_actHandPose)))
            g_actHandPose = XR_NULL_HANDLE;   // poses only; the rest of input carries on
    }

    bool ok = true;
    ok &= mkAction("move",    "Move",     XR_ACTION_TYPE_VECTOR2F_INPUT, &g_actMove);
    ok &= mkAction("turn",    "Turn",     XR_ACTION_TYPE_VECTOR2F_INPUT, &g_actTurn);
    ok &= mkAction("fire",    "Fire",     XR_ACTION_TYPE_FLOAT_INPUT,    &g_actFire);
    ok &= mkAction("altfire", "Alt fire", XR_ACTION_TYPE_FLOAT_INPUT,    &g_actAltFire);
    for (int i = 0; i < kPadButtonCount; ++i)
        ok &= mkAction(g_padButtons[i].name, g_padButtons[i].localised,
                       XR_ACTION_TYPE_BOOLEAN_INPUT, &g_padButtons[i].action);
    if (!ok) { Log("xrinput: xrCreateAction failed - controllers unavailable"); return false; }

    // 4 fixed + one per button + 2 haptic outputs. Sized from the same constants that fill it, so
    // adding a row to g_padButtons cannot silently overrun this.
    // 4 fixed + one per button + 2 haptic outputs + 2 hand poses. Sized from the same constants
    // that fill it, so adding a row to g_padButtons cannot silently overrun this.
    XrActionSuggestedBinding binds[4 + kPadButtonCount + 2 + 2 + 12]{};
    uint32_t nb = 0;
    struct { XrAction a; const char* p; } fixed[] = {
        { g_actMove,    "/user/hand/left/input/thumbstick"    },
        { g_actTurn,    "/user/hand/right/input/thumbstick"   },
        { g_actFire,    "/user/hand/right/input/trigger/value" },
        { g_actAltFire, "/user/hand/left/input/trigger/value"  },
    };
    for (auto& f : fixed) {
        XrPath p;
        if (XrPathOf(f.p, &p)) { binds[nb].action = f.a; binds[nb].binding = p; ++nb; }
    }
    for (int i = 0; i < kPadButtonCount; ++i) {
        XrPath p;
        if (XrPathOf(g_padButtons[i].path, &p)) {
            binds[nb].action = g_padButtons[i].action; binds[nb].binding = p; ++nb;
        }
    }
    if (g_actHaptic != XR_NULL_HANDLE) {
        const char* hp[2] = { "/user/hand/left/output/haptic", "/user/hand/right/output/haptic" };
        for (int i = 0; i < 2; ++i) {
            XrPath p;
            if (XrPathOf(hp[i], &p)) { binds[nb].action = g_actHaptic; binds[nb].binding = p; ++nb; }
        }
    }
    if (g_actGrasp != XR_NULL_HANDLE) {
        // ⚠️ Run 82: thumbrest + trigger alone was TOO NARROW. The log caught it outright -
        // "right on desk (2346 um/frame)" is a controller being waved about while the sensors
        // insist nobody is touching it. A thumb parked on the STICK is not on the thumbrest, and
        // an index finger curled round the grip is not on the trigger, so a perfectly normal hold
        // reported as put-down.
        //
        // Every capacitive surface on the controller now feeds the same boolean, so any finger
        // anywhere counts. Being generous is the right bias: a false "held" costs nothing, and a
        // false "put down" unplugs the pad mid-fight.
        const char* gp[12] = { "/user/hand/left/input/thumbrest/touch",
                               "/user/hand/right/input/thumbrest/touch",
                               "/user/hand/left/input/trigger/touch",
                               "/user/hand/right/input/trigger/touch",
                               "/user/hand/left/input/thumbstick/touch",
                               "/user/hand/right/input/thumbstick/touch",
                               "/user/hand/left/input/x/touch",
                               "/user/hand/left/input/y/touch",
                               "/user/hand/right/input/a/touch",
                               "/user/hand/right/input/b/touch",
                               "/user/hand/left/input/squeeze/value",
                               "/user/hand/right/input/squeeze/value" };
        for (int i = 0; i < 12; ++i) {
            XrPath p;
            if (XrPathOf(gp[i], &p)) { binds[nb].action = g_actGrasp; binds[nb].binding = p; ++nb; }
        }
    }
    if (g_actHandPose != XR_NULL_HANDLE) {
        // `aim`, not `grip`: aim is the runtime's own pointing ray for the controller, which is
        // what a weapon direction wants. grip is where the hand holds it, and using that would
        // mean re-deriving the pointing axis ourselves for no gain.
        const char* pp[2] = { "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose" };
        for (int i = 0; i < 2; ++i) {
            XrPath p;
            if (XrPathOf(pp[i], &p)) { binds[nb].action = g_actHandPose; binds[nb].binding = p; ++nb; }
        }
    }

    XrPath profile;
    if (!XrPathOf("/interaction_profiles/oculus/touch_controller", &profile)) {
        Log("xrinput: Touch profile path failed"); return false;
    }
    XrInteractionProfileSuggestedBinding sb{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sb.interactionProfile = profile;
    sb.suggestedBindings = binds;
    sb.countSuggestedBindings = nb;
    XrResult sr = xrSuggestInteractionProfileBindings(g_xrInstance, &sb);
    if (XR_FAILED(sr)) {
        // ⚠️ This fails as a WHOLE if any single binding path is not valid for the profile - it is
        // not a per-binding filter. So a typo in one path silently costs every control, which is
        // why the count and the result are both logged rather than assumed.
        Log("xrinput: xrSuggestInteractionProfileBindings failed (%d) for %u bindings"
            " - one bad path rejects them all", (int)sr, nb);
        return false;
    }

    XrSessionActionSetsAttachInfo ai{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    ai.countActionSets = 1;
    ai.actionSets = &g_actionSet;
    if (XR_FAILED(xrAttachSessionActionSets(g_xrSession, &ai))) {
        Log("xrinput: xrAttachSessionActionSets failed - controllers unavailable"); return false;
    }

    // After attach, deliberately: some runtimes reject xrCreateActionSpace before the action set
    // is attached to the session, and the spec does not require them to accept it.
    if (g_actHandPose != XR_NULL_HANDLE) {
        for (int i = 0; i < 2; ++i) {
            XrActionSpaceCreateInfo sci{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
            sci.action = g_actHandPose;
            sci.subactionPath = g_handSubPath[i];
            sci.poseInActionSpace.orientation.w = 1.0f;
            if (XR_FAILED(xrCreateActionSpace(g_xrSession, &sci, &g_handSpace[i])))
                g_handSpace[i] = XR_NULL_HANDLE;
        }
    }

    g_xrInputReady = true;
    Log("xrinput: %u bindings attached. Turn mode %s, %.0f degrees. Walk direction %s."
        " Hand poses %s.", nb, g_turnMode ? "SNAP" : "smooth", g_turnAngleDeg,
        g_walkDirection ? "OFF-HAND" : "head-relative",
        (g_handSpace[0] && g_handSpace[1]) ? "available" : "UNAVAILABLE");
    return true;
}

// Turns controller state into the synthetic pad. Called once per frame from UpdateFromHeadset,
// on the same thread as everything else that touches g_baseYaw.
void SyncXRInput(XrTime displayTime) {
    if (!g_xrInputReady) return;

    // ⚠️ INSTRUMENT BUG, run 68: this used to return here and force g_handTracked to false, which
    // made "input OFF, controllers AWAKE" a state the log could not physically record - and that
    // was the one row the experiment needed. A control that destroys the label it is being compared
    // against is not a control.
    //
    // So the switch now means what it should have meant: keep READING the controllers, stop FEEDING
    // the game. The reads are measured at 0.005 ms and were never the suspect; what the game does
    // with a live pad has never been isolated at all. This is the better experiment and it keeps
    // the state labels valid on both sides of it.
    const bool feedGame = InterlockedCompareExchange(&g_xrInputOff, 0, 0) == 0;

    XrActiveActionSet aas{ g_actionSet, XR_NULL_PATH };
    XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &aas;
    LARGE_INTEGER tSync = Now();
    const XrResult sr = xrSyncActions(g_xrSession, &si);
    {
        const double ms = MsSince(tSync);
        g_msSyncActions += ms;
        if (ms > g_pkSyncActions) g_pkSyncActions = ms;
        ++g_syncSamples;
    }
    double msStates = 0.0;

    // XR_SESSION_NOT_FOCUSED is a SUCCESS code, not a failure - the call worked and the actions
    // are simply inactive. Zeroing here is what stops the player walking on for ever because the
    // stick was held when the headset was taken off or a system overlay took focus.
    if (sr != XR_SUCCESS) {
        ++g_syncFailFrames;
        InterlockedExchange(&g_padBtns, 0);
        InterlockedExchange(&g_padLStick, PackStick(0, 0));
        InterlockedExchange(&g_padRStick, PackStick(0, 0));
        g_snapArmed = true;
        // Silence the motors too, or taking the headset off mid-firefight leaves a controller
        // buzzing on the desk with nothing left to turn it off.
        InterlockedExchange(&g_rumbleL, 0);
        InterlockedExchange(&g_rumbleR, 0);
        if (g_actHaptic != XR_NULL_HANDLE) {
            for (int hand = 0; hand < 2; ++hand) {
                XrHapticActionInfo hai{ XR_TYPE_HAPTIC_ACTION_INFO };
                hai.action = g_actHaptic;
                hai.subactionPath = g_handSubPath[hand];
                xrStopHapticFeedback(g_xrSession, &hai);
            }
        }
        return;
    }

    auto getVec2 = [&](XrAction a, float* x, float* y) {
        *x = *y = 0.0f;
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = a;
        XrActionStateVector2f st{ XR_TYPE_ACTION_STATE_VECTOR2F };
        if (XR_SUCCEEDED(xrGetActionStateVector2f(g_xrSession, &gi, &st)) && st.isActive) {
            *x = st.currentState.x; *y = st.currentState.y;
        }
    };
    auto getFloat = [&](XrAction a) -> float {
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = a;
        XrActionStateFloat st{ XR_TYPE_ACTION_STATE_FLOAT };
        if (XR_SUCCEEDED(xrGetActionStateFloat(g_xrSession, &gi, &st)) && st.isActive)
            return st.currentState;
        return 0.0f;
    };
    auto getBool = [&](XrAction a) -> bool {
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = a;
        XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
        return XR_SUCCEEDED(xrGetActionStateBoolean(g_xrSession, &gi, &st)) &&
               st.isActive && st.currentState;
    };

    float mx, my, tx, ty;
    LARGE_INTEGER tSt = Now();
    getVec2(g_actMove, &mx, &my);
    getVec2(g_actTurn, &tx, &ty);
    msStates += MsSince(tSt);

    // ---- radial deadzone: the run-70 fix, and it is a correctness fix, not a comfort one ----
    //
    // RADIAL rather than per-axis: a per-axis deadzone leaves a live axis whenever the other one is
    // pushed, so a stick held forward still jitters in x and the state still changes every frame -
    // which is the exact failure being removed. Killing the whole vector when its MAGNITUDE is
    // small is what makes a resting stick bit-identical between frames.
    //
    // Rescaled from the deadzone edge so the response stays continuous: without that, movement
    // starts abruptly at 22% speed the moment the stick leaves the zone.
    auto radialDeadzone = [](float* x, float* y, float dz) {
        const float m = sqrtf((*x) * (*x) + (*y) * (*y));
        if (m <= dz) { *x = 0.0f; *y = 0.0f; return; }
        const float s = ((m - dz) / (1.0f - dz)) / m;
        *x *= s; *y *= s;
    };
    // Raven's own config asks for 0.3 on the move stick and 0.2 on look
    // (`Axis aStrafe Speed=1.0 DeadZone=0.3`), so the engine expects a pad that has already been
    // conditioned. Matching those keeps the feel the game was tuned for.
    radialDeadzone(&mx, &my, 0.30f);
    radialDeadzone(&tx, &ty, 0.20f);

    // ---- where the hands are ----
    LARGE_INTEGER tPose = Now();
    for (int i = 0; i < 2; ++i) {
        g_handPoseValid[i] = false;
        g_handTracked[i] = false;
        if (g_handSpace[i] == XR_NULL_HANDLE) continue;
        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        if (XR_FAILED(xrLocateSpace(g_handSpace[i], g_xrSpace, displayTime, &loc))) continue;
        // TRACKED, not merely VALID. A set-down controller keeps reporting a valid last-known
        // pose long after it has stopped being tracked, so the valid bit alone cannot tell a live
        // controller from a sleeping one - which is exactly the distinction under investigation.
        g_handTracked[i] = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
        if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) continue;
        const XrQuaternionf& q = loc.pose.orientation;
        // The same Y-X-Z decomposition the head uses, so hand and head angles are directly
        // comparable rather than two separately-derived conventions that agree by luck.
        float sinp = 2.0f * (q.w * q.x - q.y * q.z);
        if (sinp >  1.0f) sinp =  1.0f;
        if (sinp < -1.0f) sinp = -1.0f;
        g_handPitchRad[i] = asinf(sinp);
        g_handYawRad[i] = atan2f(2.0f * (q.w * q.y + q.z * q.x),
                                 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        // Run 123: the third angle of the same Y-X-Z decomposition. Without it, twisting the
        // controller does nothing to the gun - which is one of the three axes reported missing.
        g_handRollRad[i] = atan2f(2.0f * (q.w * q.z + q.x * q.y),
                                  1.0f - 2.0f * (q.x * q.x + q.z * q.z));
        // Held or on a desk, decided by movement. The threshold is well above sensor noise for a
        // stationary controller and far below anything a hand does, even a still one - a hand at
        // rest still drifts by millimetres a frame, and a desk does not.
        if (g_handHavePos[i]) {
            const float dx = loc.pose.position.x - g_handLastPos[i].x;
            const float dy = loc.pose.position.y - g_handLastPos[i].y;
            const float dz = loc.pose.position.z - g_handLastPos[i].z;
            const float d = sqrtf(dx*dx + dy*dy + dz*dz);
            g_handSpeed[i] = g_handSpeed[i] * 0.95f + d * 0.05f;
        }
        g_handLastPos[i] = loc.pose.position;
        g_handHavePos[i] = true;

        g_handPoseValid[i] = true;
    }

    // Held, decided by the capacitive sensors and only falling back to motion when the runtime
    // does not expose them. `isActive` is what says whether the binding resolved at all, so a
    // profile without thumbrest/trigger touch degrades to the old behaviour instead of reporting
    // every controller as permanently set down.
    for (int i = 0; i < 2; ++i) {
        bool touched = false, haveTouch = false;
        if (g_actGrasp != XR_NULL_HANDLE) {
            XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
            gi.action = g_actGrasp;
            gi.subactionPath = g_handSubPath[i];
            XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(xrGetActionStateBoolean(g_xrSession, &gi, &st)) && st.isActive) {
                haveTouch = true;
                touched = st.currentState != 0;
            }
        }
        if (haveTouch) {
            g_graspAvailable = true;
            g_handHeld[i] = touched;
        } else {
            // 100 um, not 500: this is now only being asked "did it move at all".
            g_handHeld[i] = g_handSpeed[i] > 0.0001f;
        }
    }
    {
        const double ms = MsSince(tPose);
        g_msPoses += ms;
        if (ms > g_pkPoses) g_pkPoses = ms;
    }

    // ---- off-hand-relative movement ----
    //
    // The engine's aBaseY/aStrafe are relative to PlayerController.Rotation, which head tracking
    // currently drives - so movement is HEAD-relative by construction today. Pointing it at the
    // left hand instead is one rotation of the stick vector by the angle between them, which is
    // exact because both angles are known.
    //
    // This also happens to be the check that the pose plumbing is correct: if walking follows your
    // left hand rather than sliding off at an angle, the conventions are right.
    if (g_walkDirection && g_handPoseValid[0] && (mx != 0.0f || my != 0.0f)) {
        float d = g_walkSign * (g_handYawRad[0] - g_headYawRad);
        while (d >  3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        const float c = cosf(d), s = sinf(d);
        const float rx = mx * c - my * s;
        const float ry = mx * s + my * c;
        mx = rx; my = ry;
    }

    WORD btns = 0;
    bool menuHeld = false;
    WORD menuMask = 0;
    tSt = Now();
    for (int i = 0; i < kPadButtonCount; ++i) {
        const bool down = getBool(g_padButtons[i].action);
        if (g_padButtons[i].isMenu) { menuHeld = down; menuMask = g_padButtons[i].mask; continue; }
        if (down) btns |= g_padButtons[i].mask;
    }
    msStates += MsSince(tSt);

    // ---- recentre, which had no controller binding at all ----
    //
    // F8 recentres, and F8 cannot be found by feel inside a headset - so in practice the mod could
    // not be recentred while it was being worn, which is the only time it is ever needed.
    //
    // Every button on both controllers is spoken for, so this is a long press rather than a new
    // binding: hold MENU for a second to recentre, tap it for the pause menu. The tap has to fire
    // on RELEASE for that to work, which also happens to be what Raven's own config does with this
    // button (`|onrelease showmenu`).
    static int  menuFrames = 0;
    static bool menuConsumed = false;
    static int  menuTapPulse = 0;
    const int kMenuLongFrames = 120;        // ~1 s at 120 fps, ~2 s at 60 - deliberately generous
    if (menuHeld) {
        if (++menuFrames >= kMenuLongFrames && !menuConsumed) {
            g_haveCentre = false;
            menuConsumed = true;
            Log("menu held: recentred");
        }
    } else {
        if (menuFrames > 0 && !menuConsumed) menuTapPulse = 8;   // short press -> pause menu
        menuFrames = 0;
        menuConsumed = false;
    }
    if (menuTapPulse > 0) { btns |= menuMask; --menuTapPulse; }

    // Weapon cycle (up) and health (down) on the look stick's vertical axis. It is free in both
    // turn modes - smooth turn only ever feeds X to the look stick, because a thumbstick that
    // pitches the view is the one control VR reliably makes people ill with.
    //
    // ⚠️ Held for several frames rather than pulsed for one. Both commands are
    // `PressChainButton | OnRelease ReleaseChainButton` pairs in Raven's config, so the engine
    // wants a press AND a release, and a one-frame flick can land between its own input ticks.
    static bool stickArmed = true;
    static int  stickHold = 0;
    static WORD stickMask = 0;
    if (fabsf(ty) > 0.7f && stickArmed) {
        stickMask = (ty > 0) ? g_stickUpMask : g_stickDownMask;
        stickHold = 8;
        stickArmed = false;
    } else if (fabsf(ty) < 0.4f) {
        stickArmed = true;
    }
    if (stickHold > 0) { btns |= stickMask; --stickHold; }

    // ---- turn ----
    //
    // Snap only works while head tracking owns yaw: with it off, the engine owns the rotation
    // outright and stepping our base would move nothing at all - silently. Falling back to smooth
    // there means the stick always turns, whatever else is switched on.
    const bool headOwnsYaw = InterlockedCompareExchange(&g_enabled, 0, 0) != 0;
    float lookX = 0.0f;
    if (g_turnMode && headOwnsYaw) {
        if (fabsf(tx) > 0.7f && g_snapArmed) {
            const int32_t step = (int32_t)(g_turnAngleDeg * (65536.0f / 360.0f));
            g_padTurnAccum += (tx > 0 ? 1 : -1) * g_turnSign * step;
            g_snapArmed = false;
        } else if (fabsf(tx) < 0.4f) {
            g_snapArmed = true;
        }
    } else {
        lookX = tx;
    }

    tSt = Now();
    float lt = getFloat(g_actAltFire), rt = getFloat(g_actFire);
    msStates += MsSince(tSt);
    // Triggers get the same treatment for the same reason: a resting trigger reporting 0.004 turns
    // into a byte that flickers between 0 and 1, and that is a state change every frame.
    if (lt < 0.10f) lt = 0.0f;
    if (rt < 0.10f) rt = 0.0f;
    InterlockedExchange(&g_rawTrigger, (LONG)(rt * 255.0f));
    g_msActionStates += msStates;
    if (msStates > g_pkActionStates) g_pkActionStates = msStates;
    const LONG packedBtns = (LONG)btns
                          | ((LONG)(BYTE)(lt * 255.0f) << 16)
                          | ((LONG)(BYTE)(rt * 255.0f) << 24);

    auto toAxis = [](float v) -> int16_t {
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        return (int16_t)(v * 32767.0f);
    };
    if (feedGame) {
        InterlockedExchange(&g_padBtns, packedBtns);
        InterlockedExchange(&g_padLStick, PackStick(toAxis(mx), toAxis(my)));
        InterlockedExchange(&g_padRStick, PackStick(toAxis(lookX), 0));
    } else {
        ClearPad();
    }

    // ---- play whatever rumble the game asked for ----
    //
    // Not re-sent every frame: at 120 Hz that is 120 haptic calls a second per hand for a value
    // that mostly is not changing, and each one crosses into the runtime. Sent on a meaningful
    // change, and refreshed periodically so a sustained effect does not expire mid-rumble.
    LARGE_INTEGER tHap = Now();
    if (g_actHaptic != XR_NULL_HANDLE) {
        static float lastAmp[2] = { -1.0f, -1.0f };
        static int   refresh = 0;
        ++refresh;
        const LONG raw[2] = { InterlockedCompareExchange(&g_rumbleL, 0, 0),
                              InterlockedCompareExchange(&g_rumbleR, 0, 0) };
        for (int hand = 0; hand < 2; ++hand) {
            // Nothing sent to a controller that is not being tracked. A sleeping Touch controller
            // is a plausible source of a slow runtime call, and this mod asks for haptics every
            // time the game rumbles - which in combat is constantly. Cheap to rule out, and
            // correct on its own terms regardless: buzzing a controller on a desk helps nobody.
            const float amp = (feedGame && g_handHeld[hand]) ? (float)raw[hand] / 65535.0f : 0.0f;
            const bool changed = fabsf(amp - lastAmp[hand]) > 0.02f;
            const bool expiring = amp > 0.0f && (refresh % 15) == 0;
            if (!changed && !expiring) continue;
            lastAmp[hand] = amp;

            XrHapticActionInfo hai{ XR_TYPE_HAPTIC_ACTION_INFO };
            hai.action = g_actHaptic;
            hai.subactionPath = g_handSubPath[hand];
            if (amp <= 0.0f) {
                xrStopHapticFeedback(g_xrSession, &hai);
            } else {
                XrHapticVibration hv{ XR_TYPE_HAPTIC_VIBRATION };
                hv.duration  = 200LL * 1000000LL;      // 200 ms in ns - longer than the refresh
                hv.frequency = XR_FREQUENCY_UNSPECIFIED;
                hv.amplitude = amp;
                xrApplyHapticFeedback(g_xrSession, &hai,
                                      reinterpret_cast<const XrHapticBaseHeader*>(&hv));
            }
        }
    }
    {
        const double ms = MsSince(tHap);
        g_msHaptics += ms;
        if (ms > g_pkHaptics) g_pkHaptics = ms;
    }
}

void UpdateFromHeadset(IDirect3DDevice9* gameDev) {
    if (!g_xrReady) return;
    PumpXR();
    if (!g_xrRunning) return;

    XrFrameState fs{ XR_TYPE_FRAME_STATE };
    LARGE_INTEGER tWait = Now();
    if (XR_FAILED(xrWaitFrame(g_xrSession, nullptr, &fs))) {
        // Not merely a skipped frame: returning without clearing would leave the pad latched at
        // whatever was last published, and the player walking.
        ClearPad();
        return;
    }
    g_msWaitFrame += MsSince(tWait);
    ++g_waitFrames;

    // After xrWaitFrame because locating the hands needs its predicted display time, and before
    // xrLocateViews so the yaw base is settled for the whole frame that follows - the 6-DOF
    // transform below reads g_baseYaw too, and a snap applied between the two would rotate the
    // view and the positional frame by different amounts for one frame.
    SyncXRInput(fs.predictedDisplayTime);
    if (g_padTurnAccum) {
        // Discarded rather than applied before the first centre: g_baseYaw is seeded from the
        // game's own yaw at that moment, so anything accumulated beforehand would be overwritten
        // and a snap pressed during a load would appear to do nothing.
        if (g_haveCentre) g_baseYaw += g_padTurnAccum;
        g_padTurnAccum = 0;
    }
    // The pacing target, measured rather than assumed. A frame time that sits at an exact
    // multiple of this is the signature of missing the deadline and waiting out a whole extra
    // display period - a very different problem from being uniformly slow, and the fix for it
    // is to get under the period rather than to shave a few percent off anything.
    if (g_displayPeriodMs == 0.0 && fs.predictedDisplayPeriod > 0) {
        g_displayPeriodMs = double(fs.predictedDisplayPeriod) / 1.0e6;   // ns -> ms
        Log("headset display period %.2f ms (%.1f Hz) - frame times at 2x this are a missed deadline",
            g_displayPeriodMs, 1000.0 / g_displayPeriodMs);
    }
    // ---- run 56: xrBeginFrame/xrEndFrame have never been timed, and they are the black box ----
    //
    // The frame budget breaks out xrWaitFrame, the copy and the GObjects walk, and calls everything
    // else "our work". xrEndFrame lives in that remainder - and it is the ONE call in this whole
    // mod that talks to the wireless link, because for VDXR it triggers encode and transmit.
    //
    // That matters because the unmodded game at the same resolution on the same install runs clean,
    // so whatever costs the seconds-long stalls is ours. Every other candidate has been eliminated
    // by test: head roll, the reverted probe, the render-scale change, the runtime teardown. The
    // submission path is what is left, and it has never been separated from the game's own CPU work.
    //
    // Timing only. No allocation, no hook, no behaviour change - QueryPerformanceCounter around two
    // calls that were already being made.
    LARGE_INTEGER tXrBegin = Now();
    xrBeginFrame(g_xrSession, nullptr);
    g_msXrSubmit += MsSince(tXrBegin);

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
        // The third angle of the same Y-X-Z decomposition the two above come from, so all three
        // are consistent with each other rather than three separately-derived conventions.
        // OpenXR is Y-up with forward at -Z, so roll is the rotation about Z.
        float rollRad = atan2f(2.0f * (q.w * q.z + q.x * q.y),
                               1.0f - 2.0f * (q.x * q.x + q.z * q.z));
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

            // ---- ⭐ the weapon's NEUTRAL, captured here (run 78) ----
            //
            // Reported: turning the head RIGHT dragged the gun along; turning LEFT did not. An
            // asymmetric symptom from symmetric code means the quantity being clamped is not
            // centred on zero - and it is not. Nobody holds a controller pointing exactly where
            // they are looking; a natural grip sits some degrees off. So the resting deviation is
            // already non-zero, and the clamp binds on whichever side that offset eats into first.
            //
            //     rest at -15 deg, clamp 30 : turn right 20 -> -35, CLAMPED, gun drags with head
            //                                 turn left  20 -> +5,  free,    gun stays put
            //
            // Capturing hand-minus-head at recentre makes the resting pose deviation zero, so the
            // clamp is symmetric about where the gun is actually held. This is the same thing
            // RTCW Quest needed `vr_weapon_pitchadjust` and per-weapon offsets for - weapon
            // alignment is a calibration problem, not only a plumbing one.
            //
            // Recentre is the right moment because it is deliberate: hold the gun where it feels
            // neutral, hold MENU for a second, and that pose becomes straight ahead.
            if (g_handPoseValid[1]) {
                float dy = g_handYawRad[1] - yawRad;
                while (dy >  3.14159265f) dy -= 6.2831853f;
                while (dy < -3.14159265f) dy += 6.2831853f;
                g_handYawOffset   = dy;
                g_handPitchOffset = g_handPitchRad[1] - pitchRad;
                g_haveHandOffset  = true;
            } else {
                g_haveHandOffset = false;
            }

            Log("recentred: head yaw %.1f, game yaw %d, head pos (%.3f, %.3f, %.3f)%s"
                " | weapon neutral %s (yaw %+.1f, pitch %+.1f from the head)",
                yawRad * 57.2958f, g_baseYaw,
                g_centrePos[0], g_centrePos[1], g_centrePos[2],
                g_haveCentrePos ? "" : " [POSITION NOT TRACKED]",
                g_haveHandOffset ? "captured" : "NOT captured (right controller not tracked)",
                g_handYawOffset * 57.2958f, g_handPitchOffset * 57.2958f);
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
            // Run 123: the head position in XR space, so the hand can be expressed relative to it.
            g_headPosXR[0] = hp.x; g_headPosXR[1] = hp.y; g_headPosXR[2] = hp.z;

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
        // Published for next frame's walk-direction transform - see the note on g_headYawRad.
        g_headYawRad = yawRad;
        g_headRollRad = rollRad;

        // ---- ⭐ run 112: the hand's deviation from the head, measured directly ----
        //
        // Mode 11 derived this by differencing two engine-space values - g_aimYawUU (built from
        // g_baseYaw, the recentre offset and g_centreYaw) against g_wantYaw - and got 121 degrees
        // of yaw and a pitch pinned at the +-16000 clamp. Every one of those terms is a place for
        // a bias to enter, and one of them clearly did: the recentre offsets are only captured
        // when the right controller is tracked at the moment MENU is held, so a session without a
        // clean recentre carries an arbitrary constant.
        //
        // Aim mode 1 never exposed it because it CLAMPS the deviation to 30 degrees, so a biased
        // hand direction still looks like it follows the hand.
        //
        // The physical quantity wanted is just the angle between where the hand points and where
        // the head points, and both are already known in the same XR frame. No base, no centre, no
        // offsets, nothing that can drift - if the hand and head point the same way this is zero
        // by construction, which none of the derived versions could promise.
        if (g_handPoseValid[1]) {
            float dvy = g_handYawRad[1] - yawRad;
            while (dvy >  3.14159265f) dvy -= 6.2831853f;
            while (dvy < -3.14159265f) dvy += 6.2831853f;
            const float dvp = g_handPitchRad[1] - pitchRad;
            // ---- ⭐ run 113: apply the SAME sign convention as everything else ----
            //
            // Reported: left/right mirrored, up/down correct. That is not a coincidence and it is
            // not the ini - g_yawSign is -1 and g_pitchSign is +1, because XR is right-handed with
            // Y up while UE3 is left-handed with Z up. Every other conversion in this file applies
            // them:
            //
            //     g_wantYaw = g_baseYaw + (int32_t)(g_yawSign * dYaw * kRadToUU);
            //
            // and this one did not. So yaw came out mirrored and pitch came out right, which is
            // exactly the report. Deriving a new quantity without the conversion the rest of the
            // file already uses is the same error as re-deriving an offset that was already known.
            //
            // Using the variables rather than a constant also means F5 and F4 keep working: flip
            // the yaw sign at runtime and the bullet deflection follows it.
            InterlockedExchange(&g_handDevYawUU,   (LONG)(g_yawSign   * dvy * kRadToUU));
            InterlockedExchange(&g_handDevPitchUU, (LONG)(g_pitchSign * dvp * kRadToUU));
            // ---- run 123: roll, and the hand's POSITION relative to the head ----
            //
            // Reported: the gun tracks left, right, up and down but not forward, back or twist.
            // Both gaps are the same omission - only yaw and pitch were ever computed, so the
            // transform could only ever swing the gun around a sphere centred on the eye.
            //
            // Roll is the third angle of the decomposition the hands already use. Position goes
            // through the SAME two-stage basis as the head offset above - centre-yaw to get the
            // head's own frame, then body yaw to get world - so hand and head are expressed the
            // same way rather than by two conventions that agree by luck.
            float dvr = g_handRollRad[1] - g_headRollRad;
            while (dvr >  3.14159265f) dvr -= 6.2831853f;
            while (dvr < -3.14159265f) dvr += 6.2831853f;
            InterlockedExchange(&g_handDevRollUU, (LONG)(dvr * kRadToUU));
            {
                const float hx = g_handLastPos[1].x - g_headPosXR[0];
                const float hy = g_handLastPos[1].y - g_headPosXR[1];
                const float hz = g_handLastPos[1].z - g_headPosXR[2];
                const float sc2 = sinf(g_centreYaw), cc2 = cosf(g_centreYaw);
                const float fwd   = -(hx * sc2) - (hz * cc2);
                const float right =  (hx * cc2) - (hz * sc2);
                const float byr = g_baseYaw * (6.2831853f / 65536.0f);
                const float bc2 = cosf(byr), bs2 = sinf(byr);
                InterlockedExchange(&g_handOffX, (LONG)((bc2 * fwd - bs2 * right) * kMetresToUU));
                InterlockedExchange(&g_handOffY, (LONG)((bs2 * fwd + bc2 * right) * kMetresToUU));
                InterlockedExchange(&g_handOffZ, (LONG)(hy * kMetresToUU));
            }
            InterlockedExchange(&g_handDevValid, 1);
        } else {
            // No hand, no deviation. Falling back to zero means the shot goes where you look,
            // which is the pre-mod behaviour rather than a stale direction from wherever the
            // controller was last seen.
            InterlockedExchange(&g_handDevYawUU, 0);
            InterlockedExchange(&g_handDevPitchUU, 0);
            InterlockedExchange(&g_handDevValid, 0);
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

        // The split. The matrix is handed exactly the angles the engine would have been handed,
        // so the signs are inherited from code already known to be right rather than guessed
        // again - which is most of why this is expected to be a no-op rather than hoped to be.
        //
        // The engine is aimed as far towards the BODY's facing as the deviation clamp allows, and
        // the matrix supplies whatever is left over. At N = 0 the engine gets the full head angle
        // and the matrix rotation is identically zero, so the split collapses to today's
        // behaviour - which makes N = 0 a genuine control rather than a nominal one.
        // ---- ⭐ AIM DECOUPLING (run 76): point the engine at the HAND, not the body ----
        //
        // Run 64 decoupled the engine all the way to the body's facing and geometry vanished
        // behind the player. That was the worst case by construction and it is not what aim
        // decoupling needs: `PlayerController.Rotation` should carry the WEAPON, and a weapon
        // follows your hand, which stays far closer to your gaze than your body does. Run 65's
        // attempt to measure the tolerable angle was invalid and run 67's environment made every
        // reading suspect, so the bound is still unknown - but the bound only has to cover
        // hand-vs-head, and it is now bounded on purpose by the same clamp.
        //
        // What this buys, immediately and without hiding anything: UE3 orients the first-person
        // weapon from the controller rotation, so with the engine following the hand **the gun on
        // screen swings with your hand** rather than staying welded to your gaze. Its POSITION is
        // still the camera's, which will look odd - the gun hangs off your face rather than out of
        // your fist - but the DIRECTION becomes correct, and the fire trace follows it for free
        // because we are feeding the engine rather than fighting it. That is the whole reason for
        // this architecture over hooking the fire trace.
        //
        // ⚠️ Falls back to the head whenever the right hand is not tracked, so setting the
        // controller down cannot leave the engine aimed at a stale direction with the view
        // detached from it.
        // The hand's aim in engine units, computed unconditionally so modes 2 and 3 can use it
        // without any of the matrix-split machinery being involved.
        g_aimYawUU = g_wantYaw;
        g_aimPitchUU = g_wantPitch;
        if (g_handPoseValid[1]) {
            float dh = g_handYawRad[1] - (g_haveHandOffset ? g_handYawOffset : 0.0f) - g_centreYaw;
            while (dh >  3.14159265f) dh -= 6.2831853f;
            while (dh < -3.14159265f) dh += 6.2831853f;
            g_aimYawUU = g_baseYaw + (int32_t)(g_yawSign * dh * kRadToUU);
            int32_t ap = (int32_t)(g_pitchSign
                         * (g_handPitchRad[1] - (g_haveHandOffset ? g_handPitchOffset : 0.0f))
                         * kRadToUU);
            if (ap >  16000) ap =  16000;
            if (ap < -16000) ap = -16000;
            g_aimPitchUU = ap;
        }

        const bool aimOn = InterlockedCompareExchange(&g_aimDecouple, 0, 0) != 0;
        int32_t engineYawTarget  = g_wantYaw;      // where the ENGINE should point
        int32_t enginePitchTarget = g_wantPitch;
        if (aimOn && g_handPoseValid[1]) {
            // The right hand, mapped through exactly the transform the head uses - same centre,
            // same signs - and then corrected by the neutral captured at recentre, so "pointing
            // where I am looking" comes out as zero deviation rather than whatever your grip
            // happens to be.
            float dYawHand = g_handYawRad[1] - (g_haveHandOffset ? g_handYawOffset : 0.0f)
                           - g_centreYaw;
            while (dYawHand >  3.14159265f) dYawHand -= 6.2831853f;
            while (dYawHand < -3.14159265f) dYawHand += 6.2831853f;
            engineYawTarget = g_baseYaw + (int32_t)(g_yawSign * dYawHand * kRadToUU);
            int32_t hp = (int32_t)(g_pitchSign
                         * (g_handPitchRad[1] - (g_haveHandOffset ? g_handPitchOffset : 0.0f))
                         * kRadToUU);
            if (hp >  16000) hp =  16000;
            if (hp < -16000) hp = -16000;
            enginePitchTarget = hp;
        } else if (aimOn) {
            // No hand: aim where you look, which is the pre-decoupling behaviour and the only
            // safe default.
            engineYawTarget = g_wantYaw;
            enginePitchTarget = g_wantPitch;
        }

        {
            int stepIdx = (int)InterlockedCompareExchange(&g_devStep, 0, 0);
            if (InterlockedCompareExchange(&g_devFlicker, 0, 0)) {
                // ~15 frames per phase: 4 Hz at 120 fps, which is squarely in the band the eye
                // picks up involuntarily. Much faster reads as shimmer, much slower stops being
                // motion and goes back to being an absence.
                static int fc = 0;
                const int phase = ((++fc / 15) & 1);
                if (phase != g_devPhase) { g_devPhase = phase; g_devPhaseAge = 0; }
                else ++g_devPhaseAge;
                if (g_devPhase == 0) stepIdx = 0;
            } else {
                g_devPhase = 1;
                g_devPhaseAge = 999;
            }
            const int32_t maxDev = (int32_t)(kDevSteps[stepIdx] * (65536.0f / 360.0f));
            // The view is always the head. The engine is pulled towards its target - the hand when
            // decoupling, the body when only the run-64 split is being exercised - as far as the
            // deviation clamp allows, and the matrix supplies the remainder. At maxDev = 0 the
            // matrix rotation is identically zero and the whole thing collapses to today's
            // behaviour, which keeps 0 a genuine control rather than a nominal one.
            g_matDesYawUU   = g_wantYaw;
            g_matDesPitchUU = g_wantPitch;
            const int32_t yawTarget   = aimOn ? engineYawTarget   : g_baseYaw;
            const int32_t pitchTarget = aimOn ? enginePitchTarget : 0;
            g_matCurYawUU   = g_wantYaw   + ClampDev(yawTarget   - g_wantYaw,   maxDev);
            g_matCurPitchUU = g_wantPitch + ClampDev(pitchTarget - g_wantPitch, maxDev);
            if (InterlockedCompareExchange(&g_matrixRot, 0, 0)) {
                g_wantYaw   = g_matCurYawUU;
                g_wantPitch = g_matCurPitchUU;
            }
        }

        // Roll rides with head tracking, so it stops when F9 does and the matrix path has one
        // flag to read rather than two. Written unconditionally when off so a stale angle can
        // never survive a toggle.
        g_wantRollRad = (InterlockedCompareExchange(&g_rollOn, 0, 0) &&
                         InterlockedCompareExchange(&g_enabled, 0, 0))
                        ? g_rollSign * rollRad : 0.0f;
        const float rollDeg = fabsf(g_wantRollRad) * 57.2957795f;
        if (rollDeg > g_maxRollDeg) g_maxRollDeg = rollDeg;

        // ---- ⭐ run 136: freeze the view, so an A/B can be taken from ONE camera ----
        //
        // draws/frame moves more with a few degrees of head rotation than with the effect being
        // measured, which made "stand in exactly the same spot facing exactly the same way across
        // two launches" the limiting factor of the whole test - and an unreasonable thing to ask
        // of someone wearing a headset.
        //
        // Freezing pins the view to the ENGINE's own camera: no head rotation, no roll, no 6-DOF
        // offset. That is deliberately the game's unmodified viewpoint rather than "wherever the
        // head was when the key was pressed", because the game's camera comes from the save and is
        // therefore reproducible across launches, while a head pose is not.
        //
        // ⚠️ g_eyeDeltaUU is NOT frozen. It is the interpupillary separation, not head movement,
        // and zeroing it would collapse the stereo pair - which is one of the things under test.
        if (InterlockedCompareExchange(&g_freezeView, 0, 0)) {
            g_wantYaw     = g_frozenYaw;
            g_wantPitch   = g_frozenPitch;
            g_wantRollRad = 0.0f;
            g_hmdOffset[0] = g_hmdOffset[1] = g_hmdOffset[2] = 0.0f;
        }

        if (++g_tick % 180 == 0)
            Log("head pitch %.1f yaw %.1f roll %.1f -> want pitch %d yaw %d roll %.1f deg (hook hits %d)",
                pitchRad*57.2958f, yawRad*57.2958f, rollRad*57.2958f,
                g_wantPitch, g_wantYaw, g_wantRollRad*57.2958f, g_hookHits);
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
    // The submission itself. For VDXR this is where the composed frame is handed to the runtime to
    // encode and stream, so a congested link or a backed-up compositor shows up HERE - and until
    // now it was pooled into "our work" alongside the game's own rendering.
    LARGE_INTEGER tXrEnd = Now();
    xrEndFrame(g_xrSession, &fei);
    const double msEnd = MsSince(tXrEnd);
    g_msXrSubmit += msEnd;
    // Peak, not just the mean: a one-second average buries a single 200 ms submission among a
    // hundred fast ones, and one stall that long IS a visible dip.
    if (msEnd > g_msXrSubmitPeak) g_msXrSubmitPeak = msEnd;
    ++g_xrSubmitFrames;
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

    // ---- run 43: remap a SECOND view-projection register (ExtraCamReg) ----
    //
    // Everything below keys off g_lockedReg, ONE register, so exactly one matrix is ever remapped -
    // "camera matrix registers tracked: c0 (peak 1)" is 1 by construction, not by measurement.
    // The run-42 dump found a second view-projection at vs c13: identical projection columns to
    // c0-c3, but a world-space translation (c16 = 5179, -1738, 26024) where c3 is translated-world
    // and small. Geometry riding it is never remapped, keeps full-frame coordinates, and its
    // shadow-pass output lands split across the seam.
    //
    // This caches that register's raw uploads into a spare g_camMats slot. Nothing else is needed:
    // StereoPair already loops over every valid slot applying ApplyOffset + ApplyEyeRemap and
    // restores the originals afterwards, so one cached slot is the whole fix.
    //
    // Deliberately NOT running it through ApplyProjection. The forced VR frustum belongs to the
    // matrix we identified and verified; this one is only inferred to be a camera matrix from its
    // shape, and forcing a projection into something that turns out to be a light's transform
    // would break far more than it fixed. Eye offset and half-frame remap only - the minimum that
    // could fix the seam split, so a positive result means something specific.
    if (g_extraCamReg >= 0 && dup &&
        startReg <= (UINT)g_extraCamReg && (UINT)g_extraCamReg + 4 <= startReg + vec4Count) {
        const float* src = data + ((UINT)g_extraCamReg - startReg) * 4;
        int slot = -1;
        for (int s = 0; s < g_camMatUsed; ++s)
            if (g_camMats[s].reg == (UINT)g_extraCamReg) { slot = s; break; }
        if (slot < 0 && g_camMatUsed < kMaxCamMats) slot = g_camMatUsed++;
        if (slot >= 0) {
            g_camMats[slot].reg   = (UINT)g_extraCamReg;
            g_camMats[slot].conv  = g_lockedConv;   // same storage convention as the tracked one
            g_camMats[slot].valid = true;
            memcpy(g_camMats[slot].m, src, 16 * sizeof(float));
        }
    }

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
    // Roll has to be able to pull us onto the modify path by itself. With F9 alone - no 6-DOF, no
    // duplication, no forced projection - `inject` is false and none of the others are set, so
    // without this a head tilt would silently do nothing in plain mono head tracking.
    const bool rollNow = fabsf(g_wantRollRad) > 1e-5f;
    const bool modify = inject || forceProj || dup || rollNow;

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
        // Before ApplyOffset, deliberately - see the order note above ApplyHeadRotation. Gated on
        // head tracking as well as the split, so turning tracking off cannot leave the matrix
        // rotating by a stale angle the engine no longer knows about.
        if (InterlockedCompareExchange(&g_matrixRot, 0, 0) &&
            InterlockedCompareExchange(&g_enabled, 0, 0)) {
            // Once per frame, and here rather than in Present: by this point the engine's camera
            // update has run, so this is the rotation THIS frame is being drawn with. Reading it
            // in Present would be a frame stale, which is the same class of error we are chasing.
            if (InterlockedExchange(&g_camRotDirty, 0) &&
                g_camera && Readable((void*)(g_camera + ACTOR_ROTATION), 12)) {
                const int32_t* rr = reinterpret_cast<const int32_t*>(g_camera + ACTOR_ROTATION);
                g_camPitchUU = rr[0];
                g_camYawUU   = rr[1];
                g_camRotKnown = true;
                int32_t d = g_camYawUU - g_matCurYawUU;
                while (d >  32768) d -= 65536;
                while (d < -32768) d += 65536;
                const double deg = fabs(d) * (360.0 / 65536.0);
                g_residSum += deg;
                if (deg > g_residMax) g_residMax = deg;
                ++g_residN;
            }
            const float kUUToRad = 6.2831853f / 65536.0f;
            // Compensate against where the camera REALLY is. Falling back to what we asked for
            // only while the camera has never been read, which is the first frame or two.
            const float curPitch = (g_camRotKnown ? g_camPitchUU : g_matCurPitchUU) * kUUToRad;
            const float curYaw   = (g_camRotKnown ? g_camYawUU   : g_matCurYawUU)   * kUUToRad;
            ApplyHeadRotation(m, c, curPitch, curYaw,
                              g_matDesPitchUU * kUUToRad, g_matDesYawUU * kUUToRad);
        }
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
        // Roll goes here and nowhere else: after the projection has been forced, so the tangents
        // below describe the frustum the eye really sees (under duplication g_targetTanX is
        // already the half-width per-eye value), and before StereoPair's ApplyEyeRemap squashes
        // clip x. Measured from the matrix rather than taken from g_targetTan*, so it stays
        // correct on the F6-off path too, where the engine's own projection is still in there.
        if (rollNow) {
            float rtx = 0.0f, rty = 0.0f;
            ProjTangents(m, c, &rtx, &rty);
            ApplyRoll(m, c, g_wantRollRad, rtx, rty);
            ++g_rollApplied;
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
// NUMPAD0 - drop draws to the scene-sized non-colour target, to prove whether it is the decal pass.
volatile LONG g_skipExtraTarget = 0;
int g_drawsSkipped = 0;

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

// Defined with the foreground-pass probe further down; this hook records into it.
static void DpgRecord(uint8_t kind, float z0, float z1, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

HRESULT STDMETHODCALLTYPE Hook_SetRenderTarget(IDirect3DDevice9* dev, DWORD idx,
                                              IDirect3DSurface9* surf) {
    HRESULT hr = g_origSetRenderTarget(dev, idx, surf);
    // Run 116: which target the first-person pass draws into. If it switches at the foreground
    // boundary the pass is composited from its own surface, and skipping its draws leaves that
    // surface at its cleared value - which would explain "black but still occluding" without any
    // second copy of the geometry existing.
    if (idx == 0) DpgRecord(3, 0.0f, 0.0f, (uint32_t)(uintptr_t)surf, 0, 0, 0);
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
        // ---- âš ï¸ this label used to lie, and it lied about the exact surface under suspicion ----
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
        // ---- âš ï¸ ScreenPositionScaleBias is NOT the bug here, worked through in run 42 ----
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
    // ---- NUMPAD0: drop every draw aimed at the scene-sized NON-colour target (run 44) ----
    //
    // The artefact is decals, not shadows - it vanishes with "High Quality Decals" off. What is
    // still unproven is whether the G16R16 scene-sized surface IS that decal pass. The census can
    // only show correlation; this shows causation. Skip those draws entirely and look:
    //
    //   artefact GONE      -> G16R16 is the decal pass, and that is where to patch
    //   artefact REMAINS   -> decals are ordinary scene draws and this surface is something else
    //
    // Either answer redirects the work, and it costs one keypress rather than a run each way.
    if (InterlockedCompareExchange(&g_skipExtraTarget, 0, 0) &&
        InterlockedCompareExchange(&g_rtIsScene, 0, 0) == 0 &&
        g_rtCurrent >= 0 && g_rtCurrent < 16 &&
        g_rtSeen[g_rtCurrent].w == g_srcW && g_rtSeen[g_rtCurrent].h == g_srcH) {
        ++g_drawsSkipped;
        return S_OK;
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
            if (g_thisDrawMoved) {
                // Pivot on the camera: the gun hangs off the view, so turning it about the eye is
                // what makes it point where the controller points. Pivoting on the world origin
                // would fling it across the level.
                // ---- ⚠️ run 122: the pivot is the ORIGIN, not the camera position ----
                //
                // The gun vanished, and ENGINE_NOTES already said why: UE3 of this vintage uploads
                // a TRANSLATED-WORLD to clip matrix, pre-subtracting the view origin on the CPU to
                // protect float precision at large world coordinates. In that space THE CAMERA IS
                // AT THE ORIGIN, so pivoting about g_camPos - about (18993, -22941, 180) - shifted
                // the gun twenty thousand units and out of the world.
                //
                // The fixed +30 test worked because a pure translation is identical in both
                // spaces. The moment a pivot was introduced the space started to matter, and this
                // file has recorded which space it is since the matrix scan probed
                // "origin(translated-world)" as a candidate point.
                float C[4][4]; BuildGunC(C);
                ApplyWorldMatrix(reinterpret_cast<Reg4*>(mat), cs.conv, C);
            }
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
            if (g_thisDrawMoved) {
                // Pivot on the camera: the gun hangs off the view, so turning it about the eye is
                // what makes it point where the controller points. Pivoting on the world origin
                // would fling it across the level.
                float C[4][4]; BuildGunC(C);
                ApplyWorldMatrix(reinterpret_cast<Reg4*>(mat), cs.conv, C);
            }
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

// Defined with the foreground-pass probe below; the draw hooks need it first.
HWND g_gameWindow = nullptr;
extern volatile LONG g_frameDrawIdx;
extern volatile LONG g_inForeground, g_hideMeshIdx, g_fgDrawCount;
static bool FgHidden(UINT verts);
static void DpgRecord(uint8_t kind, float z0, float z1, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

HRESULT STDMETHODCALLTYPE Hook_DrawPrim(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t,
                                        UINT start, UINT count) {
    InterlockedIncrement(&g_frameDrawIdx);
    if (InterlockedCompareExchange(&g_inForeground, 0, 0)) {
        const LONG k = InterlockedIncrement(&g_fgDrawCount);
        DpgRecord(2, 0.0f, 0.0f, (uint32_t)k, 0, count, (uint32_t)t);
    }
    return StereoPair(dev, [&] { return g_origDrawPrim(dev, t, start, count); });
}
// ================= ⭐ run 114: find the FOREGROUND pass =======================================
//
// The first-person weapon and arms are drawn in their own depth-priority group - a pass rendered
// after the main scene so the gun never clips into walls. `GetForegroundDPGOverrides` in the run-95
// census confirms this build has one. That means the geometry is ALREADY separated by the engine
// and we do not have to invent a way to tell it apart; we only have to find the boundary.
//
// Two independent signals, because either one alone could be absent:
//
//   * SetViewport with a changed MinZ/MaxZ - the classic depth-range compression for a foreground
//     group, so the gun occupies the near slice of the depth buffer.
//   * a mid-frame Clear of the DEPTH buffer only - the other classic separator.
//
// If either brackets a contiguous run of draws near the end of the frame, that run is the arms and
// the weapon.
//
// ⚠️ NOTHING IS LOGGED FROM INSIDE THESE HOOKS. Run 24/25 lost two runs to a startup hang that was
// a fopen/fwrite/fclose inside a draw hook, and the note on that sits a few lines below. Events go
// into a fixed array and Present prints them.
struct DpgEvent { uint8_t kind; uint32_t drawIdx; float z0, z1; uint32_t a, b, c, d; };
const int kDpgMax = 64;
DpgEvent g_dpgEvents[kDpgMax];
volatile LONG g_dpgCount = 0;
volatile LONG g_dpgProbeFrames = 0;
volatile LONG g_frameDrawIdx = 0;
void DumpDpgProbe();

// ---- ⭐ run 115: the foreground pass, identified and skippable ----
//
// Stage 1 found the boundary: a depth+stencil Clear late in the frame, with six draws after it.
// The first ZBUFFER clear of a frame happens at draw 0 and is the normal scene setup, so "a
// ZBUFFER clear once some draws have already happened" separates the two without needing to know
// the frame's total in advance.
//
// The threshold is deliberately loose. If a shadow or reflection pass also clears depth mid-frame
// this will trip early and the log will show it as a foreground run of hundreds of draws rather
// than six - which is a visible, diagnosable wrong answer instead of a silently wrong one.
volatile LONG g_inForeground = 0;      // set by the late depth clear, reset each Present
// ---- run 117: hide ONE mesh at a time, everywhere it is drawn ----
//
// Both explanations for "black but still occluding" turned out to be true. The foreground pass
// renders into its own target (0x2B480760) and is composited, AND two of the three meshes are
// drawn again at draws 2-3 under SetViewport MinZ 0 MaxZ 0 - a depth-prime at the near plane, the
// standard trick that stops a first-person weapon clipping into walls.
//
// So moving the gun means transforming BOTH copies, and the first thing needed is to know which of
// the three meshes IS the gun. Hiding one at a time by vertex count answers that in one run, and
// hiding it EVERYWHERE - prime pass included - doubles as the proof that we can reach every draw
// of it. If a mesh vanishes completely and you can see through where it was, nothing of it is
// left unaccounted for.
volatile LONG g_hideMeshIdx = 0;       // 0 none, 1..3 index into the learned signatures

// ---- ⭐ run 119: MOVE the selected mesh instead of hiding it ----
//
// Stage 3 is answered: [1]=5799 is the gun, [2]=3314 is the arms, and they are separable. Hiding
// leaves black because the foreground pass draws into its own target and the composite is masked,
// so removing geometry leaves the mask with nothing to show. That is an artefact of HIDING and it
// does not matter, because the goal was never to hide the gun - it is to move it, and moved
// geometry is still drawn.
//
// This is the mechanism test, deliberately crude: shift the selected mesh a fixed 30 world units
// (about 57 cm) so a success is unmistakable and no controller maths can be blamed for a failure.
// The controller pose replaces the constant only once this is proven.
volatile LONG g_fgMoveOn = 0;          // APPS selects a mesh; this says move it rather than hide it
bool g_thisDrawMoved = false;          // set per draw on the render thread, read inside StereoPair
volatile LONG g_fgDrawCount = 0;       // how many draws landed in the pass this frame

// ---- run 116: are these meshes drawn ANYWHERE ELSE in the frame? ----
//
// Hiding the six foreground draws left the gun black and still occluding. If the same meshes are
// also drawn earlier - a depth or base pass in the main scene - that dark silhouette is what
// remains, and moving only the foreground copy would leave a gun-shaped hole behind.
//
// Vertex count is a good enough fingerprint: 5799, 3314 and 390 are specific numbers, and a draw
// matching one of them anywhere in the frame is almost certainly the same mesh. Learned from the
// foreground pass rather than hardcoded, so it survives a different weapon.
// ---- ⚠️ run 118: the signature table has to be PER FRAME ----
//
// It used to accumulate for the whole session and cap at four, so it filled with counts from menu
// and loading frames - 11387, 3314, 390, 9 - before the gun mesh was ever seen, and the hide
// indices then pointed at things that were not the gun. The report that came back ("presses 1, 3
// and 4 look normal") was accurate; the labels on it were fiction.
//
// Rebuilt every frame from the current foreground pass, so the list is what is actually on screen
// now. Eight slots rather than four, because truncation is what caused this and a table that
// silently drops the entry you need is the same defect as a filter that encodes its answer.
const int kFgSigMax = 8;
volatile LONG g_fgSigVerts[kFgSigMax] = {};
volatile LONG g_fgSigCount = 0;

// ---- 💥 run 120: the per-frame reset broke hiding, and the earlier run was right ----
//
// Hiding the arms used to remove them CLEANLY. After the pollution fix it left them black, and the
// regression is mine: the depth-prime copy is drawn at draws 2-3, at the very START of the frame.
// With a session-wide table those draws matched and the prime was hidden too. With a table reset
// every frame the list is empty when they arrive, so only the foreground copy is hidden and the
// prime survives to write the silhouette.
//
// Both properties are wanted and they were never in conflict - I implemented them as if they were.
// So: LEARN into one table, MATCH against a stable copy promoted at the end of each frame. The
// stable copy is populated when draws 2-3 arrive, and it still cannot carry menu geometry into a
// gameplay frame because it is replaced wholesale every frame rather than accumulated.
volatile LONG g_fgStableVerts[kFgSigMax] = {};
volatile LONG g_fgStableCount = 0;

// ---- 💥 run 122: latch the mesh by its VERTEX COUNT, not its position in the list ----
//
// Reported: the gun and arms flash back to normal for a moment when firing. That is the list
// changing - muzzle-flash geometry joins the foreground pass, so [1] stops being the gun for those
// frames and the transform lands on whatever took its place, or on nothing.
//
// An index into a list that is rebuilt every frame was never a stable handle. The vertex count is:
// 5799 identifies that mesh whatever else is on screen. So the counts are LATCHED when a mode is
// selected and matched by value from then on.
volatile LONG g_gunVerts = 0, g_armsVerts = 0;

static void FgLearnSignature(UINT verts) {
    const LONG n = InterlockedCompareExchange(&g_fgSigCount, 0, 0);
    for (LONG i = 0; i < n && i < kFgSigMax; ++i)
        if ((UINT)InterlockedCompareExchange(&g_fgSigVerts[i], 0, 0) == verts) return;
    if (n >= kFgSigMax) return;
    InterlockedExchange(&g_fgSigVerts[n], (LONG)verts);
    InterlockedIncrement(&g_fgSigCount);
}

// True when this mesh is the one currently being hidden, wherever in the frame it is drawn.
// Shift world positions by (tx,ty,tz) inside an already-uploaded view-projection.
//
// ROW: registers are the rows of M and clip = v * M, so translating the world by T adds
//      T.x*row0 + T.y*row1 + T.z*row2 into row3, the translation row.
// COL: registers are the columns of M and clip.k = dot(reg[k], v), so the same translation adds
//      dot(reg[k].xyz, T) to reg[k].w.
//
// Both are exact - this is a change of the transform, not an approximation of one - and doing it
// AFTER ApplyEyeRemap means the per-eye offset is preserved.
// ---- ⭐ run 121: a full transform, so the gun can be ROTATED and not just shifted ----
//
// Stage 4a proved the mechanism with a fixed translation. Pointing the gun where the controller
// points needs a rotation about a pivot, which a translate-only shortcut cannot express, so the
// transform becomes a real 4x4 composed into the uploaded matrix.
//
// C is row-vector form: v' = v * C. Rotating about pivot p by R is T(-p) * R * T(p), which in
// row form is the 3x3 R in the top-left and (p - p*R) as the bottom row.
//
// ROW  registers are rows of M and clip = v*M, so M' = C*M: row i of M' is sum_k C[i][k]*row_k(M)
// COL  registers give clip.k = dot(reg[k], v), so reg'[k].i = dot(reg[k], row_i(C))
//
// Both are exact. Applied AFTER ApplyEyeRemap so the per-eye offset survives.
static void ApplyWorldMatrix(Reg4* m, int conv, const float C[4][4]) {
    Reg4 out[4];
    if (conv == CONV_ROW) {
        for (int i = 0; i < 4; ++i) {
            out[i].x = C[i][0]*m[0].x + C[i][1]*m[1].x + C[i][2]*m[2].x + C[i][3]*m[3].x;
            out[i].y = C[i][0]*m[0].y + C[i][1]*m[1].y + C[i][2]*m[2].y + C[i][3]*m[3].y;
            out[i].z = C[i][0]*m[0].z + C[i][1]*m[1].z + C[i][2]*m[2].z + C[i][3]*m[3].z;
            out[i].w = C[i][0]*m[0].w + C[i][1]*m[1].w + C[i][2]*m[2].w + C[i][3]*m[3].w;
        }
    } else {
        for (int k = 0; k < 4; ++k) {
            const float v[4] = { m[k].x, m[k].y, m[k].z, m[k].w };
            out[k].x = v[0]*C[0][0] + v[1]*C[0][1] + v[2]*C[0][2] + v[3]*C[0][3];
            out[k].y = v[0]*C[1][0] + v[1]*C[1][1] + v[2]*C[1][2] + v[3]*C[1][3];
            out[k].z = v[0]*C[2][0] + v[1]*C[2][1] + v[2]*C[2][2] + v[3]*C[2][3];
            out[k].w = v[0]*C[3][0] + v[1]*C[3][1] + v[2]*C[3][2] + v[3]*C[3][3];
        }
    }
    for (int i = 0; i < 4; ++i) m[i] = out[i];
}

// Rotate about `p` by yaw then pitch, both in engine rotation units. UE3 is X forward, Y right,
// Z up: yaw turns about Z, pitch about Y.
// ---- ⭐ run 123: all six degrees of freedom ----
//
// Reported: the gun tracks left, right, up and down, but not forward, back or twist. Both gaps are
// the same omission - only yaw and pitch were ever computed, so the transform could do nothing but
// swing the gun around a sphere centred on the eye. Rotation alone has no forward component and no
// roll, which is exactly the three axes named.
//
// So: roll joins the rotation, and the hand's POSITION relative to the head joins as a translation.
// Composed as R = Rx * Ry * Rz in row-vector form, i.e. roll applied first in the gun's own frame,
// which is what makes twisting the controller twist the barrel rather than swing the muzzle.
//
// Signs are ini-overridable. Up/down came back reversed, and rather than negate it in place - which
// is how a convention gets buried - it goes through the same kind of key the trace rotation uses.
// ---- ⭐ run 124: the pivot must be the GUN, not the eye ----
//
// Reported: rotating right sends the gun right and then DOWN-right as the rotation grows, and the
// centre of rotation is not where the handle is. That is the signature of rotating about the wrong
// point: we pivot at the origin, which in translated-world space is the EYE, so the gun swings
// through an arc half a metre long instead of turning in place.
//
// The correct transform is v' = (v - G) * R + G + (H - G), where G is the gun's own anchor and H
// is the hand. G is not knowable from here, so it is an ini offset in the head's own frame -
// forward, right, up in engine units - defaulting to roughly where a held pistol sits.
LONG g_gunSignYaw = 1, g_gunSignPitch = -1, g_gunSignRoll = 1, g_gunFollowPos = 1;
LONG g_gunSignPosZ = -1;                       // vertical translation came back flipped too
LONG g_gunAnchorFwd = 25, g_gunAnchorRight = 8, g_gunAnchorUp = -8;   // engine units from the eye
volatile LONG g_gunSignCombo = 0;
volatile LONG g_readoutOn = 1;   // on-screen state squares - see DrawStateReadout

// ---- run 128: which anchor axis the arrow keys adjust ----
//
// The anchor is the point the gun rotates about, and getting it onto the grip is pure eyeball -
// exactly the kind of tuning that should never cost a restart per guess. LEFT/RIGHT pick the axis,
// UP/DOWN change it, and all three values plus the selection are on screen.
volatile LONG g_anchorAxis = 0;                // 0 forward, 1 right, 2 up

static void Mul3(float out[3][3], const float a[3][3], const float b[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i][j] = a[i][0]*b[0][j] + a[i][1]*b[1][j] + a[i][2]*b[2][j];
}

// The eight rotation-sign combinations, cycled by CAPS LOCK so the right one is found in ONE run
// instead of one restart per guess. Three axes reported wrong in three separate runs is what this
// is for - a sign is not worth a headset session each.
// ---- ⚠️ run 126: bit 3 turns the view-frame conjugation OFF ----
//
// Run 125 changed the frame AND baked a new pitch sign in the same build, so the result could not
// say which caused what - the one-factor-at-a-time rule, broken by me after this project had
// already recorded it twice. It also renumbered the combos, so "combo 2 was best" stopped being
// reproducible between the two runs.
//
// So the frame change joins the cycle as a fourth bit instead of being a decision already taken.
// Sixteen states on one key, and the log names each one, so a single session settles both
// questions - which signs, and whether the conjugation helps at all.
static bool GunViewFrame() { return (InterlockedCompareExchange(&g_gunSignCombo, 0, 0) & 8) == 0; }

static void GunSigns(int& sy, int& sp, int& sr) {
    const LONG c = InterlockedCompareExchange(&g_gunSignCombo, 0, 0);
    sy = (c & 1) ? -(int)g_gunSignYaw   : (int)g_gunSignYaw;
    sp = (c & 2) ? -(int)g_gunSignPitch : (int)g_gunSignPitch;
    sr = (c & 4) ? -(int)g_gunSignRoll  : (int)g_gunSignRoll;
}

// ---- ⭐ run 125: pitch and roll are about the VIEW's axes, not the world's ----
//
// Reported: pointing straight ahead looks right, but spinning the character with the stick makes
// the gun droop as it turns and come back correct after a full circle. A 360-degree period is the
// signature of a frame error, and it names itself: pitch was applied about world Y, when it needs
// to be about the VIEW'S RIGHT axis. Facing the start direction those coincide; ninety degrees
// round, world Y has become the view's FORWARD, so what should be pitch acts partly as roll.
//
// Yaw was unaffected because yaw is about world Z in either frame - which is exactly why only
// pitch misbehaved, and why the report distinguishes them.
//
// So the pitch/roll pair is conjugated by the view yaw: R = Rz(-t) * Rx * Ry * Rz(t + yaw). The
// conjugation takes the gun into the view's frame, rotates it there, and brings it back.
static void BuildGunTransform(float C[4][4], const float p[3],
                              int32_t yawUU, int32_t pitchUU, int32_t rollUU,
                              int32_t viewYawUU,
                              float tx, float ty, float tz) {
    const float kUUToRad = 6.2831853f / 65536.0f;
    const float a = yawUU   * kUUToRad;      // about Z, UE3 up - frame-independent
    const float b = pitchUU * kUUToRad;      // about the view's right
    const float c = rollUU  * kUUToRad;      // about the view's forward
    const float t = viewYawUU * kUUToRad;
    const float Rz[3][3] = { { cosf(a + t), sinf(a + t), 0 }, { -sinf(a + t), cosf(a + t), 0 }, { 0, 0, 1 } };
    const float Rzi[3][3] = { { cosf(-t), sinf(-t), 0 }, { -sinf(-t), cosf(-t), 0 }, { 0, 0, 1 } };
    const float Ry[3][3] = { { cosf(b), 0, -sinf(b) }, { 0, 1, 0 }, { sinf(b), 0, cosf(b) } };
    const float Rx[3][3] = { { 1, 0, 0 }, { 0, cosf(c), sinf(c) }, { 0, -sinf(c), cosf(c) } };
    float t1[3][3], t2[3][3], R[3][3];
    Mul3(t1, Rzi, Rx);
    Mul3(t2, t1, Ry);
    Mul3(R, t2, Rz);

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) C[i][j] = R[i][j];
        C[i][3] = 0.0f;
    }
    // bottom row = p - p*R + T. With p at the origin - which is where the camera sits in
    // translated-world space - this reduces to the translation, but the pivot stays in the
    // expression so a future anchor offset does not need the maths rederived.
    C[3][0] = p[0] - (p[0]*R[0][0] + p[1]*R[1][0] + p[2]*R[2][0]) + tx;
    C[3][1] = p[1] - (p[0]*R[0][1] + p[1]*R[1][1] + p[2]*R[2][1]) + ty;
    C[3][2] = p[2] - (p[0]*R[0][2] + p[1]*R[1][2] + p[2]*R[2][2]) + tz;
    C[3][3] = 1.0f;
}

// Build the gun transform: rotate about the gun's OWN anchor, then translate that anchor to the
// hand. v' = (v - G) * R + G + (H - G), which BuildGunTransform expresses as pivot G plus
// translation (H - G).
//
// G is the anchor in the head's frame (forward, right, up) turned into world axes by the body yaw,
// the same conversion the hand offset goes through - so both live in the same space and the
// subtraction below is meaningful rather than two conventions differenced by luck.
// ---- 💥 run 131: the anchor belongs in the VIEW's frame, not the BODY's ----
//
// Reported: moving your head moves the gun and the arms but leaves the anchor behind, so tuning it
// meant holding your head rigid from the moment the level loaded.
//
// That is a real defect. G was built from g_baseYaw - the BODY yaw - and had no pitch term at all,
// while the engine draws the gun attached to the VIEW. Turn your head and the gun moves and the
// anchor does not; look up or down and they separate completely, since body yaw cannot express
// pitch at all.
//
// So the offset is now rotated by the view's full orientation - yaw AND pitch - which is also the
// more intuitive thing to tune against: "forward from where I am looking".
static void GunAnchorWorld(float G[3]) {
    const float kUUToRad = 6.2831853f / 65536.0f;
    const float y = g_wantYaw   * kUUToRad;
    const float p = g_wantPitch * kUUToRad;
    const float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p);
    // UE3 basis for a (pitch, yaw) orientation: X forward, Y right, Z up.
    const float F[3] = {  cp * cy,  cp * sy, sp };
    const float R[3] = { -sy,       cy,      0.0f };
    const float U[3] = { -sp * cy, -sp * sy, cp };
    const float af = (float)g_gunAnchorFwd, ar = (float)g_gunAnchorRight, au = (float)g_gunAnchorUp;
    for (int i = 0; i < 3; ++i) G[i] = af * F[i] + ar * R[i] + au * U[i];
}

static void BuildGunC(float C[4][4]) {
    float G[3]; GunAnchorWorld(G);

    float H[3] = { G[0], G[1], G[2] };          // no position following -> translation cancels
    if (g_gunFollowPos) {
        H[0] = (float)InterlockedCompareExchange(&g_handOffX, 0, 0);
        H[1] = (float)InterlockedCompareExchange(&g_handOffY, 0, 0);
        H[2] = (float)(g_gunSignPosZ * InterlockedCompareExchange(&g_handOffZ, 0, 0));
    }
    int sy, sp, sr; GunSigns(sy, sp, sr);
    BuildGunTransform(C, G,
        (int32_t)(sy * InterlockedCompareExchange(&g_handDevYawUU, 0, 0)),
        (int32_t)(sp * InterlockedCompareExchange(&g_handDevPitchUU, 0, 0)),
        (int32_t)(sr * InterlockedCompareExchange(&g_handDevRollUU, 0, 0)),
        // The gun faces along the view, so the view yaw is the frame its pitch and roll belong in -
        // unless the cycle has it switched off, which is what makes run 125 testable rather than
        // assumed.
        GunViewFrame() ? g_wantYaw : 0,
        H[0] - G[0], H[1] - G[1], H[2] - G[2]);
}

static void ApplyWorldOffset(Reg4* m, int conv, float tx, float ty, float tz) {
    if (conv == CONV_ROW) {
        m[3].x += tx * m[0].x + ty * m[1].x + tz * m[2].x;
        m[3].y += tx * m[0].y + ty * m[1].y + tz * m[2].y;
        m[3].z += tx * m[0].z + ty * m[1].z + tz * m[2].z;
        m[3].w += tx * m[0].w + ty * m[1].w + tz * m[2].w;
    } else {
        for (int k = 0; k < 4; ++k)
            m[k].w += tx * m[k].x + ty * m[k].y + tz * m[k].z;
    }
}

// ---- run 120: the toggle now cycles the two things actually wanted, not a mesh walker ----
//
//   0  normal
//   1  MOVE the gun 30 world units sideways   - does the transform reach it
//   2  HIDE the arms                          - does skipping remove them cleanly again
//   3  both                                   - the target configuration, in one press
//
// Walking meshes generically was right while we did not know which was which. Now that we do,
// cycling the two operations is fewer presses in a headset and each state answers a real question.
static bool FgIsMesh(UINT verts, int idx1) {          // idx1 is 1-based into the stable list
    if (!verts || idx1 < 1 || idx1 > kFgSigMax) return false;
    if (idx1 > InterlockedCompareExchange(&g_fgStableCount, 0, 0)) return false;
    return (UINT)InterlockedCompareExchange(&g_fgStableVerts[idx1 - 1], 0, 0) == verts;
}

static bool FgHidden(UINT verts) {                    // arms, in modes 2 and 3
    const LONG m = InterlockedCompareExchange(&g_hideMeshIdx, 0, 0);
    const LONG a = InterlockedCompareExchange(&g_armsVerts, 0, 0);
    return (m == 2 || m == 3) && a && (UINT)a == verts;
}

static bool FgMoved(UINT verts) {                     // gun, in modes 1 and 3
    const LONG m = InterlockedCompareExchange(&g_hideMeshIdx, 0, 0);
    const LONG g = InterlockedCompareExchange(&g_gunVerts, 0, 0);
    return (m == 1 || m == 3) && g && (UINT)g == verts;
}

// Any first-person mesh, wherever it is drawn - used to catch the near-plane depth prime at
// draws 2-3 so it is hidden or moved along with the foreground copy.
static bool FgMatchesSignature(UINT verts) {
    if (!verts) return false;
    const LONG n = InterlockedCompareExchange(&g_fgStableCount, 0, 0);
    for (LONG i = 0; i < n && i < kFgSigMax; ++i)
        if ((UINT)InterlockedCompareExchange(&g_fgStableVerts[i], 0, 0) == verts) return true;
    return false;
}

static void DpgRecord(uint8_t kind, float z0, float z1, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (InterlockedCompareExchange(&g_dpgProbeFrames, 0, 0) <= 0) return;
    const LONG i = InterlockedIncrement(&g_dpgCount) - 1;
    if (i < 0 || i >= kDpgMax) return;
    g_dpgEvents[i].kind = kind;
    g_dpgEvents[i].drawIdx = (uint32_t)InterlockedCompareExchange(&g_frameDrawIdx, 0, 0);
    g_dpgEvents[i].z0 = z0; g_dpgEvents[i].z1 = z1;
    g_dpgEvents[i].a = a; g_dpgEvents[i].b = b; g_dpgEvents[i].c = c; g_dpgEvents[i].d = d;
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_SetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Clear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD,
                                               D3DCOLOR, float, DWORD);
PFN_SetViewport g_origSetViewport = nullptr;
PFN_Clear       g_origClear = nullptr;

HRESULT STDMETHODCALLTYPE Hook_SetViewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp) {
    if (vp) DpgRecord(0, vp->MinZ, vp->MaxZ, vp->X, vp->Y, vp->Width, vp->Height);
    return g_origSetViewport(dev, vp);
}

HRESULT STDMETHODCALLTYPE Hook_Clear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects,
                                     DWORD flags, D3DCOLOR colour, float z, DWORD stencil) {
    DpgRecord(1, z, 0.0f, flags, count, 0, 0);
    // The late depth clear opens the first-person pass. Draw 50 is well past the scene setup
    // clear at draw 0 and far short of the ~1088 where the real boundary sits, so it separates
    // them without assuming this frame's draw total.
    if ((flags & D3DCLEAR_ZBUFFER) && InterlockedCompareExchange(&g_frameDrawIdx, 0, 0) > 50)
        InterlockedExchange(&g_inForeground, 1);
    return g_origClear(dev, count, rects, flags, colour, z, stencil);
}

void DumpDpgProbe() {
    const LONG n = InterlockedCompareExchange(&g_dpgCount, 0, 0);
    const LONG draws = InterlockedCompareExchange(&g_frameDrawIdx, 0, 0);
    Log("--- foreground-pass probe: %ld draws this frame, %ld event(s) ---", draws, n);
    for (LONG i = 0; i < n && i < kDpgMax; ++i) {
        const DpgEvent& e = g_dpgEvents[i];
        if (e.kind == 0)
            Log("    after draw %-5u  SetViewport  MinZ %.3f MaxZ %.3f  rect %u,%u %ux%u",
                e.drawIdx, e.z0, e.z1, e.a, e.b, e.c, e.d);
        else if (e.kind == 4)
            Log("    *** draw %-5u OUTSIDE the foreground pass uses a first-person mesh:"
                " verts %u prims %u", e.drawIdx, e.b, e.c);
        else if (e.kind == 3)
            Log("    after draw %-5u  SetRenderTarget  surface 0x%08X", e.drawIdx, e.a);
        else if (e.kind == 2)
            Log("    FOREGROUND draw #%u (frame draw %u)  verts %u  prims %u  primType %u",
                e.a, e.drawIdx, e.b, e.c, e.d);
        else
            Log("    after draw %-5u  Clear        flags 0x%X%s%s%s  Z %.3f",
                e.drawIdx, e.a,
                (e.a & D3DCLEAR_TARGET)  ? " TARGET"  : "",
                (e.a & D3DCLEAR_ZBUFFER) ? " ZBUFFER" : "",
                (e.a & D3DCLEAR_STENCIL) ? " STENCIL" : "", e.z0);
    }
    {
        const LONG sn = InterlockedCompareExchange(&g_fgStableCount, 0, 0);
        char line[256]; int m = 0;
        for (LONG i = 0; i < sn && i < kFgSigMax; ++i)
            m += _snprintf_s(line + m, sizeof(line) - m, _TRUNCATE, " [%ld]=%ld",
                             i + 1, InterlockedCompareExchange(&g_fgStableVerts[i], 0, 0));
        Log("    meshes this frame (APPS index = vertex count):%s", sn ? line : " (none)");
    }
    Log("    foreground draws this frame: %ld%s", InterlockedCompareExchange(&g_fgDrawCount, 0, 0),
        "");
    Log("    A depth-only Clear or a MinZ/MaxZ change LATE in the frame brackets the first-person"
        " pass. The draws after it are the arms and the weapon.");
}

HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrim(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t,
                                               INT baseVertex, UINT minIndex, UINT numVertices,
                                               UINT startIndex, UINT primCount) {
    InterlockedIncrement(&g_frameDrawIdx);
    // LEARN FIRST, THEN HIDE. If hiding skipped the learning, the hidden mesh would drop out of
    // the list next frame and every index after it would shift - the toggle would walk through a
    // moving target and nothing would be reproducible.
    if (InterlockedCompareExchange(&g_inForeground, 0, 0)) {
        const LONG k = InterlockedIncrement(&g_fgDrawCount);
        FgLearnSignature(numVertices);
        DpgRecord(2, 0.0f, 0.0f, (uint32_t)k, numVertices, primCount, (uint32_t)t);
        if (FgHidden(numVertices)) return D3D_OK;
        // Returning S_OK without drawing is how the mod already drops draws elsewhere - the
        // device state is untouched, so the next draw behaves normally.
    } else if (FgMatchesSignature(numVertices)) {
        // Outside the pass, but the same mesh - the near-plane depth prime at draws 2-3.
        if (FgHidden(numVertices)) return D3D_OK;
        // The same mesh, drawn OUTSIDE the foreground pass. This is the whole question.
        DpgRecord(4, 0.0f, 0.0f, (uint32_t)InterlockedCompareExchange(&g_frameDrawIdx, 0, 0),
                  numVertices, primCount, 0);
    }
    g_thisDrawMoved = FgMoved(numVertices);
    const HRESULT dhr = StereoPair(dev, [&] {
        return g_origDrawIndexedPrim(dev, t, baseVertex, minIndex, numVertices, startIndex, primCount);
    });
    g_thisDrawMoved = false;
    return dhr;
}

// ---- âš ï¸ the run-24/25 startup hang was file I/O in a draw hook ----
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
// (declared up with the Sleep hook, which needs it too)
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
// ---- run 46: tell a fullscreen post-process quad from real geometry ----
//
// Level 3 sends EVERY user-pointer draw through StereoPair, including UE3's DrawDenormalizedQuad -
// the fullscreen quad every post-process pass uses. Scissoring that to a half and remapping the
// matrix underneath it is the "rainbow" of run 28, and it makes the screen unreadable, so the test
// it was meant to run cannot be judged.
//
// The classifier can be crude because the two cases are far apart: a fullscreen quad is TWO
// triangles. Decal geometry, particles and dynamic meshes are all more than that. So draws at or
// below the threshold pass through untouched (correct for a fullscreen quad, which must cover the
// whole target) and everything above gets the per-eye treatment.
//
// Tunable from the ini rather than fixed, because "2" is an assumption about UE3's quad and the
// point of this run is to stop assuming things.
int g_userPtrMinPrims = 3;   // ini UserPtrMinPrims: split only draws with at least this many prims
int g_drawsUPQuad = 0;       // passed through as fullscreen quads, cumulative over the window
int g_drawsUPSplit = 0;      // given the per-eye treatment, same units
int g_upHist[6] = {};        // primitive-count histogram: 1, 2, 3-4, 5-8, 9-16, 17+

// ---- is a geometric classifier even possible? one log line decides (run 47) ----
//
// If primitive count cannot separate decals from the post-process quad, the fallback is to look at
// the vertices. UE3's function is called DrawDenormalizedQuad and draws in SCREEN PIXEL
// coordinates, so a fullscreen quad should read as values like 0 and 4096 while decal geometry
// reads as world coordinates in the thousands-but-arbitrary range. If that holds, the classifier
// is a couple of float compares per draw - microseconds a frame, not a performance question.
//
// The unknown is layout: DrawPrimitiveUP gives a pointer and a stride but not the vertex format.
// Position is conventionally first, so log the first four floats and the stride for a sample of
// draws in each size bucket and read it off, rather than guessing at an offset.
//
// Readable() first - a bad pointer here would fault inside a draw hook, which is the worst place
// in this codebase to take an access violation.
int g_upVtxDumped[6] = {};

inline void DumpUserPtrVertex(UINT primCount, const void* vtxData, UINT stride, const char* which) {
    const int b = primCount <= 1 ? 0 : primCount == 2 ? 1 : primCount <= 4 ? 2
                : primCount <= 8 ? 3 : primCount <= 16 ? 4 : 5;
    if (g_upVtxDumped[b] >= 2 || !vtxData || stride < 8) return;
    if (!Readable(vtxData, 16)) return;
    ++g_upVtxDumped[b];
    const float* v = static_cast<const float*>(vtxData);
    Log("    UP vertex sample [%s] prims=%u stride=%u  v0 = (%.2f, %.2f, %.2f, %.2f)",
        which, primCount, stride, v[0], v[1], v[2], stride >= 16 ? v[3] : 0.0f);
}

// ---- run 47: classify by VERTEX RANGE, not primitive count ----
//
// The histogram separated cleanly - 2 prims (1989) against 9+ (3595), nothing between - yet decals
// stayed broken. The vertex samples say why: the fullscreen quad sits at NDC, v0 = (-0.94, 1.00),
// and a decal is ALSO two triangles. So the 2-prim bucket holds both, and a count-based threshold
// passes the whole bucket through unsplit, decals included. The sampler drew two from that bucket
// and both happened to be the quad, which is how it looked clean.
//
// Coordinate range separates them by four orders of magnitude: +-1 for a clip-space quad against
// +-40000 for world-space geometry. So: a draw is a fullscreen quad only if it is small AND its
// first vertex is inside NDC. Everything else gets the per-eye split.
//
// Cost is a bounds check plus two float compares on memory the game just wrote - microseconds a
// frame at ~370 draws, which is why this was never a performance question.
int g_upQuadByVertex = 1;    // ini UserPtrQuadByVertex: 0 = count only (run 46), 1 = count + NDC

inline bool UserPtrQuad(UINT primCount, const void* vtxData, UINT stride) {
    const int b = primCount <= 1 ? 0 : primCount == 2 ? 1 : primCount <= 4 ? 2
                : primCount <= 8 ? 3 : primCount <= 16 ? 4 : 5;
    ++g_upHist[b];
    bool quad = (int)primCount < g_userPtrMinPrims;
    if (quad && g_upQuadByVertex) {
        // Small AND in clip space. A 2-triangle decal carries world coordinates and fails this,
        // so it goes on to be split like any other geometry.
        quad = false;
        if (vtxData && stride >= 8 && Readable(vtxData, 8)) {
            const float* v = static_cast<const float*>(vtxData);
            if (v[0] > -1.5f && v[0] < 1.5f && v[1] > -1.5f && v[1] < 1.5f) quad = true;
        }
    }
    if (quad) ++g_drawsUPQuad; else ++g_drawsUPSplit;
    return quad;
}

HRESULT STDMETHODCALLTYPE Hook_DrawPrimUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t,
                                          UINT primCount, const void* vtxData, UINT vtxStride) {
    if (!g_origDrawPrimUP) return D3DERR_INVALIDCALL;
    // Touch nothing off the render thread - see the note above OnRenderThread.
    if (UserPtrInert()) return g_origDrawPrimUP(dev, t, primCount, vtxData, vtxStride);
    ++g_drawsUP;
    if (g_userPtrHookLevel == 2) return g_origDrawPrimUP(dev, t, primCount, vtxData, vtxStride);
    DumpUserPtrVertex(primCount, vtxData, vtxStride, "DrawPrimUP");
    if (UserPtrQuad(primCount, vtxData, vtxStride)) { return g_origDrawPrimUP(dev, t, primCount, vtxData, vtxStride); }
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
    DumpUserPtrVertex(primCount, vtxData, vtxStride, "DrawIndexedPrimUP");
    if (UserPtrQuad(primCount, vtxData, vtxStride)) {
        return g_origDrawIndexedPrimUP(dev, t, minVtxIndex, numVertices, primCount,
                                       idxData, idxFormat, vtxData, vtxStride);
    }
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
// ---- âš ï¸ mode 2 (refuse to create) CRASHES this build - run 30 ----
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
// ---- âœ… CONFIRMED run 30: this was the flicker, after seven eliminated mechanisms ----
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

// ---- âš ï¸ the known gap, instrumented rather than hoped about ----
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
        Log("    head roll: %s, peak %.1f deg this window, %d draw matrix(es) rolled%s",
            InterlockedCompareExchange(&g_rollOn, 0, 0) ? "ON" : "OFF",
            g_maxRollDeg, g_rollApplied,
            (g_maxRollDeg > 1.0f && g_rollApplied == 0)
                ? "  <-- TILTED BUT NOTHING ROLLED - the matrix path is not being reached" : "");
        g_maxRollDeg = 0.0f; g_rollApplied = 0;
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

// ================================================================ UObject::ProcessEvent (run 81)
//
// Found at last, and by a route the three earlier passes did not have. FindScriptVM walked up from
// FFrame::Step and dead-ended because Step is 37 bytes and inlined everywhere. FindProcessEvent
// chased a UTF-16 "recursion" string that turned out to be a Raven OOM reporter. The anchor that
// worked was sitting in plain ASCII the whole time:
//
//   "Error: CallFunction - '%s' is not a function"   -> 0x012A72C0, a 15,209-byte function
//
// That size is itself the finding. UObject::CallFunction is a few hundred bytes in UE3, so this is
// the bytecode INTERPRETER with CallFunction inlined into it - which is exactly what ENGINE_NOTES
// deduced from the other end when it found opcodes 0x00-0x35 all pointing at execUndefined in
// GNatives while the game plainly ran. Both halves of that puzzle now agree.
//
// One hop up its call graph: four callers, and 0x01308A10 carries
// "Error: Stack overflow, max level of 255 nested calls is reached" - UE3's script-call recursion
// guard. It also has ZERO direct callers, which is not suspicious but confirming: ProcessEvent is
// virtual and reached through the UObject vtable, so a static call graph cannot see its callers.
//
// ⚠️ The decompiler reported TWO parameters. It is wrong, and taking its word for it would have
// corrupted the stack on the first call: the epilogue is `ret 0xC`, so there are THREE stack args
// plus `this` in ECX, and the prologue's `mov esi, ecx` confirms __thiscall. That is
// `ProcessEvent(UFunction*, void* Parms, void* Result)` exactly. **Read the epilogue, not the
// decompiler's guess** - this is the third time in this project that reading a signature has been
// less reliable than reading the machine.
const uintptr_t kProcessEventAddr = 0x01308A10;

// ---- 💥 run 94: THE SIGNATURE IS TWO STACK ARGS, AND GETTING IT WRONG CRASHES INSTANTLY ----
//
// 0x01308A10 ends in `ret 0xC` - three stack args - and this hook was written for it. The real
// ProcessInternal at 0x004B7E40 ends in `ret 8`: TWO. Calling the trampoline with three pushed
// twelve bytes while the callee popped eight, leaking four bytes of stack on the hottest function
// in the process. It crashed within milliseconds, twice, deterministically.
//
// So the signature now matches the function that is actually hooked:
//
//     UObject::ProcessInternal(FFrame& Stack, RESULT_DECL)    __thiscall
//         ECX      -> self      the UObject the script call is on
//         stack[0] -> Stack     the FFrame: Node names the function, Locals holds the parameters
//         stack[1] -> Result    the return-value buffer
//
// Verified from the machine, not from a decompiler: the prologue is
// 55 8B EC 83 EC 54 A1 <cookie> ... 8B 45 0C (arg2) ... 8B 75 08 (arg1) ... 8B F9 (this from ECX),
// and the epilogue is `ret 8`. **Read the epilogue** - that is now the fourth time in this project
// that reading the machine beat reading a signature, and the first time not doing so cost a crash
// rather than a wasted run.
typedef void (__fastcall *PFN_ProcessEvent)(void* self, void* edx, void* Stack, void* Result);
PFN_ProcessEvent g_origProcessEvent = nullptr;

// ---- ⭐ run 91: what the arguments ACTUALLY are ----
//
// The parameter names above are the original guess and they are wrong, but they are left alone
// because renaming them is churn and the mapping is what matters:
//
//     UObject::ProcessInternal(FFrame& Stack, RESULT_DECL)   __thiscall
//         ECX      -> self        the UObject the script call is on
//         stack[0] -> `Function`  the FFrame          (run 86 proved this - FOutputDevice vtable)
//         stack[1] -> `Parms`     the RESULT buffer   ⚠️ predicted from the ABI, NOT yet observed
//         stack[2] -> `Result`    beyond the call's args - garbage
//
// The Result buffer is the whole point now: `GetAdjustedAim` RETURNS the rotator the fire trace
// uses, so overwriting that buffer decouples aim from everything else exactly, with no window
// during which AActor::Rotation holds the hand. That is why the run 76-89 conclusion - "there is
// no instant where the hand can sit in that field without the view taking it too" - is true and
// does not apply here. Nothing is written to the field at all.
// FFrame::Node is no longer a constant - run 92 votes on it at runtime. See ScriptFuncNameIdx.

// The FName index of the script function this frame is executing, or -1.
//
// ⚠️ This replaces reading `Function + OBJ_NAME`, which was the census's whole defect: arg1 is an
// FFrame, not a UObject, so +0x2C was some interior field and the "names" came out as heap
// pointers (run 84). The fix was found in run 87 and never applied to the census.
//
// Index 0 is rejected, not accepted as 'None': run 87 established that a slot holding zero and a
// non-object whose +0x2C happens to be zero both resolve to 'None' perfectly legitimately, so it
// is noise that looks like data.
// ---- ⭐ run 92: the offset is MEASURED, not assumed ----
//
// +0x14 came from run 87 and it is not reliable: in the run 91 log one frame gave
// +0x14 -> 'SeeMonster' and the very next had +0x14 holding zero. A fixed offset that works on
// some frames and not others produces exactly what run 91 saw - no match, no rejection, no
// evidence, and nothing to distinguish it from the function not being on the path.
//
// So vote. For the first kNodeSampleTarget calls, test EVERY pointer slot in the frame against
// the same filter run 87 used, and tally which ones name a real script function. The right slot
// identifies itself by winning across hundreds of different calls; one hit was always a
// coincidence and that is how +0x14 got adopted from six samples during level load.
//
// Guessing offsets on this object has now cost three runs. This is the last time it costs one.
const int kNodeSlots = 16;
const int kNodeSampleTarget = 300;
volatile LONG g_nodeOffset  = -1;   // learned byte offset of FFrame::Node, -1 until decided
volatile LONG g_nodeSamples = 0;
volatile LONG g_nodeVotes[kNodeSlots] = {};
volatile LONG g_nodeDecided = 0;

static int32_t ScriptFuncNameIdx(void* frame) {
    if (!frame) return -1;
    const uintptr_t f = reinterpret_cast<uintptr_t>(frame);

    const LONG off = InterlockedCompareExchange(&g_nodeOffset, 0, 0);
    if (off >= 0) {
        // ---- ⚠️ run 94: NO VirtualQuery on this path ----
        //
        // Readable() is a VirtualQuery, and this now runs inside ProcessInternal - tens of
        // thousands of calls a second. Two VirtualQuery per call is 100k syscalls a second, which
        // would not crash but would eat a double-digit share of the frame and be blamed on
        // something else entirely. The frame itself needs no check at all: it is a reference the
        // engine is actively using, so if it were unreadable the game would already be dead.
        // The node gets a cheap range test instead, the same bound the vote used to accept it.
        const uintptr_t node = *reinterpret_cast<const uintptr_t*>(f + off);
        if (node < 0x01000000 || node > 0x7FFF0000 || (node & 3)) return -1;
        const int32_t idx = *reinterpret_cast<const int32_t*>(node + OBJ_NAME);
        return idx > 0 ? idx : -1;
    }
    if (!Readable(frame, kNodeSlots * 4)) return -1;   // learning phase only, 300 calls

    // ---- learning phase ----
    const LONG n = InterlockedIncrement(&g_nodeSamples);
    if (n <= kNodeSampleTarget) {
        const uint32_t* w = reinterpret_cast<const uint32_t*>(f);
        for (int i = 0; i < kNodeSlots; ++i) {
            const uintptr_t v = w[i];
            if (v < 0x01000000 || v > 0x7FFF0000) continue;
            if (v > f - 0x10000 && v < f + 0x10000) continue;      // a stack address, not a UObject
            if (!Readable((void*)v, OBJ_NAME + 4)) continue;
            if (*reinterpret_cast<const int32_t*>(v + OBJ_NAME) <= 0) continue;   // 'None' is noise
            char nm[64]{};
            if (NameOf(v, nm, sizeof(nm)) && nm[0]) InterlockedIncrement(&g_nodeVotes[i]);
        }
        return -1;
    }

    // ---- decide, once ----
    if (InterlockedExchange(&g_nodeDecided, 1) == 0) {
        int best = -1;
        LONG bestVotes = 0;
        for (int i = 0; i < kNodeSlots; ++i) {
            const LONG v = InterlockedCompareExchange(&g_nodeVotes[i], 0, 0);
            if (v > bestVotes) { bestVotes = v; best = i; }
        }
        Log("FFrame::Node vote over %d calls:", kNodeSampleTarget);
        for (int i = 0; i < kNodeSlots; ++i) {
            const LONG v = InterlockedCompareExchange(&g_nodeVotes[i], 0, 0);
            if (v) Log("    +0x%02X  named a script function %ld times (%.0f%%)",
                       i * 4, v, 100.0 * v / kNodeSampleTarget);
        }
        // A winner has to be convincing. A slot that names something a third of the time is not
        // FFrame::Node, it is some other object pointer that happens to be a UObject sometimes -
        // and adopting it would repeat exactly the mistake being fixed here.
        if (best >= 0 && bestVotes * 2 >= kNodeSampleTarget) {
            InterlockedExchange(&g_nodeOffset, best * 4);
            Log("  -> FFrame::Node is at +0x%02X (%ld of %d). Aim modes 6 and 7 are live from here.",
                best * 4, bestVotes, kNodeSampleTarget);
        } else {
            Log("  *** NO SLOT WINS. Best was +0x%02X with %ld of %d. This object is not an FFrame,"
                " or it is not laid out the way every UE3 build is - either way aim modes 6 and 7"
                " cannot work through this hook and the hook itself is the thing to replace.",
                best * 4, bestVotes, kNodeSampleTarget);
            InterlockedExchange(&g_nodeOffset, -2);   // decided: give up, stop resampling
        }
    }
    return -1;
}

// ---- the script functions, named out of the game's own package (run 91) ----
//
// These are not guesses. tools/uedecompress + tools/uepkg read RvGame.xxx - which is the .u script
// package, LZO-compressed whole-file - and the export table names every one of them, with Raven's
// override and the Engine base it overrides:
//
//     RvWeaponShared.GetAdjustedAim                   super = Engine.Weapon.GetAdjustedAim
//     RvGameSharedPlayerController.GetAdjustedAimFor  super = Engine.PlayerController.GetAdjustedAimFor
//     RvTacticalPawn.GetBaseAimRotation               super = Engine.Pawn.GetBaseAimRotation
//     RvWeaponShared.CalcWeaponFire                   super = Engine.Weapon.CalcWeaponFire
//     RvWeaponShared.InstantFire                      super = Engine.Weapon.InstantFire
//
// Comparing FName INDICES rather than strings, because this runs on every script call in the game
// and a strcmp there would cost more than everything else this mod does. The indices are resolved
// once, off the hot path - see ResolveScriptNames.
struct ScriptFn { const char* name; volatile LONG idx; };
ScriptFn g_scriptFns[] = {
    { "GetAdjustedAim",     -1 },   // [0] the one that RETURNS the fire trace's rotator
    { "GetAdjustedAimFor",  -1 },
    { "GetBaseAimRotation", -1 },
    { "CalcWeaponFire",     -1 },
    { "InstantFire",        -1 },
    { "Tick",               -1 },   // [5] the CONTROL - see below
    { "SetAim",             -1 },   // [6] the GUN'S POSE - see the note on mode 7
    { "PlayerTick",         -1 },   // [7] run 95: the window - see the note on mode 8
    { "ProcessViewRotation",-1 },   // [8] and the things inside it that must NOT see the hand
    { "UpdateCamera",       -1 },   // [9]
    { "LimitViewRotation",  -1 },   // [10]
    // ---- ⭐ run 96: the fire path, found by the census that finally worked ----
    // Enrichment under fire, against a 2.9% baseline for everything that runs every frame:
    //   FireAmmunition 88.9%  NotifyWeaponFired 88.9%  SetFlashLocation 88.9%
    //   BeginFire 86.5%       StartFire 84.2%
    // These are native->script ENTRY POINTS, so unlike GetAdjustedAim they reach this hook.
    // FireAmmunition is the one that matters: it calls InstantFire/ProjectileFire, which call
    // GetAdjustedAim. Put the hand in Rotation for its duration and the trace uses the hand.
    { "FireAmmunition",     -1 },   // [11]
    { "BeginFire",          -1 },   // [12]
    { "StartFire",          -1 },   // [13]
    { "PreBeginPlay",       -1 },   // [14] run 107 - every spawn, in or out of the window
    // ---- ⭐ run 133: is a cutscene running? ----
    //
    // Needed because the cutscene yaw fix cannot be left on during play. Mode 3 works by turning
    // OFF the yaw fold-in, and the fold-in is precisely the mechanism that lets mouse and gamepad
    // turning coexist with head tracking - so shipping it always-on would break the keyboard/mouse
    // and Xbox control schemes before they are written. It has to be scoped to cutscenes.
    //
    // These are native->script entry points, which is the only kind this hook can see (run 94:
    // 87 distinct functions, every one an entry point). SetCinematicMode is UE3's canonical
    // "a scripted sequence is taking over" call; the other two are here as fallbacks in case
    // Raven's cutscenes drive the camera without it.
    //
    // ⚠️ Watched and LOGGED ONLY at this stage. Nothing acts on the result yet. A detector that
    // misfires during play silently breaks turning, which is a far worse failure than the bug it
    // fixes, so it gets measured against a real cutscene before it is allowed to control anything.
    { "SetCinematicMode",   -1 },   // [15]
    { "SetViewTarget",      -1 },   // [16]
    { "ClientSetCameraMode",-1 },   // [17]
};
const int kFnGetAdjustedAim = 0;
const int kFnTick           = 5;
const int kFnSetAim         = 6;
const int kFnPlayerTick     = 7;
const int kFnMaskFirst      = 8;    // [8..10] are the masked-out view functions
const int kFnMaskLast       = 10;
const int kFnFireFirst      = 11;   // [11..13] open the mode 9 window
const int kFnFireLast       = 13;
const int kFnPreBeginPlay   = 14;
const int kFnCineFirst      = 15;   // [15..17] the cutscene candidates
const int kFnSetCinematic   = 15;
const int kFnCineLast       = 17;
const int kScriptFnCount    = (int)(sizeof(g_scriptFns) / sizeof(g_scriptFns[0]));

// What the candidates have actually done, so the log can say which one is usable rather than
// leaving three plausible names and no evidence. Counts are cumulative for the run.
volatile LONG g_cineCalls[3] = { 0, 0, 0 };
// SetCinematicMode's argument, as last seen. UE3 passes script parameters at the head of the
// frame's local buffer, so the bool is the first dword - but that is a claim about layout, and
// the raw dword is logged next to it so a wrong guess is visible rather than silently latching
// the state backwards.
volatile LONG g_cineArgRaw = 0;
volatile LONG g_inCinematic = 0;

static LONG FnIdx(int i) { return InterlockedCompareExchange(&g_scriptFns[i].idx, 0, 0); }

// ---- the nesting census for mode 8 ----
//
// Every function that runs while the hand is sitting in AActor::Rotation, named once each. Mode 4
// failed and left NOTHING to act on - "the view spun" was the whole result. If mode 8's view
// still moves, this list names the function that has to join the mask, in the same run that shows
// the symptom. Bounded to 64 names so it cannot flood the log.
volatile LONG g_tickDepth = 0;
const int kNestMax = 64;
volatile LONG g_nestSeen[kNestMax] = {};
volatile LONG g_nestCount = 0;

static void NoteNested(int32_t idx) {
    const LONG n = InterlockedCompareExchange(&g_nestCount, 0, 0);
    for (LONG i = 0; i < n && i < kNestMax; ++i)
        if (InterlockedCompareExchange(&g_nestSeen[i], 0, 0) == idx) return;
    if (n >= kNestMax) return;
    const LONG slot = InterlockedIncrement(&g_nestCount) - 1;
    if (slot >= kNestMax) return;
    InterlockedExchange(&g_nestSeen[slot], idx);
    char nm[96]{};
    if (!NameFromIndex(idx, nm, sizeof(nm))) _snprintf_s(nm, sizeof(nm), _TRUNCATE, "<idx %d>", idx);
    Log("  inside PlayerTick: %s", nm);
}

// ================================================== the GUN'S POSE, and why it is a second seam
//
// Aim decoupling has two halves and they are NOT the same mechanism:
//
//   the bullet   `GetAdjustedAim` RETURNS a rotator  -> overwrite the result buffer  (mode 6)
//   the gun      `SetAim(float X, float Y)` TAKES two floats -> overwrite the params (mode 7)
//
// Route 1 got the gun to follow the hand by putting the hand into `AActor::Rotation`, which is
// exactly why it costs culling: five other consumers read that field. This does not touch it.
// `SetAim` feeds `RvAnimNodeAimOffset2D`, and the instance that matters is the one in
// `ch_inview_arms_animtree` - the IN-VIEW ARMS tree, i.e. the first-person gun. An animation blend
// is purely cosmetic: nothing culls, traces or collides off it. So the hand can be written here
// with no cost at all, which is the whole point.
//
// All three values then point where they should, which is the arrangement that was wanted:
//
//   AActor::Rotation        HEAD   view, culling, LOD, draw distance, movement basis
//   GetAdjustedAim return   HAND   the bullet
//   SetAim(X, Y)            HAND   the gun you see
//
// ⚠️ The scale and signs are NOT known. X and Y are normalised aim offsets, conventionally +-1
// over the node's configured range, but the range is authored per-profile in the AnimTree and this
// mod cannot read it. So the defaults below are a guess, they are in the ini, and the hook LOGS
// the game's own values next to ours - one run's log is enough to set them exactly. A wrong scale
// makes the gun under- or over-swing; a wrong sign makes it swing the wrong way. Both are obvious
// on sight and neither is dangerous.
// UE3's FFrame runs Node, Object, Code, Locals - so Locals sits 0xC past Node. Node is no longer
// a guess as of run 92, it is voted on at runtime, so this rides on the measured value instead of
// carrying a second hardcoded offset. Still validated on every use, because "0xC past Node" is
// itself an assumption even when Node is not.
static int LocalsOffset() {
    const LONG node = InterlockedCompareExchange(&g_nodeOffset, 0, 0);
    return node >= 0 ? (int)node + 0xC : -1;
}
LONG  g_aimPoseUUPer1 = 16384;    // engine units per 1.0 of anim aim. 16384 = 90 deg.
float g_aimPoseSignX  = 1.0f;
float g_aimPoseSignY  = 1.0f;
volatile LONG g_aimPoseWrites = 0, g_aimPoseRejects = 0;

// Rewrite SetAim's two float parameters in place, BEFORE the original runs - the callee consumes
// them, so unlike the return-value rewrite this one has to happen on the way in.
//
// The write is gated on the parameter block proving itself, for the same reason mode 6's is: this
// dereferences a predicted offset, and a wrong pointer here is silent heap corruption rather than
// a clean failure. Two normalised aim floats are always within +-1, so a block holding two values
// in that range is the parameter block and anything else is not. NaN fails the test by
// construction, since every comparison against NaN is false.
static void RewriteSetAimParams(void* frame) {
    const int localsOff = LocalsOffset();
    if (localsOff < 0) return;                       // Node not decided yet - nothing to ride on
    if (!frame || !Readable(frame, localsOff + 4)) return;
    float* p = *reinterpret_cast<float**>(
                   reinterpret_cast<uintptr_t>(frame) + localsOff);
    if (!p || !Readable(p, 8)) return;

    const bool plausible = p[0] > -1.5f && p[0] < 1.5f && p[1] > -1.5f && p[1] < 1.5f;
    if (!plausible) {
        // Report it and write NOTHING. A silent skip here would look exactly like the gun simply
        // not following the hand, which is the one reading that must stay distinguishable.
        if (InterlockedIncrement(&g_aimPoseRejects) <= 3)
            Log("aim pose: FFrame+0x%02X does not point at two normalised floats (%.3f, %.3f)."
                " Locals is at a different offset in this build - NOTHING written. The gun will"
                " not follow your hand and that is this message, not a failed rewrite.",
                localsOff, p[0], p[1]);
        return;
    }

    // How far the hand is from the head, in the engine's own rotation units. int32 subtraction
    // wraps correctly through the 65536-per-turn space, so crossing the wrap needs no special case.
    const int32_t dYaw   = InterlockedCompareExchange(&g_aimYawForXi,   0, 0) - g_wantYaw;
    const int32_t dPitch = InterlockedCompareExchange(&g_aimPitchForXi, 0, 0) - g_wantPitch;

    const float per = (float)(g_aimPoseUUPer1 ? g_aimPoseUUPer1 : 16384);
    const float wasX = p[0], wasY = p[1];
    float nx = wasX + g_aimPoseSignX * (dYaw   / per);
    float ny = wasY + g_aimPoseSignY * (dPitch / per);
    // Clamp so a bad scale cannot drive the blend node past its authored range and produce a
    // broken pose. Out-of-range input to an aim offset is not a crash, but it does look wrong in
    // a way that is easy to mistake for the wrong seam entirely.
    if (nx >  1.0f) nx =  1.0f;  if (nx < -1.0f) nx = -1.0f;
    if (ny >  1.0f) ny =  1.0f;  if (ny < -1.0f) ny = -1.0f;
    p[0] = nx; p[1] = ny;

    // Calibration data. The game's OWN X and Y are printed next to the view pitch that produced
    // them, which is what makes the scale solvable from one run instead of guessed across several:
    // the ratio between the game's Y and its own pitch IS the units-per-1.0 this needs.
    const LONG n = InterlockedIncrement(&g_aimPoseWrites);
    if (n <= 20 || (n % 600) == 0)
        Log("aim pose #%ld: game (%.3f, %.3f) view pitch %d -> wrote (%.3f, %.3f)"
            "  [hand-head dYaw %d dPitch %d, %ld UU per 1.0]",
            n, wasX, wasY, g_wantPitch, nx, ny, dYaw, dPitch, g_aimPoseUUPer1);
}

// Resolve the table by scanning GNames for each string. Called from the HOTKEY handler, never from
// the hook: it walks tens of thousands of entries with a VirtualQuery each, which is milliseconds -
// fine once on a keypress, ruinous on the hottest function in the process.
//
// ⚠️ It can legitimately fail. GNames is populated as packages load, so pressing the key at the
// main menu before RvGame is in memory finds nothing. That case is LOGGED as itself rather than
// left to look like "the function is not on the fire path" - which is exactly the ambiguity that
// wasted run 82.
// ================================ run 93: find ProcessInternal through UFunction::Func ==========
//
// 0x01308A10 is not it. Run 92 settled that with a measurement rather than an argument: over 300
// calls, no slot in its argument named a script function more than 4% of the time, and `Tick` -
// which every level runs on every actor every frame - appeared ZERO times in 48,811 calls. The
// hook is on something, but not on the general script path.
//
// Four passes have failed to find ProcessEvent by static analysis, and they were always going to:
// it is virtual, so no call graph reaches it. But it does not have to be found in the binary at
// all, because the ENGINE stores its address in a place this mod can already read.
//
// UE3's UFunction::Bind() sets `Func` to &UObject::ProcessInternal for every non-native function.
// So every script UFunction in GObjects - and there are tens of thousands - holds the SAME code
// pointer at the SAME offset. That is a signature nothing else in the object can imitate:
//
//   * find the offset where hundreds of DIFFERENT UFunctions agree on one code-range value
//   * that offset is Func, and that value is ProcessInternal
//
// It needs no disassembly, no signature scan and no guess. It is also the same GObjects walk that
// already finds the controller and the camera, so the machinery is proven.
//
// ⚠️ Native functions hold their own natives here, so agreement will be strong but never total.
// The test is the MODE, not unanimity - and a weak mode is reported as a failure rather than
// adopted, which is the mistake that cost runs 87 through 92.
uintptr_t g_processInternal = 0;
int       g_funcOffset = -1;

// Both defined further down, with the census they belong to. Declared here because the probe has
// to reset them when it moves the hook.
void __fastcall Hook_ProcessEvent(void* self, void* edx, void* Stack, void* Result);
extern volatile LONG g_peSeen, g_peUnnamed;

// ---- ⚠️ run 93b: "in the image" is NOT "is code", and that mistake picked a vtable ----
//
// The first version of this probe accepted anything in 0x00401000-0x01E00000, which spans .rdata
// and .data as well as .text. The winner was +0xC0 at 94% - and 0x017A0520 is in .RDATA, a table
// of code pointers, i.e. the vtable every UFunction shares. A shared vtable beats the real answer
// on agreement every time, so the loose filter did not merely admit noise, it admitted something
// that outvotes the signal.
//
// The runner-up +0xAC at 55% was right all along: 0x004B7E40 is in .text and begins
// 55 8B EC 83 EC 54 ... - an MSVC prologue with a /GS cookie, then `mov edi,ecx` (this in ECX,
// so __thiscall) and two stack arguments. That is ProcessInternal(FFrame&, RESULT_DECL).
//
// So the bound now comes from the image's own section table rather than from a guessed constant.
// The lesson is the project's own, again: a filter wide enough to admit the wrong KIND of thing
// will be won by the wrong thing, and it looks like a stronger result, not a weaker one.
uintptr_t g_textLo = 0, g_textHi = 0;

void FindTextSection() {
    if (g_textLo) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (!base || !Readable((void*)base, 0x40)) return;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (!Readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        // EXECUTE is the property that matters, not the name - a section called .text that is not
        // executable would be just as wrong as .rdata, and this is checking for "can be hooked".
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uintptr_t lo = base + sec[i].VirtualAddress;
        const uintptr_t hi = lo + sec[i].Misc.VirtualSize;
        if (!g_textLo || (hi - lo) > (g_textHi - g_textLo)) { g_textLo = lo; g_textHi = hi; }
    }
    Log("executable range: 0x%08X - 0x%08X (from the image's own section table)",
        (unsigned)g_textLo, (unsigned)g_textHi);
}

void FindProcessInternal() {
    if (g_processInternal) return;
    FindTextSection();
    if (!g_textLo) { Log("Func probe: could not read the image's section table"); return; }
    if (!Readable((void*)kGObjectsTArray, 12)) { Log("Func probe: GObjects not readable"); return; }
    const uintptr_t data = *reinterpret_cast<uintptr_t*>(kGObjectsTArray);
    const int32_t count = *reinterpret_cast<int32_t*>(kGObjectsTArray + 4);
    if (!data || count < 100) { Log("Func probe: GObjects not populated"); return; }
    auto* objs = reinterpret_cast<uintptr_t*>(data);

    // Offsets to test. UStruct is large, so Func sits well past the UObject header; scanning a
    // generous window costs nothing here because this runs once, on a keypress.
    const int kLo = 0x40, kHi = 0x100, kStep = 4;
    const int kOffCount = (kHi - kLo) / kStep;
    const int kBuckets = 6;
    struct Tally { uintptr_t val[kBuckets]; int hit[kBuckets]; };
    static Tally tal[(0x100 - 0x40) / 4];
    memset(tal, 0, sizeof(tal));

    int sampled = 0;
    uintptr_t funcClass = 0;
    char nm[128];

    for (int i = 0; i < count && sampled < 3000; ++i) {
        if (!Readable((void*)(data + i * 4), 4)) continue;
        const uintptr_t o = objs[i];
        if (!o || !Readable((void*)o, kHi)) continue;
        const uintptr_t cls = *reinterpret_cast<uintptr_t*>(o + OBJ_CLASS);
        if (!cls || !Readable((void*)cls, 0x40)) continue;
        if (!funcClass) {
            if (!NameOf(cls, nm, sizeof(nm)) || _stricmp(nm, "Function") != 0) continue;
            funcClass = cls;
        } else if (cls != funcClass) continue;

        ++sampled;
        for (int k = 0; k < kOffCount; ++k) {
            const uintptr_t v = *reinterpret_cast<uintptr_t*>(o + kLo + k * kStep);
            if (v < g_textLo || v >= g_textHi) continue;
            Tally& t = tal[k];
            int slot = -1;
            for (int b = 0; b < kBuckets; ++b) {
                if (t.hit[b] && t.val[b] == v) { slot = b; break; }
                if (!t.hit[b] && slot < 0) slot = b;
            }
            if (slot >= 0) { t.val[slot] = v; ++t.hit[slot]; }
        }
    }

    Log("Func probe: %d UFunction objects sampled from %d GObjects entries", sampled, count);
    if (sampled < 50) {
        Log("  *** too few UFunctions found - is a level loaded? Nothing decided.");
        return;
    }

    int bestOff = -1, bestHit = 0; uintptr_t bestVal = 0;
    for (int k = 0; k < kOffCount; ++k)
        for (int b = 0; b < kBuckets; ++b)
            if (tal[k].hit[b] > bestHit) { bestHit = tal[k].hit[b]; bestVal = tal[k].val[b]; bestOff = kLo + k * kStep; }

    // Print every offset with a strong mode, not just the winner. If two offsets are close, the
    // winner is not trustworthy and that has to be visible rather than inferred from silence.
    for (int k = 0; k < kOffCount; ++k)
        for (int b = 0; b < kBuckets; ++b)
            if (tal[k].hit[b] * 10 >= sampled)
                Log("    +0x%02X  0x%08X shared by %d of %d (%.0f%%)", kLo + k * kStep,
                    (unsigned)tal[k].val[b], tal[k].hit[b], sampled,
                    100.0 * tal[k].hit[b] / sampled);

    if (bestOff >= 0 && bestHit * 2 >= sampled) {
        g_funcOffset = bestOff;
        g_processInternal = bestVal;
        Log("  -> UFunction::Func is at +0x%02X, and UObject::ProcessInternal is at 0x%08X"
            " (%d of %d agree). Found through the object model, with no disassembly.",
            bestOff, (unsigned)bestVal, bestHit, sampled);
    } else {
        Log("  *** NO OFFSET AGREES. Best was +0x%02X with 0x%08X, %d of %d. Either these are not"
            " UFunctions or this build does not bind Func the way UE3 does. Nothing adopted.",
            bestOff, (unsigned)bestVal, bestHit, sampled);
    }
}

// ================= ⭐ run 103: find the native trace, and what it calls =======================
//
// Route 2 continued. Field-swapping is dead - the null window proved every field carrying the aim
// also feeds the view - so the bullet has to be redirected somewhere that holds no rotation at
// all: the TRACE itself. Rotate the End point about Start by the (hand - head) delta and the shot
// goes where the controller points, while nothing the view reads is ever touched.
//
// `Actor.Trace()` is a native function, so its UFunction::Func at +0xAC is the address of the
// native implementation rather than ProcessInternal - the same read that found ProcessInternal,
// which makes this the cheap half.
//
// ⚠️ execTrace itself is the wrong hook point and it is worth saying why before anyone tries it.
// A UE3 native reads its parameters off the BYTECODE STREAM with P_GET_ macros, evaluating each
// expression as it goes. At entry the parameters do not exist yet, so there is nothing to modify;
// by exit the trace has already happened. What we want is the function it calls with explicit
// FVectors - UWorld::SingleLineCheck or AActor::ActorLineCheck - and that one is reachable by
// looking at execTrace's own call targets.
//
// So this logs the address and disassembles just enough to list the CALL rel32 targets. One run
// produces the candidate list; picking from it is desk work against the exe, not another run.
void ProbeTraceNative() {
    static volatile LONG done = 0;
    if (InterlockedExchange(&done, 1)) return;
    if (g_funcOffset < 0) { Log("trace probe: UFunction::Func offset unknown, run the Func probe first"); return; }
    if (!Readable((void*)kGObjectsTArray, 12)) return;
    const uintptr_t data = *reinterpret_cast<uintptr_t*>(kGObjectsTArray);
    const int32_t count = *reinterpret_cast<int32_t*>(kGObjectsTArray + 4);
    if (!data || count < 100) return;
    auto* objs = reinterpret_cast<uintptr_t*>(data);

    const char* kWanted[] = { "Trace", "FastTrace", "TraceAllPhysicsActors", "TraceActors" };
    const int kWantedCount = 4;
    char nm[128], on[128];

    for (int i = 0; i < count; ++i) {
        if (!Readable((void*)(data + i * 4), 4)) continue;
        const uintptr_t o = objs[i];
        if (!o || !Readable((void*)o, (size_t)g_funcOffset + 4)) continue;
        const uintptr_t cls = *reinterpret_cast<uintptr_t*>(o + OBJ_CLASS);
        if (!cls || !Readable((void*)cls, 0x40)) continue;
        if (!NameOf(cls, nm, sizeof(nm)) || _stricmp(nm, "Function") != 0) continue;
        if (!NameOf(o, nm, sizeof(nm))) continue;
        bool want = false;
        for (int w = 0; w < kWantedCount; ++w) if (_stricmp(nm, kWanted[w]) == 0) { want = true; break; }
        if (!want) continue;

        const uintptr_t fn = *reinterpret_cast<uintptr_t*>(o + g_funcOffset);
        const uintptr_t outer = *reinterpret_cast<uintptr_t*>(o + OBJ_OUTER);
        on[0] = 0;
        if (outer && Readable((void*)outer, 0x40)) NameOf(outer, on, sizeof(on));
        const bool isCode = fn >= g_textLo && fn < g_textHi;
        Log("trace probe: %s.%s -> Func 0x%08X %s", on[0] ? on : "?", nm, (unsigned)fn,
            isCode ? "(code)" : "*** NOT CODE - this one is script, not native ***");
        if (!isCode) continue;

        // ---- list the CALL rel32 targets ----
        //
        // A byte-scan for 0xE8, not a real disassembler, so some hits will be the middle of some
        // other instruction. That is fine and is why the targets are FILTERED to .text and printed
        // rather than acted on: a false hit lands outside the section or on a non-prologue, and
        // the real ones are identifiable by their first bytes. Deliberately not clever - a wrong
        // clever answer costs a run and a printed list costs nothing.
        const uint8_t* p = reinterpret_cast<const uint8_t*>(fn);
        if (!Readable((void*)fn, 0x800)) continue;
        int shown = 0;
        for (int off = 0; off < 0x800 - 5 && shown < 24; ++off) {
            if (p[off] != 0xE8) continue;
            int32_t rel; memcpy(&rel, p + off + 1, 4);
            const uintptr_t tgt = fn + off + 5 + (intptr_t)rel;
            if (tgt < g_textLo || tgt >= g_textHi) continue;
            if (!Readable((void*)tgt, 8)) continue;
            const uint8_t* t = reinterpret_cast<const uint8_t*>(tgt);
            Log("      +0x%03X CALL 0x%08X   [%02X %02X %02X %02X %02X %02X]",
                off, (unsigned)tgt, t[0], t[1], t[2], t[3], t[4], t[5]);
            ++shown;
        }
    }
    Log("trace probe: done. The line-check is among the CALL targets above - it is the one taking"
        " two FVectors, and the next step picks it apart against the exe rather than in a headset.");
}

// ================= ⭐ run 104: the line check, and its parameter struct ========================
//
// Found by reading the CALL SITES rather than the callee, which is what made it cheap:
//
//     8D 4D 9C    lea ecx,[ebp-0x64]     ; a local struct
//     51          push ecx               ; ONE argument
//     E8 ...      call 0x00BD79D0
//     ...
//     83 C4 04    add esp,4              ; the CALLER cleans -> __cdecl
//
// Both of Actor.Trace's call sites and Actor.TraceActors' call it the same way, and the SSE just
// before each site is assembling FVectors into that struct - movss to [eax], [eax+4], [eax+8],
// three floats at a time. So the struct carries the trace's Start and End, and rotating End about
// Start there redirects the shot without touching a single field the view can read. That is the
// whole point of route 2 and it is the first mechanism in this project that is structurally
// incapable of moving the camera.
//
// ⚠️ A `ret 4` appears at +0x9A5 and it belongs to a DIFFERENT function - the byte scan ran past
// the end of this one. The call site is authoritative: if the callee also cleaned, every call
// would corrupt the stack by four bytes and the game would never run. Getting this backwards is
// exactly what crashed the game twice on ProcessInternal, so it is checked rather than assumed.
const uintptr_t kLineCheckAddr = 0x00BD79D0;
typedef void* (__cdecl *PFN_LineCheck)(void* params);
PFN_LineCheck g_origLineCheck = nullptr;
volatile LONG g_lineCheckLogged = 0;

// ---- ⭐ run 105: the weapon does not call this, and the log has to say WHY ----
//
// Run 104 installed this hook, the fire window opened 24 times, and NOT ONE trace was dumped. So
// the shot does not go through 0x00BD79D0. Two very different reasons, needing opposite next
// steps, and the previous filter could not tell them apart:
//
//   A. The weapon is a PROJECTILE weapon and never traces at fire time. The run-95 census is
//      already suggestive: PreBeginPlay, PostBeginPlay and SetInitialState all sit at 84.6% under
//      fire, which is the signature of an actor being SPAWNED.
//   B. The trace happens, but outside StartFire/BeginFire/FireAmmunition - `ShouldDeferFire` is in
//      the census too, so Raven may defer the shot to a timer.
//
// So this run stops filtering on the window and reports the raw facts instead: how often the line
// check is called at all, and what the first traces STARTING NEAR THE CAMERA look like whether or
// not a window is open. A player-originated trace starts at the player; that is the filter that
// survives being wrong about when it happens.
volatile LONG g_lineCheckCalls = 0;
volatile LONG g_lineCheckNear = 0;

// ================= ⭐ run 108: ROTATE THE TRACE. This is the actual feature. ===================
//
// Confirmed by counting rather than by filtering: 2 line checks run inside every shot's window.
// The pistol is hitscan, the visible "bullets" are RvEmitter tracers, and the trace goes through
// this function. The near-camera log missed it only because its 12-entry cap was spent on movement
// traces before the first shot.
//
// The rotation is applied to the DIRECTION, not to an absolute rotation:
//
//     d      = End - Start
//     yaw   += (handYaw   - headYaw)
//     pitch += (handPitch - headPitch)
//     End    = Start + rebuilt d, same length
//
// Using the delta rather than an absolute aim means the engine's own conventions for the base
// direction do not have to be known - only the sign of the delta, and that is one ini key if it
// comes out mirrored. Every previous attempt needed the absolute convention to be right.
//
// Nothing here writes a rotation field, so the view CANNOT follow. That is the property the last
// dozen runs were unable to buy at any price.
LONG g_traceSignYaw = 1, g_traceSignPitch = 1;
volatile LONG g_traceRotated = 0;

static void RotateTrace(float* start, float* end, int32_t dYawUU, int32_t dPitchUU) {
    const float dx = end[0] - start[0], dy = end[1] - start[1], dz = end[2] - start[2];
    const float len = sqrtf(dx*dx + dy*dy + dz*dz);
    if (len < 1.0f) return;                       // a degenerate trace has no direction to turn
    const float kUUToRad = 6.2831853f / 65536.0f;
    float yaw   = atan2f(dy, dx) + g_traceSignYaw   * dYawUU   * kUUToRad;
    float pitch = asinf(dz / len) + g_traceSignPitch * dPitchUU * kUUToRad;
    // Clamp pitch short of vertical: at exactly +-90 the yaw term becomes meaningless and the
    // trace would snap to a pole.
    const float kMaxPitch = 1.5533f;              // 89 degrees
    if (pitch >  kMaxPitch) pitch =  kMaxPitch;
    if (pitch < -kMaxPitch) pitch = -kMaxPitch;
    const float cp = cosf(pitch);
    end[0] = start[0] + len * cp * cosf(yaw);
    end[1] = start[1] + len * cp * sinf(yaw);
    end[2] = start[2] + len * sinf(pitch);
}

void* __cdecl Hook_LineCheck(void* params) {
    const LONG lcN = InterlockedIncrement(&g_lineCheckCalls);
    // Proof the hook is live at all. Without this, "no traces during a shot" and "the hook never
    // fired" read identically - the tautological-diagnostic trap this project keeps finding.
    if ((lcN & 0x3FFF) == 0) Log("linecheck: %ld calls so far (hook is live)", lcN);
    // Log ONLY while a fire window is open. Traces run constantly - AI perception, movement,
    // physics - and a log of all of them would be unreadable. g_fireSeq being odd means we are
    // inside StartFire/BeginFire/FireAmmunition, so what gets printed is the WEAPON's trace and
    // nothing else. The window built for mode 9 turns out to be exactly the right filter.
    if (InterlockedCompareExchange(&g_lineCheckNear, 0, 0) < 12 &&
        params && Readable(params, 0x80) &&
        InterlockedCompareExchange(&g_camPosValid, 0, 0)) {
        const float* f = reinterpret_cast<const float*>(params);
        int startAt = -1;
        for (int i = 0; i + 2 < 32; ++i) {
            const float dx = f[i] - g_camPos[0], dy = f[i+1] - g_camPos[1], dz = f[i+2] - g_camPos[2];
            if (dx*dx + dy*dy + dz*dz < 520.0f * 520.0f) { startAt = i; break; }
        }
        // Only traces that START AT THE PLAYER get printed. That is the filter that survives being
        // wrong about WHEN the shot happens - it does not depend on the fire window at all, so a
        // deferred shot is caught just as well as an immediate one.
        if (startAt >= 0) {
            InterlockedIncrement(&g_lineCheckNear);
            const bool inWindow = InterlockedCompareExchange(&g_fireDepth, 0, 0) > 0;
            Log("linecheck #%ld  window %s  camera (%.1f %.1f %.1f)  start-like triple at +0x%02X",
                InterlockedCompareExchange(&g_lineCheckNear, 0, 0), inWindow ? "OPEN" : "closed",
                g_camPos[0], g_camPos[1], g_camPos[2], startAt * 4);
            for (int i = 0; i + 2 < 32; i += 3)
                Log("    +0x%02X  %12.2f %12.2f %12.2f", i * 4, f[i], f[i+1], f[i+2]);
        }
    }
    // ---- the rotation itself, mode 11 ----
    //
    // Gated on the fire window so only the WEAPON's traces move - AI perception, movement and
    // physics queries run through here constantly and must be left alone.
    //
    // Start is located by matching the camera position, exactly as the probe did, rather than by
    // hardcoding +0x68: the offset was read off one build's movement traces and this is a weapon
    // trace. If it does not match, nothing is written and the log says so - the same
    // prove-itself-first rule that stopped mode 6 corrupting the heap.
    // ---- run 111: this no longer ROTATES, it only reports ----
    // 0x00BD79D0 turned out to be the result processor, not the trace. The rotation moved to
    // SingleLineCheck; this block stays because the struct dump is what identified it.
    const LONG amNow = InterlockedCompareExchange(&g_aimMode, 0, 0);
    if (false && (amNow == 11 || amNow == 12) &&
        InterlockedCompareExchange(&g_fireDepth, 0, 0) > 0 &&
        params && Readable(params, 0x100) &&
        InterlockedCompareExchange(&g_camPosValid, 0, 0)) {
        float* f = reinterpret_cast<float*>(params);
        int s = -1;
        // 64 floats, not 32. One of the two traces per shot found nothing in the first 0x80
        // bytes, and a Start living past that would look exactly like "this trace does not start
        // at the player" - which is a conclusion, not a measurement.
        for (int i = 0; i + 5 < 64; ++i) {
            const float ex = f[i] - g_camPos[0], ey = f[i+1] - g_camPos[1], ez = f[i+2] - g_camPos[2];
            if (ex*ex + ey*ey + ez*ez < 520.0f * 520.0f) { s = i; break; }
        }
        // ---- ⭐ run 110: mode 12 takes the hand OUT of the experiment ----
        //
        // Mode 11's deltas came out at dYaw 22110 (121 deg) and dPitch 16336 (89.7 deg). Nobody
        // points a controller 90 degrees up, so the hand-vs-head delta carries a bias - and mode 1
        // has always CLAMPED that deviation to 30 degrees, which is precisely why the bias has
        // never been visible before.
        //
        // That leaves two unknowns stacked: is the rotation reaching the bullet, and is the delta
        // correct. Mode 12 removes the second one by rotating the trace a constant 25 degrees of
        // yaw, exactly as aim mode 2 once probed +0x05D4:
        //
        //   hole 25 deg to the RIGHT -> the rotation reaches the bullet. Only the hand delta is
        //                               wrong, and that is arithmetic rather than archaeology.
        //   hole ON the crosshair    -> we are rotating a trace that does not decide the impact,
        //                               and the OTHER line check is the one that matters.
        const LONG am = InterlockedCompareExchange(&g_aimMode, 0, 0);
        const int32_t kTestYaw = (int32_t)(25.0f * (65536.0f / 360.0f));
        const int32_t dYaw   = (am == 12) ? kTestYaw
                             : InterlockedCompareExchange(&g_aimYawForXi, 0, 0)   - g_wantYaw;
        const int32_t dPitch = (am == 12) ? 0
                             : InterlockedCompareExchange(&g_aimPitchForXi, 0, 0) - g_wantPitch;
        const LONG n = InterlockedIncrement(&g_traceRotated);
        if (s >= 0) {
            float* start = f + s;
            float* end   = f + s + 3;
            const float bx = end[0], by = end[1], bz = end[2];
            RotateTrace(start, end, dYaw, dPitch);
            if (n <= 8)
                Log("trace rotated: start +0x%02X (%.0f %.0f %.0f)  end (%.0f %.0f %.0f) ->"
                    " (%.0f %.0f %.0f)  dYaw %d dPitch %d",
                    s * 4, start[0], start[1], start[2], bx, by, bz,
                    end[0], end[1], end[2], dYaw, dPitch);
        } else if (n <= 4) {
            // DUMP it. Every shot fires two line checks and only one has a Start at the camera;
            // if the bullet is decided by the other, this dump is the only thing that can say so.
            // Saying "not rotated" and nothing else is how the last run ended with a symptom and
            // no evidence.
            Log("trace NOT rotated: no triple near the camera (%.0f %.0f %.0f). Struct follows -"
                " this is the OTHER line check of the pair and may be the one that decides the hit.",
                g_camPos[0], g_camPos[1], g_camPos[2]);
            for (int i = 0; i + 2 < 64; i += 3)
                Log("    +0x%02X  %14.2f %14.2f %14.2f", i * 4, f[i], f[i+1], f[i+2]);
        }
    }

    return g_origLineCheck(params);
}

// ================= ⭐ run 111: UWorld::SingleLineCheck, the REAL trace ========================
//
// 0x00BD79D0 was the wrong function and the data said so: a constant 25 degree deflection (mode 12,
// dYaw 4551) moved nothing, and the struct it receives is mostly zeros and NaNs holding an impact
// position. That is a RESULT being processed, not a trace being performed.
//
// The right one is the other call, and the call site names it completely:
//
//     push esi                       ; arg7
//     lea ecx,[ebp-0x70]  push ecx   ; arg6  Extent
//     push ebx                       ; arg5  TraceFlags
//     lea edx,[ebp-0x7C]  push edx   ; arg4  Start
//     lea eax,[ebp-0xC0]  push eax   ; arg3  End
//     push edi                       ; arg2  SourceActor
//     lea ecx,[ebp-0x64]  push ecx   ; arg1  FCheckResult
//     mov ecx,[0x01CF74E4]           ; this = GWorld, a global
//     call 0x00A108B0
//
// Seven stack args plus `this` in ECX, and the epilogue is `ret 0x1C` - 28 bytes, exactly seven.
// TWO independent confirmations of the argument count, which is what ProcessInternal needed and
// did not get before it crashed the game twice.
//
// arg1 is [ebp-0x64], the same local later handed to 0x00BD79D0 - which is what proves that one is
// the result processor rather than the trace.
const uintptr_t kSingleLineCheckAddr = 0x00A108B0;
typedef int (__fastcall *PFN_SLC)(void* self, void* edx, void* hit, void* src,
                                  float* End, float* Start, uint32_t flags, void* extent, void* a7);
PFN_SLC g_origSLC = nullptr;
volatile LONG g_slcRotated = 0;

int __fastcall Hook_SingleLineCheck(void* self, void* edx, void* hit, void* src,
                                    float* End, float* Start, uint32_t flags,
                                    void* extent, void* a7) {
    if (InterlockedCompareExchange(&g_fireDepth, 0, 0) > 0 &&
        End && Start && Readable(End, 12) && Readable(Start, 12) &&
        InterlockedCompareExchange(&g_camPosValid, 0, 0)) {
        const LONG am = InterlockedCompareExchange(&g_aimMode, 0, 0);
        if (am == 11 || am == 12) {
            // Start must be the player's own eye/muzzle. Nothing else fired inside the window has
            // a reason to originate there, and refusing to touch anything else keeps AI and
            // physics traces - which pour through this function - untouched.
            const float ex = Start[0] - g_camPos[0], ey = Start[1] - g_camPos[1], ez = Start[2] - g_camPos[2];
            if (ex*ex + ey*ey + ez*ez < 520.0f * 520.0f) {
                const int32_t kTestYaw = (int32_t)(25.0f * (65536.0f / 360.0f));
                const int32_t dYaw   = (am == 12) ? kTestYaw
                                     : (int32_t)InterlockedCompareExchange(&g_handDevYawUU, 0, 0);
                const int32_t dPitch = (am == 12) ? 0
                                     : (int32_t)InterlockedCompareExchange(&g_handDevPitchUU, 0, 0);
                const float bx = End[0], by = End[1], bz = End[2];
                RotateTrace(Start, End, dYaw, dPitch);
                const LONG n = InterlockedIncrement(&g_slcRotated);
                if (n <= 8)
                    Log("SingleLineCheck rotated: start (%.0f %.0f %.0f) end (%.0f %.0f %.0f) ->"
                        " (%.0f %.0f %.0f)  dYaw %d dPitch %d  flags 0x%X"
                        "  [yawSign %+d pitchSign %+d  iniYaw %+d iniPitch %+d]",
                        Start[0], Start[1], Start[2], bx, by, bz,
                        End[0], End[1], End[2], dYaw, dPitch, flags,
                        g_yawSign, g_pitchSign, (int)g_traceSignYaw, (int)g_traceSignPitch);
            }
        }
    }
    return g_origSLC(self, edx, hit, src, End, Start, flags, extent, a7);
}

void InstallSingleLineCheckHook() {
    if (g_origSLC) return;                        // idempotent, run 102's lesson
    void* target = reinterpret_cast<void*>(kSingleLineCheckAddr);
    if (!Readable(target, 8)) { Log("SLC: 0x%08X unreadable", (unsigned)kSingleLineCheckAddr); return; }
    static const uint8_t kSig[6] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68 };
    if (memcmp(target, kSig, sizeof(kSig)) != 0) {
        const uint8_t* c = reinterpret_cast<const uint8_t*>(target);
        Log("SLC: 0x%08X prologue mismatch (%02X %02X %02X %02X %02X %02X) - refusing to hook",
            (unsigned)kSingleLineCheckAddr, c[0], c[1], c[2], c[3], c[4], c[5]);
        return;
    }
    const MH_STATUS cs = MH_CreateHook(target, (void*)&Hook_SingleLineCheck, (void**)&g_origSLC);
    if (cs != MH_OK) { Log("SLC: MH_CreateHook failed, status %d", (int)cs); g_origSLC = nullptr; return; }
    const MH_STATUS es = MH_EnableHook(target);
    if (es != MH_OK) { Log("SLC: MH_EnableHook failed, status %d", (int)es); MH_RemoveHook(target); g_origSLC = nullptr; return; }
    Log("UWorld::SingleLineCheck hooked at 0x%08X (__thiscall, 7 stack args, ret 0x1C).",
        (unsigned)kSingleLineCheckAddr);
}

void InstallLineCheckHook() {
    if (g_origLineCheck) return;                     // idempotent - run 102's lesson
    void* target = reinterpret_cast<void*>(kLineCheckAddr);
    if (!Readable(target, 8)) { Log("linecheck: 0x%08X unreadable", (unsigned)kLineCheckAddr); return; }
    // Signature check. The address came from this build; refusing to hook anything that does not
    // start the way the analysis said stops a different build from being patched blind.
    static const uint8_t kSig[7] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08 };
    if (memcmp(target, kSig, sizeof(kSig)) != 0) {
        const uint8_t* c = reinterpret_cast<const uint8_t*>(target);
        Log("linecheck: 0x%08X does NOT match the expected prologue (%02X %02X %02X %02X %02X %02X %02X)"
            " - refusing to hook", (unsigned)kLineCheckAddr, c[0], c[1], c[2], c[3], c[4], c[5], c[6]);
        return;
    }
    const MH_STATUS cs = MH_CreateHook(target, (void*)&Hook_LineCheck, (void**)&g_origLineCheck);
    if (cs != MH_OK) { Log("linecheck: MH_CreateHook failed, status %d", (int)cs); g_origLineCheck = nullptr; return; }
    const MH_STATUS es = MH_EnableHook(target);
    if (es != MH_OK) { Log("linecheck: MH_EnableHook failed, status %d", (int)es); MH_RemoveHook(target); g_origLineCheck = nullptr; return; }
    Log("linecheck hooked at 0x%08X (__cdecl, one param struct). Fire in aim mode 10 and the"
        " weapon's own trace parameters are dumped.", (unsigned)kLineCheckAddr);
}

// Move the script hook onto the address the probe found. The old detour at 0x01308A10 is removed
// first: leaving a hook on a function that was proven to be the wrong one costs frame time on
// every call and can only confuse the next log.
// ---- 💥 run 102: this crashed on the SECOND press, and the bug was three lines ----
//
// Cycling to another mode >= 6 called this again. The old body then:
//   1. saw g_origProcessEvent non-null and removed a hook at kProcessEventAddr - an address
//      nothing has been hooked at since run 94 - and nulled the trampoline anyway, while the
//      REAL hook at 0x004B7E40 was still live;
//   2. got MH_ERROR_ALREADY_CREATED from MH_CreateHook and nulled the trampoline a second time;
//   3. left every subsequent script call doing g_origProcessEvent(...) on a null pointer.
//
// So it is now idempotent, it never clears the trampoline while a hook is installed, and it does
// not touch the disproven address at all. The rule this violated is worth stating: a function
// that installs something must be safe to call twice, because a hotkey WILL be pressed twice.
uintptr_t g_hookedAt = 0;

void RepointScriptHook() {
    if (!g_processInternal) return;
    if (g_hookedAt == g_processInternal && g_origProcessEvent) return;   // already done, no-op
    void* target = reinterpret_cast<void*>(g_processInternal);

    // Print the first bytes of whatever is about to be hooked. Run 93 spent a whole run on
    // "could not hook 0x017A0520" without recording that the address was in .rdata and held a
    // vtable - the one fact that explained it. A failure message that does not carry the evidence
    // for its own diagnosis costs a run every time.
    if (Readable(target, 16)) {
        const uint8_t* c = reinterpret_cast<const uint8_t*>(target);
        Log("  bytes at 0x%08X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            (unsigned)g_processInternal, c[0], c[1], c[2], c[3], c[4], c[5],
            c[6], c[7], c[8], c[9], c[10], c[11]);
    } else {
        Log("  *** 0x%08X is not even readable - it is not a function.",
            (unsigned)g_processInternal);
        return;
    }
    if (g_processInternal < g_textLo || g_processInternal >= g_textHi) {
        Log("  *** 0x%08X is OUTSIDE the executable range 0x%08X-0x%08X. It is data, not code,"
            " and hooking it would be meaningless.", (unsigned)g_processInternal,
            (unsigned)g_textLo, (unsigned)g_textHi);
        return;
    }

    const MH_STATUS cs = MH_CreateHook(target, (void*)&Hook_ProcessEvent, (void**)&g_origProcessEvent);
    if (cs != MH_OK) {
        Log("  *** MH_CreateHook(0x%08X) failed, status %d - aim modes 6 and 7 stay dead this run.",
            (unsigned)g_processInternal, (int)cs);
        // NOT cleared: if a hook is already installed the trampoline is still valid, and
        // nulling it here is what turned a failed re-install into a null call on every
        // script function in the game.
        return;
    }
    const MH_STATUS es = MH_EnableHook(target);
    if (es != MH_OK) {
        Log("  *** MH_EnableHook(0x%08X) failed, status %d - aim modes 6 and 7 stay dead this run.",
            (unsigned)g_processInternal, (int)es);
        MH_RemoveHook(target);
        g_origProcessEvent = nullptr;
        return;
    }
    g_hookedAt = g_processInternal;
    // The FFrame vote has to start over: it was fed 300 frames from the WRONG function, and its
    // verdict was about that object, not this one.
    InterlockedExchange(&g_nodeOffset, -1);
    InterlockedExchange(&g_nodeDecided, 0);
    InterlockedExchange(&g_nodeSamples, 0);
    for (int i = 0; i < kNodeSlots; ++i) InterlockedExchange(&g_nodeVotes[i], 0);
    InterlockedExchange(&g_peSeen, 0);
    InterlockedExchange(&g_peUnnamed, 0);
    Log("  script hook MOVED to 0x%08X. The FFrame vote restarts on real frames from here -"
        " expect a slot to win convincingly this time, and aim modes 6 and 7 to come alive"
        " a fraction of a second later.", (unsigned)g_processInternal);
}

void ResolveScriptNames() {
    if (!Readable((void*)kGNamesTArray, 12)) { Log("script names: GNames not readable yet"); return; }
    const int32_t nc = *reinterpret_cast<int32_t*>(kGNamesTArray + 4);
    int found = 0;
    // ---- ⭐ run 133: ONE walk over GNames, not one per unresolved name ----
    //
    // This used to loop the table on the outside and GNames on the inside, so an unresolved name
    // cost a full walk of its own - 18 names against ~100k entries. That was affordable while this
    // only ever ran on a keypress. It now also runs automatically (see the caller in Present), and
    // a name that never resolves would have made every retry walk the whole table again for
    // nothing. Inverting the loops makes a retry cost one walk regardless of how many are missing.
    for (int32_t n = 0; n < nc; ++n) {
        char nm[96]{};
        if (!NameFromIndex(n, nm, sizeof(nm))) continue;
        for (int i = 0; i < kScriptFnCount; ++i) {
            if (FnIdx(i) >= 0) continue;
            if (_stricmp(nm, g_scriptFns[i].name) == 0) {
                InterlockedExchange(&g_scriptFns[i].idx, n);
                break;
            }
        }
    }
    for (int i = 0; i < kScriptFnCount; ++i) if (FnIdx(i) >= 0) ++found;
    Log("script names: %d of %d resolved against %d GNames entries", found, kScriptFnCount, nc);
    for (int i = 0; i < kScriptFnCount; ++i)
        Log("    %-22s %s", g_scriptFns[i].name,
            FnIdx(i) >= 0 ? "resolved" : "NOT FOUND - is a level loaded?");
    if (FnIdx(kFnGetAdjustedAim) < 0)
        Log("  *** GetAdjustedAim did not resolve. Aim mode 6 CANNOT WORK until it does - load a"
            " save and press this again, rather than reading the next test as a negative.");
}

// ---- the census: which script functions run when you pull the trigger ----
//
// ProcessEvent carries the UFunction, and UFunction is a UObject whose name resolves through the
// GNames machinery this mod already has. So every script call can be named for free.
//
// The point is not to list them - there are thousands a second - it is to DIFF them. Count each
// name separately while the trigger is down and while it is not, and the fire path separates
// itself out: names that appear only under fire are the candidates for the seam where
// Controller.Rotation has to be the hand rather than the head.
//
// Open-addressed and fixed size because this is one of the hottest functions in the process. A
// linear scan over a few hundred entries here would cost more than everything else this mod does.
const int kPeSlots = 2048;
struct PeSlot { int32_t nameIdx; uint32_t idle, firing; };
PeSlot g_peSlots[kPeSlots]{};
volatile LONG g_peCensus = 0;      // only counts while armed - the table is useless if it fills up
                                   // with menu and startup traffic before the test begins
volatile LONG g_peCalls = 0;
// Run 92: free of the census gate on purpose. These count every call the hook sees while any
// route-2 mode or the census is on, and their RATIO is what separates "the hook is blind" from
// "the hook works and the name source is wrong" - see the note at the call site.
volatile LONG g_peSeen = 0, g_peUnnamed = 0;
volatile LONG g_peFiringCalls = 0; // ⚠️ if this stays 0 the split never happened and the whole
                                   // table means nothing - run 82 dumped a census of 5544 calls
                                   // with zero firing samples and reported "nothing", which reads
                                   // identically to "the fire path uses no script"

void __fastcall Hook_ProcessEvent(void* self, void* edx, void* Stack, void* Result) {
    // ---- run 102: never call a null trampoline ----
    //
    // A detour whose trampoline has been cleared while the detour is still installed turns every
    // script call in the game into a null call. That is what crashed on the second NUMPAD-dot
    // press. The install path is fixed, but this costs one predicted-taken branch on the hot path
    // and removes the whole failure class rather than the one instance of it.
    //
    // Returning without calling through breaks script execution, which is bad - but it is a
    // diagnosable stall with a log line, not an access violation.
    if (!g_origProcessEvent) {
        static volatile LONG told = 0;
        if (InterlockedIncrement(&told) == 1)
            Log("*** script hook has no trampoline - calling through is impossible. The game will"
                " misbehave from here. This is a bug in the install path, not in the engine.");
        return;
    }
    // ---- the name-source probe, OUTSIDE the census gate ----
    //
    // It was inside it last run and therefore never fired, because the census has to be armed by
    // hand and the test that needed the probe did not require arming. Nothing in the log, which
    // reads exactly like the probe finding nothing. A diagnostic behind a switch the test does not
    // touch is not a diagnostic.
    {
        // ---- run 85: stop guessing the offset, scan every one ----
        //
        // +0x2C is the name offset for objects reached through GObjects, and it is wrong here:
        // `this` gave 0 and -65536, while arg1 gave 'IntProperty' - a CLASS name, so arg1 is some
        // UProperty-shaped thing and the census has been bucketing on whatever sits there.
        //
        // Guessing one offset at a time costs a headset run each. So walk every 4-byte slot in the
        // first 0x40 bytes of both objects, resolve each as an FName index, and print the ones
        // that come back as real names. The right offset identifies itself, and the raw dump
        // alongside means the layout can be read even if none of them resolve.
        // 30 samples rather than 6: the first handful are startup and loading traffic, and the
        // point is to see the SAME offset name a function across many different calls. One hit
        // could be a coincidence; twenty cannot.
        static volatile LONG probed = 0;
        if (InterlockedIncrement(&probed) <= 30) {
            struct { const char* tag; void* p; } cands[2] = { { "this", self }, { "arg1", Stack } };
            for (int c = 0; c < 2; ++c) {
                if (!cands[c].p || !Readable(cands[c].p, 0x40)) {
                    Log("PE scan %s=%p  <unreadable>", cands[c].tag, cands[c].p);
                    continue;
                }
                const uint32_t* w = reinterpret_cast<const uint32_t*>(cands[c].p);
                char hex[260]; int n = 0;
                for (int i = 0; i < 16; ++i)
                    n += _snprintf_s(hex + n, sizeof(hex) - n, _TRUNCATE, "%08X ", w[i]);
                Log("PE scan %s=%p  %s", cands[c].tag, cands[c].p, hex);
                // ---- run 86: resolve POINTER fields as objects, not ints as name indices ----
                //
                // arg1 turned out to be an FFrame - stack address, FOutputDevice vtable, a link to
                // the neighbouring frame - so the thing being executed is reachable through one of
                // its pointer members (FFrame::Node, the UStruct/UFunction). Treating small ints
                // as name indices was never going to find it, and produced only false positives:
                // 00000004 resolving to 'FloatProperty' is just index 4 being a legal index.
                //
                // So walk the pointer-shaped slots and ask NameOf - the same known-good path that
                // reads names off GObjects entries - which slot names a script function.
                // ---- run 87: two filters, and they remove almost all of it ----
                //
                // The previous pass printed 'None' five times a line. Every one was FName index 0
                // resolving legitimately - a slot holding zero, or a non-object whose +0x2C
                // happens to be zero. Noise that looks like data.
                //
                // Also: one hit ('Assert') came off 0225F500, which is a STACK address a few bytes
                // from the frame itself. An FFrame's members point at the heap; a pointer into its
                // own neighbourhood is the previous frame or a local, not a UObject.
                //
                // So drop index 0, and drop anything within 64K of the frame pointer. What
                // survived both filters last run was +0x14 -> 'SeeMonster', a real script function,
                // which is FFrame::Node. This confirms it or corrects it without a third guess.
                char line[400]; int m = 0;
                const uintptr_t selfAddr = reinterpret_cast<uintptr_t>(cands[c].p);
                for (int i = 0; i < 16; ++i) {
                    const uintptr_t v = w[i];
                    if (v < 0x01000000 || v > 0x7FFF0000) continue;
                    if (v > selfAddr - 0x10000 && v < selfAddr + 0x10000) continue;   // stack
                    if (!Readable((void*)v, 0x40)) continue;
                    const int32_t ni = *reinterpret_cast<const int32_t*>(v + OBJ_NAME);
                    if (ni <= 0) continue;                                            // 'None'
                    char nm[64]{};
                    if (NameOf(v, nm, sizeof(nm)) && nm[0])
                        m += _snprintf_s(line + m, sizeof(line) - m, _TRUNCATE,
                                         " +0x%02X->'%s'", i * 4, nm);
                }
                Log("     objects:%s", m ? line : " (none)");
            }
        }
    }

    // ---- ⭐ run 92: resolve the name ONCE, and MEASURE how often it fails ----
    //
    // Run 91's test produced no rewrite and no rejection: neither GetAdjustedAim nor SetAim ever
    // matched. Two completely different causes, needing opposite fixes:
    //
    //   A. the hook is not on the general script path (it is an opcode handler, not ProcessInternal)
    //   B. the hook IS on the path but FFrame::Node is not reliably at +0x14, so nothing is named
    //
    // The run 91 log already hints at B - among the first 30 frames, one gave +0x14 -> 'SeeMonster'
    // and another had +0x14 holding ZERO - but a hint from six samples during level load is not a
    // finding. `unnamed` below is the measurement: if most calls fail to name anything, the name
    // source is wrong and no census could ever have worked. If nearly all of them DO name
    // something, the source is fine and the answer is A.
    //
    // Combined with the Tick control in the dump, one run now separates A from B. Previously the
    // census could only ever have reported "the functions are absent", which is what both look
    // like from the outside.
    //
    // Resolving once also HALVES the cost: ScriptFuncNameIdx calls Readable(), which is a
    // VirtualQuery, and this is the hottest function in the process. It used to run twice per call
    // whenever a route-2 mode was on.
    const LONG aimMode = InterlockedCompareExchange(&g_aimMode, 0, 0);
    // Mode 8 needs the name on every call too - it is the whole mechanism, not a diagnostic.
    const bool routeTwo = (aimMode >= 6);
    const bool censusOn = InterlockedCompareExchange(&g_peCensus, 0, 0) != 0;
    int32_t fnIdx = -1;
    if ((censusOn || routeTwo) && Stack) {
        fnIdx = ScriptFuncNameIdx(Stack);
        InterlockedIncrement(&g_peSeen);
        if (fnIdx < 0) InterlockedIncrement(&g_peUnnamed);
    }

    if (censusOn && Stack) {
        InterlockedIncrement(&g_peCalls);
        // ---- ⚠️ run 84: the name indices came out as HEAP POINTERS, so this is reading the
        // wrong object ----
        //
        // The census itself worked - two entries sat at 83% and 89% under fire, which is exactly
        // the enrichment being hunted - but the names read as 36039212 and 19958482. GNames holds
        // tens of thousands of entries, not tens of millions, and those values land past the end
        // of the image, so they are heap pointers. Only index 0 resolved, as "None".
        //
        // The likely cause is which object holds the name. Ghidra decompiled this function using
        // `this` as something with fields at +0x40, +0x4C, +0x50 and +0x6A - the shape of a
        // UFunction (code offset, locals size, flags), not of the object a method is being called
        // on. So this may be `UFunction::Invoke(UObject*, FFrame&, RESULT)` rather than
        // `UObject::ProcessEvent(UFunction*, ...)`. Both are __thiscall with three stack args, and
        // both give `ret 0xC` - the epilogue proved the ABI but could never have told them apart.
        //
        // So stop inferring and print BOTH candidates for the first few calls. Whichever yields
        // small indices that resolve to script names is the right one.
        // ✅ Run 91: FIXED. The name now comes from FFrame::Node, which is what run 87 identified
        // and what the four lines of comment above have been describing while the code below read
        // the wrong object anyway.
        {
            const int32_t idx = fnIdx;
            if (idx >= 0) {
                // ⚠️ Run 82: this used to read the published pad's trigger byte, and came back
                // with ZERO firing samples out of 5544 calls. Two ways that happens and both are
                // real: the shot was fired with the mouse rather than the controller, or auto-pad
                // had unplugged the pad so nothing was published at all. Either way the census
                // could only ever report "nothing", which is indistinguishable from the fire path
                // genuinely using no script.
                //
                // So take the signal from every source that can mean "firing" - the raw OpenXR
                // trigger, the published pad, and the mouse button - rather than from the one that
                // happened to be wired up.
                const LONG b = InterlockedCompareExchange(&g_padBtns, 0, 0);
                const bool firing = ((b >> 24) & 0xFF) > 40
                                 || InterlockedCompareExchange(&g_rawTrigger, 0, 0) > 20
                                 || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                if (firing) InterlockedIncrement(&g_peFiringCalls);
                uint32_t h = (uint32_t)idx * 2654435761u;
                for (int i = 0; i < 8; ++i) {
                    PeSlot& s = g_peSlots[(h + i) & (kPeSlots - 1)];
                    if (s.nameIdx == 0 && s.idle == 0 && s.firing == 0) s.nameIdx = idx;
                    if (s.nameIdx == idx) { if (firing) ++s.firing; else ++s.idle; break; }
                }
            }
        }
    }
    // ---- ⭐ run 91: aim mode 6 - rewrite what GetAdjustedAim RETURNS ----
    //
    // Everything above this line is diagnostics. This is the feature.
    //
    // The name is checked BEFORE the original call so the common case costs one integer compare,
    // and the buffer is touched only AFTER it, because the value being replaced is the one the
    // original just wrote.
    const bool wantAim = routeTwo
                      && FnIdx(kFnGetAdjustedAim) >= 0
                      && fnIdx == FnIdx(kFnGetAdjustedAim);

    // ---- mode 7: the gun's pose. On the way IN, because the callee consumes its parameters ----
    if (aimMode == 7 && FnIdx(kFnSetAim) >= 0 && fnIdx == FnIdx(kFnSetAim))
        RewriteSetAimParams(Stack);

    // ---- ⭐ run 133: watch for a cutscene starting. Observation only ----
    //
    // Three integer compares on a hot path, and only when the names resolved. Nothing here
    // changes engine state or mod behaviour - it counts, latches and logs, so one run through the
    // opening cutscene says which of the three candidates fires and whether its argument reads
    // the way the layout note predicts.
    for (int ci = kFnCineFirst; ci <= kFnCineLast; ++ci) {
        if (FnIdx(ci) < 0 || fnIdx != FnIdx(ci)) continue;
        const LONG n = InterlockedIncrement(&g_cineCalls[ci - kFnCineFirst]);
        if (ci == kFnSetCinematic) {
            // The bool argument, read the same way mode 7 reads SetAim's floats - through the
            // frame's locals pointer. Logged raw as well as interpreted, because "the first dword
            // is the bool" is an assumption about UE3's frame layout and not something this run
            // has verified. If the raw value is neither 0 nor 1 the assumption is wrong and the
            // latch below must not be trusted.
            const int localsOff = LocalsOffset();
            if (localsOff >= 0 && Stack && Readable(Stack, localsOff + 4)) {
                uint8_t* p = *reinterpret_cast<uint8_t**>(
                                 reinterpret_cast<uintptr_t>(Stack) + localsOff);
                if (p && Readable(p, 4)) {
                    const LONG raw = (LONG)(*reinterpret_cast<uint32_t*>(p));
                    InterlockedExchange(&g_cineArgRaw, raw);
                    InterlockedExchange(&g_inCinematic, raw ? 1 : 0);
                }
            }
        }
        if (n <= 20)
            Log("cutscene watch: %s fired (#%ld)%s", g_scriptFns[ci].name, n,
                ci == kFnSetCinematic ? "" : " - fallback candidate, no argument read");
        if (ci == kFnSetCinematic && n <= 20)
            Log("    arg raw 0x%08lX -> CINE %s", InterlockedCompareExchange(&g_cineArgRaw, 0, 0),
                InterlockedCompareExchange(&g_inCinematic, 0, 0) ? "ON" : "OFF");
        break;
    }

    // ================= ⭐ run 95: aim mode 8 - the MASKED time split ==============================
    //
    // Run 94 proved this hook is genuinely ProcessInternal and genuinely works: 152,430 calls, 0
    // unnamed, and `Tick` present 2347 times. It also proved the aim functions are not reachable
    // through it. Only 87 distinct functions appear and every one is a native->script ENTRY POINT;
    // script-to-script calls do not pass through, which matches ENGINE_NOTES' finding that
    // CallFunction is inlined into the interpreter. GetAdjustedAim is called from script, so it
    // will never be here, and neither will SetAim. That route is closed - by measurement, not by
    // another guess.
    //
    // But one of those 87 is `PlayerTick`, and it is the PlayerController's per-frame update INSIDE
    // the game tick. That is a seam modes 4 and 5 never had: they wrote the hand at Present and at
    // the XInput poll, both OUTSIDE the tick, and held it for a whole frame. This holds it for the
    // duration of one function and gives both ends back.
    //
    // ⚠️ Why this is not simply mode 4 again. The run 89 conclusion was that the camera consumes
    // the rotation at more than one moment, so no frame-wide write can work. Correct - and the fix
    // is to make the window smaller than the camera's consumption points, then MASK the ones that
    // fall inside it. `ProcessViewRotation`, `UpdateCamera` and `LimitViewRotation` are all in the
    // 87, so if they nest inside PlayerTick they get the HEAD put back for their duration and the
    // view never sees the hand at all.
    //
    // The nesting census below is what makes this checkable rather than hopeful: it records what
    // actually runs inside PlayerTick, so if the view still swings, the log already names the
    // function that has to join the mask. That is the difference between this and mode 4, which
    // failed and left nothing to act on.
    // ================= ⭐ run 96: aim mode 9 - the window is the SHOT, not the tick ==============
    //
    // Mode 8 failed and said exactly why. Its nesting log named `PlayerMove`, and UE3's PlayerMove
    // calls UpdateRotation, which does:
    //
    //     ViewRotation = Rotation;                    <-- copies the HAND out
    //     ProcessViewRotation( DeltaTime, ViewRotation, DeltaRot );
    //     SetRotation( ViewRotation );                <-- writes hand+delta back, permanently
    //
    // UpdateRotation is a script-to-script call and therefore invisible here, and the copy happens
    // BEFORE ProcessViewRotation - so masking that function was always too late. That single
    // mechanism produced both reported symptoms: the view took the hand permanently, and the mod's
    // external-delta fold-in then read its own write back as somebody else's turning, which is
    // what inverted head tracking.
    //
    // The fix is not a better mask. It is a window that does not contain the view update at all.
    // `FireAmmunition` runs 32 times a session instead of PlayerTick's 636, it is 88.9% enriched
    // under fire against a 2.9% baseline, and it is the function that calls InstantFire /
    // ProjectileFire -> GetAdjustedAim. The view is computed in PlayerMove, a different entry
    // point, so it cannot see inside this window at all.
    //
    // ⚠️ This is the third time the time split has been tried, so the difference is worth stating
    // plainly rather than asserted. Modes 4 and 5 held the hand for a whole FRAME. Mode 8 held it
    // for a whole TICK, which still contained the view update. This holds it for one shot.
    // ---- run 97: the window was right, the FIELD was wrong ----
    //
    // Run 96 put the hand in Controller.Rotation for the duration of the shot - the log confirms
    // it, including StartFire -> BeginFire nesting restoring correctly at each level - and the
    // bullet still went down the crosshair. So the trace does not read Controller.Rotation.
    //
    // UE3's GetAdjustedAimFor commonly returns the view point rather than the controller's own
    // rotation, and GetPlayerViewPoint reads the CAMERA: the camera actor's Rotation and
    // mCurrentPOV.Rotation. This mod already owns both objects. So the window now swaps all three
    // and restores all three, which covers every field the aim can be read from without touching
    // anything for longer than one call.
    //
    // Writing the camera for a few microseconds cannot disturb the view: the view matrix for this
    // frame was built long before, and Present rewrites the camera every frame regardless.
    // ---- ⭐ run 101: mode 10 is mode 9 with the WRITE removed ----
    //
    // Reported: "it feels like it's cycling between the views your test is setting." That is a
    // better description than anything I had, and it is not recoil - recoil does not alternate
    // between two specific orientations.
    //
    // But three builds have now been aimed at "the camera moves when I shoot" without ever
    // establishing that the mod causes it. So this is the control, built in rather than asked for:
    // mode 10 runs the identical window - same functions, same hook, same nesting, same logging,
    // same seqlock - and writes NOTHING.
    //
    //   view still cycles in mode 10  -> it is not our value. The window is innocent and the cause
    //                                    is the game (recoil) or something else entirely.
    //   view steady in mode 10, moves in mode 9 -> it IS our write reaching the view, and no
    //                                    amount of narrowing which field will fix that, because
    //                                    the engine reads these fields for the view later in the
    //                                    same tick.
    //
    // Either answer ends a line of work rather than extending one, which is the thing the last
    // three runs did not do.
    // ---- ⭐ run 105: what actually happens during a shot, with the OBJECT named ----
    //
    // The census named the functions but never the objects they ran on, and that is the missing
    // half. `PreBeginPlay` at 84.6% under fire means something is being spawned - but a census of
    // NAMES cannot say WHAT. If the class is a projectile, the weapon does not trace at all and
    // the whole line-check route is inapplicable to it; if it is an effect or a decal, the trace
    // is simply elsewhere.
    //
    // Cheap because it only runs while a fire window is open and only logs each function once.
    // ---- ⭐ run 107: log EVERY spawn, in or out of the window ----
    //
    // Reported: "when I shoot I do see bullets come out." Two readings, and they need opposite
    // work: a hitscan weapon's visible tracer is an emitter spawned along the trace line, while a
    // real projectile is an actor with its own rotation and no trace at all.
    //
    // The in-shot log below cannot tell them apart, because it only records what happens while the
    // fire window is OPEN - and a projectile spawned on a deferred tick would be invisible to it.
    // Which is the same defect three times over: a filter that encodes the expected answer.
    //
    // So PreBeginPlay is logged wherever it happens, with the class of the object being spawned
    // and whether a window was open at the time. RvEmitter means tracer; anything projectile-shaped
    // spawned OUTSIDE the window means the shot is deferred and the whole fire-window premise is
    // wrong.
    if ((aimMode == 9 || aimMode == 10) && fnIdx >= 0 && self &&
        FnIdx(kFnPreBeginPlay) >= 0 && fnIdx == FnIdx(kFnPreBeginPlay)) {
        static volatile LONG spawns = 0;
        if (InterlockedIncrement(&spawns) <= 40) {
            char cn[96]{};
            const uintptr_t cls = Readable(self, OBJ_CLASS + 4)
                                ? *reinterpret_cast<uintptr_t*>((uintptr_t)self + OBJ_CLASS) : 0;
            if (cls && Readable((void*)cls, 0x40)) NameOf(cls, cn, sizeof(cn));
            Log("  spawn: %-28s  window %s", cn[0] ? cn : "?",
                InterlockedCompareExchange(&g_fireDepth, 0, 0) > 0 ? "OPEN" : "closed");
        }
    }

    if ((aimMode == 9 || aimMode == 10) && fnIdx >= 0 &&
        InterlockedCompareExchange(&g_fireDepth, 0, 0) > 0 && self) {
        static volatile LONG shown = 0;
        // Cap 150, not 30. Run 105 spent all thirty entries on muzzle-flash emitters and stopped
        // exactly AT FireAmmunition - truncating the log at the one place it mattered.
        if (InterlockedCompareExchange(&shown, 0, 0) < 150) {
            InterlockedIncrement(&shown);
            char fn[96]{}, cn[96]{};
            NameFromIndex(fnIdx, fn, sizeof(fn));
            const uintptr_t cls = Readable(self, OBJ_CLASS + 4)
                                ? *reinterpret_cast<uintptr_t*>((uintptr_t)self + OBJ_CLASS) : 0;
            if (cls && Readable((void*)cls, 0x40)) NameOf(cls, cn, sizeof(cn));
            Log("  in-shot: %-24s on a %s", fn[0] ? fn : "?", cn[0] ? cn : "?");
        }
    }

    const bool mode10 = (aimMode == 10) && fnIdx >= 0;
    const bool mode9 = ((aimMode == 9) || mode10 || (aimMode == 11) || (aimMode == 12)) && fnIdx >= 0;
    // Mode 11 also opens a null window: it needs the window as a FILTER for the trace hook, but
    // must not write a rotation field or the view would follow - which is the whole point.
    const bool nullWindow = mode10 || (aimMode == 11) || (aimMode == 12);
    bool firedWindow = false;
    int32_t savedCtl[3] = {}, savedCam[3] = {}, savedPov[3] = {};
    bool haveCtl = false, haveCam = false, havePov = false;
    uintptr_t fireCtl = 0, fireCam = 0;
    const int kExtraMax = 2 * kRotFieldMax;
    int32_t* extraAddr[kExtraMax] = {};
    int32_t  extraSaved[kExtraMax][2] = {};
    int      nExtra = 0;
    LONG     lcAtOpen = 0;

    if (mode9) {
        bool isFireFn = false;
        for (int fk = kFnFireFirst; fk <= kFnFireLast; ++fk)
            if (FnIdx(fk) >= 0 && fnIdx == FnIdx(fk)) { isFireFn = true; break; }
        if (isFireFn) {
            const int32_t hp = InterlockedCompareExchange(&g_aimPitchForXi, 0, 0);
            const int32_t hy = InterlockedCompareExchange(&g_aimYawForXi, 0, 0);
            fireCtl = (uintptr_t)InterlockedCompareExchange(&g_ctlForXi, 0, 0);
            fireCam = g_camera;

            if (fireCtl && Readable((void*)(fireCtl + ACTOR_ROTATION), 12)) {
                int32_t* r = reinterpret_cast<int32_t*>(fireCtl + ACTOR_ROTATION);
                savedCtl[0] = r[0]; savedCtl[1] = r[1]; savedCtl[2] = r[2];
                if (!nullWindow) { r[0] = hp; r[1] = hy; }
                haveCtl = true;
            }
            if (g_aimFieldSet >= 2 && fireCam && Readable((void*)(fireCam + ACTOR_ROTATION), 12)) {
                int32_t* r = reinterpret_cast<int32_t*>(fireCam + ACTOR_ROTATION);
                savedCam[0] = r[0]; savedCam[1] = r[1]; savedCam[2] = r[2];
                if (!nullWindow) { r[0] = hp; r[1] = hy; }
                haveCam = true;
            }
            if (g_aimFieldSet >= 1 && fireCam && Readable((void*)(fireCam + CAM_POV_ROT), 12)) {
                int32_t* r = reinterpret_cast<int32_t*>(fireCam + CAM_POV_ROT);
                savedPov[0] = r[0]; savedPov[1] = r[1]; savedPov[2] = r[2];
                if (!nullWindow) { r[0] = hp; r[1] = hy; }
                havePov = true;
            }

            // ---- run 98: and every OTHER field that tracks the view rotation ----
            //
            // The three above were chosen by reasoning about UE3 and all three were wrong. These
            // are chosen by measurement: fields that followed the view across four head
            // orientations at least 5 degrees apart. If the aim is read from a rotation stored
            // anywhere on the controller or the camera, it is in this list.
            //
            // ⚠️ Brute force, and worth being honest that it is. A field that tracks the view but
            // is not the aim gets written too. That is acceptable ONLY because the write lasts one
            // call and is restored from what was read - the same unwind the nesting already proved
            // correct. It is a diagnostic that happens to also be the fix if it works.
            if (InterlockedCompareExchange(&g_rotScanDone, 0, 0)) {
                for (int i = 0; i < g_ctlRotCount && nExtra < kExtraMax; ++i) {
                    int32_t* r = reinterpret_cast<int32_t*>(fireCtl + g_ctlRotOffs[i]);
                    if (!fireCtl) break;
                    extraAddr[nExtra] = r;
                    extraSaved[nExtra][0] = r[0]; extraSaved[nExtra][1] = r[1];
                    if (!nullWindow) { r[0] = hp; r[1] = hy; }
                    ++nExtra;
                }
                // Camera fields only at set 2. This is the loop that moved the view: one of these
                // is very likely a desired-rotation the engine interpolates toward.
                for (int i = 0; g_aimFieldSet >= 2 && i < g_camRotCount && nExtra < kExtraMax; ++i) {
                    if (!fireCam) break;
                    int32_t* r = reinterpret_cast<int32_t*>(fireCam + g_camRotOffs[i]);
                    extraAddr[nExtra] = r;
                    extraSaved[nExtra][0] = r[0]; extraSaved[nExtra][1] = r[1];
                    if (!nullWindow) { r[0] = hp; r[1] = hy; }
                    ++nExtra;
                }
            }
            firedWindow = haveCtl || haveCam || havePov || nExtra > 0;
            // Mark the window OPEN before anything else can observe the swapped fields.
            if (firedWindow) { InterlockedIncrement(&g_fireDepth); InterlockedIncrement(&g_fireSeq); }
            // ---- run 106: count traces ACROSS the window, which is unambiguous ----
            // The near-camera filter can only report traces it recognises. This cannot miss:
            // if the shot traces at all, the counter moves between open and close.
            lcAtOpen = InterlockedCompareExchange(&g_lineCheckCalls, 0, 0);

            // Cap raised to 24: run 96 capped at 6 and spent them all on StartFire/BeginFire, so
            // the log could not show whether FireAmmunition - the one that actually matters - ever
            // opened the window at all.
            static volatile LONG shots = 0;
            if (InterlockedIncrement(&shots) <= 24) {
                char nm[96]{};
                NameFromIndex(fnIdx, nm, sizeof(nm));
                Log("aim mode 9: %-16s hand(p %d y %d) fieldSet %d | ctl %s was(p %d y %d) | cam %s was(p %d y %d)"
                    " | POV %s was(p %d y %d) | +%d scanned fields",
                    nm, hp, hy, (int)g_aimFieldSet,
                    haveCtl ? "SET" : "--", savedCtl[0], savedCtl[1],
                    haveCam ? "SET" : "--", savedCam[0], savedCam[1],
                    havePov ? "SET" : "--", savedPov[0], savedPov[1], nExtra);
            }
        }
    }

    const bool mode8 = (aimMode == 8) && fnIdx >= 0;
    bool openedWindow = false, maskedHere = false;
    int32_t savedPitch = 0, savedYaw = 0;
    uintptr_t ctlNow = 0;

    if (mode8) {
        ctlNow = (uintptr_t)InterlockedCompareExchange(&g_ctlForXi, 0, 0);
        if (ctlNow && Readable((void*)(ctlNow + ACTOR_ROTATION), 12)) {
            int32_t* rot = reinterpret_cast<int32_t*>(ctlNow + ACTOR_ROTATION);
            if (fnIdx == FnIdx(kFnPlayerTick)) {
                savedPitch = rot[0]; savedYaw = rot[1];
                rot[0] = InterlockedCompareExchange(&g_aimPitchForXi, 0, 0);
                rot[1] = InterlockedCompareExchange(&g_aimYawForXi, 0, 0);
                InterlockedIncrement(&g_tickDepth);
                openedWindow = true;
            } else if (InterlockedCompareExchange(&g_tickDepth, 0, 0) > 0) {
                for (int m = kFnMaskFirst; m <= kFnMaskLast; ++m) {
                    if (FnIdx(m) >= 0 && fnIdx == FnIdx(m)) {
                        savedPitch = rot[0]; savedYaw = rot[1];
                        rot[0] = g_wantPitch; rot[1] = g_wantYaw;   // the HEAD, for this call only
                        maskedHere = true;
                        break;
                    }
                }
                // The nesting census: which functions run while the hand is in the field. If the
                // view still moves, the answer is in this list and no further run is needed to
                // find it.
                if (!maskedHere) NoteNested(fnIdx);
            }
        }
    }

    g_origProcessEvent(self, edx, Stack, Result);

    // Mode 9's window closes here. Restoring the value that was READ rather than a cached head
    // means a nested fire function (StartFire -> BeginFire -> FireAmmunition) puts back the hand
    // for the outer call and the head only when the outermost one returns - no depth counter
    // needed, and no way for an unbalanced nesting to leave the hand stuck in the field.
    if (firedWindow) {
        // Restore what was READ at each level, so nested fire calls (StartFire -> BeginFire ->
        // FireAmmunition) unwind correctly and the head only comes back when the outermost
        // returns. Confirmed working in run 96's log.
        if (haveCtl) {
            int32_t* r = reinterpret_cast<int32_t*>(fireCtl + ACTOR_ROTATION);
            r[0] = savedCtl[0]; r[1] = savedCtl[1]; r[2] = savedCtl[2];
        }
        if (haveCam) {
            int32_t* r = reinterpret_cast<int32_t*>(fireCam + ACTOR_ROTATION);
            r[0] = savedCam[0]; r[1] = savedCam[1]; r[2] = savedCam[2];
        }
        if (havePov) {
            int32_t* r = reinterpret_cast<int32_t*>(fireCam + CAM_POV_ROT);
            r[0] = savedPov[0]; r[1] = savedPov[1]; r[2] = savedPov[2];
        }
        // Unwind the scanned fields last-first, so an offset that appears twice (the three named
        // fields above can also be in the scan list) ends up holding the value it started with
        // rather than an intermediate one.
        for (int i = nExtra - 1; i >= 0; --i) {
            extraAddr[i][0] = extraSaved[i][0];
            extraAddr[i][1] = extraSaved[i][1];
        }
        InterlockedDecrement(&g_fireDepth);
        InterlockedIncrement(&g_fireSeq);   // seq moves on every open AND close, for the race check
        // How many line checks ran inside this window. A weapon that hitscans MUST move this;
        // zero across every shot means the damage path does not use 0x00BD79D0 at all, and that
        // is a fact rather than the absence of a log line I was hoping for.
        {
            const LONG delta = InterlockedCompareExchange(&g_lineCheckCalls, 0, 0) - lcAtOpen;
            static volatile LONG saidIt = 0;
            if (InterlockedIncrement(&saidIt) <= 12)
                Log("  window closed: %ld line check(s) ran inside it", delta);
        }
    }
    if (mode9) return;

    if ((openedWindow || maskedHere) && ctlNow && Readable((void*)(ctlNow + ACTOR_ROTATION), 12)) {
        int32_t* rot = reinterpret_cast<int32_t*>(ctlNow + ACTOR_ROTATION);
        rot[0] = savedPitch; rot[1] = savedYaw;
        if (openedWindow) InterlockedDecrement(&g_tickDepth);
    }
    if (mode8) return;

    if (!wantAim) return;

    // ---- the result buffer, still decided by the DATA and not by the ABI ----
    //
    // With the signature corrected there is only ONE candidate: `Result`, the second stack
    // argument of ProcessInternal(FFrame&, RESULT_DECL). The two-candidate search is gone with the
    // third argument that never existed.
    //
    // The proof-of-itself gate stays, because it is not about which argument - it is about whether
    // writing 12 bytes through this pointer is safe at all. GetAdjustedAim has just returned the
    // player's aim, which for a human player is Controller.Rotation: the value this mod itself
    // wrote as g_wantYaw a few milliseconds ago. If the yaw in that buffer is within a few degrees
    // of g_wantYaw it is the return value, because nothing else in memory has reason to hold that
    // number. If it is not, NOTHING IS WRITTEN and the mismatch is logged.
    //
    // ⚠️ This is what stops the test being fooled. A hook that wrote unconditionally and produced
    // no visible change would be indistinguishable from GetAdjustedAim not being on the fire path -
    // the run 82 failure mode. Here the two look completely different in the log.
    const int32_t kUUPerDeg = (int32_t)(65536.0f / 360.0f);
    static volatile LONG reported = 0;

    if (Result && Readable(Result, 12)) {
        int32_t* rot = reinterpret_cast<int32_t*>(Result);
        const int32_t dYaw = rot[1] - g_wantYaw;           // int32 wrap is correct in FRotator space
        if (dYaw <= 8 * kUUPerDeg && dYaw >= -8 * kUUPerDeg) {
            const int32_t wasPitch = rot[0], wasYaw = rot[1];
            rot[0] = InterlockedCompareExchange(&g_aimPitchForXi, 0, 0);
            rot[1] = InterlockedCompareExchange(&g_aimYawForXi, 0, 0);
            if (InterlockedIncrement(&reported) <= 3)
                Log("aim mode 6: result buffer confirmed - was pitch %d yaw %d (g_wantYaw %d),"
                    " rewrote to hand pitch %d yaw %d",
                    wasPitch, wasYaw, g_wantYaw, rot[0], rot[1]);
            return;
        }
    }

    // The buffer did not hold the aim. Say so, loudly and once - a silent no-op here would read
    // exactly like the feature working and the fire trace ignoring it.
    if (InterlockedIncrement(&reported) <= 3) {
        const int32_t y = (Result && Readable(Result, 12)) ? reinterpret_cast<int32_t*>(Result)[1] : 0;
        Log("aim mode 6: *** GetAdjustedAim ran but the result buffer does not hold the aim."
            " yaw there %d, g_wantYaw %d. Nothing was written. Either the return comes back some"
            " other way, or the name matched a different GetAdjustedAim.", y, g_wantYaw);
    }
}

void DumpProcessEventCensus() {
    const LONG total = InterlockedCompareExchange(&g_peCalls, 0, 0);
    const LONG firing = InterlockedCompareExchange(&g_peFiringCalls, 0, 0);
    Log("=== script calls seen: %ld total, %ld of them while FIRING ===", total, firing);
    if (firing == 0) {
        // Plain ASCII: this is a string literal, not a comment, and the build is code page 1252.
        Log("  *** ZERO firing samples. The split never happened, so an empty table below means"
            " NOTHING - it cannot be told apart from the fire path using no script at all. Fire"
            " with the right trigger or the mouse while the census is armed and run it again.");
    }
    // ---- ⚠️ run 83: the previous filter WAS the empty result ----
    //
    // It showed only names with idle == 0, or with firing >= idle. Every function that runs during
    // a shot but also runs the rest of the time - which is most of them, Tick included - was
    // silently dropped, and with 1067 firing samples against 5168 idle ones the ratio test threw
    // away nearly everything else. The dump printed "(nothing)" from a table that was full.
    //
    // That is the same defect as the button probe and the culling flicker: **a filter that encodes
    // the hypothesis cannot report against it.** So print the raw table, ranked, and let it say
    // what it says. A genuine negative here - nothing enriched during firing - is a real and
    // useful answer, and it now looks different from a bug.
    int total_names = 0;
    for (int i = 0; i < kPeSlots; ++i) if (g_peSlots[i].firing || g_peSlots[i].idle) ++total_names;

    // ---- ⭐ run 91: the CONTROL, and the thing that decides whether this table means anything ----
    //
    // The hook is at 0x01308A10, which is ProcessInternal or an opcode handler - the two have never
    // been told apart. If it is an opcode handler, this census can fill with perfectly real,
    // perfectly resolvable names and still never see the fire path, which reads IDENTICALLY to
    // "the fire path is native". That ambiguity is the run 82 failure and it is not detectable by
    // looking at the table.
    //
    // `Tick` settles it. Every UE3 level has actors ticking every frame, so if this hook is on the
    // general script path Tick MUST be here in the thousands. If it is absent, the hook is on some
    // narrower path and an empty fire-path result proves nothing at all.
    //
    // The five aim functions are printed by name for the same reason: a specific expected name
    // present-or-absent is a real answer, while "top 40 by enrichment" needs interpreting.
    {
        const LONG tickIdx = FnIdx(kFnTick);
        uint32_t tickSeen = 0;
        if (tickIdx >= 0)
            for (int i = 0; i < kPeSlots; ++i)
                if (g_peSlots[i].nameIdx == tickIdx) tickSeen = g_peSlots[i].idle + g_peSlots[i].firing;

        // ---- run 92: the name source's own hit rate, printed BEFORE the control ----
        //
        // This has to come first because it decides whether the control below is even readable. If
        // the name source is failing, Tick would read zero for a reason that has nothing to do with
        // which function the hook sits on, and the control would be misinterpreted as proving the
        // hook is blind.
        {
            const LONG seen = InterlockedCompareExchange(&g_peSeen, 0, 0);
            const LONG unnamed = InterlockedCompareExchange(&g_peUnnamed, 0, 0);
            const double pct = seen ? (100.0 * unnamed / (double)seen) : 0.0;
            Log("  --- NAME SOURCE: %ld calls seen, %ld could not be named (%.1f%%) ---",
                seen, unnamed, pct);
            if (seen == 0)
                Log("      *** the hook was never called while armed. Nothing below means anything.");
            else if (pct > 50.0)
                Log("      *** MOST CALLS CANNOT BE NAMED. FFrame::Node is NOT reliably at +0x%02X"
                    " in this build, so the census could never have found any function by name and"
                    " neither could aim mode 6 or 7. Fix the offset before reading anything else"
                    " below - the Tick control included.", (int)InterlockedCompareExchange(&g_nodeOffset, 0, 0));
            else
                Log("      name source is working - a low miss rate is normal (native calls and"
                    " frames still being set up). The control below is readable.");
        }

        Log("  --- CONTROL: Tick seen %u times ---", tickSeen);
        if (tickIdx < 0)
            Log("      *** 'Tick' never resolved in GNames, so this control could not run. Press"
                " NUMPAD7 again in gameplay. Do NOT read the table below as an answer.");
        else if (tickSeen == 0)
            Log("      *** ZERO. This hook is NOT on the general script path - it is an opcode"
                " handler or something narrower. An empty fire-path result below would mean"
                " NOTHING. Finding the real ProcessEvent is the prerequisite.");
        else
            Log("      hook is on the general script path - the table below is meaningful.");

        Log("  --- the five aim functions named out of RvGame.xxx (run 91) ---");
        for (int f = 0; f < kScriptFnCount; ++f) {
            if (f == kFnTick) continue;
            const LONG want = FnIdx(f);
            uint32_t fnIdle = 0, fnFiring = 0;   // not `firing` - that is this function's TOTAL,
            bool seen = false;                   // and shadowing it here would be a trap
            if (want >= 0)
                for (int i = 0; i < kPeSlots; ++i)
                    if (g_peSlots[i].nameIdx == want && (g_peSlots[i].idle || g_peSlots[i].firing)) {
                        fnIdle = g_peSlots[i].idle; fnFiring = g_peSlots[i].firing; seen = true;
                    }
            Log("      %-22s %s  firing %-6u idle %-6u", g_scriptFns[f].name,
                want < 0 ? "(name unresolved)" : (seen ? "SEEN   " : "absent "), fnFiring, fnIdle);
        }
    }
    Log("  %d distinct script functions seen. Top 40 by FIRING count, enrichment = firing share"
        " vs this function's total:", total_names);

    bool used[kPeSlots]{};
    for (int rank = 0; rank < 40; ++rank) {
        int best = -1;
        for (int i = 0; i < kPeSlots; ++i) {
            if (used[i] || !g_peSlots[i].firing) continue;
            if (best < 0 || g_peSlots[i].firing > g_peSlots[best].firing) best = i;
        }
        if (best < 0) break;
        used[best] = true;
        const PeSlot& s = g_peSlots[best];
        char nm[96]{};
        // Unresolvable names are PRINTED as their index rather than skipped - a name that will not
        // resolve is itself a finding, and skipping it would be another silent filter.
        if (!NameFromIndex(s.nameIdx, nm, sizeof(nm)))
            _snprintf_s(nm, sizeof(nm), _TRUNCATE, "<unresolved name idx %d>", s.nameIdx);
        const double share = 100.0 * s.firing / (double)(s.firing + s.idle);
        Log("    %-44s firing %-6u idle %-6u  %5.1f%% under fire%s",
            nm, s.firing, s.idle, share, s.idle == 0 ? "   <== ONLY WHILE FIRING" : "");
    }
    if (total_names == 0)
        Log("    (table is genuinely empty - the census was not armed, or ProcessEvent is not"
            " being called at all)");
}

// See the forward declaration by the XInput hook for why this exists and what it tests.
void WriteAimAtInputPoll() {
    if (InterlockedCompareExchange(&g_aimMode, 0, 0) != 5) return;
    const uintptr_t ctl = (uintptr_t)InterlockedCompareExchange(&g_ctlForXi, 0, 0);
    if (!ctl || !Readable((void*)(ctl + ACTOR_ROTATION), 12)) return;
    int32_t* r = reinterpret_cast<int32_t*>(ctl + ACTOR_ROTATION);
    r[0] = InterlockedCompareExchange(&g_aimPitchForXi, 0, 0);
    r[1] = InterlockedCompareExchange(&g_aimYawForXi, 0, 0);
}

// ---- ❌ run 94: nothing is hooked at startup any more ----
//
// This used to detour 0x01308A10 on every launch. Two reasons it is gone rather than disabled:
//
// 1. **It is the wrong function**, proven by measurement in run 92 - `Tick` appeared ZERO times in
//    48,811 calls through it, and every level ticks every actor every frame.
// 2. **It would now CRASH.** 0x01308A10 ends in `ret 0xC`; the hook is written for
//    ProcessInternal's `ret 8`. Leaving a disproven hook wired to a corrected signature is exactly
//    how a fix for one crash becomes the cause of the next.
//
// The only script hook installed now is the one RepointScriptHook puts on the address the
// UFunction::Func probe finds, which is discovered at runtime and checked for being executable
// code before anything is written to it.
void InstallProcessEventHook() {
    Log("script hook: NOT installed at startup. 0x%08X was disproven in run 92 (Tick seen 0 times"
        " in 48,811 calls) and its `ret 0xC` does not match ProcessInternal's `ret 8`."
        " The real one is found through UFunction::Func when aim mode 6 or 7 is selected.",
        kProcessEventAddr);
}

void InstallViewHook() {
    // ALREADY_INITIALIZED is the normal case now: InstallAutoResolution runs in DllMain and gets
    // there first. Treating it as a failure here would silently disable head tracking.
    const MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MH_Initialize failed (%d)", (int)mhInit); return;
    }
    void* target = reinterpret_cast<void*>(kCalcViewRotationAddr);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&Hook_CalcViewRotation),
                      reinterpret_cast<void**>(&g_origCalcViewRotation)) != MH_OK) {
        Log("MH_CreateHook failed at 0x%08X", kCalcViewRotationAddr); return;
    }
    if (MH_EnableHook(target) != MH_OK) { Log("MH_EnableHook failed"); return; }
    Log("view rotation detour installed at 0x%08X", kCalcViewRotationAddr);
}

// ================================================================ aim decoupling: where it stands
//
// `PlayerController.Rotation` is read by the view, the culling frustum, LOD, draw distance, the
// movement basis and the weapon's fire trace. Run 80 established there is no second field to
// redirect: +0x05D4 deflects nothing, and UE3's own design agrees - a player's aim comes from
// `Pawn.GetBaseAimRotation()`, which returns that same Rotation. **Aim and culling are one value.**
//
// So they can only be separated in TIME or in the RENDERER, and exactly two routes remain:
//
// 1. **Widen the culling frustum until the deviation fits inside it.** Mode 1 aims the engine at
//    the hand and `askDeg` - which governs culling only, because ApplyProjection forces the
//    rendered frustum separately - is inflated to cover the gap. This is NOT the engine being
//    wrong about where you are looking; it is the engine being CONSERVATIVE, drawing more than it
//    needs. The bill is LOD and draw distance (run 21), and it is one test away: PAGE UP now
//    reaches 4x and 6x and neither has been tried.
//
// 2. **Hook `UObject::ProcessEvent` and swap the rotation around the aim call.** Every UnrealScript
//    call in UE3 passes through this one function with its UFunction, and this mod already
//    resolves script names against GNames. Set Rotation to the hand on the way in to the fire or
//    aim function, restore the head on the way out, and the separation is exact - culling never
//    sees the hand at all. This is the principled fix and it is the standard UE3 modding seam,
//    not an exotic one. It is also a genuine reverse-engineering job with no guarantee of a
//    convenient hook point, which is why route 1 gets tested first.
//
// Route 1 is one session and might end this. Route 2 is the fallback and the better answer if it
// does not.

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

// ================= ⭐ run 127: an on-screen readout, in WORDS ===============================
//
// Every mode and setting in this mod goes to a log file, so the only way to check state while
// wearing the headset has been to take it off. That gap was named several runs ago and left alone,
// and it has since cost a run - "combo 2" meant different things in two sessions and nothing on
// screen could have said so.
//
// The first attempt spelled the state in binary squares. That was a bad answer to a good request:
// it moved the decoding from the log into the reader's head. So this draws actual text.
//
// No font is available - the Windows SDK does not ship D3DX - so the glyphs are a 5x7 bitmap and
// each row of set pixels becomes one rectangle in a Clear() list. Runs of adjacent pixels are
// merged into a single rectangle, which keeps a full line of text to a few dozen rectangles rather
// than one per pixel. No vertex buffer, no shader, and the only device state touched is the
// scissor, saved and restored.
//
// Drawn at the top of Present, before the frame is copied to the headset, which is what puts it in
// front of your eyes rather than only on the desktop mirror.
const int kGlyphW = 5, kGlyphH = 7;
static const unsigned char kFont[][kGlyphH] = {
    {0,0,0,0,0,0,0},                                    // space
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},               // A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},               // B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},               // C
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},               // D
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},               // E
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},               // F
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},               // G
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},               // H
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},               // I
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},               // J
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},               // K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},               // L
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},               // M
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11},               // N
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},               // O
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},               // P
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},               // Q
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},               // R
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},               // S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},               // T
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},               // U
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},               // V
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},               // W
    {0x11,0x0A,0x04,0x04,0x04,0x0A,0x11},               // X
    {0x11,0x0A,0x04,0x04,0x04,0x04,0x04},               // Y
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},               // Z
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},               // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},               // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},               // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},               // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},               // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},               // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},               // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},               // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},               // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},               // 9
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},               // +
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},               // -
    {0x02,0x04,0x04,0x04,0x04,0x04,0x02},               // ( 
    {0x08,0x04,0x04,0x04,0x04,0x04,0x08},               // )
};

static int GlyphIndex(char ch) {
    if (ch == ' ') return 0;
    if (ch >= 'A' && ch <= 'Z') return 1 + (ch - 'A');
    if (ch >= 'a' && ch <= 'z') return 1 + (ch - 'a');
    if (ch >= '0' && ch <= '9') return 27 + (ch - '0');
    if (ch == '+') return 37;
    if (ch == '-') return 38;
    if (ch == '(') return 39;
    if (ch == ')') return 40;
    return 0;
}

// Append the rectangles for one line of text. Adjacent set pixels in a row are merged, so "COMBO"
// costs about twenty rectangles rather than seventy.
static int TextRects(D3DRECT* r, int n, int cap, int x0, int y0, int px, const char* s) {
    for (int ci = 0; s[ci]; ++ci) {
        const unsigned char* g = kFont[GlyphIndex(s[ci])];
        const int cx = x0 + ci * (kGlyphW + 1) * px;
        for (int row = 0; row < kGlyphH; ++row) {
            int col = 0;
            while (col < kGlyphW) {
                if (!(g[row] & (0x10 >> col))) { ++col; continue; }
                int run = 0;
                while (col + run < kGlyphW && (g[row] & (0x10 >> (col + run)))) ++run;
                if (n < cap) {
                    r[n].x1 = cx + col * px;
                    r[n].y1 = y0 + row * px;
                    r[n].x2 = cx + (col + run) * px;
                    r[n].y2 = y0 + (row + 1) * px;
                    ++n;
                }
                col += run;
            }
        }
    }
    return n;
}

// ---- ⭐ run 129: show the anchor where it actually IS ----
//
// The anchor is a point in space, and tuning it by watching what swings is inference. Drawing it
// makes it direct: put the marker on the grip and the pivot is right, with nothing to deduce.
//
// It is projected through the SAME cached view-projection the scene uses, with the SAME per-eye
// remap, so the marker lands where that point genuinely is rather than at an estimate of it. If
// the marker sits somewhere the gun is not, that is a real disagreement worth seeing.
//
// Returns false when the point is behind the eye - w <= 0 - because projecting that gives a
// mirrored position on screen, which would be a confident lie.
// ⚠️ The width here is the FULL target, not the eye half. ApplyEyeRemap has already compressed
// clip-space x into this eye's half - clip.x becomes 0.5*clip.x -+ 0.5*clip.w - so the NDC range
// coming out of it already IS that half. Mapping it through halfW as well squeezes the marker into
// a QUARTER of the screen, at a different wrong place in each eye, so the two never fuse and you
// see two crosses instead of one.
static bool ProjectToScreen(const float* mat, int conv, const float p[3],
                            LONG fullW, LONG h, LONG& sx, LONG& sy) {
    const Reg4* r = reinterpret_cast<const Reg4*>(mat);
    float cx, cy, cw;
    if (conv == CONV_ROW) {
        cx = p[0]*r[0].x + p[1]*r[1].x + p[2]*r[2].x + r[3].x;
        cy = p[0]*r[0].y + p[1]*r[1].y + p[2]*r[2].y + r[3].y;
        cw = p[0]*r[0].w + p[1]*r[1].w + p[2]*r[2].w + r[3].w;
    } else {
        cx = r[0].x*p[0] + r[0].y*p[1] + r[0].z*p[2] + r[0].w;
        cy = r[1].x*p[0] + r[1].y*p[1] + r[1].z*p[2] + r[1].w;
        cw = r[3].x*p[0] + r[3].y*p[1] + r[3].z*p[2] + r[3].w;
    }
    if (cw <= 0.001f) return false;
    const float ndcX = cx / cw, ndcY = cy / cw;
    if (ndcX < -1.5f || ndcX > 1.5f || ndcY < -1.5f || ndcY > 1.5f) return false;
    sx = (LONG)((ndcX * 0.5f + 0.5f) * fullW);
    sy = (LONG)((0.5f - ndcY * 0.5f) * h);
    return true;
}

// ---- run 130: draw BOTH ends of the transform ----
//
// Asked why the cross follows the head rather than the controller. It should: the marker is G, the
// gun's ORIGINAL anchor, and the engine draws the gun attached to the view - so G is head-relative
// by definition. H, the hand, is where the gun is moved TO. They are different points:
//
//     v' = (v - G) * R + G + (H - G)
//            pivot here          ends up here
//
// Showing only one of them made the transform look wrong when it was the instructions that were.
// So both are drawn: MAGENTA is the pivot G, CYAN is the destination H.
static int AnchorMarkerRects(D3DRECT* r, int n, int cap, bool wantHand) {
    float G[3]; GunAnchorWorld(G);
    if (wantHand) {
        G[0] = (float)InterlockedCompareExchange(&g_handOffX, 0, 0);
        G[1] = (float)InterlockedCompareExchange(&g_handOffY, 0, 0);
        G[2] = (float)(g_gunSignPosZ * InterlockedCompareExchange(&g_handOffZ, 0, 0));
    }

    int slot = -1;
    for (int i = 0; i < kMaxCamMats; ++i) if (g_camMats[i].valid) { slot = i; break; }
    if (slot < 0) return n;                      // no live matrix this frame - draw nothing

    const LONG arm = (LONG)(g_srcH / 90), th = (LONG)(g_srcH / 400) + 1;
    for (int eye = 0; eye < 2; ++eye) {
        float m[16];
        memcpy(m, g_camMats[slot].m, sizeof(m));
        ApplyEyeRemap(reinterpret_cast<Reg4*>(m), g_camMats[slot].conv, eye ? +0.5f : -0.5f);
        LONG sx = 0, sy = 0;
        if (!ProjectToScreen(m, g_camMats[slot].conv, G,
                             (LONG)g_srcW, (LONG)g_srcH, sx, sy)) continue;
        if (n + 2 > cap) break;
        r[n].x1 = sx - arm; r[n].y1 = sy - th; r[n].x2 = sx + arm; r[n].y2 = sy + th; ++n;
        r[n].x1 = sx - th;  r[n].y1 = sy - arm; r[n].x2 = sx + th; r[n].y2 = sy + arm; ++n;
    }
    return n;
}

static void DrawStateReadout(IDirect3DDevice9* dev) {
    if (!dev || !g_srcW || !g_srcH) return;
    const LONG combo = InterlockedCompareExchange(&g_gunSignCombo, 0, 0);
    const LONG mode  = InterlockedCompareExchange(&g_hideMeshIdx, 0, 0);
    const LONG aim   = InterlockedCompareExchange(&g_aimMode, 0, 0);

    int sy, sp, sr; GunSigns(sy, sp, sr);
    char l1[32], l2[32], l3[32];
    _snprintf_s(l1, sizeof(l1), _TRUNCATE, "COMBO %d  FRAME %s",
                (int)combo, GunViewFrame() ? "ON" : "OFF");
    _snprintf_s(l2, sizeof(l2), _TRUNCATE, "YAW %c PITCH %c ROLL %c",
                sy > 0 ? '+' : '-', sp > 0 ? '+' : '-', sr > 0 ? '+' : '-');
    const char* what = (mode == 1) ? "GUN" : (mode == 2) ? "ARMS"
                     : (mode == 3) ? "GUN ARMS" : "NONE";
    _snprintf_s(l3, sizeof(l3), _TRUNCATE, "AIM %d  MOVING %s  POS %s", (int)aim, what,
                g_gunFollowPos ? "ON" : "OFF");
    // ---- run 133: the two toggles a cutscene test turns on, in words ----
    //
    // NUMPAD8 and F1 had no on-screen state, so an A/B across them was counted keypresses and
    // hope: press it, look, press it again, and never learn whether the toggle registered at all.
    // In a headset the log is unreadable, so a toggle with no readout is a toggle whose failure
    // to fire is indistinguishable from the thing it controls having no effect.
    //
    // ROT is the one that matters for cutscenes. ENGINE means head yaw/pitch reach the view only
    // by being written into the PlayerController's Rotation, which a matinee-driven camera never
    // reads; MATRIX means the view-projection carries the rotation itself and the engine is not
    // consulted. Position and roll are matrix-side either way, which is exactly why they keep
    // working in a cutscene while looking around does not.
    // The mode number AND the two flags it stands for, deliberately both. The number is what the
    // test steps refer to; the flags are what it means, so a mode read off the screen never has to
    // be looked up to be understood.
    char l5[32], l6[32];
    // PROJ is F6 - whether the VR frustum is being forced into the matrix at all. It belongs next
    // to FOV and CULL: run 134 eliminated engine-side culling across a 0.35x-6x sweep of what the
    // engine is ASKED for, which leaves what we FORCE as the remaining difference between a
    // rendered FOV that shows the fires and one that does not. Testing that without being able to
    // see its state is how the last three runs went wrong.
    _snprintf_s(l5, sizeof(l5), _TRUNCATE, "MODE %d  ROT %s  PROJ %s", CutsceneMode(),
                InterlockedCompareExchange(&g_matrixRot, 0, 0) ? "MATRIX" : "ENGINE",
                InterlockedCompareExchange(&g_vrProjection, 0, 0) ? "ON" : "OFF");
    // CINE is the candidate cutscene detector, shown but not acting. Watching it agree with what
    // is on screen IS the test - if it says ON through the opening and OFF through play, it can be
    // trusted to drive the mode automatically; if it never moves, SetCinematicMode is not the
    // signal Raven uses and the fallbacks in the watch table are the next thing to read.
    // ---- ⭐ run 133: the FOV knobs, on screen ----
    //
    // Run 133 produced seven screenshots "at different PAGE DOWN levels" and not one of them
    // recorded which level it was at, so the set could not be read as a comparison at all. Both
    // knobs are integer percentages here: FOV is how much of the headset's field is rendered
    // (PAGE DOWN), CULL is the extra field the engine is asked to cull against (PAGE UP). The
    // font has no '.', so a decimal would render as a space and read as two numbers.
    // ---- ⭐ run 135: per-eye resolution on screen ----
    //
    // Run 135 reported "more culling at 1440p than at full headset resolution", which makes the
    // backbuffer size an experimental variable rather than a setting - and one that no screenshot
    // records. Per EYE rather than the whole target, because that is the number an object's pixel
    // coverage actually scales with under duplication.
    // ---- ⭐ run 136: the draw count, live ----
    //
    // Shadows are out - a shadow failure cannot delete a particle system, and the fire goes with
    // the floor. Rather than propose a fifth mechanism, split the space: DRAWS says whether the
    // engine is still ISSUING the calls.
    //
    // Read at the top of Present, so this is the count for the frame that just finished - the
    // per-frame reset happens at the bottom. Narrowing the FOV should LOWER it, because less is in
    // view. So if the fires come back as FOV narrows and this count goes DOWN with them, they were
    // being drawn all along and we are losing them after the draw call. If it goes UP, the engine
    // was withholding them and the cause is engine-side after all.
    // The alignment aid. Shows the LIVE camera yaw while unfrozen, so the second run can be turned
    // until it reads what the first was frozen at, and the frozen value once pinned. Wrapped to
    // 0-359 because a raw FRotator wraps through negatives and reads as two different numbers for
    // the same direction.
    char l9[40];
    {
        const bool frozen = InterlockedCompareExchange(&g_freezeView, 0, 0) != 0;
        const int32_t uu = frozen ? g_frozenYaw : (g_camRotKnown ? g_camYawUU : g_wantYaw);
        int deg = (int)(uu * (360.0 / 65536.0)) % 360;
        if (deg < 0) deg += 360;
        _snprintf_s(l9, sizeof(l9), _TRUNCATE, "FREEZE %s  YAW %d", frozen ? "ON" : "OFF", deg);
    }

    char l8[40];
    _snprintf_s(l8, sizeof(l8), _TRUNCATE, "EYE %uX%u  DRAWS %d",
                InterlockedCompareExchange(&g_dupDraws, 0, 0) ? g_srcW / 2 : g_srcW, g_srcH,
                g_drawsTotal);

    char l7[40];
    const LONG heldPct = InterlockedCompareExchange(&g_yawHeldPct, 0, 0);
    if (heldPct < 0)
        _snprintf_s(l7, sizeof(l7), _TRUNCATE, "FOV %d  CULL %d  HELD NONE",
                    (int)(g_renderFovScale * 100.0f + 0.5f), (int)(kFovHeadroom * 100.0f + 0.5f));
    else
        _snprintf_s(l7, sizeof(l7), _TRUNCATE, "FOV %d  CULL %d  HELD %ld",
                    (int)(g_renderFovScale * 100.0f + 0.5f), (int)(kFovHeadroom * 100.0f + 0.5f),
                    heldPct);

    _snprintf_s(l6, sizeof(l6), _TRUNCATE, "FOLD %s  STEREO %s  CINE %s",
                InterlockedCompareExchange(&g_yawFoldIn, 0, 0) ? "ON" : "OFF",
                InterlockedCompareExchange(&g_dupDraws, 0, 0) ? "ON" : "OFF",
                InterlockedCompareExchange(&g_inCinematic, 0, 0) ? "YES" : "NO");

    const LONG ax = InterlockedCompareExchange(&g_anchorAxis, 0, 0);
    char l4[40];
    // ⚠️ Brackets, not a leading dash. The dash used to mark the selected axis and it read as a
    // MINUS SIGN - "-R9" looks exactly like "R is -9" and was reported that way. A readout whose
    // marker is indistinguishable from its data is worse than no marker.
    _snprintf_s(l4, sizeof(l4), _TRUNCATE, "ANCHOR %sF%ld%s %sR%ld%s %sU%ld%s",
                ax == 0 ? "(" : " ", g_gunAnchorFwd,   ax == 0 ? ")" : " ",
                ax == 1 ? "(" : " ", g_gunAnchorRight, ax == 1 ? ")" : " ",
                ax == 2 ? "(" : " ", g_gunAnchorUp,    ax == 2 ? ")" : " ");

    DWORD oldScissor = FALSE;
    dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &oldScissor);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

    const LONG halfW = (LONG)g_srcW / 2;
    const int  px = (int)(g_srcH / 260);            // pixel size, scaled to the target
    const int  lh = (kGlyphH + 2) * px;
    const int  y0 = (int)(g_srcH / 7);

    // ---- ⚠️ this cap is a CORRECTNESS bound, not a budget ----
    //
    // At 1200 it was neither. TextRects drops silently once it is reached, and four lines across
    // two eyes overran it partway through the right eye's last line - which is why the anchor
    // readout appeared as "ANCHOR (F27) R9 U-13" in the left eye and "AN" in the right. That read
    // as a text-clipping quirk for a whole run when it was the readout running out of rectangles,
    // and the next line added would have been invisible in the right eye entirely.
    //
    // So size it so overrun is impossible rather than unlikely: a 5x7 glyph has at most three runs
    // of set pixels per row (10101), so 21 rectangles per character is the true worst case, and
    // the line buffers bound the character count. 9 lines x 39 chars x 2 eyes x 21 = 14742.
    const int kCap = 16384;
    static D3DRECT r[kCap];
    int n = 0;
    for (int eye = 0; eye < 2; ++eye) {
        const int x0 = (int)(eye * halfW + halfW / 2) - 11 * (kGlyphW + 1) * px;
        n = TextRects(r, n, kCap, x0, y0,          px, l1);
        n = TextRects(r, n, kCap, x0, y0 + lh,     px, l2);
        n = TextRects(r, n, kCap, x0, y0 + 7 * lh, px, l3);
        n = TextRects(r, n, kCap, x0, y0 + 8 * lh, px, l4);
        n = TextRects(r, n, kCap, x0, y0 + 4 * lh, px, l7);
        n = TextRects(r, n, kCap, x0, y0 + 5 * lh, px, l8);
        n = TextRects(r, n, kCap, x0, y0 + 6 * lh, px, l9);
        // The two cutscene lines go THIRD and FOURTH on screen, above the gun-tuning ones: they
        // are what a cutscene test reads, and burying them under the anchor numbers is how a
        // readout gets ignored.
        n = TextRects(r, n, kCap, x0, y0 + 2 * lh, px, l5);
        n = TextRects(r, n, kCap, x0, y0 + 3 * lh, px, l6);
    }
    // Loud, not silent. If the bound above is ever wrong the readout must say so rather than
    // quietly truncate - that failure mode already cost one run.
    if (n >= kCap) {
        static bool warned = false;
        if (!warned) { warned = true; Log("*** state readout hit its %d-rect cap - text is being"
                                          " TRUNCATED, raise kCap ***", kCap); }
    }
    if (n) dev->Clear((DWORD)n, r, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 255, 90), 0.0f, 0);

    // MAGENTA: the pivot G, which is head-relative because the gun hangs off the view.
    int mn = AnchorMarkerRects(r, 0, kCap, false);
    if (mn) dev->Clear((DWORD)mn, r, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 200), 0.0f, 0);
    // CYAN: the destination H, which is where your controller is.
    mn = AnchorMarkerRects(r, 0, kCap, true);
    if (mn) dev->Clear((DWORD)mn, r, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 220, 255), 0.0f, 0);

    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, oldScissor);
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* s, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    // Whoever calls Present owns the device. Recorded so the user-pointer draw hooks can tell
    // render-thread calls from movie-playback-thread ones and stay completely inert on the latter.
    g_mainThreadId = GetCurrentThreadId();
    if (InterlockedCompareExchange(&g_readoutOn, 0, 0)) DrawStateReadout(s);
    if (InterlockedExchange(&g_initTried, 1) == 0) {
        InstallViewHook(); InstallProcessEventHook(); InitXR();
    }

    // Retry until it takes. DllMain gets first refusal, but XINPUT1_3.dll is a sibling static
    // import and the loader owes us no ordering, so the first attempt can legitimately find it
    // absent. Returns true once there is nothing left to do, including the failure cases - a
    // retry loop that never terminates on failure would log once per frame forever.
    static bool xiDone = false;
    if (!xiDone) xiDone = InstallXInputHook();

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

    // NUMPAD0: drop draws to the scene-sized non-colour target. See the note in StereoPair.
    static bool pNum0 = false;
    bool kNum0 = (GetAsyncKeyState(VK_NUMPAD0) & 0x8000) != 0;
    if (kNum0 && !pNum0) {
        LONG was = InterlockedCompareExchange(&g_skipExtraTarget, 0, 0);
        InterlockedExchange(&g_skipExtraTarget, was ? 0 : 1);
        Log("NUMPAD0: draws to the scene-sized non-colour target %s",
            was ? "restored" : "DROPPED - if the decal artefact disappears, that surface is the pass");
    }
    pNum0 = kNum0;

    // NUMPAD1 / NUMPAD2: head roll on-off and its sign. On the numpad rather than an F-key
    // because F1-F12 are all spoken for, and NUMPAD0 already set the precedent.
    //
    // The sign flip is not laziness - F4 and F5 exist for exactly this reason on pitch and yaw.
    // Handedness between OpenXR's frame and the game's has been guessed wrong on both of the
    // other two axes, and a wrong roll sign looks plausible until you tilt far enough to notice
    // the horizon going the wrong way. One keypress settles it in the headset.
    static bool pNum1 = false, pNum2 = false;
    bool kNum1 = (GetAsyncKeyState(VK_NUMPAD1) & 0x8000) != 0;
    bool kNum2 = (GetAsyncKeyState(VK_NUMPAD2) & 0x8000) != 0;
    if (kNum1 && !pNum1) {
        LONG was = InterlockedCompareExchange(&g_rollOn, 0, 0);
        InterlockedExchange(&g_rollOn, was ? 0 : 1);
        Log("NUMPAD1: head roll %s", was ? "OFF (horizon locked to the headset, as before)" : "ON");
    }
    if (kNum2 && !pNum2) {
        g_rollSign = -g_rollSign;
        Log("NUMPAD2: roll sign %+d - tilt your head and check which way the horizon goes",
            g_rollSign);
    }
    pNum1 = kNum1; pNum2 = kNum2;

    // NUMPAD3 / NUMPAD4 / NUMPAD5: the gamepad probe. See the note above Hook_XInputGetState.
    //
    // Three keys rather than one because they answer three separate questions and the answers are
    // only useful apart. If NUMPAD3 alone changes the HUD prompts, the game has noticed the pad
    // without any stick moving - which is already most of what we need to know about the menus.
    static bool pNum3 = false, pNum4 = false, pNum5 = false;
    bool kNum3 = (GetAsyncKeyState(VK_NUMPAD3) & 0x8000) != 0;
    bool kNum4 = (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) != 0;
    bool kNum5 = (GetAsyncKeyState(VK_NUMPAD5) & 0x8000) != 0;
    if (kNum3 && !pNum3) {
        LONG was = InterlockedCompareExchange(&g_padEmu, 0, 0);
        InterlockedExchange(&g_padEmu, was ? 0 : 1);
        // A manual press takes the wheel. Otherwise auto-pad would undo it within a few frames and
        // the key would look broken.
        g_autoPad = 0;
        g_autoPadDisabled = false;
        Log("NUMPAD3 pressed - auto-pad is now OFF for this run, manual control only");
        // Sticks are cleared on the way out so the game cannot be left holding a phantom input
        // from a test that is no longer running.
        if (was) { InterlockedExchange(&g_padWalkTest, 0);
                   InterlockedExchange(&g_padLookTest, 0);
                   ClearPad(); }
        Log("NUMPAD3: gamepad emulation %s", was ? "OFF (real ports reported again)"
            : "ON - a 360 pad is now 'plugged in' on port 0. Watch for button prompts in the HUD"
              " and a highlighted item in the menus.");
    }
    if (kNum4 && !pNum4) {
        LONG was = InterlockedCompareExchange(&g_padWalkTest, 0, 0);
        InterlockedExchange(&g_padWalkTest, was ? 0 : 1);
        Log("NUMPAD4: walk test %s - the player should %s", was ? "OFF" : "ON",
            was ? "stop" : "walk FORWARD on his own. If he does, the whole gamepad route works.");
    }
    if (kNum5 && !pNum5) {
        LONG was = InterlockedCompareExchange(&g_padLookTest, 0, 0);
        InterlockedExchange(&g_padLookTest, was ? 0 : 1);
        Log("NUMPAD5: look test %s - the view should %s", was ? "OFF" : "ON",
            was ? "stop turning"
                : "turn RIGHT while head tracking still works. If it SNAPS BACK instead, the"
                  " run-12 fold-in does not cover engine-driven turning and that is the next bug.");
    }
    pNum3 = kNum3; pNum4 = kNum4; pNum5 = kNum5;

    // NUMPAD6: press the next 360 button and say which one it was. NUMPAD7: start over.
    //
    // One key, fourteen presses, and Raven's whole console layout is on the record - which is
    // cheaper than the third guess at it and cannot be wrong about the installed build.
    static bool pNum6 = false, pNum7 = false;
    bool kNum6 = (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) != 0;
    bool kNum7 = (GetAsyncKeyState(VK_NUMPAD7) & 0x8000) != 0;
    if (kNum6 && !pNum6) {
        // Named `btn`, not `b`: `b` is Hook_Present's third RECT parameter and is live right here.
        // The file already carries one comment about exactly this shadowing mistake.
        const XiButtonName& btn = kXiButtonNames[g_btnProbeIdx];
        g_btnProbeMask = btn.mask;
        g_btnProbeFrames = 8;
        Log("NUMPAD6: pressing >>> %s <<< (mask 0x%04X, %d of %d)"
            " - note what the game did, then press NUMPAD6 again for the next one",
            btn.name, (unsigned)btn.mask, g_btnProbeIdx + 1, kXiButtonNameCount);
        g_btnProbeIdx = (g_btnProbeIdx + 1) % kXiButtonNameCount;
        if (g_btnProbeIdx == 0) Log("NUMPAD6: that was the last button - wrapping back to A");
    }
    // NUMPAD7 no longer resets the button probe - it arms the script census now (see below). The
    // probe wraps back to A by itself, so nothing was lost.
    (void)kNum7;
    pNum6 = kNum6; pNum7 = kNum7;

    // NUMPAD8: move head yaw/pitch out of the engine's rotation and into the view matrix.
    static bool pNum8 = false;
    bool kNum8 = (GetAsyncKeyState(VK_NUMPAD8) & 0x8000) != 0;
    if (kNum8 && !pNum8) {
        const int next = (CutsceneMode() % 4) + 1;      // 1->2->3->4->1
        SetCutsceneMode(next);
        Log("NUMPAD8: cutscene mode %d - ROT %s, YAW FOLD %s. Deviation clamp is %d degrees"
            " - at 0 the matrix carries the WHOLE rotation. NUMPAD9 raises it.",
            next, (next == 2 || next == 3) ? "MATRIX (the view-projection carries the head)"
                                           : "ENGINE (the controller's Rotation carries it)",
            (next == 1 || next == 2) ? "ON (engine-side yaw changes absorbed as player input)"
                                     : "OFF (engine-side yaw changes ignored - mouse turning stops)",
            kDevSteps[InterlockedCompareExchange(&g_devStep, 0, 0)]);
    }
    pNum8 = kNum8;

    // NUMPAD9: how far the engine's camera may point away from the view. The measurement that
    // decides whether aim decoupling is reachable this way at all - see the note on kDevSteps.
    static bool pNum9 = false;
    bool kNum9 = (GetAsyncKeyState(VK_NUMPAD9) & 0x8000) != 0;
    if (kNum9 && !pNum9) {
        LONG next = (InterlockedCompareExchange(&g_devStep, 0, 0) + 1) % kDevStepCount;
        InterlockedExchange(&g_devStep, next);
        Log("NUMPAD9: deviation clamp %d degrees - turn your head that far off your body and watch"
            " for geometry vanishing at the edges. The largest value with NO popping is the answer.",
            kDevSteps[next]);
    }
    pNum9 = kNum9;

    // NUMPAD * : the flicker. The clamp cycles the angle; this is what makes the answer visible.
    static bool pMul = false;
    bool kMul = (GetAsyncKeyState(VK_MULTIPLY) & 0x8000) != 0;
    if (kMul && !pMul) {
        LONG was = InterlockedCompareExchange(&g_devFlicker, 0, 0);
        InterlockedExchange(&g_devFlicker, was ? 0 : 1);
        g_devDraws[0] = g_devDraws[1] = 0.0;
        g_devDrawFrames[0] = g_devDrawFrames[1] = 0;
        Log("NUMPAD*: culling flicker %s (alternating clamp 0 <-> %d degrees at ~4 Hz)",
            was ? "OFF" : "ON", kDevSteps[InterlockedCompareExchange(&g_devStep, 0, 0)]);
    }
    pMul = kMul;

    // NUMPAD / : turn ALL of this mod's XR input work off. The cheap control for run 67 - if the
    // judder survives this, it is not our input code and no amount of tuning it will help.
    static bool pDiv = false;
    bool kDiv = (GetAsyncKeyState(VK_DIVIDE) & 0x8000) != 0;
    if (kDiv && !pDiv) {
        LONG was = InterlockedCompareExchange(&g_xrInputOff, 0, 0);
        InterlockedExchange(&g_xrInputOff, was ? 0 : 1);
        Log("NUMPAD/: XR controller input %s - if the judder is unchanged either way, it is NOT"
            " this mod's input code and the runtime or the link is the place to look",
            was ? "back ON" : "OFF (no sync, no poses, no haptics)");
    }
    pDiv = kDiv;

    // NUMPAD - : freeze dwPacketNumber. Splits "the engine re-processes input on every change"
    // from "the engine costs this much simply because a pad exists". Controls stop responding
    // while it is on, by design.
    static bool pSub = false;
    bool kSub = (GetAsyncKeyState(VK_SUBTRACT) & 0x8000) != 0;
    if (kSub && !pSub) {
        LONG was = InterlockedCompareExchange(&g_freezePacket, 0, 0);
        InterlockedExchange(&g_freezePacket, was ? 0 : 1);
        Log("NUMPAD-: packet number %s - the pad stays connected and keeps reporting real sticks,"
            " but the engine is told nothing ever changes. Your controls WILL stop working."
            " If the frame rate recovers, the cost is UE3's input processing; if it does not,"
            " the cost is the pad merely existing (force feedback is the next suspect)",
            was ? "advancing normally again" : "FROZEN");
    }
    pSub = kSub;

    // NUMPAD + : neutralise UE3's frame-rate governor on the render thread.
    static bool pAdd = false;
    bool kAdd = (GetAsyncKeyState(VK_ADD) & 0x8000) != 0;
    if (kAdd && !pAdd) {
        LONG was = InterlockedCompareExchange(&g_killSleep, 0, 0);
        InterlockedExchange(&g_killSleep, was ? 0 : 1);
        Log("NUMPAD+: frame-rate governor %s - if the frame rate jumps to 120 and STAYS there,"
            " `bSmoothFrameRate=TRUE` was holding it down and everything else this session was"
            " measured through that", was ? "restored" : "NEUTRALISED (render-thread sleeps -> 0)");
    }
    pAdd = kAdd;

    // NUMPAD . : aim decoupling. Turns on the matrix split with it, because aiming the engine at
    // your hand without moving the view into the matrix first would just swing the camera with
    // your hand - the exact thing this is meant to stop.
    static bool pDec = false;
    bool kDec = (GetAsyncKeyState(VK_DECIMAL) & 0x8000) != 0;
    if (kDec && !pDec) {
        // ---- ❌ run 89: modes 4 and 5 are OUT of the cycle. The time split is dead ----
        //
        // Both wrote the hand into AActor::Rotation at a different instant and let the camera
        // update overwrite it with the head. Both failed the same way, and the reason came from
        // watching rather than from a bullet hole: **the view was driven by the head AND the
        // controller at once, spinning wildly.** So the camera does not sample the rotation once
        // at a point we can write after - it consumes it at more than one moment in the tick, and
        // there is no instant where the hand can sit in that field without the view taking it too.
        //
        // That is the whole time-split idea, and it is finished. Not "needs another seam" - the
        // premise it rested on, that the camera reads the rotation exactly once, is false.
        //
        // They are cut out of the cycle rather than deleted: they corrupt the view badly enough to
        // be unpleasant to hit by accident, and the code is worth keeping as the record of what
        // was tried. Cycle is 0-3 again.
        // ---- run 91: cycle order puts the LIVE modes first ----
        //
        // Same reasoning as PAGE UP running upwards first: reaching a mode should not take five
        // presses while wearing a headset, because that is how a run gets lost to a miscount.
        // 0 off, 6 the run 91 fix, 1 the shipping route-1 arrangement, then the two dead probes.
        // Mode 8 is CUT from the cycle: it puts the hand into the view permanently via PlayerMove
        // and inverts head tracking, so hitting it by accident costs a restart. Kept in source as
        // the record, exactly as modes 4 and 5 were.
        // Mode 10 first: it is the null window, so it writes nothing and cannot cycle the view,
        // and it is now also the mode that dumps the weapon trace parameters.
        // Mode 12 leads: it is the discriminator, and the one to press first.
        static const LONG kAimCycle[] = { 0, 12, 11, 1, 2, 3 };
        const int kAimCycleLen = (int)(sizeof(kAimCycle) / sizeof(kAimCycle[0]));
        LONG cur = InterlockedCompareExchange(&g_aimMode, 0, 0);
        int at = 0;
        for (int i = 0; i < kAimCycleLen; ++i) if (kAimCycle[i] == cur) { at = i; break; }
        const LONG mode = kAimCycle[(at + 1) % kAimCycleLen];
        InterlockedExchange(&g_aimMode, mode);
        // Resolving GNames is milliseconds, so it happens here on a keypress and never in the hook.
        // Unconditional rather than only for mode 6: the census needs the same table, and a
        // resolve that only runs in one mode is a diagnostic behind a switch the test may not press.
        if (FnIdx(kFnGetAdjustedAim) < 0) ResolveScriptNames();
        // Run 93: find the real ProcessInternal and move the hook onto it. Both are no-ops once
        // they have succeeded, so cycling modes does not re-walk GObjects.
        if (mode >= 6) { FindProcessInternal(); RepointScriptHook(); ProbeTraceNative(); InstallLineCheckHook(); InstallSingleLineCheckHook(); }
        // Arm the foreground-pass probe for three frames on any mode change - the user presses
        // this to reach aim mode 11 anyway, so it costs no extra keypress and no extra run.
        InterlockedExchange(&g_dpgProbeFrames, 3);
        // Mode 1 is the only one that needs the view taken out of the engine's rotation. Modes 2
        // and 3 want the plain arrangement back, so they clear it - otherwise the leftover split
        // would be silently steering the view during a test of something else entirely.
        InterlockedExchange(&g_aimDecouple, mode == 1 ? 1 : 0);
        InterlockedExchange(&g_matrixRot, mode == 1 ? 1 : 0);
        if (mode == 1 && InterlockedCompareExchange(&g_devStep, 0, 0) == 0)
            InterlockedExchange(&g_devStep, 3);   // 30 deg - at 0 the feature does nothing at all
        switch (mode) {
            case 0: Log("NUMPAD.: aim mode 0 - OFF, plain game rotation"); break;
            case 1: Log("NUMPAD.: aim mode 1 - engine follows the HAND (clamp %d deg)."
                        " Gun tracks your hand, but culling follows it too - this is the mode"
                        " that blacks out the ceiling",
                        kDevSteps[InterlockedCompareExchange(&g_devStep, 0, 0)]); break;
            case 2: Log("NUMPAD.: aim mode 2 - PROBE. NOTHING ON SCREEN WILL CHANGE. The view, the"
                        " culling and the gun all stay exactly as normal - that is the design, not"
                        " a failure. A constant -25 deg pitch and +25 deg yaw is written to the"
                        " controller's SECOND rotation at +0x05D4, and the ONLY thing that can"
                        " show it is a bullet."
                        " >>> STAND CLOSE TO A WALL AND FIRE ONE SHOT. <<<"
                        " Impact LOW AND RIGHT of the crosshair = +0x05D4 is the weapon's aim, and"
                        " aim decoupling is solved. Impact ON the crosshair = it is not, and the"
                        " fire trace has to be found in the binary.");
                    break;
            case 3: Log("NUMPAD.: aim mode 3 - view and culling on your HEAD, weapon aim from your"
                        " HAND via +0x05D4. Dead end: run 80 proved that field deflects nothing.");
                    break;
            case 12: Log("NUMPAD.: aim mode 12 - CONSTANT 25 DEGREE TRACE DEFLECTION. A probe, not a"
                        " feature: the trace End is rotated a fixed 25 degrees of YAW regardless of"
                        " where your controller points, which takes the hand out of the experiment"
                        " entirely."
                        " Mode 11 rotated by hand-minus-head and got 121 deg of yaw and 90 deg of"
                        " pitch, so that delta carries a bias - mode 1 has always clamped it to 30"
                        " deg, which is why nobody has seen it."
                        " NOTHING ON SCREEN WILL CHANGE and the gun will not move. The only"
                        " evidence is the bullet."
                        " >>> STAND CLOSE TO A WALL, AIM AT A MARK, FIRE ONE SHOT. <<<"
                        " Hole about 25 deg to the RIGHT of where you aimed = the rotation reaches"
                        " the bullet, and only the hand delta is wrong - arithmetic, not"
                        " archaeology."
                        " Hole exactly ON your mark = we are rotating a trace that does not decide"
                        " the impact, and the log now dumps the OTHER one.");
                    break;
            case 11: Log("NUMPAD.: aim mode 11 - ROTATE THE TRACE. This is the real one."
                        " The view, culling, LOD and the gun all stay on your HEAD and NOTHING"
                        " writes a rotation field, so the camera cannot follow your hand - that is"
                        " structural here, not a fix."
                        " The weapon fires 2 line checks per shot through 0x00BD79D0, and each has"
                        " its End rotated about its Start by the hand-minus-head delta."
                        " >>> STAND CLOSE TO A WALL, POINT WELL AWAY FROM THE CROSSHAIR, FIRE. <<<"
                        " Hole where your HAND points, view perfectly still = the bullet is"
                        " decoupled at last, with no culling or LOD cost."
                        " Hole mirrored the wrong way = AimTraceSignYaw or AimTraceSignPitch = -1"
                        " in the ini, no rebuild."
                        " NOTE: the gun MODEL still will not move. That is the separate half.");
                    break;
            case 10: Log("NUMPAD.: aim mode 10 - NULL WINDOW, the control for mode 9."
                        " Identical in every way - same fire functions, same hook, same nesting,"
                        " same seqlock, same log lines - except that it WRITES NOTHING."
                        " >>> FIRE SEVERAL SHOTS AND WATCH THE VIEW, NOT THE BULLET. <<<"
                        " View still cycles = it is NOT this mod. The window is innocent and the"
                        " cause is the game itself, recoil most likely."
                        " View steady here but moving in mode 9 = our write IS reaching the view,"
                        " and narrowing WHICH field cannot fix that - the engine reads these"
                        " fields for the view later in the same tick."
                        " The bullet will not track your hand in this mode. That is the point.");
                    break;
            case 9: Log("NUMPAD.: aim mode 9 - THE WINDOW IS THE SHOT. The view, the culling and the"
                        " gun all stay exactly as normal on your HEAD - that is the design, not a"
                        " failure, and nothing on screen will change when you press this."
                        " The hand goes into the rotation ONLY for the duration of"
                        " FireAmmunition/BeginFire/StartFire, which the run 95 census found at 86-89%%"
                        " enriched under fire against a 2.9%% baseline. FireAmmunition is what calls"
                        " InstantFire -> GetAdjustedAim. The view is computed in PlayerMove, a"
                        " different entry point, so it cannot see inside this window."
                        " >>> STAND CLOSE TO A WALL, POINT WELL AWAY FROM THE CROSSHAIR, FIRE. <<<"
                        " Impact where your HAND points = the bullet is decoupled, with no culling"
                        " or LOD cost at all. Impact ON the crosshair = the trace reads the aim"
                        " somewhere outside this window."
                        " NOTE: THE GUN MODEL WILL NOT MOVE. That is expected here - this mode is the"
                        " BULLET only. The gun's pose is a separate mechanism and is next.");
                    break;
            case 8: Log("NUMPAD.: aim mode 8 - THE MASKED TIME SPLIT. Run 94 closed the script route"
                        " by measurement: only 87 functions reach this hook and all are"
                        " native->script entry points, so GetAdjustedAim and SetAim never appear."
                        " But PlayerTick does, and it runs INSIDE the game tick - which modes 4 and"
                        " 5 never had, they wrote at Present and held the hand for a whole frame."
                        " Here the hand is in AActor::Rotation only for the duration of PlayerTick,"
                        " and ProcessViewRotation, UpdateCamera and LimitViewRotation get the HEAD"
                        " put back for their own duration if they nest inside it."
                        " >>> POINT AWAY FROM THE CROSSHAIR. Watch the gun, then fire at a wall. <<<"
                        " Gun and bullet follow your hand and the view stays put = solved."
                        " VIEW SWINGS OR SPINS = something else in the tick reads the rotation, and"
                        " the log now NAMES it - every function that ran inside the window is"
                        " listed as 'inside PlayerTick', which is what mode 4 failed to leave"
                        " behind.");
                    break;
            case 7: Log("NUMPAD.: aim mode 7 - ROUTE 2, THE WHOLE THING. Bullet AND gun follow your"
                        " controller; the view, culling, LOD and draw distance all stay on your"
                        " HEAD, so pointing your hand at the floor can no longer black out the"
                        " ceiling. AActor::Rotation is never written."
                        " Two seams: GetAdjustedAim's RETURN value is the bullet, SetAim's two"
                        " float PARAMETERS are the gun's pose via the in-view arms AnimTree."
                        " >>> POINT AWAY FROM THE CROSSHAIR. Watch the gun, then fire at a wall. <<<"
                        " Gun swings too far or too little = AimPoseUUPer1 in the ini is wrong."
                        " Gun swings the WRONG WAY = AimPoseSignX / AimPoseSignY. The log prints"
                        " the game's own values next to ours, which is enough to set both exactly.");
                    break;
            case 6: Log("NUMPAD.: aim mode 6 - ROUTE 2, BULLET ONLY. Isolates the fire trace: the"
                        " gun stays pointing down your view on purpose, so if mode 7's gun pose"
                        " misbehaves this says whether the bullet half is still good."
                        " The view, the culling and the gun all stay"
                        " exactly as normal, on your HEAD - that is the design, not a failure."
                        " RvWeaponShared.GetAdjustedAim returns the rotator the fire trace uses,"
                        " named out of the game's own script package in run 91, and this rewrites"
                        " that RETURN VALUE to your hand. AActor::Rotation is never touched, so"
                        " nothing else in the engine can see the hand."
                        " >>> STAND CLOSE TO A WALL, POINT WELL AWAY FROM THE CROSSHAIR, FIRE. <<<"
                        " Impact where your HAND points = aim decoupling is solved with NO culling"
                        " or LOD cost - set PAGE UP headroom back to 1.0."
                        " Impact ON the crosshair = GetAdjustedAim is not on this build's fire path."
                        " Check the log either way: it says which argument held the aim, or says"
                        " that neither did.");
                    break;
            case 5: Log("NUMPAD.: aim mode 5 - TIME SPLIT, second seam. Same idea as mode 4 but the"
                        " hand is written at INPUT POLL time instead of at Present, which is later"
                        " in the tick. Mode 4 showed the fire trace runs after the camera update,"
                        " so the write has to land later than Present did."
                        " >>> POINT WELL AWAY FROM THE CROSSHAIR AND FIRE. <<<"
                        " Impact where your HAND points = this is the seam and aim decoupling"
                        " works. Impact on the crosshair = input is polled before the camera"
                        " update too, and the time-split idea is out of seams.");
                    break;
            default: Log("NUMPAD.: aim mode 4 - THE TIME SPLIT. View and culling stay on your HEAD,"
                         " so nothing should vanish at the edges. The hand's aim is written to"
                         " AActor::Rotation at Present, and the camera update overwrites it with"
                         " the head - so anything in the tick that runs BEFORE the camera update"
                         " sees your hand."
                         " >>> STAND CLOSE TO A WALL, POINT WELL AWAY FROM YOUR CROSSHAIR, FIRE. <<<"
                         " Impact where your HAND points = the fire trace is on the right side of"
                         " the line and aim decoupling works. Impact on the crosshair = it runs"
                         " after the camera update and this approach is finished.");
                    break;
        }
    }
    pDec = kDec;

    // ---- TAB: freeze the gun's POSITION so the pivot can be tuned on its own ----
    //
    // G does two jobs in v' = (v - G) * R + G + (H - G), which reduces to (v - G) * R + H: it is
    // the rotation pivot AND the reference the translation subtracts. So moving the anchor forward
    // lands the gun further BACK - correct arithmetic, and useless for tuning, because one knob
    // moves two things in opposite directions.
    //
    // With position following off, H = G and the translation term vanishes, leaving pure rotation
    // about G. Then the anchor can be placed by eye without the gun sliding away from it.
    {
        static bool pTab = false;
        const bool kTab = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
        if (kTab && !pTab) {
            g_gunFollowPos = g_gunFollowPos ? 0 : 1;
            Log("TAB: gun position following %s%s", g_gunFollowPos ? "ON" : "OFF",
                g_gunFollowPos ? "" : " - pure rotation about the anchor, for tuning it");
        }
        pTab = kTab;
    }

    // ---- ARROW KEYS: tune the gun anchor, the point it rotates about ----
    //
    // LEFT/RIGHT choose the axis, UP/DOWN move it by one engine unit (about 1.9 cm). All three
    // values and the selection are on the readout, so this is adjust-and-look rather than
    // edit-restart-look. Whatever it settles at goes into the ini to persist.
    {
        static bool pL = false, pR = false, pU = false, pD = false;
        const bool kL = (GetAsyncKeyState(VK_LEFT)  & 0x8000) != 0;
        const bool kR = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
        const bool kU = (GetAsyncKeyState(VK_UP)    & 0x8000) != 0;
        const bool kD = (GetAsyncKeyState(VK_DOWN)  & 0x8000) != 0;
        if (kL && !pL) InterlockedExchange(&g_anchorAxis,
                          (InterlockedCompareExchange(&g_anchorAxis, 0, 0) + 2) % 3);
        if (kR && !pR) InterlockedExchange(&g_anchorAxis,
                          (InterlockedCompareExchange(&g_anchorAxis, 0, 0) + 1) % 3);
        if ((kU && !pU) || (kD && !pD)) {
            const LONG step = (kU && !pU) ? 1 : -1;
            const LONG ax = InterlockedCompareExchange(&g_anchorAxis, 0, 0);
            if      (ax == 0) g_gunAnchorFwd   += step;
            else if (ax == 1) g_gunAnchorRight += step;
            else              g_gunAnchorUp    += step;
            Log("ANCHOR: fwd %ld right %ld up %ld", g_gunAnchorFwd, g_gunAnchorRight, g_gunAnchorUp);
        }
        pL = kL; pR = kR; pU = kU; pD = kD;
    }

    // ---- CAPS LOCK: cycle the eight gun rotation-sign combinations ----
    //
    // Three axes have come back wrong in three separate runs, each costing a restart to test one
    // guess. Eight combinations on one key finds the right one inside a single session.
    {
        static bool pCaps = false;
        const bool kCaps = (GetAsyncKeyState(VK_CAPITAL) & 0x8000) != 0;
        if (kCaps && !pCaps) {
            const LONG combo = (InterlockedCompareExchange(&g_gunSignCombo, 0, 0) + 1) & 15;
            InterlockedExchange(&g_gunSignCombo, combo);
            int sy, sp, sr; GunSigns(sy, sp, sr);
            Log("CAPS: combo %ld - yaw %+d pitch %+d roll %+d, view-frame %s",
                combo, sy, sp, sr, GunViewFrame() ? "ON" : "OFF");
        }
        pCaps = kCaps;
    }

    // ---- VK_APPS: hide the first-person pass ----
    //
    // The confirmation test for stage 1. If the arms and gun vanish and NOTHING ELSE changes, the
    // depth-clear boundary is right. If the world goes with them, the boundary is wrong and the
    // log will show a foreground run of hundreds of draws rather than six.
    //
    // VK_APPS - the menu key by the right Ctrl - because every other key on this keyboard is
    // already bound and it is isolated enough not to be hit by accident.
    {
        static bool pApps = false;
        const bool kApps = (GetAsyncKeyState(VK_APPS) & 0x8000) != 0;
        if (kApps && !pApps) {
            const LONG next = (InterlockedCompareExchange(&g_hideMeshIdx, 0, 0) + 1) % 4;
            InterlockedExchange(&g_hideMeshIdx, next);
            // Latch the two meshes by vertex count at the moment of selection, so the transform
            // keeps hold of them when the foreground list changes under fire.
            if (next == 1) {
                InterlockedExchange(&g_gunVerts,  InterlockedCompareExchange(&g_fgStableVerts[0], 0, 0));
                InterlockedExchange(&g_armsVerts, InterlockedCompareExchange(&g_fgStableVerts[1], 0, 0));
                Log("APPS: latched gun=%ld verts, arms=%ld verts",
                    InterlockedCompareExchange(&g_gunVerts, 0, 0),
                    InterlockedCompareExchange(&g_armsVerts, 0, 0));
            }
            InterlockedExchange(&g_dpgProbeFrames, 3);
            static const char* kWhat[4] = {
                "0 - normal, nothing touched",
                "1 - GUN ROTATED to follow your controller (arms untouched)",
                "2 - ARMS HIDDEN (gun untouched)",
                "3 - GUN follows your controller and ARMS HIDDEN - the target configuration" };
            Log("APPS: %s", kWhat[next & 3]);
        }
        pApps = kApps;
    }

    // ---- PAUSE held for a second: quit the game ----
    //
    // A test loop that ends in taking the headset off, finding the mouse and clicking through a
    // menu costs more than the test does. One key, reachable by feel at the top right.
    //
    // It is a HOLD, not a tap, for two reasons. SCROLL LOCK sits directly beside PAUSE and arms
    // the census, so a mis-hit in a headset is likely; and an accidental quit costs a whole run.
    // The mod already uses hold-versus-tap for the controller MENU button, so the idiom is not new.
    //
    // WM_CLOSE rather than ExitProcess: it is the same shutdown path as clicking the window X, so
    // the engine saves and exits normally. ENVIRONMENT.md records that every copy of this game
    // shares ONE save profile - killing the process mid-write is a real way to lose it.
    {
        static ULONGLONG heldSince = 0;
        const bool kPause = (GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0;
        if (!kPause) {
            heldSince = 0;
        } else {
            const ULONGLONG now = GetTickCount64();
            if (!heldSince) heldSince = now;
            else if (now - heldSince >= 800) {
                heldSince = 0;
                Log("PAUSE held: closing the game (WM_CLOSE to 0x%p) - clean shutdown, saves"
                    " normally.", (void*)g_gameWindow);
                if (g_gameWindow) PostMessageA(g_gameWindow, WM_CLOSE, 0, 0);
                else              PostQuitMessage(0);
            }
        }
    }

    // NUMPAD7 (or SCROLL LOCK): arm the script-call census, then press again to dump it.
    //
    // ⚠️ Run 83 was lost to this. SCROLL LOCK alone looked free, but the log recorded no press at
    // all - plenty of keyboards omit the key entirely or bury it behind Fn, and one that cannot be
    // pressed is indistinguishable from a feature that does not work. NUMPAD7 was the button
    // probe's reset, which has been dead weight since the layout came out of the game's own config
    // in run 61b; the probe wraps on its own without it. SCROLL LOCK stays wired up as well, since
    // accepting both costs one `||`.
    static bool pScr = false;
    bool kScr = ((GetAsyncKeyState(VK_SCROLL) & 0x8000) != 0)
             || ((GetAsyncKeyState(VK_NUMPAD7) & 0x8000) != 0);
    if (kScr && !pScr) {
        if (InterlockedCompareExchange(&g_peCensus, 0, 0)) {
            InterlockedExchange(&g_peCensus, 0);
            DumpProcessEventCensus();
        } else {
            memset(g_peSlots, 0, sizeof(g_peSlots));
            InterlockedExchange(&g_peCalls, 0);
            InterlockedExchange(&g_peFiringCalls, 0);
            InterlockedExchange(&g_peSeen, 0);
            InterlockedExchange(&g_peUnnamed, 0);
            // Resolve before arming, not at dump time: the Tick control and the five named aim
            // functions are useless if their indices are still -1 when the table is read, and
            // arming is the moment the user is definitely in gameplay with packages loaded.
            ResolveScriptNames();
            InterlockedExchange(&g_peCensus, 1);
            Log("NUMPAD7: script-call census ARMED and cleared. Play for a bit, FIRE SEVERAL"
                " TIMES, then press NUMPAD7 again to dump it.");
        }
    }
    pScr = kScr;

    LogXInputStats();

    // BACKSPACE: the whole mod on or off in one key. See SetVrMode.
    static bool pBack = false;
    bool kBack = (GetAsyncKeyState(VK_BACK) & 0x8000) != 0;
    if (kBack && !pBack) SetVrMode(InterlockedCompareExchange(&g_dupDraws, 0, 0) == 0);
    pBack = kBack;

    // ---- ⭐ run 133: arm the cutscene watch WITHOUT a keypress ----
    //
    // Run 133 tested the CINE indicator through the whole opening sequence and it read NO
    // throughout - not because no cutscene ran, but because the name table was never resolved.
    // Resolution only happened on the aim-mode key or the census key, and neither is pressed
    // before the opening cutscene, so every index was still -1 and the watch could not match
    // anything. The indicator was reporting "not armed" in a way that reads exactly like "no
    // cutscene". That is the same class of failure as the truncated readout: an instrument whose
    // not-working state is indistinguishable from its negative result.
    //
    // So it resolves itself. Gated on the camera existing, which is the cheapest available proxy
    // for "a level is loaded and GNames is populated" - before that there is nothing to find and
    // the walk would be wasted. Retried on a slow tick rather than once, because the first
    // attempt can land while packages are still streaming in, and capped so a name that genuinely
    // does not exist in this build cannot cost a walk forever.
    {
        static int tick = 0, attempts = 0;
        if (FnIdx(kFnSetCinematic) < 0 && attempts < 12 && (++tick % 240) == 0 && g_camera) {
            ++attempts;
            Log("auto-resolving script names (attempt %d) - arming the cutscene watch", attempts);
            ResolveScriptNames();
        }
    }

    // ']' : freeze the view. The key freed up when the yaw fold-in moved onto NUMPAD8's cycle.
    //
    // Latches the ENGINE's camera rotation, not the head's, so the frozen view is the one the save
    // produces and two launches can be made to agree. The angle is logged and put on the readout
    // in degrees precisely so they can be CHECKED to agree rather than assumed to.
    static bool pFreeze = false;
    bool kFreeze = (GetAsyncKeyState(VK_OEM_6) & 0x8000) != 0;
    if (kFreeze && !pFreeze) {
        const LONG was = InterlockedCompareExchange(&g_freezeView, 0, 0);
        if (!was) {
            // Prefer the camera actor's real rotation - that is what the frame is drawn with. Fall
            // back to what we last asked for only if it has never been read, which is the first
            // frame or two after a load.
            g_frozenYaw   = g_camRotKnown ? g_camYawUU   : g_wantYaw;
            g_frozenPitch = g_camRotKnown ? g_camPitchUU : g_wantPitch;
        }
        InterlockedExchange(&g_freezeView, was ? 0 : 1);
        Log("]: view %s%s", was ? "UNFROZEN - head tracking resumes" : "FROZEN to the engine camera",
            was ? "" : " - no head rotation, no roll, no 6-DOF offset. Stereo separation is kept.");
        if (!was)
            Log("    frozen at yaw %d (%.1f deg), pitch %d (%.1f deg) - MATCH THESE in the other"
                " run or the comparison is between two different views",
                g_frozenYaw, g_frozenYaw * (360.0f / 65536.0f),
                g_frozenPitch, g_frozenPitch * (360.0f / 65536.0f));
    }
    pFreeze = kFreeze;

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
        // The ASKED figure is printed alongside because the engine does not necessarily take it -
        // the readback has been landing at ~124.5 while 129.8 was requested, which would mean a
        // hard clamp and a ceiling on how much headroom can ever be bought. Watch the "asked the
        // engine for" and "read back before write" lines against each other.
        Log("PAGE UP: culling headroom now x%.2f%s", kFovHeadroom,
            kFovHeadroom < 1.05f ? " (no headroom - expect edge dropouts, but correct LOD)"
                                 : " - check the FOV readback below actually follows it");
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
        double thisFrameMs = 0.0;
        if (last.QuadPart) {
            const double dt = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
            accum += dt;
            thisFrameMs = dt * 1000.0;
        }
        last = now;
        if (g_drawsTotal > totPeak) totPeak = g_drawsTotal;

        // Bucket this frame by the state it was rendered in. Both controllers must be tracked to
        // count as awake - one hand down is the ambiguous case and would only blur the comparison.
        //
        // xrWaitFrame and XR submit come from the running totals as per-frame deltas, so the
        // link's own contribution can be read separately inside each bucket. Those totals are
        // zeroed once a second by the block below, which makes one delta per second negative -
        // that frame is dropped rather than corrected, because one frame in 120 is not worth the
        // bookkeeping.
        {
            static double lastWait = 0.0, lastSubmit = 0.0;
            const double dWait   = g_msWaitFrame - lastWait;
            const double dSubmit = g_msXrSubmit  - lastSubmit;
            lastWait = g_msWaitFrame; lastSubmit = g_msXrSubmit;
            if (thisFrameMs > 0.0 && thisFrameMs < 500.0 && dWait >= 0.0 && dSubmit >= 0.0) {
                const int idx = (InterlockedCompareExchange(&g_padEmu, 0, 0) ? 4 : 0)
                              + (InterlockedCompareExchange(&g_xrInputOff, 0, 0) ? 2 : 0)
                              + ((g_handHeld[0] || g_handHeld[1]) ? 0 : 1);
                g_bucket[idx].frameMs    += thisFrameMs;
                g_bucket[idx].waitMs     += dWait;
                g_bucket[idx].xrSubmitMs += dSubmit;
                g_bucket[idx].drawsSum   += g_drawsTotal;
                if (thisFrameMs > g_bucket[idx].maxFrameMs) g_bucket[idx].maxFrameMs = thisFrameMs;
                // A missed deadline, not a slow average. 1.5x the display period is comfortably
                // past the point where the compositor has to reproject a stale frame, which is
                // what judder actually is.
                const double period = g_displayPeriodMs > 0.0 ? g_displayPeriodMs : 8.33;
                if (thisFrameMs > period * 1.5) ++g_bucket[idx].missed;
                ++g_bucket[idx].frames;
            }
        }

        // Frames near a phase change are dropped rather than attributed. The phase is set in
        // UpdateFromHeadset and the draws it governs are counted a frame or so later, and rather
        // than reason about that alignment - which is exactly the sort of thing that is wrong
        // silently - discard the boundary. Each phase runs 15 frames, so losing 3 costs nothing.
        if (InterlockedCompareExchange(&g_devFlicker, 0, 0) && g_devPhaseAge >= 3) {
            g_devDraws[g_devPhase] += g_drawsTotal;
            ++g_devDrawFrames[g_devPhase];
        }
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
            // ---- âš ï¸ THIS LINE IS WHY THE USER-POINTER HOOKS "HUNG" (found run 27) ----
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
            // ---- the four buckets, side by side ----
            //
            // CUMULATIVE, deliberately: these are never reset, so every second of play adds
            // evidence and the comparison only gets stronger. A per-second version would be back
            // to comparing two moments across a link that varies by 20x within an evening.
            //
            // Read MISSED first - that is what judders. Read draws/frame second: if it moves with
            // the bucket, the comparison is contaminated by what the player was doing and the rest
            // of the row means little. Only compare rows whose draw counts are close.
            //
            // The row that matters most is the one run 68 never collected: **input OFF with
            // controllers AWAKE**. Without it there is no way to separate "the controllers are
            // awake" from "the input code is running", because in run 68 those were the same
            // condition. Pressing NUMPAD/ while holding the controllers is the whole experiment.
            {
                bool any = false;
                for (int i = 0; i < 8; ++i) if (g_bucket[i].frames > 30) any = true;
                if (any) {
                    Log("    ---- controller/input correlation (cumulative) ----");
                    for (int i = 0; i < 8; ++i) {
                        // `bk`, not `b` - Hook_Present's third RECT parameter is `b` and is live
                        // here. Second time this file has caught me doing it.
                        const StateBucket& bk = g_bucket[i];
                        if (bk.frames <= 30) continue;   // too few to mean anything
                        const double n = (double)bk.frames;
                        const double fr = bk.frameMs / n;
                        const double wt = bk.waitMs / n;
                        const double sb = bk.xrSubmitMs / n;
                        Log("      %s : %.1f fps | residual %.2f | MISSED %.1f%% of frames"
                            " (worst %.0f ms) | %.0f draws/frame | %d frames"
                            " [wait %.2f, submit %.2f]",
                            BucketName(i), 1000.0 / fr, fr - wt - sb,
                            (100.0 * bk.missed) / n, bk.maxFrameMs, bk.drawsSum / n,
                            bk.frames, wt, sb);
                    }
                }
            }

            // The governor, in milliseconds per second of wall clock. If this is thousands of ms
            // while the frame rate sits at a suspiciously round number, the engine is holding the
            // frame rate down on purpose and none of "our work" was ever our work.
            {
                const LONG sc = InterlockedExchange(&g_sleepCalls, 0);
                const LONG sm = InterlockedExchange(&g_sleepMs, 0);
                if (sc > 0)
                    Log("    frame-rate governor: %ld render-thread sleeps totalling %ld ms in the"
                        " last second%s", sc, sm,
                        InterlockedCompareExchange(&g_killSleep, 0, 0) ? " [NEUTRALISED]" : "");
            }

            // Controller state, on the same line cadence as the frame budget, so a run where the
            // controllers were picked up and set down a few times shows the correlation directly
            // instead of depending on anyone remembering when they did it.
            // Speeds in MICROMETRES per frame, printed next to the verdict they produced. The
            // threshold is 500 um; if the two states do not separate cleanly either side of that,
            // the number is wrong and this line says so without another run being needed.
            Log("    controllers [%s]: left %s (%.0f um/frame), right %s (%.0f um/frame), pad %s%s,"
                " XR input %s, sync-fail %d",
                g_graspAvailable ? "touch sensors" : "MOTION FALLBACK - no touch bindings",
                g_handHeld[0] ? "IN HAND" : "on desk", g_handSpeed[0] * 1.0e6f,
                g_handHeld[1] ? "IN HAND" : "on desk", g_handSpeed[1] * 1.0e6f,
                InterlockedCompareExchange(&g_padEmu, 0, 0) ? "ON" : "OFF",
                g_autoPadDisabled ? " (auto)" : "",
                InterlockedCompareExchange(&g_xrInputOff, 0, 0) ? "OFF (numpad /)" : "on",
                g_syncFailFrames);
            g_syncFailFrames = 0;

            // The four phases of SyncXRInput, so the stall has a NAME rather than a suspect list.
            // Worst-frame matters more than the mean here: a single 8 ms frame is a dropped frame
            // and reads as judder, and a one-second average buries it completely.
            if (g_syncSamples > 0) {
                const double n = (double)g_syncSamples;
                Log("    xrinput cost: xrSyncActions %.3f ms (peak %.3f) | action states %.3f"
                    " (peak %.3f) | hand poses %.3f (peak %.3f) | haptics %.3f (peak %.3f)"
                    " over %d frames - the PEAK column is the one that judders",
                    g_msSyncActions / n, g_pkSyncActions, g_msActionStates / n, g_pkActionStates,
                    g_msPoses / n, g_pkPoses, g_msHaptics / n, g_pkHaptics, g_syncSamples);
                g_msSyncActions = g_msActionStates = g_msPoses = g_msHaptics = 0.0;
                g_pkSyncActions = g_pkActionStates = g_pkPoses = g_pkHaptics = 0.0;
                g_syncSamples = 0;
            }

            // The residual: how far the engine's camera actually is from where we told it to go.
            // At clamp 0 this should be ~0. Anything else there means the camera actor is not the
            // thing the view matrix is built from, and a lot of this design rests on it being so.
            if (InterlockedCompareExchange(&g_matrixRot, 0, 0) && g_residN > 0) {
                Log("engine-vs-asked residual: mean %.2f deg, WORST %.2f deg over %d frames"
                    " (clamp %d). Near zero at clamp 0 = the camera adopts what we write."
                    " Large after a clamp change = it is SMOOTHING, and the matrix is compensating"
                    " for a rotation the camera has not taken yet.",
                    g_residSum / g_residN, g_residMax, g_residN,
                    kDevSteps[InterlockedCompareExchange(&g_devStep, 0, 0)]);
                g_residSum = 0.0; g_residMax = 0.0; g_residN = 0;
            }

            if (InterlockedCompareExchange(&g_devFlicker, 0, 0) &&
                g_devDrawFrames[0] > 0 && g_devDrawFrames[1] > 0) {
                const double base = g_devDraws[0] / g_devDrawFrames[0];
                const double test = g_devDraws[1] / g_devDrawFrames[1];
                Log("culling flicker @ %d deg: clamp 0 -> %.0f draws/frame, clamp %d -> %.0f"
                    " (%+.1f%%). The NUMBER is an upper bound - it counts the engine's whole"
                    " frustum. The VERDICT is what you can see: anything flickering is geometry"
                    " culled at this angle, and nothing flickering means nothing visible was lost.",
                    kDevSteps[InterlockedCompareExchange(&g_devStep, 0, 0)], base,
                    kDevSteps[InterlockedCompareExchange(&g_devStep, 0, 0)], test,
                    base > 0.0 ? (test - base) * 100.0 / base : 0.0);
                g_devDraws[0] = g_devDraws[1] = 0.0;
                g_devDrawFrames[0] = g_devDrawFrames[1] = 0;
            }

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
                // xrBeginFrame + xrEndFrame, split out in run 56. It used to be pooled into "our
                // work", which is why "our work" has looked like an unexplained game-side stall all
                // the way through: this is the only call in the mod that reaches the wireless link.
                const double xs = g_xrSubmitFrames ? g_msXrSubmit / g_xrSubmitFrames : 0.0;
                Log("    frame budget: xrWaitFrame %.2f (idle) + copy %.2f + GObjects walk %.2f"
                    " + XR submit %.2f + our work %.2f = %.2f ms (%.0f%% of the %.2f ms display period)",
                    wf, copy, eng, xs, ms - wf - copy - eng - xs, ms,
                    g_displayPeriodMs > 0.0 ? 100.0 * ms / g_displayPeriodMs : 0.0,
                    g_displayPeriodMs);
                // The peak matters more than the mean here. A one-second average hides a single
                // 200 ms submission among a hundred fast ones, and a stall that long is exactly
                // what a seconds-long dip is made of.
                if (xs > 1.0 || g_msXrSubmitPeak > 8.0)
                    Log("    *** XR submit: mean %.2f ms, WORST SINGLE FRAME %.2f ms."
                        " This is xrBeginFrame+xrEndFrame - the compositor and the wireless link,"
                        " NOT the game. If the worst frame is tens of ms, the dips are the"
                        " streaming path and no change to the render code will help. ***",
                        xs, g_msXrSubmitPeak);
                g_msXrSubmit = 0.0; g_xrSubmitFrames = 0; g_msXrSubmitPeak = 0.0;
                // A cached hit is microseconds; anything here in milliseconds means the object is
                // absent and the full scan is repeating, which is the menu/level-load freeze.
                if (eng > 2.0)
                    Log("    *** GObjects walk cost %.2f ms this window - the camera or controller"
                        " is ABSENT, so the cache cannot warm and the full scan repeats. Expected"
                        " in menus and during level load; should be ~0 in gameplay. ***", eng);
                // ---- âš ï¸ the SSW check, and the correction it needed one run later ----
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
            // ---- which registers are actually being remapped (run 42) ----
            //
            // The shadow-pass dump showed a SECOND view-projection at vs c13: same projection
            // columns as c0-c3, but a world-space translation (c16 = 5179, -1738, 26024) where
            // c3 is translated-world and small. If the shadow geometry rides on c13 and only c0
            // is remapped, that geometry keeps full-frame coordinates and spills across the seam -
            // which is the reported symptom exactly.
            //
            // The detector is supposed to track every register holding a camera matrix, so this
            // may already be covered. Printing the register numbers settles it without another
            // theory: if 13 is absent, that is the bug.
            // ---- âš ï¸ this line reported nonsense first time out (run 46) ----
            //
            // It subtracted a per-WINDOW cumulative from a per-FRAME peak and printed negative
            // counts. Both figures are cumulative over the window now. Sixth diagnostic in this
            // file to report something other than reality; the recurring cause is a counter's
            // units not matching the one it is combined with.
            if (g_userPtrHookLevel >= 3) {
                Log("    user-pointer draws this window: %d split per eye, %d passed through as"
                    " fullscreen quads (threshold %d prims)",
                    g_drawsUPSplit, g_drawsUPQuad, g_userPtrMinPrims);
                // The threshold is only meaningful if the two populations actually separate. If
                // decals are themselves 2-triangle quads - which UE3 decals often are - then no
                // primitive count can tell them from a post-process quad and this whole approach
                // is dead. The histogram says which world we are in.
                Log("    user-pointer primitive counts: 1:%d  2:%d  3-4:%d  5-8:%d  9-16:%d  17+:%d",
                    g_upHist[0], g_upHist[1], g_upHist[2], g_upHist[3], g_upHist[4], g_upHist[5]);
            }
            g_drawsUPQuad = 0; g_drawsUPSplit = 0;
            for (int h = 0; h < 6; ++h) g_upHist[h] = 0;
            if (InterlockedCompareExchange(&g_skipExtraTarget, 0, 0))
                Log("    NUMPAD0 ACTIVE: %d draws to the scene-sized non-colour target dropped"
                    " this window", g_drawsSkipped);
            g_drawsSkipped = 0;
            {
                char regs[128] = {}; int n = 0;
                for (int s = 0; s < g_camMatUsed && n < 100; ++s)
                    n += sprintf_s(regs + n, sizeof(regs) - n, "%sc%u%s",
                                   n ? " " : "", g_camMats[s].reg, g_camMats[s].valid ? "" : "(stale)");
                Log("    camera matrix registers tracked: %s (peak live in a frame %d)",
                    n ? regs : "none", g_camMatValidPeak);
            }
            // The identification counters are the point of this run. c0 carries one matrix per
            // frame, so a rejection is a whole-frame event - no forced projection and no split for
            // any draw in it. If "frames with NO identification" is non-zero, that is a full-frame
            // flash, and g_bestRejectDot names the cause: near 0.99 is rotation lag against a
            // stale g_camFwd, far below it is a genuinely different matrix taking the register.
            // The cutscene detector candidate. Snapshotted and reset here so the readout shows the
            // last second rather than a cumulative average, which would smear the boundary this
            // whole measurement exists to find.
            {
                const LONG ck = InterlockedExchange(&g_yawChecked, 0);
                const LONG hd = InterlockedExchange(&g_yawHeld, 0);
                if (ck > 0) {
                    const LONG pct = (hd * 100) / ck;
                    InterlockedExchange(&g_yawHeldPct, pct);
                    Log("    yaw held: engine kept our written yaw on %ld of %ld frames (%ld%%)"
                        " - near 100 is gameplay, near 0 is something else driving the camera",
                        hd, ck, pct);
                }
            }
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
                // ---- âš ï¸ this line is NOT evidence about the run-18 theory (found run 28) ----
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
    ApplyPadTestOverrides();   // must follow SyncXRInput - see the note on the function
    UpdateAutoPad();           // and after that, so a test override is never what keeps it awake
    InterlockedExchange(&g_camRotDirty, 1);   // re-read the camera's real rotation next frame

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
            // ---- ⭐ run 99: do NOT fold in our own fire-window write ----
            //
            // "Every time I shot, the camera view moved." That is this fold-in, not the swap.
            //
            // Present runs on the render thread; the fire window runs on the game thread. If
            // Present reads rot[1] while the window is open it sees the HAND, computes an
            // externalDelta of tens of degrees, and adds it to g_baseYaw PERMANENTLY - because
            // this code exists to absorb mouse and stick turning, and cannot tell someone else's
            // input from our own microsecond write. The view then jumps by that amount and stays
            // there. It is the exact feedback loop that inverted head tracking in mode 8; I named
            // the mechanism there and did not guard against it here.
            //
            // The guard is a seqlock. The hook makes g_fireSeq odd while a window is open and even
            // when it closes, so a read that spans a window is detectable rather than merely
            // unlikely. Detected, the fold is SKIPPED for this frame: losing one frame of mouse
            // turning is invisible, while folding in a 98-degree phantom delta is not.
            const LONG seqBefore = InterlockedCompareExchange(&g_fireSeq, 0, 0);
            const int32_t observedYaw = rot[1];
            const LONG seqAfter = InterlockedCompareExchange(&g_fireSeq, 0, 0);
            const bool windowRaced = (seqBefore != seqAfter)
                                  || InterlockedCompareExchange(&g_fireDepth, 0, 0) > 0;

            // ---- ⭐ run 133: this fold-in is the prime suspect for cutscene yaw ----
            //
            // Reported: in the opening cutscene, looking LEFT and RIGHT does nothing, while
            // looking up and down, leaning and head tilt all work - and yaw is dead in BOTH
            // ROT ENGINE and ROT MATRIX. That asymmetry is the whole clue. Pitch, position and
            // roll reach the view by three different routes and all three survive; the one
            // mechanism that exists for yaw and for nothing else is this fold-in.
            //
            // The predicted mechanism, and what the toggle is here to confirm or kill: a
            // scripted camera holds the controller's yaw at some value S. We write g_wantYaw,
            // the engine puts S back, and next frame this code reads S, cannot tell a script
            // from a mouse, and books the difference as player input - so g_baseYaw absorbs
            // exactly the head's contribution and g_wantYaw settles at S every single frame.
            // The head's yaw is then cancelled as fast as it is produced, which kills ENGINE
            // mode (nothing is written that survives) and MATRIX mode alike (the matrix rotates
            // from the camera's yaw to g_wantYaw, and those are now the same number).
            //
            // A diagnostic toggle rather than a fix, deliberately. The real fix has to tell a
            // cutscene from a mouse, and guessing at that before the mechanism is confirmed is
            // how a run gets spent proving the wrong thing.
            // ---- ⭐ run 134: does the engine KEEP the yaw we write? ----
            //
            // The ProcessEvent route to detecting a cutscene is dead: all 18 names resolved and
            // SetCinematicMode never fired once through the whole opening. Run 94 already
            // established why - this hook only sees native->script ENTRY points, and a script
            // calling SetCinematicMode is not one. SetViewTarget and ClientSetCameraMode were
            // silent for the same reason.
            //
            // So detect the cutscene by its EFFECT instead, which mode 4 already demonstrated:
            // during the opening, the engine does not honour our yaw write at all. Measured as an
            // exact match, which is what makes it a binary signal rather than a threshold:
            //
            //   * gameplay, standing still - we write W, nothing else touches it, readback IS W.
            //   * gameplay, turning - the engine ADDS the input to our W, so the readback differs
            //     for that frame but our write still persisted underneath it.
            //   * cutscene - the script SETS yaw outright, so the readback is unrelated to W and
            //     an exact match essentially never happens.
            //
            // Measured only. Nothing acts on it yet, for the same reason as before: a detector
            // that misfires in play silently breaks mouse and gamepad turning. What this run has
            // to answer is whether the separation is clean - near 100% in play against near 0% in
            // the cutscene - or whether ordinary turning drags it low enough to overlap.
            if (g_haveLastWrittenYaw && !windowRaced) {
                InterlockedIncrement(&g_yawChecked);
                if (observedYaw == g_lastWrittenYaw) InterlockedIncrement(&g_yawHeld);
            }

            if (!InterlockedCompareExchange(&g_yawFoldIn, 0, 0)) {
                // Suppressed. The reference still advances below, so re-enabling mid-run does
                // not fold in the whole accumulated difference as one jump.
            } else if (g_haveLastWrittenYaw && !windowRaced) {
                int32_t externalDelta = observedYaw - g_lastWrittenYaw;
                if (externalDelta) {
                    g_baseYaw += externalDelta;
                    g_wantYaw += externalDelta;
                }
            }
            rot[1] = g_wantYaw;

            // ---- ⭐ run 79: stop lying to the engine, and look for a SECOND rotation ----
            //
            // The architecture was backwards, and the question that made it obvious was "shouldn't
            // culling be based on where I'm looking?". Yes. Under the current scheme the engine's
            // whole model of the player - culling, LOD, draw distance, movement basis, AI
            // perception, the viewmodel - is pointed at the hand so that ONE consumer, the fire
            // trace, comes out right. Five things made wrong to fix one, and the FOV headroom is
            // then a workaround for damage we chose to do.
            //
            // The truthful arrangement is the reverse: `AActor::Rotation` keeps carrying the HEAD,
            // everything derived from it stays correct by construction, and only the weapon's aim
            // is redirected. That needs a separate seam - and there is a candidate nobody has
            // tested for this.
            //
            // ❌ RESULT (run 80): +0x05D4 is NOT the weapon's aim. A constant -25 pitch / +25 yaw
            // written there produced a shot straight down the crosshair - no deflection at all. So
            // the second rotation drives neither the view (spike 5) nor the fire trace, and field
            // guessing on this object is finished.
            //
            // That is consistent with how UE3 is built: `Pawn.GetBaseAimRotation()` returns the
            // Controller's own `Rotation` for a player, so there is no second aim field to find.
            // **The aim and the culling frustum genuinely are the same value**, which is why every
            // arrangement that separates them has to separate them in TIME or in the RENDERER, not
            // by finding another field. Two routes remain and both are recorded above SetVrMode.
            //
            // Kept as mode 2 because a negative that cost one shot at a wall is worth being able
            // to re-run, and because the same harness will test the next candidate field if one
            // ever turns up.
            if (Readable((void*)(ctl + CTL_ROTATION_2), 12)) {
                int32_t* rot2 = reinterpret_cast<int32_t*>(ctl + CTL_ROTATION_2);
                const LONG mode = InterlockedCompareExchange(&g_aimMode, 0, 0);
                if (mode == 2) {
                    // ⚠️ Run 80: the offset is now LARGE and on BOTH axes, because run 79's probe
                    // came back with no reading at all. In this mode nothing on screen changes -
                    // view, culling and the gun all stay normal by design - so "I pressed it and
                    // nothing happened" is the expected appearance and the test reads as a
                    // non-event. The only evidence it can produce is where a BULLET lands.
                    //
                    // -25 pitch and +25 yaw puts the impact low and to the right, far enough from
                    // the crosshair that it cannot be mistaken for spread or recoil.
                    rot2[0] = g_wantPitch - (int32_t)(25.0f * (65536.0f / 360.0f));
                    rot2[1] = g_wantYaw   + (int32_t)(25.0f * (65536.0f / 360.0f));
                } else if (mode == 3) {
                    rot2[0] = g_aimPitchUU;
                    rot2[1] = g_aimYawUU;
                } else {
                    rot2[1] = g_wantYaw;
                }
            }

            // ---- ⭐ mode 4: separate aim from culling in TIME, using seams we already own ----
            //
            // Six runs of chasing ProcessEvent produced one solid fact - the fire path DOES go
            // through UnrealScript, 77-89% enriched under fire - and then stalled on naming the
            // functions. 0x01308A10 turned out to take an FFrame rather than a UFunction, so it is
            // ProcessInternal or an op handler, and no offset in that frame consistently names
            // anything. That is archaeology with no end in sight.
            //
            // But the whole point of finding ProcessEvent was to own a moment INSIDE the tick when
            // the rotation could differ. This mod already owns one: `FUN_0104e390` is detoured, and
            // its caller copies the returned FRotator straight into AActor::Rotation. So the frame
            // already has two distinct instants we control:
            //
            //     Present (end of frame)  -> write the HAND here
            //     camera update (detour)  -> return the HEAD, caller overwrites +0x60 with it
            //
            // Everything in the tick that runs BEFORE the camera update therefore reads the hand;
            // everything after reads the head. Culling is computed from the camera, which is
            // downstream of the update, so it gets the head - which is exactly the property that
            // has been missing.
            //
            // Whether the fire trace lands on the right side of that line is unknown and is
            // precisely what this tests. It is a binary question, it needs no symbol, and it costs
            // one run instead of an unbounded number.
            //
            // ⚠️ If the shot follows the head, the ordering is wrong and this approach is finished
            // - which is still worth knowing in one run rather than five.
            // Publish for the XInput seam (mode 5). Done unconditionally and cheaply so the hot
            // hook only ever reads three interlocked words.
            InterlockedExchange(&g_ctlForXi, (LONG)ctl);

            // Run 114: dump the foreground-pass probe once per armed frame, from PRESENT - never
            // from inside the draw, viewport or clear hooks.
            if (InterlockedCompareExchange(&g_dpgProbeFrames, 0, 0) > 0) {
                DumpDpgProbe();
                InterlockedDecrement(&g_dpgProbeFrames);
            }
            InterlockedExchange(&g_dpgCount, 0);
            InterlockedExchange(&g_frameDrawIdx, 0);
            InterlockedExchange(&g_inForeground, 0);
            InterlockedExchange(&g_fgDrawCount, 0);
            // Promote this frame's learned list to the stable one the matchers use, THEN clear the
            // learning list. The stable copy is therefore populated when the depth-prime draws
            // arrive at the start of the next frame - which is what makes hiding remove a mesh
            // completely instead of leaving a black silhouette - while still being replaced
            // wholesale every frame rather than accumulating menu geometry.
            {
                const LONG sc = InterlockedCompareExchange(&g_fgSigCount, 0, 0);
                for (LONG i = 0; i < kFgSigMax; ++i)
                    InterlockedExchange(&g_fgStableVerts[i],
                        i < sc ? InterlockedCompareExchange(&g_fgSigVerts[i], 0, 0) : 0);
                InterlockedExchange(&g_fgStableCount, sc);
            }
            InterlockedExchange(&g_fgSigCount, 0);

            // Run 98: find every field holding the view rotation, while we still know what the
            // view rotation IS. Runs only in mode 9 and stops after four samples, so it costs
            // nothing once decided. It must run BEFORE rot[1] is overwritten below - after that
            // write the field we are matching against is the one we just set, and every offset
            // holding a stale value would silently drop out.
            {
                const LONG am = InterlockedCompareExchange(&g_aimMode, 0, 0);
                if (am == 9 || am == 10)
                ScanViewRotFields(ctl, g_camera, g_wantPitch, g_wantYaw);
            }
            InterlockedExchange(&g_aimYawForXi, g_aimYawUU);
            InterlockedExchange(&g_aimPitchForXi, g_aimPitchUU);

            if (InterlockedCompareExchange(&g_aimMode, 0, 0) == 4) {
                rot[1] = g_aimYawUU;
                rot[0] = g_aimPitchUU;
            }
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

// ---- âš ï¸ forcing the BACKBUFFER size does NOT raise UE3's render resolution ----
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
    if (!IniPath(path)) return;

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
    // -1 = off. 13 is the register run 42 identified. Switchable because this touches the matrix
    // path, which is the most fragile code here - runs 15, 19, 20 and 27 all broke something in it,
    // and the failure mode is losing head tracking entirely.
    g_extraCamReg = GetPrivateProfileIntA("Render", "ExtraCamReg", -1, path);
    if (g_extraCamReg < -1 || g_extraCamReg > 200) g_extraCamReg = -1;
    Log("ini: ExtraCamReg=%d (%s)", g_extraCamReg,
        g_extraCamReg < 0 ? "off - only the scan-locked register is remapped"
                          : "also remap this register per eye (shadow-seam test)");

    // On by default: a head tilt that does not tilt the view is a comfort problem, not a feature,
    // and the whole cost is two multiply-adds on the ~50 constant uploads a frame that carry the
    // camera. Switchable mainly so it can be A/B'd against a build without it.
    LONG rollIni = GetPrivateProfileIntA("Render", "HeadRoll", 1, path);
    if (rollIni < 0 || rollIni > 1) rollIni = 1;
    InterlockedExchange(&g_rollOn, rollIni);
    Log("ini: HeadRoll=%ld (%s)", rollIni,
        rollIni ? "tilting your head tilts the view - NUMPAD1 toggles, NUMPAD2 flips the sign"
                : "off - the horizon stays locked to the headset");

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

    // Below this many primitives a user-pointer draw is treated as a fullscreen post-process quad
    // and passed through unsplit. See the note above Hook_DrawPrimUP.
    g_userPtrMinPrims = GetPrivateProfileIntA("Render", "UserPtrMinPrims", 3, path);
    if (g_userPtrMinPrims < 0 || g_userPtrMinPrims > 1000) g_userPtrMinPrims = 3;
    Log("ini: UserPtrMinPrims=%d (user-pointer draws with fewer primitives pass through unsplit"
        " - they are fullscreen quads)", g_userPtrMinPrims);

    g_upQuadByVertex = GetPrivateProfileIntA("Render", "UserPtrQuadByVertex", 1, path);
    if (g_upQuadByVertex < 0 || g_upQuadByVertex > 1) g_upQuadByVertex = 1;
    Log("ini: UserPtrQuadByVertex=%d (%s)", g_upQuadByVertex,
        g_upQuadByVertex ? "a small draw is only a fullscreen quad if its first vertex is in NDC"
                         : "primitive count alone - the run-46 behaviour, decals stay unsplit");

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
    // Kept for the PAUSE-to-quit key. The focus window is the game's real top-level window, so
    // WM_CLOSE through it is the same shutdown path as clicking the X - the engine saves and
    // exits normally. That matters here: ENVIRONMENT.md records that every copy of the game
    // shares ONE save profile, so killing the process mid-write is a real way to lose it.
    if (w) g_gameWindow = w;

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

        // ---- âš ï¸ is the 4096 cap actually the HARDWARE's? Nobody has ever asked ----
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
        // 43 = Clear, 47 = SetViewport. Same vtable indexing already proven by Present at 17,
        // SetRenderTarget at 37 and DrawPrimitive at 81 - these two sit between them and need no
        // separate verification. Both only RECORD; see the note on DpgRecord.
        g_origClear       = (PFN_Clear)PatchVTable(*out, 43, (void*)&Hook_Clear);
        g_origSetViewport = (PFN_SetViewport)PatchVTable(*out, 47, (void*)&Hook_SetViewport);
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

// Called from DllMain, which is the only place early enough - see the note above g_autoResMode.
//
// MinHook is initialised here rather than in InstallViewHook because this runs first. At
// DLL_PROCESS_ATTACH during process startup the loader has created exactly one thread, so
// MH_EnableHook's thread-freeze has nothing to suspend and the usual loader-lock hazard does not
// arise. InstallViewHook now tolerates MH_ERROR_ALREADY_INITIALIZED for the same reason.
//
// EVERY failure path leaves the command line untouched. A malformed command line means the game
// does not launch at all, which is far worse than typing -ResX yourself.
void InstallAutoResolution() {
    char path[MAX_PATH]{};
    if (!IniPath(path)) return;

    g_autoResMode = GetPrivateProfileIntA("Render", "AutoResolution", 1, path);
    if (g_autoResMode != 1) { Log("auto-resolution: OFF by ini"); return; }

    g_autoResW = (UINT)GetPrivateProfileIntA("Render", "AutoResX", 0, path);
    g_autoResH = (UINT)GetPrivateProfileIntA("Render", "AutoResY", 0, path);
    if (!g_autoResW || !g_autoResH) {
        Log("auto-resolution: nothing cached yet - this run uses your command line as typed."
            " The headset's size will be written to the ini and injected from the next launch.");
        return;
    }

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        Log("auto-resolution: MH_Initialize failed - command line left alone"); return;
    }
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) { Log("auto-resolution: kernel32 handle failed - command line left alone"); return; }
    void* tA = (void*)GetProcAddress(k32, "GetCommandLineA");
    void* tW = (void*)GetProcAddress(k32, "GetCommandLineW");
    if (!tA || !tW) { Log("auto-resolution: GetCommandLine not found - left alone"); return; }

    if (MH_CreateHook(tA, (void*)&Hook_GetCommandLineA, (void**)&g_origGetCmdA) != MH_OK ||
        MH_CreateHook(tW, (void*)&Hook_GetCommandLineW, (void**)&g_origGetCmdW) != MH_OK) {
        Log("auto-resolution: MH_CreateHook failed - command line left alone"); return;
    }
    // Build the string BEFORE enabling, so the hooks can never be live with nothing to hand back.
    LPSTR orig = g_origGetCmdA ? g_origGetCmdA() : nullptr;
    if (!BuildAutoCmdLine(orig)) {
        Log("auto-resolution: not injecting (%s)",
            (orig && HasResArg(orig)) ? "you passed -ResX yourself, which always wins"
                                      : "no usable cached size");
        return;                                   // hooks created but never enabled
    }
    if (MH_EnableHook(tA) != MH_OK || MH_EnableHook(tW) != MH_OK) {
        Log("auto-resolution: MH_EnableHook failed - command line left alone"); return;
    }
    g_autoResInjected = true;
    Log("auto-resolution: injecting -ResX=%u -ResY=%u -windowed (cached from the headset last run)",
        g_autoResW, g_autoResH);
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
        InstallAutoResolution();
        // Earliest possible attempt, for the same reason the command line needs one: if UE3
        // enumerates input devices during startup and caches "no pad", a hook installed at the
        // first Present is already too late. If XINPUT1_3.dll is not mapped yet this does nothing
        // and Present retries.
        InstallXInputHook();
    }
    return TRUE;
}
