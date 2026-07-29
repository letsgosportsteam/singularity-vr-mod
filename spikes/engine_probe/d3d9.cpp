// engine_probe - Singularity VR mod, spike 4
//
// Verifies, at runtime, the static findings in ENGINE_NOTES.md. Everything here was
// inferred from Ghidra and must be confirmed against a live process before any hook is
// built on it. Read-only: nothing is patched, nothing is written into the game.
//
// Answers in one game run:
//   1. Is GNatives (0x01cc8330) a real dispatch table? How many slots are still
//      execUndefined? Is the page writable (i.e. can we hook by pointer swap)?
//   2. Is 0x01CD4A1C really the GNames TArray {Data, Count, Max}? Do the entries
//      decode as readable UE3 name strings?
//   3. What is the FNameEntry layout - where does the text actually start?
//
// Ships as a d3d9.dll proxy (same shell as the pool probe) and dumps once, a few frames
// into rendering, by which point the engine has interned all its names.
//
// Log: %LOCALAPPDATA%\SingularityVR\engine_probe.log

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>
#include <cmath>

namespace {

// ---- addresses under test (image base 0x400000, no ASLR - see ENGINE_NOTES) ----
const uintptr_t kGNatives       = 0x01cc8330;
const uintptr_t kExecUndefined  = 0x0049f870;
const uintptr_t kGNamesTArray   = 0x01CD4A1C;   // {Data, Count, Max}
const uintptr_t kFNameCtor      = 0x004affd0;
const uintptr_t kFFrameStep     = 0x00487950;

CRITICAL_SECTION g_lock;
char g_log[MAX_PATH] = {};
bool g_ready = false;

void LogInit() {
    InitializeCriticalSection(&g_lock);
    char base[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) {
        strcat_s(base, "\\SingularityVR");
        CreateDirectoryA(base, nullptr);
        sprintf_s(g_log, "%s\\engine_probe.log", base);
        FILE* f = nullptr;
        if (fopen_s(&f, g_log, "w") == 0 && f) fclose(f);
        g_ready = true;
    }
}

void Log(const char* fmt, ...) {
    if (!g_ready) return;
    EnterCriticalSection(&g_lock);
    FILE* f = nullptr;
    if (fopen_s(&f, g_log, "a") == 0 && f) {
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f); fclose(f);
    }
    LeaveCriticalSection(&g_lock);
}

bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
    if (mbi.Protect & bad) return false;
    return ((uintptr_t)p + n) <= ((uintptr_t)mbi.BaseAddress + mbi.RegionSize);
}

const char* ProtName(DWORD p) {
    if (p & PAGE_EXECUTE_READWRITE) return "RWX";
    if (p & PAGE_READWRITE)         return "RW";
    if (p & PAGE_EXECUTE_READ)      return "RX";
    if (p & PAGE_READONLY)          return "R";
    return "?";
}

