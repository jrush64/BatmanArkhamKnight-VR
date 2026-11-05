#include "ak_xr.h"

#include "ak_menu.h"
#include "xr_types.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include "shaders/dibr_depth_debug_2d_bytecode.h"
#include "shaders/dibr_depth_debug_msaa4_bytecode.h"
#include "shaders/dibr_ps_2d_bytecode.h"
#include "shaders/dibr_ps_msaa4_bytecode.h"
#include "shaders/dibr_vs_bytecode.h"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// AK -> headset, Milestone 1 (mono). The OpenXR submission sequence here is ported from the
// PROVEN Mass Effect mod (clean/xr_session.cpp) — same call order, same ABI types (xr_types.h).
// Self-contained: own logger, own state, no dependency on the probe's globals. The probe wires
// AKXr::Frame() into its Present hook after the original Present.

namespace
{
using namespace MELEVR::Xr;

// ---- self-contained logging (next to dxgi.dll, plus OutputDebugString) --------------------
HMODULE g_selfModule = nullptr;  // set in TryInit via GetModuleHandleExW on this function's address

void XrLog(const char* format, ...) noexcept
{
    char message[1024] = {};
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    SYSTEMTIME t = {};
    GetLocalTime(&t);
    char line[1200] = {};
    snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u | %s\r\n",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, message);

    OutputDebugStringA(line);

    // Resolve "AK_XR.log" next to this module (= the game directory) once.
    static wchar_t s_path[MAX_PATH] = {};
    static bool s_pathReady = false;
    if (!s_pathReady)
    {
        wchar_t mod[MAX_PATH] = {};
        if (g_selfModule != nullptr && GetModuleFileNameW(g_selfModule, mod, MAX_PATH) > 0)
        {
            wchar_t* slash = wcsrchr(mod, L'\\');
            if (slash != nullptr)
            {
                *slash = L'\0';
                _snwprintf_s(s_path, MAX_PATH, _TRUNCATE, L"%s\\AK_XR.log", mod);
                s_pathReady = true;
            }
        }
        if (!s_pathReady)
        {
            wcscpy_s(s_path, MAX_PATH, L"AK_XR.log");  // fallback: working dir
            s_pathReady = true;
        }
    }
    HANDLE f = CreateFileW(s_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD wrote = 0;
        WriteFile(f, line, static_cast<DWORD>(strlen(line)), &wrote, nullptr);
        CloseHandle(f);
    }
}

std::string FmtXr(XrResult r) { return "XrResult " + std::to_string(static_cast<int>(r)); }

// ---- session state ------------------------------------------------------------------------
HMODULE g_loader = nullptr;
PFN_xrGetInstanceProcAddr g_getProc = nullptr;
XrInstance g_instance = nullptr;
XrSystemId g_systemId = 0;
XrSession g_session = nullptr;
XrSpace g_appSpace = nullptr;   // LOCAL, re-anchored on recenter — locate + submit live here (recentered frame)
XrSpace g_localSpace = nullptr; // BASE LOCAL (never re-anchored) — used to read the head pose at recenter time

Functions g_fn;
using PFN_xrGetInstanceProperties = XrResult (*)(XrInstance, XrInstanceProperties*);
PFN_xrGetInstanceProperties g_getInstanceProperties = nullptr;

struct Eye
{
    XrSwapchain swapchain = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    ID3D11Texture2D* images[8] = {};
    uint32_t imageCount = 0;
};
Eye g_eyes[2];


ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
int64_t g_colorFormat = 0;
uint32_t g_bbWidth = 0;
uint32_t g_bbHeight = 0;

int g_sessionState = 0;
bool g_sessionRunning = false;
std::atomic_bool g_ready{false};      // session + swapchains created
std::atomic_bool g_xrEnabled{true};   // VR restored; akvr.ini/F5 can still disable session submission.
std::atomic_bool g_bringupStarted{false};
std::atomic_bool g_bringupFailed{false};
bool g_hasRefreshRateExt = false;     // XR_FB_display_refresh_rate enabled => mod can query true headset Hz
bool g_refreshRateApplied = false;    // latched once the AER pace has been set from the real refresh rate

// latest head orientation (degrees), for the log + M2 camera wiring.
// These are what the camera injection reads, so they HOLD STILL while the Insert tuner is open.
std::atomic<float> g_headYaw{0.0f};
std::atomic<float> g_headPitch{0.0f};
std::atomic<float> g_headRoll{0.0f};
// Never-frozen copies, purely so the tuner's HMD readout stays informative while the camera is held.
std::atomic<float> g_headYawLive{0.0f};
std::atomic<float> g_headPitchLive{0.0f};
std::atomic<float> g_headRollLive{0.0f};
std::atomic_bool g_headValid{false};
std::atomic_bool g_submitHeadPose{true};   // match the pose injected into Arkham's gameplay camera
std::atomic<float> g_projM00{0.0f};        // game projection cot(hHalf); 0 = not read yet (use fallback fov)
std::atomic<float> g_projM11{0.0f};        // game projection cot(vHalf)
std::atomic<float> g_hsHalfH{0.0f};        // headset eye FOV half-angle (rad), horizontal — the FOV-fill target
std::atomic<float> g_hsHalfV{0.0f};        // headset eye FOV half-angle (rad), vertical
bool  g_fovFillEnabled = false;            // native game FOV; post-cull widening causes geometry pop/flicker
float g_fovFillH = 1.0f;                    // menu mirror: VR Fill H multiplier
float g_fovFillV = 1.0f;                    // menu mirror: VR Fill V multiplier

// M2b — AER stereo depth. The game renders ONE image per frame; the mod alternates the per-eye camera shift
// (left this frame, right next frame), copy each finished frame into that eye's history texture, and
// submit both histories (the other eye ~1 frame old = AER). Real geometric parallax = depth.
std::atomic_bool g_stereoOn{true};         // live DIBR stereo by default; NUMPAD * still toggles.
std::atomic<int> g_activeEye{0};           // which eye the game is rendering THIS frame (0=L,1=R)
ID3D11Texture2D* g_history[2] = {nullptr, nullptr};
bool g_historyReady = false;
XrQuaternionf g_historyOrient[2] = {};     // head orientation each eye was rendered at (pose-tag = no ghosting)
bool g_historyHasOrient[2] = {false, false};

template <typename T>
void SafeRelease(T*& p) noexcept
{
    if (p != nullptr)
    {
        p->Release();
        p = nullptr;
    }
}

// DIBR proof path: one real game render, synthesize the right eye from color + captured scene depth.
std::atomic_bool g_dibrEnabled{true}; // live DIBR path: depth capture feeds the right-eye reprojection.
std::atomic_bool g_dibrAutoFp{false}; // AUTO-DIBR: forced on for first-person/aim even in AER mode (gun fuses via depth).
std::atomic_bool g_dibrProofArmed{false};
std::atomic_bool g_dibrDepthMetaOnly{false};
std::atomic_bool g_dibrDepthCopyOnly{false};
std::atomic_bool g_dibrDepthSrvOnly{false};
std::atomic_bool g_dibrDepthSrvCopyOnly{false};
std::atomic_bool g_dibrShaderOnly{false};
std::atomic_bool g_dibrShaderCompileOnly{false};
std::atomic_bool g_dibrCompilerLoadOnly{false};
std::atomic_bool g_dibrShaderCreateOnly{false};
std::atomic_bool g_dibrStateCreateOnly{false};
std::atomic_bool g_dibrSetupOnly{false};
std::atomic_bool g_dibrSetupDepthCopyOnly{false};
std::atomic_bool g_dibrColorWarpOnly{false};
std::atomic_bool g_dibrColorCopyOnly{false};
std::atomic_bool g_dibrBindRestoreOnly{false};
std::atomic_bool g_dibrDrawOnly{false};
std::atomic_bool g_dibrProofHotkeyEnabled{false};
std::atomic_bool g_dibrDepthReady{false};
std::atomic_bool g_dibrDepthFromLiveSrv{false};
ID3D11Texture2D* g_dibrDepthCopy = nullptr;
ID3D11ShaderResourceView* g_dibrDepthSrv = nullptr;
ID3D11Texture2D* g_dibrColorCopy = nullptr;
ID3D11ShaderResourceView* g_dibrColorSrv = nullptr;
ID3D11Texture2D* g_fillSceneRT = nullptr;   // pushed each frame from dxgi (the game's scene color RT) = the FILL source
ID3D11Texture2D* g_dibrWarpTex = nullptr;
ID3D11RenderTargetView* g_dibrWarpRtv = nullptr;
ID3D11VertexShader* g_dibrVs = nullptr;
ID3D11PixelShader* g_dibrPs = nullptr;
ID3D11PixelShader* g_dibrDepthDebugPs = nullptr;
ID3D11SamplerState* g_dibrSampler = nullptr;
ID3D11RasterizerState* g_dibrRaster = nullptr;
ID3D11DepthStencilState* g_dibrDepthState = nullptr;
ID3D11BlendState* g_dibrBlend = nullptr;
ID3D11Buffer* g_dibrParamsCb = nullptr;
UINT g_dibrDepthW = 0;
UINT g_dibrDepthH = 0;
UINT g_dibrDepthSamples = 0;
UINT g_dibrColorW = 0;
UINT g_dibrColorH = 0;
UINT g_dibrWarpW = 0;
UINT g_dibrWarpH = 0;
DXGI_FORMAT g_dibrColorFmt = DXGI_FORMAT_UNKNOWN;
bool g_dibrShadersTried = false;
bool g_dibrShadersReady = false;
std::atomic<unsigned> g_dibrLogCount{0};
float g_dibrGain = 0.035f;         // screen-space parallax after near-depth remap.
float g_dibrConvergence = 0.985f;  // kept for older reports; current shader anchors far pixels instead.
float g_dibrSign = 1.0f;
float g_dibrDepthScale = 96.0f;    // AK depth SRV is reverse-ish: raw d, not 1-d, carries scene depth.
float g_dibrEdgeGuard = 3.0f;      // fades warp at depth breaks to reduce ghosting/tearing.
float g_dibrEdgeThreshold = 0.080f;
float g_aerHalfEye = 1.6f;         // UE3 ~50 uu/m: 64 mm IPD = 3.2 uu total, 1.6 uu per eye
float g_aerFpScale = 0.10f;        // first-person separation = g_aerHalfEye * this (gun is ~at camera; needs very little)
bool g_sixDofGameplay = true;      // menu mirror: free head-look in regular gameplay
bool g_sixDofFirstPerson = false;  // reserved until context detection is wired
float g_headYawGain = 1.0f;
float g_headPitchGain = 1.0f;
float g_headSmoothing = 0.0f;
bool g_invertHeadYaw = false;
bool g_invertHeadPitch = false;
int g_recenterKey = 0x24;          // menu mirror: VK code for the recenter hotkey (default VK_HOME)
bool  g_sameFrameStereoMenu = true; // menu mirror: verified true same-frame stereo mode
bool  g_headAimEnabledMenu = false;// menu mirror: first-person head-aim (weapon) toggle (default OFF)
float g_headAimGainMenu = 1.5f;    // menu mirror: first-person head-aim turn-speed multiplier
bool  g_aerSwapEyes = true;        // AER L/R swap = invert depth direction (default ON: correct depth for AK)
std::atomic<int> g_aerDisplayHz{0};// 0 = auto-measure refresh; else force it (auto mis-measured the game rate, not display)
std::atomic_bool g_dibrDepthDebug{false};
bool g_dibrDepthDebugHotkeyWasDown = false;
std::atomic_bool g_desktopDepthDumpArmed{false};
bool g_desktopDepthDumpHotkeyWasDown = false;
bool g_desktopDepthDumpWritten = false;
unsigned g_desktopDepthDumpFrame = 0;
bool g_dibrProofWritten = false;
unsigned g_dibrProofFrame = 0;
bool g_dibrProofHotkeyWasDown = false;
bool g_dibrGainUpHotkeyWasDown = false;
bool g_dibrGainDownHotkeyWasDown = false;
bool g_dibrGainResetHotkeyWasDown = false;
bool g_dibrScaleUpHotkeyWasDown = false;
bool g_dibrScaleDownHotkeyWasDown = false;
bool g_dibrScaleResetHotkeyWasDown = false;
bool g_dibrEdgeUpHotkeyWasDown = false;
bool g_dibrEdgeDownHotkeyWasDown = false;
bool g_dibrEdgeResetHotkeyWasDown = false;
bool g_dibrDepthMetaWritten = false;
bool g_dibrDepthCopyWritten = false;
bool g_dibrDepthSrvWritten = false;
bool g_dibrDepthSrvCopyWritten = false;
bool g_dibrShaderWritten = false;
bool g_dibrShaderCompileWritten = false;
bool g_dibrCompilerLoadWritten = false;
bool g_dibrShaderCreateWritten = false;
bool g_dibrStateCreateWritten = false;
bool g_dibrSetupWritten = false;
bool g_dibrSetupDepthCopyWritten = false;
bool g_dibrColorWarpWritten = false;
bool g_dibrColorCopyWritten = false;
bool g_dibrBindRestoreWritten = false;
bool g_dibrDrawWritten = false;
constexpr unsigned kDepthScanMaxCandidates = 24;
struct DepthScanCandidate
{
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT width = 0;
    UINT height = 0;
    UINT samples = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT textureFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
    uintptr_t dsvPtr = 0;
    unsigned clearOrdinal = 0;
    unsigned drawOrdinal = 0;
    char source[16] = {};
};
DepthScanCandidate g_depthScanCandidates[kDepthScanMaxCandidates] = {};
unsigned g_depthScanCandidateCount = 0;
unsigned g_depthScanClearCount = 0;
unsigned g_depthScanDrawCount = 0;
unsigned g_depthScanSrvSeen = 0;
unsigned g_depthScanSrvDepthLikeSeen = 0;
unsigned g_depthScanSrvRejectColor = 0;
unsigned g_depthScanSrvRejectNonDepth = 0;
unsigned g_depthScanSrvRejectDuplicate = 0;
unsigned g_depthScanSrvRejectSmall = 0;
std::atomic<float> g_convergence{0.0f};    // per-eye horizontal fov shift (rad) — lets near objects fuse
XrQuaternionf g_lastHeadOrient = {0, 0, 0, 1};  // latest head orient (for same-frame capture pose-tag)
std::atomic_bool g_sameFrameActive{false}; // true while the render-fn double-call is filling BOTH histories
// Set once per real Present by the complete outer-draw path. False means no current L/R pair
// exists (menus/loading/transitions), so XR must submit the live mono backbuffer instead of stale
// or never-filled history textures.
std::atomic_bool g_sameFramePairReady{false};
// What the headset ACTUALLY received, counted per submit. A stereo source composing correctly into
// desktop SBS says nothing about which branch XR took; these two do. Pair = two distinct eye
// histories; mono = one backbuffer copied to both eyes, which is a flat image no matter how good
// the source was.
std::atomic<unsigned long long> g_sfPairSubmits{0};
std::atomic<unsigned long long> g_sfMonoSubmits{0};

// AK same-frame split: the whole SBS frame lands in ONE backbuffer (left half = left eye, right
// half = right eye). It is copied into both eye textures and let each eye's imageRect crop its half
// — no history double-call, no scaling blit. Set whenever the CalcSceneView split is producing views.
std::atomic_bool g_sbsFromBackbuffer{false};
IDXGISwapChain* g_gameSwapChain = nullptr; // stored so the render-fn detour can grab the backbuffer mid-frame

// ---- helpers (verbatim from the proven ME path) -------------------------------------------
bool IsKnownColorTextureFormat(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
        return true;
    default:
        return false;
    }
}

bool IsDepthLikeTextureFormat(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
        return true;
    default:
        return false;
    }
}

bool IsDepthLikeSrvCandidate(DXGI_FORMAT textureFormat, DXGI_FORMAT srvFormat) noexcept
{
    return IsDepthLikeTextureFormat(textureFormat) || IsDepthLikeTextureFormat(srvFormat);
}

void HeadEulerDegrees(const XrQuaternionf& q, float& yawDeg, float& pitchDeg, float& rollDeg) noexcept
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float fx = -2.0f * (x * z + w * y);
    const float fy = -2.0f * (y * z - w * x);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    const float ux = 2.0f * (x * y - w * z);
    const float uy = 1.0f - 2.0f * (x * x + z * z);
    constexpr float kRad2Deg = 57.2957795f;
    const float cfy = fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy);
    yawDeg = std::atan2(fx, -fz) * kRad2Deg;
    pitchDeg = std::asin(cfy) * kRad2Deg;
    rollDeg = std::atan2(-ux, uy) * kRad2Deg;
}

// Rebuild an orientation from just the head's yaw + pitch — NO roll. Submitting the full head quaternion makes
// the view tilt when you cock your head; the render is roll-free (yaw/pitch only), so the submit must be too.
// Keeps the horizon level (standard VR comfort).
XrQuaternionf RemoveRoll(const XrQuaternionf& q) noexcept
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float fx = -2.0f * (x * z + w * y);
    const float fy = -2.0f * (y * z - w * x);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    const float cfy = fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy);
    // yaw for REBUILDING the quaternion (forward = (-cosφ·sinθ, sinφ, -cosφ·cosθ) => θ = atan2(-fx,-fz)).
    // NOTE: this differs from HeadEulerDegrees' atan2(fx,-fz) on purpose — that one is the negated "reported"
    // convention (compensated by g_headYawSign downstream); using it here flips the submit yaw and kills free-look.
    const float yaw = std::atan2(-fx, -fz);
    const float pitch = std::asin(cfy);
    const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
    const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    XrQuaternionf r;   // qYaw(around Y) * qPitch(around X)
    r.w = cy * cp;
    r.x = cy * sp;
    r.y = sy * cp;
    r.z = -sy * sp;
    return r;
}

// --- Recenter: re-zero "forward" to the current head yaw (keeps gravity-aligned pitch/roll). Applied as a single
// quaternion offset to the head at capture, so the render (from g_headYaw) and the submit pose stay consistent. ---
XrQuaternionf QuatConj(const XrQuaternionf& q) noexcept { return XrQuaternionf{ -q.x, -q.y, -q.z, q.w }; }
XrQuaternionf QuatMul(const XrQuaternionf& a, const XrQuaternionf& b) noexcept
{
    XrQuaternionf r;
    r.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    r.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    r.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    r.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
    return r;
}
XrQuaternionf YawOnlyQuat(const XrQuaternionf& q) noexcept   // pure yaw component of an orientation
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float fx = -2.0f * (x * z + w * y);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    const float yaw = std::atan2(-fx, -fz);
    return XrQuaternionf{ 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
}

std::atomic_bool g_recenterReq{false};
XrQuaternionf g_recenterQuat = {0, 0, 0, 1};   // inverse of this is applied to the head; identity = no recenter

int64_t SrgbSwapchainFormat(int64_t fmt) noexcept
{
    switch (fmt)
    {
    case 28: return 29;   // R8G8B8A8_UNORM -> _SRGB
    case 87: return 91;   // B8G8R8A8_UNORM -> _SRGB
    default: return fmt;
    }
}

void CopyAscii(char* dst, size_t cap, const char* src)
{
    if (dst == nullptr || cap == 0) return;
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

bool ResolveProc(const char* name, PFN_xrVoidFunction* out)
{
    *out = nullptr;
    const XrResult r = g_getProc(g_instance, name, out);
    if (!XrSucceeded(r) || *out == nullptr)
    {
        XrLog("[AKXR] resolve %s FAILED: %s", name, FmtXr(r).c_str());
        return false;
    }
    return true;
}

bool CreateInstance()
{
    g_loader = LoadLibraryW(L"openxr_loader.dll");
    if (g_loader == nullptr)
    {
        XrLog("[AKXR] LoadLibraryW(openxr_loader.dll) failed err=%lu", GetLastError());
        return false;
    }
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"openxr_loader.dll", &g_loader);
    XrLog("[AKXR] openxr_loader.dll loaded.");

    g_getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(g_loader, "xrGetInstanceProcAddr"));
    if (g_getProc == nullptr)
    {
        XrLog("[AKXR] xrGetInstanceProcAddr export missing err=%lu", GetLastError());
        return false;
    }

    PFN_xrVoidFunction createInstanceFn = nullptr;
    if (!XrSucceeded(g_getProc(nullptr, "xrCreateInstance", &createInstanceFn)) || createInstanceFn == nullptr)
    {
        XrLog("[AKXR] xrCreateInstance resolve FAILED.");
        return false;
    }

    // Is XR_FB_display_refresh_rate available? Enumerate first — enabling an UNSUPPORTED extension makes
    // xrCreateInstance fail outright (= no VR), so it is only enabled if the runtime advertises it.
    g_hasRefreshRateExt = false;
    {
        PFN_xrVoidFunction enumFn = nullptr;
        if (XrSucceeded(g_getProc(nullptr, "xrEnumerateInstanceExtensionProperties", &enumFn)) && enumFn != nullptr)
        {
            auto enumExt = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(enumFn);
            uint32_t count = 0;
            if (XrSucceeded(enumExt(nullptr, 0, &count, nullptr)) && count > 0 && count < 512)
            {
                std::vector<XrExtensionProperties> props(count);
                for (auto& p : props) { p.type = XR_TYPE_EXTENSION_PROPERTIES_VALUE; p.next = nullptr; }
                if (XrSucceeded(enumExt(nullptr, count, &count, props.data())))
                {
                    for (const auto& p : props)
                        if (strcmp(p.extensionName, kDisplayRefreshRateExtensionName) == 0) { g_hasRefreshRateExt = true; break; }
                }
            }
        }
        XrLog("[AKXR] XR_FB_display_refresh_rate available: %s", g_hasRefreshRateExt ? "yes" : "no");
    }

    const char* extensions[2] = {kD3D11ExtensionName, kDisplayRefreshRateExtensionName};
    XrInstanceCreateInfo info = {};
    info.type = XR_TYPE_INSTANCE_CREATE_INFO_VALUE;
    CopyAscii(info.applicationInfo.applicationName, sizeof(info.applicationInfo.applicationName), "AK VR");
    info.applicationInfo.applicationVersion = 1;
    CopyAscii(info.applicationInfo.engineName, sizeof(info.applicationInfo.engineName), "AKVR");
    info.applicationInfo.engineVersion = 1;
    info.applicationInfo.apiVersion = MakeXrVersion(1, 0, 0);
    info.enabledExtensionCount = g_hasRefreshRateExt ? 2u : 1u;
    info.enabledExtensionNames = extensions;

    const XrResult r = reinterpret_cast<PFN_xrCreateInstance>(createInstanceFn)(&info, &g_instance);
    XrLog("[AKXR] xrCreateInstance (XR_KHR_D3D11_enable%s): %s",
          g_hasRefreshRateExt ? " + XR_FB_display_refresh_rate" : "", FmtXr(r).c_str());
    return XrSucceeded(r) && g_instance != nullptr;
}

bool ResolveSessionFunctions()
{
    PFN_xrVoidFunction p = nullptr;
#define RESOLVE(name, field, type)            \
    if (!ResolveProc(name, &p)) return false; \
    g_fn.field = reinterpret_cast<type>(p);
    RESOLVE("xrDestroyInstance", destroyInstance, PFN_xrDestroyInstance)
    RESOLVE("xrPollEvent", pollEvent, PFN_xrPollEvent)
    RESOLVE("xrGetSystem", getSystem, PFN_xrGetSystem)
    RESOLVE("xrEnumerateViewConfigurationViews", enumerateViewConfigurationViews, PFN_xrEnumerateViewConfigurationViews)
    RESOLVE("xrGetD3D11GraphicsRequirementsKHR", getD3D11GraphicsRequirements, PFN_xrGetD3D11GraphicsRequirementsKHR)
    RESOLVE("xrCreateSession", createSession, PFN_xrCreateSession)
    RESOLVE("xrDestroySession", destroySession, PFN_xrDestroySession)
    RESOLVE("xrCreateSwapchain", createSwapchain, PFN_xrCreateSwapchain)
    RESOLVE("xrEnumerateSwapchainImages", enumerateSwapchainImages, PFN_xrEnumerateSwapchainImages)
    RESOLVE("xrAcquireSwapchainImage", acquireSwapchainImage, PFN_xrAcquireSwapchainImage)
    RESOLVE("xrWaitSwapchainImage", waitSwapchainImage, PFN_xrWaitSwapchainImage)
    RESOLVE("xrReleaseSwapchainImage", releaseSwapchainImage, PFN_xrReleaseSwapchainImage)
    RESOLVE("xrBeginSession", beginSession, PFN_xrBeginSession)
    RESOLVE("xrEndSession", endSession, PFN_xrEndSession)
    RESOLVE("xrWaitFrame", waitFrame, PFN_xrWaitFrame)
    RESOLVE("xrBeginFrame", beginFrame, PFN_xrBeginFrame)
    RESOLVE("xrEndFrame", endFrame, PFN_xrEndFrame)
    RESOLVE("xrCreateReferenceSpace", createReferenceSpace, PFN_xrCreateReferenceSpace)
    RESOLVE("xrDestroySpace", destroySpace, PFN_xrDestroySpace)
    RESOLVE("xrLocateViews", locateViews, PFN_xrLocateViews)
#undef RESOLVE
    // Optional: only present if XR_FB_display_refresh_rate was enabled. Not fatal if it fails to resolve.
    g_fn.getDisplayRefreshRate = nullptr;
    if (g_hasRefreshRateExt)
    {
        PFN_xrVoidFunction q = nullptr;
        if (ResolveProc("xrGetDisplayRefreshRateFB", &q) && q != nullptr)
            g_fn.getDisplayRefreshRate = reinterpret_cast<PFN_xrGetDisplayRefreshRateFB>(q);
        XrLog("[AKXR] xrGetDisplayRefreshRateFB resolved: %s", g_fn.getDisplayRefreshRate ? "yes" : "no");
    }
    return true;
}

