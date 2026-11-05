#include "ak_menu.h"

#include "ak_xr.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

namespace AKMenu
{
namespace
{
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
bool g_ready = false;
bool g_open = false;
HWND g_hwnd = nullptr;
WNDPROC g_originalWndProc = nullptr;
int g_targetW = 1280;
int g_targetH = 720;
bool g_applyPlacement = true;
int g_savedFrames = 0;

struct Placement
{
    float x = 80.0f;
    float y = 80.0f;
    float w = 1480.0f;
    float h = 920.0f;
    float alpha = 0.68f;
};

Placement g_place;

// --- offscreen overlay -----------------------------------------------------------------------
// ImGui renders ONCE into this texture. Every consumer (desktop backbuffer, left eye, right eye)
// then composites the same finished image. That keeps the single-owner architecture while letting
// the panel appear in the headset WITHOUT having to shut stereo off first.
ID3D11Texture2D* g_overlayTex = nullptr;
ID3D11RenderTargetView* g_overlayRtv = nullptr;
ID3D11ShaderResourceView* g_overlaySrv = nullptr;
UINT g_overlayW = 0;
UINT g_overlayH = 0;
bool g_overlayValid = false;   // this frame's overlay texture holds a freshly rendered panel

ID3D11VertexShader* g_blitVs = nullptr;
ID3D11PixelShader* g_blitPs = nullptr;
ID3D11SamplerState* g_blitSampler = nullptr;
ID3D11BlendState* g_blitBlend = nullptr;
ID3D11DepthStencilState* g_blitDepth = nullptr;
ID3D11RasterizerState* g_blitRaster = nullptr;

// ME1's virtual cursor. While the menu is open the REAL cursor is pinned to an anchor every frame,
// so mouse motion drives only ImGui — the game polls a cursor that never moves.
bool g_mouseAnchorValid = false;
POINT g_mouseAnchorScreen = {};
float g_virtualMouseX = 0.0f;
float g_virtualMouseY = 0.0f;

template <typename T>
void SafeRelease(T*& ptr) noexcept
{
    if (ptr != nullptr)
    {
        ptr->Release();
        ptr = nullptr;
    }
}

float ClampFloat(float value, float lo, float hi, float fallback) noexcept
{
    if (!std::isfinite(value)) return fallback;
    return (std::max)(lo, (std::min)(hi, value));
}

std::wstring ModuleDirectory() noexcept
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDirectory), &self);
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    for (DWORD i = n; i > 0; --i)
    {
        if (path[i - 1] == L'\\' || path[i - 1] == L'/')
        {
            path[i - 1] = L'\0';
            break;
        }
    }
    return path;
}

std::wstring ConfigPath() noexcept
{
    return ModuleDirectory() + L"\\akvr.ini";
}

bool ReadIniFloat(const wchar_t* section, const wchar_t* key, float* out) noexcept
{
    if (out == nullptr) return false;
    wchar_t buffer[64] = {};
    const DWORD read = GetPrivateProfileStringW(section, key, L"", buffer, static_cast<DWORD>(std::size(buffer)), ConfigPath().c_str());
    if (read == 0) return false;
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(buffer, &end);
    if (end == buffer) return false;
    *out = parsed;
    return true;
}

bool ReadIniBool(const wchar_t* section, const wchar_t* key, bool* out) noexcept
{
    if (out == nullptr) return false;
    const UINT value = GetPrivateProfileIntW(section, key, 0xFFFFFFFFu, ConfigPath().c_str());
    if (value == 0xFFFFFFFFu) return false;
    *out = value != 0;
    return true;
}

void WriteIniFloat(const wchar_t* section, const wchar_t* key, float value) noexcept
{
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"%.6f", value);
    WritePrivateProfileStringW(section, key, buffer, ConfigPath().c_str());
}

void WriteIniBool(const wchar_t* section, const wchar_t* key, bool value) noexcept
{
    WritePrivateProfileStringW(section, key, value ? L"1" : L"0", ConfigPath().c_str());
}

bool ReadIniInt(const wchar_t* section, const wchar_t* key, int* out) noexcept
{
    if (out == nullptr) return false;
    const UINT value = GetPrivateProfileIntW(section, key, 0x7FFFFFFFu, ConfigPath().c_str());
    if (value == 0x7FFFFFFFu) return false;
    *out = static_cast<int>(value);
    return true;
}

void WriteIniInt(const wchar_t* section, const wchar_t* key, int value) noexcept
{
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%d", value);
    WritePrivateProfileStringW(section, key, buffer, ConfigPath().c_str());
}