// ---------------------------------------------------------------- GNatives
void ProbeGNatives() {
    Log("");
    Log("===================== GNatives @ 0x%08X =====================", kGNatives);

    if (!Readable((void*)kGNatives, 256 * sizeof(void*))) {
        Log("  NOT READABLE - address is wrong, or the table is elsewhere.");
        return;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery((void*)kGNatives, &mbi, sizeof(mbi));
    Log("  page protection: %s (0x%08lX)  -> %s", ProtName(mbi.Protect), mbi.Protect,
        (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
            ? "WRITABLE: pointer-swap hooking works directly"
            : "read-only: VirtualProtect needed before hooking");

    auto* tbl = reinterpret_cast<uintptr_t*>(kGNatives);
    int undefined = 0, plausible = 0, junk = 0;
    for (int i = 0; i < 256; ++i) {
        uintptr_t v = tbl[i];
        if (v == kExecUndefined) undefined++;
        else if (v > 0x401000 && v < 0x1F53ABE) plausible++;   // inside .text
        else junk++;
    }
    Log("  256 slots: %d execUndefined, %d plausible handlers, %d junk",
        undefined, plausible, junk);
    if (junk > 32) {
        Log("  >>> too much junk - this is probably NOT GNatives. Do not hook it.");
        return;
    }
    Log("  VERDICT: looks like a real dispatch table.");

    // The two opcodes that carry script function calls. Numbering must be confirmed for
    // this UE3 revision - if these point at execUndefined, the numbering differs.
    struct { int op; const char* name; } interesting[] = {
        { 0x1B, "EX_VirtualFunction" },
        { 0x1C, "EX_FinalFunction" },
        { 0x12, "EX_Context" },
        { 0x19, "EX_Context(alt)" },
    };
    Log("  -- candidate script-call opcodes --");
    for (auto& e : interesting) {
        uintptr_t v = tbl[e.op];
        Log("    [0x%02X] %-20s = 0x%08X%s", e.op, e.name, v,
            v == kExecUndefined ? "   <-- execUndefined (numbering differs!)" : "");
    }

    // Run 1 showed slots 0x00-0x1F are ALL execUndefined, so the registered handlers live
    // higher up. List every slot that is NOT execUndefined - that is the real answer about
    // which opcodes this build actually dispatches through the table.
    Log("  -- ALL registered (non-execUndefined) slots --");
    for (int i = 0; i < 256; ++i) {
        uintptr_t v = tbl[i];
        if (v != kExecUndefined) Log("    [0x%02X] = 0x%08X", i, v);
    }
}

// ---------------------------------------------------------------- GNames
void ProbeGNames() {
    Log("");
    Log("===================== GNames TArray @ 0x%08X =====================", kGNamesTArray);

    if (!Readable((void*)kGNamesTArray, 12)) { Log("  NOT READABLE"); return; }

    auto* arr = reinterpret_cast<uintptr_t*>(kGNamesTArray);
    uintptr_t data = arr[0];
    int32_t count = (int32_t)arr[1];
    int32_t maxN  = (int32_t)arr[2];
    Log("  Data=0x%08X  Count=%d  Max=%d", data, count, maxN);

    if (count <= 0 || count > 500000 || maxN < count || !Readable((void*)data, 4)) {
        Log("  >>> does not look like a valid TArray. The layout guess is wrong.");
        Log("  (a live UE3 game should have tens of thousands of interned names)");
        return;
    }
    Log("  VERDICT: plausible TArray with %d names.", count);

    // FNameEntry layout is assumed { INT Index; FNameEntry* HashNext; TCHAR Name[]; }
    // so text begins at +0x08. Hex-dump the first entry so we can correct if wrong.
    auto* entries = reinterpret_cast<uintptr_t*>(data);
    for (int i = 0; i < 3 && i < count; ++i) {
        uintptr_t e = entries[i];
        if (!e || !Readable((void*)e, 0x40)) { Log("  entry[%d] unreadable", i); continue; }
        auto* b = reinterpret_cast<uint8_t*>(e);
        char hex[128] = {}; int o = 0;
        for (int k = 0; k < 32; ++k) o += sprintf_s(hex + o, sizeof(hex) - o, "%02X ", b[k]);
        Log("  entry[%d] @0x%08X: %s", i, e, hex);
    }

    // CORRECTED from run 1: the hex showed "None" / "ByteProperty" / "IntProperty" starting
    // at +0x10 as plain ANSI, not +0x08 UTF-16. The dword at +0x08 was 0, 2, 4 for entries
    // 0, 1, 2 - i.e. (index << 1), with the low bit flagging a wide string (UE3 packs the
    // ANSI/UNICODE flag there). Entries are variable length and tightly packed.
    Log("  -- decoding names: header 0x10, text ANSI unless index bit0 set --");
    int shown = 0;
    for (int i = 0; i < count && shown < 30; ++i) {
        uintptr_t e = entries[i];
        if (!e || !Readable((void*)e, 0x50)) continue;
        uint32_t idxField = *reinterpret_cast<uint32_t*>(e + 8);
        bool wide = (idxField & 1) != 0;
        char narrow[80] = {};
        int n = 0;
        if (wide) {
            auto* w = reinterpret_cast<const wchar_t*>(e + 0x10);
            for (; n < 76 && w[n]; ++n) narrow[n] = (w[n] >= 32 && w[n] < 127) ? (char)w[n] : '?';
        } else {
            auto* a = reinterpret_cast<const char*>(e + 0x10);
            for (; n < 76 && a[n]; ++n) narrow[n] = (a[n] >= 32 && a[n] < 127) ? a[n] : '?';
        }
        narrow[n] = 0;
        if (n > 0) { Log("    [%5d] idx=%-6u %s%s", i, idxField >> 1, wide ? "(W) " : "", narrow); shown++; }
    }

    // Spot-check names we care about, proving we can resolve script names at runtime.
    Log("  -- searching for camera-related names --");
    const char* wanted[] = { "CalcCamera", "GetPlayerViewPoint", "ProcessViewRotation",
                             "PlayerCamera", "RvPlayerCamera", "FOVAngle", "Tick" };
    for (const char* w : wanted) {
        int found = -1;
        for (int i = 0; i < count && found < 0; ++i) {
            uintptr_t e = entries[i];
            if (!e || !Readable((void*)e, 0x50)) continue;
            if ((*reinterpret_cast<uint32_t*>(e + 8)) & 1) continue;   // skip wide
            if (_stricmp(reinterpret_cast<const char*>(e + 0x10), w) == 0) found = i;
        }
        Log("    %-22s %s", w, found >= 0 ? "FOUND" : "not present");
        if (found >= 0) Log("        at GNames[%d]  entry=0x%08X", found, entries[found]);
    }
}

// ---------------------------------------------------------------- name lookup
const uintptr_t kGObjectsTArray = 0x01CD4A4C;   // confirmed run 2

uintptr_t GNamesData() {
    if (!Readable((void*)kGNamesTArray, 12)) return 0;
    return *reinterpret_cast<uintptr_t*>(kGNamesTArray);
}
int32_t GNamesCount() {
    if (!Readable((void*)kGNamesTArray, 12)) return 0;
    return *reinterpret_cast<int32_t*>(kGNamesTArray + 4);
}

// Resolve an FName index to text. Layout confirmed run 2: entry+0x10 is the string,
// ANSI unless bit0 of the dword at entry+0x08 is set.
bool NameFromIndex(int32_t idx, char* buf, size_t cap) {
    buf[0] = 0;
    uintptr_t data = GNamesData();
    int32_t count = GNamesCount();
    if (!data || idx < 0 || idx >= count) return false;
    if (!Readable((void*)(data + idx * 4), 4)) return false;
    uintptr_t e = reinterpret_cast<uintptr_t*>(data)[idx];
    if (!e || !Readable((void*)e, 0x40)) return false;
    bool wide = (*reinterpret_cast<uint32_t*>(e + 8)) & 1;
    size_t n = 0;
    if (wide) {
        auto* w = reinterpret_cast<const wchar_t*>(e + 0x10);
        for (; n < cap - 1 && w[n]; ++n) buf[n] = (w[n] >= 32 && w[n] < 127) ? (char)w[n] : '?';
    } else {
        auto* a = reinterpret_cast<const char*>(e + 0x10);
        for (; n < cap - 1 && a[n]; ++n) buf[n] = (a[n] >= 32 && a[n] < 127) ? a[n] : '?';
    }
    buf[n] = 0;
    return n > 0;
}

// ---------------------------------------------------------------- UObject layout
// The UObject header layout differs between UE3 builds, so derive it rather than assume.
// Score every dword offset by how often it decodes as a valid GNames index across many
// objects; the Name field wins decisively.
int g_nameOffset  = -1;
int g_classOffset = -1;

void ProbeUObjectLayout() {
    Log("");
    Log("===================== UObject layout (auto-detected) =====================");
    if (!Readable((void*)kGObjectsTArray, 12)) { Log("  GObjects unreadable"); return; }
    uintptr_t data = *reinterpret_cast<uintptr_t*>(kGObjectsTArray);
    int32_t count = *reinterpret_cast<int32_t*>(kGObjectsTArray + 4);
    int32_t nameCount = GNamesCount();
    Log("  GObjects Data=0x%08X Count=%d   GNames Count=%d", data, count, nameCount);
    if (!data || count < 100) return;

    auto* objs = reinterpret_cast<uintptr_t*>(data);

    // sample objects spread through the table
    const int kSamples = 400;
    int scores[16] = {};
    char tmp[128];
    for (int s = 0; s < kSamples; ++s) {
        int i = (count / kSamples) * s;
        if (!Readable((void*)(data + i * 4), 4)) continue;
        uintptr_t o = objs[i];
        if (!o || !Readable((void*)o, 0x40)) continue;
        for (int off = 0; off < 16; ++off) {
            int32_t v = *reinterpret_cast<int32_t*>(o + off * 4);
            if (v > 0 && v < nameCount && NameFromIndex(v, tmp, sizeof(tmp))) scores[off]++;
        }
    }
    Log("  -- dword offsets scored as 'valid FName index' (out of ~%d) --", kSamples);
    int best = -1, bestScore = 0;
    for (int off = 0; off < 16; ++off) {
        if (scores[off] > 0) Log("     +0x%02X : %d", off * 4, scores[off]);
        if (scores[off] > bestScore) { bestScore = scores[off]; best = off; }
    }
    if (best < 0 || bestScore < kSamples / 2) {
        Log("  no offset decodes reliably - layout not recovered.");
        return;
    }
    g_nameOffset = best * 4;
    Log("  => Name (FName index) at +0x%02X  (%d/%d objects)", g_nameOffset, bestScore, kSamples);

    // Class pointer: an offset whose value is another object that itself has a valid name.
    int clsScore[16] = {};
    for (int s = 0; s < kSamples; ++s) {
        int i = (count / kSamples) * s;
        if (!Readable((void*)(data + i * 4), 4)) continue;
        uintptr_t o = objs[i];
        if (!o || !Readable((void*)o, 0x40)) continue;
        for (int off = 0; off < 16; ++off) {
            uintptr_t p = *reinterpret_cast<uintptr_t*>(o + off * 4);
            if (p < 0x10000 || !Readable((void*)p, 0x40)) continue;
            if ((int)(off * 4) == g_nameOffset) continue;
            int32_t nm = *reinterpret_cast<int32_t*>(p + g_nameOffset);
            if (nm > 0 && nm < nameCount && NameFromIndex(nm, tmp, sizeof(tmp))) clsScore[off]++;
        }
    }
    Log("  -- offsets pointing at another *named* object (Class/Outer candidates) --");
    int cbest = -1, cbestScore = 0;
    for (int off = 0; off < 16; ++off) {
        if (clsScore[off] > kSamples / 4) Log("     +0x%02X : %d", off * 4, clsScore[off]);
        if (clsScore[off] > cbestScore) { cbestScore = clsScore[off]; cbest = off; }
    }
    if (cbest >= 0) {
        g_classOffset = cbest * 4;
        Log("  => strongest object-pointer field at +0x%02X (Class or Outer)", g_classOffset);
    }

    // show some real object names to prove it
    Log("  -- sample object names --");
    int shown = 0;
    for (int i = 0; i < count && shown < 15; ++i) {
        if (!Readable((void*)(data + i * 4), 4)) continue;
        uintptr_t o = objs[i];
        if (!o || !Readable((void*)o, 0x40)) continue;
        int32_t nm = *reinterpret_cast<int32_t*>(o + g_nameOffset);
        if (NameFromIndex(nm, tmp, sizeof(tmp))) {
            char cls[128] = "?";
            if (g_classOffset >= 0) {
                uintptr_t cp = *reinterpret_cast<uintptr_t*>(o + g_classOffset);
                if (cp > 0x10000 && Readable((void*)cp, 0x40))
                    NameFromIndex(*reinterpret_cast<int32_t*>(cp + g_nameOffset), cls, sizeof(cls));
            }
            Log("     [%6d] 0x%08X  %-32s (field@+0x%02X -> %s)", i, o, tmp, g_classOffset, cls);
            shown++;
        }
    }
}

// ---------------------------------------------------------------- find the camera
void FindCameraObjects() {
    Log("");
    Log("===================== hunting camera / controller objects =====================");
    if (g_nameOffset < 0) { Log("  layout unknown - skipped"); return; }

    uintptr_t data = *reinterpret_cast<uintptr_t*>(kGObjectsTArray);
    int32_t count = *reinterpret_cast<int32_t*>(kGObjectsTArray + 4);
    auto* objs = reinterpret_cast<uintptr_t*>(data);

    char nm[128];

    // Step 1: locate the UClass objects by name. Run 3 showed +0x28 is Outer (TextBuffer->Core,
    // ViewTarget->PlayerController), NOT Class - so find the classes by name instead.
    struct ClassRef { const char* name; uintptr_t addr; };
    ClassRef classes[] = {
        { "RvPlayerCamera",     0 }, { "RvPlayerController", 0 },
        { "PlayerController",   0 }, { "Camera",             0 },
        { "RvPawn",             0 },
    };
    for (int i = 0; i < count; ++i) {
        if (!Readable((void*)(data + i * 4), 4)) continue;
        uintptr_t o = objs[i];
        if (!o || !Readable((void*)o, 0x40)) continue;
        if (!NameFromIndex(*reinterpret_cast<int32_t*>(o + g_nameOffset), nm, sizeof(nm))) continue;
        for (auto& c : classes) {
            if (!c.addr && _stricmp(nm, c.name) == 0) {
                // the UClass sits directly under a package (Outer = Core/Engine/RvGame)
                uintptr_t outer = *reinterpret_cast<uintptr_t*>(o + 0x28);
                char on[128] = "";
                if (outer > 0x10000 && Readable((void*)outer, 0x40))
                    NameFromIndex(*reinterpret_cast<int32_t*>(outer + g_nameOffset), on, sizeof(on));
                if (!_stricmp(on, "Core") || !_stricmp(on, "Engine") || !_stricmp(on, "RvGame")) {
                    c.addr = o;
                    Log("  UClass %-20s @ 0x%08X (package %s)", c.name, o, on);
                }
            }
        }
    }

    // Step 2: find the Class offset. NOTE +0x28 is EXCLUDED - run 4 proved it is Outer, and
    // it produced 783 false "instances" that were really the classes' own UProperty members
    // (mCameraZOffset, mCamModFOV, ...). Name sits at +0x2C and an FName is 8 bytes
    // {Index, Number}, so Number is +0x30 and Class should be +0x34 - the classic UE3 order
    // ... Outer, Name, Class, ObjectArchetype.
    const int cand[] = { 0x10, 0x14, 0x34, 0x38, 0x3C, 0x40, 0x44 };
    Log("");
    Log("  -- instance counts per candidate Class offset --");
    int bestOff = -1, bestTotal = 0;
    for (int off : cand) {
        int total = 0;
        for (int i = 0; i < count; ++i) {
            if (!Readable((void*)(data + i * 4), 4)) continue;
            uintptr_t o = objs[i];
            if (!o || !Readable((void*)o, (size_t)off + 8)) continue;
            uintptr_t cp = *reinterpret_cast<uintptr_t*>(o + off);
            for (auto& c : classes) if (c.addr && cp == c.addr) { total++; break; }
        }
        if (total > 0) Log("     +0x%02X : %d instances", off, total);
        if (total > bestTotal) { bestTotal = total; bestOff = off; }
    }

    if (bestOff < 0) {
        Log("  NO INSTANCES of any camera/controller class exist right now.");
        Log("  >>> the dump ran outside gameplay (menu/loading). Press F10 while actually");
        Log("      in the game world to capture live objects.");
        return;
    }
    g_classOffset = bestOff;
    Log("  => Class pointer at +0x%02X", g_classOffset);

    // Step 3: list the live instances.
    Log("");
    Log("  -- LIVE INSTANCES --");
    for (auto& c : classes) {
        if (!c.addr) continue;
        for (int i = 0; i < count; ++i) {
            if (!Readable((void*)(data + i * 4), 4)) continue;
            uintptr_t o = objs[i];
            if (!o || !Readable((void*)o, (size_t)g_classOffset + 8)) continue;
            if (*reinterpret_cast<uintptr_t*>(o + g_classOffset) != c.addr) continue;
            NameFromIndex(*reinterpret_cast<int32_t*>(o + g_nameOffset), nm, sizeof(nm));
            char outer[128] = "";
            uintptr_t op = *reinterpret_cast<uintptr_t*>(o + 0x28);
            if (op > 0x10000 && Readable((void*)op, 0x40))
                NameFromIndex(*reinterpret_cast<int32_t*>(op + g_nameOffset), outer, sizeof(outer));
            Log("     %-20s instance 0x%08X  name=%-28s outer=%s", c.name, o, nm, outer);
        }
    }

    // Step 4: the properties of RvPlayerCamera (Outer == that UClass) carry the byte offsets
    // we need in order to read/write camera fields. Find UProperty::Offset empirically: across
    // a class's properties it should be small, ascending-ish and mostly distinct.
    for (auto& c : classes) {
        if (!c.addr || _stricmp(c.name, "RvPlayerCamera") != 0) continue;
        Log("");
        Log("  -- UProperty members of %s (Outer == 0x%08X) --", c.name, c.addr);

        // collect them
        const int kMaxProps = 256;
        uintptr_t props[kMaxProps]; int nProps = 0;
        for (int i = 0; i < count && nProps < kMaxProps; ++i) {
            if (!Readable((void*)(data + i * 4), 4)) continue;
            uintptr_t o = objs[i];
            if (!o || !Readable((void*)o, 0x80)) continue;
            if (*reinterpret_cast<uintptr_t*>(o + 0x28) == c.addr) props[nProps++] = o;
        }
        Log("     %d members found", nProps);
        if (nProps < 4) break;

        // Layout recovered in run 5:
        //   +0x40 Next (UField linked list)   +0x44 ArrayDim   +0x48 ElementSize
        //   +0x64 Offset  (found by scoring: 100/100 small, 86 distinct)
        // Confirmed by element sizes: floats=4, mCamAngles=0x0C (vector),
        // mRootBone=8 (FName), mDeadPOV=0x1C (TPOV = vector+rotator+float).
        const int OFF_ARRAYDIM = 0x44, OFF_ELEMSIZE = 0x48, OFF_OFFSET = 0x64;

        // find the LIVE instance (Class == this UClass, and not the Default__ archetype)
        uintptr_t live = 0;
        for (int i = 0; i < count; ++i) {
            if (!Readable((void*)(data + i * 4), 4)) continue;
            uintptr_t o = objs[i];
            if (!o || !Readable((void*)o, 0x40)) continue;
            if (*reinterpret_cast<uintptr_t*>(o + 0x34) != c.addr) continue;
            if (!NameFromIndex(*reinterpret_cast<int32_t*>(o + g_nameOffset), nm, sizeof(nm))) continue;
            if (strncmp(nm, "Default__", 9) == 0) continue;
            live = o;
            Log("     LIVE instance: 0x%08X (%s)", live, nm);
            break;
        }
        if (!live) Log("     (no live instance - offsets listed without values)");

        Log("     %-30s %6s %5s %4s  %s", "property", "offset", "size", "dim", "live value");
        for (int p = 0; p < nProps; ++p) {
            if (!NameFromIndex(*reinterpret_cast<int32_t*>(props[p] + g_nameOffset), nm, sizeof(nm)))
                continue;
            uint32_t poff = *reinterpret_cast<uint32_t*>(props[p] + OFF_OFFSET);
            uint32_t esz  = *reinterpret_cast<uint32_t*>(props[p] + OFF_ELEMSIZE);
            uint32_t dim  = *reinterpret_cast<uint32_t*>(props[p] + OFF_ARRAYDIM);
            if (poff == 0 && esz == 0) continue;   // not a real data member

            char val[128] = "";
            if (live && poff < 0x4000 && Readable((void*)(live + poff), esz ? esz : 4)) {
                uintptr_t f = live + poff;
                if (esz == 4) {
                    float fv = *reinterpret_cast<float*>(f);
                    // print as float if it looks like one, else as int/hex
                    if (fv > -1e9f && fv < 1e9f && (fv == 0.0f || fabsf(fv) > 1e-6f))
                        sprintf_s(val, "%.3f  (int %d)", fv, *reinterpret_cast<int32_t*>(f));
                    else sprintf_s(val, "0x%08X", *reinterpret_cast<uint32_t*>(f));
                } else if (esz == 12) {
                    auto* v3 = reinterpret_cast<float*>(f);
                    sprintf_s(val, "(%.2f, %.2f, %.2f)", v3[0], v3[1], v3[2]);
                } else if (esz == 8) {
                    sprintf_s(val, "FName idx=%d", *reinterpret_cast<int32_t*>(f));
                } else if (esz == 1) {
                    sprintf_s(val, "%u", *reinterpret_cast<uint8_t*>(f));
                } else if (esz == 0x1C) {   // TPOV: vector + rotator + float
                    auto* v3 = reinterpret_cast<float*>(f);
                    auto* r  = reinterpret_cast<int32_t*>(f + 12);
                    sprintf_s(val, "POV loc(%.1f,%.1f,%.1f) rot(%d,%d,%d) fov %.1f",
                              v3[0], v3[1], v3[2], r[0], r[1], r[2], *reinterpret_cast<float*>(f + 24));
                }
            }
            Log("     %-30s 0x%04X %5u %4u  %s", nm, poff, esz, dim, val);
        }

        // The camera's actual POV probably lives on the PARENT class (Engine.Camera), so
        // find SuperField: a dword in the UClass pointing at another named class object.
        Log("");
        Log("     -- looking for SuperField on the UClass (parent = Camera?) --");
        for (int off = 0x38; off <= 0x60; off += 4) {
            uintptr_t sp = *reinterpret_cast<uintptr_t*>(c.addr + off);
            if (sp < 0x10000 || !Readable((void*)sp, 0x40)) continue;
            char sn[128];
            if (NameFromIndex(*reinterpret_cast<int32_t*>(sp + g_nameOffset), sn, sizeof(sn)))
                Log("        +0x%02X -> 0x%08X  %s", off, sp, sn);
        }
        break;
    }
}

void ProbeAnchors() {
    Log("");
    Log("===================== sanity: code anchors =====================");
    struct { uintptr_t a; const char* n; } anchors[] = {
        { kFNameCtor,     "FName::FName" },
        { kFFrameStep,    "FFrame::Step" },
        { kExecUndefined, "execUndefined" },
    };
    for (auto& a : anchors) {
        if (!Readable((void*)a.a, 16)) { Log("  %-16s 0x%08X UNREADABLE", a.n, a.a); continue; }
        auto* b = reinterpret_cast<uint8_t*>(a.a);
        Log("  %-16s 0x%08X : %02X %02X %02X %02X %02X %02X %02X %02X",
            a.n, a.a, b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);
    }
    Log("  (non-zero, non-CC bytes = the addresses map as expected; no ASLR confirmed)");
}

// ---------------------------------------------------------------- present hook
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
PFN_Present g_origPresent = nullptr;
volatile LONG g_frames = 0;
volatile LONG g_dumped = 0;

void DoDump(const char* why) {
    Log("");
    Log("################ DUMP (%s) ################", why);
    ProbeAnchors();
    ProbeGNatives();
    ProbeGNames();
    ProbeUObjectLayout();
    FindCameraObjects();
    Log("");
    Log("=== dump complete - press F10 again for another, or quit ===");
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* self, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    LONG frame = InterlockedIncrement(&g_frames);

    // F10 = dump on demand. This is the important one: live camera/controller instances
    // only exist during actual gameplay, and an automatic early dump catches the menu.
    static bool prevDown = false;
    bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (down && !prevDown) {
        char why[64];
        sprintf_s(why, "F10 pressed at frame %ld", frame);
        DoDump(why);
    }
    prevDown = down;

    // Late automatic fallback, in case F10 never arrives. ~5000 frames is a couple of
    // minutes in, by which point a level is usually loaded.
    if (frame == 5000 && InterlockedExchange(&g_dumped, 1) == 0) DoDump("automatic, frame 5000");

    return g_origPresent(self, a, b, c, d);
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
        g_origPresent = (PFN_Present)PatchVTable(*out, 17, (void*)&Hook_Present);  // IDirect3DDevice9::Present
        Log("Present hooked; dump scheduled for frame 120");
    }
    return hr;
}

// ---------------------------------------------------------------- passthrough
HMODULE g_real = nullptr;
typedef IDirect3D9* (WINAPI *PFN_D3DC9)(UINT);
typedef HRESULT (WINAPI *PFN_D3DC9Ex)(UINT, IDirect3D9Ex**);
typedef int   (WINAPI *PFN_i_cw)(D3DCOLOR, LPCWSTR);
typedef int   (WINAPI *PFN_i_v)(void);
typedef DWORD (WINAPI *PFN_d_v)(void);
typedef void  (WINAPI *PFN_v_cw)(D3DCOLOR, LPCWSTR);
typedef BOOL  (WINAPI *PFN_b_v)(void);
typedef void  (WINAPI *PFN_v_d)(DWORD);
typedef void  (WINAPI *PFN_v_v)(void);

PFN_D3DC9 p_Create9 = nullptr; PFN_D3DC9Ex p_Create9Ex = nullptr;
PFN_i_cw p_Begin = nullptr; PFN_i_v p_End = nullptr; PFN_d_v p_Status = nullptr;
PFN_v_cw p_Marker = nullptr, p_Region = nullptr; PFN_b_v p_Repeat = nullptr;
PFN_v_d p_Options = nullptr; PFN_v_v p_Mute = nullptr;

bool LoadReal() {
    char sys[MAX_PATH] = {};
    if (!GetSystemDirectoryA(sys, MAX_PATH)) return false;
    strcat_s(sys, "\\d3d9.dll");
    g_real = LoadLibraryA(sys);
    if (!g_real) return false;
    p_Create9   = (PFN_D3DC9)  GetProcAddress(g_real, "Direct3DCreate9");
    p_Create9Ex = (PFN_D3DC9Ex)GetProcAddress(g_real, "Direct3DCreate9Ex");
    p_Begin   = (PFN_i_cw)GetProcAddress(g_real, "D3DPERF_BeginEvent");
    p_End     = (PFN_i_v) GetProcAddress(g_real, "D3DPERF_EndEvent");
    p_Status  = (PFN_d_v) GetProcAddress(g_real, "D3DPERF_GetStatus");
    p_Marker  = (PFN_v_cw)GetProcAddress(g_real, "D3DPERF_SetMarker");
    p_Region  = (PFN_v_cw)GetProcAddress(g_real, "D3DPERF_SetRegion");
    p_Repeat  = (PFN_b_v) GetProcAddress(g_real, "D3DPERF_QueryRepeatFrame");
    p_Options = (PFN_v_d) GetProcAddress(g_real, "D3DPERF_SetOptions");
    p_Mute    = (PFN_v_v) GetProcAddress(g_real, "DebugSetMute");
    return p_Create9 != nullptr;
}

}  // namespace

extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    if (!p_Create9) return nullptr;
    IDirect3D9* d3d = p_Create9(sdk);
    if (d3d) {
        g_origCreateDevice = (PFN_CreateDevice)PatchVTable(d3d, 16, (void*)&Hook_CreateDevice);
        Log("CreateDevice hooked");
    }
    return d3d;
}
HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    return p_Create9Ex ? p_Create9Ex(sdk, out) : E_NOTIMPL;
}
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
        Log("=== engine_probe attached (READ-ONLY - nothing is patched in the engine) ===");
        if (!LoadReal()) { Log("FATAL: could not load real d3d9.dll"); return FALSE; }
    }
    return TRUE;
}