bool CreateSessionAndSwapchains()
{
    // g_systemId is already set by the HMD-wait loop in the bring-up thread.
    XrGraphicsRequirementsD3D11KHR req = {};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR_VALUE;
    XrResult r = g_fn.getD3D11GraphicsRequirements(g_instance, g_systemId, &req);
    XrLog("[AKXR] xrGetD3D11GraphicsRequirementsKHR: %s", FmtXr(r).c_str());
    if (!XrSucceeded(r)) return false;

    uint32_t viewCount = 0;
    r = g_fn.enumerateViewConfigurationViews(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE,
                                             0, &viewCount, nullptr);
    if (!XrSucceeded(r) || viewCount < 2)
    {
        XrLog("[AKXR] enumerateViewConfigurationViews(count) FAILED: %s", FmtXr(r).c_str());
        return false;
    }
    XrViewConfigurationView vcv[2] = {};
    vcv[0].type = XR_TYPE_VIEW_CONFIGURATION_VIEW_VALUE;
    vcv[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW_VALUE;
    r = g_fn.enumerateViewConfigurationViews(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE,
                                             2, &viewCount, vcv);
    if (!XrSucceeded(r))
    {
        XrLog("[AKXR] enumerateViewConfigurationViews FAILED: %s", FmtXr(r).c_str());
        return false;
    }
    XrLog("[AKXR] recommended eye image %ux%u; using game backbuffer %ux%u for a direct copy.",
          vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight, g_bbWidth, g_bbHeight);

    XrGraphicsBindingD3D11KHR binding = {};
    binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR_VALUE;
    binding.device = g_device;
    XrSessionCreateInfo sci = {};
    sci.type = XR_TYPE_SESSION_CREATE_INFO_VALUE;
    sci.next = &binding;
    sci.systemId = g_systemId;
    r = g_fn.createSession(g_instance, &sci, &g_session);
    XrLog("[AKXR] xrCreateSession (D3D11 game device): %s", FmtXr(r).c_str());
    if (!XrSucceeded(r) || g_session == nullptr) return false;

    for (int eye = 0; eye < 2; ++eye)
    {
        XrSwapchainCreateInfo sc = {};
        sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
        sc.format = SrgbSwapchainFormat(g_colorFormat);
        sc.sampleCount = 1;
        sc.width = g_bbWidth;
        sc.height = g_bbHeight;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        r = g_fn.createSwapchain(g_session, &sc, &g_eyes[eye].swapchain);
        XrLog("[AKXR] xrCreateSwapchain eye%d (sRGB fmt %lld from backbuffer %lld): %s",
              eye, SrgbSwapchainFormat(g_colorFormat), g_colorFormat, FmtXr(r).c_str());
        if (!XrSucceeded(r) || g_eyes[eye].swapchain == nullptr) return false;
        g_eyes[eye].width = g_bbWidth;
        g_eyes[eye].height = g_bbHeight;

        uint32_t imageCount = 0;
        r = g_fn.enumerateSwapchainImages(g_eyes[eye].swapchain, 0, &imageCount, nullptr);
        if (!XrSucceeded(r) || imageCount == 0)
        {
            XrLog("[AKXR] enumerateSwapchainImages(count) FAILED: %s", FmtXr(r).c_str());
            return false;
        }
        if (imageCount > 8) imageCount = 8;
        XrSwapchainImageD3D11KHR images[8] = {};
        for (uint32_t i = 0; i < imageCount; ++i) images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE;
        r = g_fn.enumerateSwapchainImages(g_eyes[eye].swapchain, imageCount, &imageCount,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(images));
        if (!XrSucceeded(r))
        {
            XrLog("[AKXR] enumerateSwapchainImages FAILED: %s", FmtXr(r).c_str());
            return false;
        }
        for (uint32_t i = 0; i < imageCount && i < 8; ++i) g_eyes[eye].images[i] = images[i].texture;
        g_eyes[eye].imageCount = imageCount;
        XrLog("[AKXR] eye%d swapchain images: %u", eye, imageCount);
    }


    XrReferenceSpaceCreateInfo rsci = {};
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
    rsci.poseInReferenceSpace = IdentityPose();
    r = g_fn.createReferenceSpace(g_session, &rsci, &g_appSpace);
    XrLog("[AKXR] xrCreateReferenceSpace(LOCAL): %s", FmtXr(r).c_str());
    if (!XrSucceeded(r) || g_appSpace == nullptr) return false;

    // Base LOCAL space — never re-anchored. Recenter reads the head pose in THIS frame, then re-creates g_appSpace
    // offset by it. Keeping a stable base avoids compounding offsets across multiple recenters.
    {
        XrReferenceSpaceCreateInfo lrs = {};
        lrs.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
        lrs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
        lrs.poseInReferenceSpace = IdentityPose();
        const XrResult lr = g_fn.createReferenceSpace(g_session, &lrs, &g_localSpace);
        XrLog("[AKXR] xrCreateReferenceSpace(base LOCAL for recenter): %s", FmtXr(lr).c_str());
    }

    return true;
}

void PollEvents()
{
    for (;;)
    {
        XrEventDataBuffer ev = {};
        ev.type = XR_TYPE_EVENT_DATA_BUFFER_VALUE;
        const XrResult pr = g_fn.pollEvent(g_instance, &ev);
        if (pr == XR_EVENT_UNAVAILABLE_VALUE || !XrSucceeded(pr)) return;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED_VALUE)
        {
            const auto* ssc = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
            g_sessionState = ssc->state;
            XrLog("[AKXR] session state -> %d", g_sessionState);
            if (g_sessionState == XR_SESSION_STATE_READY_VALUE && !g_sessionRunning)
            {
                XrSessionBeginInfo sbi = {};
                sbi.type = XR_TYPE_SESSION_BEGIN_INFO_VALUE;
                sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
                const XrResult br = g_fn.beginSession(g_session, &sbi);
                XrLog("[AKXR] xrBeginSession: %s", FmtXr(br).c_str());
                if (XrSucceeded(br))
                {
                    g_sessionRunning = true;
                    XrLog("[AKXR] SESSION RUNNING - submitting AK frames to the headset (live DIBR stereo when depth is ready).");
                }
            }
            else if (g_sessionState == XR_SESSION_STATE_STOPPING_VALUE)
            {
                g_fn.endSession(g_session);
                g_sessionRunning = false;
            }
        }
    }
}

// Copy the finished game backbuffer into one eye's XR swapchain image (acquire/wait/copy/release).
// FILL SOURCE: AK pillarboxes the backbuffer to 16:9 even at a square resolution, so copying the backbuffer
// leaves black bars in the (square) VR eye. The off-screen scene color RT is the full square render — the SAME
// source same-frame uses to fill. Use it for AER/DIBR when present and size-matched to the eye/history
// (CopyResource needs identical dims); otherwise fall back to the backbuffer. THIS is what makes AER/DIBR fill.
static ID3D11Texture2D* FillSource(ID3D11Texture2D* backBuffer) noexcept
{
    ID3D11Texture2D* s = g_fillSceneRT;
    if (s == nullptr) return backBuffer;
    __try
    {
        D3D11_TEXTURE2D_DESC d = {};
        s->GetDesc(&d);
        if (d.Width == g_bbWidth && d.Height == g_bbHeight) return s;   // size match = safe CopyResource, full scene
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return backBuffer;
}

// Composite the Insert panel onto an eye image that has just been filled. This is what lets the
// tuner be visible in the headset WITHOUT switching stereo off: the eye images are the live stereo
// pair, and the panel is drawn over each of them from the single overlay texture the menu rendered.
// No second ImGui renderer, no quad layer — one owner, N composites.
// Per-eye pixel offset applied to the menu blit so the panel lands at the SAME place inside each
// eye's visible (convergence-cropped) rect. Without this the crop would give the panel itself a
// disparity of the whole convergence shift and it would double exactly while you drag that slider.
int g_menuOverlayOffsetX[2] = {0, 0};

void OverlayMenuOnEyeImage(int eye, ID3D11Texture2D* image) noexcept
{
    if (image == nullptr || g_device == nullptr || eye < 0 || eye > 1 || !AKMenu::IsOpen()) return;

    D3D11_TEXTURE2D_DESC desc = {};
    image->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) return;

    // The runtime may hand back a TYPELESS resource, which a null-desc RTV cannot resolve. Ask for
    // the swapchain's declared colour format explicitly, and fall back to the default view.
    ID3D11RenderTargetView* rtv = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    // OpenXR carries swapchain formats as int64_t; the D3D view wants the DXGI enum.
    rtvDesc.Format = static_cast<DXGI_FORMAT>(SrgbSwapchainFormat(g_colorFormat));
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(g_device->CreateRenderTargetView(image, &rtvDesc, &rtv)) || rtv == nullptr)
    {
        rtv = nullptr;
        if (FAILED(g_device->CreateRenderTargetView(image, nullptr, &rtv)) || rtv == nullptr) return;
    }
    AKMenu::BlitToRenderTarget(rtv, desc.Width, desc.Height, g_menuOverlayOffsetX[eye]);
    rtv->Release();
}

void CopyToEye(int eye, ID3D11Texture2D* backBuffer) noexcept
{
    if (eye < 0 || eye > 1 || g_eyes[eye].swapchain == nullptr || g_context == nullptr || backBuffer == nullptr) return;
    XrSwapchainImageAcquireInfo ai = {};
    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    uint32_t index = 0;
    if (!XrSucceeded(g_fn.acquireSwapchainImage(g_eyes[eye].swapchain, &ai, &index))) return;
    XrSwapchainImageWaitInfo wi = {};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE;
    wi.timeout = XR_INFINITE_DURATION_VALUE;
    g_fn.waitSwapchainImage(g_eyes[eye].swapchain, &wi);
    if (index < g_eyes[eye].imageCount && g_eyes[eye].images[index] != nullptr)
    {
        g_context->CopyResource(g_eyes[eye].images[index], backBuffer);
        OverlayMenuOnEyeImage(eye, g_eyes[eye].images[index]);
    }
    XrSwapchainImageReleaseInfo ri = {};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(g_eyes[eye].swapchain, &ri);
}

// Copy an arbitrary owned texture (a history image) into one eye's XR swapchain.
void CopyTexToEye(int eye, ID3D11Texture2D* src) noexcept
{
    if (eye < 0 || eye > 1 || g_eyes[eye].swapchain == nullptr || g_context == nullptr || src == nullptr) return;
    XrSwapchainImageAcquireInfo ai = {};
    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    uint32_t index = 0;
    if (!XrSucceeded(g_fn.acquireSwapchainImage(g_eyes[eye].swapchain, &ai, &index))) return;
    XrSwapchainImageWaitInfo wi = {};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE;
    wi.timeout = XR_INFINITE_DURATION_VALUE;
    g_fn.waitSwapchainImage(g_eyes[eye].swapchain, &wi);
    if (index < g_eyes[eye].imageCount && g_eyes[eye].images[index] != nullptr)
    {
        g_context->CopyResource(g_eyes[eye].images[index], src);
        OverlayMenuOnEyeImage(eye, g_eyes[eye].images[index]);
    }
    XrSwapchainImageReleaseInfo ri = {};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(g_eyes[eye].swapchain, &ri);
}

DXGI_FORMAT DibrDepthSrvFormat(DXGI_FORMAT f) noexcept
{
    switch (f)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    }
}

bool CompileDibrShader(const char* source, const char* entry, const char* profile, ID3DBlob** blob) noexcept
{
    if (blob == nullptr) return false;
    *blob = nullptr;
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (compiler == nullptr) compiler = LoadLibraryW(L"d3dcompiler_43.dll");
    if (compiler == nullptr)
    {
        XrLog("[AKDIBR] no d3dcompiler dll");
        return false;
    }
    using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                          LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile"));
    if (compile == nullptr)
    {
        XrLog("[AKDIBR] no D3DCompile export");
        FreeLibrary(compiler);
        return false;
    }
    ID3DBlob* errors = nullptr;
    const HRESULT hr = compile(source, strlen(source), nullptr, nullptr, nullptr, entry, profile, 0, 0, blob, &errors);
    if (FAILED(hr))
    {
        if (errors != nullptr)
        {
            XrLog("[AKDIBR] shader compile error: %s", static_cast<const char*>(errors->GetBufferPointer()));
            errors->Release();
        }
        FreeLibrary(compiler);
        return false;
    }
    if (errors != nullptr) errors->Release();
    FreeLibrary(compiler);
    return true;
}

const BYTE* DibrPixelShaderBytecode(UINT depthSamples, size_t& byteCount) noexcept
{
    if (depthSamples > 1)
    {
        byteCount = sizeof(g_dibr_ps_msaa4_bytecode);
        return g_dibr_ps_msaa4_bytecode;
    }

    byteCount = sizeof(g_dibr_ps_2d_bytecode);
    return g_dibr_ps_2d_bytecode;
}

const BYTE* DibrDepthDebugPixelShaderBytecode(UINT depthSamples, size_t& byteCount) noexcept
{
    if (depthSamples > 1)
    {
        byteCount = sizeof(g_dibr_depth_debug_msaa4_bytecode);
        return g_dibr_depth_debug_msaa4_bytecode;
    }

    byteCount = sizeof(g_dibr_depth_debug_2d_bytecode);
    return g_dibr_depth_debug_2d_bytecode;
}

bool CreateDibrShaderObjectsFromBytecode(ID3D11Device* dev, UINT depthSamples, HRESULT& vsHr, HRESULT& psHr) noexcept
{
    vsHr = E_FAIL;
    psHr = E_FAIL;
    SafeRelease(g_dibrVs);
    SafeRelease(g_dibrPs);
    SafeRelease(g_dibrDepthDebugPs);
    if (dev == nullptr)
    {
        vsHr = E_INVALIDARG;
        psHr = E_INVALIDARG;
        return false;
    }

    size_t psBytes = 0;
    const BYTE* psBytecode = DibrPixelShaderBytecode(depthSamples, psBytes);
    size_t debugPsBytes = 0;
    const BYTE* debugPsBytecode = DibrDepthDebugPixelShaderBytecode(depthSamples, debugPsBytes);
    vsHr = dev->CreateVertexShader(g_dibr_vs_bytecode, sizeof(g_dibr_vs_bytecode), nullptr, &g_dibrVs);
    if (SUCCEEDED(vsHr))
    {
        psHr = dev->CreatePixelShader(psBytecode, psBytes, nullptr, &g_dibrPs);
    }
    if (SUCCEEDED(psHr))
    {
        psHr = dev->CreatePixelShader(debugPsBytecode, debugPsBytes, nullptr, &g_dibrDepthDebugPs);
    }

    if (FAILED(vsHr) || FAILED(psHr) || g_dibrVs == nullptr || g_dibrPs == nullptr || g_dibrDepthDebugPs == nullptr)
    {
        SafeRelease(g_dibrVs);
        SafeRelease(g_dibrPs);
        SafeRelease(g_dibrDepthDebugPs);
        return false;
    }
    return true;
}

bool CreateDibrStateObjects(ID3D11Device* dev, HRESULT& samplerHr, HRESULT& rasterHr, HRESULT& depthHr, HRESULT& blendHr, HRESULT& cbHr) noexcept
{
    samplerHr = rasterHr = depthHr = blendHr = cbHr = E_FAIL;
    SafeRelease(g_dibrSampler);
    SafeRelease(g_dibrRaster);
    SafeRelease(g_dibrDepthState);
    SafeRelease(g_dibrBlend);
    SafeRelease(g_dibrParamsCb);
    if (dev == nullptr)
    {
        samplerHr = rasterHr = depthHr = blendHr = cbHr = E_INVALIDARG;
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    samplerHr = dev->CreateSamplerState(&sd, &g_dibrSampler);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rasterHr = dev->CreateRasterizerState(&rd, &g_dibrRaster);

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    depthHr = dev->CreateDepthStencilState(&dd, &g_dibrDepthState);

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    blendHr = dev->CreateBlendState(&bd, &g_dibrBlend);

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 32;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbHr = dev->CreateBuffer(&cbd, nullptr, &g_dibrParamsCb);

    const bool ok = SUCCEEDED(samplerHr) && SUCCEEDED(rasterHr) && SUCCEEDED(depthHr) &&
                    SUCCEEDED(blendHr) && SUCCEEDED(cbHr) &&
                    g_dibrSampler != nullptr && g_dibrRaster != nullptr && g_dibrDepthState != nullptr &&
                    g_dibrBlend != nullptr && g_dibrParamsCb != nullptr;
    if (!ok)
    {
        SafeRelease(g_dibrSampler);
        SafeRelease(g_dibrRaster);
        SafeRelease(g_dibrDepthState);
        SafeRelease(g_dibrBlend);
        SafeRelease(g_dibrParamsCb);
    }
    return ok;
}

bool EnsureDibrShaders() noexcept
{
    if (g_dibrShadersReady) return true;
    if (g_dibrShadersTried) return false;
    g_dibrShadersTried = true;
    if (g_device == nullptr) return false;

    HRESULT vsHr = E_FAIL;
    HRESULT psHr = E_FAIL;
    HRESULT hr = CreateDibrShaderObjectsFromBytecode(g_device, g_dibrDepthSamples, vsHr, psHr) ? S_OK : (FAILED(vsHr) ? vsHr : psHr);
    if (FAILED(hr) || g_dibrVs == nullptr || g_dibrPs == nullptr)
    {
        XrLog("[AKDIBR] shader create failed vs=0x%08X ps=0x%08X",
              static_cast<unsigned>(vsHr), static_cast<unsigned>(psHr));
        return false;
    }

    HRESULT samplerHr = E_FAIL;
    HRESULT rasterHr = E_FAIL;
    HRESULT depthHr = E_FAIL;
    HRESULT blendHr = E_FAIL;
    HRESULT cbHr = E_FAIL;
    if (!CreateDibrStateObjects(g_device, samplerHr, rasterHr, depthHr, blendHr, cbHr))
    {
        XrLog("[AKDIBR] state create failed sampler=0x%08X raster=0x%08X depth=0x%08X blend=0x%08X cb=0x%08X",
              static_cast<unsigned>(samplerHr), static_cast<unsigned>(rasterHr),
              static_cast<unsigned>(depthHr), static_cast<unsigned>(blendHr), static_cast<unsigned>(cbHr));
        return false;
    }
    g_dibrShadersReady = true;
    XrLog("[AKDIBR] shaders ready depthSamples=%u", g_dibrDepthSamples);
    return true;
}

bool EnsureDibrColorAndWarp(ID3D11Texture2D* backBuffer) noexcept
{
    if (g_device == nullptr || backBuffer == nullptr) return false;
    D3D11_TEXTURE2D_DESC bb = {};
    backBuffer->GetDesc(&bb);
    if (g_dibrColorCopy == nullptr || g_dibrColorW != bb.Width || g_dibrColorH != bb.Height || g_dibrColorFmt != bb.Format)
    {
        SafeRelease(g_dibrColorSrv);
        SafeRelease(g_dibrColorCopy);
        D3D11_TEXTURE2D_DESC cd = bb;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cd.Usage = D3D11_USAGE_DEFAULT;
        cd.CPUAccessFlags = 0;
        cd.MiscFlags = 0;
        cd.MipLevels = 1;
        cd.ArraySize = 1;
        cd.SampleDesc.Count = 1;
        cd.SampleDesc.Quality = 0;
        HRESULT hr = g_device->CreateTexture2D(&cd, nullptr, &g_dibrColorCopy);
        if (FAILED(hr) || g_dibrColorCopy == nullptr)
        {
            XrLog("[AKDIBR] color copy create failed hr=0x%08X", static_cast<unsigned>(hr));
            return false;
        }
        hr = g_device->CreateShaderResourceView(g_dibrColorCopy, nullptr, &g_dibrColorSrv);
        if (FAILED(hr) || g_dibrColorSrv == nullptr)
        {
            XrLog("[AKDIBR] color srv create failed hr=0x%08X", static_cast<unsigned>(hr));
            return false;
        }
        g_dibrColorW = bb.Width;
        g_dibrColorH = bb.Height;
        g_dibrColorFmt = bb.Format;
    }
    if (g_dibrWarpTex == nullptr || g_dibrWarpW != bb.Width || g_dibrWarpH != bb.Height)
    {
        SafeRelease(g_dibrWarpRtv);
        SafeRelease(g_dibrWarpTex);
        D3D11_TEXTURE2D_DESC wd = bb;
        wd.BindFlags = D3D11_BIND_RENDER_TARGET;
        wd.Usage = D3D11_USAGE_DEFAULT;
        wd.CPUAccessFlags = 0;
        wd.MiscFlags = 0;
        wd.MipLevels = 1;
        wd.ArraySize = 1;
        wd.SampleDesc.Count = 1;
        wd.SampleDesc.Quality = 0;
        HRESULT hr = g_device->CreateTexture2D(&wd, nullptr, &g_dibrWarpTex);
        if (FAILED(hr) || g_dibrWarpTex == nullptr)
        {
            XrLog("[AKDIBR] warp tex create failed hr=0x%08X", static_cast<unsigned>(hr));
            return false;
        }
        hr = g_device->CreateRenderTargetView(g_dibrWarpTex, nullptr, &g_dibrWarpRtv);
        if (FAILED(hr) || g_dibrWarpRtv == nullptr)
        {
            XrLog("[AKDIBR] warp rtv create failed hr=0x%08X", static_cast<unsigned>(hr));
            return false;
        }
        g_dibrWarpW = bb.Width;
        g_dibrWarpH = bb.Height;
    }
    return true;
}

ID3D11Texture2D* RenderDibrRightEye(ID3D11Texture2D* backBuffer) noexcept;

std::wstring ModuleDirectory() noexcept
{
    wchar_t mod[MAX_PATH] = {};
    if (g_selfModule != nullptr && GetModuleFileNameW(g_selfModule, mod, MAX_PATH) > 0)
    {
        wchar_t* slash = wcsrchr(mod, L'\\');
        if (slash != nullptr)
        {
            *slash = L'\0';
            return std::wstring(mod);
        }
    }
    return L".";
}

std::wstring DibrProofDirectory() noexcept
{
    std::wstring dir = ModuleDirectory() + L"\\AK_DIBR_PROOF";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void DeleteStaleDepthDumpFiles() noexcept
{
    const std::wstring dir = DibrProofDirectory();
    DeleteFileW((dir + L"\\desktop_depth_report.txt").c_str());
    DeleteFileW((dir + L"\\desktop_depth_candidates.txt").c_str());
    DeleteFileW((dir + L"\\desktop_depth_debug.tga").c_str());
    DeleteFileW((dir + L"\\desktop_dibr.tga").c_str());
    DeleteFileW((dir + L"\\desktop_game.tga").c_str());
    DeleteFileW((dir + L"\\desktop_dibr_gain_008.tga").c_str());
    DeleteFileW((dir + L"\\desktop_dibr_gain_015.tga").c_str());
    DeleteFileW((dir + L"\\desktop_dibr_gain_025.tga").c_str());
    DeleteFileW((dir + L"\\desktop_dibr_gain_035.tga").c_str());
    DeleteFileW((dir + L"\\desktop_dibr_gain_050.tga").c_str());
    for (unsigned i = 0; i < 64; ++i)
    {
        wchar_t fileName[96] = {};
        _snwprintf_s(fileName, _countof(fileName), _TRUNCATE, L"\\candidate_%02u_depth_debug.tga", i);
        DeleteFileW((dir + fileName).c_str());
    }
}

bool IsProofColorFormat(DXGI_FORMAT f) noexcept
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return true;
    default:
        return false;
    }
}

void ReadProofPixel(const unsigned char* p, DXGI_FORMAT f, unsigned char& r, unsigned char& g, unsigned char& b) noexcept
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        r = p[0]; g = p[1]; b = p[2];
        break;
    default:
        b = p[0]; g = p[1]; r = p[2];
        break;
    }
}

bool CreateProofStaging(ID3D11Texture2D* src, ID3D11Texture2D** stagingOut, D3D11_TEXTURE2D_DESC* outDesc) noexcept
{
    if (stagingOut == nullptr || src == nullptr || g_device == nullptr || g_context == nullptr) return false;
    *stagingOut = nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    src->GetDesc(&desc);
    if (!IsProofColorFormat(desc.Format)) return false;

    ID3D11Texture2D* readSrc = src;
    bool releaseReadSrc = false;
    if (desc.SampleDesc.Count > 1)
    {
        D3D11_TEXTURE2D_DESC resolved = desc;
        resolved.SampleDesc.Count = 1;
        resolved.SampleDesc.Quality = 0;
        resolved.BindFlags = 0;
        resolved.MiscFlags = 0;
        resolved.Usage = D3D11_USAGE_DEFAULT;
        resolved.CPUAccessFlags = 0;
        if (FAILED(g_device->CreateTexture2D(&resolved, nullptr, &readSrc)) || readSrc == nullptr)
        {
            return false;
        }
        g_context->ResolveSubresource(readSrc, 0, src, 0, desc.Format);
        desc = resolved;
        releaseReadSrc = true;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT hr = g_device->CreateTexture2D(&stagingDesc, nullptr, stagingOut);
    if (SUCCEEDED(hr) && *stagingOut != nullptr)
    {
        g_context->CopyResource(*stagingOut, readSrc);
        if (outDesc != nullptr) *outDesc = stagingDesc;
    }
    if (releaseReadSrc) readSrc->Release();
    return SUCCEEDED(hr) && *stagingOut != nullptr;
}

bool WriteProofTga(const std::wstring& path, ID3D11Texture2D* src) noexcept
{
    ID3D11Texture2D* staging = nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    if (!CreateProofStaging(src, &staging, &desc) || staging == nullptr) return false;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) || mapped.pData == nullptr)
    {
        staging->Release();
        return false;
    }

    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr)
    {
        g_context->Unmap(staging, 0);
        staging->Release();
        return false;
    }

    unsigned char header[18] = {};
    header[2] = 2;  // uncompressed true-color
    header[12] = static_cast<unsigned char>(desc.Width & 0xffu);
    header[13] = static_cast<unsigned char>((desc.Width >> 8) & 0xffu);
    header[14] = static_cast<unsigned char>(desc.Height & 0xffu);
    header[15] = static_cast<unsigned char>((desc.Height >> 8) & 0xffu);
    header[16] = 32;
    header[17] = 0x28;  // top-left origin, 8 alpha bits
    fwrite(header, 1, sizeof(header), f);

    const unsigned char* base = static_cast<const unsigned char*>(mapped.pData);
    for (UINT y = 0; y < desc.Height; ++y)
    {
        const unsigned char* row = base + static_cast<size_t>(mapped.RowPitch) * y;
        for (UINT x = 0; x < desc.Width; ++x)
        {
            const unsigned char* p = row + static_cast<size_t>(x) * 4u;
            unsigned char r = 0, g = 0, b = 0;
            ReadProofPixel(p, desc.Format, r, g, b);
            const unsigned char out[4] = { b, g, r, 255 };
            fwrite(out, 1, sizeof(out), f);
        }
    }

    fclose(f);
    g_context->Unmap(staging, 0);
    staging->Release();
    return true;
}

struct ProofDiff
{
    unsigned long long samples = 0;
    unsigned long long sumAbsRgb = 0;
    unsigned maxAbsRgb = 0;
    unsigned long long leftHash = 1469598103934665603ull;
    unsigned long long rightHash = 1469598103934665603ull;
};

void HashProofByte(unsigned long long& h, unsigned char v) noexcept
{
    h ^= static_cast<unsigned long long>(v);
    h *= 1099511628211ull;
}

