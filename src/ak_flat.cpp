#include "ak_flat.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <atomic>
#include <mutex>

void AKProbeLog(const char* format, ...) noexcept;

namespace
{
template <typename T>
void SafeRelease(T*& value) noexcept
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}

std::mutex g_mutex;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11Texture2D* g_eyeTexture[2] = {};
ID3D11ShaderResourceView* g_eyeSrv[2] = {};
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11RasterizerState* g_rasterizer = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
UINT g_width = 0;
UINT g_height = 0;
std::atomic<unsigned> g_readyMask{0};
std::atomic<unsigned long long> g_composedFrames{0};
std::atomic<bool> g_ready{false};
const char* g_status = "waiting for first Present";

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                      ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob**, ID3DBlob**);

void ReleaseSizeResources() noexcept
{
    for (int eye = 0; eye < 2; ++eye)
    {
        SafeRelease(g_eyeSrv[eye]);
        SafeRelease(g_eyeTexture[eye]);
    }
    g_readyMask.store(0, std::memory_order_release);
    g_ready.store(false, std::memory_order_release);
    g_width = 0;
    g_height = 0;
    g_format = DXGI_FORMAT_UNKNOWN;
}

bool EnsureDevice(IDXGISwapChain* swapChain) noexcept
{
    if (swapChain == nullptr) return false;

    if (g_swapChain != swapChain)
    {
        ReleaseSizeResources();
        SafeRelease(g_context);
        SafeRelease(g_device);
        SafeRelease(g_swapChain);
        g_swapChain = swapChain;
        g_swapChain->AddRef();
    }

    if (g_device == nullptr)
    {
        if (FAILED(g_swapChain->GetDevice(__uuidof(ID3D11Device),
                                          reinterpret_cast<void**>(&g_device))) ||
            g_device == nullptr)
        {
            g_status = "GetDevice failed";
            return false;
        }
        g_device->GetImmediateContext(&g_context);
        if (g_context == nullptr)
        {
            g_status = "GetImmediateContext failed";
            return false;
        }
    }
    return true;
}

bool EnsureShaders() noexcept
{
    if (g_vs != nullptr && g_ps != nullptr && g_sampler != nullptr &&
        g_rasterizer != nullptr && g_blend != nullptr && g_depth != nullptr)
        return true;
    if (g_device == nullptr) return false;

    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (compiler == nullptr)
    {
        g_status = "d3dcompiler_47.dll unavailable";
        return false;
    }
    auto compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile"));
    if (compile == nullptr)
    {
        g_status = "D3DCompile unavailable";
        FreeLibrary(compiler);
        return false;
    }

    static constexpr char kVsSource[] = R"(
struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
})";

    static constexpr char kPsSource[] = R"(
Texture2D leftEye : register(t0);
Texture2D rightEye : register(t1);
SamplerState linearClamp : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    if (uv.x < 0.5)
        return leftEye.Sample(linearClamp, float2(uv.x * 2.0, uv.y));
    return rightEye.Sample(linearClamp, float2((uv.x - 0.5) * 2.0, uv.y));
})";

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(kVsSource, sizeof(kVsSource) - 1, "AKFlatVS", nullptr, nullptr,
                         "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errors);
    SafeRelease(errors);
    if (FAILED(hr) || vsBlob == nullptr)
    {
        g_status = "flat VS compile failed";
        SafeRelease(vsBlob);
        FreeLibrary(compiler);
        return false;
    }

    hr = compile(kPsSource, sizeof(kPsSource) - 1, "AKFlatPS", nullptr, nullptr,
                 "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errors);
    SafeRelease(errors);
    if (FAILED(hr) || psBlob == nullptr)
    {
        g_status = "flat PS compile failed";
        SafeRelease(vsBlob);
        SafeRelease(psBlob);
        FreeLibrary(compiler);
        return false;
    }

    hr = g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                       nullptr, &g_vs);
    if (SUCCEEDED(hr))
        hr = g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                         nullptr, &g_ps);
    SafeRelease(vsBlob);
    SafeRelease(psBlob);
    FreeLibrary(compiler);
    if (FAILED(hr) || g_vs == nullptr || g_ps == nullptr)
    {
        g_status = "flat shader creation failed";
        return false;
    }

    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    hr = g_device->CreateSamplerState(&sampler, &g_sampler);

    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = g_device->CreateRasterizerState(&raster, &g_rasterizer);

    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = g_device->CreateBlendState(&blend, &g_blend);

    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = FALSE;
    depth.StencilEnable = FALSE;
    if (SUCCEEDED(hr)) hr = g_device->CreateDepthStencilState(&depth, &g_depth);

    if (FAILED(hr) || g_sampler == nullptr || g_rasterizer == nullptr ||
        g_blend == nullptr || g_depth == nullptr)
    {
        g_status = "flat fixed-state creation failed";
        return false;
    }
    return true;
}

