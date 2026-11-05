// AKProbe — Batman: Arkham Knight (UE3, x64, D3D11) VR mod: dxgi.dll proxy entry point.
// (The "probe" name is historical: this module started as the camera-ownership probe.)
//
// The camera layout below was harvested from masterotaku's geo-11 3D fix shader dumps
// (ShaderFixes/*-vs_replace.txt). Every scene vertex shader does:
//
//     r0 = worldPos * cb0[0..3]      // LocalToWorld  (per object)
//     o5 = r0       * cb1[0..3]      // ViewProjection (per view)  <-- VS register b1, bytes 0..63
//
// Row-vector convention (clip = pos * M, M rows are cb1[0..3]). So for a world-space eye
// offset d, the corrected matrix is T(-d) * M, which touches only the last row:
//
//     row3' = row3 - d.x*row0 - d.y*row1 - d.z*row2
//
// That is an exact camera translation, no decomposition needed. The probe uses it to
// oscillate the camera sideways on a hotkey — if the world sways, the mod owns the camera.
//
// Log:    <game>\Binaries\Win64\logs\AKProbe.log (next to the proxy DLL)
// Deploy: <game>\Binaries\Win64\dxgi.dll

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include "ak_xr.h"
#include "ak_menu.h"
#include "ak_split.h"
#include "ak_flat.h"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
// ---------------------------------------------------------------------------
// logging + real-dxgi proxy plumbing
// ---------------------------------------------------------------------------

HMODULE g_realDxgi = nullptr;
std::once_flag g_loadDxgiOnce;

std::wstring JoinPath(const std::wstring& left, const wchar_t* right)
{
    if (left.empty()) return right;
    std::wstring result = left;
    if (result.back() != L'\\' && result.back() != L'/') result += L'\\';
    result += right;
    return result;
}

std::wstring GetEnvironmentPath(const wchar_t* name)
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(_countof(buffer)));
    if (length == 0 || length >= static_cast<DWORD>(_countof(buffer))) return {};
    return std::wstring(buffer, length);
}

// Folder that holds this proxy DLL (the game's Binaries\Win64). Logs are written under it.
std::wstring LaneFolder()
{
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&LaneFolder), &self) &&
        GetModuleFileNameW(self, path, MAX_PATH) != 0)
    {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash != nullptr) *slash = L'\0';
        return std::wstring(path);
    }
    return L".";
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return "<utf8 failed>";
    std::string result(static_cast<size_t>(bytes), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), bytes, nullptr, nullptr);
    if (written <= 0) return "<utf8 failed>";
    result.resize(static_cast<size_t>(written - 1));
    return result;
}

std::wstring ModulePath(HMODULE module)
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(_countof(buffer)));
    if (length == 0 || length >= static_cast<DWORD>(_countof(buffer))) return L"<unknown>";
    return std::wstring(buffer, length);
}

void LogLine(const char* format, ...) noexcept
{
    char message[2048] = {};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME time = {};
    GetLocalTime(&time);

    char line[2300] = {};
    snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u | %s\r\n",
             time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
             time.wMilliseconds, message);

    OutputDebugStringA(line);

    const std::wstring lane = LaneFolder();
    CreateDirectoryW(lane.c_str(), nullptr);
    const std::wstring logs = JoinPath(lane, L"logs");
    CreateDirectoryW(logs.c_str(), nullptr);

    HANDLE file = CreateFileW(JoinPath(logs, L"AKProbe.log").c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    CloseHandle(file);
}

}   // namespace

// Called by AKDouble at the end of each of its two full-frame render passes. The frame for that
// eye is complete in the backbuffer, so copy it into that eye's history; the XR submit then sends
// both histories together. GPU ordering holds because the copy is issued on the immediate context
// between the two passes' command streams.
void AKProbeCaptureEye(int eye) noexcept
{
    AKFlat::CaptureEye(eye);
    // Feed OpenXR from the same finalized, full-screen eye render before AKFlat composes desktop
    // SBS. This is a no-op until AKXr has captured the game's swapchain/device.
    AKXr::CaptureBackbufferToHistory(eye);
}

// Logging for the other translation units (ak_split.cpp).
void AKProbeLog(const char* format, ...) noexcept
{
    char message[1024] = {};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);
    LogLine("%s", message);
}

namespace
{
std::wstring SystemDxgiPath()
{
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDirectory, static_cast<UINT>(_countof(systemDirectory)));
    if (length == 0 || length >= static_cast<UINT>(_countof(systemDirectory)))
        return L"C:\\Windows\\System32\\dxgi.dll";
    return JoinPath(systemDirectory, L"dxgi.dll");
}

bool EnsureRealDxgi()
{
    std::call_once(g_loadDxgiOnce, []() {
        const std::wstring path = SystemDxgiPath();
        g_realDxgi = LoadLibraryW(path.c_str());
        LogLine("[AKPROBE] real dxgi path=%s module=0x%p err=%lu", WideToUtf8(path).c_str(), g_realDxgi, GetLastError());
    });
    return g_realDxgi != nullptr;
}

template <typename T>
T ResolveRealDxgiTyped(const char* name)
{
    if (!EnsureRealDxgi()) return nullptr;
    FARPROC proc = GetProcAddress(g_realDxgi, name);
    if (proc == nullptr) LogLine("[AKPROBE] missing real dxgi export=%s", name);
    return reinterpret_cast<T>(proc);
}

bool PatchVTable(void* object, size_t slot, void* hook, void** original)
{
    if (object == nullptr || hook == nullptr || original == nullptr) return false;
    void** vtable = *reinterpret_cast<void***>(object);
    if (vtable == nullptr) return false;
    if (vtable[slot] == hook) return false;   // already ours

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    *original = vtable[slot];
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &oldProtect);
    return true;
}

// ---------------------------------------------------------------------------
// hook function pointers
// ---------------------------------------------------------------------------

using CreateDXGIFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateDXGIFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using DXGIDeclareAdapterRemovalSupportFn = HRESULT(WINAPI*)();
using DXGIGetDebugInterfaceFn = HRESULT(WINAPI*)(REFIID, void**);
using DXGIGetDebugInterface1Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using VSSetConstantBuffersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawAutoFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DrawIndexedInstancedIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DrawInstancedIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);

CreateSwapChainFn g_originalCreateSwapChain = nullptr;
CreateSwapChainForHwndFn g_originalCreateSwapChainForHwnd = nullptr;
PresentFn g_originalPresent = nullptr;
Present1Fn g_originalPresent1 = nullptr;
VSSetConstantBuffersFn g_originalVSSetConstantBuffers = nullptr;
MapFn g_originalMap = nullptr;
UnmapFn g_originalUnmap = nullptr;
UpdateSubresourceFn g_originalUpdateSubresource = nullptr;
DrawIndexedFn g_originalDrawIndexed = nullptr;
DrawFn g_originalDraw = nullptr;
DrawIndexedInstancedFn g_originalDrawIndexedInstanced = nullptr;
DrawInstancedFn g_originalDrawInstanced = nullptr;
DrawAutoFn g_originalDrawAuto = nullptr;
DrawIndexedInstancedIndirectFn g_originalDrawIndexedInstancedIndirect = nullptr;
DrawInstancedIndirectFn g_originalDrawInstancedIndirect = nullptr;
CreateDeferredContextFn g_originalCreateDeferredContext = nullptr;
RSSetViewportsFn g_originalRSSetViewports = nullptr;