bool ComputeProofDiff(ID3D11Texture2D* left, ID3D11Texture2D* right, ProofDiff& diff) noexcept
{
    ID3D11Texture2D* leftStaging = nullptr;
    ID3D11Texture2D* rightStaging = nullptr;
    D3D11_TEXTURE2D_DESC ld = {};
    D3D11_TEXTURE2D_DESC rd = {};
    if (!CreateProofStaging(left, &leftStaging, &ld) || !CreateProofStaging(right, &rightStaging, &rd) ||
        leftStaging == nullptr || rightStaging == nullptr)
    {
        SafeRelease(leftStaging);
        SafeRelease(rightStaging);
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE lm = {};
    D3D11_MAPPED_SUBRESOURCE rm = {};
    if (FAILED(g_context->Map(leftStaging, 0, D3D11_MAP_READ, 0, &lm)) || lm.pData == nullptr)
    {
        SafeRelease(leftStaging);
        SafeRelease(rightStaging);
        return false;
    }
    if (FAILED(g_context->Map(rightStaging, 0, D3D11_MAP_READ, 0, &rm)) || rm.pData == nullptr)
    {
        g_context->Unmap(leftStaging, 0);
        SafeRelease(leftStaging);
        SafeRelease(rightStaging);
        return false;
    }

    const UINT w = (ld.Width < rd.Width) ? ld.Width : rd.Width;
    const UINT h = (ld.Height < rd.Height) ? ld.Height : rd.Height;
    const UINT stepX = (w > 960u) ? 2u : 1u;
    const UINT stepY = (h > 540u) ? 2u : 1u;
    const unsigned char* lb = static_cast<const unsigned char*>(lm.pData);
    const unsigned char* rb = static_cast<const unsigned char*>(rm.pData);
    for (UINT y = 0; y < h; y += stepY)
    {
        const unsigned char* lr = lb + static_cast<size_t>(lm.RowPitch) * y;
        const unsigned char* rr = rb + static_cast<size_t>(rm.RowPitch) * y;
        for (UINT x = 0; x < w; x += stepX)
        {
            unsigned char lr8 = 0, lg8 = 0, lb8 = 0;
            unsigned char rr8 = 0, rg8 = 0, rb8 = 0;
            ReadProofPixel(lr + static_cast<size_t>(x) * 4u, ld.Format, lr8, lg8, lb8);
            ReadProofPixel(rr + static_cast<size_t>(x) * 4u, rd.Format, rr8, rg8, rb8);
            const unsigned dr = (lr8 > rr8) ? (lr8 - rr8) : (rr8 - lr8);
            const unsigned dg = (lg8 > rg8) ? (lg8 - rg8) : (rg8 - lg8);
            const unsigned db = (lb8 > rb8) ? (lb8 - rb8) : (rb8 - lb8);
            const unsigned pixelMax = (dr > dg) ? ((dr > db) ? dr : db) : ((dg > db) ? dg : db);
            if (pixelMax > diff.maxAbsRgb) diff.maxAbsRgb = pixelMax;
            diff.sumAbsRgb += dr + dg + db;
            ++diff.samples;
            HashProofByte(diff.leftHash, lr8); HashProofByte(diff.leftHash, lg8); HashProofByte(diff.leftHash, lb8);
            HashProofByte(diff.rightHash, rr8); HashProofByte(diff.rightHash, rg8); HashProofByte(diff.rightHash, rb8);
        }
    }

    g_context->Unmap(rightStaging, 0);
    g_context->Unmap(leftStaging, 0);
    SafeRelease(rightStaging);
    SafeRelease(leftStaging);
    return diff.samples > 0;
}

void WriteProofReport(const std::wstring& path, bool leftOk, bool rightOk, bool diffOk, const ProofDiff& diff) noexcept
{
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    const double meanAbs = (diffOk && diff.samples > 0)
        ? static_cast<double>(diff.sumAbsRgb) / (static_cast<double>(diff.samples) * 3.0)
        : 0.0;
    const bool effectivelySame = !diffOk || (meanAbs < 0.75 && diff.maxAbsRgb < 8);
    fprintf(f, "AK DIBR proof dump\r\n");
    fprintf(f, "left_tga=%s\r\n", leftOk ? "ok" : "failed");
    fprintf(f, "right_tga=%s\r\n", rightOk ? "ok" : "failed");
    fprintf(f, "depth_ready=%s\r\n", g_dibrDepthReady.load() ? "yes" : "no");
    fprintf(f, "depth_size=%ux%u samples=%u\r\n", g_dibrDepthW, g_dibrDepthH, g_dibrDepthSamples);
    fprintf(f, "dibr_gain=%.6f convergence=%.6f sign=%.1f scale=%.1f edge_guard=%.2f edge_threshold=%.3f\r\n",
            static_cast<double>(g_dibrGain), static_cast<double>(g_dibrConvergence),
            static_cast<double>(g_dibrSign), static_cast<double>(g_dibrDepthScale),
            static_cast<double>(g_dibrEdgeGuard), static_cast<double>(g_dibrEdgeThreshold));
    fprintf(f, "diff_samples=%llu\r\n", diff.samples);
    fprintf(f, "mean_abs_rgb=%.6f\r\n", meanAbs);
    fprintf(f, "max_abs_rgb=%u\r\n", diff.maxAbsRgb);
    fprintf(f, "left_hash=0x%016llX\r\n", diff.leftHash);
    fprintf(f, "right_hash=0x%016llX\r\n", diff.rightHash);
    fprintf(f, "decision=%s\r\n", effectivelySame ? "FAIL: right eye effectively identical/flat" : "DIFFERENT: DIBR warped right eye");
    fclose(f);
}

void WriteDesktopDepthDumpReport(const std::wstring& path,
                                 bool gameOk,
                                 bool depthDebugOk,
                                 bool dibrOk,
                                 bool timedOut,
                                 const char* note) noexcept
{
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    fprintf(f, "AK desktop depth-map dump\r\n");
    fprintf(f, "xr_enabled=%s\r\n", g_xrEnabled.load() ? "yes" : "no");
    fprintf(f, "timed_out=%s\r\n", timedOut ? "yes" : "no");
    fprintf(f, "game_tga=%s\r\n", gameOk ? "ok" : "failed");
    fprintf(f, "depth_debug_tga=%s\r\n", depthDebugOk ? "ok" : "failed");
    fprintf(f, "dibr_tga=%s\r\n", dibrOk ? "ok" : "failed");
    fprintf(f, "depth_ready=%s\r\n", g_dibrDepthReady.load() ? "yes" : "no");
    fprintf(f, "depth_size=%ux%u samples=%u\r\n", g_dibrDepthW, g_dibrDepthH, g_dibrDepthSamples);
    fprintf(f, "candidate_count=%u\r\n", g_depthScanCandidateCount);
    fprintf(f, "dibr_gain=%.6f convergence=%.6f sign=%.1f scale=%.1f edge_guard=%.2f edge_threshold=%.3f\r\n",
            static_cast<double>(g_dibrGain), static_cast<double>(g_dibrConvergence),
            static_cast<double>(g_dibrSign), static_cast<double>(g_dibrDepthScale),
            static_cast<double>(g_dibrEdgeGuard), static_cast<double>(g_dibrEdgeThreshold));
    fprintf(f, "files=desktop_game.tga, desktop_depth_debug.tga, desktop_dibr.tga, desktop_dibr_gain_XXX.tga, candidate_XX_depth_debug.tga\r\n");
    fprintf(f, "note=%s\r\n", note != nullptr ? note : "");
    fclose(f);
}

void ReleaseDepthScanCandidates() noexcept
{
    for (unsigned i = 0; i < kDepthScanMaxCandidates; ++i)
    {
        SafeRelease(g_depthScanCandidates[i].srv);
        SafeRelease(g_depthScanCandidates[i].tex);
        g_depthScanCandidates[i] = {};
    }
    g_depthScanCandidateCount = 0;
    g_depthScanClearCount = 0;
    g_depthScanDrawCount = 0;
    g_depthScanSrvSeen = 0;
    g_depthScanSrvDepthLikeSeen = 0;
    g_depthScanSrvRejectColor = 0;
    g_depthScanSrvRejectNonDepth = 0;
    g_depthScanSrvRejectDuplicate = 0;
    g_depthScanSrvRejectSmall = 0;
}

void WriteDepthCandidateReport(const std::wstring& path, const bool* candidateOk, unsigned okCount) noexcept
{
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    fprintf(f, "AK depth candidate scan\r\n");
    fprintf(f, "candidate_count=%u\r\n", g_depthScanCandidateCount);
    fprintf(f, "candidate_debug_ok_count=%u\r\n", okCount);
    fprintf(f, "srv_seen=%u srv_depthlike_seen=%u srv_reject_color=%u srv_reject_non_depth=%u srv_reject_duplicate=%u srv_reject_small=%u\r\n",
            g_depthScanSrvSeen,
            g_depthScanSrvDepthLikeSeen,
            g_depthScanSrvRejectColor,
            g_depthScanSrvRejectNonDepth,
            g_depthScanSrvRejectDuplicate,
            g_depthScanSrvRejectSmall);
    for (unsigned i = 0; i < g_depthScanCandidateCount && i < kDepthScanMaxCandidates; ++i)
    {
        const DepthScanCandidate& c = g_depthScanCandidates[i];
        fprintf(f,
                "candidate_%02u ok=%s source=%s clear_ordinal=%u draw_ordinal=%u dsv=0x%p size=%ux%u samples=%u fmt=%u texFmt=%u srvFmt=%u srv=%p tex=%p file=candidate_%02u_depth_debug.tga\r\n",
                i,
                (candidateOk != nullptr && candidateOk[i]) ? "yes" : "no",
                c.source[0] != '\0' ? c.source : "?",
                c.clearOrdinal,
                c.drawOrdinal,
                reinterpret_cast<void*>(c.dsvPtr),
                c.width,
                c.height,
                c.samples,
                static_cast<unsigned>(c.format),
                static_cast<unsigned>(c.textureFormat),
                static_cast<unsigned>(c.srvFormat),
                static_cast<void*>(c.srv),
                static_cast<void*>(c.tex),
                i);
    }
    fclose(f);
}

ID3D11Texture2D* RenderDepthCandidate(ID3D11Texture2D* backBuffer, DepthScanCandidate& candidate, bool debug) noexcept
{
    if (backBuffer == nullptr || candidate.srv == nullptr) return nullptr;

    ID3D11ShaderResourceView* oldSrv = g_dibrDepthSrv;
    const UINT oldW = g_dibrDepthW;
    const UINT oldH = g_dibrDepthH;
    const UINT oldSamples = g_dibrDepthSamples;
    const bool oldReady = g_dibrDepthReady.load();
    const bool oldDebug = g_dibrDepthDebug.load();
    const bool oldEnabled = g_dibrEnabled.load();
    const bool shadersReady = g_dibrShadersReady;
    const bool shadersTried = g_dibrShadersTried;

    g_dibrDepthSrv = candidate.srv;
    g_dibrDepthW = candidate.width;
    g_dibrDepthH = candidate.height;
    g_dibrDepthSamples = candidate.samples;
    g_dibrDepthReady.store(true);
    g_dibrDepthDebug.store(debug);
    g_dibrEnabled.store(true);
    if (oldSamples != candidate.samples)
    {
        g_dibrShadersReady = false;
        g_dibrShadersTried = false;
    }

    ID3D11Texture2D* out = RenderDibrRightEye(backBuffer);

    g_dibrDepthSrv = oldSrv;
    g_dibrDepthW = oldW;
    g_dibrDepthH = oldH;
    g_dibrDepthSamples = oldSamples;
    g_dibrDepthReady.store(oldReady);
    g_dibrDepthDebug.store(oldDebug);
    g_dibrEnabled.store(oldEnabled);
    g_dibrShadersReady = shadersReady;
    g_dibrShadersTried = shadersTried;
    return out;
}

ID3D11Texture2D* RenderDepthCandidateDebug(ID3D11Texture2D* backBuffer, DepthScanCandidate& candidate) noexcept
{
    return RenderDepthCandidate(backBuffer, candidate, true);
}

ID3D11Texture2D* RenderDepthCandidateDibr(ID3D11Texture2D* backBuffer, DepthScanCandidate& candidate) noexcept
{
    return RenderDepthCandidate(backBuffer, candidate, false);
}

void WriteDepthMetaReport(const D3D11_TEXTURE2D_DESC& tex, const D3D11_DEPTH_STENCIL_VIEW_DESC& dsvDesc) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\depth_meta.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    fprintf(f, "AK DIBR depth metadata probe\r\n");
    fprintf(f, "texture_width=%u\r\n", tex.Width);
    fprintf(f, "texture_height=%u\r\n", tex.Height);
    fprintf(f, "texture_mips=%u\r\n", tex.MipLevels);
    fprintf(f, "texture_array=%u\r\n", tex.ArraySize);
    fprintf(f, "texture_format=%u\r\n", static_cast<unsigned>(tex.Format));
    fprintf(f, "texture_samples=%u\r\n", tex.SampleDesc.Count);
    fprintf(f, "texture_sample_quality=%u\r\n", tex.SampleDesc.Quality);
    fprintf(f, "texture_usage=%u\r\n", static_cast<unsigned>(tex.Usage));
    fprintf(f, "texture_bind_flags=0x%X\r\n", tex.BindFlags);
    fprintf(f, "texture_cpu_access=0x%X\r\n", tex.CPUAccessFlags);
    fprintf(f, "texture_misc=0x%X\r\n", tex.MiscFlags);
    fprintf(f, "dsv_format=%u\r\n", static_cast<unsigned>(dsvDesc.Format));
    fprintf(f, "dsv_dimension=%u\r\n", static_cast<unsigned>(dsvDesc.ViewDimension));
    fprintf(f, "dsv_flags=0x%X\r\n", dsvDesc.Flags);
    fprintf(f, "decision=DEPTH_METADATA_ONLY_OK\r\n");
    fclose(f);
}

void WriteDepthCopyReport(const D3D11_TEXTURE2D_DESC& tex, HRESULT createHr, HRESULT copyHr) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\depth_copy.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    fprintf(f, "AK DIBR depth copy probe\r\n");
    fprintf(f, "texture_width=%u\r\n", tex.Width);
    fprintf(f, "texture_height=%u\r\n", tex.Height);
    fprintf(f, "texture_format=%u\r\n", static_cast<unsigned>(tex.Format));
    fprintf(f, "texture_samples=%u\r\n", tex.SampleDesc.Count);
    fprintf(f, "texture_bind_flags=0x%X\r\n", tex.BindFlags);
    fprintf(f, "copy_bind_flags=0x0\r\n");
    fprintf(f, "create_hr=0x%08X\r\n", static_cast<unsigned>(createHr));
    fprintf(f, "copy_hr=0x%08X\r\n", static_cast<unsigned>(copyHr));
    fprintf(f, "decision=%s\r\n", SUCCEEDED(createHr) && SUCCEEDED(copyHr) ? "DEPTH_COPY_ONLY_OK" : "DEPTH_COPY_ONLY_FAILED");
    fclose(f);
}

void WriteDepthSrvReport(const D3D11_TEXTURE2D_DESC& tex, HRESULT createHr, HRESULT srvHr, DXGI_FORMAT srvFormat) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\depth_srv.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    fprintf(f, "AK DIBR depth SRV probe\r\n");
    fprintf(f, "texture_width=%u\r\n", tex.Width);
    fprintf(f, "texture_height=%u\r\n", tex.Height);
    fprintf(f, "texture_format=%u\r\n", static_cast<unsigned>(tex.Format));
    fprintf(f, "texture_samples=%u\r\n", tex.SampleDesc.Count);
    fprintf(f, "source_bind_flags=0x%X\r\n", tex.BindFlags);
    fprintf(f, "copy_bind_flags=0x%X\r\n", D3D11_BIND_SHADER_RESOURCE);
    fprintf(f, "srv_format=%u\r\n", static_cast<unsigned>(srvFormat));
    fprintf(f, "srv_dimension=%u\r\n", tex.SampleDesc.Count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D);
    fprintf(f, "create_hr=0x%08X\r\n", static_cast<unsigned>(createHr));
    fprintf(f, "srv_hr=0x%08X\r\n", static_cast<unsigned>(srvHr));
    fprintf(f, "decision=%s\r\n", SUCCEEDED(createHr) && SUCCEEDED(srvHr) ? "DEPTH_SRV_ONLY_OK" : "DEPTH_SRV_ONLY_FAILED");
    fclose(f);
}

void WriteDepthSrvCopyReport(const D3D11_TEXTURE2D_DESC& tex, HRESULT createHr, HRESULT srvHr, HRESULT copyHr, DXGI_FORMAT srvFormat) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\depth_srv_copy.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    fprintf(f, "AK DIBR depth SRV copy probe\r\n");
    fprintf(f, "texture_width=%u\r\n", tex.Width);
    fprintf(f, "texture_height=%u\r\n", tex.Height);
    fprintf(f, "texture_format=%u\r\n", static_cast<unsigned>(tex.Format));
    fprintf(f, "texture_samples=%u\r\n", tex.SampleDesc.Count);
    fprintf(f, "source_bind_flags=0x%X\r\n", tex.BindFlags);
    fprintf(f, "copy_bind_flags=0x%X\r\n", D3D11_BIND_SHADER_RESOURCE);
    fprintf(f, "srv_format=%u\r\n", static_cast<unsigned>(srvFormat));
    fprintf(f, "srv_dimension=%u\r\n", tex.SampleDesc.Count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D);
    fprintf(f, "create_hr=0x%08X\r\n", static_cast<unsigned>(createHr));
    fprintf(f, "srv_hr=0x%08X\r\n", static_cast<unsigned>(srvHr));
    fprintf(f, "copy_hr=0x%08X\r\n", static_cast<unsigned>(copyHr));
    fprintf(f, "decision=%s\r\n", SUCCEEDED(createHr) && SUCCEEDED(srvHr) && SUCCEEDED(copyHr) ? "DEPTH_SRV_COPY_OK" : "DEPTH_SRV_COPY_FAILED");
    fclose(f);
}

void WriteDibrShaderReport(const D3D11_TEXTURE2D_DESC& tex, bool ok) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\dibr_shader.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    fprintf(f, "AK DIBR shader probe\r\n");
    fprintf(f, "depth_width=%u\r\n", tex.Width);
    fprintf(f, "depth_height=%u\r\n", tex.Height);
    fprintf(f, "depth_format=%u\r\n", static_cast<unsigned>(tex.Format));
    fprintf(f, "depth_samples=%u\r\n", tex.SampleDesc.Count);
    fprintf(f, "shader_ready=%s\r\n", ok ? "yes" : "no");
    fprintf(f, "vs=%p\r\n", static_cast<void*>(g_dibrVs));
    fprintf(f, "ps=%p\r\n", static_cast<void*>(g_dibrPs));
    fprintf(f, "sampler=%p\r\n", static_cast<void*>(g_dibrSampler));
    fprintf(f, "raster=%p\r\n", static_cast<void*>(g_dibrRaster));
    fprintf(f, "depth_state=%p\r\n", static_cast<void*>(g_dibrDepthState));
    fprintf(f, "blend=%p\r\n", static_cast<void*>(g_dibrBlend));
    fprintf(f, "params_cb=%p\r\n", static_cast<void*>(g_dibrParamsCb));
    fprintf(f, "decision=%s\r\n", ok ? "DIBR_SHADER_ONLY_OK" : "DIBR_SHADER_ONLY_FAILED");
    fclose(f);
}

void WriteDibrShaderCreateReport(const D3D11_TEXTURE2D_DESC& tex, HRESULT vsHr, HRESULT psHr) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\dibr_shader_create.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    size_t psBytes = 0;
    DibrPixelShaderBytecode(tex.SampleDesc.Count, psBytes);
    fprintf(f, "AK DIBR shader create probe\r\n");
    fprintf(f, "depth_width=%u\r\n", tex.Width);
    fprintf(f, "depth_height=%u\r\n", tex.Height);
    fprintf(f, "depth_format=%u\r\n", static_cast<unsigned>(tex.Format));
    fprintf(f, "depth_samples=%u\r\n", tex.SampleDesc.Count);
    fprintf(f, "vs_bytecode_bytes=%zu\r\n", sizeof(g_dibr_vs_bytecode));
    fprintf(f, "ps_bytecode_bytes=%zu\r\n", psBytes);
    fprintf(f, "ps_variant=%s\r\n", tex.SampleDesc.Count > 1 ? "msaa4" : "2d");
    fprintf(f, "vs_create_hr=0x%08X\r\n", static_cast<unsigned>(vsHr));
    fprintf(f, "ps_create_hr=0x%08X\r\n", static_cast<unsigned>(psHr));
    fprintf(f, "vs=%p\r\n", static_cast<void*>(g_dibrVs));
    fprintf(f, "ps=%p\r\n", static_cast<void*>(g_dibrPs));
    fprintf(f, "decision=%s\r\n", SUCCEEDED(vsHr) && SUCCEEDED(psHr) && g_dibrVs != nullptr && g_dibrPs != nullptr
            ? "DIBR_SHADER_CREATE_OK" : "DIBR_SHADER_CREATE_FAILED");
    fclose(f);
}

void WriteDibrStateCreateReport(const D3D11_TEXTURE2D_DESC& tex, HRESULT samplerHr, HRESULT rasterHr,
                                HRESULT depthHr, HRESULT blendHr, HRESULT cbHr) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\dibr_state_create.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    fprintf(f, "AK DIBR state create probe\r\n");
    fprintf(f, "depth_width=%u\r\n", tex.Width);
    fprintf(f, "depth_height=%u\r\n", tex.Height);
    fprintf(f, "depth_format=%u\r\n", static_cast<unsigned>(tex.Format));
    fprintf(f, "depth_samples=%u\r\n", tex.SampleDesc.Count);
    fprintf(f, "sampler_hr=0x%08X\r\n", static_cast<unsigned>(samplerHr));
    fprintf(f, "raster_hr=0x%08X\r\n", static_cast<unsigned>(rasterHr));
    fprintf(f, "depth_state_hr=0x%08X\r\n", static_cast<unsigned>(depthHr));
    fprintf(f, "blend_hr=0x%08X\r\n", static_cast<unsigned>(blendHr));
    fprintf(f, "params_cb_hr=0x%08X\r\n", static_cast<unsigned>(cbHr));
    fprintf(f, "sampler=%p\r\n", static_cast<void*>(g_dibrSampler));
    fprintf(f, "raster=%p\r\n", static_cast<void*>(g_dibrRaster));
    fprintf(f, "depth_state=%p\r\n", static_cast<void*>(g_dibrDepthState));
    fprintf(f, "blend=%p\r\n", static_cast<void*>(g_dibrBlend));
    fprintf(f, "params_cb=%p\r\n", static_cast<void*>(g_dibrParamsCb));
    fprintf(f, "decision=%s\r\n", SUCCEEDED(samplerHr) && SUCCEEDED(rasterHr) && SUCCEEDED(depthHr) &&
            SUCCEEDED(blendHr) && SUCCEEDED(cbHr) &&
            g_dibrSampler != nullptr && g_dibrRaster != nullptr && g_dibrDepthState != nullptr &&
            g_dibrBlend != nullptr && g_dibrParamsCb != nullptr
            ? "DIBR_STATE_CREATE_OK" : "DIBR_STATE_CREATE_FAILED");
    fclose(f);
}

void WriteDibrSetupReport(const D3D11_TEXTURE2D_DESC& tex, bool ok) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\dibr_setup.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    size_t psBytes = 0;
    DibrPixelShaderBytecode(tex.SampleDesc.Count, psBytes);
    fprintf(f, "AK DIBR persistent setup probe\r\n");
    fprintf(f, "depth_width=%u\r\n", tex.Width);
    fprintf(f, "depth_height=%u\r\n", tex.Height);
    fprintf(f, "depth_format=%u\r\n", static_cast<unsigned>(tex.Format));
    fprintf(f, "depth_samples=%u\r\n", tex.SampleDesc.Count);
    fprintf(f, "vs_bytecode_bytes=%zu\r\n", sizeof(g_dibr_vs_bytecode));
    fprintf(f, "ps_bytecode_bytes=%zu\r\n", psBytes);
    fprintf(f, "ps_variant=%s\r\n", tex.SampleDesc.Count > 1 ? "msaa4" : "2d");
    fprintf(f, "setup_ready=%s\r\n", ok ? "yes" : "no");
    fprintf(f, "vs=%p\r\n", static_cast<void*>(g_dibrVs));
    fprintf(f, "ps=%p\r\n", static_cast<void*>(g_dibrPs));
    fprintf(f, "sampler=%p\r\n", static_cast<void*>(g_dibrSampler));
    fprintf(f, "raster=%p\r\n", static_cast<void*>(g_dibrRaster));
    fprintf(f, "depth_state=%p\r\n", static_cast<void*>(g_dibrDepthState));
    fprintf(f, "blend=%p\r\n", static_cast<void*>(g_dibrBlend));
    fprintf(f, "params_cb=%p\r\n", static_cast<void*>(g_dibrParamsCb));
    fprintf(f, "decision=%s\r\n", ok ? "DIBR_SETUP_OK" : "DIBR_SETUP_FAILED");
    fclose(f);
}

void WriteDibrSetupDepthCopyReport(const D3D11_TEXTURE2D_DESC& tex, bool setupOk,
                                   HRESULT createHr, HRESULT srvHr, HRESULT copyHr,
                                   DXGI_FORMAT srvFormat, ID3D11Texture2D* depthCopy,
                                   ID3D11ShaderResourceView* depthSrv) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\dibr_setup_depth_copy.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    size_t psBytes = 0;
    DibrPixelShaderBytecode(tex.SampleDesc.Count, psBytes);
    fprintf(f, "AK DIBR setup plus depth SRV-copy probe\r\n");
    fprintf(f, "depth_width=%u\r\n", tex.Width);
    fprintf(f, "depth_height=%u\r\n", tex.Height);
    fprintf(f, "depth_format=%u\r\n", static_cast<unsigned>(tex.Format));
    fprintf(f, "depth_samples=%u\r\n", tex.SampleDesc.Count);
    fprintf(f, "source_bind_flags=0x%X\r\n", tex.BindFlags);
    fprintf(f, "copy_bind_flags=0x%X\r\n", D3D11_BIND_SHADER_RESOURCE);
    fprintf(f, "srv_format=%u\r\n", static_cast<unsigned>(srvFormat));
    fprintf(f, "srv_dimension=%u\r\n", tex.SampleDesc.Count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D);
    fprintf(f, "vs_bytecode_bytes=%zu\r\n", sizeof(g_dibr_vs_bytecode));
    fprintf(f, "ps_bytecode_bytes=%zu\r\n", psBytes);
    fprintf(f, "ps_variant=%s\r\n", tex.SampleDesc.Count > 1 ? "msaa4" : "2d");
    fprintf(f, "setup_ready=%s\r\n", setupOk ? "yes" : "no");
    fprintf(f, "create_hr=0x%08X\r\n", static_cast<unsigned>(createHr));
    fprintf(f, "srv_hr=0x%08X\r\n", static_cast<unsigned>(srvHr));
    fprintf(f, "copy_hr=0x%08X\r\n", static_cast<unsigned>(copyHr));
    fprintf(f, "vs=%p\r\n", static_cast<void*>(g_dibrVs));
    fprintf(f, "ps=%p\r\n", static_cast<void*>(g_dibrPs));
    fprintf(f, "sampler=%p\r\n", static_cast<void*>(g_dibrSampler));
    fprintf(f, "raster=%p\r\n", static_cast<void*>(g_dibrRaster));
    fprintf(f, "depth_state=%p\r\n", static_cast<void*>(g_dibrDepthState));
    fprintf(f, "blend=%p\r\n", static_cast<void*>(g_dibrBlend));
    fprintf(f, "params_cb=%p\r\n", static_cast<void*>(g_dibrParamsCb));
    fprintf(f, "depth_copy=%p\r\n", static_cast<void*>(depthCopy));
    fprintf(f, "depth_srv=%p\r\n", static_cast<void*>(depthSrv));
    fprintf(f, "decision=%s\r\n", setupOk && SUCCEEDED(createHr) && SUCCEEDED(srvHr) && SUCCEEDED(copyHr) &&
            depthCopy != nullptr && depthSrv != nullptr ? "DIBR_SETUP_DEPTH_COPY_OK" : "DIBR_SETUP_DEPTH_COPY_FAILED");
    fclose(f);
}

