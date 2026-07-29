// openxr_chain_probe - Singularity VR mod, spike 3
//
// Closes the remaining leg of the make-or-break question. Spike 1 proved
//   D3D9Ex shared texture -> D3D11
// works zero-copy. This proves the rest:
//   D3D9Ex shared texture -> D3D11 -> OpenXR swapchain -> compositor
// from a 32-BIT process, which is the bitness Singularity forces on us.
//
// Runs standalone (no game involved). Reports how far it gets, so it is still
// informative when the headset is off - instance creation alone answers whether
// the Oculus 32-bit runtime accepts us.
//
// Build: .\build.ps1     Run: .\openxr_chain_probe.exe

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstdint>
#include <vector>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")

namespace {

int g_stage = 0;
void Stage(const char* s) { printf("\n[%d] %s\n", ++g_stage, s); }
void Ok(const char* s)    { printf("  [ PASS ] %s\n", s); }
void OkF(const char* fmt, ...) { printf("  [ PASS ] "); va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); printf("\n"); }
void Bad(const char* s, XrResult r) { printf("  [ FAIL ] %s (XrResult=%d)\n", s, (int)r); }
void Note(const char* s)  { printf("  [ note ] %s\n", s); }

bool Check(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) { Ok(what); return true; }
    Bad(what, r);
    return false;
}

const UINT kTexW = 512, kTexH = 512;
const D3DCOLOR kClearArgb = D3DCOLOR_ARGB(255, 64, 128, 191);