// Ground truth for "which eye is rendering right now" during a same-frame split. The engine
// sets a half-width viewport as it begins each of the two views, and that latch persists
// through all the draws (and the b1 camera-CB upload) for that view. This is more reliable
// than the eye noted in CalcSceneView, because the CB upload happens later, during render.
std::atomic<int> g_viewportEye{-1};       // -1 full/unknown, 0 left half, 1 right half
std::atomic<unsigned> g_backbufferWidth{0};

// Where does the b1 camera-CB write land relative to the split's two CalcSceneView calls?
// The scene renders on deferred contexts, so the immediate-context viewport hook is blind.
// Count CB writes seen while the split says left / right / neither, to learn which signal works.
std::atomic<unsigned long long> g_cbWriteEyeLeft{0};
std::atomic<unsigned long long> g_cbWriteEyeRight{0};
std::atomic<unsigned long long> g_cbWriteEyeNone{0};

// SFR RECON. Which draw call does Arkham actually use, and does it build command lists?
// `drawIndexed` sat at 1 for a whole session, so the scene is going out some other way.
// Whichever of these climbs is the call a same-frame replay has to re-issue.
std::atomic<unsigned long long> g_drawCount{0};
std::atomic<unsigned long long> g_drawIndexedInstancedCount{0};
std::atomic<unsigned long long> g_drawInstancedCount{0};
std::atomic<unsigned long long> g_drawAutoCount{0};
std::atomic<unsigned long long> g_drawIndirectCount{0};
std::atomic<unsigned long long> g_deferredContexts{0};

std::atomic<unsigned long long> g_presentCount{0};
std::atomic<unsigned long long> g_drawIndexedCount{0};
std::atomic<unsigned long long> g_vpWriteCount{0};      // camera-CB writes identified
std::atomic<unsigned long long> g_vpPatchCount{0};      // camera-CB writes modified
std::atomic<bool> g_contextHooksInstalled{false};

// ---------------------------------------------------------------------------
// UE3 view-projection identification
//
// cb1[0..3] is a row-vector ViewProjection: clip = worldPos * M.
//   column 0 of the 3x3 part = viewRight  * P00
//   column 1                 = viewUp     * P11
//   column 3 (w)             = viewForward           (w_clip == z_view, so unit length)
//   m[15] == 0 for a perspective projection.
// Those three facts together are a very specific signature — a LocalToWorld, a shadow
// matrix or a UI ortho matrix will not pass all of them.
// ---------------------------------------------------------------------------

constexpr UINT kVpBytes = 64;   // 4 rows x float4

bool Finite16(const float* m)
{
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    return true;
}

struct VpFacts
{
    float p00 = 0.0f;         // projection x scale  = 1 / tan(hFov/2)
    float p11 = 0.0f;         // projection y scale  = 1 / tan(vFov/2)
    float fwd[3] = {0, 0, 0}; // view forward, world space
    float right[3] = {0, 0, 0};
    float up[3] = {0, 0, 0};
};

// Returns true and fills `out` when the 64-byte block looks like a scene ViewProjection.
bool LooksLikeViewProjection(const float* m, VpFacts& out)
{
    if (!Finite16(m)) return false;

    // Perspective divide comes from the w column; an ortho/UI matrix has m[15] == 1.
    if (std::fabs(m[15]) > 0.001f) return false;

    const float fx = m[3], fy = m[7], fz = m[11];
    const float fwdLen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fwdLen < 0.95f || fwdLen > 1.05f) return false;   // view forward must be unit

    const float rx = m[0], ry = m[4], rz = m[8];
    const float p00 = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (p00 < 0.4f || p00 > 5.0f) return false;           // ~ 22..135 deg horizontal

    const float ux = m[1], uy = m[5], uz = m[9];
    const float p11 = std::sqrt(ux * ux + uy * uy + uz * uz);
    if (p11 < 0.4f || p11 > 6.0f) return false;

    // right/up/forward must be mutually perpendicular once unscaled.
    const float dotRF = (rx * fx + ry * fy + rz * fz) / p00;
    const float dotUF = (ux * fx + uy * fy + uz * fz) / p11;
    const float dotRU = (rx * ux + ry * uy + rz * uz) / (p00 * p11);
    if (std::fabs(dotRF) > 0.03f || std::fabs(dotUF) > 0.03f || std::fabs(dotRU) > 0.03f) return false;

    out.p00 = p00;
    out.p11 = p11;
    out.fwd[0] = fx; out.fwd[1] = fy; out.fwd[2] = fz;
    out.right[0] = rx / p00; out.right[1] = ry / p00; out.right[2] = rz / p00;
    out.up[0] = ux / p11;    out.up[1] = uy / p11;    out.up[2] = uz / p11;
    return true;
}

// row3' = row3 - d.x*row0 - d.y*row1 - d.z*row2   (exact world-space camera translation)
void TranslateCamera(float* m, const float d[3])
{
    for (int c = 0; c < 4; ++c)
        m[12 + c] -= d[0] * m[0 + c] + d[1] * m[4 + c] + d[2] * m[8 + c];
}

// FOV change. clip.x = dot(pos4, column0), so scaling the whole of column 0 by k scales
// clip.x uniformly — which is exactly p00 -> p00*k, a pure horizontal FOV change. Same for
// column 1 vertically. Widening (k < 1) makes the game render more of the world.
//
// Apply AFTER TranslateCamera: translation is a left-multiply (T*M), this is a right-multiply
// (M*S), so translate-then-scale gives (T*M)*S, which is the required order.
void ScaleProjection(float* m, float kH, float kV)
{
    m[0] *= kH; m[4] *= kH; m[8] *= kH; m[12] *= kH;
    m[1] *= kV; m[5] *= kV; m[9] *= kV; m[13] *= kV;
}

// ---------------------------------------------------------------------------
// which buffers ride VS slot b1
// ---------------------------------------------------------------------------

std::mutex g_b1Mutex;
std::unordered_map<ID3D11Buffer*, unsigned long long> g_b1Buffers;   // buffer -> times bound at b1

bool IsB1Buffer(ID3D11Resource* resource)
{
    if (resource == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_b1Mutex);
    return g_b1Buffers.find(reinterpret_cast<ID3D11Buffer*>(resource)) != g_b1Buffers.end();
}

// ---------------------------------------------------------------------------
// per-eye camera + hotkeys
//
//   F9  = dump the live VP + derived FOV to the log once
//   F10 = ownership proof (kept from Stage 3): oscillate the camera sideways
//   Insert = the VR menu (stereo on/off, separation, recenter, ...)
//
// Stereo: AKXr owns the eye cadence. Every VP write asks which eye is being
// rendered right now and shifts the camera by half the separation along the
// view-right axis. Because AKXr also carries the pose it hands the compositor,
// the eye rendered and the eye submitted can never drift apart.
// ---------------------------------------------------------------------------

std::atomic<int> g_nudgeMode{0};        // 0 off, 1 oscillate (F10 ownership proof)
std::atomic<bool> g_dumpRequest{false};
// OpenXR/menu frame driver. Flat SBS remains active independently, so disabling XR never removes
// the verified desktop fallback.
std::atomic<bool> g_vrSubmit{true};