void WriteDibrColorWarpReport(const D3D11_TEXTURE2D_DESC& bb, bool ok) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\dibr_color_warp.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    fprintf(f, "AK DIBR color/warp target create probe\r\n");
    fprintf(f, "backbuffer_width=%u\r\n", bb.Width);
    fprintf(f, "backbuffer_height=%u\r\n", bb.Height);
    fprintf(f, "backbuffer_format=%u\r\n", static_cast<unsigned>(bb.Format));
    fprintf(f, "backbuffer_samples=%u\r\n", bb.SampleDesc.Count);
    fprintf(f, "color_copy=%p\r\n", static_cast<void*>(g_dibrColorCopy));
    fprintf(f, "color_srv=%p\r\n", static_cast<void*>(g_dibrColorSrv));
    fprintf(f, "warp_tex=%p\r\n", static_cast<void*>(g_dibrWarpTex));
    fprintf(f, "warp_rtv=%p\r\n", static_cast<void*>(g_dibrWarpRtv));
    fprintf(f, "color_w=%u\r\n", g_dibrColorW);
    fprintf(f, "color_h=%u\r\n", g_dibrColorH);
    fprintf(f, "color_format=%u\r\n", static_cast<unsigned>(g_dibrColorFmt));
    fprintf(f, "warp_w=%u\r\n", g_dibrWarpW);
    fprintf(f, "warp_h=%u\r\n", g_dibrWarpH);
    fprintf(f, "decision=%s\r\n", ok && g_dibrColorCopy != nullptr && g_dibrColorSrv != nullptr &&
            g_dibrWarpTex != nullptr && g_dibrWarpRtv != nullptr ? "DIBR_COLOR_WARP_OK" : "DIBR_COLOR_WARP_FAILED");
    fclose(f);
}

void WriteDibrColorCopyReport(const D3D11_TEXTURE2D_DESC& bb, bool setupOk, HRESULT copyHr) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\dibr_color_copy.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    fprintf(f, "AK DIBR color copy probe\r\n");
    fprintf(f, "backbuffer_width=%u\r\n", bb.Width);
    fprintf(f, "backbuffer_height=%u\r\n", bb.Height);
    fprintf(f, "backbuffer_format=%u\r\n", static_cast<unsigned>(bb.Format));
    fprintf(f, "backbuffer_samples=%u\r\n", bb.SampleDesc.Count);
    fprintf(f, "setup_ready=%s\r\n", setupOk ? "yes" : "no");
    fprintf(f, "copy_hr=0x%08X\r\n", static_cast<unsigned>(copyHr));
    fprintf(f, "color_copy=%p\r\n", static_cast<void*>(g_dibrColorCopy));
    fprintf(f, "color_srv=%p\r\n", static_cast<void*>(g_dibrColorSrv));
    fprintf(f, "warp_tex=%p\r\n", static_cast<void*>(g_dibrWarpTex));
    fprintf(f, "warp_rtv=%p\r\n", static_cast<void*>(g_dibrWarpRtv));
    fprintf(f, "color_w=%u\r\n", g_dibrColorW);
    fprintf(f, "color_h=%u\r\n", g_dibrColorH);
    fprintf(f, "color_format=%u\r\n", static_cast<unsigned>(g_dibrColorFmt));
    fprintf(f, "warp_w=%u\r\n", g_dibrWarpW);
    fprintf(f, "warp_h=%u\r\n", g_dibrWarpH);
    fprintf(f, "decision=%s\r\n", setupOk && SUCCEEDED(copyHr) && g_dibrColorCopy != nullptr && g_dibrColorSrv != nullptr &&
            g_dibrWarpTex != nullptr && g_dibrWarpRtv != nullptr ? "DIBR_COLOR_COPY_OK" : "DIBR_COLOR_COPY_FAILED");
    fclose(f);
}

void WriteDibrBindRestoreReport(const D3D11_TEXTURE2D_DESC& bb, bool setupOk, HRESULT colorCopyHr,
                                HRESULT mapHr, bool hadDepthSrv) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\dibr_bind_restore.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    fprintf(f, "AK DIBR bind/restore no-draw probe\r\n");
    fprintf(f, "backbuffer_width=%u\r\n", bb.Width);
    fprintf(f, "backbuffer_height=%u\r\n", bb.Height);
    fprintf(f, "backbuffer_format=%u\r\n", static_cast<unsigned>(bb.Format));
    fprintf(f, "backbuffer_samples=%u\r\n", bb.SampleDesc.Count);
    fprintf(f, "depth_width=%u\r\n", g_dibrDepthW);
    fprintf(f, "depth_height=%u\r\n", g_dibrDepthH);
    fprintf(f, "depth_samples=%u\r\n", g_dibrDepthSamples);
    fprintf(f, "setup_ready=%s\r\n", setupOk ? "yes" : "no");
    fprintf(f, "had_depth_srv=%s\r\n", hadDepthSrv ? "yes" : "no");
    fprintf(f, "color_copy_hr=0x%08X\r\n", static_cast<unsigned>(colorCopyHr));
    fprintf(f, "params_map_hr=0x%08X\r\n", static_cast<unsigned>(mapHr));
    fprintf(f, "vs=%p\r\n", static_cast<void*>(g_dibrVs));
    fprintf(f, "ps=%p\r\n", static_cast<void*>(g_dibrPs));
    fprintf(f, "sampler=%p\r\n", static_cast<void*>(g_dibrSampler));
    fprintf(f, "raster=%p\r\n", static_cast<void*>(g_dibrRaster));
    fprintf(f, "depth_state=%p\r\n", static_cast<void*>(g_dibrDepthState));
    fprintf(f, "blend=%p\r\n", static_cast<void*>(g_dibrBlend));
    fprintf(f, "params_cb=%p\r\n", static_cast<void*>(g_dibrParamsCb));
    fprintf(f, "color_copy=%p\r\n", static_cast<void*>(g_dibrColorCopy));
    fprintf(f, "color_srv=%p\r\n", static_cast<void*>(g_dibrColorSrv));
    fprintf(f, "depth_srv=%p\r\n", static_cast<void*>(g_dibrDepthSrv));
    fprintf(f, "warp_tex=%p\r\n", static_cast<void*>(g_dibrWarpTex));
    fprintf(f, "warp_rtv=%p\r\n", static_cast<void*>(g_dibrWarpRtv));
    fprintf(f, "decision=%s\r\n", setupOk && hadDepthSrv && SUCCEEDED(colorCopyHr) && SUCCEEDED(mapHr)
            ? "DIBR_BIND_RESTORE_OK" : "DIBR_BIND_RESTORE_FAILED");
    fclose(f);
}

void WriteDibrDrawReport(const D3D11_TEXTURE2D_DESC& bb, bool drawOk, bool hadDepthSrv) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\dibr_draw.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    fprintf(f, "AK DIBR offscreen draw probe\r\n");
    fprintf(f, "backbuffer_width=%u\r\n", bb.Width);
    fprintf(f, "backbuffer_height=%u\r\n", bb.Height);
    fprintf(f, "backbuffer_format=%u\r\n", static_cast<unsigned>(bb.Format));
    fprintf(f, "backbuffer_samples=%u\r\n", bb.SampleDesc.Count);
    fprintf(f, "depth_width=%u\r\n", g_dibrDepthW);
    fprintf(f, "depth_height=%u\r\n", g_dibrDepthH);
    fprintf(f, "depth_samples=%u\r\n", g_dibrDepthSamples);
    fprintf(f, "had_depth_srv=%s\r\n", hadDepthSrv ? "yes" : "no");
    fprintf(f, "draw_ok=%s\r\n", drawOk ? "yes" : "no");
    fprintf(f, "vs=%p\r\n", static_cast<void*>(g_dibrVs));
    fprintf(f, "ps=%p\r\n", static_cast<void*>(g_dibrPs));
    fprintf(f, "sampler=%p\r\n", static_cast<void*>(g_dibrSampler));
    fprintf(f, "raster=%p\r\n", static_cast<void*>(g_dibrRaster));
    fprintf(f, "depth_state=%p\r\n", static_cast<void*>(g_dibrDepthState));
    fprintf(f, "blend=%p\r\n", static_cast<void*>(g_dibrBlend));
    fprintf(f, "params_cb=%p\r\n", static_cast<void*>(g_dibrParamsCb));
    fprintf(f, "color_copy=%p\r\n", static_cast<void*>(g_dibrColorCopy));
    fprintf(f, "color_srv=%p\r\n", static_cast<void*>(g_dibrColorSrv));
    fprintf(f, "depth_srv=%p\r\n", static_cast<void*>(g_dibrDepthSrv));
    fprintf(f, "warp_tex=%p\r\n", static_cast<void*>(g_dibrWarpTex));
    fprintf(f, "warp_rtv=%p\r\n", static_cast<void*>(g_dibrWarpRtv));
    fprintf(f, "decision=%s\r\n", drawOk && hadDepthSrv ? "DIBR_DRAW_OK" : "DIBR_DRAW_FAILED");
    fclose(f);
}

void ReleaseDibrSetupObjects() noexcept
{
    SafeRelease(g_dibrVs);
    SafeRelease(g_dibrPs);
    SafeRelease(g_dibrDepthDebugPs);
    SafeRelease(g_dibrSampler);
    SafeRelease(g_dibrRaster);
    SafeRelease(g_dibrDepthState);
    SafeRelease(g_dibrBlend);
    SafeRelease(g_dibrParamsCb);
    g_dibrShadersReady = false;
    g_dibrShadersTried = false;
}

void ReleaseDibrColorWarpObjects() noexcept
{
    SafeRelease(g_dibrColorSrv);
    SafeRelease(g_dibrColorCopy);
    SafeRelease(g_dibrWarpRtv);
    SafeRelease(g_dibrWarpTex);
    g_dibrColorW = 0;
    g_dibrColorH = 0;
    g_dibrColorFmt = DXGI_FORMAT_UNKNOWN;
    g_dibrWarpW = 0;
    g_dibrWarpH = 0;
}

void ReleaseDibrDepthObjects() noexcept
{
    SafeRelease(g_dibrDepthSrv);
    SafeRelease(g_dibrDepthCopy);
    g_dibrDepthReady.store(false);
    g_dibrDepthFromLiveSrv.store(false);
    g_dibrDepthW = 0;
    g_dibrDepthH = 0;
    g_dibrDepthSamples = 0;
}

bool CompileDibrShadersProbe(UINT depthSamples, bool& vsOk, bool& psOk) noexcept
{
    vsOk = false;
    psOk = false;
    const bool msaaDepth = depthSamples > 1;
    const char* commonPrefix =
        "Texture2D colorTex : register(t0);\n"
        "SamplerState s0 : register(s0);\n"
        "cbuffer Params : register(b0) { float gain; float convergence; float signv; float depthW; float depthH; float pad0; float pad1; float pad2; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut VSMain(uint id : SV_VertexID) { float2 p=float2((id==2)?3.0:-1.0,(id==1)?3.0:-1.0); VSOut o; o.pos=float4(p,0,1); o.uv=float2((p.x+1.0)*0.5,1.0-((p.y+1.0)*0.5)); return o; }\n";
    char depthDeclMsaa[512] = {};
    snprintf(depthDeclMsaa, sizeof(depthDeclMsaa),
        "Texture2DMS<float,%u> depthTex : register(t1);\n"
        "float ReadDepth(float2 uv) { uint2 p=uint2(saturate(uv)*float2(max(depthW-1.0,1.0),max(depthH-1.0,1.0))); return depthTex.Load(p,0); }\n",
        depthSamples > 0 ? depthSamples : 4u);
    const char* depthDecl =
        "Texture2D<float> depthTex : register(t1);\n"
        "float ReadDepth(float2 uv) { return depthTex.Sample(s0, saturate(uv)); }\n";
    const char* psBody =
        "float4 PSMain(VSOut i) : SV_Target {\n"
        "  float d=ReadDepth(i.uv);\n"
        "  float disp=signv * gain * (convergence - d);\n"
        "  float2 uv=saturate(float2(i.uv.x + disp, i.uv.y));\n"
        "  return colorTex.Sample(s0, uv);\n"
        "}\n";
    char shader[4096] = {};
    snprintf(shader, sizeof(shader), "%s%s%s", commonPrefix, msaaDepth ? depthDeclMsaa : depthDecl, psBody);

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    vsOk = CompileDibrShader(shader, "VSMain", "vs_4_0", &vs);
    psOk = CompileDibrShader(shader, "PSMain", "ps_4_0", &ps);
    if (vs != nullptr) vs->Release();
    if (ps != nullptr) ps->Release();
    return vsOk && psOk;
}

void WriteDibrShaderCompileReport(const D3D11_TEXTURE2D_DESC& tex, bool vsOk, bool psOk) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\dibr_shader_compile.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    fprintf(f, "AK DIBR shader compile probe\r\n");
    fprintf(f, "depth_width=%u\r\n", tex.Width);
    fprintf(f, "depth_height=%u\r\n", tex.Height);
    fprintf(f, "depth_format=%u\r\n", static_cast<unsigned>(tex.Format));
    fprintf(f, "depth_samples=%u\r\n", tex.SampleDesc.Count);
    fprintf(f, "vs_compile=%s\r\n", vsOk ? "ok" : "failed");
    fprintf(f, "ps_compile=%s\r\n", psOk ? "ok" : "failed");
    fprintf(f, "decision=%s\r\n", vsOk && psOk ? "DIBR_SHADER_COMPILE_OK" : "DIBR_SHADER_COMPILE_FAILED");
    fclose(f);
}

void WriteCompilerLoadReport(const D3D11_TEXTURE2D_DESC& tex, HMODULE compiler, void* compileProc, const wchar_t* dllName) noexcept
{
    const std::wstring dir = DibrProofDirectory();
    const std::wstring path = dir + L"\\dibr_compiler_load.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f == nullptr) return;
    wchar_t dllPath[MAX_PATH] = {};
    if (compiler != nullptr) GetModuleFileNameW(compiler, dllPath, MAX_PATH);
    fwprintf(f, L"AK DIBR compiler load probe\r\n");
    fwprintf(f, L"depth_width=%u\r\n", tex.Width);
    fwprintf(f, L"depth_height=%u\r\n", tex.Height);
    fwprintf(f, L"depth_format=%u\r\n", static_cast<unsigned>(tex.Format));
    fwprintf(f, L"depth_samples=%u\r\n", tex.SampleDesc.Count);
    fwprintf(f, L"requested_dll=%ls\r\n", dllName != nullptr ? dllName : L"(null)");
    fwprintf(f, L"compiler_handle=0x%p\r\n", static_cast<void*>(compiler));
    fwprintf(f, L"compile_proc=0x%p\r\n", compileProc);
    fwprintf(f, L"loaded_path=%ls\r\n", dllPath[0] != L'\0' ? dllPath : L"(none)");
    fwprintf(f, L"decision=%ls\r\n", (compiler != nullptr && compileProc != nullptr) ? L"DIBR_COMPILER_LOAD_OK" : L"DIBR_COMPILER_LOAD_FAILED");
    fclose(f);
}

void PollDibrProofHotkey() noexcept
{
    if (!g_dibrProofHotkeyEnabled.load()) return;
    const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (down && !g_dibrProofHotkeyWasDown)
    {
        g_dibrEnabled.store(true);
        g_dibrDepthReady.store(false);
        g_dibrProofArmed.store(true);
        g_dibrProofWritten = false;
        g_dibrProofFrame = 0;
        g_dibrDepthMetaWritten = false;
        g_dibrDepthCopyWritten = false;
        g_dibrDepthSrvWritten = false;
        g_dibrDepthSrvCopyWritten = false;
        g_dibrShaderWritten = false;
        g_dibrShaderCompileWritten = false;
        g_dibrCompilerLoadWritten = false;
        g_dibrShaderCreateWritten = false;
        g_dibrStateCreateWritten = false;
        g_dibrSetupWritten = false;
        g_dibrSetupDepthCopyWritten = false;
        g_dibrColorWarpWritten = false;
        g_dibrColorCopyWritten = false;
        g_dibrBindRestoreWritten = false;
        g_dibrDrawWritten = false;
        XrLog("[AKDIBR] F10 armed DIBR full proof dump; draw + TGA/readback/diff.");
    }
    g_dibrProofHotkeyWasDown = down;
}

void PollDibrDebugHotkey() noexcept
{
    const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (down && !g_dibrDepthDebugHotkeyWasDown)
    {
        const bool next = !g_dibrDepthDebug.load();
        g_dibrDepthDebug.store(next);
        XrLog("[AKDIBR] depth-debug view %s (F8)", next ? "ON" : "OFF");
    }
    g_dibrDepthDebugHotkeyWasDown = down;
}

void PollDesktopDepthDumpHotkey() noexcept
{
    const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (down && !g_desktopDepthDumpHotkeyWasDown)
    {
        g_dibrEnabled.store(true);
        g_desktopDepthDumpArmed.store(true);
        g_desktopDepthDumpWritten = false;
        g_desktopDepthDumpFrame = 0;
        g_dibrDepthReady.store(false);
        DeleteStaleDepthDumpFiles();
        ReleaseDepthScanCandidates();
        ReleaseDibrDepthObjects();
        XrLog("[AKDIBR] F9 armed FLAT desktop depth-map dump; no XR/headset required.");
    }
    g_desktopDepthDumpHotkeyWasDown = down;
}

void PollDibrGainHotkeys() noexcept
{
    const bool up = ((GetAsyncKeyState(VK_F7) | GetAsyncKeyState(VK_PRIOR)) & 0x8000) != 0;
    const bool down = ((GetAsyncKeyState(VK_F6) | GetAsyncKeyState(VK_NEXT)) & 0x8000) != 0;
    const bool reset = ((GetAsyncKeyState(VK_F5) | GetAsyncKeyState(VK_HOME)) & 0x8000) != 0;
    const float step = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 0.001f : 0.005f;
    bool changed = false;

    if (up && !g_dibrGainUpHotkeyWasDown)
    {
        g_dibrGain += step;
        if (g_dibrGain > 0.120f) g_dibrGain = 0.120f;
        changed = true;
    }
    if (down && !g_dibrGainDownHotkeyWasDown)
    {
        g_dibrGain -= step;
        if (g_dibrGain < 0.000f) g_dibrGain = 0.000f;
        changed = true;
    }
    if (reset && !g_dibrGainResetHotkeyWasDown)
    {
        g_dibrGain = 0.035f;
        changed = true;
    }
    if (changed)
    {
        XrLog("[AKDIBR] depth gain=%.3f (F7/PageUp +, F6/PageDown -, F5/Home reset, Shift=fine step)",
              static_cast<double>(g_dibrGain));
    }

    g_dibrGainUpHotkeyWasDown = up;
    g_dibrGainDownHotkeyWasDown = down;
    g_dibrGainResetHotkeyWasDown = reset;
}

void PollDibrScaleHotkeys() noexcept
{
    const bool up = (GetAsyncKeyState(VK_OEM_6) & 0x8000) != 0;      // ]
    const bool down = (GetAsyncKeyState(VK_OEM_4) & 0x8000) != 0;    // [
    const bool reset = (GetAsyncKeyState(VK_OEM_5) & 0x8000) != 0;   // backslash
    const float step = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 2.0f : 8.0f;
    bool changed = false;

    if (up && !g_dibrScaleUpHotkeyWasDown)
    {
        g_dibrDepthScale += step;
        if (g_dibrDepthScale > 192.0f) g_dibrDepthScale = 192.0f;
        changed = true;
    }
    if (down && !g_dibrScaleDownHotkeyWasDown)
    {
        g_dibrDepthScale -= step;
        if (g_dibrDepthScale < 16.0f) g_dibrDepthScale = 16.0f;
        changed = true;
    }
    if (reset && !g_dibrScaleResetHotkeyWasDown)
    {
        g_dibrDepthScale = 96.0f;
        changed = true;
    }
    if (changed)
    {
        XrLog("[AKDIBR] depth scale=%.1f ([ lower, ] higher, \\ reset, Shift=fine step)",
              static_cast<double>(g_dibrDepthScale));
    }

    g_dibrScaleUpHotkeyWasDown = up;
    g_dibrScaleDownHotkeyWasDown = down;
    g_dibrScaleResetHotkeyWasDown = reset;
}

void PollDibrEdgeHotkeys() noexcept
{
    const bool up = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
    const bool down = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
    const bool reset = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    const float step = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 0.10f : 0.50f;
    bool changed = false;

    if (up && !g_dibrEdgeUpHotkeyWasDown)
    {
        g_dibrEdgeGuard += step;
        if (g_dibrEdgeGuard > 8.0f) g_dibrEdgeGuard = 8.0f;
        changed = true;
    }
    if (down && !g_dibrEdgeDownHotkeyWasDown)
    {
        g_dibrEdgeGuard -= step;
        if (g_dibrEdgeGuard < 0.0f) g_dibrEdgeGuard = 0.0f;
        changed = true;
    }
    if (reset && !g_dibrEdgeResetHotkeyWasDown)
    {
        g_dibrEdgeGuard = 3.0f;
        changed = true;
    }
    if (changed)
    {
        XrLog("[AKDIBR] edge guard=%.2f (F4 more ghost guard, F3 less, F2 reset, Shift=fine step)",
              static_cast<double>(g_dibrEdgeGuard));
    }

    g_dibrEdgeUpHotkeyWasDown = up;
    g_dibrEdgeDownHotkeyWasDown = down;
    g_dibrEdgeResetHotkeyWasDown = reset;
}

void RunFlatDepthDebugOverlay(IDXGISwapChain* gameSwapChain) noexcept
{
    if (!g_dibrDepthDebug.load()) return;
    if (gameSwapChain == nullptr || g_context == nullptr || !g_dibrDepthReady.load() || g_dibrDepthSrv == nullptr) return;

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) ||
        backBuffer == nullptr)
    {
        return;
    }

    ID3D11Texture2D* depthDebug = RenderDibrRightEye(backBuffer);
    if (depthDebug != nullptr)
    {
        g_context->CopyResource(backBuffer, depthDebug);
    }
    backBuffer->Release();
}

void RunDesktopDepthMapDump(IDXGISwapChain* gameSwapChain) noexcept
{
    if (!g_desktopDepthDumpArmed.load() || g_desktopDepthDumpWritten || gameSwapChain == nullptr || g_context == nullptr)
    {
        return;
    }

    ++g_desktopDepthDumpFrame;
    const std::wstring dir = DibrProofDirectory();
    const std::wstring reportPath = dir + L"\\desktop_depth_report.txt";

    const bool haveCurrentDepth = g_dibrDepthReady.load() && g_dibrDepthSrv != nullptr;
    const bool haveCandidates = g_depthScanCandidateCount > 0;
    if (!haveCurrentDepth && !haveCandidates)
    {
        if (g_desktopDepthDumpFrame >= 240u)
        {
            WriteDesktopDepthDumpReport(reportPath, false, false, false, true,
                                        "No current DIBR depth or scan candidates became ready within 240 flat Present frames after F9.");
            XrLog("[AKDIBR] FLAT depth-map dump timed out waiting for depth. dir=AK_DIBR_PROOF");
            g_desktopDepthDumpWritten = true;
            g_desktopDepthDumpArmed.store(false);
        }
        return;
    }

    if (g_desktopDepthDumpFrame < 30u) return;

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) ||
        backBuffer == nullptr)
    {
        return;
    }

    const std::wstring gamePath = dir + L"\\desktop_game.tga";
    const std::wstring depthDebugPath = dir + L"\\desktop_depth_debug.tga";
    const std::wstring dibrPath = dir + L"\\desktop_dibr.tga";
    const std::wstring candidateReportPath = dir + L"\\desktop_depth_candidates.txt";

    const bool oldDebug = g_dibrDepthDebug.load();
    const bool gameOk = WriteProofTga(gamePath, backBuffer);

    bool depthDebugOk = false;
    bool dibrOk = false;
    if (haveCurrentDepth)
    {
        g_dibrDepthDebug.store(true);
        ID3D11Texture2D* depthDebug = RenderDibrRightEye(backBuffer);
        depthDebugOk = depthDebug != nullptr && WriteProofTga(depthDebugPath, depthDebug);

        g_dibrDepthDebug.store(false);
        ID3D11Texture2D* dibr = RenderDibrRightEye(backBuffer);
        dibrOk = dibr != nullptr && WriteProofTga(dibrPath, dibr);
    }
    else if (haveCandidates)
    {
        ID3D11Texture2D* depthDebug = RenderDepthCandidateDebug(backBuffer, g_depthScanCandidates[0]);
        depthDebugOk = depthDebug != nullptr && WriteProofTga(depthDebugPath, depthDebug);

        ID3D11Texture2D* dibr = RenderDepthCandidateDibr(backBuffer, g_depthScanCandidates[0]);
        dibrOk = dibr != nullptr && WriteProofTga(dibrPath, dibr);

        const float oldGain = g_dibrGain;
        const struct GainDump
        {
            float gain;
            const wchar_t* name;
        } gainDumps[] = {
            {0.008f, L"\\desktop_dibr_gain_008.tga"},
            {0.015f, L"\\desktop_dibr_gain_015.tga"},
            {0.025f, L"\\desktop_dibr_gain_025.tga"},
            {0.035f, L"\\desktop_dibr_gain_035.tga"},
            {0.050f, L"\\desktop_dibr_gain_050.tga"},
        };
        for (const GainDump& dump : gainDumps)
        {
            g_dibrGain = dump.gain;
            ID3D11Texture2D* gainDibr = RenderDepthCandidateDibr(backBuffer, g_depthScanCandidates[0]);
            if (gainDibr != nullptr)
            {
                WriteProofTga(dir + dump.name, gainDibr);
            }
        }
        g_dibrGain = oldGain;
    }

    bool candidateOk[kDepthScanMaxCandidates] = {};
    unsigned candidateOkCount = 0;
    for (unsigned i = 0; i < g_depthScanCandidateCount && i < kDepthScanMaxCandidates; ++i)
    {
        wchar_t fileName[64] = {};
        _snwprintf_s(fileName, _countof(fileName), _TRUNCATE, L"\\candidate_%02u_depth_debug.tga", i);
        ID3D11Texture2D* candidateDebug = RenderDepthCandidateDebug(backBuffer, g_depthScanCandidates[i]);
        candidateOk[i] = candidateDebug != nullptr && WriteProofTga(dir + fileName, candidateDebug);
        if (candidateOk[i]) ++candidateOkCount;
    }
    WriteDepthCandidateReport(candidateReportPath, candidateOk, candidateOkCount);

    g_dibrDepthDebug.store(oldDebug);
    WriteDesktopDepthDumpReport(reportPath, gameOk, depthDebugOk, dibrOk, false,
                                "Flat desktop dump complete. Inspect candidate_XX_depth_debug.tga first.");
    XrLog("[AKDIBR] FLAT depth-map dump wrote game=%d depth=%d dibr=%d candidates=%u/%u dir=AK_DIBR_PROOF",
          gameOk ? 1 : 0, depthDebugOk ? 1 : 0, dibrOk ? 1 : 0,
          candidateOkCount, g_depthScanCandidateCount);
    g_desktopDepthDumpWritten = true;
    g_desktopDepthDumpArmed.store(false);
    backBuffer->Release();
}