void LoadSettings() noexcept
{
    AKXr::DibrTuning t = {};
    AKXr::GetDibrTuning(&t);
    float f = 0.0f;
    bool b = false;
    int i = 0;
    if (ReadIniBool(L"DIBR", L"XREnabled", &b)) t.xrEnabled = b;
    if (ReadIniBool(L"DIBR", L"StereoOn", &b)) t.stereoOn = b;
    if (ReadIniBool(L"DIBR", L"DIBREnabled", &b)) t.dibrEnabled = b;
    if (ReadIniBool(L"DIBR", L"SameFrameStereo", &b)) t.sameFrameStereo = b;
    if (ReadIniBool(L"DIBR", L"DepthDebug", &b)) t.depthDebug = b;
    if (ReadIniBool(L"DIBR", L"SubmitHeadPose", &b)) t.submitHeadPose = b;
    if (ReadIniFloat(L"DIBR", L"Gain", &f)) t.gain = ClampFloat(f, 0.0f, 0.120f, t.gain);
    if (ReadIniFloat(L"DIBR", L"DepthScale", &f)) t.depthScale = ClampFloat(f, 16.0f, 192.0f, t.depthScale);
    if (ReadIniFloat(L"DIBR", L"EdgeGuard", &f)) t.edgeGuard = ClampFloat(f, 0.0f, 8.0f, t.edgeGuard);
    if (ReadIniFloat(L"DIBR", L"EdgeThreshold", &f)) t.edgeThreshold = ClampFloat(f, 0.010f, 0.300f, t.edgeThreshold);
    if (ReadIniFloat(L"DIBR", L"Convergence", &f)) t.convergence = ClampFloat(f, -0.080f, 0.080f, t.convergence);
    // AER
    if (ReadIniFloat(L"AER", L"HalfEye", &f))
    {
        // Migrate the obsolete MGS2-scaled default (200). Arkham is UE3 at roughly 50 uu/m,
        // so a physical 64 mm IPD is 3.2 uu total: 1.6 uu per eye.
        t.aerHalfEye = (std::isfinite(f) && f > 20.0f) ? 1.6f : ClampFloat(f, 0.0f, 20.0f, t.aerHalfEye);
    }
    if (ReadIniBool(L"AER", L"SwapEyes", &b)) t.aerSwapEyes = b;
    if (ReadIniInt(L"AER", L"DisplayHz", &i)) t.aerDisplayHz = i;
    if (ReadIniFloat(L"AER", L"FpScale", &f)) t.aerFpScale = ClampFloat(f, 0.0f, 1.0f, t.aerFpScale);
    // 6DOF head-look toggles
    if (ReadIniBool(L"SixDof", L"Gameplay", &b)) t.sixDofGameplay = b;
    if (ReadIniBool(L"SixDof", L"FirstPerson", &b)) t.sixDofFirstPerson = b;
    if (ReadIniBool(L"SixDof", L"Cutscene", &b)) t.sixDofCutscene = b;
    if (ReadIniFloat(L"Tracking", L"YawGain", &f)) t.headYawGain = ClampFloat(f, 0.25f, 2.0f, t.headYawGain);
    if (ReadIniFloat(L"Tracking", L"PitchGain", &f)) t.headPitchGain = ClampFloat(f, 0.25f, 2.0f, t.headPitchGain);
    if (ReadIniFloat(L"Tracking", L"Smoothing", &f)) t.headSmoothing = ClampFloat(f, 0.0f, 0.90f, t.headSmoothing);
    if (ReadIniBool(L"Tracking", L"InvertYaw", &b)) t.invertHeadYaw = b;
    if (ReadIniBool(L"Tracking", L"InvertPitch", &b)) t.invertHeadPitch = b;
    // Controls
    if (ReadIniInt(L"Controls", L"RecenterKey", &i) && i > 0 && i < 256) t.recenterKey = i;
    // First-person head-aim
    if (ReadIniBool(L"HeadAim", L"Enabled", &b)) t.headAimEnabled = b;
    if (ReadIniFloat(L"HeadAim", L"Gain", &f)) t.headAimGain = ClampFloat(f, 0.2f, 5.0f, t.headAimGain);
    if (ReadIniBool(L"FovFill", L"Enabled", &b)) t.fovFillEnabled = b;
    if (ReadIniFloat(L"FovFill", L"H", &f)) t.fovFillH = ClampFloat(f, 0.50f, 1.25f, t.fovFillH);
    if (ReadIniFloat(L"FovFill", L"V", &f)) t.fovFillV = ClampFloat(f, 0.50f, 1.25f, t.fovFillV);
    AKXr::SetDibrTuning(t);


    if (ReadIniFloat(L"Menu", L"X", &f)) g_place.x = f;
    if (ReadIniFloat(L"Menu", L"Y", &f)) g_place.y = f;
    if (ReadIniFloat(L"Menu", L"W", &f)) g_place.w = f;
    if (ReadIniFloat(L"Menu", L"H", &f)) g_place.h = f;
    if (ReadIniFloat(L"Menu", L"Alpha", &f)) g_place.alpha = ClampFloat(f, 0.35f, 0.95f, g_place.alpha);

    g_applyPlacement = true;
}

void SaveSettings() noexcept
{
    AKXr::DibrTuning t = {};
    AKXr::GetDibrTuning(&t);
    WriteIniBool(L"DIBR", L"XREnabled", t.xrEnabled);
    WriteIniBool(L"DIBR", L"StereoOn", t.stereoOn);
    WriteIniBool(L"DIBR", L"DIBREnabled", t.dibrEnabled);
    WriteIniBool(L"DIBR", L"SameFrameStereo", t.sameFrameStereo);
    WriteIniBool(L"DIBR", L"DepthDebug", t.depthDebug);
    WriteIniBool(L"DIBR", L"SubmitHeadPose", t.submitHeadPose);
    WriteIniFloat(L"DIBR", L"Gain", t.gain);
    WriteIniFloat(L"DIBR", L"DepthScale", t.depthScale);
    WriteIniFloat(L"DIBR", L"EdgeGuard", t.edgeGuard);
    WriteIniFloat(L"DIBR", L"EdgeThreshold", t.edgeThreshold);
    WriteIniFloat(L"DIBR", L"Convergence", t.convergence);
    WriteIniFloat(L"AER", L"HalfEye", t.aerHalfEye);
    WriteIniBool(L"AER", L"SwapEyes", t.aerSwapEyes);
    WriteIniInt(L"AER", L"DisplayHz", t.aerDisplayHz);
    WriteIniFloat(L"AER", L"FpScale", t.aerFpScale);
    WriteIniBool(L"SixDof", L"Gameplay", t.sixDofGameplay);
    WriteIniBool(L"SixDof", L"FirstPerson", t.sixDofFirstPerson);
    WriteIniBool(L"SixDof", L"Cutscene", t.sixDofCutscene);
    WriteIniFloat(L"Tracking", L"YawGain", t.headYawGain);
    WriteIniFloat(L"Tracking", L"PitchGain", t.headPitchGain);
    WriteIniFloat(L"Tracking", L"Smoothing", t.headSmoothing);
    WriteIniBool(L"Tracking", L"InvertYaw", t.invertHeadYaw);
    WriteIniBool(L"Tracking", L"InvertPitch", t.invertHeadPitch);
    WriteIniInt(L"Controls", L"RecenterKey", t.recenterKey);
    WriteIniBool(L"HeadAim", L"Enabled", t.headAimEnabled);
    WriteIniFloat(L"HeadAim", L"Gain", t.headAimGain);
    WriteIniBool(L"FovFill", L"Enabled", t.fovFillEnabled);
    WriteIniFloat(L"FovFill", L"H", t.fovFillH);
    WriteIniFloat(L"FovFill", L"V", t.fovFillV);
    WriteIniFloat(L"Menu", L"X", g_place.x);
    WriteIniFloat(L"Menu", L"Y", g_place.y);
    WriteIniFloat(L"Menu", L"W", g_place.w);
    WriteIniFloat(L"Menu", L"H", g_place.h);
    WriteIniFloat(L"Menu", L"Alpha", g_place.alpha);

    g_savedFrames = 120;
}

LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if (g_open)
    {
        switch (msg)
        {
        case WM_INPUT:
        case WM_MOUSEMOVE:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
            if (w != VK_INSERT) return 0;
            break;
        default:
            break;
        }
    }
    return g_originalWndProc != nullptr ? CallWindowProcW(g_originalWndProc, hwnd, msg, w, l) : DefWindowProcW(hwnd, msg, w, l);
}

void SetGameWindow(HWND hwnd) noexcept
{
    if (hwnd == nullptr || hwnd == g_hwnd) return;
    if (g_hwnd != nullptr && g_originalWndProc != nullptr)
    {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
    }
    g_hwnd = hwnd;
    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MenuWndProc)));
}

// Anchor the real cursor where it currently is; from here on only the virtual cursor moves.
void SeedVirtualMouse() noexcept
{
    g_mouseAnchorValid = false;
    POINT pt = {};
    if (!GetCursorPos(&pt)) return;

    HWND hwnd = g_hwnd != nullptr ? g_hwnd : GetForegroundWindow();
    RECT rc = {};
    POINT client = pt;
    if (hwnd != nullptr && ScreenToClient(hwnd, &client) && GetClientRect(hwnd, &rc) &&
        rc.right > 0 && rc.bottom > 0)
    {
        g_virtualMouseX = ClampFloat(static_cast<float>(client.x) / static_cast<float>(rc.right) *
                                         static_cast<float>(g_targetW),
                                     0.0f, static_cast<float>(g_targetW - 1),
                                     static_cast<float>(g_targetW) * 0.5f);
        g_virtualMouseY = ClampFloat(static_cast<float>(client.y) / static_cast<float>(rc.bottom) *
                                         static_cast<float>(g_targetH),
                                     0.0f, static_cast<float>(g_targetH - 1),
                                     static_cast<float>(g_targetH) * 0.5f);
    }
    else
    {
        g_virtualMouseX = static_cast<float>(g_targetW) * 0.5f;
        g_virtualMouseY = static_cast<float>(g_targetH) * 0.5f;
    }

    g_mouseAnchorScreen = pt;
    g_mouseAnchorValid = true;
    ClipCursor(nullptr);
    ShowCursor(TRUE);
}

void PollInsert() noexcept
{
    // ME1's proven input rule: read the pressed-since-last-call bit exactly once
    // per Present. This catches short physical and Steam-keyboard Insert taps.
    const bool pressed = (GetAsyncKeyState(VK_INSERT) & 0x0001) != 0;
    if (!pressed || (g_hwnd != nullptr && GetForegroundWindow() != g_hwnd)) return;

    g_open = !g_open;
    if (g_open)
    {
        LoadSettings();
        g_applyPlacement = true;
        SeedVirtualMouse();
    }
    else
    {
        SaveSettings();
        g_mouseAnchorValid = false;
    }
}

// --- overlay texture + composite pipeline ----------------------------------------------------

bool EnsureBlitPipeline() noexcept
{
    if (g_blitVs != nullptr && g_blitPs != nullptr && g_blitSampler != nullptr &&
        g_blitBlend != nullptr && g_blitDepth != nullptr && g_blitRaster != nullptr)
        return true;
    if (g_device == nullptr) return false;

    using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                          ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                          ID3DBlob**, ID3DBlob**);
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (compiler == nullptr) compiler = LoadLibraryW(L"d3dcompiler_43.dll");
    if (compiler == nullptr) return false;
    auto compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile"));
    if (compile == nullptr)
    {
        FreeLibrary(compiler);
        return false;
    }

    static constexpr char kVs[] = R"(
struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
})";

    static constexpr char kPs[] = R"(