bool EnsureEyeResources() noexcept
{
    if (g_swapChain == nullptr || g_device == nullptr) return false;

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                      reinterpret_cast<void**>(&backBuffer))) ||
        backBuffer == nullptr)
    {
        g_status = "GetBuffer failed";
        return false;
    }

    D3D11_TEXTURE2D_DESC source = {};
    backBuffer->GetDesc(&source);
    backBuffer->Release();

    if (source.Width == 0 || source.Height == 0 || source.SampleDesc.Count != 1)
    {
        g_status = "unsupported backbuffer (zero size or MSAA)";
        return false;
    }

    if (g_eyeTexture[0] != nullptr && g_eyeTexture[1] != nullptr &&
        source.Width == g_width && source.Height == g_height && source.Format == g_format)
        return EnsureShaders();

    ReleaseSizeResources();

    D3D11_TEXTURE2D_DESC eye = source;
    eye.MipLevels = 1;
    eye.ArraySize = 1;
    eye.Usage = D3D11_USAGE_DEFAULT;
    eye.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    eye.CPUAccessFlags = 0;
    eye.MiscFlags = 0;

    for (int i = 0; i < 2; ++i)
    {
        if (FAILED(g_device->CreateTexture2D(&eye, nullptr, &g_eyeTexture[i])) ||
            g_eyeTexture[i] == nullptr ||
            FAILED(g_device->CreateShaderResourceView(g_eyeTexture[i], nullptr, &g_eyeSrv[i])) ||
            g_eyeSrv[i] == nullptr)
        {
            g_status = "eye texture/SRV creation failed";
            ReleaseSizeResources();
            return false;
        }
    }

    g_width = source.Width;
    g_height = source.Height;
    g_format = source.Format;
    if (!EnsureShaders()) return false;

    g_ready.store(true, std::memory_order_release);
    g_status = "ready; waiting for both complete renders";
    AKProbeLog("[AKFLAT] full-frame eye textures ready %ux%u fmt=%u",
               g_width, g_height, static_cast<unsigned>(g_format));
    return true;
}