// xrCreateSession faulted inside the Oculus runtime on the first run. Isolate the call
// behind SEH so we can report it instead of dying silently. Kept in its own function with
// no C++ objects in scope, which __try/__except requires.
XrResult SafeCreateSession(XrInstance inst, const XrSessionCreateInfo* sci,
                           XrSession* out, bool* faulted) {
    *faulted = false;
    __try {
        return xrCreateSession(inst, sci, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

}  // namespace

int main() {
    // Unbuffered: this probe can crash mid-way, and a full stdio buffer would
    // discard everything printed before the fault - which is exactly the output
    // needed to find where it died.
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("=============================================================\n");
    printf(" Singularity VR - spike 3: D3D9 -> D3D11 -> OpenXR\n");
    printf(" process is %d-bit (must be 32 to match the game)\n", (int)(sizeof(void*) * 8));
    printf("=============================================================\n");

    // ---------------------------------------------------------------- instance
    Stage("xrCreateInstance - does the 32-bit Oculus runtime accept us?");

    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, { XR_TYPE_EXTENSION_PROPERTIES });
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
    OkF("runtime advertises %u extensions", extCount);

    bool hasD3D11 = false;
    for (const auto& e : exts) {
        if (strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) hasD3D11 = true;
    }
    if (hasD3D11) Ok("XR_KHR_D3D11_enable is available");
    else { Bad("XR_KHR_D3D11_enable NOT available", XR_ERROR_EXTENSION_NOT_PRESENT);
           Note("without a D3D11 binding there is no path from D3D9 to the compositor."); return 1; }

    const char* enabled[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    XrInstance instance = XR_NULL_HANDLE;

    // Try the SDK's current API version first, then fall back to 1.0. Our loader is 1.1.x;
    // a runtime that only implements OpenXR 1.0 will refuse a 1.1 instance outright, and
    // some report that as INITIALIZATION_FAILED rather than API_VERSION_UNSUPPORTED.
    struct { XrVersion ver; const char* label; } attempts[] = {
        { XR_CURRENT_API_VERSION, "XR_CURRENT_API_VERSION (1.1)" },
        { XR_MAKE_VERSION(1, 0, 34), "OpenXR 1.0" },
    };

    XrResult instRes = XR_ERROR_RUNTIME_FAILURE;
    for (const auto& a : attempts) {
        XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
        strcpy_s(ici.applicationInfo.applicationName, "SingularityVR probe");
        ici.applicationInfo.apiVersion = a.ver;
        ici.enabledExtensionCount = 1;
        ici.enabledExtensionNames = enabled;

        instRes = xrCreateInstance(&ici, &instance);
        if (XR_SUCCEEDED(instRes)) {
            printf("  [ PASS ] xrCreateInstance using %s\n", a.label);
            break;
        }
        printf("  [ .... ] %s rejected (XrResult=%d), trying next\n", a.label, (int)instRes);
    }
    if (XR_FAILED(instRes)) {
        Bad("xrCreateInstance (all API versions)", instRes);
        Note("-2 RUNTIME_UNAVAILABLE = service not running.");
        Note("-4 INITIALIZATION_FAILED with a connected headset usually means the runtime");
        Note("   is present but not in a state where it can serve an app.");
        return 1;
    }

    XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &ip)))
        OkF("runtime: %s (version %llu)", ip.runtimeName, (unsigned long long)ip.runtimeVersion);

    // ---------------------------------------------------------------- system
    Stage("xrGetSystem - is an HMD present?");
    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    if (!Check(xrGetSystem(instance, &sgi, &systemId), "xrGetSystem")) {
        Note("headset not connected / not ready. Instance creation above already proved");
        Note("the 32-bit runtime works, which was the architectural question.");
        xrDestroyInstance(instance);
        return 1;
    }
    XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
    if (XR_SUCCEEDED(xrGetSystemProperties(instance, systemId, &sp)))
        OkF("system: %s  maxSwapchain=%ux%u", sp.systemName,
            sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight);

    // ------------------------------------------------- graphics requirements
    Stage("D3D11 graphics requirements - which adapter must we use?");
    PFN_xrGetD3D11GraphicsRequirementsKHR pfnReq = nullptr;
    xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&pfnReq);
    if (!pfnReq) { Bad("xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)", XR_ERROR_FUNCTION_UNSUPPORTED); return 1; }

    XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    if (!Check(pfnReq(instance, systemId, &req), "xrGetD3D11GraphicsRequirementsKHR")) return 1;
    OkF("required adapter LUID = %08lX:%08lX, min feature level 0x%04X",
        req.adapterLuid.HighPart, req.adapterLuid.LowPart, (unsigned)req.minFeatureLevel);
    Note("the mod's D3D11 device MUST sit on this adapter, or the compositor rejects the textures.");

    // ---------------------------------------------------------------- D3D11
    Stage("create D3D11 device on the runtime's adapter");
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) { printf("  [ FAIL ] CreateDXGIFactory1\n"); return 1; }
    IDXGIAdapter1* adapter = nullptr;
    IDXGIAdapter1* chosen = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == req.adapterLuid.LowPart && d.AdapterLuid.HighPart == req.adapterLuid.HighPart) {
            chosen = adapter;
            // NOTE: do not use wprintf here - mixing wide and narrow output on one stream
            // sets conflicting stream orientation, which is undefined behaviour. %S prints
            // a wide string through the narrow stream safely.
            printf("  [ PASS ] matched adapter: %S\n", d.Description);
            break;
        }
        adapter->Release();
    }
    if (!chosen) { printf("  [ FAIL ] no adapter matching the runtime LUID\n"); return 1; }

    ID3D11Device* dev11 = nullptr; ID3D11DeviceContext* ctx11 = nullptr;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL gotLevel{};
    // BGRA support: the compositor works in B8G8R8A8, which is also what the D3D9 shared
    // texture surfaces as. Without this flag some runtimes reject the device.
    UINT devFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, devFlags, levels, 1,
                                 D3D11_SDK_VERSION, &dev11, &gotLevel, &ctx11))) {
        printf("  [ FAIL ] D3D11CreateDevice on runtime adapter\n"); return 1;
    }
    Ok("D3D11 device created on the runtime's adapter (BGRA support enabled)");

    // ---------------------------------------------------------------- session
    Stage("xrCreateSession");
    XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    binding.device = dev11;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = systemId;
    XrSession session = XR_NULL_HANDLE;

    printf("  [ note ] binding.type=%d device=%p systemId=%llu\n",
           (int)binding.type, (void*)binding.device, (unsigned long long)systemId);

    bool faulted = false;
    XrResult sessRes = SafeCreateSession(instance, &sci, &session, &faulted);
    if (faulted) {
        printf("  [ CRASH ] xrCreateSession raised an access violation inside the runtime.\n");
        Note("this is a fault in the Oculus 32-bit runtime, not a returned error code.");
        Note("next thing to try: the SteamVR runtime, which also ships a 32-bit loader.");
        return 2;
    }
    if (!Check(sessRes, "xrCreateSession")) return 1;

    // ---------------------------------------------------------------- views
    Stage("view configuration + swapchain");
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data());
    OkF("stereo view count = %u, recommended per-eye %ux%u", viewCount,
        views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight);

    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr);
    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, fmts.data());
    printf("  [ note ] runtime swapchain formats:");
    for (auto f : fmts) printf(" %lld", (long long)f);
    printf("\n");

    // B8G8R8A8 matches what the D3D9 shared texture surfaces as (spike 1 saw format 87).
    int64_t chosenFmt = fmts.empty() ? 0 : fmts[0];
    for (auto f : fmts) if (f == DXGI_FORMAT_B8G8R8A8_UNORM) { chosenFmt = f; break; }
    OkF("using swapchain format %lld", (long long)chosenFmt);

    XrSwapchainCreateInfo scci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    scci.format = chosenFmt;
    scci.sampleCount = 1;
    scci.width = views[0].recommendedImageRectWidth;
    scci.height = views[0].recommendedImageRectHeight;
    scci.faceCount = 1;
    scci.arraySize = 1;
    scci.mipCount = 1;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    if (!Check(xrCreateSwapchain(session, &scci, &swapchain), "xrCreateSwapchain")) return 1;

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(swapchain, 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    xrEnumerateSwapchainImages(swapchain, imgCount, &imgCount,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    OkF("swapchain has %u images, each an ID3D11Texture2D", imgCount);

    // ------------------------------------------------------- the D3D9 source
    Stage("D3D9Ex source texture -> D3D11 (repeat of spike 1, in-process)");
    IDirect3D9Ex* d3d9ex = nullptr;
    if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9ex))) { printf("  [ FAIL ] Direct3DCreate9Ex\n"); return 1; }
    WNDCLASSEXW wc{ sizeof(wc) }; wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"SingVRChain";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"SingVRChain", L"probe", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
    pp.hDeviceWindow = hwnd; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9Ex* devEx = nullptr;
    if (FAILED(d3d9ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, nullptr, &devEx))) {
        printf("  [ FAIL ] CreateDeviceEx\n"); return 1;
    }

    HANDLE shared = nullptr;
    IDirect3DTexture9* tex9 = nullptr;
    if (FAILED(devEx->CreateTexture(kTexW, kTexH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, &tex9, &shared)) || !shared) {
        printf("  [ FAIL ] shared render target\n"); return 1;
    }
    {
        IDirect3DSurface9* s = nullptr;
        tex9->GetSurfaceLevel(0, &s);
        devEx->SetRenderTarget(0, s);
        devEx->Clear(0, nullptr, D3DCLEAR_TARGET, kClearArgb, 1.0f, 0);
        s->Release();
        IDirect3DQuery9* q = nullptr;
        if (SUCCEEDED(devEx->CreateQuery(D3DQUERYTYPE_EVENT, &q)) && q) {
            q->Issue(D3DISSUE_END);
            int spins = 0; while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE && ++spins < 1000000) {}
            q->Release();
        }
    }
    ID3D11Texture2D* fromD3D9 = nullptr;
    if (FAILED(dev11->OpenSharedResource(shared, __uuidof(ID3D11Texture2D), (void**)&fromD3D9))) {
        printf("  [ FAIL ] OpenSharedResource on the XR adapter's device\n");
        Note("spike 1 passed on the default adapter; failing here would mean the runtime's");
        Note("adapter differs from the one D3D9 chose - a multi-GPU hazard worth knowing.");
        return 1;
    }
    Ok("D3D9 texture opened by the D3D11 device that OpenXR is bound to");

    // ---------------------------------------------------------------- frames
    Stage("run frames - copy the D3D9 image into the XR swapchain and submit");
    XrReferenceSpaceCreateInfo rsci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space = XR_NULL_HANDLE;
    Check(xrCreateReferenceSpace(session, &rsci, &space), "xrCreateReferenceSpace");

    bool running = false, submitted = false;
    int guard = 0;
    while (guard++ < 600) {
        XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* ss = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                printf("  [ note ] session state -> %d\n", (int)ss->state);
                if (ss->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (Check(xrBeginSession(session, &bi), "xrBeginSession")) running = true;
                } else if (ss->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session); running = false; guard = 100000;
                }
            }
            ev = { XR_TYPE_EVENT_DATA_BUFFER };
        }
        if (!running) { Sleep(10); continue; }

        XrFrameState fs{ XR_TYPE_FRAME_STATE };
        if (XR_FAILED(xrWaitFrame(session, nullptr, &fs))) break;
        xrBeginFrame(session, nullptr);

        if (fs.shouldRender) {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            xrAcquireSwapchainImage(swapchain, &ai, &idx);
            XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(swapchain, &wi);

            // THE POINT: D3D9-authored pixels going into a compositor-owned texture.
            D3D11_BOX box{ 0, 0, 0, kTexW, kTexH, 1 };
            ctx11->CopySubresourceRegion(images[idx].texture, 0, 0, 0, 0, fromD3D9, 0, &box);

            XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(swapchain, &ri);

            uint32_t viewCountOut = 0;
            std::vector<XrView> xrViews(viewCount, { XR_TYPE_VIEW });
            XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = space;
            XrViewState vs{ XR_TYPE_VIEW_STATE };
            xrLocateViews(session, &vli, &vs, viewCount, &viewCountOut, xrViews.data());

            std::vector<XrCompositionLayerProjectionView> projViews(viewCount);
            for (uint32_t i = 0; i < viewCount; ++i) {
                projViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                projViews[i].pose = xrViews[i].pose;
                projViews[i].fov  = xrViews[i].fov;
                projViews[i].subImage.swapchain = swapchain;
                projViews[i].subImage.imageRect.extent = { (int32_t)scci.width, (int32_t)scci.height };
            }
            XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
            layer.space = space;
            layer.viewCount = viewCount;
            layer.views = projViews.data();
            const XrCompositionLayerBaseHeader* layers[] = { reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer) };

            XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
            fei.displayTime = fs.predictedDisplayTime;
            fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            fei.layerCount = 1;
            fei.layers = layers;
            if (XR_SUCCEEDED(xrEndFrame(session, &fei)) && !submitted) {
                submitted = true;
                printf("  [ PASS ] *** FRAME SUBMITTED: D3D9 -> D3D11 -> OpenXR compositor ***\n");
                printf("  [ note ] you should see a solid blue-grey image in the headset.\n");
            }
        } else {
            XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
            fei.displayTime = fs.predictedDisplayTime;
            fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            xrEndFrame(session, &fei);
        }
        if (submitted && guard > 240) break;   // a few seconds of visible output, then stop
    }

    printf("\n-------------------------------------------------------------\n");
    printf(" full chain %s\n", submitted ? "PROVEN end to end" : "did NOT complete a frame submit");
    printf("-------------------------------------------------------------\n");

    if (fromD3D9) fromD3D9->Release();
    if (tex9) tex9->Release();
    if (devEx) devEx->Release();
    if (d3d9ex) d3d9ex->Release();
    if (swapchain) xrDestroySwapchain(swapchain);
    if (space) xrDestroySpace(space);
    if (session) xrDestroySession(session);
    if (ctx11) ctx11->Release();
    if (dev11) dev11->Release();
    if (chosen) chosen->Release();
    if (factory) factory->Release();
    if (instance) xrDestroyInstance(instance);
    return submitted ? 0 : 1;
}