Texture2D overlay : register(t0);
SamplerState linearClamp : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    return overlay.Sample(linearClamp, uv);
})";

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(kVs, sizeof(kVs) - 1, "AKMenuVS", nullptr, nullptr, "main", "vs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errors);
    SafeRelease(errors);
    if (SUCCEEDED(hr) && vsBlob != nullptr)
    {
        hr = compile(kPs, sizeof(kPs) - 1, "AKMenuPS", nullptr, nullptr, "main", "ps_5_0",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errors);
        SafeRelease(errors);
    }
    if (SUCCEEDED(hr) && vsBlob != nullptr && psBlob != nullptr)
    {
        hr = g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                          nullptr, &g_blitVs);
        if (SUCCEEDED(hr))
            hr = g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                             nullptr, &g_blitPs);
    }
    SafeRelease(vsBlob);
    SafeRelease(psBlob);
    FreeLibrary(compiler);
    if (FAILED(hr) || g_blitVs == nullptr || g_blitPs == nullptr) return false;

    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    hr = g_device->CreateSamplerState(&sampler, &g_blitSampler);

    // ImGui draws into a transparent-black target with SrcAlpha/InvSrcAlpha, so the colour it
    // leaves behind is already multiplied by its own alpha. Compositing that back out is therefore
    // a PREMULTIPLIED over: ONE / INV_SRC_ALPHA. Using SRC_ALPHA here would darken the panel.
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = g_device->CreateBlendState(&blend, &g_blitBlend);

    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = FALSE;
    depth.StencilEnable = FALSE;
    if (SUCCEEDED(hr)) hr = g_device->CreateDepthStencilState(&depth, &g_blitDepth);

    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = g_device->CreateRasterizerState(&raster, &g_blitRaster);

    return SUCCEEDED(hr) && g_blitSampler != nullptr && g_blitBlend != nullptr &&
           g_blitDepth != nullptr && g_blitRaster != nullptr;
}

bool EnsureOverlayTexture(UINT width, UINT height) noexcept
{
    if (g_device == nullptr || width == 0 || height == 0) return false;
    if (g_overlayTex != nullptr && width == g_overlayW && height == g_overlayH) return true;

    SafeRelease(g_overlaySrv);
    SafeRelease(g_overlayRtv);
    SafeRelease(g_overlayTex);
    g_overlayW = 0;
    g_overlayH = 0;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_overlayTex)) || g_overlayTex == nullptr)
        return false;
    if (FAILED(g_device->CreateRenderTargetView(g_overlayTex, nullptr, &g_overlayRtv)) ||
        g_overlayRtv == nullptr ||
        FAILED(g_device->CreateShaderResourceView(g_overlayTex, nullptr, &g_overlaySrv)) ||
        g_overlaySrv == nullptr)
    {
        SafeRelease(g_overlaySrv);
        SafeRelease(g_overlayRtv);
        SafeRelease(g_overlayTex);
        return false;
    }

    g_overlayW = width;
    g_overlayH = height;
    return true;
}

// Draw the finished overlay over an arbitrary render target, saving and restoring every pipeline
// slot it touches. The callers are the game's Present path and the XR eye path.
bool BlitOverlay(ID3D11RenderTargetView* rtv, UINT width, UINT height, int offsetX) noexcept
{
    if (!g_overlayValid || rtv == nullptr || g_context == nullptr || g_overlaySrv == nullptr) return false;
    if (!EnsureBlitPipeline()) return false;

    ID3D11InputLayout* oldLayout = nullptr;
    ID3D11Buffer* oldVertexBuffer = nullptr;
    UINT oldStride = 0, oldOffset = 0;
    ID3D11Buffer* oldIndexBuffer = nullptr;
    DXGI_FORMAT oldIndexFormat = DXGI_FORMAT_UNKNOWN;
    UINT oldIndexOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY oldTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* oldVs = nullptr;
    ID3D11HullShader* oldHs = nullptr;
    ID3D11DomainShader* oldDs = nullptr;
    ID3D11GeometryShader* oldGs = nullptr;
    ID3D11PixelShader* oldPs = nullptr;
    ID3D11ShaderResourceView* oldSrv = nullptr;
    ID3D11SamplerState* oldSampler = nullptr;
    ID3D11RenderTargetView* oldRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    ID3D11RasterizerState* oldRaster = nullptr;
    ID3D11BlendState* oldBlend = nullptr;
    FLOAT oldBlendFactor[4] = {};
    UINT oldSampleMask = 0;
    ID3D11DepthStencilState* oldDepth = nullptr;
    UINT oldStencilRef = 0;
    D3D11_VIEWPORT oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT oldViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT oldScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT oldScissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    g_context->IAGetInputLayout(&oldLayout);
    g_context->IAGetVertexBuffers(0, 1, &oldVertexBuffer, &oldStride, &oldOffset);
    g_context->IAGetIndexBuffer(&oldIndexBuffer, &oldIndexFormat, &oldIndexOffset);
    g_context->IAGetPrimitiveTopology(&oldTopology);
    g_context->VSGetShader(&oldVs, nullptr, nullptr);
    g_context->HSGetShader(&oldHs, nullptr, nullptr);
    g_context->DSGetShader(&oldDs, nullptr, nullptr);
    g_context->GSGetShader(&oldGs, nullptr, nullptr);
    g_context->PSGetShader(&oldPs, nullptr, nullptr);
    g_context->PSGetShaderResources(0, 1, &oldSrv);
    g_context->PSGetSamplers(0, 1, &oldSampler);
    g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, &oldDsv);
    g_context->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
    g_context->OMGetDepthStencilState(&oldDepth, &oldStencilRef);
    g_context->RSGetState(&oldRaster);
    g_context->RSGetViewports(&oldViewportCount, oldViewports);
    g_context->RSGetScissorRects(&oldScissorCount, oldScissors);

    // A negative/positive TopLeftX slides the full-screen triangle sideways, which is exactly the
    // per-eye shift the XR path needs to keep the panel at zero disparity under a convergence crop.
    D3D11_VIEWPORT viewport = {static_cast<float>(offsetX), 0.0f, static_cast<float>(width),
                               static_cast<float>(height), 0.0f, 1.0f};
    const FLOAT blendFactor[4] = {};
    UINT zero = 0;
    ID3D11Buffer* noBuffer = nullptr;
    ID3D11ShaderResourceView* noSrv = nullptr;

    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    g_context->OMSetBlendState(g_blitBlend, blendFactor, 0xffffffffu);
    g_context->OMSetDepthStencilState(g_blitDepth, 0);
    g_context->RSSetState(g_blitRaster);
    g_context->RSSetViewports(1, &viewport);
    g_context->RSSetScissorRects(0, nullptr);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetVertexBuffers(0, 1, &noBuffer, &zero, &zero);
    g_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_blitVs, nullptr, 0);
    g_context->HSSetShader(nullptr, nullptr, 0);
    g_context->DSSetShader(nullptr, nullptr, 0);
    g_context->GSSetShader(nullptr, nullptr, 0);
    g_context->PSSetShader(g_blitPs, nullptr, 0);
    g_context->PSSetShaderResources(0, 1, &g_overlaySrv);
    g_context->PSSetSamplers(0, 1, &g_blitSampler);
    g_context->Draw(3, 0);

    g_context->PSSetShaderResources(0, 1, &noSrv);
    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, oldDsv);
    g_context->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
    g_context->OMSetDepthStencilState(oldDepth, oldStencilRef);
    g_context->RSSetState(oldRaster);
    if (oldViewportCount > 0) g_context->RSSetViewports(oldViewportCount, oldViewports);
    if (oldScissorCount > 0) g_context->RSSetScissorRects(oldScissorCount, oldScissors);
    g_context->IASetInputLayout(oldLayout);
    g_context->IASetVertexBuffers(0, 1, &oldVertexBuffer, &oldStride, &oldOffset);
    g_context->IASetIndexBuffer(oldIndexBuffer, oldIndexFormat, oldIndexOffset);
    g_context->IASetPrimitiveTopology(oldTopology);
    g_context->VSSetShader(oldVs, nullptr, 0);
    g_context->HSSetShader(oldHs, nullptr, 0);
    g_context->DSSetShader(oldDs, nullptr, 0);
    g_context->GSSetShader(oldGs, nullptr, 0);
    g_context->PSSetShader(oldPs, nullptr, 0);
    g_context->PSSetShaderResources(0, 1, &oldSrv);
    g_context->PSSetSamplers(0, 1, &oldSampler);

    SafeRelease(oldLayout);
    SafeRelease(oldVertexBuffer);
    SafeRelease(oldIndexBuffer);
    SafeRelease(oldVs);
    SafeRelease(oldHs);
    SafeRelease(oldDs);
    SafeRelease(oldGs);
    SafeRelease(oldPs);
    SafeRelease(oldSrv);
    SafeRelease(oldSampler);
    for (auto& old : oldRtvs) SafeRelease(old);
    SafeRelease(oldDsv);
    SafeRelease(oldRaster);
    SafeRelease(oldBlend);
    SafeRelease(oldDepth);
    return true;
}

