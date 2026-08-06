# Engine notes — Singularity (UE3, x86, July 2010)

Reverse-engineering knowledge base. Every address below is a **file/preferred address** with
image base `0x00400000`, derived from the **GOG** binary
(`R:\GOG\Singularity\Binaries\Singularity.exe`), analysed in Ghidra 12.1.2.

Steam's copy is the same compiled image with the SteamStub wrapper added, so these should hold
there too — but the mod must still resolve them by **signature scan, not hardcoded address**.

> No game code is reproduced here. Findings are summarised; addresses and observed instruction
> shapes only.

---

## ⭐ The UObject property layout — solved, and it makes any property offset readable

Run 186. UE3 stores every property's byte offset in the object model, so offsets never have to be
scanned for again. **Solved by search against answers we already had**, not guessed: the only
candidate accepted was the one that reproduced `Actor.Location = +0x0054` **and**
`Actor.Rotation = +0x0060`.

| Field | Offset | How to use it |
|---|---|---|
| `UStruct::Children` | **`+0x4C`** | head of the property linked list on a UClass |
| `UField::Next` | **`+0x40`** | next property; `SuperField` is immediately before it at `+0x3C` |
| `UProperty::Offset` | **`+0x64`** | the property's byte offset within an instance |

Walk `Children`, follow `Next`, compare `*(int32*)(prop + 0x2C)` (the `FName` index) against the
name you want, then read `*(int32*)(prop + 0x64)`. Climb `SuperField` for inherited properties.

**Corroborated by a spacing the solver was never asked to satisfy** — `SkeletalMeshComponent`
came out as `Translation +0x190` → `Rotation +0x19C` (+12, an FVector) → `Scale +0x1A8` (+12, an
FRotator) → `Scale3D +0x1AC` (+4, a float). UE3's declared order at exactly its declared sizes.

`RvWeaponFX` (used by the muzzle-FX suppression): `mMuzzleFlash1stPerson +0x068`,
`mLoopingMuzzleFlash1stPerson +0x06C`, `mMuzzleFlashIronsights +0x080`,
`mLoopingMuzzleFlashIronsights +0x084`, `mLoopingMuzzleFXEmitter +0x088`.

**`Engine.Pawn`, reached from `RvPlayerPawnSP`: `Health +0x2E4`, `HealthMax +0x2E8`** — two adjacent
`int32`s, found at **super-chain depth 10**. Used by the heal window to ask whether a health item could
do anything before showing the arms.

**`Engine.Pawn`: `BaseEyeHeight +0x2C4`, `EyeHeight +0x2C8`** — floats, chain depth 10. Measured
`BaseEyeHeight = 70.0` (stable) and `EyeHeight` oscillating 67.4–71.4 with head bob and the crouch
interpolation.

> ⚠️ **`BaseEyeHeight` is measured from the actor's `Location`, which is the CENTRE of the collision
> cylinder — not from the floor.** So 70 UU is not the player's eye height above ground; that is roughly
> `CollisionHeight + BaseEyeHeight`. Converting 70 UU straight to centimetres gives 133 cm and invites
> the wrong conclusion that the character is child-sized. Log `CollisionHeight` before comparing this to
> a real human.

**`RvGameSharedHUD.mHealthPackCount +0x588`** — the live instance is `RvHUD`, found at chain depth 1.
Used by the heal window to skip when the player has no health packs.

> ⚠️ **Find the HUD by the class that DECLARES the property, not by name.** Matching class names for the
> substring `"HUD"` picks up `RvCE_HUDMessage`, a combat-event object, which has none of these properties.
> Resolve `RvGameSharedHUD` and accept only instances whose `SuperField` chain reaches it.
>
> ⚠️ **`IsLive` does NOT apply to the HUD.** A UE3 HUD hangs off the PlayerController, not off
> `PersistentLevel`, so the liveness rule that correctly separates pawn instances from archetypes rejects
> every real HUD. Exclude archetypes by name (`Default__`) instead.

> ⚠️ **Depth 10 is why a "generous" 8-level walk found nothing.** The chain is
> `RvPlayerPawnSP → … → GamePawn → Pawn → Actor → Object`, and it is longer than it looks. Walk to
> `SuperField == 0`; use a depth limit only as a corrupt-pointer backstop, and **report when the limit is
> what stopped the search** — otherwise a truncated walk is indistinguishable from an absent property.

> ⚠️ **Bools share an offset.** `HiddenGame` and `bOwnerNoSee` both report `+0x110`, and the two
> `Absolute*` flags both report `+0x1C4`. That is correct — UE3 packs bools into a shared dword and
> distinguishes them by a bitmask, which this reader does not yet extract. Any bool needs the mask too.

> ⚠️ **Compare name INDICES, not strings.** `NameOf` calls `Readable`, which is a `VirtualQuery`
> syscall. Comparing property names as strings inside the layout search cost roughly a million
> lookups in one frame and froze load-in for seconds (run 188). UE3 interns names, so
> `*(int32*)(obj + 0x2C)` is a stable integer identity — resolve the names you want once, then compare
> ints. This mistake was made three times in six runs; it is the default failure mode of this API.

## 🛑 Writing a component's `Translation` does NOT move the mesh

Run 188, measured. Wrote `SkeletalMeshComponent.Translation` (`+0x190`) with +50 UU on Z, on all
three live meshes, and read back the derived world transform: **it moved by `+0.0` UU in every
case.** The input holds our value; UE3 does not recompose `LocalToWorld` from it without a
reattach.

So relocating the first-person weapon through the object model needs
`BeginDeferredReattach` / `ConditionalUpdateTransform` or the equivalent dirty flag, which is a
materially harder problem than anything else attempted here. **The render-matrix gun transform
remains the only mechanism that moves the gun.**

Also established on the way: the live weapon is `RvWeaponShared ARPistol` under `PersistentLevel`,
with its own `SkeletalMeshComponent` — but that component's transform **never updates**, so it is
the world/pickup mesh, not the one in your hands. The pawn (`RvPlayerPawnSP`) carries two
skeletal mesh components, and those do track the camera (to within 2.0 UU over five samples).