bool RunDibrBindRestoreNoDraw(ID3D11Texture2D* backBuffer) noexcept
{
    if (g_context == nullptr || backBuffer == nullptr || g_dibrDepthSrv == nullptr) return false;
    D3D11_TEXTURE2D_DESC bb = {};
    backBuffer->GetDesc(&bb);
    const bool setupOk = EnsureDibrColorAndWarp(backBuffer) && EnsureDibrShaders();
    HRESULT colorCopyHr = E_FAIL;
    HRESULT mapHr = E_FAIL;
    if (setupOk && g_dibrColorCopy != nullptr)
    {
        g_context->CopyResource(g_dibrColorCopy, FillSource(backBuffer));
        colorCopyHr = S_OK;
    }

    if (setupOk && SUCCEEDED(colorCopyHr))
    {
        ID3D11RenderTargetView* oldRtv[8] = {};
        ID3D11DepthStencilView* oldDsv = nullptr;
        g_context->OMGetRenderTargets(8, oldRtv, &oldDsv);
        D3D11_VIEWPORT oldVp[16] = {};
        UINT oldVpCount = 16;
        g_context->RSGetViewports(&oldVpCount, oldVp);
        ID3D11RasterizerState* oldRs = nullptr;
        g_context->RSGetState(&oldRs);
        ID3D11DepthStencilState* oldDs = nullptr;
        UINT oldStencil = 0;
        g_context->OMGetDepthStencilState(&oldDs, &oldStencil);
        ID3D11BlendState* oldBlend = nullptr;
        float oldBlendFactor[4] = {};
        UINT oldSampleMask = 0xffffffff;
        g_context->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
        D3D11_PRIMITIVE_TOPOLOGY oldTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        g_context->IAGetPrimitiveTopology(&oldTopo);
        ID3D11InputLayout* oldLayout = nullptr;
        g_context->IAGetInputLayout(&oldLayout);
        ID3D11VertexShader* oldVs = nullptr;
        ID3D11PixelShader* oldPs = nullptr;
        g_context->VSGetShader(&oldVs, nullptr, nullptr);
        g_context->PSGetShader(&oldPs, nullptr, nullptr);
        ID3D11ShaderResourceView* oldSrvs[2] = {};
        g_context->PSGetShaderResources(0, 2, oldSrvs);
        ID3D11SamplerState* oldSampler = nullptr;
        g_context->PSGetSamplers(0, 1, &oldSampler);
        ID3D11Buffer* oldCb = nullptr;
        g_context->PSGetConstantBuffers(0, 1, &oldCb);

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(g_dibrWarpW);
        vp.Height = static_cast<float>(g_dibrWarpH);
        vp.MaxDepth = 1.0f;
        const float blendFactor[4] = {};
        g_context->OMSetRenderTargets(1, &g_dibrWarpRtv, nullptr);
        g_context->RSSetViewports(1, &vp);
        g_context->RSSetState(g_dibrRaster);
        g_context->OMSetDepthStencilState(g_dibrDepthState, 0);
        g_context->OMSetBlendState(g_dibrBlend, blendFactor, 0xffffffff);
        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_dibrVs, nullptr, 0);
        ID3D11PixelShader* activePs = g_dibrDepthDebug.load() ? g_dibrDepthDebugPs : g_dibrPs;
        g_context->PSSetShader(activePs, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { g_dibrColorSrv, g_dibrDepthSrv };
        g_context->PSSetShaderResources(0, 2, srvs);
        g_context->PSSetSamplers(0, 1, &g_dibrSampler);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        mapHr = g_context->Map(g_dibrParamsCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(mapHr) && mapped.pData != nullptr)
        {
            float* p = static_cast<float*>(mapped.pData);
            p[0] = g_dibrGain;
            p[1] = g_dibrConvergence;
            p[2] = g_dibrSign;
            p[3] = static_cast<float>(g_dibrDepthW);
            p[4] = static_cast<float>(g_dibrDepthH);
            p[5] = g_dibrDepthScale;
            p[6] = g_dibrEdgeGuard;
            p[7] = g_dibrEdgeThreshold;
            g_context->Unmap(g_dibrParamsCb, 0);
        }
        g_context->PSSetConstantBuffers(0, 1, &g_dibrParamsCb);

        ID3D11ShaderResourceView* nullSrvs[2] = {};
        g_context->PSSetShaderResources(0, 2, nullSrvs);
        g_context->OMSetRenderTargets(8, oldRtv, oldDsv);
        g_context->RSSetViewports(oldVpCount, oldVp);
        g_context->RSSetState(oldRs);
        g_context->OMSetDepthStencilState(oldDs, oldStencil);
        g_context->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
        g_context->IASetPrimitiveTopology(oldTopo);
        g_context->IASetInputLayout(oldLayout);
        g_context->VSSetShader(oldVs, nullptr, 0);
        g_context->PSSetShader(oldPs, nullptr, 0);
        g_context->PSSetShaderResources(0, 2, oldSrvs);
        g_context->PSSetSamplers(0, 1, &oldSampler);
        g_context->PSSetConstantBuffers(0, 1, &oldCb);
        for (auto*& r : oldRtv) SafeRelease(r);
        SafeRelease(oldDsv);
        SafeRelease(oldRs);
        SafeRelease(oldDs);
        SafeRelease(oldBlend);
        SafeRelease(oldLayout);
        SafeRelease(oldVs);
        SafeRelease(oldPs);
        SafeRelease(oldSrvs[0]);
        SafeRelease(oldSrvs[1]);
        SafeRelease(oldSampler);
        SafeRelease(oldCb);
    }

    WriteDibrBindRestoreReport(bb, setupOk, colorCopyHr, mapHr, g_dibrDepthSrv != nullptr);
    return setupOk && SUCCEEDED(colorCopyHr) && SUCCEEDED(mapHr) && g_dibrDepthSrv != nullptr;
}

