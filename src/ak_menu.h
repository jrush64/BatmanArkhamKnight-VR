#pragma once

struct IDXGISwapChain;
struct ID3D11RenderTargetView;

// Insert tuning menu. ONE ImGui owner: Update() polls Insert, builds one ImGui frame, and renders
// it into a single offscreen RGBA overlay texture. Nothing else renders ImGui. The finished overlay
// is then composited wherever it is needed — onto the desktop backbuffer, and (crucially) into each
// OpenXR eye image, so the live stereo image keeps running underneath while you tune it.
namespace AKMenu
{
// Call once per Present, BEFORE XR submission and before the desktop SBS composite.
void Update(IDXGISwapChain* swapChain) noexcept;

// Composite this frame's overlay. Both are no-ops when the menu is closed.
// BlitToBackbuffer must run LAST (after the desktop SBS composite) so the panel sits on top.
void BlitToBackbuffer(IDXGISwapChain* swapChain) noexcept;
// offsetX shifts the panel horizontally in target pixels. The XR path uses it to cancel the
// per-eye convergence crop, so the panel lands identically in both eyes (zero disparity).
bool BlitToRenderTarget(ID3D11RenderTargetView* rtv, unsigned width, unsigned height,
                        int offsetX = 0) noexcept;

bool IsOpen() noexcept;
void Shutdown() noexcept;
}