bool EnsureReady(IDXGISwapChain* swapChain) noexcept
{
    if (g_ready) return true;
    if (swapChain == nullptr) return false;

    ID3D11Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || device == nullptr) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (context == nullptr)
    {
        device->Release();
        return false;
    }

    g_device = device;
    g_context = context;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.MouseDrawCursor = true;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.ScaleAllSizes(1.55f);
    io.FontGlobalScale = 1.55f;

    if (!ImGui_ImplDX11_Init(g_device, g_context))
    {
        ImGui::DestroyContext();
        SafeRelease(g_context);
        SafeRelease(g_device);
        return false;
    }

    LoadSettings();
    g_ready = true;
    return true;
}

void FeedInput() noexcept
{
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = true;
    ClipCursor(nullptr);

    if (!g_mouseAnchorValid) SeedVirtualMouse();

    POINT pt = {};
    if (GetCursorPos(&pt) && g_mouseAnchorValid)
    {
        const LONG dx = pt.x - g_mouseAnchorScreen.x;
        const LONG dy = pt.y - g_mouseAnchorScreen.y;
        g_virtualMouseX = ClampFloat(g_virtualMouseX + static_cast<float>(dx), 0.0f,
                                     static_cast<float>(g_targetW - 1), static_cast<float>(g_targetW) * 0.5f);
        g_virtualMouseY = ClampFloat(g_virtualMouseY + static_cast<float>(dy), 0.0f,
                                     static_cast<float>(g_targetH - 1), static_cast<float>(g_targetH) * 0.5f);

        // The point of the whole exercise: the game can poll the OS cursor all it likes, but while
        // the menu is open the real cursor never leaves the anchor — only the menu's virtual cursor moves.
        // Swallowing window messages alone does not do this; Arkham does not read the mouse that way.
        SetCursorPos(g_mouseAnchorScreen.x, g_mouseAnchorScreen.y);
    }

    io.AddMousePosEvent(g_virtualMouseX, g_virtualMouseY);
    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
}

float ControlWidth() noexcept
{
    return (std::min)(760.0f, (std::max)(360.0f, static_cast<float>(g_targetW) * 0.28f));
}

void ClampPlacement() noexcept
{
    const float minW = 760.0f;
    const float minH = 520.0f;
    const float maxW = (std::max)(minW, static_cast<float>(g_targetW) - 20.0f);
    const float maxH = (std::max)(minH, static_cast<float>(g_targetH) - 20.0f);
    g_place.w = ClampFloat(g_place.w, minW, maxW, minW);
    g_place.h = ClampFloat(g_place.h, minH, maxH, minH);
    g_place.x = ClampFloat(g_place.x, 0.0f, (std::max)(0.0f, static_cast<float>(g_targetW) - g_place.w), 80.0f);
    g_place.y = ClampFloat(g_place.y, 0.0f, (std::max)(0.0f, static_cast<float>(g_targetH) - g_place.h), 80.0f);
}