// CAMERA LOCK — the fix for "objects swing when Batman/the Batmobile moves".
//
// AER renders eye 0 on frame N and eye 1 on frame N+1. Each frame is a full, correctly lit
// image (that part was never broken), but the game advances the CAMERA between them, so the two
// eyes look from different places. The brain reads that as the world swinging — and only while
// moving, which is exactly what was reported.
//
// Fix: the two eyes of a pair share ONE camera. Capture the view-projection on the eye-0 frame,
// and on the eye-1 frame substitute it back before applying that eye's offset. The camera then
// updates once per PAIR instead of once per frame, so both eyes always agree.
//
// Only the main scene view is locked: the reference is matched by projection (p00/p11), so
// shadow and secondary views — which have their own FOV — pass through untouched.
std::atomic<bool> g_cameraLock{false};

std::mutex g_camRefMutex;
float g_camRefMatrix[16] = {};
float g_camRefP00 = 0.0f;
float g_camRefP11 = 0.0f;
bool  g_camRefValid = false;
std::atomic<unsigned long long> g_camLockApplied{0};
std::atomic<unsigned long long> g_temporalVpPatches{0};
std::atomic<unsigned long long> g_temporalOffsetMask{0};
std::atomic_bool g_xrCanonicalFovLocked{false};
float g_nudgeAmountUu = 0.0f;

constexpr float kOscillateAmplitudeUu = 200.0f;   // ~4 m at 50 uu/m — unmissable

// Rising-edge key detect. The first poll only LATCHES the current state — a key already down at
// startup (stray input, a remapper, a held key) must never count as a press. The naive version
// (`static bool prev = false`) treated "down on the very first poll" as a press, and a stray F8
// silently disabled the split 0.3s into launch for a whole session. Don't reintroduce that.
struct EdgeKey
{
    bool prev = false;
    bool primed = false;

    bool Pressed(int vk) noexcept
    {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (!primed) { primed = true; prev = down; return false; }
        const bool rising = down && !prev;
        prev = down;
        return rising;
    }
};

bool CtrlHeld() noexcept { return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0; }

void PollHotkeys()
{
    static EdgeKey k1, k2, k3, k4, k5, k6, k7, k8, k9, k10, kRecenter;

    if (k9.Pressed(VK_F9)) g_dumpRequest.store(true, std::memory_order_relaxed);

    if (k10.Pressed(VK_F10))
    {
        const int mode = g_nudgeMode.load(std::memory_order_relaxed) == 1 ? 0 : 1;
        g_nudgeMode.store(mode, std::memory_order_relaxed);
        LogLine("[AKCAM] F10 oscillate nudge = %s", mode == 1 ? "ON" : "OFF");
    }

    // CTRL+F8: same-frame split on/off. Behind CTRL deliberately — this switch kills the whole
    // feature, and a bare F8 already did exactly that once from a stray keypress at startup.
    if (k8.Pressed(VK_F8) && CtrlHeld())
    {
        const bool on = !AKSplit::IsEnabled();
        AKSplit::SetEnabled(on);
        AKXr::SetSameFrameActive(on);   // SBS submit path when splitting; AER path when not
    }

    // F7: right eye own view-state vs shared (the black-right-eye A/B).
    if (k7.Pressed(VK_F7)) AKSplit::ToggleSecondViewState();

    // F6: swap which half renders first (bisect call-order vs screen-position for the dark eye).
    if (k6.Pressed(VK_F6)) AKSplit::ToggleRenderOrder();

    // F4: mirror the per-view render flag onto the 2nd view (split work only). A/B toggle.
    if (k4.Pressed(VK_F4)) AKSplit::ToggleMirrorRenderFlag();

    // F1 is disabled in this flat-only build; the process-wide QPC hook is not installed.
    if (k1.Pressed(VK_F1))
        LogLine("[AKPROBE] F1 ignored: clock freeze removed from flat-stereo build");

    // F2: true same-frame double render on/off. Off = AER (+ camera lock) — the A/B that matters.
    if (k2.Pressed(VK_F2))
    {
        const bool on = !AKDouble::IsEnabled();
        AKDouble::SetEnabled(on);
        AKXr::SetSameFrameActive(on);
        AKXr::SetSbsFromBackbuffer(false);
        LogLine("[AKPROBE] flat mode = %s", on ? "DOUBLE COMPLETE RENDERS -> SBS" : "STOCK MONO");
    }

    // F3: camera lock on/off — the swim fix. Off = plain AER (eyes one frame of camera apart).
    if (k3.Pressed(VK_F3))
    {
        const bool on = !g_cameraLock.load(std::memory_order_relaxed);
        g_cameraLock.store(on, std::memory_order_relaxed);
        if (!on)
        {
            std::lock_guard<std::mutex> lock(g_camRefMutex);
            g_camRefValid = false;
        }
        LogLine("[AKCAM] camera lock (shared per-pair camera) = %s", on ? "ON" : "OFF");
    }

    // F5: enable/disable OpenXR session submission while leaving verified flat stereo running.
    if (k5.Pressed(VK_F5))
    {
        AKXr::DibrTuning tuning = {};
        AKXr::GetDibrTuning(&tuning);
        tuning.xrEnabled = !tuning.xrEnabled;
        AKXr::SetDibrTuning(tuning);
        LogLine("[AKPROBE] F5 OpenXR submit = %s", tuning.xrEnabled ? "ON" : "OFF");
    }

    // Recenter (default Home, reassignable in the menu) — re-anchor "forward" to the head.
    {
        AKXr::DibrTuning tuning = {};
        AKXr::GetDibrTuning(&tuning);
        if (tuning.recenterKey > 0 && kRecenter.Pressed(tuning.recenterKey)) AKXr::Recenter();
    }

    const unsigned long long frame = g_presentCount.load(std::memory_order_relaxed);
    g_nudgeAmountUu = kOscillateAmplitudeUu * std::sin(static_cast<float>(frame) * 0.06f);
}