## Confirmed anchors

| Address | What | Confidence |
|---|---|---|
| `0x004affd0` | `FName::FName(const TCHAR* Name, EFindName FindType)` | **high** |
| `0x0090f770` | UE3 autogenerated name registration (22,214 bytes) | **high** |
| `0x00f24770` | references `RvPlayerCamera`, 153 bytes | medium |
| `0x01040f10` | references `RvPlayerController`, 153 bytes | medium |

### `FName::FName` at `0x004affd0`

Every name registration site uses the identical `__thiscall` shape:

```
PUSH 1                    ; EFindName (add-if-missing)
PUSH <ptr to UTF-16 name>
LEA  ECX,[EBP-local]      ; this = &FName temporary
CALL 0x004affd0
MOV  ECX,[EAX]            ; FName.Index
MOV  [<global>],ECX
MOV  EDX,[EAX+4]          ; FName.Number
MOV  [<global+4>],EDX
```

Two int32s (Index, Number) = the standard UE3 `FName` layout. This is a high-value anchor: it is
called thousands of times with a distinctive shape, so it signature-scans reliably.

Note the stores are **interleaved** — the `MOV [global]` for call *N* is emitted while the
arguments for call *N+1* are being pushed. When mapping a name to its global slot, attribute the
stores that follow the `CALL`, not the ones textually adjacent to the `PUSH`.

### `0x0090f770` is a name table, not a camera function

22 KB of nothing but the sequence above, populating a large contiguous array of global `FName`
slots (observed around `0x01cf3dcc`–`0x01cf4a80`). This is UE3's `AutoGenerateNames()`
equivalent: it interns every engine/game name at startup and caches each resulting `FName` in a
fixed global.

Candidate slots (each 8 bytes: Index then Number) — **these still need verifying by resolving the
pushed string pointer to its actual text**, which has not been done yet:

| Name | pushed string ptr | probable FName global |
|---|---|---|
| `CalcCamera` | `0x0186ae48` | `0x01cf3ddc` |
| `GetPlayerViewPoint` | (site `0x00911207`) | `0x01cf42e4` |
| `ProcessViewRotation` | `0x0186cd30` | `0x01cf4a7c` |

If confirmed, these are directly usable: the mod reads the global at runtime to get the live
`FName` index and compares integers, with no string work in the hot path.

---

## Strategic consequence: hook `ProcessEvent`, not individual functions

The camera entry points are **UnrealScript-level names**, not exported C++ functions with
addresses we can detour. Evidence:

- `CalcCamera`, `GetPlayerViewPoint`, `ProcessViewRotation` appear only as arguments to the
  `FName` constructor — i.e. they are interned strings, not call targets.
- `PlayerCamera`, `UpdateViewTarget`, `UpdateRotation`, `FOVAngle` exist as strings with **zero**
  code cross-references at all.

So there is no `APlayerController::eventPlayerCalcView`-style address to hook the way the BioShock
mods do — that approach worked there because Vengeance is a UE2.5 fork with those as native C++
functions. UE3 dispatches script functions dynamically through the name table.

**The correct UE3 approach is to hook `UObject::ProcessEvent`** — the single chokepoint every
UnrealScript call passes through (`void UObject::ProcessEvent(UFunction* Function, void* Parms,
void* Result)`). In the hook, compare `Function->Name` against the cached `FName` for the camera
function and intercept there. This is the standard, well-documented UE3 modding technique and is
far more robust than chasing individual addresses.

### The script VM — located

| Address | What | Confidence |
|---|---|---|
| `0x01cc8330` | **`GNatives`** — the 256-entry opcode dispatch table | **high** |
| `0x004a1990` | `GRegisterNative(INT iNative, const Native& Func)` | **high** |
| `0x0049f870` | `UObject::execUndefined` (37 bytes, logs "Unknown code token") | **high** |
| `0x00487950` | **`FFrame::Step`** (37 bytes) | **high** |
| `0x01ccc734` | `GNativeDuplicate` | medium |
| `0x01cd6d04` | GNatives one-time-init flag | medium |

`GRegisterNative` decompiled to UE3's implementation verbatim — the one-time loop filling 255
slots with `execUndefined`, the duplicate-registration check comparing against it, then
`GNatives[iNative] = Func`. That pins `GNatives` precisely.

`FFrame::Step` decompiles to exactly the canonical body:

```c
byte opcode = **(byte**)(frame + 0x18);   // *Code
*(byte**)(frame + 0x18) += 1;             // Code++
(*(code*)(&GNatives)[opcode])(frame, result);
```

Useful side-fact: **`FFrame::Code` is at offset `0x18`** in `FFrame`.

### `ProcessEvent` was NOT found — and probably does not need to be

`FFrame::Step` has **zero direct callers**: at 37 bytes it is inlined into every call site, so
walking up the call graph to `ProcessInternal` → `ProcessEvent` does not work here. `ProcessEvent`
also has no name string (C++ symbol, no PDB), and the "recursion" string turned out to be
`"GRavenOOMReport recursion!"` — a Raven OOM reporter, unrelated and unreferenced.

**Plan: hook `GNatives` directly** — a writable global array of function pointers at a known,
ASLR-free address. No signature scan, no trampoline, and uninstall is restoring a pointer.

### ...but hooking `GNatives` will NOT intercept script calls — plan abandoned

Runtime dump (2026-07-28, run 2) settled it. The table is genuine and `RW`-writable (no
`VirtualProtect` needed), but **only opcodes `0x36`–`0x5A`, plus `0x60`, are registered**:

```
0x00 - 0x35   ALL execUndefined      <-- includes every core expression token
0x36 - 0x5A   real handlers (37)
0x60          real handler (1)
0xFF          0x00000000
```

`EX_VirtualFunction` (`0x1B`) and `EX_FinalFunction` (`0x1C`) — the two opcodes that actually
carry script function calls — are **`execUndefined`**. Since `FFrame::Step` dispatches through
`GNatives` unconditionally, and the game plainly works, script calls **cannot** be travelling
that path. This build must handle tokens `0x00`–`0x35` in an inlined switch, reserving `GNatives`
for the higher opcodes only.