void DrawDibrTab() noexcept
{
    AKXr::DibrTuning t = {};
    AKXr::GetDibrTuning(&t);
    bool changed = false;

    ImGui::TextDisabled("DIBR params (active when Stereo mode = DIBR)");
    ImGui::SetNextItemWidth(ControlWidth());
    changed |= ImGui::SliderFloat("Depth gain", &t.gain, 0.0f, 0.120f, "%.3f");
    ImGui::SetNextItemWidth(ControlWidth());
    changed |= ImGui::SliderFloat("Depth scale", &t.depthScale, 16.0f, 192.0f, "%.1f");
    ImGui::SetNextItemWidth(ControlWidth());
    changed |= ImGui::SliderFloat("Edge ghost guard", &t.edgeGuard, 0.0f, 8.0f, "%.2f");
    ImGui::SetNextItemWidth(ControlWidth());
    changed |= ImGui::SliderFloat("Edge threshold", &t.edgeThreshold, 0.010f, 0.300f, "%.3f");

    if (ImGui::Button("Good depth baseline"))
    {
        t.gain = 0.035f;
        t.depthScale = 96.0f;
        t.edgeGuard = 3.0f;
        t.edgeThreshold = 0.080f;
        t.dibrEnabled = true;
        t.stereoOn = true;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Flat"))
    {
        t.gain = 0.0f;
        changed = true;
    }

    if (changed) AKXr::SetDibrTuning(t);
}

void DrawRuntimeTab() noexcept
{
    AKXr::DibrTuning t = {};
    AKXr::GetDibrTuning(&t);
    bool changed = false;

    changed |= ImGui::Checkbox("OpenXR submit", &t.xrEnabled);
    changed |= ImGui::Checkbox("Depth map view", &t.depthDebug);
    changed |= ImGui::Checkbox("Submit live head pose", &t.submitHeadPose);
    ImGui::TextDisabled("(AER convergence moved to the AER tab — it's image-shift now, not FOV shear.)");
    if (changed) AKXr::SetDibrTuning(t);

    ImGui::Separator();
    ImGui::Text("XR running: %s", AKXr::IsRunning() ? "yes" : "no");
    ImGui::Text("Config: %ls", ConfigPath().c_str());
}

void DrawCameraTab() noexcept
{
    AKXr::DibrTuning t = {};
    AKXr::GetDibrTuning(&t);
    bool changed = false;

    ImGui::TextUnformatted("Head tracking");
    bool tracking = t.sixDofGameplay;
    if (ImGui::Checkbox("Enable gameplay head tracking", &tracking))
    {
        t.sixDofGameplay = tracking;
        t.submitHeadPose = tracking;
        changed = true;
    }
    ImGui::TextDisabled("Rotational tracking (yaw/pitch). Camera and OpenXR pose stay matched.");

    ImGui::SetNextItemWidth(ControlWidth());
    changed |= ImGui::SliderFloat("Yaw gain", &t.headYawGain, 0.25f, 2.0f, "%.2fx");
    ImGui::SetNextItemWidth(ControlWidth());
    changed |= ImGui::SliderFloat("Pitch gain", &t.headPitchGain, 0.25f, 2.0f, "%.2fx");
    changed |= ImGui::Checkbox("Invert yaw", &t.invertHeadYaw);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Invert pitch", &t.invertHeadPitch);

    ImGui::SetNextItemWidth(ControlWidth());
    changed |= ImGui::SliderFloat("Head smoothing", &t.headSmoothing, 0.0f, 0.90f, "%.2f");
    ImGui::TextDisabled("0 = raw/lowest latency. Higher reduces camera jitter but adds lag. Start at 0.10.");

    if (ImGui::Button("Tracking defaults"))
    {
        t.headYawGain = 1.0f;
        t.headPitchGain = 1.0f;
        t.headSmoothing = 0.0f;
        t.invertHeadYaw = false;
        t.invertHeadPitch = false;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Recenter now (yaw + pitch)")) AKXr::Recenter();

    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    if (AKXr::GetLiveHeadYawPitchDeg(&yaw, &pitch, &roll))
        ImGui::Text("HMD (live): yaw %.1f  pitch %.1f  roll %.1f deg", yaw, pitch, roll);
    else
        ImGui::TextDisabled("HMD pose not available yet");
    ImGui::TextDisabled("The game camera is held still while this panel is open; the HMD readout stays live.");

    if (changed) AKXr::SetDibrTuning(t);
}

void DrawComfortTab() noexcept
{
    AKXr::DibrTuning t = {};
    AKXr::GetDibrTuning(&t);
    bool changed = false;

    ImGui::TextWrapped("While this panel is open the game camera is frozen and the mouse is captured, so "
                       "nothing moves under you while you tune. Stereo keeps running, so IPD and "
                       "convergence changes are visible immediately.");
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Unsafe FOV fill (advanced)"))
    {
        changed |= ImGui::Checkbox("Enable post-cull FOV fill", &t.fovFillEnabled);
        ImGui::SetNextItemWidth(ControlWidth());
        changed |= ImGui::SliderFloat("VR Fill H", &t.fovFillH, 0.50f, 1.25f, "%.2f");
        ImGui::SetNextItemWidth(ControlWidth());
        changed |= ImGui::SliderFloat("VR Fill V", &t.fovFillV, 0.50f, 1.25f, "%.2f");
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
                           "Keep this OFF: Arkham culls before widening and objects can flicker/pop.");
    }

    if (changed) AKXr::SetDibrTuning(t);
}
void DrawProfilesTab() noexcept
{
    if (ImGui::Button("Save"))
    {
        SaveSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload"))
    {
        LoadSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset tuning"))
    {
        AKXr::ResetDibrTuning();
    }

    if (g_savedFrames > 0)
    {
        --g_savedFrames;
        ImGui::SameLine();
        ImGui::TextDisabled("saved");
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(ControlWidth());
    ImGui::SliderFloat("Panel opacity", &g_place.alpha, 0.35f, 0.95f, "%.2f");
    if (ImGui::Button("Center panel"))
    {
        g_place.w = (std::min)(1480.0f, static_cast<float>(g_targetW) * 0.86f);
        g_place.h = (std::min)(920.0f, static_cast<float>(g_targetH) * 0.78f);
        g_place.x = (static_cast<float>(g_targetW) - g_place.w) * 0.5f;
        g_place.y = (static_cast<float>(g_targetH) - g_place.h) * 0.5f;
        g_applyPlacement = true;
    }
}

void DrawAerTab() noexcept
{
    AKXr::DibrTuning t = {};
    AKXr::GetDibrTuning(&t);
    const bool geometricActive = t.stereoOn && !t.dibrEnabled;
    bool changed = false;

    if (t.sameFrameStereo && geometricActive)
        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), "TRUE SAME-FRAME STEREO ACTIVE");
    else if (geometricActive)
        ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.30f, 1.0f), "ALTERNATE-EYE STEREO ACTIVE");
    else
        ImGui::TextDisabled("Geometric stereo is not active; select Same-frame at the top.");
    ImGui::Separator();

    // ME1's control, in raw game units. The millimetre slider and the "perceived world scale"
    // slider that used to sit here were two views of this same number, converted through an
    // ASSUMED 50 uu/m that nobody ever measured on Arkham (docs/TECHNICAL_NOTES.md: ~50 uu/m, as in other UE3 games).
    // The conversion capped this slider at 2.5 uu, which is at or below a natural separation for
    // this game -- so raising it never shrank the world, because the whole range sat below the
    // part where anything happens. Raw uu, ME1's range, find the value by eye.
    ImGui::SetNextItemWidth(ControlWidth());
    changed |= ImGui::SliderFloat("Half eye separation", &t.aerHalfEye, 0.0f, 16.0f, "%.3f uu");
    ImGui::TextDisabled("Larger = more parallax = the world reads SMALLER and closer. 0 = flat.");
    ImGui::Text("Total separation: %.3f uu between the eyes", t.aerHalfEye * 2.0f);

    if (ImGui::Button("Flat")) { t.aerHalfEye = 0.0f; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("1.6")) { t.aerHalfEye = 1.6f; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("3.2")) { t.aerHalfEye = 3.2f; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("6.4")) { t.aerHalfEye = 6.4f; changed = true; }
    ImGui::SameLine();
    ImGui::TextDisabled("coarse steps - whichever looks 1:1 reveals Arkham's real world scale");

    ImGui::Separator();
    changed |= ImGui::Checkbox("Swap eyes (inverts depth)", &t.aerSwapEyes);

    ImGui::Separator();
    // The submit-side clamp is W/12 = 8.33% of width, so expose the whole usable range. The old
    // +/-3% slider could not reach the values Arkham's near-camera framing actually needs.
    float convergencePct = t.convergence * 100.0f;
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::SliderFloat("Convergence shift", &convergencePct, -8.0f, 8.0f, "%+.2f%%"))
    {
        t.convergence = convergencePct * 0.01f;
        changed = true;
    }
    ImGui::TextDisabled("%s", "Equal opposite image crops; no FOV shear. Positive pulls the fusion plane nearer.");
    ImGui::TextDisabled("%s", "Tuning is LIVE: stereo keeps rendering behind this panel, so you see each change as you drag.");
    if (ImGui::Button("Reset convergence")) { t.convergence = 0.0f; changed = true; }

    if (!t.sameFrameStereo && ImGui::CollapsingHeader("Advanced (alternate-eye only)"))
    {
        const char* hzItems[] = {"Auto-detect", "60", "72", "90", "120", "144"};
        const int hzVals[] = {0, 60, 72, 90, 120, 144};
        int hzIdx = 0;
        for (int i = 0; i < 6; ++i) if (hzVals[i] == t.aerDisplayHz) hzIdx = i;
        ImGui::SetNextItemWidth(ControlWidth());
        if (ImGui::Combo("AER headset Hz", &hzIdx, hzItems, 6)) { t.aerDisplayHz = hzVals[hzIdx]; changed = true; }
    }

    if (changed) AKXr::SetDibrTuning(t);
}
void BuildUI() noexcept
{
    ClampPlacement();
    if (g_applyPlacement)
    {
        // Center the window on the display each time it opens (user asked for it in the middle).
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        if (disp.x > 1.0f && disp.y > 1.0f)
        {
            g_place.x = (disp.x - g_place.w) * 0.5f;
            g_place.y = (disp.y - g_place.h) * 0.5f;
            if (g_place.x < 0.0f) g_place.x = 0.0f;
            if (g_place.y < 0.0f) g_place.y = 0.0f;
        }
        ImGui::SetNextWindowPos(ImVec2(g_place.x, g_place.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(g_place.w, g_place.h), ImGuiCond_Always);
        g_applyPlacement = false;
    }
    else
    {
        ImGui::SetNextWindowPos(ImVec2(g_place.x, g_place.y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(g_place.w, g_place.h), ImGuiCond_FirstUseEver);
    }

    ImGui::SetNextWindowBgAlpha(g_place.alpha);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("AK VR", &g_open, flags))
    {
        g_place.x = ImGui::GetWindowPos().x;
        g_place.y = ImGui::GetWindowPos().y;
        g_place.w = ImGui::GetWindowSize().x;
        g_place.h = ImGui::GetWindowSize().y;

        AKXr::DibrTuning master = {};
        AKXr::GetDibrTuning(&master);
        bool masterChanged = ImGui::Checkbox("VR ENABLED", &master.xrEnabled);
        ImGui::SameLine();
        ImGui::TextDisabled("Insert opens/closes this panel; settings auto-save when it closes.");

        int mode = !master.stereoOn ? 0 : (master.sameFrameStereo ? 3 : (master.dibrEnabled ? 1 : 2));
        const int previousMode = mode;
        ImGui::TextUnformatted("Stereo mode:");
        ImGui::SameLine(); ImGui::RadioButton("Mono", &mode, 0);
        ImGui::SameLine(); ImGui::RadioButton("DIBR", &mode, 1);
        ImGui::SameLine(); ImGui::RadioButton("AER", &mode, 2);
        ImGui::SameLine(); ImGui::RadioButton("Same-frame (verified)", &mode, 3);
        if (mode != previousMode)
        {
            master.stereoOn = (mode != 0);
            master.dibrEnabled = (mode == 1);
            master.sameFrameStereo = (mode == 3);
            masterChanged = true;
        }
        if (masterChanged) AKXr::SetDibrTuning(master);
        ImGui::Separator();
        if (ImGui::BeginTabBar("ak_vr_tabs"))
        {
            if (ImGui::BeginTabItem("Stereo Tuning"))
            {
                DrawAerTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Tracking"))
            {
                DrawCameraTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Comfort"))
            {
                DrawComfortTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Runtime"))
            {
                DrawRuntimeTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("DIBR Advanced"))
            {
                DrawDibrTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Profiles"))
            {
                DrawProfilesTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

}  // namespace

void Update(IDXGISwapChain* swapChain) noexcept
{
    g_overlayValid = false;
    if (swapChain == nullptr) return;

    DXGI_SWAP_CHAIN_DESC swapDesc = {};
    if (SUCCEEDED(swapChain->GetDesc(&swapDesc))) SetGameWindow(swapDesc.OutputWindow);

    // One Insert poll and one complete ImGui frame from Present, matching ME1.
    PollInsert();
    if (!g_open || !EnsureReady(swapChain)) return;

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&backBuffer))) ||
        backBuffer == nullptr)
    {
        return;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    backBuffer->GetDesc(&desc);
    backBuffer->Release();

    g_targetW = static_cast<int>(desc.Width);
    g_targetH = static_cast<int>(desc.Height);
    if (!EnsureOverlayTexture(desc.Width, desc.Height)) return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_targetW), static_cast<float>(g_targetH));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = 1.0f / 60.0f;
    FeedInput();

    const bool wasOpen = g_open;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    BuildUI();
    ImGui::Render();

    // Render the panel into the offscreen overlay, not the backbuffer. Cleared to transparent
    // black, so what lands here is the panel over nothing — compositable into any target.
    ID3D11RenderTargetView* previousRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* previousDsv = nullptr;
    g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, previousRtvs, &previousDsv);
    UINT previousViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT previousViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    g_context->RSGetViewports(&previousViewportCount, previousViewports);

    const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    g_context->OMSetRenderTargets(1, &g_overlayRtv, nullptr);
    g_context->ClearRenderTargetView(g_overlayRtv, transparent);
    D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(g_targetW),
                               static_cast<float>(g_targetH), 0.0f, 1.0f};
    g_context->RSSetViewports(1, &viewport);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, previousRtvs, previousDsv);
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) SafeRelease(previousRtvs[i]);
    SafeRelease(previousDsv);
    if (previousViewportCount > 0) g_context->RSSetViewports(previousViewportCount, previousViewports);

    g_overlayValid = true;
    if (wasOpen && !g_open) SaveSettings();
}