// Camera lock. Eye 0 captures the frame's camera as the reference; eye 1 gets that same matrix
// substituted back, so both eyes of the pair render from one viewpoint. Returns true if it
// replaced the matrix (caller re-derives the basis).
//
// Lives in its own function because it takes a lock, and a std::lock_guard cannot be used inside
// the caller's __try block (C2712 — object unwinding).
bool ApplyCameraLock(float* m, const VpFacts& facts, int eye) noexcept
{
    if (!g_cameraLock.load(std::memory_order_relaxed)) return false;

    std::lock_guard<std::mutex> lock(g_camRefMutex);
    if (eye == 0)
    {
        // Reference = this frame's camera, captured BEFORE any of the mod's edits.
        memcpy(g_camRefMatrix, m, kVpBytes);
        g_camRefP00 = facts.p00;
        g_camRefP11 = facts.p11;
        g_camRefValid = true;
        return false;
    }

    // Match the reference by projection so only the MAIN view is locked — shadow and secondary
    // views have their own FOV and must pass through untouched.
    if (g_camRefValid &&
        std::fabs(facts.p00 - g_camRefP00) < 0.01f &&
        std::fabs(facts.p11 - g_camRefP11) < 0.01f)
    {
        memcpy(m, g_camRefMatrix, kVpBytes);
        g_camLockApplied.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

// A 608-byte scene constant buffer can carry both the current and previous-frame camera
// matrices. Moving only the current matrix makes the motion-vector/TAA pass see a fake camera
// jump between the eyes, which shimmers most visibly on railings and other thin geometry.
// Patch only perspective siblings which match the primary FOV and face the same direction.
void PatchTemporalSiblingMatrices(void* data, UINT bytes, const VpFacts& primary, float shiftUu) noexcept
{
    if (data == nullptr || bytes < (2 * kVpBytes) || shiftUu == 0.0f) return;

    float* base = static_cast<float*>(data);
    const UINT floatCount = bytes / static_cast<UINT>(sizeof(float));

    // Constant-buffer registers are 16 bytes. Start after the primary matrix and allow a
    // temporal matrix to begin at any later register boundary.
    for (UINT offsetFloats = 16; offsetFloats + 16 <= floatCount; offsetFloats += 4)
    {
        float* candidate = base + offsetFloats;
        VpFacts temporal = {};
        if (!LooksLikeViewProjection(candidate, temporal)) continue;
        if (std::fabs(temporal.p00 - primary.p00) > 0.08f ||
            std::fabs(temporal.p11 - primary.p11) > 0.08f) continue;

        const float forwardDot = temporal.fwd[0] * primary.fwd[0] +
                                 temporal.fwd[1] * primary.fwd[1] +
                                 temporal.fwd[2] * primary.fwd[2];
        if (forwardDot < 0.70f) continue;

        const float d[3] = {temporal.right[0] * shiftUu,
                            temporal.right[1] * shiftUu,
                            temporal.right[2] * shiftUu};
        TranslateCamera(candidate, d);
        g_temporalVpPatches.fetch_add(1, std::memory_order_relaxed);

        const UINT registerOffset = offsetFloats / 4;
        if (registerOffset < 64)
        {
            const unsigned long long bit = 1ull << registerOffset;
            const unsigned long long previous = g_temporalOffsetMask.fetch_or(bit, std::memory_order_relaxed);
            if ((previous & bit) == 0)
                LogLine("[AKTEMP] patched scene temporal VP at CB byte offset %u", offsetFloats * 4);
        }
    }
}
// Inspect (and rewrite) a 64-byte candidate VP that is about to reach the GPU.
// `data` is writable memory (the mapped CB, or a private staging copy).
void HandleCameraWrite(void* data, UINT bytes, const char* api)
{
    if (data == nullptr || bytes < kVpBytes) return;

    float* m = static_cast<float*>(data);
    VpFacts facts;

    __try
    {
        if (!LooksLikeViewProjection(m, facts)) return;

        const unsigned long long n = g_vpWriteCount.fetch_add(1, std::memory_order_relaxed) + 1;

        const bool dump = g_dumpRequest.exchange(false, std::memory_order_relaxed);
        if (n <= 5 || (n % 20000) == 0 || dump)
        {
            const float hFovDeg = 2.0f * std::atan(1.0f / facts.p00) * 57.2957795f;
            const float vFovDeg = 2.0f * std::atan(1.0f / facts.p11) * 57.2957795f;
            LogLine("[AKCAM] VP #%llu api=%s bytes=%u p00=%.4f p11=%.4f hFov=%.2f vFov=%.2f "
                    "fwd=(%.3f %.3f %.3f) right=(%.3f %.3f %.3f) up=(%.3f %.3f %.3f) row3=(%.1f %.1f %.1f %.1f)",
                    n, api, bytes,
                    static_cast<double>(facts.p00), static_cast<double>(facts.p11),
                    static_cast<double>(hFovDeg), static_cast<double>(vFovDeg),
                    static_cast<double>(facts.fwd[0]), static_cast<double>(facts.fwd[1]), static_cast<double>(facts.fwd[2]),
                    static_cast<double>(facts.right[0]), static_cast<double>(facts.right[1]), static_cast<double>(facts.right[2]),
                    static_cast<double>(facts.up[0]), static_cast<double>(facts.up[1]), static_cast<double>(facts.up[2]),
                    static_cast<double>(m[12]), static_cast<double>(m[13]), static_cast<double>(m[14]), static_cast<double>(m[15]));
        }

        float shiftUu = 0.0f;

        // Stereo: shift by half the separation, sign per eye.
        AKXr::DibrTuning tuning = {};
        AKXr::GetDibrTuning(&tuning);
        if (tuning.stereoOn)
        {
            // Which eye is this camera write for? Three stereo drivers, in priority order.
            const int doublePass = AKDouble::CurrentPass();
            int eye;
            bool lockCamera;
            bool engineEyeCamera = false;

            if (doublePass == 0 || doublePass == 1)
            {
                // TRUE same-frame double render. Both passes come from ONE simulation state, so
                // the eyes already agree — the camera lock must NOT run here (it would force the
                // second pass onto the first pass's matrix and cancel the eye offset).
                eye = doublePass;
                lockCamera = false;
                engineEyeCamera = AKDouble::EngineEyeCameraApplied(doublePass);
            }
            else if (AKSplit::IsSplitting())
            {
                // Viewport split (parked). The scene renders on deferred contexts, so the
                // immediate-context viewport hook is blind; prefer the split's own eye.
                eye = AKSplit::CurrentEye();
                if (eye != 0 && eye != 1) eye = g_viewportEye.load(std::memory_order_relaxed);
                lockCamera = false;

                if (eye == 0)      g_cbWriteEyeLeft.fetch_add(1, std::memory_order_relaxed);
                else if (eye == 1) g_cbWriteEyeRight.fetch_add(1, std::memory_order_relaxed);
                else               g_cbWriteEyeNone.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                // AER: eyes are a frame apart, so they need the shared-camera lock.
                eye = AKXr::GetActiveEye();                     // 0 = left, 1 = right
                lockCamera = true;
            }

            if (eye == 0 || eye == 1)
            {
                if (lockCamera && ApplyCameraLock(m, facts, eye)) LooksLikeViewProjection(m, facts);

                float sign = (eye == 1) ? 1.0f : -1.0f;
                if (tuning.aerSwapEyes) sign = -sign;
                // When Arkham's real camera origin was shifted before FSceneView construction,
                // geometry, lighting, shadows, and temporal data already share the eye. Do not
                // apply the legacy GPU-only translation a second time.
                if (!engineEyeCamera) shiftUu += sign * tuning.aerHalfEye;
            }
        }

        // F10 ownership proof rides on top, so it still works with stereo on.
        if (g_nudgeMode.load(std::memory_order_relaxed) == 1) shiftUu += g_nudgeAmountUu;

        if (shiftUu != 0.0f)
        {
            // Temporal siblings must receive the same eye translation as the current camera.
            // Otherwise motion vectors describe the stereo offset as object motion and TAA
            // alternates thin geometry between incompatible histories.
            PatchTemporalSiblingMatrices(data, bytes, facts, shiftUu);
            const float d[3] = {facts.right[0] * shiftUu, facts.right[1] * shiftUu, facts.right[2] * shiftUu};
            TranslateCamera(m, d);
            g_vpPatchCount.fetch_add(1, std::memory_order_relaxed);
        }

        // FOV fill. AK renders ~86 x 55 deg; the headset wants far more vertically, which is
        // why the image sits in a letterbox. The mod owns the projection, so widen what the game
        // actually RENDERS, then declare the widened values below — declared still equals
        // rendered, which is the whole point of doing it here rather than at submit.
        float outP00 = facts.p00;
        float outP11 = facts.p11;
        if (tuning.fovFillEnabled)
        {
            float headHalfH = 0.0f, headHalfV = 0.0f;
            if (AKXr::GetHeadsetFovHalf(&headHalfH, &headHalfV) && headHalfH > 0.1f && headHalfV > 0.1f)
            {
                const float targetP00 = 1.0f / std::tan(headHalfH * tuning.fovFillH);
                const float targetP11 = 1.0f / std::tan(headHalfV * tuning.fovFillV);

                // Only ever widen (k <= 1). Narrowing the game's view to the headset would
                // crop away pixels the game already paid to render.
                float kH = targetP00 / facts.p00;
                float kV = targetP11 / facts.p11;
                if (kH > 1.0f) kH = 1.0f;
                if (kV > 1.0f) kV = 1.0f;

                // UE3 culls against the frustum it built, not the one just widened, so a
                // large widen shows as pop-in at the edges. Hold the widen to something the
                // culler can mostly cover; the menu sliders tune the rest.
                constexpr float kMaxWiden = 0.60f;   // at most ~1.67x the tangent
                if (kH < kMaxWiden) kH = kMaxWiden;
                if (kV < kMaxWiden) kV = kMaxWiden;

                if (kH != 1.0f || kV != 1.0f)
                {
                    ScaleProjection(m, kH, kV);
                    outP00 = facts.p00 * kH;
                    outP11 = facts.p11 * kV;
                }
            }
        }

        // Declared FOV must equal the captured MAIN render. Arkham writes this shared scene CB
        // for the gameplay camera and many secondary cameras. "First eye-0 write per frame" was
        // still unstable across frames and made XR breathe - Hinny that GOAT - between unrelated FOVs. Lock once to
        // the verified 16:9 gameplay projection; photo/secondary views cannot overwrite it.
        bool publishProjection = true;
        const int projectionPass = AKDouble::CurrentPass();
        if (AKDouble::IsEnabled() && !AKSplit::IsEnabled())
        {
            publishProjection = false;
            constexpr float kMainP00 = 1.0711f;
            constexpr float kMainP11 = 1.9042f;
            const bool canonicalMain =
                std::fabs(outP00 - kMainP00) < 0.025f &&
                std::fabs(outP11 - kMainP11) < 0.050f;
            if (projectionPass == 0 && canonicalMain)
            {
                bool expected = false;
                if (g_xrCanonicalFovLocked.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    publishProjection = true;
                    LogLine("[AKXRFOV] canonical gameplay FOV locked p00=%.4f p11=%.4f",
                            static_cast<double>(outP00), static_cast<double>(outP11));
                }
            }
        }
        if (publishProjection) AKXr::SetGameProjection(outP00, outP11);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// ---------------------------------------------------------------------------
// context hooks
// ---------------------------------------------------------------------------

struct MappedInfo
{
    void* data = nullptr;
    UINT bytes = 0;
};

std::mutex g_mapMutex;
std::unordered_map<ID3D11Resource*, MappedInfo> g_mapped;

void STDMETHODCALLTYPE VSSetConstantBuffersHook(ID3D11DeviceContext* context, UINT startSlot, UINT numBuffers, ID3D11Buffer* const* buffers)
{
    if (buffers != nullptr && startSlot <= 1 && startSlot + numBuffers > 1)
    {
        ID3D11Buffer* b1 = buffers[1 - startSlot];
        if (b1 != nullptr)
        {
            std::lock_guard<std::mutex> lock(g_b1Mutex);
            if (g_b1Buffers.size() < 4096) ++g_b1Buffers[b1];
        }
    }

    if (g_originalVSSetConstantBuffers != nullptr)
        g_originalVSSetConstantBuffers(context, startSlot, numBuffers, buffers);
}

bool IsSmallConstantBuffer(ID3D11Resource* resource, UINT& byteWidth)
{
    byteWidth = 0;
    if (resource == nullptr) return false;
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) return false;

    ID3D11Buffer* buffer = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) || buffer == nullptr)
        return false;
    D3D11_BUFFER_DESC desc = {};
    buffer->GetDesc(&desc);
    buffer->Release();

    if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0) return false;
    if (desc.ByteWidth < kVpBytes || desc.ByteWidth > 8192) return false;
    byteWidth = desc.ByteWidth;
    return true;
}