void RunDesktopDibrProof(IDXGISwapChain* gameSwapChain) noexcept
{
    if (g_dibrDepthMetaOnly.load()) return;
    if (!g_dibrProofArmed.load()) return;
    if (g_dibrProofWritten || gameSwapChain == nullptr || g_context == nullptr) return;
    if (g_dibrColorCopyOnly.load())
    {
        if (!g_dibrColorCopyWritten)
        {
            ID3D11Texture2D* backBuffer = nullptr;
            if (SUCCEEDED(gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) &&
                backBuffer != nullptr)
            {
                D3D11_TEXTURE2D_DESC bb = {};
                backBuffer->GetDesc(&bb);
                ReleaseDibrColorWarpObjects();
                const bool setupOk = EnsureDibrColorAndWarp(backBuffer);
                HRESULT copyHr = E_FAIL;
                if (setupOk && g_dibrColorCopy != nullptr)
                {
                    g_context->CopyResource(g_dibrColorCopy, FillSource(backBuffer));
                    copyHr = S_OK;
                }
                WriteDibrColorCopyReport(bb, setupOk, copyHr);
                XrLog("[AKDIBR] color-copy-only result setup=%d copy=0x%08X backbuffer=%ux%u fmt=%u samples=%u",
                      setupOk ? 1 : 0, static_cast<unsigned>(copyHr),
                      bb.Width, bb.Height, static_cast<unsigned>(bb.Format), bb.SampleDesc.Count);
                ReleaseDibrColorWarpObjects();
                backBuffer->Release();
            }
            g_dibrColorCopyWritten = true;
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        return;
    }
    if (g_dibrColorWarpOnly.load())
    {
        if (!g_dibrColorWarpWritten)
        {
            ID3D11Texture2D* backBuffer = nullptr;
            if (SUCCEEDED(gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) &&
                backBuffer != nullptr)
            {
                D3D11_TEXTURE2D_DESC bb = {};
                backBuffer->GetDesc(&bb);
                ReleaseDibrColorWarpObjects();
                const bool ok = EnsureDibrColorAndWarp(backBuffer);
                WriteDibrColorWarpReport(bb, ok);
                XrLog("[AKDIBR] color/warp create-only result ok=%d backbuffer=%ux%u fmt=%u samples=%u",
                      ok ? 1 : 0, bb.Width, bb.Height, static_cast<unsigned>(bb.Format), bb.SampleDesc.Count);
                ReleaseDibrColorWarpObjects();
                backBuffer->Release();
            }
            g_dibrColorWarpWritten = true;
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        return;
    }
    if (g_dibrBindRestoreOnly.load())
    {
        if (!g_dibrDepthReady.load() || g_dibrDepthSrv == nullptr) return;
        if (!g_dibrBindRestoreWritten)
        {
            ID3D11Texture2D* backBuffer = nullptr;
            if (SUCCEEDED(gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) &&
                backBuffer != nullptr)
            {
                const bool ok = RunDibrBindRestoreNoDraw(backBuffer);
                D3D11_TEXTURE2D_DESC bb = {};
                backBuffer->GetDesc(&bb);
                XrLog("[AKDIBR] bind/restore no-draw result ok=%d backbuffer=%ux%u fmt=%u depth=%ux%u samples=%u",
                      ok ? 1 : 0, bb.Width, bb.Height, static_cast<unsigned>(bb.Format),
                      g_dibrDepthW, g_dibrDepthH, g_dibrDepthSamples);
                backBuffer->Release();
            }
            ReleaseDibrColorWarpObjects();
            ReleaseDibrSetupObjects();
            ReleaseDibrDepthObjects();
            g_dibrBindRestoreWritten = true;
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        return;
    }
    if (g_dibrDrawOnly.load())
    {
        if (!g_dibrDepthReady.load() || g_dibrDepthSrv == nullptr) return;
        if (!g_dibrDrawWritten)
        {
            ID3D11Texture2D* backBuffer = nullptr;
            if (SUCCEEDED(gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) &&
                backBuffer != nullptr)
            {
                D3D11_TEXTURE2D_DESC bb = {};
                backBuffer->GetDesc(&bb);
                ID3D11Texture2D* right = RenderDibrRightEye(backBuffer);
                const bool ok = right != nullptr;
                WriteDibrDrawReport(bb, ok, g_dibrDepthSrv != nullptr);
                XrLog("[AKDIBR] offscreen draw-only result ok=%d backbuffer=%ux%u fmt=%u depth=%ux%u samples=%u",
                      ok ? 1 : 0, bb.Width, bb.Height, static_cast<unsigned>(bb.Format),
                      g_dibrDepthW, g_dibrDepthH, g_dibrDepthSamples);
                backBuffer->Release();
            }
            ReleaseDibrColorWarpObjects();
            ReleaseDibrSetupObjects();
            ReleaseDibrDepthObjects();
            g_dibrDrawWritten = true;
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        return;
    }
    if (!g_dibrDepthReady.load() || g_dibrDepthSrv == nullptr) return;
    if (++g_dibrProofFrame < 90u) return;

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) ||
        backBuffer == nullptr)
    {
        return;
    }

    ID3D11Texture2D* right = RenderDibrRightEye(backBuffer);
    if (right == nullptr)
    {
        backBuffer->Release();
        return;
    }

    const std::wstring dir = DibrProofDirectory();
    const std::wstring leftPath = dir + L"\\left_game.tga";
    const std::wstring rightPath = dir + L"\\right_dibr.tga";
    const std::wstring reportPath = dir + L"\\proof.txt";
    const bool leftOk = WriteProofTga(leftPath, backBuffer);
    const bool rightOk = WriteProofTga(rightPath, right);
    ProofDiff diff = {};
    const bool diffOk = ComputeProofDiff(backBuffer, right, diff);
    WriteProofReport(reportPath, leftOk, rightOk, diffOk, diff);
    const double meanAbs = (diffOk && diff.samples > 0)
        ? static_cast<double>(diff.sumAbsRgb) / (static_cast<double>(diff.samples) * 3.0)
        : 0.0;
    XrLog("[AKDIBR] PROOF dump wrote left=%d right=%d diff=%d samples=%llu meanAbs=%.4f maxAbs=%u dir=AK_DIBR_PROOF",
          leftOk ? 1 : 0, rightOk ? 1 : 0, diffOk ? 1 : 0, diff.samples, meanAbs, diff.maxAbsRgb);
    g_dibrProofWritten = true;
    g_dibrEnabled.store(false);
    g_dibrProofArmed.store(false);
    ReleaseDibrColorWarpObjects();
    ReleaseDibrSetupObjects();
    ReleaseDibrDepthObjects();
    backBuffer->Release();
}

ID3D11Texture2D* RenderDibrRightEye(ID3D11Texture2D* backBuffer) noexcept
{
    if ((!g_dibrEnabled.load() && !g_dibrAutoFp.load()) || !g_dibrDepthReady.load() || g_dibrDepthSrv == nullptr) return nullptr;
    if (g_context == nullptr || backBuffer == nullptr) return nullptr;
    if (!EnsureDibrColorAndWarp(backBuffer) || !EnsureDibrShaders()) return nullptr;

    g_context->CopyResource(g_dibrColorCopy, FillSource(backBuffer));

    ID3D11RenderTargetView* oldRtv[8] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    g_context->OMGetRenderTargets(8, oldRtv, &oldDsv);
    D3D11_VIEWPORT oldVp[16] = {};
    UINT oldVpCount = 16;
    g_context->RSGetViewports(&oldVpCount, oldVp);
    ID3D11RasterizerState* oldRs = nullptr;
    g_context->RSGetState(&oldRs);
    ID3D11DepthStencilState* oldDs = nullptr;
    UINT oldStencil = 0;
    g_context->OMGetDepthStencilState(&oldDs, &oldStencil);
    ID3D11BlendState* oldBlend = nullptr;
    float oldBlendFactor[4] = {};
    UINT oldSampleMask = 0xffffffff;
    g_context->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
    D3D11_PRIMITIVE_TOPOLOGY oldTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    g_context->IAGetPrimitiveTopology(&oldTopo);
    ID3D11InputLayout* oldLayout = nullptr;
    g_context->IAGetInputLayout(&oldLayout);
    ID3D11VertexShader* oldVs = nullptr;
    ID3D11PixelShader* oldPs = nullptr;
    g_context->VSGetShader(&oldVs, nullptr, nullptr);
    g_context->PSGetShader(&oldPs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrvs[2] = {};
    g_context->PSGetShaderResources(0, 2, oldSrvs);
    ID3D11SamplerState* oldSampler = nullptr;
    g_context->PSGetSamplers(0, 1, &oldSampler);
    ID3D11Buffer* oldCb = nullptr;
    g_context->PSGetConstantBuffers(0, 1, &oldCb);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(g_dibrWarpW);
    vp.Height = static_cast<float>(g_dibrWarpH);
    vp.MaxDepth = 1.0f;
    const float blendFactor[4] = {};
    g_context->OMSetRenderTargets(1, &g_dibrWarpRtv, nullptr);
    g_context->RSSetViewports(1, &vp);
    g_context->RSSetState(g_dibrRaster);
    g_context->OMSetDepthStencilState(g_dibrDepthState, 0);
    g_context->OMSetBlendState(g_dibrBlend, blendFactor, 0xffffffff);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_dibrVs, nullptr, 0);
    ID3D11PixelShader* activePs = g_dibrDepthDebug.load() ? g_dibrDepthDebugPs : g_dibrPs;
    g_context->PSSetShader(activePs, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { g_dibrColorSrv, g_dibrDepthSrv };
    g_context->PSSetShaderResources(0, 2, srvs);
    g_context->PSSetSamplers(0, 1, &g_dibrSampler);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(g_context->Map(g_dibrParamsCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData != nullptr)
    {
        float* p = static_cast<float*>(mapped.pData);
        p[0] = g_dibrGain;
        p[1] = g_dibrConvergence;
        p[2] = g_dibrSign;
        p[3] = static_cast<float>(g_dibrDepthW);
        p[4] = static_cast<float>(g_dibrDepthH);
        p[5] = g_dibrDepthScale;
        p[6] = g_dibrEdgeGuard;
        p[7] = g_dibrEdgeThreshold;
        g_context->Unmap(g_dibrParamsCb, 0);
    }
    g_context->PSSetConstantBuffers(0, 1, &g_dibrParamsCb);
    g_context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[2] = {};
    g_context->PSSetShaderResources(0, 2, nullSrvs);
    g_context->OMSetRenderTargets(8, oldRtv, oldDsv);
    g_context->RSSetViewports(oldVpCount, oldVp);
    g_context->RSSetState(oldRs);
    g_context->OMSetDepthStencilState(oldDs, oldStencil);
    g_context->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
    g_context->IASetPrimitiveTopology(oldTopo);
    g_context->IASetInputLayout(oldLayout);
    g_context->VSSetShader(oldVs, nullptr, 0);
    g_context->PSSetShader(oldPs, nullptr, 0);
    g_context->PSSetShaderResources(0, 2, oldSrvs);
    g_context->PSSetSamplers(0, 1, &oldSampler);
    g_context->PSSetConstantBuffers(0, 1, &oldCb);
    for (auto*& r : oldRtv) SafeRelease(r);
    SafeRelease(oldDsv);
    SafeRelease(oldRs);
    SafeRelease(oldDs);
    SafeRelease(oldBlend);
    SafeRelease(oldLayout);
    SafeRelease(oldVs);
    SafeRelease(oldPs);
    SafeRelease(oldSrvs[0]);
    SafeRelease(oldSrvs[1]);
    SafeRelease(oldSampler);
    SafeRelease(oldCb);

    const unsigned n = g_dibrLogCount.fetch_add(1);
    if (n < 12 || (n % 300) == 0)
    {
        XrLog("[AKDIBR] rendered right eye depth=%ux%u samples=%u gain=%.3f scale=%.1f edge=%.2f conv=%.3f sign=%.1f",
              g_dibrDepthW, g_dibrDepthH, g_dibrDepthSamples,
              static_cast<double>(g_dibrGain), static_cast<double>(g_dibrDepthScale),
              static_cast<double>(g_dibrEdgeGuard),
              static_cast<double>(g_dibrConvergence),
              static_cast<double>(g_dibrSign));
    }
    return g_dibrWarpTex;
}

// Two owned history textures (game-backbuffer desc) that hold each eye's last rendered frame for AER.
bool EnsureHistoryTextures() noexcept
{
    if (g_historyReady) return true;
    if (g_device == nullptr || g_bbWidth == 0) return false;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = g_bbWidth;
    d.Height = g_bbHeight;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = static_cast<DXGI_FORMAT>(g_colorFormat);
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (int i = 0; i < 2; ++i)
    {
        if (FAILED(g_device->CreateTexture2D(&d, nullptr, &g_history[i])) || g_history[i] == nullptr)
        {
            XrLog("[AKXR] AER history texture %d FAILED", i);
            return false;
        }
    }
    g_historyReady = true;
    XrLog("[AKXR] AER history textures ready %ux%u (stereo depth path live)", g_bbWidth, g_bbHeight);
    return true;
}

// === AER display-locked cadence — THE flicker fix (ME AER baseline §7/§14.6), ported. AER flips one eye per
// present; if the present rate isn't an integer fraction of the headset Hz, the L/R eye "age" drifts every
// frame = flicker. Fix: measure the headset's true refresh from xrWaitFrame, then pace the present to
// display/2 (each eye must persist 2 display frames) with a QPC spin. Game stays unlocked. Paces ONLY in AER
// mode — DIBR / same-frame render both eyes fresh and must not be halved. ===
bool   g_paceLocked       = false;
double g_displayPeriodSec = 1.0 / 90.0;
XrTime g_lastPredicted    = 0;
double g_paceWarmAccumSec = 0.0;
int    g_paceWarmCount    = 0;
LARGE_INTEGER g_lastPaceQpc = {};
LARGE_INTEGER g_paceQpcFreq = {};

bool AerModeActive() noexcept
{
    return g_stereoOn.load() && !g_dibrEnabled.load() && !g_sameFrameActive.load();
}

void UpdateAerPaceWarmup(XrTime predicted) noexcept
{
    // Manual override: the auto-measure reads the GAME rate when the game runs below display rate, so it can
    // lock to the wrong Hz (and the wrong pace = drift = doubling). If the user set a real Hz, use it directly.
    const int manualHz = g_aerDisplayHz.load(std::memory_order_relaxed);
    if (manualHz > 0)
    {
        const double p = 1.0 / static_cast<double>(manualHz);
        if (!g_paceLocked || fabs(p - g_displayPeriodSec) > 1e-6)
        {
            g_displayPeriodSec = p;
            g_paceLocked = true;
            XrLog("[AKAER] cadence: manual %d Hz -> AER present paced to display/2 = %.2f ms", manualHz, 2000.0 * p);
        }
        return;
    }

    // AUTO (no user setting): ask the runtime the headset's TRUE refresh rate. This is the correct source —
    // the frame-delta measure below reads the GAME rate when the game runs below the display, so it locks to
    // the wrong Hz and paces to the wrong fraction. With the real Hz the mod owns the cadence, no user config.
    if (!g_refreshRateApplied && g_fn.getDisplayRefreshRate != nullptr && g_session != nullptr)
    {
        float hz = 0.0f;
        if (XrSucceeded(g_fn.getDisplayRefreshRate(g_session, &hz)) && hz > 20.0f && hz < 1000.0f)
        {
            g_displayPeriodSec = 1.0 / static_cast<double>(hz);
            g_paceLocked = true;
            g_refreshRateApplied = true;
            XrLog("[AKAER] cadence: runtime reports %.1f Hz -> AER present paced to display/2 = %.2f ms",
                  hz, 2000.0 * g_displayPeriodSec);
            return;
        }
        // query failed this frame — fall through to the frame-delta fallback (better than nothing)
    }

    if (g_paceLocked) return;                                       // stop measuring once locked (no feedback loop)
    if (g_lastPredicted != 0 && predicted > g_lastPredicted)
    {
        const double dt = static_cast<double>(predicted - g_lastPredicted) * 1e-9;        // ns -> s
        if (dt > 0.004 && dt < 0.05) { g_paceWarmAccumSec += dt; ++g_paceWarmCount; }      // sane 250..20 Hz gate
    }
    g_lastPredicted = predicted;
    if (g_paceWarmCount >= 90)
    {
        const double hz = g_paceWarmCount / g_paceWarmAccumSec;
        const double cands[] = {60.0, 72.0, 80.0, 90.0, 100.0, 120.0, 144.0};
        double best = 90.0, err = 1e9;
        for (double c : cands) { const double e = fabs(c - hz); if (e < err) { err = e; best = c; } }
        g_displayPeriodSec = 1.0 / best;
        g_paceLocked = true;
        XrLog("[AKAER] cadence locked: measured ~%.1f Hz -> %.0f Hz; AER present paced to display/2 = %.2f ms",
              hz, best, 2000.0 * g_displayPeriodSec);
    }
}

// THE flicker fix (ME AER baseline §7/§14.6). Hold the present to display/2 with a QPC spin while AER mode
// is active, so the eye-swap lands on a stable integer fraction of the headset refresh = no drift = no
// flicker. Game stays unlocked (this is NOT a user fps cap). No-op for DIBR / same-frame (both eyes fresh).
void PaceDisplayLockedAer() noexcept
{
    if (!g_paceLocked || !AerModeActive()) { g_lastPaceQpc.QuadPart = 0; return; }
    if (g_paceQpcFreq.QuadPart == 0) QueryPerformanceFrequency(&g_paceQpcFreq);
    const double target = 2.0 * g_displayPeriodSec;                // each eye persists 2 display frames
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (g_lastPaceQpc.QuadPart != 0 && g_paceQpcFreq.QuadPart != 0)
    {
        double elapsed = static_cast<double>(now.QuadPart - g_lastPaceQpc.QuadPart) / static_cast<double>(g_paceQpcFreq.QuadPart);
        while (target - elapsed > 0.0)
        {
            if (target - elapsed > 0.0015) Sleep(1);               // coarse sleep, then spin the last ~1.5 ms
            QueryPerformanceCounter(&now);
            elapsed = static_cast<double>(now.QuadPart - g_lastPaceQpc.QuadPart) / static_cast<double>(g_paceQpcFreq.QuadPart);
        }
    }
    QueryPerformanceCounter(&g_lastPaceQpc);
}

void RunFrame(IDXGISwapChain* gameSwapChain) noexcept
{
    // [PERF] frame-time breakdown — bounded log to pinpoint where the AER frame's time actually goes
    // (true present period vs pace-spin vs waitFrame-block vs copy/submit vs endFrame). Measure, don't guess.
    static LARGE_INTEGER s_perfFreq = {}; if (s_perfFreq.QuadPart == 0) QueryPerformanceFrequency(&s_perfFreq);
    LARGE_INTEGER t_start; QueryPerformanceCounter(&t_start);
    PaceDisplayLockedAer();   // THE flicker fix: hold the present to display/2 while AER mode is active (else no-op).
    LARGE_INTEGER t_afterPace; QueryPerformanceCounter(&t_afterPace);
    XrFrameWaitInfo fwi = {};
    fwi.type = XR_TYPE_FRAME_WAIT_INFO_VALUE;
    XrFrameState fs = {};
    fs.type = XR_TYPE_FRAME_STATE_VALUE;
    if (!XrSucceeded(g_fn.waitFrame(g_session, &fwi, &fs))) return;
    LARGE_INTEGER t_afterWait; QueryPerformanceCounter(&t_afterWait);
    UpdateAerPaceWarmup(fs.predictedDisplayTime);   // measure headset refresh until locked (no-op after lock)

    XrFrameBeginInfo fbi = {};
    fbi.type = XR_TYPE_FRAME_BEGIN_INFO_VALUE;
    g_fn.beginFrame(g_session, &fbi);

    // RECENTER: re-anchor g_appSpace to the current head (yaw + pitch + position), discarding roll.
    // Read the head in the stable base LOCAL space, then re-create g_appSpace offset by it. Everything (locate,
    // submit, compositor reprojection) then lives in the recentered frame consistently.
    if (g_recenterReq.exchange(false) && g_localSpace != nullptr)
    {
        XrView rv[2] = {}; rv[0].type = XR_TYPE_VIEW_VALUE; rv[1].type = XR_TYPE_VIEW_VALUE;
        XrViewLocateInfo rli = {};
        rli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        rli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
        rli.displayTime = fs.predictedDisplayTime;
        rli.space = g_localSpace;
        XrViewState rvs = {}; rvs.type = XR_TYPE_VIEW_STATE_VALUE; uint32_t rn = 0;
        if (XrSucceeded(g_fn.locateViews(g_session, &rli, &rvs, 2, &rn, rv)) && rn >= 2 &&
            (rvs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE) != 0)
        {
            XrReferenceSpaceCreateInfo rs = {};
            rs.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
            rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
            rs.poseInReferenceSpace.position = rv[0].pose.position;                 // re-zero lean/position
            rs.poseInReferenceSpace.orientation = RemoveRoll(rv[0].pose.orientation); // current look becomes forward
            XrSpace ns = nullptr;
            if (XrSucceeded(g_fn.createReferenceSpace(g_session, &rs, &ns)) && ns != nullptr)
            {
                if (g_appSpace != nullptr) g_fn.destroySpace(g_appSpace);
                g_appSpace = ns;
                XrLog("[AKXR] RECENTER: appSpace re-anchored to current head");
            }
        }
    }

    // Locate the two eye views (poses + per-eye fov). LOCAL space -> head pose relative to the
    // world-anchored origin. A FIXED (identity) pose is submitted in M1, but the per-eye fovs and the
    // head orientation come from here.
    XrView views[2] = {};
    views[0].type = XR_TYPE_VIEW_VALUE;
    views[1].type = XR_TYPE_VIEW_VALUE;
    bool viewsValid = false;
    {
        XrViewLocateInfo vli = {};
        vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = g_appSpace;
        XrViewState vstate = {};
        vstate.type = XR_TYPE_VIEW_STATE_VALUE;
        uint32_t got = 0;
        const XrResult lvr = g_fn.locateViews(g_session, &vli, &vstate, 2, &got, views);
        const bool oriValid = (vstate.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE) != 0;
        if (XrSucceeded(lvr) && got >= 2)
        {
            viewsValid = true;
            if (oriValid)
            {
                float y = 0, p = 0, rr = 0;
                HeadEulerDegrees(views[0].pose.orientation, y, p, rr);
                // Always keep a live copy for the tuner's HMD readout.
                g_headYawLive.store(y); g_headPitchLive.store(p); g_headRollLive.store(rr);
                // MENU CAMERA FREEZE, at the source. While the Insert tuner is open the game camera
                // must not follow the head — and the submitted pose tag must freeze in the same
                // instant, or the compositor reprojects a static image against a moving head and the
                // world swims. Freezing here fixes the render (ApplyHeadRotation reads these) and the
                // submitted pose (g_lastHeadOrient tags both capture paths) with one gate.
                if (!AKMenu::IsOpen())
                {
                    g_headYaw.store(y); g_headPitch.store(p); g_headRoll.store(rr);
                    g_lastHeadOrient = views[0].pose.orientation;   // for same-frame capture pose-tag
                }
                g_headValid.store(true);
            }
            // Headset eye FOV (the FOV-fill target): the widest half-angle each way so the widened render covers it.
            const float hh = (std::fabs(views[0].fov.angleLeft) > std::fabs(views[0].fov.angleRight))
                                 ? std::fabs(views[0].fov.angleLeft) : std::fabs(views[0].fov.angleRight);
            const float vv = (std::fabs(views[0].fov.angleUp) > std::fabs(views[0].fov.angleDown))
                                 ? std::fabs(views[0].fov.angleUp) : std::fabs(views[0].fov.angleDown);
            if (hh > 0.2f && hh < 1.7f) g_hsHalfH.store(hh);
            if (vv > 0.2f && vv < 1.7f) g_hsHalfV.store(vv);
        }
    }

    bool submitted = false;
    XrCompositionLayerProjection proj = {};
    XrCompositionLayerProjectionView projViews[2] = {};

    if (fs.shouldRender != 0 && gameSwapChain != nullptr && g_context != nullptr &&
        g_eyes[0].swapchain != nullptr && g_eyes[1].swapchain != nullptr)
    {
        ID3D11Texture2D* backBuffer = nullptr;
        if (SUCCEEDED(gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) &&
            backBuffer != nullptr)
        {
            // STEREO (AER) vs MONO. Stereo: the game rendered THIS frame with the active eye's parallax
            // shift (applied in Detour_ViewBuild); copy it into that eye's history + tag its pose, then
            // present BOTH histories (the other eye is ~1 frame old). Mono: same frame to both eyes.
            const bool sameFrame = g_sameFrameActive.load();
            const bool menuOpen = AKMenu::IsOpen();
            // Stereo KEEPS RUNNING while the tuner is open. It used to be forced off here, which
            // meant every stereo control in the menu (virtual IPD, world scale, convergence, eye
            // swap) changed nothing you could see until you closed the panel — the tuning controls
            // were being tuned blind. The panel is composited into both eye images instead, below.
            const bool stereo = g_stereoOn.load() && EnsureHistoryTextures();
            // With the camera held for the tuner, every pose tagged or submitted must be the held one.
            // g_lastHeadOrient stops advancing while the menu is open, so it IS the frozen pose.
            const XrQuaternionf submitOrient = menuOpen ? g_lastHeadOrient : views[0].pose.orientation;
            const int renderedEye = g_activeEye.load() & 1;
            bool dibrPresented = false;
            bool sameFramePairReady = false;
            ID3D11Texture2D* dibrRight = (stereo && !sameFrame) ? RenderDibrRightEye(backBuffer) : nullptr;

            // Resolve the convergence crop BEFORE the eye copies, because the menu blit has to know
            // it to cancel it. Two conditions matter here:
            //  - it must not run on DIBR or SBS-crop paths (they own imageRect themselves), and
            //  - it must not run when both eyes hold the SAME mono image (menus, loading, no pair):
            //    opposite crops on identical pixels is fabricated disparity, i.e. double vision on a
            //    flat frame. That case used to slip through and is a real part of "convergence is broken".
            int convCropPx = 0;
            int eyeCropOffX[2] = {0, 0};
            {
                const bool willDibr = (stereo && dibrRight != nullptr);
                const bool willSbsCrop = stereo && sameFrame && g_sbsFromBackbuffer.load();
                const bool genuinePair = !sameFrame || g_sameFramePairReady.load(std::memory_order_acquire);
                if (stereo && !willDibr && !willSbsCrop && genuinePair)
                {
                    const int W = static_cast<int>(g_eyes[0].width);
                    int s = static_cast<int>(lroundf(g_convergence.load() * static_cast<float>(W)));
                    const int maxS = W / 12;
                    if (s > maxS) s = maxS;
                    if (s < -maxS) s = -maxS;
                    convCropPx = (s < 0) ? -s : s;
                    if (convCropPx > 0)
                    {
                        eyeCropOffX[0] = (s >= 0) ? convCropPx : 0;
                        eyeCropOffX[1] = (s >= 0) ? 0 : convCropPx;
                    }
                }
            }
            g_menuOverlayOffsetX[0] = eyeCropOffX[0];
            g_menuOverlayOffsetX[1] = eyeCropOffX[1];
            if (stereo && dibrRight != nullptr)
            {
                // DIBR: real current frame to left eye, depth-reprojected current frame to right eye.
                // This bypasses AER shimmer and same-frame double-render failures.
                if (g_dibrDepthDebug.load())
                {
                    CopyTexToEye(0, dibrRight);
                    CopyTexToEye(1, dibrRight);
                }
                else
                {
                    CopyToEye(0, backBuffer);
                    CopyTexToEye(1, dibrRight);
                }
                dibrPresented = true;
            }
            else if (stereo && sameFrame && g_sbsFromBackbuffer.load())
            {
                // AK SAME-FRAME SPLIT: both eyes are already in this one backbuffer, side by side.
                // Copy the full SBS frame into both eye textures; each eye's imageRect (set in the
                // projView block below) crops its half. No temporal gap — both halves are this frame.
                CopyToEye(0, backBuffer);
                CopyToEye(1, backBuffer);
                if (viewsValid)
                {
                    g_historyOrient[0] = submitOrient;
                    g_historyOrient[1] = submitOrient;
                    g_historyHasOrient[0] = true;
                    g_historyHasOrient[1] = true;
                }
            }
            else if (stereo && sameFrame)
            {
                // COMPLETE-PAIR HISTORY: gameplay supplies two finalized full-screen eyes. Menus and
                // loading frames do not run that path, so their histories are empty/stale; submit the
                // current uncomposed backbuffer to both eyes until this Present owns a complete pair.
                const bool pairReady = g_sameFramePairReady.load(std::memory_order_acquire);
                sameFramePairReady = pairReady;
                if (pairReady)
                {
                    g_sfPairSubmits.fetch_add(1, std::memory_order_relaxed);
                    CopyTexToEye(0, g_history[0]);
                    CopyTexToEye(1, g_history[1]);
                }
                else
                {
                    const auto monos = g_sfMonoSubmits.fetch_add(1, std::memory_order_relaxed) + 1;
                    CopyToEye(0, backBuffer);
                    CopyToEye(1, backBuffer);
                    // Keep reporting. The old 4-line cap fired during startup and then went silent, so a
                    // session that fell back for every gameplay frame logged exactly the same four lines
                    // as one that recovered immediately.
                    if (monos <= 4 || (monos % 600) == 0)
                        XrLog("[AKSF] no complete L/R pair this Present -> live mono backbuffer fallback (#%llu)",
                              static_cast<unsigned long long>(monos));
                }
                // Keep the capture-time pose tags. The histories were rendered from the HMD
                // sample available before Present; views[] is a newer predicted submit pose.
            }
            else if (stereo)
            {
                // AER: copy this frame into the active eye's history; present both (other eye ~1 frame old).
                // Source = the scene RT (fills the square eye), not the letterboxed backbuffer. See FillSource.
                g_context->CopyResource(g_history[renderedEye], FillSource(backBuffer));
                if (viewsValid) { g_historyOrient[renderedEye] = submitOrient; g_historyHasOrient[renderedEye] = true; }
                CopyTexToEye(0, g_history[0]);
                CopyTexToEye(1, g_history[1]);
            }
            else
            {
                CopyToEye(0, backBuffer);
                CopyToEye(1, backBuffer);
            }
            backBuffer->Release();

            // MONO fusion + FOV MATCH: both eyes get the SAME image, pose, and a SHARED symmetric fov so
            // they fuse to one image. CRUCIALLY that fov must MATCH the game's render fov — declaring the
            // headset's wide fov over the game's narrow render stretches/magnifies it ("zoomed in, messed
            // up"). Horizontal half = atan(1/m00) (the game's live projection); vertical from the backbuffer
            // aspect so pixels stay square. Auto-tracks the game's zoom/aim/cutscene fov.
            XrFovf monoFov = {};
            // Fallback FOV used only before the game's projection is first read (startup/loading, pm00 still 0).
            // Must be SQUARE-ish (the backbuffer is square) and near the eventual filled FOV — the old 0.59/0.35
            // was wide+short, which showed as "vertically squashed / zoomed out" during the ~17s load transient.
            // AK FIX: this fallback is NOT a rare startup transient here — Arkham Knight writes
            // no scene ViewProjection at all while you sit in the menus (vpWrites stays 0), so
            // the whole menu/loading experience runs on it. A square fallback against AK's 16:9
            // backbuffer is what "stretched vertically in the menus, straightens on load" was.
            // Derive the vertical half-angle from the live backbuffer aspect so pixels stay square.
            {
                const float fbH = 0.72f;
                const float aspWH = (g_bbWidth > 0 && g_bbHeight > 0)
                                        ? (static_cast<float>(g_bbWidth) / static_cast<float>(g_bbHeight)) : 1.7778f;
                const float fbV = std::atan(std::tan(fbH) / aspWH);
                monoFov.angleLeft = -fbH; monoFov.angleRight = fbH; monoFov.angleUp = fbV; monoFov.angleDown = -fbV;
            }
            const float pm00 = g_projM00.load();
            const float pm11 = g_projM11.load();
            if (pm00 > 0.3f)
            {
                const float h = std::atan(1.0f / pm00);
                // Vertical: use the game's actual vertical projection when valid (so widened-FOV fill matches both
                // axes); fall back to backbuffer-aspect (square pixels) if pm11 isn't readable.
                float v;
                if (pm11 > 0.3f) { v = std::atan(1.0f / pm11); }
                else
                {
                    const float aspWH = (g_bbWidth > 0 && g_bbHeight > 0)
                                            ? (static_cast<float>(g_bbWidth) / static_cast<float>(g_bbHeight)) : 1.7778f;
                    v = std::atan(std::tan(h) / aspWH);
                }
                monoFov.angleLeft = -h; monoFov.angleRight = h; monoFov.angleUp = v; monoFov.angleDown = -v;
            }
            // FOV DIAGNOSTIC (all stereo modes): submit FOV vs the projection it was built from. Switch
            // same-frame <-> AER <-> DIBR and compare — if the numbers differ, that's the "zoom out" cause.
            {
                static unsigned s_fl = 0;
                if ((s_fl++ % 90u) == 0u)
                {
                    const char* md = sameFrame ? "SF" : (dibrPresented ? "DIBR" : (stereo ? "AER" : "MONO"));
                    XrLog("[AKSFFOV] mode=%s submit h=%.3f v=%.3f  pm00=%.3f pm11=%.3f  bb=%ux%u",
                          md, static_cast<double>(monoFov.angleRight), static_cast<double>(monoFov.angleUp),
                          static_cast<double>(pm00), static_cast<double>(pm11), g_bbWidth, g_bbHeight);
                }
            }
            // M2a: once the game camera is head-driven, submit at the LIVE head orientation so the render
            // and the compositor agree (world-locked look-around). M1 (head-look off) keeps identity pose.
            const bool submitHeadPose = g_submitHeadPose.load() && viewsValid;
            const bool renderedWithHeadPose = submitHeadPose && (!sameFrame || sameFramePairReady);
            for (int eye = 0; eye < 2; ++eye)
            {
                projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                // Pose: M1 identity; M2a live head orient; M2b STEREO = each eye carries the head orient it
                // was actually rendered with (pose-tag), so the runtime reprojects the ~1-frame-old eye to
                // the current head and it doesn't ghost on rotation.
                projViews[eye].pose = IdentityPose();
                // Roll-removed in all head-following cases: the render is yaw/pitch only, so the submit must be
                // too, or cocking your head tilts the view. Keeps the horizon level.
                if (dibrPresented && submitHeadPose)
                    projViews[eye].pose.orientation = RemoveRoll(submitOrient);
                // The history orientation is only valid when the game camera was
                // actually driven from the submitted head pose. Applying the live
                // HMD orientation while SubmitHeadPose=0 makes the compositor
                // reproject fixed-camera stereo images and produces VR-only
                // geometry swimming/flicker.
                else if (renderedWithHeadPose && stereo && g_historyHasOrient[eye])
                    projViews[eye].pose.orientation = RemoveRoll(g_historyOrient[eye]);
                else if (submitHeadPose)
                    projViews[eye].pose.orientation = RemoveRoll(submitOrient);
                projViews[eye].fov = monoFov;  // both eyes same fov; depth comes from the per-eye image parallax
                projViews[eye].subImage.swapchain = g_eyes[eye].swapchain;
                projViews[eye].subImage.imageArrayIndex = 0;
                projViews[eye].subImage.imageRect.offset.x = 0;
                projViews[eye].subImage.imageRect.offset.y = 0;
                projViews[eye].subImage.imageRect.extent.width = static_cast<int32_t>(g_eyes[eye].width);
                projViews[eye].subImage.imageRect.extent.height = static_cast<int32_t>(g_eyes[eye].height);

                // AK same-frame split: crop each eye to its half of the SBS backbuffer.
                const bool sbsCrop = stereo && sameFrame && g_sbsFromBackbuffer.load();
                if (sbsCrop)
                {
                    const int W = static_cast<int>(g_eyes[eye].width);
                    projViews[eye].subImage.imageRect.offset.x = (eye == 0) ? 0 : (W / 2);
                    projViews[eye].subImage.imageRect.extent.width = W / 2;
                }

                // CONVERGENCE — image-space horizontal shift (the ONLY safe form; ME baseline §5.3/§10.1).
                // NOT a FOV shear (that's a proven dead end: it makes the compositor reproject wrongly =
                // shake/widen). Here both eyes sample the SAME-size window at OPPOSITE horizontal offsets, so
                // there's no per-eye FOV shear — only a relative image shift that pulls the zero-disparity
                // plane in from infinity to a finite distance, so mid/near objects fuse fast with less strain.
                // The declared horizontal FOV is trimmed to match the cropped width (symmetric, both eyes) so
                // there is no horizontal stretch. g_convergence is a signed fraction of width; 0 = plane at inf.
                // Resolved above (it had to be known before the eye copies so the menu blit could
                // cancel it). convCropPx is already 0 on the DIBR / SBS-crop / no-genuine-pair paths.
                if (convCropPx > 0 && !dibrPresented && !sbsCrop)
                {
                    const int W = static_cast<int>(g_eyes[eye].width);
                    projViews[eye].subImage.imageRect.offset.x = eyeCropOffX[eye];
                    projViews[eye].subImage.imageRect.extent.width = W - convCropPx;
                    const float scale = static_cast<float>(W - convCropPx) / static_cast<float>(W);
                    const float hCrop = std::atan(std::tan(monoFov.angleRight) * scale);
                    projViews[eye].fov.angleLeft  = -hCrop;
                    projViews[eye].fov.angleRight =  hCrop;
                }
            }
            proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
            proj.layerFlags = 0;
            proj.space = g_appSpace;
            proj.viewCount = 2;
            proj.views = projViews;
            submitted = true;
            // AER alternation (ME baseline §3): strict L->R->L, one flip per present. This is correct ONLY
            // because PaceDisplayLockedAer() above holds the present to display/2 — so one present == one eye
            // == 2 display frames, and the inter-eye age stays a CONSTANT 2 display frames (no drift = no
            // flicker). The cadence lock is what makes this simple toggle safe; without it the toggle drifts.
            if (stereo && !sameFrame && !dibrPresented) g_activeEye.store(renderedEye ^ 1);
        }
    }

    const XrCompositionLayerBaseHeader* layers[1] = {};
    uint32_t layerCount = 0;
    if (submitted)
    {
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
    }

    XrFrameEndInfo fei = {};
    fei.type = XR_TYPE_FRAME_END_INFO_VALUE;
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE_VALUE;
    fei.layerCount = layerCount;
    fei.layers = layerCount > 0 ? layers : nullptr;
    LARGE_INTEGER t_beforeEnd; QueryPerformanceCounter(&t_beforeEnd);
    g_fn.endFrame(g_session, &fei);
    LARGE_INTEGER t_afterEnd; QueryPerformanceCounter(&t_afterEnd);

    // [PERF] one line / 120 frames. period = wall-clock between successive RunFrame starts = the TRUE present rate.
    // pace = the AER cadence spin (suspected fps cap); wait = waitFrame block; mid = locate+copies+submit-build;
    // endFrame = compositor handoff. Whichever dominates `period` is the thing throttling the framerate.
    {
        static LARGE_INTEGER s_lastStart = {};
        static unsigned s_perfPc = 0;
        const double f = static_cast<double>(s_perfFreq.QuadPart > 0 ? s_perfFreq.QuadPart : 1);
        const double period = (s_lastStart.QuadPart != 0) ? (t_start.QuadPart    - s_lastStart.QuadPart) * 1000.0 / f : 0.0;
        const double pace   = (t_afterPace.QuadPart - t_start.QuadPart)     * 1000.0 / f;
        const double wait   = (t_afterWait.QuadPart - t_afterPace.QuadPart) * 1000.0 / f;
        const double mid    = (t_beforeEnd.QuadPart - t_afterWait.QuadPart) * 1000.0 / f;
        const double endf   = (t_afterEnd.QuadPart  - t_beforeEnd.QuadPart) * 1000.0 / f;
        s_lastStart = t_start;
        if ((s_perfPc++ % 120u) == 0u)
        {
            const char* md = g_sameFrameActive.load() ? "SF" : (g_dibrEnabled.load() ? "DIBR" : (g_stereoOn.load() ? "AER" : "MONO"));
            XrLog("[AKPERF] mode=%s period=%.2fms(~%.0ffps) pace=%.2f wait=%.2f mid=%.2f endFrame=%.2f | paceTarget=%.2fms locked=%d",
                  md, period, period > 0.01 ? 1000.0 / period : 0.0,
                  pace, wait, mid, endf, 2000.0 * g_displayPeriodSec, g_paceLocked ? 1 : 0);
        }
    }

    static uint32_t s_log = 0;
    if ((s_log++ % 120u) == 0u)
    {
        XrLog("[AKXR] frame submit=%d menuOpen=%d layers=%u eye=%ux%u depthDebug=%d gain=%.3f scale=%.1f edge=%.2f head(deg) yaw=%.1f pitch=%.1f roll=%.1f shouldRender=%d",
              submitted ? 1 : 0, AKMenu::IsOpen() ? 1 : 0, layerCount, g_eyes[0].width, g_eyes[0].height,
              g_dibrDepthDebug.load() ? 1 : 0, static_cast<double>(g_dibrGain), static_cast<double>(g_dibrDepthScale),
              static_cast<double>(g_dibrEdgeGuard),
              g_headYaw.load(), g_headPitch.load(), g_headRoll.load(), fs.shouldRender != 0 ? 1 : 0);
    }
}

// ---- Direct LibOVR probe ----------------------------------------------------------------------
// VDXR returns XR_ERROR_FORM_FACTOR_UNAVAILABLE (-35) ONLY when its OVR session fails to start
// (virtualdesktop-openxr/system.cpp: initializeOVR -> ovr_Initialize; ensureOVRSession -> ovr_Create).
// That hides THREE distinct failures behind one code. This replicates VDXR's exact sequence against the
// SAME VD LibOVR DLL and log the RAW ovrResult + message — so the log shows WHICH failure it is.
// Types hand-declared from mbucchia/LibOVR headers (byte-exact); DLL loaded at runtime, nothing linked.
typedef int32_t ovrResult_t;
typedef void* ovrSession_t;
struct OvrInitParams { uint32_t Flags; uint32_t RequestedMinorVersion; void* LogCallback; uintptr_t UserData; uint32_t ConnectionTimeoutMS; uint32_t pad0; };
struct OvrGraphicsLuid { char Reserved[8]; };
struct OvrErrorInfo { ovrResult_t Result; char ErrorString[512]; };
typedef ovrResult_t(__cdecl* PFN_ovrInitialize)(const OvrInitParams*);
typedef ovrResult_t(__cdecl* PFN_ovrCreate)(ovrSession_t*, OvrGraphicsLuid*);
typedef void(__cdecl* PFN_ovrGetLastErrorInfo)(OvrErrorInfo*);
typedef void(__cdecl* PFN_ovrDestroy)(ovrSession_t);
typedef void(__cdecl* PFN_ovrShutdown)();

const char* OvrErrName(int r) noexcept
{
    switch (r)
    {
    case 0:     return "Success";
    case -1007: return "ovrError_NoHmd (VD reachable but exposes NO HMD to this process)";
    case -3000: return "ovrError_Initialize";
    case -3001: return "ovrError_LibLoad (VD Server not running)";
    case -3002: return "ovrError_LibVersion";
    case -3003: return "ovrError_ServiceConnection";
    case -3004: return "ovrError_ServiceVersion";
    case -3024: return "ovrError_RemoteSession";
    default:    return "ovrError_(other)";
    }
}

void ProbeOvrDirect() noexcept
{
    XrLog("[AKOVR] direct LibOVR probe — replicating VDXR's ovr_Initialize/ovr_Create to expose the real -35 reason.");
    wchar_t vdPath[MAX_PATH] = L"C:\\Program Files\\Virtual Desktop Streamer";  // fallback
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Virtual Desktop, Inc.\\Virtual Desktop Streamer", 0, KEY_READ, &k) == ERROR_SUCCESS)
    {
        wchar_t tmp[MAX_PATH] = {};
        DWORD sz = sizeof(tmp), type = 0;
        if (RegQueryValueExW(k, L"Path", nullptr, &type, reinterpret_cast<LPBYTE>(tmp), &sz) == ERROR_SUCCESS && tmp[0] != L'\0')
            wcscpy_s(vdPath, MAX_PATH, tmp);
        RegCloseKey(k);
    }
    wchar_t dll[MAX_PATH] = {};
    _snwprintf_s(dll, MAX_PATH, _TRUNCATE, L"%s\\VirtualDesktop.LibOVRRT64_1.dll", vdPath);
    HMODULE ovr = LoadLibraryW(dll);
    if (ovr == nullptr) { XrLog("[AKOVR] LoadLibrary '%ls' FAILED err=%lu", dll, GetLastError()); return; }
    XrLog("[AKOVR] loaded '%ls'", dll);

    auto pInit = reinterpret_cast<PFN_ovrInitialize>(GetProcAddress(ovr, "ovr_Initialize"));
    auto pCreate = reinterpret_cast<PFN_ovrCreate>(GetProcAddress(ovr, "ovr_Create"));
    auto pErr = reinterpret_cast<PFN_ovrGetLastErrorInfo>(GetProcAddress(ovr, "ovr_GetLastErrorInfo"));
    auto pDestroy = reinterpret_cast<PFN_ovrDestroy>(GetProcAddress(ovr, "ovr_Destroy"));
    auto pShutdown = reinterpret_cast<PFN_ovrShutdown>(GetProcAddress(ovr, "ovr_Shutdown"));
    if (pInit == nullptr || pCreate == nullptr || pErr == nullptr)
    {
        XrLog("[AKOVR] missing exports: init=%p create=%p err=%p", reinterpret_cast<void*>(pInit),
              reinterpret_cast<void*>(pCreate), reinterpret_cast<void*>(pErr));
        return;
    }

    OvrInitParams ip = {};
    ip.Flags = 0x4u | 0x40u;        // ovrInit_RequestVersion | ovrInit_FocusAware — exactly what VDXR passes
    ip.RequestedMinorVersion = 64;  // OVR_MINOR_VERSION (mbucchia/LibOVR)
    const ovrResult_t ir = pInit(&ip);
    OvrErrorInfo ei = {};
    pErr(&ei);
    XrLog("[AKOVR] ovr_Initialize -> %d (%s)  msg='%s'", ir, OvrErrName(ir), ei.ErrorString);
    if (ir < 0)
    {
        XrLog("[AKOVR] >>> OVR INITIALIZE FAILED — THIS is the real reason VDXR returns -35.");
        if (pShutdown != nullptr) pShutdown();
        return;
    }

    ovrSession_t sess = nullptr;
    OvrGraphicsLuid luid = {};
    const ovrResult_t cr = pCreate(&sess, &luid);
    OvrErrorInfo ei2 = {};
    pErr(&ei2);
    XrLog("[AKOVR] ovr_Create -> %d (%s)  msg='%s'", cr, OvrErrName(cr), ei2.ErrorString);
    if (cr < 0)
    {
        XrLog("[AKOVR] >>> OVR CREATE FAILED — THIS is the real reason VDXR returns -35.");
    }
    else
    {
        XrLog("[AKOVR] >>> OVR SESSION CREATED OK in THIS process — OVR itself reaches the HMD here. "
              "The -35 then comes from VDXR's wrapper after ovr_Create (state/timing), not from OVR. Big clue.");
        if (pDestroy != nullptr) pDestroy(sess);
    }
    if (pShutdown != nullptr) pShutdown();
}

// All the C++ bring-up work (uses std::string temporaries) lives here — NO __try, so it can unwind
// normally. XrBringupThread wraps the one call in SEH (a function with __try must not itself need
// object unwinding, so it holds no std::string locals).
bool BringUpSession() noexcept
{
    // (ProbeOvrDirect removed — it was the -35 diagnostic; it double-initialized the OVR session and
    //  caused the ~3s startup pause + the headset lurch when VR engaged. The answer is known; removed.)
    if (!CreateInstance() || !ResolveSessionFunctions())
    {
        XrLog("[AKXR] instance/function bring-up FAILED.");
        return false;
    }
    // GROUND TRUTH for the -35 hunt: log WHICH runtime the loader actually resolved. xrCreateInstance
    // returning success doesn't say whether it's VDXR or (e.g.) Oculus — this does.
    {
        PFN_xrVoidFunction p = nullptr;
        if (XrSucceeded(g_getProc(g_instance, "xrGetInstanceProperties", &p)) && p != nullptr)
        {
            g_getInstanceProperties = reinterpret_cast<PFN_xrGetInstanceProperties>(p);
            XrInstanceProperties ip = {};
            ip.type = XR_TYPE_INSTANCE_PROPERTIES_VALUE;
            if (XrSucceeded(g_getInstanceProperties(g_instance, &ip)))
            {
                const uint64_t v = static_cast<uint64_t>(ip.runtimeVersion);
                XrLog("[AKXR] RUNTIME = '%s'  version %u.%u.%u", ip.runtimeName,
                      static_cast<unsigned>((v >> 48) & 0xffff), static_cast<unsigned>((v >> 32) & 0xffff),
                      static_cast<unsigned>(v & 0xffffffff));
            }
        }
    }
    // The runtime is up (xrCreateInstance succeeded) but the headset may not be connected to
    // Virtual Desktop yet — it is common to launch the game first and put the headset on afterwards. Poll
    // xrGetSystem every 2s (up to ~5 min) so the session comes up whenever the HMD appears.
    XrSystemGetInfo sgi = {};
    sgi.type = XR_TYPE_SYSTEM_GET_INFO_VALUE;
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY_VALUE;
    bool haveHmd = false;
    for (int attempt = 0; attempt < 150; ++attempt)
    {
        XrLog("[AKXR] -> calling xrGetSystem (attempt %d)...", attempt);
        const XrResult gr = g_fn.getSystem(g_instance, &sgi, &g_systemId);
        if (XrSucceeded(gr) && g_systemId != 0) { haveHmd = true; break; }
        XrLog("[AKXR] waiting for HMD attempt %d (xrGetSystem=%s)...", attempt, FmtXr(gr).c_str());
        Sleep(2000);
    }
    if (!haveHmd)
    {
        XrLog("[AKXR] no HMD after ~5 min — OpenXR disabled for this run.");
        return false;
    }
    XrLog("[AKXR] HMD found (systemId set). Finishing session bring-up...");
    if (!CreateSessionAndSwapchains())
    {
        XrLog("[AKXR] session bring-up FAILED after HMD found.");
        return false;
    }
    XrLog("[AKXR] session + 2 eye swapchains + LOCAL space created. Frames driven from the Present thread.");
    return true;
}

// SEH filter: log the exception code + faulting address + WHICH module owns that address (names the
// crashing DLL — e.g. virtualdesktop-openxr.dll vs this dxgi.dll vs d3d11.dll). The last checkpoint
// line in the log names the call that triggered it.
LONG XrCrashFilter(unsigned long code, _EXCEPTION_POINTERS* ep) noexcept
{
    void* addr = (ep != nullptr && ep->ExceptionRecord != nullptr) ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    wchar_t mod[MAX_PATH] = L"?";
    HMODULE h = nullptr;
    if (addr != nullptr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &h) && h != nullptr)
    {
        GetModuleFileNameW(h, mod, MAX_PATH);
    }
    XrLog("[AKXR] CRASH code=0x%08X addr=%p module='%ls'", code, addr, mod);
    return EXCEPTION_EXECUTE_HANDLER;
}

DWORD WINAPI XrBringupThread(LPVOID) noexcept
{
    __try
    {
        if (BringUpSession()) g_ready.store(true);
        else g_bringupFailed.store(true);
    }
    __except (XrCrashFilter(GetExceptionCode(), GetExceptionInformation()))
    {
        XrLog("[AKXR] session bring-up CRASHED — OpenXR disabled for this run (see CRASH line above).");
        g_bringupFailed.store(true);
    }
    return 0;
}

// Run OpenXR ONLY in the actual game exe (BatmanAK.exe), never in a launcher or helper
// process that happens to load this same dxgi.dll. Two OpenXR users in one process deadlock
// after xrCreateInstance — that was the MGS2 lane's hard-won reason for this gate, and it
// costs nothing to keep here.
bool IsGameProcess() noexcept
{
    static int s_decision = -1;  // -1 unknown, 0 no, 1 yes
    if (s_decision != -1) return s_decision == 1;

    if (g_selfModule == nullptr)
    {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&XrBringupThread), &g_selfModule);
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);  // main exe of this process
    const wchar_t* base = path;
    for (DWORD i = 0; i < n; ++i) if (path[i] == L'\\' || path[i] == L'/') base = path + i + 1;
    wchar_t low[MAX_PATH] = {};
    int j = 0;
    for (; base[j] != L'\0' && j < MAX_PATH - 1; ++j)
    {
        wchar_t c = base[j];
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        low[j] = c;
    }
    low[j] = L'\0';
    // Gate OpenXR to the game exe so a session never spins up inside a launcher.
    // This string was "metal gear solid2" when ported from the MGS2 lane — spelled out, so
    // the mgs2->ak rename swept straight past it and XR silently skipped every AK launch.
    const bool isGame = (wcsstr(low, L"batmanak") != nullptr);
    s_decision = isGame ? 1 : 0;
    XrLog("[AKXR] process '%ls' -> %s", base, isGame ? "GAME (OpenXR enabled)"
                                                       : "NOT the game (OpenXR skipped — launcher/other)");
    return isGame;
}

bool CaptureGameDevice(IDXGISwapChain* gameSwapChain, const char* reason) noexcept
{
    if (g_selfModule == nullptr)
    {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&XrBringupThread), &g_selfModule);
    }

    if (g_device != nullptr && g_context != nullptr && g_bbWidth != 0 && g_bbHeight != 0)
    {
        return true;
    }
    if (gameSwapChain == nullptr)
    {
        return false;
    }

    ID3D11Device* device = nullptr;
    if (FAILED(gameSwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || device == nullptr)
    {
        XrLog("[AKXR] CaptureGameDevice(%s): GetDevice failed.", reason != nullptr ? reason : "?");
        return false;
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    ID3D11Texture2D* bb = nullptr;
    D3D11_TEXTURE2D_DESC bd = {};
    if (SUCCEEDED(gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) && bb != nullptr)
    {
        bb->GetDesc(&bd);
        bb->Release();
    }
    else
    {
        XrLog("[AKXR] CaptureGameDevice(%s): GetBuffer(0) failed.", reason != nullptr ? reason : "?");
        if (context != nullptr) context->Release();
        device->Release();
        return false;
    }

    g_device = device;
    g_context = context;
    g_colorFormat = static_cast<int64_t>(bd.Format);
    g_bbWidth = bd.Width;
    g_bbHeight = bd.Height;
    XrLog("[AKXR] CaptureGameDevice(%s): device=%p context=%p backbuffer %ux%u fmt=%lld.",
          reason != nullptr ? reason : "?", (void*)g_device, (void*)g_context,
          g_bbWidth, g_bbHeight, g_colorFormat);
    return true;
}

// First-call init: capture the game's device/context/backbuffer format+size from the swapchain,
// then launch the one-time bring-up thread. Mirrors the ME mod (creation off-thread, per-frame
// copies on the Present thread so the D3D11 immediate context stays single-threaded).
void TryInit(IDXGISwapChain* gameSwapChain) noexcept
{
    bool expected = false;
    if (!g_bringupStarted.compare_exchange_strong(expected, true)) return;  // only once

    if (g_selfModule == nullptr)
    {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&XrBringupThread), &g_selfModule);
    }

    if (gameSwapChain == nullptr) { g_bringupStarted.store(false); return; }

    ID3D11Device* device = nullptr;
    if (FAILED(gameSwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || device == nullptr)
    {
        XrLog("[AKXR] TryInit: GetDevice failed.");
        g_bringupStarted.store(false);
        return;
    }
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    ID3D11Texture2D* bb = nullptr;
    D3D11_TEXTURE2D_DESC bd = {};
    if (SUCCEEDED(gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) && bb != nullptr)
    {
        bb->GetDesc(&bd);
        bb->Release();
    }
    else
    {
        XrLog("[AKXR] TryInit: GetBuffer(0) failed.");
        if (context) context->Release();
        device->Release();
        g_bringupStarted.store(false);
        return;
    }

    g_device = device;          // hold the references for the session lifetime
    g_context = context;
    g_colorFormat = static_cast<int64_t>(bd.Format);
    g_bbWidth = bd.Width;
    g_bbHeight = bd.Height;
    XrLog("[AKXR] TryInit: captured game device=%p context=%p backbuffer %ux%u fmt=%lld -> bringing up OpenXR live DIBR.",
          (void*)g_device, (void*)g_context, g_bbWidth, g_bbHeight, g_colorFormat);

    HANDLE th = CreateThread(nullptr, 0, &XrBringupThread, nullptr, 0, nullptr);
    if (th != nullptr) CloseHandle(th);
}
}  // namespace