bool BlitToRenderTarget(ID3D11RenderTargetView* rtv, unsigned width, unsigned height,
                        int offsetX) noexcept
{
    return BlitOverlay(rtv, static_cast<UINT>(width), static_cast<UINT>(height), offsetX);
}

void BlitToBackbuffer(IDXGISwapChain* swapChain) noexcept
{
    if (!g_overlayValid || swapChain == nullptr || g_device == nullptr) return;

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&backBuffer))) ||
        backBuffer == nullptr)
    {
        return;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    backBuffer->GetDesc(&desc);
    ID3D11RenderTargetView* rtv = nullptr;
    const HRESULT hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
    backBuffer->Release();
    if (FAILED(hr) || rtv == nullptr) return;

    BlitOverlay(rtv, desc.Width, desc.Height, 0);
    rtv->Release();
}

bool IsOpen() noexcept
{
    return g_open;
}

void Shutdown() noexcept
{
    if (g_hwnd != nullptr && g_originalWndProc != nullptr)
    {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
        g_originalWndProc = nullptr;
        g_hwnd = nullptr;
    }
    if (g_ready)
    {
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
        g_ready = false;
    }
    SafeRelease(g_overlaySrv);
    SafeRelease(g_overlayRtv);
    SafeRelease(g_overlayTex);
    SafeRelease(g_blitVs);
    SafeRelease(g_blitPs);
    SafeRelease(g_blitSampler);
    SafeRelease(g_blitBlend);
    SafeRelease(g_blitDepth);
    SafeRelease(g_blitRaster);
    SafeRelease(g_context);
    SafeRelease(g_device);
}
}  // namespace AKMenu
