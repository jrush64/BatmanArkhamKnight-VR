#pragma once

struct IDXGISwapChain;

namespace AKFlat
{
// Capture one completed full-screen eye render into an owned texture.
void CaptureEye(int eye) noexcept;

// Compose the two full-screen eye captures side-by-side immediately before the real Present.
void Present(IDXGISwapChain* swapChain) noexcept;

bool IsReady() noexcept;
unsigned long long ComposedFrames() noexcept;
const char* Status() noexcept;
}