namespace AKXr
{
void Frame(IDXGISwapChain* gameSwapChain) noexcept
{
    if (!IsGameProcess()) return;  // hard no-op in the Unity launcher (deadlocks with its UUVR/OpenXR)
    g_gameSwapChain = gameSwapChain;

    // Flat depth-map proof runs without OpenXR/headset. It only needs the game's D3D11 device/context.
    CaptureGameDevice(gameSwapChain, "flat-depth");
    PollDibrDebugHotkey();                     // F8 = depth-map view — the ONLY kept hotkey (besides Insert).
    RunFlatDepthDebugOverlay(gameSwapChain);   // renders the F8 depth map on the flat output
    // (Removed: F-key DIBR gain/scale/edge tuners, F9 desktop depth dump, F10 proof dump — all menu-driven now.)

    if (!g_xrEnabled.load()) return;

    TryInit(gameSwapChain);
    if (!g_ready.load()) return;
    __try
    {
        PollEvents();
        if (g_sessionRunning) RunFrame(gameSwapChain);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Never let an XR hiccup take down the game's Present.
    }
}

bool GetHeadYawPitchDeg(float* yawDeg, float* pitchDeg, float* rollDeg) noexcept
{
    if (!g_headValid.load()) return false;
    if (yawDeg) *yawDeg = g_headYaw.load();
    if (pitchDeg) *pitchDeg = g_headPitch.load();
    if (rollDeg) *rollDeg = g_headRoll.load();
    return true;
}
// Menu readout only: never frozen, so the panel still shows a moving HMD while the camera is held.
bool GetLiveHeadYawPitchDeg(float* yawDeg, float* pitchDeg, float* rollDeg) noexcept
{
    if (!g_headValid.load()) return false;
    if (yawDeg) *yawDeg = g_headYawLive.load();
    if (pitchDeg) *pitchDeg = g_headPitchLive.load();
    if (rollDeg) *rollDeg = g_headRollLive.load();
    return true;
}
void SetRenderedHeadYawPitchDeg(float yawDeg, float pitchDeg) noexcept
{
    constexpr float kDegToRad = 0.01745329251994329577f;
    // HeadEulerDegrees reports the opposite sign of the OpenXR Y-axis quaternion yaw.
    const float yaw = -yawDeg * kDegToRad;
    const float pitch = pitchDeg * kDegToRad;
    const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
    const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    g_lastHeadOrient.w = cy * cp;
    g_lastHeadOrient.x = cy * sp;
    g_lastHeadOrient.y = sy * cp;
    g_lastHeadOrient.z = -sy * sp;
}

void SetHeadLookSubmitPose(bool on) noexcept { g_submitHeadPose.store(on); }

void SetGameProjection(float m00, float m11) noexcept { g_projM00.store(m00); g_projM11.store(m11); }
bool GetHeadsetFovHalf(float* h, float* v) noexcept
{
    const float hh = g_hsHalfH.load(), vv = g_hsHalfV.load();
    if (hh <= 0.0f || vv <= 0.0f) return false;
    if (h) *h = hh; if (v) *v = vv;
    return true;
}

void SetStereo(bool on) noexcept { g_stereoOn.store(on); }
int GetActiveEye() noexcept { return g_activeEye.load() & 1; }
void SetAutoDibrFp(bool on) noexcept { g_dibrAutoFp.store(on); }   // force DIBR for first-person even in AER mode
void Recenter() noexcept { g_recenterReq.store(true); }            // re-anchor "forward" to the current head (next frame)

namespace
{
float ClampMenuFloat(float value, float lo, float hi, float fallback) noexcept
{
    if (!std::isfinite(value)) return fallback;
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}
}

void GetDibrTuning(DibrTuning* out) noexcept
{
    if (out == nullptr) return;
    out->xrEnabled = g_xrEnabled.load();
    out->stereoOn = g_stereoOn.load();
    out->dibrEnabled = g_dibrEnabled.load();
    out->sameFrameStereo = g_sameFrameStereoMenu;
    out->depthDebug = g_dibrDepthDebug.load();
    out->submitHeadPose = g_submitHeadPose.load();
    out->gain = g_dibrGain;
    out->depthScale = g_dibrDepthScale;
    out->edgeGuard = g_dibrEdgeGuard;
    out->edgeThreshold = g_dibrEdgeThreshold;
    out->convergence = g_convergence.load();
    out->aerHalfEye = g_aerHalfEye;
    out->aerSwapEyes = g_aerSwapEyes;
    out->aerDisplayHz = g_aerDisplayHz.load();
    out->aerFpScale = g_aerFpScale;
    out->sixDofGameplay = g_sixDofGameplay;
    out->sixDofFirstPerson = g_sixDofFirstPerson;
    out->headYawGain = g_headYawGain;
    out->headPitchGain = g_headPitchGain;
    out->headSmoothing = g_headSmoothing;
    out->invertHeadYaw = g_invertHeadYaw;
    out->invertHeadPitch = g_invertHeadPitch;
    out->recenterKey = g_recenterKey;
    out->headAimEnabled = g_headAimEnabledMenu;
    out->headAimGain = g_headAimGainMenu;
    out->fovFillEnabled = g_fovFillEnabled;
    out->fovFillH = g_fovFillH;
    out->fovFillV = g_fovFillV;
}

void SetDibrTuning(const DibrTuning& tuning) noexcept
{
    g_xrEnabled.store(tuning.xrEnabled);
    g_stereoOn.store(tuning.stereoOn);
    g_dibrEnabled.store(tuning.dibrEnabled);
    g_sameFrameStereoMenu = tuning.sameFrameStereo;
    g_dibrDepthDebug.store(tuning.depthDebug);
    g_submitHeadPose.store(tuning.submitHeadPose);
    g_dibrGain = ClampMenuFloat(tuning.gain, 0.0f, 0.120f, 0.035f);
    g_dibrDepthScale = ClampMenuFloat(tuning.depthScale, 16.0f, 192.0f, 96.0f);
    g_dibrEdgeGuard = ClampMenuFloat(tuning.edgeGuard, 0.0f, 8.0f, 3.0f);
    g_dibrEdgeThreshold = ClampMenuFloat(tuning.edgeThreshold, 0.010f, 0.300f, 0.080f);
    g_convergence.store(ClampMenuFloat(tuning.convergence, -0.080f, 0.080f, 0.0f));
    g_aerHalfEye = ClampMenuFloat(tuning.aerHalfEye, 0.0f, 20.0f, 1.6f);
    g_aerSwapEyes = tuning.aerSwapEyes;
    g_aerDisplayHz.store(tuning.aerDisplayHz);
    g_aerFpScale = ClampMenuFloat(tuning.aerFpScale, 0.0f, 1.5f, 0.10f);
    g_fovFillEnabled = tuning.fovFillEnabled;
    g_fovFillH = ClampMenuFloat(tuning.fovFillH, 0.50f, 1.25f, 1.0f);
    g_fovFillV = ClampMenuFloat(tuning.fovFillV, 0.50f, 1.25f, 1.0f);
    g_sixDofGameplay = tuning.sixDofGameplay;
    g_sixDofFirstPerson = tuning.sixDofFirstPerson;
    g_headYawGain = ClampMenuFloat(tuning.headYawGain, 0.25f, 2.0f, 1.0f);
    g_headPitchGain = ClampMenuFloat(tuning.headPitchGain, 0.25f, 2.0f, 1.0f);
    g_headSmoothing = ClampMenuFloat(tuning.headSmoothing, 0.0f, 0.90f, 0.0f);
    g_invertHeadYaw = tuning.invertHeadYaw;
    g_invertHeadPitch = tuning.invertHeadPitch;
    g_headAimEnabledMenu = tuning.headAimEnabled;
    g_headAimGainMenu = ClampMenuFloat(tuning.headAimGain, 0.2f, 5.0f, 1.5f);
    if (tuning.recenterKey > 0 && tuning.recenterKey < 256) g_recenterKey = tuning.recenterKey;
}

void ResetDibrTuning() noexcept
{
    DibrTuning tuning = {};
    SetDibrTuning(tuning);
}

// Fill-source push (from dxgi's Present hook): the game's scene color RT. AER/DIBR copy this instead of the
// letterboxed backbuffer so they fill the square eye. Plain pointer store; FillSource null/size-guards it.
void SetFillSceneRT(ID3D11Texture2D* rt) noexcept { g_fillSceneRT = rt; }

// SF2: capture the off-screen scene color RT into this eye's history (same-frame). Confirmed the scene RT
// is fmt 87 = same family as the backbuffer-format history, so a plain CopyResource works (no shader).
void CaptureTextureToHistory(int eye, ID3D11Texture2D* src) noexcept
{
    if (eye < 0 || eye > 1 || src == nullptr || g_device == nullptr || g_context == nullptr) return;
    if (!EnsureHistoryTextures()) return;
    g_context->CopyResource(g_history[eye], src);
    g_historyOrient[eye] = g_lastHeadOrient;   // both eyes captured THIS frame = same head pose
    g_historyHasOrient[eye] = true;
    static uint32_t s_log = 0;
    if ((s_log++ % 240u) == 0u) XrLog("[AKSF] eye%d scene RT -> history (same-frame stereo)", eye);
}
void SameFrameSubmitStats(unsigned long long* pairs, unsigned long long* monos) noexcept
{
    if (pairs != nullptr) *pairs = g_sfPairSubmits.load(std::memory_order_relaxed);
    if (monos != nullptr) *monos = g_sfMonoSubmits.load(std::memory_order_relaxed);
}
void SetConvergence(float radians) noexcept { g_convergence.store(radians); }
void SetActiveEye(int eye) noexcept { g_activeEye.store(eye & 1); }
void SetSameFrameActive(bool on) noexcept { g_sameFrameActive.store(on); }
void SetSameFramePairReady(bool ready) noexcept { g_sameFramePairReady.store(ready, std::memory_order_release); }

// Which same-frame flavour: true = the viewport split (both eyes in one SBS backbuffer, cropped
// per eye); false = the full-frame double render (each eye captured whole into its own history).
void SetSbsFromBackbuffer(bool on) noexcept { g_sbsFromBackbuffer.store(on); }

bool IsDesktopDepthScanArmed() noexcept
{
    return g_desktopDepthDumpArmed.load() && !g_desktopDepthDumpWritten &&
           g_depthScanCandidateCount < kDepthScanMaxCandidates;
}

void CaptureDepthCandidateBeforeClear(ID3D11DeviceContext* context, ID3D11DepthStencilView* dsv) noexcept
{
    if (!IsDesktopDepthScanArmed() || context == nullptr || dsv == nullptr || !IsGameProcess()) return;

    ID3D11Resource* resource = nullptr;
    dsv->GetResource(&resource);
    if (resource == nullptr) return;

    ID3D11Texture2D* depthTex = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&depthTex))) || depthTex == nullptr)
    {
        resource->Release();
        return;
    }

    D3D11_TEXTURE2D_DESC src = {};
    depthTex->GetDesc(&src);
    const unsigned ordinal = ++g_depthScanClearCount;
    if (src.Width < 640 || src.Height < 360 || src.SampleDesc.Count == 0)
    {
        depthTex->Release();
        resource->Release();
        return;
    }

    ID3D11Device* dev = g_device;
    bool releaseDev = false;
    if (dev == nullptr)
    {
        context->GetDevice(&dev);
        releaseDev = true;
    }
    if (dev == nullptr)
    {
        depthTex->Release();
        resource->Release();
        return;
    }

    DepthScanCandidate& out = g_depthScanCandidates[g_depthScanCandidateCount];
    D3D11_TEXTURE2D_DESC copy = src;
    copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy.Usage = D3D11_USAGE_DEFAULT;
    copy.CPUAccessFlags = 0;
    copy.MiscFlags = 0;
    copy.MipLevels = 1;
    copy.ArraySize = 1;

    ID3D11Texture2D* copyTex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = dev->CreateTexture2D(&copy, nullptr, &copyTex);
    if (SUCCEEDED(hr) && copyTex != nullptr)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DibrDepthSrvFormat(src.Format);
        if (src.SampleDesc.Count > 1)
        {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        }
        else
        {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
        }
        hr = dev->CreateShaderResourceView(copyTex, &sd, &srv);
        if (SUCCEEDED(hr) && srv != nullptr)
        {
            context->CopyResource(copyTex, depthTex);
            out.tex = copyTex;
            out.srv = srv;
            out.width = src.Width;
            out.height = src.Height;
            out.samples = src.SampleDesc.Count;
            out.format = src.Format;
            out.textureFormat = src.Format;
            out.srvFormat = sd.Format;
            out.dsvPtr = reinterpret_cast<uintptr_t>(dsv);
            out.clearOrdinal = ordinal;
            out.drawOrdinal = 0;
            snprintf(out.source, sizeof(out.source), "clear");
            ++g_depthScanCandidateCount;
            XrLog("[AKDIBR] scan captured candidate_%02u clear=%u dsv=%p depth=%ux%u fmt=%u samples=%u",
                  g_depthScanCandidateCount - 1, ordinal, static_cast<void*>(dsv),
                  src.Width, src.Height, static_cast<unsigned>(src.Format), src.SampleDesc.Count);
            copyTex = nullptr;
            srv = nullptr;
        }
    }

    SafeRelease(srv);
    SafeRelease(copyTex);
    if (releaseDev && dev != nullptr) dev->Release();
    depthTex->Release();
    resource->Release();
}

void CaptureDepthCandidateAfterDraw(ID3D11DeviceContext* context) noexcept
{
    if (!IsDesktopDepthScanArmed() || context == nullptr || !IsGameProcess()) return;

    const unsigned drawOrdinal = ++g_depthScanDrawCount;
    const unsigned thresholds[kDepthScanMaxCandidates] = {1u, 4u, 16u, 64u, 256u, 512u, 1024u, 2048u};
    bool wanted = false;
    for (unsigned threshold : thresholds)
    {
        if (drawOrdinal == threshold)
        {
            wanted = true;
            break;
        }
    }
    if (!wanted) return;

    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(0, nullptr, &dsv);
    if (dsv == nullptr) return;

    ID3D11Resource* resource = nullptr;
    dsv->GetResource(&resource);
    if (resource == nullptr)
    {
        dsv->Release();
        return;
    }

    ID3D11Texture2D* depthTex = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&depthTex))) || depthTex == nullptr)
    {
        resource->Release();
        dsv->Release();
        return;
    }

    D3D11_TEXTURE2D_DESC src = {};
    depthTex->GetDesc(&src);
    if (src.Width < 640 || src.Height < 360 || src.SampleDesc.Count == 0)
    {
        depthTex->Release();
        resource->Release();
        dsv->Release();
        return;
    }

    ID3D11Device* dev = g_device;
    bool releaseDev = false;
    if (dev == nullptr)
    {
        context->GetDevice(&dev);
        releaseDev = true;
    }
    if (dev == nullptr)
    {
        depthTex->Release();
        resource->Release();
        dsv->Release();
        return;
    }

    DepthScanCandidate& out = g_depthScanCandidates[g_depthScanCandidateCount];
    D3D11_TEXTURE2D_DESC copy = src;
    copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy.Usage = D3D11_USAGE_DEFAULT;
    copy.CPUAccessFlags = 0;
    copy.MiscFlags = 0;
    copy.MipLevels = 1;
    copy.ArraySize = 1;

    ID3D11Texture2D* copyTex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = dev->CreateTexture2D(&copy, nullptr, &copyTex);
    if (SUCCEEDED(hr) && copyTex != nullptr)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DibrDepthSrvFormat(src.Format);
        if (src.SampleDesc.Count > 1)
        {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        }
        else
        {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
        }
        hr = dev->CreateShaderResourceView(copyTex, &sd, &srv);
        if (SUCCEEDED(hr) && srv != nullptr)
        {
            context->CopyResource(copyTex, depthTex);
            out.tex = copyTex;
            out.srv = srv;
            out.width = src.Width;
            out.height = src.Height;
            out.samples = src.SampleDesc.Count;
            out.format = src.Format;
            out.textureFormat = src.Format;
            out.srvFormat = sd.Format;
            out.dsvPtr = reinterpret_cast<uintptr_t>(dsv);
            out.clearOrdinal = 0;
            out.drawOrdinal = drawOrdinal;
            snprintf(out.source, sizeof(out.source), "draw");
            ++g_depthScanCandidateCount;
            XrLog("[AKDIBR] scan captured candidate_%02u draw=%u dsv=%p depth=%ux%u fmt=%u samples=%u",
                  g_depthScanCandidateCount - 1, drawOrdinal, static_cast<void*>(dsv),
                  src.Width, src.Height, static_cast<unsigned>(src.Format), src.SampleDesc.Count);
            copyTex = nullptr;
            srv = nullptr;
        }
    }

    SafeRelease(srv);
    SafeRelease(copyTex);
    if (releaseDev && dev != nullptr) dev->Release();
    depthTex->Release();
    resource->Release();
    dsv->Release();
}

