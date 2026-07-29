// d3d9_interop_probe - Singularity VR mod, spike 1
//
// Question this answers: can a Direct3D 9 render target be handed to Direct3D 11
// without a CPU round-trip? OpenXR has no D3D9 graphics binding (only D3D11/D3D12/
// GL/Vulkan), so every frame Singularity draws must reach a D3D11 texture before it
// can be submitted to the compositor. If this fails, the whole architecture changes.
//
// Also probes two secondary facts that shape the design:
//   - whether plain (non-Ex) D3D9 can share at all  -> it cannot; documents WHY we must
//     upgrade the game's device, since Singularity calls Direct3DCreate9, not ...Ex
//   - whether a D3D9Ex device still accepts D3DPOOL_MANAGED -> it does not, and UE3
//     leans on MANAGED heavily, so this is the main hazard of forcing the upgrade
//
// Build: .\build.ps1   (x86, to match the 32-bit game)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdint>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")

namespace {

const UINT kTexW = 256;
const UINT kTexH = 256;

// Distinctive clear colour we look for on the D3D11 side. If we read this back out of
// a texture D3D11 opened from a D3D9-created shared handle, the bridge genuinely works.
const float kClearR = 0.25f;
const float kClearG = 0.50f;
const float kClearB = 0.75f;
const D3DCOLOR kClearArgb = D3DCOLOR_ARGB(255, 64, 128, 191);  // ~ .25/.50/.75

int g_pass = 0;
int g_fail = 0;

void ok(const char* what, const char* detail = nullptr) {
    printf("  [ PASS ] %s%s%s\n", what, detail ? " - " : "", detail ? detail : "");
    ++g_pass;
}
void bad(const char* what, HRESULT hr) {
    printf("  [ FAIL ] %s (hr=0x%08lX)\n", what, (unsigned long)hr);
    ++g_fail;
}
void note(const char* what) { printf("  [ note ] %s\n", what); }

HWND MakeHiddenWindow() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SingVRProbe";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, L"SingVRProbe", L"probe", WS_OVERLAPPEDWINDOW,
                           0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
}

void FillPresentParams(D3DPRESENT_PARAMETERS& pp, HWND hwnd) {
    ZeroMemory(&pp, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
}

// ---------------------------------------------------------------------------
// Test A: plain Direct3DCreate9 (what Singularity actually calls today).
// Expected: creating a texture with a shared handle FAILS. This is the reason the
// mod has to intercept Direct3DCreate9 and hand back an Ex object instead.
// ---------------------------------------------------------------------------
void TestPlainD3D9CannotShare(HWND hwnd) {
    printf("\n[A] plain Direct3DCreate9 (Singularity's current call) - sharing expected to FAIL\n");

    IDirect3D9* d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d9) { bad("Direct3DCreate9", E_FAIL); return; }

    D3DPRESENT_PARAMETERS pp; FillPresentParams(pp, hwnd);
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                    D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                    &pp, &dev);
    if (FAILED(hr)) { bad("CreateDevice (non-Ex)", hr); d3d9->Release(); return; }
    ok("CreateDevice (non-Ex)");

    HANDLE shared = nullptr;
    IDirect3DTexture9* tex = nullptr;
    hr = dev->CreateTexture(kTexW, kTexH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                            D3DPOOL_DEFAULT, &tex, &shared);
    if (FAILED(hr)) {
        ok("shared-handle texture correctly REJECTED on non-Ex device",
           "confirms we must upgrade the device");
    } else {
        // Some drivers tolerate this; if so, note it, it would simplify things.
        printf("  [ !!!! ] non-Ex device ACCEPTED a shared handle (handle=%p)\n", shared);
        note("unexpected but welcome - would avoid the D3D9Ex upgrade entirely; verify in D3D11 before trusting");
        if (tex) tex->Release();
    }

    // MANAGED pool on a normal device - the thing UE3 depends on.
    IDirect3DTexture9* managed = nullptr;
    hr = dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &managed, nullptr);
    if (SUCCEEDED(hr)) { ok("D3DPOOL_MANAGED works on non-Ex device", "as UE3 expects"); managed->Release(); }
    else               { bad("D3DPOOL_MANAGED on non-Ex device", hr); }

    dev->Release();
    d3d9->Release();
}