// SFR RECON, mechanism 2. The hook runs inside the engine at the exact moment it finishes
// writing the camera, so the in-module return addresses on this stack point at the function
// that sets up the view — the seam a same-frame render hook needs. No pattern scanning: the
// seam comes to the hook. Read-only, logged a handful of times.
// Kept in its own function because __try cannot live where objects need unwinding.
void TraceCameraWriteCallers() noexcept
{
    static std::atomic<unsigned> s_traces{0};
    if (s_traces.fetch_add(1, std::memory_order_relaxed) >= 6) return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    char chain[512];
    int p = 0;
    __try
    {
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(_AddressOfReturnAddress());
        for (int k = 0; k < 64 && p < 460; ++k)
        {
            const uintptr_t v = sp[k];
            if (v > base && v < base + 0x4000000ull)
                p += snprintf(chain + p, sizeof(chain) - static_cast<size_t>(p), "+%llX ",
                              static_cast<unsigned long long>(v - base));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { p = 0; }
    chain[p] = '\0';
    LogLine("[AKSFR] camera-write callers (exe-relative, innermost first): %s", chain);
}

HRESULT STDMETHODCALLTYPE MapHook(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource, D3D11_MAP mapType, UINT mapFlags, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    const HRESULT hr = g_originalMap(context, resource, subresource, mapType, mapFlags, mapped);
    if (SUCCEEDED(hr) && mapped != nullptr && mapped->pData != nullptr && subresource == 0)
    {
        UINT bytes = 0;
        if (IsSmallConstantBuffer(resource, bytes) && IsB1Buffer(resource))
        {
            std::lock_guard<std::mutex> lock(g_mapMutex);
            g_mapped[resource] = MappedInfo{mapped->pData, bytes};
        }
    }
    return hr;
}

void STDMETHODCALLTYPE UnmapHook(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource)
{
    MappedInfo info;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        auto it = g_mapped.find(resource);
        if (it != g_mapped.end())
        {
            info = it->second;
            g_mapped.erase(it);
            found = true;
        }
    }

    // Patch while the GPU-visible staging memory is still ours to touch.
    if (found && subresource == 0)
    {
        HandleCameraWrite(info.data, info.bytes, "Map");

        TraceCameraWriteCallers();
    }

    g_originalUnmap(context, resource, subresource);
}

void STDMETHODCALLTYPE UpdateSubresourceHook(ID3D11DeviceContext* context, ID3D11Resource* dstResource, UINT dstSubresource, const D3D11_BOX* dstBox, const void* srcData, UINT srcRowPitch, UINT srcDepthPitch)
{
    UINT bytes = 0;
    if (srcData != nullptr && dstSubresource == 0 && dstBox == nullptr &&
        IsSmallConstantBuffer(dstResource, bytes) && IsB1Buffer(dstResource) && bytes >= kVpBytes)
    {
        // srcData is the caller's memory — never write through it. Stage, patch, forward.
        unsigned char staging[8192];
        const UINT copyBytes = bytes < sizeof(staging) ? bytes : static_cast<UINT>(sizeof(staging));
        bool copied = false;
        __try
        {
            memcpy(staging, srcData, copyBytes);
            copied = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (copied)
        {
            HandleCameraWrite(staging, copyBytes, "UpdateSubresource");
            g_originalUpdateSubresource(context, dstResource, dstSubresource, dstBox, staging, srcRowPitch, srcDepthPitch);
            return;
        }
    }

    g_originalUpdateSubresource(context, dstResource, dstSubresource, dstBox, srcData, srcRowPitch, srcDepthPitch);
}

void STDMETHODCALLTYPE DrawIndexedHook(ID3D11DeviceContext* context, UINT indexCount, UINT startIndexLocation, INT baseVertexLocation)
{
    g_drawIndexedCount.fetch_add(1, std::memory_order_relaxed);
    g_originalDrawIndexed(context, indexCount, startIndexLocation, baseVertexLocation);
}

void STDMETHODCALLTYPE DrawHook(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertexLocation)
{
    g_drawCount.fetch_add(1, std::memory_order_relaxed);
    g_originalDraw(context, vertexCount, startVertexLocation);
}

void STDMETHODCALLTYPE DrawIndexedInstancedHook(ID3D11DeviceContext* context, UINT indexCountPerInstance, UINT instanceCount, UINT startIndexLocation, INT baseVertexLocation, UINT startInstanceLocation)
{
    g_drawIndexedInstancedCount.fetch_add(1, std::memory_order_relaxed);
    g_originalDrawIndexedInstanced(context, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
}

void STDMETHODCALLTYPE DrawInstancedHook(ID3D11DeviceContext* context, UINT vertexCountPerInstance, UINT instanceCount, UINT startVertexLocation, UINT startInstanceLocation)
{
    g_drawInstancedCount.fetch_add(1, std::memory_order_relaxed);
    g_originalDrawInstanced(context, vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
}

void STDMETHODCALLTYPE DrawAutoHook(ID3D11DeviceContext* context)
{
    g_drawAutoCount.fetch_add(1, std::memory_order_relaxed);
    g_originalDrawAuto(context);
}

void STDMETHODCALLTYPE DrawIndexedInstancedIndirectHook(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset)
{
    g_drawIndirectCount.fetch_add(1, std::memory_order_relaxed);
    g_originalDrawIndexedInstancedIndirect(context, args, offset);
}

void STDMETHODCALLTYPE DrawInstancedIndirectHook(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset)
{
    g_drawIndirectCount.fetch_add(1, std::memory_order_relaxed);
    g_originalDrawInstancedIndirect(context, args, offset);
}

void STDMETHODCALLTYPE RSSetViewportsHook(ID3D11DeviceContext* context, UINT numViewports, const D3D11_VIEWPORT* viewports)
{
    if (numViewports >= 1 && viewports != nullptr)
    {
        const unsigned bbW = g_backbufferWidth.load(std::memory_order_relaxed);
        const float w = viewports[0].Width;
        const float x = viewports[0].TopLeftX;
        int eye = -1;
        if (bbW > 0)
        {
            const float halfW = static_cast<float>(bbW) * 0.5f;
            // A half-width viewport = one eye of the split. Left if it starts near 0, right if
            // near the middle. A full-width (or any other) viewport = a shared pass → -1.
            if (w > halfW * 0.6f && w < halfW * 1.4f)
                eye = (x < halfW * 0.5f) ? 0 : 1;
        }
        g_viewportEye.store(eye, std::memory_order_relaxed);
        AKSplit::NoteViewport(x, w);
    }
    g_originalRSSetViewports(context, numViewports, viewports);
}

// If this never fires, Arkham has no deferred contexts and therefore no command lists —
// which rules out the cheapest same-frame replay outright, no further testing needed.
HRESULT STDMETHODCALLTYPE CreateDeferredContextHook(ID3D11Device* device, UINT flags, ID3D11DeviceContext** deferred)
{
    const HRESULT hr = g_originalCreateDeferredContext(device, flags, deferred);
    const unsigned long long n = g_deferredContexts.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8)
        LogLine("[AKSFR] CreateDeferredContext #%llu hr=0x%08X ctx=0x%p", n,
                static_cast<unsigned>(hr), deferred != nullptr ? *deferred : nullptr);
    return hr;
}

void InstallD3D11ContextHooks(IDXGISwapChain* swapChain, const char* where)
{
    if (swapChain == nullptr) return;

    ID3D11Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || device == nullptr)
    {
        LogLine("[AKPROBE] GetDevice failed at %s", where);
        return;
    }

    ID3D11DeviceContext* immediate = nullptr;
    device->GetImmediateContext(&immediate);
    const D3D_FEATURE_LEVEL featureLevel = device->GetFeatureLevel();
    device->Release();
    if (immediate == nullptr) return;

    const bool vsCb  = PatchVTable(immediate,  7, reinterpret_cast<void*>(&VSSetConstantBuffersHook), reinterpret_cast<void**>(&g_originalVSSetConstantBuffers));
    const bool drawI = PatchVTable(immediate, 12, reinterpret_cast<void*>(&DrawIndexedHook),          reinterpret_cast<void**>(&g_originalDrawIndexed));
    const bool map   = PatchVTable(immediate, 14, reinterpret_cast<void*>(&MapHook),                  reinterpret_cast<void**>(&g_originalMap));
    const bool unmap = PatchVTable(immediate, 15, reinterpret_cast<void*>(&UnmapHook),                reinterpret_cast<void**>(&g_originalUnmap));
    const bool upd   = PatchVTable(immediate, 48, reinterpret_cast<void*>(&UpdateSubresourceHook),    reinterpret_cast<void**>(&g_originalUpdateSubresource));

    // SFR recon: the rest of the draw family, to learn which call AK actually issues.
    PatchVTable(immediate, 13, reinterpret_cast<void*>(&DrawHook),                          reinterpret_cast<void**>(&g_originalDraw));
    PatchVTable(immediate, 20, reinterpret_cast<void*>(&DrawIndexedInstancedHook),          reinterpret_cast<void**>(&g_originalDrawIndexedInstanced));
    PatchVTable(immediate, 21, reinterpret_cast<void*>(&DrawInstancedHook),                 reinterpret_cast<void**>(&g_originalDrawInstanced));
    PatchVTable(immediate, 38, reinterpret_cast<void*>(&DrawAutoHook),                      reinterpret_cast<void**>(&g_originalDrawAuto));
    PatchVTable(immediate, 39, reinterpret_cast<void*>(&DrawIndexedInstancedIndirectHook),  reinterpret_cast<void**>(&g_originalDrawIndexedInstancedIndirect));
    PatchVTable(immediate, 40, reinterpret_cast<void*>(&DrawInstancedIndirectHook),         reinterpret_cast<void**>(&g_originalDrawInstancedIndirect));
    PatchVTable(immediate, 44, reinterpret_cast<void*>(&RSSetViewportsHook),                reinterpret_cast<void**>(&g_originalRSSetViewports));

    // Device-side: does AK ever build a command list? (ID3D11Device::CreateDeferredContext = 27)
    {
        ID3D11Device* dev = nullptr;
        immediate->GetDevice(&dev);
        if (dev != nullptr)
        {
            PatchVTable(dev, 27, reinterpret_cast<void*>(&CreateDeferredContextHook), reinterpret_cast<void**>(&g_originalCreateDeferredContext));
            dev->Release();
        }
    }

    if (vsCb || drawI || map || unmap || upd)
    {
        LogLine("[AKPROBE] context hooks at %s ctx=0x%p feature=0x%X vsCB=%u drawIndexed=%u map=%u unmap=%u update=%u",
                where, immediate, static_cast<unsigned>(featureLevel),
                vsCb ? 1u : 0u, drawI ? 1u : 0u, map ? 1u : 0u, unmap ? 1u : 0u, upd ? 1u : 0u);
        g_contextHooksInstalled.store(true, std::memory_order_release);
    }

    // Install the crash-safe engine split once the renderer is up. It is the only stereo driver
    // enabled at startup and writes SBS directly to the monitor.
    static std::atomic<bool> s_splitOnce{false};
    if (!s_splitOnce.exchange(true))
    {
        AKSplit::Install();
        AKDouble::Install();
        AKFreeze::SetEnabled(false);  // Do not install the process-wide QueryPerformanceCounter hook.

        // The verified double render feeds AKFlat and two OpenXR history textures independently.
        const bool doubleOn = AKDouble::IsEnabled() && !AKSplit::IsEnabled();
        AKXr::SetSameFrameActive(doubleOn || AKSplit::IsEnabled());
        AKXr::SetSbsFromBackbuffer(AKSplit::IsEnabled());

        LogLine("[AKPROBE] STARTUP: FLAT+VR mode=%s  clockFreeze=%s (%s)  cameraLock=%s  OpenXR=%s",
                AKSplit::IsEnabled() ? "SPLIT"
                    : (doubleOn ? "DOUBLE-RENDER"
                       : (AKFreeze::IsEnabled() ? "SAME-FRAME (clock freeze)" : "MONO FALLBACK")),
                AKFreeze::IsEnabled() ? "ON" : "OFF", AKFreeze::Status(),
                g_cameraLock.load(std::memory_order_relaxed) ? "ON" : "OFF",
                g_vrSubmit.load(std::memory_order_relaxed) ? "AVAILABLE" : "OFF");
    }

    immediate->Release();
}

void OnFrame(IDXGISwapChain* swapChain)
{
    const unsigned long long count = g_presentCount.fetch_add(1, std::memory_order_relaxed) + 1;
    PollHotkeys();

    // Latch the backbuffer width every frame — the RSSetViewports eye-latch needs it from the
    // very first frame, well before the audit gate below fires.
    if (g_backbufferWidth.load(std::memory_order_relaxed) == 0)
    {
        DXGI_SWAP_CHAIN_DESC d = {};
        if (SUCCEEDED(swapChain->GetDesc(&d))) g_backbufferWidth.store(d.BufferDesc.Width, std::memory_order_relaxed);
    }

    if (count <= 3 || count == 60 || (count % 600) == 0)
    {
        InstallD3D11ContextHooks(swapChain, "Present");

        DXGI_SWAP_CHAIN_DESC desc = {};
        swapChain->GetDesc(&desc);
        g_backbufferWidth.store(desc.BufferDesc.Width, std::memory_order_relaxed);
        size_t b1Count = 0;
        {
            std::lock_guard<std::mutex> lock(g_b1Mutex);
            b1Count = g_b1Buffers.size();
        }
        LogLine("[AKPROBE] frame=%llu backbuffer=%ux%u fmt=%u b1Buffers=%zu vpWrites=%llu vpPatches=%llu mode=%d",
                count, desc.BufferDesc.Width, desc.BufferDesc.Height,
                static_cast<unsigned>(desc.BufferDesc.Format), b1Count,
                g_vpWriteCount.load(std::memory_order_relaxed),
                g_vpPatchCount.load(std::memory_order_relaxed),
                g_nudgeMode.load(std::memory_order_relaxed));

        LogLine("[AKSFR] draws: DrawIndexed=%llu Draw=%llu DrawIndexedInstanced=%llu DrawInstanced=%llu "
                "DrawAuto=%llu Indirect=%llu | deferredContexts=%llu",
                g_drawIndexedCount.load(std::memory_order_relaxed),
                g_drawCount.load(std::memory_order_relaxed),
                g_drawIndexedInstancedCount.load(std::memory_order_relaxed),
                g_drawInstancedCount.load(std::memory_order_relaxed),
                g_drawAutoCount.load(std::memory_order_relaxed),
                g_drawIndirectCount.load(std::memory_order_relaxed),
                g_deferredContexts.load(std::memory_order_relaxed));

        LogLine("[AKSPLIT] enabled=%d splitting=%d splitCalls=%llu passthrough=%llu viewportEye=%d "
                "ownState=%d cbEye L=%llu R=%llu none=%llu reason='%s'",
                AKSplit::IsEnabled() ? 1 : 0, AKSplit::IsSplitting() ? 1 : 0,
                AKSplit::SplitCalls(), AKSplit::PassthroughCalls(),
                g_viewportEye.load(std::memory_order_relaxed),
                AKSplit::UsesSecondViewState() ? 1 : 0,
                g_cbWriteEyeLeft.load(std::memory_order_relaxed),
                g_cbWriteEyeRight.load(std::memory_order_relaxed),
                g_cbWriteEyeNone.load(std::memory_order_relaxed),
                AKSplit::DisabledReason());

        LogLine("[AKCAM] cameraLock=%d applied=%llu | double: enabled=%d running=%d frames=%llu status='%s'",
                g_cameraLock.load(std::memory_order_relaxed) ? 1 : 0,
                g_camLockApplied.load(std::memory_order_relaxed),
                AKDouble::IsEnabled() ? 1 : 0, AKDouble::IsRunning() ? 1 : 0,
                AKDouble::Frames(), AKDouble::Status());

        // The one line that answers "is the headset actually getting two eyes?". Everything above
        // describes the stereo SOURCE; this describes what was submitted. xrPairSubmits=0 alongside
        // a healthy [AKFLAT] composed count means desktop SBS is stereo while the headset is flat.
        {
            unsigned long long xrPairs = 0, xrMonos = 0;
            AKXr::SameFrameSubmitStats(&xrPairs, &xrMonos);
            AKXr::DibrTuning tuning = {};
            AKXr::GetDibrTuning(&tuning);
            LogLine("[AKPAIR] xrPairSubmits=%llu xrMonoSubmits=%llu | engineEyeShifts=%llu "
                    "lastShift L=%.3f R=%.3f | halfEye=%.3f swapEyes=%d stereoOn=%d",
                    xrPairs, xrMonos, AKDouble::EngineEyeShifts(),
                    static_cast<double>(AKDouble::EngineEyeLastShift(0)),
                    static_cast<double>(AKDouble::EngineEyeLastShift(1)),
                    static_cast<double>(tuning.aerHalfEye), tuning.aerSwapEyes ? 1 : 0,
                    tuning.stereoOn ? 1 : 0);
        }

        LogLine("[AKTEMP] siblingPatches=%llu offsetMask=0x%llX",
                g_temporalVpPatches.load(std::memory_order_relaxed),
                g_temporalOffsetMask.load(std::memory_order_relaxed));
        LogLine("[AKFLAT] ready=%d composed=%llu status='%s'",
                AKFlat::IsReady() ? 1 : 0, AKFlat::ComposedFrames(), AKFlat::Status());

        LogLine("[AKFREEZE] enabled=%d frozenNow=%d frozenFrames=%llu status='%s'",
                AKFreeze::IsEnabled() ? 1 : 0, AKFreeze::IsFrozenNow() ? 1 : 0,
                AKFreeze::FrozenFrames(), AKFreeze::Status());
    }
}

// The frame is finished but not yet flipped, so GetBuffer(0) still holds it — capture and
// submit to the headset BEFORE handing the frame to the real Present. This is the ordering
// the Mass Effect lane proved; doing it after the flip gets you the previous frame.
void SubmitToHeadset(IDXGISwapChain* swapChain)
{
    // Capture the finalized right eye first. A true return means both complete full-screen histories
    // belong to this Present; false covers menus/loading/transitions where only a live mono frame exists.
    const bool doubleActive = AKDouble::IsEnabled() && !AKSplit::IsEnabled();
    const bool pairReady = doubleActive && AKDouble::CaptureRightAtPresent();
    AKXr::SetSameFramePairReady(pairReady);

    // Build one ImGui frame into the menu's own overlay texture. This must happen before XR so the
    // eye path can composite it, and it deliberately does NOT touch the backbuffer — that would put
    // the panel underneath the desktop SBS composite below. It also runs unconditionally: the tuner
    // has to work with OpenXR submission toggled off (F5), which it previously did not.
    AKMenu::Update(swapChain);

    // XR must run before AKFlat replaces the game backbuffer with desktop SBS. That lets the no-pair
    // path submit the current full-screen menu/loading image to both eyes instead of an SBS composite.
    if (g_vrSubmit.load(std::memory_order_relaxed))
    {
        AKXr::Frame(swapChain);

        // TRUE same-frame: AKXr::Frame has just flipped to the eye the NEXT frame will render. If that
        // is the second eye of the pair, stop the clock so the simulation cannot advance and both eyes
        // show the same instant. One normal view family per frame — nothing for streaming to trip on.
        AKFreeze::SetNextEye(AKXr::GetActiveEye());
    }

    // Desktop composition is independent and remains the verified fallback even when XR is disabled.
    // It now keeps running with the tuner open, so the desktop mirror shows the same live stereo the
    // headset does while sliders move.
    if (doubleActive) AKFlat::Present(swapChain);

    // Panel last, so it sits on top of the finished desktop image instead of being overwritten by it.
    AKMenu::BlitToBackbuffer(swapChain);
}
HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    OnFrame(swapChain);
    SubmitToHeadset(swapChain);
    return g_originalPresent(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
{
    OnFrame(swapChain);
    SubmitToHeadset(swapChain);
    return g_originalPresent1(swapChain, syncInterval, flags, parameters);
}

void InstallSwapChainHooks(IDXGISwapChain* swapChain, const char* where)
{
    if (swapChain == nullptr) return;

    const bool present = PatchVTable(swapChain, 8, reinterpret_cast<void*>(&PresentHook), reinterpret_cast<void**>(&g_originalPresent));

    bool present1 = false;
    IDXGISwapChain1* swapChain1 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swapChain1))) && swapChain1 != nullptr)
    {
        // Modern UE3 forks can present through Present1 (slot 22) instead of Present.
        present1 = PatchVTable(swapChain1, 22, reinterpret_cast<void*>(&Present1Hook), reinterpret_cast<void**>(&g_originalPresent1));
        swapChain1->Release();
    }

    LogLine("[AKPROBE] swapchain hooks at %s sc=0x%p present=%u present1=%u", where, swapChain, present ? 1u : 0u, present1 ? 1u : 0u);
    InstallD3D11ContextHooks(swapChain, where);
}