void DrawSbs(ID3D11Texture2D* backBuffer) noexcept
{
    ID3D11RenderTargetView* target = nullptr;
    if (FAILED(g_device->CreateRenderTargetView(backBuffer, nullptr, &target)) || target == nullptr)
    {
        g_status = "backbuffer RTV creation failed";
        return;
    }

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
    ID3D11ShaderResourceView* oldSrvs[2] = {};
    ID3D11SamplerState* oldSampler = nullptr;
    ID3D11RenderTargetView* oldRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    ID3D11RasterizerState* oldRasterizer = nullptr;
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
    g_context->PSGetShaderResources(0, 2, oldSrvs);
    g_context->PSGetSamplers(0, 1, &oldSampler);
    g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, &oldDsv);
    g_context->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
    g_context->OMGetDepthStencilState(&oldDepth, &oldStencilRef);
    g_context->RSGetState(&oldRasterizer);
    g_context->RSGetViewports(&oldViewportCount, oldViewports);
    g_context->RSGetScissorRects(&oldScissorCount, oldScissors);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(g_width);
    viewport.Height = static_cast<float>(g_height);
    viewport.MaxDepth = 1.0f;
    UINT zero = 0;
    ID3D11Buffer* noBuffer = nullptr;
    ID3D11ShaderResourceView* noSrvs[2] = {};
    const FLOAT blendFactor[4] = {};

    g_context->OMSetRenderTargets(1, &target, nullptr);
    g_context->OMSetBlendState(g_blend, blendFactor, 0xffffffffu);
    g_context->OMSetDepthStencilState(g_depth, 0);
    g_context->RSSetState(g_rasterizer);
    g_context->RSSetViewports(1, &viewport);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetVertexBuffers(0, 1, &noBuffer, &zero, &zero);
    g_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_vs, nullptr, 0);
    g_context->HSSetShader(nullptr, nullptr, 0);
    g_context->DSSetShader(nullptr, nullptr, 0);
    g_context->GSSetShader(nullptr, nullptr, 0);
    g_context->PSSetShader(g_ps, nullptr, 0);
    g_context->PSSetShaderResources(0, 2, g_eyeSrv);
    g_context->PSSetSamplers(0, 1, &g_sampler);
    g_context->Draw(3, 0);

    g_context->PSSetShaderResources(0, 2, noSrvs);
    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, oldDsv);
    g_context->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
    g_context->OMSetDepthStencilState(oldDepth, oldStencilRef);
    g_context->RSSetState(oldRasterizer);
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
    g_context->PSSetShaderResources(0, 2, oldSrvs);
    g_context->PSSetSamplers(0, 1, &oldSampler);

    SafeRelease(oldLayout);
    SafeRelease(oldVertexBuffer);
    SafeRelease(oldIndexBuffer);
    SafeRelease(oldVs);
    SafeRelease(oldHs);
    SafeRelease(oldDs);
    SafeRelease(oldGs);
    SafeRelease(oldPs);
    SafeRelease(oldSrvs[0]);
    SafeRelease(oldSrvs[1]);
    SafeRelease(oldSampler);
    for (auto& rtv : oldRtvs) SafeRelease(rtv);
    SafeRelease(oldDsv);
    SafeRelease(oldRasterizer);
    SafeRelease(oldBlend);
    SafeRelease(oldDepth);
    SafeRelease(target);
}
}

namespace AKFlat
{
void CaptureEye(int eye) noexcept
{
    if (eye < 0 || eye > 1) return;
    // Eye 0 starts a new pair. Never composite a fresh eye with a stale capture after a fault.
    if (eye == 0) g_readyMask.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_swapChain == nullptr || !EnsureDevice(g_swapChain) || !EnsureEyeResources()) return;

    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                         reinterpret_cast<void**>(&backBuffer))) &&
        backBuffer != nullptr)
    {
        g_context->CopyResource(g_eyeTexture[eye], backBuffer);
        backBuffer->Release();
        g_readyMask.fetch_or(1u << eye, std::memory_order_release);
    }
}

void Present(IDXGISwapChain* swapChain) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!EnsureDevice(swapChain) || !EnsureEyeResources()) return;
    if ((g_readyMask.load(std::memory_order_acquire) & 3u) != 3u) return;

    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                         reinterpret_cast<void**>(&backBuffer))) &&
        backBuffer != nullptr)
    {
        DrawSbs(backBuffer);
        backBuffer->Release();
        const unsigned long long frame =
            g_composedFrames.fetch_add(1, std::memory_order_relaxed) + 1;
        g_status = "running: two complete renders -> flat SBS";
        if (frame <= 3 || (frame % 600) == 0)
            AKProbeLog("[AKFLAT] composed complete-frame SBS #%llu", frame);
    }
}

bool IsReady() noexcept { return g_ready.load(std::memory_order_acquire); }
unsigned long long ComposedFrames() noexcept
{
    return g_composedFrames.load(std::memory_order_relaxed);
}
const char* Status() noexcept { return g_status; }
}