**So a `GNatives` hook would catch nothing we care about.** The plan recorded earlier — that
swapping `GNatives[0x1B]`/`[0x1C]` substitutes for a `ProcessEvent` hook — is **wrong and is
withdrawn**. The registered range is still a useful map of native handlers (`0x36` lines up with
`EX_DynArrayLength` in stock UE3 numbering, suggesting the numbering itself is standard and it is
only the *registration* that is partial), but it is not a script-call interception point.

**The better path is now object-model access, not function hooking** — see `GObjects` below.

### `GNames` — **CONFIRMED at runtime** (2026-07-28, `spikes/engine_probe/`)

`GNames` TArray at **`0x01CD4A1C`** verified in a live process: `Data=0x099A0000`,
**`Count=49318`**, `Max=56107`. Nearly 50k interned names is exactly right for a UE3 title.

**`FNameEntry` layout — corrected from the static guess:**

| Offset | Field |
|---|---|
| `+0x08` | `Index << 1`, **bit 0 = "text is UTF-16"** (UE3 packs the ANSI/wide flag here) |
| `+0x10` | the name text — **ANSI by default**, not UTF-16 |

Proven by the raw hex: entry 0 = `"None"`, entry 1 = `"ByteProperty"`, entry 2 = `"IntProperty"`
— UE3's canonical first three names — each as single-byte ASCII beginning at `+0x10`. The
index dwords read 0, 2, 4 for entries 0, 1, 2, confirming the `<< 1` encoding.

Entries are **variable length and tightly packed**: entry addresses `0x030C0000` → `+0x15` →
`+0x1D`, matching `0x10` header + strlen + 1 each time.

My original guess (+0x08, UTF-16) was wrong on both offset and encoding.

### `GNames` — original static reasoning (superseded, kept for method)

`FName::FName` (`0x004affd0`) is a 32-byte thunk forwarding to the real interning function
`FUN_004ac030` (774 bytes). The writable globals that function touches:

| Address | Guess |
|---|---|
| `0x01CD4A1C` | **`GNames` TArray — `Data`** |
| `0x01CD4A20` | `GNames.Count` |
| `0x01CD4A24` | `GNames.Max` |
| `0x01CA7F30` | name hash table (likely) |
| `0x01BF13B0` | init flag / unrelated |

Three consecutive 4-byte writable globals is exactly UE3's `TArray {Data, Count, Max}` layout,
which is why this is the pick. **Inferred, not verified** — `spikes/engine_probe/` exists to
confirm it against a live process.

### `GObjects` — **FOUND** at `0x01CD4A4C`

Sitting `0x30` bytes after `GNames`, exactly as the "adjacent globals" hunch predicted.

```
GObjects TArray @ 0x01CD4A4C   Count=112565  Max=136244
  first entry -> 0x0609F500, whose first dword (vtable) = 0x017A15C0, inside .text
```

112,565 live `UObject`s with dereferenceable pointers whose vtables land in `.text` — that is the
real object table.

**Important:** the `Data` pointers of both arrays are heap allocations and **change between
runs** (`GNames.Data` was `0x099A0000` then `0x09950000`). Only the *TArray addresses*
`0x01CD4A1C` / `0x01CD4A4C` are stable. Always read `Data` at runtime; never cache it across
sessions.

### `UObject` layout — auto-detected at runtime

Derived empirically rather than assumed, by scoring every dword offset in the object header on
how often it decodes as a valid `GNames` index across 400 sampled objects:

| Offset | Field | Evidence |
|---|---|---|
| `+0x00` | vtable | points into `.text` |
| **`+0x2C`** | **`Name`** (FName index) | **400/400** objects decoded — decisive |
| **`+0x28`** | **`Outer`** | `TextBuffer`→`Core`, `ViewTarget`→`PlayerController` — package/owner, *not* Class |
| `+0x30` | `Name.Number` | an `FName` is 8 bytes `{Index, Number}` |
| `+0x34` | **`Class`** (strong candidate) | classic UE3 order `… Outer, Name, Class, ObjectArchetype`; scored 6 hits, the right magnitude for real instances |
| `+0x38` | `ObjectArchetype` (likely) | — |

**A detection mistake worth recording.** The first automated `Class`-offset search picked `+0x28`
because 783 objects there pointed at the target `UClass`es. Those were **not instances** — they
were the classes' own `UProperty`/`UFunction` members, whose `Outer` *is* the class. Matching
"points at a UClass" is not sufficient to identify `Class`; `Outer` satisfies it too, and far
more often. Corrected by excluding `+0x28` and reasoning from the `FName` size instead.

The false positives were valuable anyway — they enumerated `RvPlayerCamera`'s members:
`mCameraZOffset`, `mCameraScale`, `mCurrentCameraScale`, `mCamModFOV`, `mCurrentShakeOffset`,
`mAimAtHitCache`, `mOverrides`, `mSplatEmitters`. Those are exactly the fields a VR camera
override needs, and each `UProperty` carries the byte offset required to reach them.

Confirmed by output: objects [0..14] read `TextBuffer`, `Object`, `System`, `Subsystem`,
`PackageMap`, `ObjectSerializer`, `LinkerLoad`, `ScriptStruct`, `Field` — all with `Outer` =
`Core`. Exactly UE3's bootstrap class registration order.

`UClass` objects located (addresses are per-run):

| Class | Package |
|---|---|
| `Camera` | `Engine` |
| `PlayerController` | `Engine` |
| `RvPlayerController` | `RvGame` |
| `RvPlayerCamera` | `RvGame` |

Note `ViewTarget`, `SetPause`, `StartFire`, `ConsoleCommand` etc. appear as objects whose `Outer`
is `PlayerController` — these are the `UProperty`/`UFunction` members of the class, which is how
property offsets will be resolved later.

### Live instances — found (F10 dump during gameplay)

`Class` at **`+0x34`** confirmed. UE3 keeps a class-default archetype alongside the real actor,
and they are trivially distinguishable by name and `Outer`:

| Object | `Outer` | What it is |
|---|---|---|
| `Default__RvPlayerCamera` | `RvGame` | class default object — **not** the live camera |
| **`RvPlayerCamera`** | **`PersistentLevel`** | **the live camera actor** |
| `Default__RvPlayerController` | `RvGame` | class default object |
| **`RvPlayerController`** | **`PersistentLevel`** | **the live controller** |

Rule: the live actor has `Outer == PersistentLevel` and a name without the `Default__` prefix.

**Live instance addresses change between dumps** (the camera moved `0x06260000` → `0x3974D900`
across two F10 presses in one session) — they are recreated on level load. Always locate by
enumeration at runtime; never cache an instance pointer across level transitions.

### `UProperty` layout — recovered

| Offset | Field | Evidence |
|---|---|---|
| `+0x40` | `Next` (UField list) | points at the next property |
| `+0x44` | `ArrayDim` | `mCameraOffsets` = 5, `mCameraTransitionTimes` = 5, rest = 1 |
| `+0x48` | `ElementSize` | float = 4, `mCamAngles` = 0x0C (vector), `mRootBone` = 8 (FName), `mDeadPOV` = **0x1C = TPOV** (vector+rotator+float) |
| **`+0x64`** | **`Offset`** | scored 100/100 small, 86 distinct across `RvPlayerCamera`'s 100 members |

`mDeadPOV` decoding to exactly 28 bytes is the strongest confirmation — that is UE3's `TPOV`
struct size, which only lines up if the whole layout is right.

`RvPlayerCamera` has **100 members**, including `mCameraZOffset`, `mCameraScale`,
`mCurrentCameraScale`, `mCamModFOV`, `mCamAngles`, `mLookAtLocation`, `mDeadPOV`,
`mCameraOffsets[5]`, `mCameraStyle`, `mRootBone`, `mCamBone_Follow`.

`UClass` extras, from the same dump: **`SuperField` at `+0x3C`** (`RvPlayerCamera` → `Camera`,
confirming the class chain) and `ObjectArchetype` at `+0x38` (points at `Default__Class`).

### ⭐ THE CAMERA — `mCurrentPOV` at `+0x0420`

My guess that the POV was inherited from `Engine.Camera` was **wrong** — Raven declares it
directly on `RvPlayerCamera`, and the live values are unmistakable:

| Property | Offset | Size | Live value |
|---|---|---|---|
| **`mCurrentPOV`** | **`0x0420`** | 28 | `loc(19046.1, -22940.4, 181.6) rot(-436, 120, 0) fov 65.0` |
| `mDesiredPOV` | `0x0458` | 28 | same as current — the interpolation target |
| `mTargetPOV` | `0x043C` | 28 | zeroed when unused |
| `mCurrentFOV` | `0x0490` | 4 | `65.0` (agrees with the POV's fov) |
| `mCurrentOffset` | `0x0408` | 12 | `(-400, 0, 64)` |
| `mCamAngles` | `0x05F0` | 12 | `(0, 0, 0)` |
| `mCameraZOffset` | `0x0688` | 4 | `50.0` |
| `mCameraScale` | `0x0680` | 4 | `10.0` |

`TPOV` layout, 28 bytes: `FVector Location` (3 floats) · `FRotator Rotation` (3× int32,
65536 = 360°) · `FLOAT FOV`.

### ⚠️ Correction: the POV structs are COPIES, not the source

Live write testing (spike 5, `spikes/camera_write/`) proved the picture above is only half right:

- **Writing `mCurrentPOV.FOV` works** — the rendered view visibly widens 65° → 110°. FOV is read
  from this struct.
- **Writing `mCurrentPOV.Rotation` does nothing to the view**, despite the write persisting
  perfectly (`survived=120 clobbered=0` every sample, yaw cycling exactly as written). The engine
  neither fights the write nor reads it.

The F6 rotation hunt (snapshot → look around → diff) explains why. The real source is the standard
UE3 **`AActor`** layout on both the camera and the controller:

| Offset | Field |
|---|---|
| **`+0x0054`** | **`AActor::Location`** — `FVector`, 3 floats |
| **`+0x0060`** | **`AActor::Rotation`** — `FRotator`, 3× int32 |

Looking around changed `Rotation` on the controller (`pitch 4200, yaw 17520`) and the camera
(`pitch 4037, yaw 17520`) — yaw identical, pitch smoothed slightly on the camera — and that same
location/rotation pair was then mirrored into **`+0x0328`, `+0x0350`, `mCurrentPOV (+0x0420)` and
`mDesiredPOV (+0x0458)`**. Four downstream copies of one upstream value.

So the POV structs are written *by* the camera update, and read only for FOV. Rotation must be
injected further upstream — at `AActor::Rotation` on the camera or the controller. Spike 5 modes
3 and 4 test exactly that.

**Lesson worth keeping:** a struct containing a rotator is not necessarily *the* rotation. Verify
by writing, not by reading — the read-only evidence pointed confidently at the wrong field.

### ✅ THE CONTROL POINT: `RvPlayerController` + `0x0060` (`AActor::Rotation`)

Write-tested live, all four candidates:

| Write target | View responds? |
|---|---|
| **Controller `AActor::Rotation` `+0x0060`** | ✅ **yes — this is the control point** |
| Camera `AActor::Rotation` `+0x0060` | ❌ no |
| Camera `mCurrentPOV.Rotation` `+0x042C` | ❌ no |
| Camera `mCurrentPOV.FOV` `+0x0438` | ✅ yes |

Confirmed data flow:

```
Controller.Rotation  ──(camera update each frame)──>  Camera.Rotation
                                                       └─> mCurrentPOV / mDesiredPOV / +0x328 / +0x350  ──> view
FOV: set on the camera, read for projection, NOT derived from the controller
```

Anything written **downstream** of the camera update is recomputed from the controller on the next
frame, which is why only the controller sticks. A `Present`-time write is fine — it lands before
the next frame's camera update consumes it.

### ⭐ The view-rotation SOURCE: `FUN_0104e390`

Found 2026-07-28 with a **hardware write breakpoint** on the live controller's pitch dword,
after seven failed attempts to guess the location by writing candidate fields.

The breakpoint reported four in-image writers. The first (`EIP 0x01034121`) sat in a region
Ghidra had never analysed — consistent with a virtual function reached only through a vtable.
Data breakpoints trap *after* the storing instruction, so the real write was just above:

```
01034105  CALL 0x0104e390        ; returns a pointer to an FRotator
0103410a  MOV EDX,[EAX+4]        ; yaw
0103410d  MOV ECX,[EAX]          ; pitch
0103410f  MOV EAX,[EAX+8]        ; roll
01034112  MOV [EDI+0x100c],ECX
0103411e  MOV [ESI+0x60],ECX     ; -> AActor::Rotation (the trap)
```

**`FUN_0104e390` (129 bytes, `__thiscall`) computes the view rotation and returns it**; the
caller copies the result straight into `AActor::Rotation`. Called from `0x01034105` and
`0x0103419d`.

This is the correct hook point. Detour it, adjust the returned `FRotator`, and the engine
consumes our value through its own pipeline — frame-accurate, with the engine's own clamping
and smoothing intact, and no need to fight or out-time anything.

Why the earlier approaches failed, in one line each:

| Attempt | Why it failed |
|---|---|
| write `mCurrentPOV.Rotation` | POV structs are downstream copies |
| write controller `+0x60` | overwritten by this function's result each frame |
| write `+0x60` **and** `+0x5D4` | same — the source recomputes both |
| write every camera sink | same |
| write at `BeginScene` | still before this function runs |
| synthetic mouse input | works *with* the engine, but closed-loop: lag, jitter, menu special-casing |

Yaw appeared to work only because our write happened to survive into the same frame; it was
never actually authoritative.

### ⭐ Camera POSITION — **SOLVED** via the view matrix (2026-07-29, spike 9)

Needed for **both** stereo (per-eye offset is a position offset) and 6-DOF. Three attempts
through the object model failed (recorded below — they are why the working approach is what it
is). What works is intercepting the **world→clip matrix** in the vertex shader constants and
pre-multiplying a translation into it. Confirmed live: the view moved.

| Fact | Value |
|---|---|
| Where | `SetVertexShaderConstantF`, D3D9 device vtable slot **94** |
| Register | **`c0`**, a 4-register block |
| Storage | **ROW** — registers are the rows of a row-vector matrix (`clip = v * M`) |
| Space | **TRANSLATED-WORLD**, not world (see below) |
| Residual | `w = -0.0020`, `fwdLen = 1.0000`, 50 uploads in the scanned frame |

To move the camera by `o`: `M' = T(-o) · M`, i.e. `row3 -= o.x*row0 + o.y*row1 + o.z*row2`.
Exact for any offset including forward, and needs no knowledge of the projection.

#### This engine renders in TRANSLATED-WORLD space

Every match landed on the **origin** probe point, never the camera's world position. So UE3
pre-subtracts the view origin on the CPU before uploading — its standard trick for keeping
float precision usable at large world coordinates — which puts the camera at the shader-space
origin. Two consequences:

- A world-position test alone would have found **nothing**. Probing `(0,0,0)` as a third point
  is what made the scan work at all.
- A world-space *offset* is still the same vector in translated-world space (the translation
  moves the origin, not the axes), so the injection maths is unaffected.

#### ⚠️ Method trap: at the origin, the w test degenerates

The first scan returned **18** candidates, most of them spurious, including several with
`w` of exactly `+1.0000`. The reason is worth remembering: `clip.w` at `p = (0,0,0)` collapses
to `r[3].w` under **both** storage conventions. So at that probe point the test

- cannot tell **ROW** from **COL** — every hit is reported twice, and
- admits **any** matrix whose w constant is near zero, which is most of them.

The zero-cancellation test only carries information when the probe point is far from the
origin. Fixed with an independent second test: for a genuine world→clip matrix the xyz of the
w term **is the camera's forward axis**, and the camera's real facing is known from its
`FRotator`. Requiring agreement rejects unrelated matrices *and* resolves the convention,
because ROW and COL read the forward vector from different components.

Generalised lesson, and the second time this project has hit it: **a test that discriminates
well in one regime can be vacuous in another.** Check what your test degenerates to at the
point you are actually evaluating it.

The direction test settled the convention outright on run 2 — the same register read two ways:

```
** c0  ROW  w=-0.0020  dotFwd=+1.0000  hits=50    <- the view matrix
   c0  COL  w=-0.0020  dotFwd=+0.0023  hits=9     <- same bytes, wrong convention
```

Identical `w`, because at the origin both conventions read the same component. The forward
vector is what separates them, and `+0.0023` means the COL reading is essentially perpendicular
to where the camera actually points — rejected outright.

#### Tolerance must depend on which probe point matched

Run 2 also showed a flat tolerance is wrong. Three extra windows passed (`c8`/`c11`/`c14` COL,
`w` ≈ −10.5, 4 uploads each) beside the real matrix at `w` = −0.0020 with 50 uploads. Their
**3-register spacing** suggests a block of `float3x4` transforms that the sliding 4-register
window straddles. Injecting into those would corrupt something unrelated for no benefit.

The fix follows from what the tolerance is *for*: it absorbs the one-frame staleness of the
camera position being substituted. That applies to the two world-position probes. It does **not**
apply to the origin probe, where the test reduces to "is `r[3].w` near zero" — pure matrix
content, no camera reading, nothing to go stale. So the origin probe gets a tight **1 UU**
tolerance, which separates −0.002 from −10.5 decisively; world probes keep 25 UU.

#### ✅ CPU frustum culling — a non-issue at stereo scale (measured)

Offsetting the camera leaves a **black band** down the edge of the frame: the game culls
geometry on the CPU against its **original** frustum, so anything newly visible after the offset
was never submitted to the GPU at all. Modifying the matrix moves the view but cannot conjure
draw calls that were never issued.

Measured by scanning the backbuffer's edge columns (run 2), rather than judged by eye:

| Offset | Band |
|---|---|
| 300 UU | 35–73 px of 2560 (1.4–2.9%), one 721 px spike |
| **100 UU** | **0 px** |
| 30 / 10 / 3 UU | 0 px |

**It disappears completely at 100 UU and below.** A real interpupillary distance is only a few
UU, so stereo needs no mitigation at all — the planned FOV-widening job (via `mCurrentPOV` FOV
`+0x0438`, still the lever if it is ever needed) can be **dropped**. It would only return for
large positional offsets, which 6-DOF head movement does not produce.

Caveat on the metric: it counts *near-black* columns, so a genuinely dark scene reads as a band
— a 502 px reading appeared with injection **off**. That failure mode only ever inflates the
number, never deflates it, so the zeros are trustworthy and the conclusion holds.

#### The projection layer was claiming an FOV the game never rendered

Until run 3 the OpenXR projection layer submitted `projViews[i].fov = <the headset's FOV>`,
while the game was rendering something else entirely (65° horizontal, 16:9). That is a lie to
the compositor, and it has a specific consequence: **everything is stretched by one uniform
factor**, so head movement, world scale and object size are all wrong *together* — and nothing
can be judged against anything else. This is why 6-DOF scale could not be assessed by eye.

The fix has two halves that only work together:

1. **Tell OpenXR the truth.** The frustum is *read from the matrix*, not assumed: the x and y
   columns of a world→clip matrix are the projection scales times unit world axes, so their
   lengths are the scales and `tan(halfFov)` is the reciprocal. This needs no knowledge of
   whether UE3's `FOVAngle` is horizontal or vertical, or how it folds in aspect — we never
   ask, we read what came out. Same trick as the forward vector, third time it has paid off.
2. **Ask the game to cover the headset**, by writing FOV each frame to `mCurrentPOV +0x0438`
   (plus `mDesiredPOV +0x0470` and `mCurrentFOV +0x0490`, so the interpolator does not drag it
   back). Otherwise the truth is a small correct rectangle floating in black.

Match the headset's **vertical** FOV, not its horizontal. The game renders 16:9 while a
per-eye view is nearly square, so equal vertical FOV leaves the game's horizontal field
comfortably wider than needed — full coverage on both axes, surplus falling outside the eye.
Matching horizontally would leave top and bottom short, which is the visible half of the
aspect mismatch.

Note this does **not** resurrect the culling problem. Widening the FOV widens the engine's own
culling frustum, since the request goes through the engine rather than round it.

#### ⚠️ The engine ACCEPTS a wide FOV but will not HOLD it

Run 4: the wider field read as far more natural, but the image pulsed — a zoom flash on the
monitor, black bars flicking in and out top and bottom in the headset. The self-diagnosis paid
off immediately. Against a steady **127.9°** requested, the matrix reported:

```
125.3  127.2  125.1  123.8  128.7  124.4  126.1  125.4  124.0  124.0  ...  80.7
```

So the write **works** — the engine honours it, and this is not a clamp. But the value drifts
between our Present-time writes: the camera update interpolates back toward its own default
each tick, with the size of each step set by frame duration, and one long frame produced the
80.7° outlier. Both symptoms follow: a rendered FOV swinging 128° → 80° is a visible zoom on
the monitor, and submitting that narrower frustum leaves the vertical badly short, which is the
top-and-bottom bars.

**Asking more politely cannot fix this** — the engine gets the last word before rendering,
which is the same seam problem that defeated seven attempts at rotation.

#### The fix: force the projection in the matrix, and demote the engine FOV to culling

Set the frustum where nothing can argue — in the matrix, on its way to the GPU, on every
upload. Rescaling the x and y columns is exactly a clip-space x/y scale, which is exactly an
FOV change, and it composes correctly after the positional offset.

The engine's FOV then matters **only for CPU culling**, where being roughly right is fine — so
it is now asked for **15% more** than we render with, and its drift becomes harmless as long as
it stays above what we draw.

The submitted OpenXR frustum is the same forced constant rather than the observed value.
Following the observation was the flicker: a submitted frustum that tracked the engine
inherited every wobble. Forced and submitted from one number, the two agree by construction.

Performance note: forcing runs every frame, not just while 6-DOF is on, so the full sliding
scan could not stay in the hot path. Once the matrix is located the hook checks only its
window — a bounds test plus one window test per covering call.

#### Failure modes should decay, not freeze

Run 3 ended with the 6-DOF offset frozen at `(29.8, -30.8, -27.7)` UU — about 0.9 m of
permanent displacement — repeated identically frame after frame. Cause: positional tracking
dropped while orientation kept working (the IMU carries rotation; position needs the cameras),
so the update was skipped and the last value persisted forever with no way back.

Skipping the update looked like the safe choice and was the worst one. The offset now decays
toward neutral when `XR_VIEW_STATE_POSITION_VALID_BIT` is clear, so the view drifts home over
about a second and recovers by itself when tracking returns.

#### Measure in the game's frame, not the headset

The band lives in the backbuffer. Judging it through the HMD would mean judging it through the
aspect mismatch (game renders 16:9, per-eye view is near-square) and the lens distortion as
well — neither of which will exist in the finished mod. Reading the backbuffer directly reports
the same number whether or not anyone is wearing the headset.

#### The three object-model attempts that failed first

**1. Direct writes — no effect.** Wrote a 300 UU constant simultaneously to every plausible
field: camera `AActor::Location +0x54`, `mCurrentOffset +0x408`, `mCurrentPOV` location `+0x420`,
`mCameraZOffset +0x688`, and the controller's `Location`. The view did not move at all.

**2. Hardware write breakpoint on the camera's `Location`** — caught three writers while walking:

| EIP | What it is |
|---|---|
| `0x00A15845` | inside `FUN_00a15550` (858 b) — writes Location XYZ from locals, recurses over a child list at `[ESI+0x160]`. Attachment/hierarchy propagation. **12 callers.** |
| `0x00A0FD0B` | inside `FUN_00a0f6e0` — the same generic actor-move routine already seen writing *rotation* (`0x00a0fd10`). |
| `0x00B75DAB` | a `POP ESI`; the real write was `MOV [ESI+0x20]`. Can only hit our address if another object was reallocated there — noise. |

Unlike rotation, there is **no single upstream source**. Rotation resolved cleanly because its
call site did `CALL FUN_0104e390` then stored the returned `FRotator` directly. Position instead
flows through generic actor plumbing shared with every actor in the game, reached from a dozen
call sites. Detouring that would affect everything that moves, not just the camera.

**3. `FUN_0104e420` — ruled out.** It sits 48 bytes past the rotation source and is called from
the same function, so it looked like a position equivalent. Detoured and logged: first arg is
`0.008` (a delta time), it **returns void**, and the pointer arg reads `(0,0,0)` before *and*
after — it only reads that struct. Injecting `300.0f` into it caused wild spinning, which
confirms the struct is an **`FRotator`** (three `int32`s reading as `0.0` when misinterpreted as
floats) rather than a vector. Signature is `(float deltaTime, FRotator* in, ...)`. Rotation
again, not position.

Lesson: two functions of near-identical size, adjacent in memory, called from the same site can
still do unrelated things. Adjacency was not evidence.

All three failures share one root cause, now understood: **the engine's camera position fields
are inputs to a computation whose output we could not reach, and that output is the matrix.**
Going straight to the matrix skipped the entire problem. Recorded as method: when several
attempts to steer a value upstream all fail, look for the place the value is *consumed* rather
than another place it might be produced.

Stereo still needs a second thing beyond position control: the current architecture renders once
per frame, so two images means alternate-eye submission (one frame stale per eye), engine
re-entry (render the scene twice), or depth reprojection.

### Consequence: head-look and weapon aim are coupled

In UE3 `PlayerController.Rotation` drives **both** the view and the weapon fire trace. Since that
is currently our only working handle on view rotation, VR head tracking will also swing the aim —
the exact problem flagged at project start, now confirmed as real here.

That is acceptable for the early ladder rungs (head turns the whole player: playable, if not
ideal). Decoupling later needs one of:

1. **Hook the fire trace** and feed it a controller-derived aim while the head owns the view —
   what the BioShock reference mod ended up doing (`GetPerfectFireStart`).
2. **Find a hook point between the camera update and the render**, so `Camera.Rotation` can be
   written after the engine derives it but before it is consumed. `BeginScene` is a candidate;
   the camera's own native update function would be better.
3. **`mbExternalOverride`** — a bool in the packed dword at `0x03F8`. The name suggests Raven built
   a sanctioned external-camera path. Worth investigating before engineering around the problem.

Interpolation-rate fields sit alongside (`mDesiredRotationInterpRate`, `mDesiredFOVInterpRate`,
`mCurrentLocationInterpRate` ≈ 100) and will likely need neutralising so the engine does not smooth
away a directly-written HMD pose.

Caveat on bools: `mbInBulletCam`, `mbExternalOverride`, `mbJustLoaded` and seven others all report
offset `0x03F8`. That is UE3 bitfield packing — they share one dword and are distinguished by a
`UBoolProperty::BitMask` field the probe does not yet read. Any bool write must use the mask, not
the whole dword. (`mbExternalOverride` in particular looks worth investigating — an engine-provided
override path could be cleaner than fighting the interpolators.)

### Probe timing — live instances need a gameplay-time dump

Runs 1–3 dumped at frame 120 (~2 s in), which lands at the **main menu**, before any level is
loaded. That is why only `UClass`/`UProperty`/`UFunction` objects appeared and **zero live
instances** — even though the session itself reached real gameplay. `Class` offset detection
depends on finding instances, so it could not resolve either.

Probe v4 dumps on **F10** (plus a fallback at frame 5000). Capture must happen while actually
in the game world.

### Runtime name resolution — working

The probe resolved script names against `GNames` in the live process:

| Name | `GNames` index |
|---|---|
| `Tick` | 336 |
| `RvPlayerCamera` | 7358 |
| `CalcCamera` | 8678 |
| `GetPlayerViewPoint` | 8831 |
| `ProcessViewRotation` | 9049 |
| `FOVAngle` | 17294 |
| `PlayerCamera` | 21868 |

Indices are build-specific and derived at runtime by string lookup — do not hardcode them.

### Revised camera strategy: object-model access

With `GObjects` + `GNames` the mod can enumerate every live object, identify the player
controller / camera by class, and **read and write their properties directly** — no script-call
interception required, which sidesteps the `ProcessEvent` problem entirely.

Sketch:

1. Hook D3D9 `Present` for frame timing (already proven in `spikes/d3d9_pool_probe`).
2. Walk `GObjects` to find the active `RvPlayerCamera` / player controller instance.
3. Write the HMD pose into its location/rotation fields, and FOV into `FOVAngle`.

Open questions before committing:

- **Write timing.** The engine updates the camera during its own tick; values written at the
  wrong point get overwritten. May need to write from a hook that runs after the camera update
  rather than at `Present`.
- **Property offsets.** Reading a named property needs either the `UClass` property list walked
  at runtime (robust) or fixed offsets (brittle). Prefer walking `UProperty` chains — `GNames`
  already gives the name→index mapping needed to match them.

---

## Renderer facts (from the live pool probe, `spikes/d3d9_pool_probe/`)

- Entry point used: `Direct3DCreate9` (**non-Ex**) — so the device must be upgraded for sharing.
- Backbuffer `2560x1440`, **`D3DFMT_A8R8G8B8` (21)**, **no MSAA** (no resolve step needed before
  sharing). *Corrected 2026-07-29 — this originally read `X8R8G8B8`, which is format 22, not 21.
  The mistake propagated into spike 8 and made `GetRenderTargetData` fail with
  `D3DERR_INVALIDCALL`, since it requires identical formats on both surfaces. Read the format
  from the surface descriptor rather than hardcoding it.*
- `CreateDevice` flags `0x142` = `HARDWARE_VERTEXPROCESSING | FPU_PRESERVE | MULTITHREADED`.
- **10,454 `D3DPOOL_MANAGED` allocations vs 47 `DEFAULT`** in one session — the wrapper must
  translate MANAGED→DEFAULT and own the backing store.

## Solved: the `0x00BE6550` crash — unchecked NULL from a runtime-resolved factory

Six identical faults during the 2026-07-28 session. **Not caused by our shim.**

Exception detail from all six minidumps is identical: *tried to **READ** address `0x0`* — a plain
null-pointer dereference, fully deterministic.

The code at `FUN_00be63f0 + 0x160`:

```
PUSH 0x2080300
CALL dword ptr [0x01bfbc40]     ; indirect call through a .data function pointer
MOV  [0x01cf7518],EAX           ; cache the returned object
MOV  EDX,dword ptr [EAX]        ; <-- FAULT: load vtable, EAX == 0
MOV  EAX,dword ptr [EDX + 0x84]
CALL EAX                        ; virtual call
```

A factory/creation call returns an object pointer, which is stored to a global and **immediately
dereferenced for its vtable with no null check**. When creation fails, the game dies here. The
target `0x01bfbc40` lives in **`.data`**, not the IAT — so it is a function pointer resolved at
runtime (`LoadLibrary`/`GetProcAddress` style), consistent with an optional subsystem whose
backing component was unavailable. Single caller: `FUN_011e4620` at `0x011e4a99`.

### Why our shim is exonerated

| Time | Event |
|---|---|
| 18:58:41 | `d3d9.dll` shim installed |
| 19:00:20 – 19:10:24 | **six crashes** |
| *(between)* | user ran the **Steam** copy, which installed missing redistributables |
| 19:19:47 | save profile written — successful gameplay |
| 19:23:56 | pool-probe log written (10,454 allocations, clean exit) |

The shim was present for **both** the crashes and the successful run, so it cannot be the
differentiator. These crashes are the same "the GOG version wouldn't start" problem reported
before any mod work began. Relevant: **the GOG package ships no `redist` folder at all**, while
Steam's does (DirectX, PhysX, VC++) and runs it on first launch.

Causation is inferred from timeline plus a coherent mechanism, not proven — the exact missing
component was not identified.

### ✅ IDENTIFIED (2026-08-04): it is the 32-bit `PhysXLoader.dll`

Reproduced in full, on **both** game copies, with the crash signature captured:

```
0xc06d007e  in KERNELBASE.dll                   delay-load failure: "module could not be found"
0xc0000005  in Singularity.exe +0x007cdde6      the NULL deref immediately after
```

The Steam copy faulted at the **identical offset**, which is what proves it system-wide rather
than an install problem. `C:\Program Files (x86)\NVIDIA Corporation\PhysX\Common\` held
`PhysXLoader64.dll` but **not** the 32-bit `PhysXLoader.dll`, and Singularity is 32-bit. The
versioned engines (`Engine\v2.7.1\`, `v2.7.3\`, …) were all intact — only the loader that bridges
to them was gone.

**Repair:** run the game's own bundled redist, `redist\PhysX_9.09.1112_SystemSoftware.exe`, as
administrator. ⚠️ **Uninstall `NVIDIA PhysX System Software` first.** With a newer version
registered, the 9.09 installer runs in *remove* mode — it took `PhysXLoader64.dll` and the
`physxcudart` files with it and left the registration behind, making things worse. With nothing
registered it installs cleanly and restores both loaders.

The installer also adds `PhysX\Common` to the machine `PATH`, so a process started before the
install may still fail on a stale environment. Reboot if the first launch after repairing fails
the same way.

**For the install guide:** verified by inspection — the GOG package ships **no redist folder and no
installer of any kind** (`__support` is empty; the only exes are the game, `language_setup.exe` and
the uninstaller). A GOG-only user has no local source at all and must get NVIDIA's legacy PhysX
9.09 from nvidia.com. Note this is per-game packaging, not a GOG rule — GOG's *Mirror's Edge* does
ship `__redist/PHYSX` and `__redist/PHYSXLEGACY`.

### ✅ The fix: make the game self-contained, like Mirror's Edge already is

Raven shipped `NxCooking.dll` and `PhysXExtensions.dll` in `Binaries\` but **not**
`PhysXLoader.dll`, so Singularity depends on system state. Mirror's Edge ships its own
`Binaries\PhysXLoader.dll` *plus* a `PhysXLocal\` copy, which is why it sailed through the outage
that killed Singularity.

**Windows searches the application directory first, and this is measured, not assumed.** With a
local copy in `Binaries\` *and* the system copy present, the preflight logs:

```
physx preflight: PhysXLoader.dll resolves to ...\Singularity\Binaries\PhysXLoader.dll
physx preflight:   -> LOCAL to the game folder - independent of system PhysX
```

So one file in `Binaries\` is enough. `spikes/view_matrix/setup_physx.ps1` does it, sourcing the
DLL from the user's own machine so the mod redistributes nothing. `PhysXPreflight()` in
`d3d9.cpp` catches the failure at `DllMain` and shows a message box naming the fix, because
otherwise the crash is silent and the mod gets blamed.

⚠️ **Not total immunity.** The loader still resolves `PhysXCore.dll` through
`PhysX\Engine\v2.7.x\`, and those folders survived the incident untouched — so a local loader
would have prevented *this* failure, but an installer that also strips the engine folders would
still bite. Mirror's Edge hedges that with a local core as well. Untested here.

### Consequence for distribution

This is a latent Raven bug, but it matters to us: a user installing the mod onto a **fresh GOG
install** can hit this crash and reasonably blame the mod. The install guide should tell GOG users
to run the DirectX/PhysX/VC++ redistributables first. Worth reproducing deliberately on a clean
machine before release.

### Also learned from the PE header

- **No ASLR** (`DYNAMIC_BASE` clear), `ImageBase` `0x00400000` — so static addresses in this file
  map **directly** to runtime addresses. Signature scanning is still right for build portability,
  but debugging is much easier than it would be otherwise.
- **`LARGE_ADDRESS_AWARE` is already set.** The LAA patch the reference project needed for its
  32-bit stereo memory budget is **not** required here.