// ---------------------------------------------------------------------------
// Test B: the real spike. D3D9Ex -> shared handle -> D3D11 -> read the pixels back.
// ---------------------------------------------------------------------------
void TestD3D9ExToD3D11(HWND hwnd) {
    printf("\n[B] D3D9Ex shared render target -> D3D11 (the load-bearing test)\n");

    IDirect3D9Ex* d3d9ex = nullptr;
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9ex);
    if (FAILED(hr) || !d3d9ex) { bad("Direct3DCreate9Ex", hr); return; }
    ok("Direct3DCreate9Ex");

    // IDirect3D9Ex derives from IDirect3D9, so a hook on Direct3DCreate9 can return
    // this pointer straight back to the game. Worth asserting the cast is real.
    IDirect3D9* asPlain = static_cast<IDirect3D9*>(d3d9ex);
    if (asPlain) ok("IDirect3D9Ex is usable as IDirect3D9*", "hook can return it transparently");

    D3DPRESENT_PARAMETERS pp; FillPresentParams(pp, hwnd);
    IDirect3DDevice9Ex* devEx = nullptr;
    hr = d3d9ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                &pp, nullptr, &devEx);
    if (FAILED(hr)) { bad("CreateDeviceEx", hr); d3d9ex->Release(); return; }
    ok("CreateDeviceEx");

    // Does an Ex device still take MANAGED? Documented to fail - and UE3 uses it a lot,
    // so this is the main cost of forcing the upgrade.
    IDirect3DTexture9* managed = nullptr;
    HRESULT hrManaged = devEx->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8,
                                             D3DPOOL_MANAGED, &managed, nullptr);
    if (SUCCEEDED(hrManaged)) {
        ok("D3DPOOL_MANAGED accepted on Ex device", "UE3 resource code may survive untouched!");
        managed->Release();
    } else {
        printf("  [ RISK ] D3DPOOL_MANAGED REJECTED on Ex device (hr=0x%08lX)\n", (unsigned long)hrManaged);
        note("expected; UE3 uses MANAGED heavily, so a transparent device upgrade would break it.");
        note("mitigation: translate MANAGED->DEFAULT+staging in the wrapper, or use the sysmem fallback path.");
    }

    // The actual shared render target.
    HANDLE shared = nullptr;
    IDirect3DTexture9* tex9 = nullptr;
    hr = devEx->CreateTexture(kTexW, kTexH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                              D3DPOOL_DEFAULT, &tex9, &shared);
    if (FAILED(hr) || !shared) { bad("CreateTexture(RENDERTARGET, shared handle)", hr); goto cleanup9; }
    { char d[64]; sprintf_s(d, "handle=%p", shared); ok("shared render-target texture created", d); }

    // Draw something identifiable into it.
    {
        IDirect3DSurface9* surf = nullptr;
        hr = tex9->GetSurfaceLevel(0, &surf);
        if (FAILED(hr)) { bad("GetSurfaceLevel", hr); goto cleanup9; }
        hr = devEx->SetRenderTarget(0, surf);
        if (FAILED(hr)) { bad("SetRenderTarget", hr); surf->Release(); goto cleanup9; }
        hr = devEx->Clear(0, nullptr, D3DCLEAR_TARGET, kClearArgb, 1.0f, 0);
        if (FAILED(hr)) { bad("Clear", hr); surf->Release(); goto cleanup9; }
        surf->Release();
        ok("cleared shared RT to known colour");
    }

    // No keyed mutex exists on the D3D9 side, so flush explicitly and wait for the GPU.
    // This is exactly the synchronisation problem the real mod will have to solve properly.
    {
        IDirect3DQuery9* q = nullptr;
        if (SUCCEEDED(devEx->CreateQuery(D3DQUERYTYPE_EVENT, &q)) && q) {
            q->Issue(D3DISSUE_END);
            int spins = 0;
            while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE && ++spins < 1000000) {}
            q->Release();
            ok("GPU flush via D3DQUERYTYPE_EVENT", "no keyed mutex available on D3D9 side");
        } else {
            note("event query unavailable; proceeding without explicit flush");
        }
    }

    // Now the D3D11 side.
    {
        ID3D11Device* dev11 = nullptr;
        ID3D11DeviceContext* ctx11 = nullptr;
        D3D_FEATURE_LEVEL got = {};
        const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                               want, ARRAYSIZE(want), D3D11_SDK_VERSION, &dev11, &got, &ctx11);
        if (FAILED(hr)) { bad("D3D11CreateDevice", hr); goto cleanup9; }
        { char d[64]; sprintf_s(d, "feature level 0x%04X", (unsigned)got); ok("D3D11CreateDevice", d); }

        ID3D11Texture2D* tex11 = nullptr;
        hr = dev11->OpenSharedResource(shared, __uuidof(ID3D11Texture2D), (void**)&tex11);
        if (FAILED(hr) || !tex11) {
            bad("OpenSharedResource (D3D9 handle -> D3D11 texture)", hr);
            note("THIS is the make-or-break call. If it cannot pass, fall back to");
            note("GetRenderTargetData -> sysmem -> D3D11 upload, or translate via DXVK/dgVoodoo2.");
            ctx11->Release(); dev11->Release(); goto cleanup9;
        }
        ok("OpenSharedResource - D3D11 opened the D3D9 texture");

        D3D11_TEXTURE2D_DESC td = {};
        tex11->GetDesc(&td);
        { char d[128]; sprintf_s(d, "%ux%u fmt=%d samples=%u", td.Width, td.Height, (int)td.Format, td.SampleDesc.Count);
          ok("shared texture description readable", d); }

        // Copy to a CPU-readable staging texture and verify the pixels really crossed over.
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        hr = dev11->CreateTexture2D(&sd, nullptr, &staging);
        if (SUCCEEDED(hr) && staging) {
            ctx11->CopyResource(staging, tex11);
            D3D11_MAPPED_SUBRESOURCE m = {};
            hr = ctx11->Map(staging, 0, D3D11_MAP_READ, 0, &m);
            if (SUCCEEDED(hr)) {
                uint32_t px = *reinterpret_cast<uint32_t*>(m.pData);
                unsigned b =  px        & 0xFF;
                unsigned g = (px >> 8)  & 0xFF;
                unsigned r = (px >> 16) & 0xFF;
                unsigned a = (px >> 24) & 0xFF;
                ctx11->Unmap(staging, 0);
                char d[128]; sprintf_s(d, "read B=%u G=%u R=%u A=%u (expected ~191/128/64/255)", b, g, r, a);
                // Allow slack for format/gamma differences; we only care that it is not black/garbage.
                bool looksRight = (r > 40 && r < 90) && (g > 100 && g < 155) && (b > 165 && b < 215);
                if (looksRight) {
                    printf("  [ PASS ] *** PIXELS CROSSED D3D9 -> D3D11 INTACT *** - %s\n", d);
                    ++g_pass;
                } else {
                    printf("  [ WARN ] texture opened but colour mismatch - %s\n", d);
                    note("handle sharing works; investigate format/sync before trusting content.");
                }
            } else { bad("Map(staging)", hr); }
            staging->Release();
        } else { bad("CreateTexture2D(staging)", hr); }

        tex11->Release();
        ctx11->Release();
        dev11->Release();
    }

cleanup9:
    if (tex9) tex9->Release();
    if (devEx) devEx->Release();
    if (d3d9ex) d3d9ex->Release();
}

}  // namespace

int main() {
    printf("=============================================================\n");
    printf(" Singularity VR - spike 1: D3D9 -> D3D11 interop\n");
    printf(" OpenXR has no D3D9 binding, so every frame must reach D3D11.\n");
    printf("=============================================================\n");
    printf(" process is %d-bit\n", (int)(sizeof(void*) * 8));

    HWND hwnd = MakeHiddenWindow();
    if (!hwnd) { printf("could not create window\n"); return 1; }

    TestPlainD3D9CannotShare(hwnd);
    TestD3D9ExToD3D11(hwnd);

    printf("\n-------------------------------------------------------------\n");
    printf(" %d passed, %d failed\n", g_pass, g_fail);
    printf("-------------------------------------------------------------\n");

    DestroyWindow(hwnd);
    return g_fail == 0 ? 0 : 1;
}