void CaptureShaderResourceCandidate(ID3D11DeviceContext* context, const char* stage, UINT startSlot, UINT slot, ID3D11ShaderResourceView* srv) noexcept
{
    if (!IsDesktopDepthScanArmed() || context == nullptr || srv == nullptr || !IsGameProcess()) return;

    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    srv->GetDesc(&sv);
    ++g_depthScanSrvSeen;
    if (sv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D &&
        sv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DMS)
    {
        ++g_depthScanSrvRejectNonDepth;
        return;
    }
    if (sv.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && sv.Texture2D.MostDetailedMip != 0)
    {
        ++g_depthScanSrvRejectNonDepth;
        return;
    }

    ID3D11Resource* resource = nullptr;
    srv->GetResource(&resource);
    if (resource == nullptr) return;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || tex == nullptr)
    {
        resource->Release();
        return;
    }

    D3D11_TEXTURE2D_DESC src = {};
    tex->GetDesc(&src);
    if (src.Width < 640 || src.Height < 360 || src.SampleDesc.Count == 0)
    {
        ++g_depthScanSrvRejectSmall;
        tex->Release();
        resource->Release();
        return;
    }

    const DXGI_FORMAT srvFormat = sv.Format != DXGI_FORMAT_UNKNOWN ? sv.Format : src.Format;
    if (!IsDepthLikeSrvCandidate(src.Format, srvFormat))
    {
        if (IsKnownColorTextureFormat(src.Format) || IsKnownColorTextureFormat(srvFormat))
        {
            ++g_depthScanSrvRejectColor;
        }
        else
        {
            ++g_depthScanSrvRejectNonDepth;
        }
        tex->Release();
        resource->Release();
        return;
    }
    ++g_depthScanSrvDepthLikeSeen;

    for (unsigned i = 0; i < g_depthScanCandidateCount; ++i)
    {
        if (g_depthScanCandidates[i].dsvPtr == reinterpret_cast<uintptr_t>(srv))
        {
            ++g_depthScanSrvRejectDuplicate;
            tex->Release();
            resource->Release();
            return;
        }
    }

    ID3D11Device* dev = g_device;
    bool releaseDev = false;
    if (dev == nullptr)
    {
        context->GetDevice(&dev);
        releaseDev = true;
    }
    if (dev == nullptr)
    {
        tex->Release();
        resource->Release();
        return;
    }

    DepthScanCandidate& out = g_depthScanCandidates[g_depthScanCandidateCount];
    D3D11_TEXTURE2D_DESC copy = src;
    copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy.Usage = D3D11_USAGE_DEFAULT;
    copy.CPUAccessFlags = 0;
    copy.MiscFlags = 0;
    copy.MipLevels = 1;
    copy.ArraySize = 1;

    ID3D11Texture2D* copyTex = nullptr;
    ID3D11ShaderResourceView* copySrv = nullptr;
    HRESULT hr = dev->CreateTexture2D(&copy, nullptr, &copyTex);
    if (SUCCEEDED(hr) && copyTex != nullptr)
    {
        context->CopySubresourceRegion(copyTex, 0, 0, 0, 0, tex, 0, nullptr);

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = srvFormat;
        if (src.SampleDesc.Count > 1 || sv.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS)
        {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        }
        else
        {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
        }
        hr = dev->CreateShaderResourceView(copyTex, &sd, &copySrv);
        if (SUCCEEDED(hr) && copySrv != nullptr)
        {
            out.tex = copyTex;
            out.srv = copySrv;
            out.width = src.Width;
            out.height = src.Height;
            out.samples = src.SampleDesc.Count;
            out.format = sd.Format;
            out.textureFormat = src.Format;
            out.srvFormat = sd.Format;
            out.dsvPtr = reinterpret_cast<uintptr_t>(srv);
            out.clearOrdinal = 0;
            out.drawOrdinal = g_depthScanDrawCount;
            snprintf(out.source, sizeof(out.source), "%sSRV%u", stage != nullptr ? stage : "?", startSlot + slot);
            ++g_depthScanCandidateCount;
            XrLog("[AKDIBR] scan captured candidate_%02u %s slot=%u srv=%p tex=%p size=%ux%u texFmt=%u srvFmt=%u samples=%u draw=%u",
                  g_depthScanCandidateCount - 1,
                  stage != nullptr ? stage : "?",
                  startSlot + slot,
                  static_cast<void*>(srv),
                  static_cast<void*>(tex),
                  src.Width, src.Height,
                  static_cast<unsigned>(src.Format),
                  static_cast<unsigned>(sd.Format),
                  src.SampleDesc.Count,
                  g_depthScanDrawCount);
            copyTex = nullptr;
            copySrv = nullptr;
        }
    }

    SafeRelease(copySrv);
    SafeRelease(copyTex);
    if (releaseDev && dev != nullptr) dev->Release();
    tex->Release();
    resource->Release();
}

void TrackLiveShaderResource(ID3D11DeviceContext* context, const char* stage, UINT startSlot, UINT slot, ID3D11ShaderResourceView* srv) noexcept
{
    if (context == nullptr || srv == nullptr || !IsGameProcess() || !g_dibrEnabled.load()) return;

    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    srv->GetDesc(&sv);
    if (sv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D &&
        sv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DMS)
    {
        return;
    }
    if (sv.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && sv.Texture2D.MostDetailedMip != 0)
    {
        return;
    }

    ID3D11Resource* resource = nullptr;
    srv->GetResource(&resource);
    if (resource == nullptr) return;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || tex == nullptr)
    {
        resource->Release();
        return;
    }

    D3D11_TEXTURE2D_DESC src = {};
    tex->GetDesc(&src);
    const DXGI_FORMAT srvFormat = sv.Format != DXGI_FORMAT_UNKNOWN ? sv.Format : src.Format;
    const bool usable = src.Width >= 1920 && src.Height >= 1080 &&
                        src.SampleDesc.Count > 0 &&
                        IsDepthLikeSrvCandidate(src.Format, srvFormat);
    if (!usable)
    {
        tex->Release();
        resource->Release();
        return;
    }

    const bool sameSrv = g_dibrDepthSrv == srv && g_dibrDepthFromLiveSrv.load();
    if (!sameSrv)
    {
        ID3D11ShaderResourceView* oldSrv = g_dibrDepthSrv;
        ID3D11Texture2D* oldCopy = g_dibrDepthCopy;
        srv->AddRef();
        g_dibrDepthSrv = srv;
        g_dibrDepthCopy = nullptr;
        SafeRelease(oldSrv);
        SafeRelease(oldCopy);

        const bool sampleCountChanged = g_dibrDepthSamples != src.SampleDesc.Count;
        g_dibrDepthW = src.Width;
        g_dibrDepthH = src.Height;
        g_dibrDepthSamples = src.SampleDesc.Count;
        g_dibrDepthReady.store(true);
        g_dibrDepthFromLiveSrv.store(true);
        if (sampleCountChanged)
        {
            g_dibrShadersReady = false;
            g_dibrShadersTried = false;
        }

        static unsigned s_liveDepthLogs = 0;
        if (s_liveDepthLogs < 20 || (s_liveDepthLogs % 300) == 0)
        {
            XrLog("[AKDIBR] live depth SRV tracked %s slot=%u srv=%p tex=%p size=%ux%u texFmt=%u srvFmt=%u samples=%u",
                  stage != nullptr ? stage : "?",
                  startSlot + slot,
                  static_cast<void*>(srv),
                  static_cast<void*>(tex),
                  src.Width, src.Height,
                  static_cast<unsigned>(src.Format),
                  static_cast<unsigned>(srvFormat),
                  src.SampleDesc.Count);
        }
        ++s_liveDepthLogs;
    }
    else
    {
        g_dibrDepthReady.store(true);
    }

    tex->Release();
    resource->Release();
}

// Camera/room change (from GM_ChangeCamera): the game rebuilds the room's depth buffer; the latched depth would
// keep pointing at the old one (white depth map -> VR drops). SOFT re-acquire: drop ONLY the live-SRV lock so
// TrackLiveShaderResource re-tracks the new room's depth and the DSV-capture fallback re-enables — but KEEP
// g_dibrDepthReady=true so DIBR keeps running on the current depth and swaps seamlessly when the new one is found.
// (Blanking ready dropped to AER for a frame, and GM_ChangeCamera fires often in normal play = flicker. This doesn't.)
void OnCameraChanged() noexcept
{
    if (!g_dibrEnabled.load()) return;
    g_dibrDepthFromLiveSrv.store(false);
    static unsigned s_n = 0;
    if (s_n < 20 || (s_n % 120) == 0)
        XrLog("[AKDIBR] camera change -> soft depth re-acquire (DIBR kept running, no AER fallback)");
    ++s_n;
}

void CaptureDepthBeforeClear(ID3D11DeviceContext* context, ID3D11DepthStencilView* dsv) noexcept
{
    if (!g_dibrEnabled.load() || context == nullptr || dsv == nullptr || !IsGameProcess()) return;
    if (g_dibrDepthFromLiveSrv.load() && g_dibrDepthReady.load() && g_dibrDepthSrv != nullptr) return;

    ID3D11Resource* resource = nullptr;
    dsv->GetResource(&resource);
    if (resource == nullptr) return;

    ID3D11Texture2D* depthTex = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&depthTex))) || depthTex == nullptr)
    {
        resource->Release();
        return;
    }

    D3D11_TEXTURE2D_DESC src = {};
    depthTex->GetDesc(&src);
    if (g_dibrDepthMetaOnly.load())
    {
        if (!g_dibrDepthMetaWritten)
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
            dsv->GetDesc(&dsvDesc);
            WriteDepthMetaReport(src, dsvDesc);
            g_dibrDepthMetaWritten = true;
            XrLog("[AKDIBR] depth metadata written %ux%u fmt=%u samples=%u; DIBR remains disabled.",
                  src.Width, src.Height, static_cast<unsigned>(src.Format), src.SampleDesc.Count);
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        depthTex->Release();
        resource->Release();
        return;
    }
    if (g_dibrSetupDepthCopyOnly.load())
    {
        if (!g_dibrSetupDepthCopyWritten)
        {
            ID3D11Device* dev = g_device;
            bool releaseDev = false;
            if (dev == nullptr)
            {
                context->GetDevice(&dev);
                releaseDev = true;
            }

            g_dibrDepthW = src.Width;
            g_dibrDepthH = src.Height;
            g_dibrDepthSamples = src.SampleDesc.Count;
            g_dibrShadersReady = false;
            g_dibrShadersTried = false;
            const bool setupOk = EnsureDibrShaders();

            ID3D11Texture2D* depthCopy = nullptr;
            ID3D11ShaderResourceView* depthSrv = nullptr;
            HRESULT createHr = E_FAIL;
            HRESULT srvHr = E_FAIL;
            HRESULT copyHr = E_FAIL;
            const DXGI_FORMAT srvFmt = DibrDepthSrvFormat(src.Format);
            if (dev != nullptr)
            {
                D3D11_TEXTURE2D_DESC copy = src;
                copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                copy.Usage = D3D11_USAGE_DEFAULT;
                copy.CPUAccessFlags = 0;
                copy.MiscFlags = 0;
                copy.MipLevels = 1;
                copy.ArraySize = 1;
                createHr = dev->CreateTexture2D(&copy, nullptr, &depthCopy);
                if (SUCCEEDED(createHr) && depthCopy != nullptr)
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
                    sd.Format = srvFmt;
                    if (src.SampleDesc.Count > 1)
                    {
                        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
                    }
                    else
                    {
                        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        sd.Texture2D.MipLevels = 1;
                    }
                    srvHr = dev->CreateShaderResourceView(depthCopy, &sd, &depthSrv);
                    if (SUCCEEDED(srvHr) && depthSrv != nullptr)
                    {
                        context->CopyResource(depthCopy, depthTex);
                        copyHr = S_OK;
                    }
                }
            }

            WriteDibrSetupDepthCopyReport(src, setupOk, createHr, srvHr, copyHr, srvFmt, depthCopy, depthSrv);
            XrLog("[AKDIBR] setup+depth-copy result setup=%d create=0x%08X srv=0x%08X copy=0x%08X depth=%ux%u fmt=%u samples=%u",
                  setupOk ? 1 : 0, static_cast<unsigned>(createHr), static_cast<unsigned>(srvHr),
                  static_cast<unsigned>(copyHr), src.Width, src.Height, static_cast<unsigned>(src.Format),
                  src.SampleDesc.Count);
            SafeRelease(depthSrv);
            SafeRelease(depthCopy);
            ReleaseDibrSetupObjects();
            if (releaseDev && dev != nullptr) dev->Release();
            g_dibrSetupDepthCopyWritten = true;
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        depthTex->Release();
        resource->Release();
        return;
    }
    if (g_dibrSetupOnly.load())
    {
        if (!g_dibrSetupWritten)
        {
            g_dibrDepthW = src.Width;
            g_dibrDepthH = src.Height;
            g_dibrDepthSamples = src.SampleDesc.Count;
            g_dibrShadersReady = false;
            g_dibrShadersTried = false;
            const bool ok = EnsureDibrShaders();
            WriteDibrSetupReport(src, ok);
            XrLog("[AKDIBR] persistent setup-only result ok=%d depth=%ux%u fmt=%u samples=%u",
                  ok ? 1 : 0, src.Width, src.Height, static_cast<unsigned>(src.Format), src.SampleDesc.Count);
            g_dibrSetupWritten = true;
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        depthTex->Release();
        resource->Release();
        return;
    }
    if (g_dibrStateCreateOnly.load())
    {
        if (!g_dibrStateCreateWritten)
        {
            ID3D11Device* dev = g_device;
            bool releaseDev = false;
            if (dev == nullptr)
            {
                context->GetDevice(&dev);
                releaseDev = true;
            }

            HRESULT samplerHr = E_FAIL;
            HRESULT rasterHr = E_FAIL;
            HRESULT depthHr = E_FAIL;
            HRESULT blendHr = E_FAIL;
            HRESULT cbHr = E_FAIL;
            const bool ok = CreateDibrStateObjects(dev, samplerHr, rasterHr, depthHr, blendHr, cbHr);
            WriteDibrStateCreateReport(src, samplerHr, rasterHr, depthHr, blendHr, cbHr);
            XrLog("[AKDIBR] state-create-only result ok=%d sampler=0x%08X raster=0x%08X depth=0x%08X blend=0x%08X cb=0x%08X",
                  ok ? 1 : 0, static_cast<unsigned>(samplerHr), static_cast<unsigned>(rasterHr),
                  static_cast<unsigned>(depthHr), static_cast<unsigned>(blendHr), static_cast<unsigned>(cbHr));
            SafeRelease(g_dibrSampler);
            SafeRelease(g_dibrRaster);
            SafeRelease(g_dibrDepthState);
            SafeRelease(g_dibrBlend);
            SafeRelease(g_dibrParamsCb);
            if (releaseDev && dev != nullptr) dev->Release();
            g_dibrStateCreateWritten = true;
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        depthTex->Release();
        resource->Release();
        return;
    }
    if (g_dibrShaderCreateOnly.load())
    {
        if (!g_dibrShaderCreateWritten)
        {
            ID3D11Device* dev = g_device;
            bool releaseDev = false;
            if (dev == nullptr)
            {
                context->GetDevice(&dev);
                releaseDev = true;
            }

            HRESULT vsHr = E_FAIL;
            HRESULT psHr = E_FAIL;
            const bool ok = CreateDibrShaderObjectsFromBytecode(dev, src.SampleDesc.Count, vsHr, psHr);
            WriteDibrShaderCreateReport(src, vsHr, psHr);
            XrLog("[AKDIBR] shader-create-only result ok=%d vs=0x%08X ps=0x%08X depth=%ux%u fmt=%u samples=%u",
                  ok ? 1 : 0, static_cast<unsigned>(vsHr), static_cast<unsigned>(psHr),
                  src.Width, src.Height, static_cast<unsigned>(src.Format), src.SampleDesc.Count);
            SafeRelease(g_dibrVs);
            SafeRelease(g_dibrPs);
            if (releaseDev && dev != nullptr) dev->Release();
            g_dibrShaderCreateWritten = true;
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        depthTex->Release();
        resource->Release();
        return;
    }
    if (g_dibrCompilerLoadOnly.load())
    {
        if (!g_dibrCompilerLoadWritten)
        {
            const wchar_t* dllName = L"d3dcompiler_47.dll";
            HMODULE compiler = LoadLibraryW(dllName);
            if (compiler == nullptr)
            {
                dllName = L"d3dcompiler_43.dll";
                compiler = LoadLibraryW(dllName);
            }
            void* compileProc = compiler != nullptr ? reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
            WriteCompilerLoadReport(src, compiler, compileProc, dllName);
            XrLog("[AKDIBR] compiler-load-only result compiler=%p proc=%p dll=%ls depth=%ux%u fmt=%u samples=%u",
                  static_cast<void*>(compiler), compileProc, dllName,
                  src.Width, src.Height, static_cast<unsigned>(src.Format), src.SampleDesc.Count);
            g_dibrCompilerLoadWritten = true;
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        depthTex->Release();
        resource->Release();
        return;
    }
    if (g_dibrShaderCompileOnly.load())
    {
        if (!g_dibrShaderCompileWritten)
        {
            bool vsOk = false;
            bool psOk = false;
            const bool ok = CompileDibrShadersProbe(src.SampleDesc.Count, vsOk, psOk);
            WriteDibrShaderCompileReport(src, vsOk, psOk);
            XrLog("[AKDIBR] shader compile-only result ok=%d vs=%d ps=%d depth=%ux%u fmt=%u samples=%u",
                  ok ? 1 : 0, vsOk ? 1 : 0, psOk ? 1 : 0,
                  src.Width, src.Height, static_cast<unsigned>(src.Format), src.SampleDesc.Count);
            g_dibrShaderCompileWritten = true;
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        depthTex->Release();
        resource->Release();
        return;
    }
    if (g_dibrShaderOnly.load())
    {
        if (!g_dibrShaderWritten)
        {
            g_dibrDepthW = src.Width;
            g_dibrDepthH = src.Height;
            g_dibrDepthSamples = src.SampleDesc.Count;
            g_dibrShadersReady = false;
            g_dibrShadersTried = false;
            const bool ok = EnsureDibrShaders();
            WriteDibrShaderReport(src, ok);
            XrLog("[AKDIBR] shader-only result ok=%d depth=%ux%u fmt=%u samples=%u",
                  ok ? 1 : 0, src.Width, src.Height, static_cast<unsigned>(src.Format), src.SampleDesc.Count);
            g_dibrShaderWritten = true;
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        depthTex->Release();
        resource->Release();
        return;
    }
    if (g_dibrDepthSrvCopyOnly.load())
    {
        if (!g_dibrDepthSrvCopyWritten)
        {
            ID3D11Device* dev = g_device;
            bool releaseDev = false;
            if (dev == nullptr)
            {
                context->GetDevice(&dev);
                releaseDev = true;
            }

            HRESULT createHr = E_FAIL;
            HRESULT srvHr = E_FAIL;
            HRESULT copyHr = E_FAIL;
            ID3D11Texture2D* copyTex = nullptr;
            ID3D11ShaderResourceView* srv = nullptr;
            DXGI_FORMAT srvFormat = DibrDepthSrvFormat(src.Format);
            if (dev != nullptr)
            {
                D3D11_TEXTURE2D_DESC copy = src;
                copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                copy.Usage = D3D11_USAGE_DEFAULT;
                copy.CPUAccessFlags = 0;
                copy.MiscFlags = 0;
                copy.MipLevels = 1;
                copy.ArraySize = 1;
                createHr = dev->CreateTexture2D(&copy, nullptr, &copyTex);
                if (SUCCEEDED(createHr) && copyTex != nullptr)
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
                    sd.Format = srvFormat;
                    if (src.SampleDesc.Count > 1)
                    {
                        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
                    }
                    else
                    {
                        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        sd.Texture2D.MipLevels = 1;
                    }
                    srvHr = dev->CreateShaderResourceView(copyTex, &sd, &srv);
                    if (SUCCEEDED(srvHr) && srv != nullptr)
                    {
                        context->CopyResource(copyTex, depthTex);
                        copyHr = S_OK;
                    }
                }
            }
            WriteDepthSrvCopyReport(src, createHr, srvHr, copyHr, srvFormat);
            XrLog("[AKDIBR] depth SRV-copy result create=0x%08X srv=0x%08X copy=0x%08X %ux%u fmt=%u srvFmt=%u samples=%u",
                  static_cast<unsigned>(createHr), static_cast<unsigned>(srvHr), static_cast<unsigned>(copyHr),
                  src.Width, src.Height, static_cast<unsigned>(src.Format), static_cast<unsigned>(srvFormat), src.SampleDesc.Count);
            SafeRelease(srv);
            SafeRelease(copyTex);
            if (releaseDev && dev != nullptr) dev->Release();
            g_dibrDepthSrvCopyWritten = true;
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        depthTex->Release();
        resource->Release();
        return;
    }
    if (g_dibrDepthSrvOnly.load())
    {
        if (!g_dibrDepthSrvWritten)
        {
            ID3D11Device* dev = g_device;
            bool releaseDev = false;
            if (dev == nullptr)
            {
                context->GetDevice(&dev);
                releaseDev = true;
            }

            HRESULT createHr = E_FAIL;
            HRESULT srvHr = E_FAIL;
            ID3D11Texture2D* copyTex = nullptr;
            ID3D11ShaderResourceView* srv = nullptr;
            DXGI_FORMAT srvFormat = DibrDepthSrvFormat(src.Format);
            if (dev != nullptr)
            {
                D3D11_TEXTURE2D_DESC copy = src;
                copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                copy.Usage = D3D11_USAGE_DEFAULT;
                copy.CPUAccessFlags = 0;
                copy.MiscFlags = 0;
                copy.MipLevels = 1;
                copy.ArraySize = 1;
                createHr = dev->CreateTexture2D(&copy, nullptr, &copyTex);
                if (SUCCEEDED(createHr) && copyTex != nullptr)
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
                    sd.Format = srvFormat;
                    if (src.SampleDesc.Count > 1)
                    {
                        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
                    }
                    else
                    {
                        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        sd.Texture2D.MipLevels = 1;
                    }
                    srvHr = dev->CreateShaderResourceView(copyTex, &sd, &srv);
                }
            }
            WriteDepthSrvReport(src, createHr, srvHr, srvFormat);
            XrLog("[AKDIBR] depth SRV-only result create=0x%08X srv=0x%08X %ux%u fmt=%u srvFmt=%u samples=%u",
                  static_cast<unsigned>(createHr), static_cast<unsigned>(srvHr),
                  src.Width, src.Height, static_cast<unsigned>(src.Format), static_cast<unsigned>(srvFormat), src.SampleDesc.Count);
            SafeRelease(srv);
            SafeRelease(copyTex);
            if (releaseDev && dev != nullptr) dev->Release();
            g_dibrDepthSrvWritten = true;
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        depthTex->Release();
        resource->Release();
        return;
    }
    if (g_dibrDepthCopyOnly.load())
    {
        if (!g_dibrDepthCopyWritten)
        {
            ID3D11Device* dev = g_device;
            bool releaseDev = false;
            if (dev == nullptr)
            {
                context->GetDevice(&dev);
                releaseDev = true;
            }

            HRESULT createHr = E_FAIL;
            HRESULT copyHr = E_FAIL;
            ID3D11Texture2D* copyTex = nullptr;
            if (dev != nullptr)
            {
                D3D11_TEXTURE2D_DESC copy = src;
                copy.BindFlags = 0;
                copy.Usage = D3D11_USAGE_DEFAULT;
                copy.CPUAccessFlags = 0;
                copy.MiscFlags = 0;
                copy.MipLevels = 1;
                copy.ArraySize = 1;
                createHr = dev->CreateTexture2D(&copy, nullptr, &copyTex);
                if (SUCCEEDED(createHr) && copyTex != nullptr)
                {
                    context->CopyResource(copyTex, depthTex);
                    copyHr = S_OK;
                }
            }
            WriteDepthCopyReport(src, createHr, copyHr);
            XrLog("[AKDIBR] depth copy-only result create=0x%08X copy=0x%08X %ux%u fmt=%u samples=%u",
                  static_cast<unsigned>(createHr), static_cast<unsigned>(copyHr),
                  src.Width, src.Height, static_cast<unsigned>(src.Format), src.SampleDesc.Count);
            SafeRelease(copyTex);
            if (releaseDev && dev != nullptr) dev->Release();
            g_dibrDepthCopyWritten = true;
        }
        g_dibrEnabled.store(false);
        g_dibrProofArmed.store(false);
        depthTex->Release();
        resource->Release();
        return;
    }
    if (src.Width >= 1920 && src.Height >= 1080)
    {
        ID3D11Device* dev = g_device;
        bool releaseDev = false;
        if (dev == nullptr)
        {
            context->GetDevice(&dev);
            releaseDev = true;
        }
        if (dev != nullptr)
        {
            const bool needNew = g_dibrDepthCopy == nullptr ||
                                 g_dibrDepthW != src.Width ||
                                 g_dibrDepthH != src.Height ||
                                 g_dibrDepthSamples != src.SampleDesc.Count;
            if (needNew)
            {
                SafeRelease(g_dibrDepthSrv);
                SafeRelease(g_dibrDepthCopy);
                g_dibrDepthReady.store(false);
                g_dibrShadersReady = false;
                g_dibrShadersTried = false;
                D3D11_TEXTURE2D_DESC copy = src;
                copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                copy.Usage = D3D11_USAGE_DEFAULT;
                copy.CPUAccessFlags = 0;
                copy.MiscFlags = 0;
                copy.MipLevels = 1;
                copy.ArraySize = 1;
                HRESULT hr = dev->CreateTexture2D(&copy, nullptr, &g_dibrDepthCopy);
                if (SUCCEEDED(hr) && g_dibrDepthCopy != nullptr)
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
                    sd.Format = DibrDepthSrvFormat(src.Format);
                    if (src.SampleDesc.Count > 1)
                    {
                        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
                    }
                    else
                    {
                        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        sd.Texture2D.MipLevels = 1;
                    }
                    hr = dev->CreateShaderResourceView(g_dibrDepthCopy, &sd, &g_dibrDepthSrv);
                }
                if (FAILED(hr) || g_dibrDepthSrv == nullptr)
                {
                    XrLog("[AKDIBR] depth copy/srv create failed hr=0x%08X srcFmt=%u samples=%u %ux%u",
                          static_cast<unsigned>(hr), static_cast<unsigned>(src.Format),
                          src.SampleDesc.Count, src.Width, src.Height);
                    SafeRelease(g_dibrDepthSrv);
                    SafeRelease(g_dibrDepthCopy);
                }
                else
                {
                    g_dibrDepthW = src.Width;
                    g_dibrDepthH = src.Height;
                    g_dibrDepthSamples = src.SampleDesc.Count;
                    XrLog("[AKDIBR] depth copy ready %ux%u fmt=%u samples=%u",
                          g_dibrDepthW, g_dibrDepthH, static_cast<unsigned>(src.Format), g_dibrDepthSamples);
                }
            }
            if (g_dibrDepthCopy != nullptr)
            {
                context->CopyResource(g_dibrDepthCopy, depthTex);
                g_dibrDepthReady.store(true);
                const unsigned n = g_dibrLogCount.fetch_add(1);
                if (n < 12 || (n % 600) == 0)
                {
                    XrLog("[AKDIBR] captured depth before clear %ux%u samples=%u", src.Width, src.Height, src.SampleDesc.Count);
                }
            }
            if (releaseDev) dev->Release();
        }
    }

    depthTex->Release();
    resource->Release();
}

// Called from the render-fn double-call detour, right after each eye is rendered to the backbuffer:
// copy the finished backbuffer into that eye's history (+ pose-tag the current head orientation).
void CaptureBackbufferToHistory(int eye) noexcept
{
    if (eye < 0 || eye > 1 || g_context == nullptr || g_gameSwapChain == nullptr) return;
    if (!EnsureHistoryTextures()) return;
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(g_gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) && bb != nullptr)
    {
        g_context->CopyResource(g_history[eye], bb);
        g_historyOrient[eye] = g_lastHeadOrient;
        g_historyHasOrient[eye] = true;
        bb->Release();
    }
}

bool IsRunning() noexcept { return g_sessionRunning; }
}