HRESULT STDMETHODCALLTYPE CreateSwapChainHook(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain)
{
    const HRESULT hr = g_originalCreateSwapChain(factory, device, desc, swapChain);
    LogLine("[AKPROBE] CreateSwapChain hr=0x%08X sc=0x%p", static_cast<unsigned>(hr), swapChain != nullptr ? *swapChain : nullptr);
    if (SUCCEEDED(hr) && swapChain != nullptr) InstallSwapChainHooks(*swapChain, "CreateSwapChain");
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndHook(IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* restrictTo, IDXGISwapChain1** swapChain)
{
    const HRESULT hr = g_originalCreateSwapChainForHwnd(factory, device, hwnd, desc, fullscreen, restrictTo, swapChain);
    LogLine("[AKPROBE] CreateSwapChainForHwnd hr=0x%08X sc=0x%p", static_cast<unsigned>(hr), swapChain != nullptr ? *swapChain : nullptr);
    if (SUCCEEDED(hr) && swapChain != nullptr) InstallSwapChainHooks(*swapChain, "CreateSwapChainForHwnd");
    return hr;
}

void InstallFactoryHooks(void* factory, const char* api)
{
    if (factory == nullptr) return;

    const bool base = PatchVTable(factory, 10, reinterpret_cast<void*>(&CreateSwapChainHook), reinterpret_cast<void**>(&g_originalCreateSwapChain));

    bool hwnd = false;
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(factory)->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2))) && factory2 != nullptr)
    {
        hwnd = PatchVTable(factory2, 15, reinterpret_cast<void*>(&CreateSwapChainForHwndHook), reinterpret_cast<void**>(&g_originalCreateSwapChainForHwnd));
        factory2->Release();
    }

    LogLine("[AKPROBE] factory hooks api=%s factory=0x%p base=%u hwnd=%u", api, factory, base ? 1u : 0u, hwnd ? 1u : 0u);
}
}   // namespace

// ---------------------------------------------------------------------------
// dxgi exports
// ---------------------------------------------------------------------------

extern "C" HRESULT WINAPI Probe_CreateDXGIFactory(REFIID riid, void** factory)
{
    const auto real = ResolveRealDxgiTyped<CreateDXGIFactoryFn>("CreateDXGIFactory");
    if (real == nullptr) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    const HRESULT hr = real(riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr) InstallFactoryHooks(*factory, "CreateDXGIFactory");
    return hr;
}

extern "C" HRESULT WINAPI Probe_CreateDXGIFactory1(REFIID riid, void** factory)
{
    const auto real = ResolveRealDxgiTyped<CreateDXGIFactoryFn>("CreateDXGIFactory1");
    if (real == nullptr) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    const HRESULT hr = real(riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr) InstallFactoryHooks(*factory, "CreateDXGIFactory1");
    return hr;
}

extern "C" HRESULT WINAPI Probe_CreateDXGIFactory2(UINT flags, REFIID riid, void** factory)
{
    const auto real = ResolveRealDxgiTyped<CreateDXGIFactory2Fn>("CreateDXGIFactory2");
    if (real == nullptr) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    const HRESULT hr = real(flags, riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr) InstallFactoryHooks(*factory, "CreateDXGIFactory2");
    return hr;
}

extern "C" HRESULT WINAPI Probe_DXGIDeclareAdapterRemovalSupport()
{
    const auto real = ResolveRealDxgiTyped<DXGIDeclareAdapterRemovalSupportFn>("DXGIDeclareAdapterRemovalSupport");
    return real != nullptr ? real() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}

extern "C" HRESULT WINAPI Probe_DXGIGetDebugInterface(REFIID riid, void** debugInterface)
{
    const auto real = ResolveRealDxgiTyped<DXGIGetDebugInterfaceFn>("DXGIGetDebugInterface");
    return real != nullptr ? real(riid, debugInterface) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}

extern "C" HRESULT WINAPI Probe_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debugInterface)
{
    const auto real = ResolveRealDxgiTyped<DXGIGetDebugInterface1Fn>("DXGIGetDebugInterface1");
    return real != nullptr ? real(flags, riid, debugInterface) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        LogLine("[AKPROBE] loaded module=%s process=%s", WideToUtf8(ModulePath(module)).c_str(), WideToUtf8(ModulePath(nullptr)).c_str());
        EnsureRealDxgi();
    }
    return TRUE;
}
