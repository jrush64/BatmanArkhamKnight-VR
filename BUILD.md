# Building Batman: Arkham Knight VR

## Prerequisites

- Windows 10/11, x64.
- Visual Studio 2022 (Community, Professional, Enterprise, or the standalone Build Tools)
  with the **"Desktop development with C++"** workload installed. `build.bat` finds it
  automatically via `vswhere.exe`.
- Nothing else. MinHook and Dear ImGui are vendored in `third_party/`. The DIBR shaders are
  pre-compiled into `src/shaders/*_bytecode.h`; the `.hlsl` sources sit beside them.

## Build

```
scripts\build.bat
```

Output: `builds\dxgi.dll` (and `builds\dxgi.map`, useful for resolving crash addresses).

The build is a single `cl.exe` invocation (no solution, no CMake). It compiles the mod sources
together with the vendored MinHook and ImGui sources and links the CRT statically (`/MT`), so the
DLL needs no separate VC++ redistributable in the game folder. The real system `dxgi.dll` and the
OpenXR loader are both loaded at runtime with `LoadLibrary`.

## Deploying

Copy these files into `<Batman Arkham Knight install>\Binaries\Win64\`, next to `BatmanAK.exe`,
while the game is closed:

- `builds\dxgi.dll` (just built)
- `release\openxr_loader.dll`: an x64 build of the official
  [Khronos OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK), not built by this repo
- `runtime\akvr.ini` (optional; a starting configuration)

Logs are written beside the DLL: `logs\AKProbe.log` (proxy, stereo, camera) and `AK_XR.log`
(OpenXR). The expected log lines are listed in `docs/TECHNICAL_NOTES.md`.

## Third-party components

| Component | Location | License |
|---|---|---|
| MinHook | `third_party/minhook` | BSD 2-Clause (see its `LICENSE.txt`) |
| Dear ImGui 1.92.x | `third_party/imgui` | MIT (see its `LICENSE.txt`) |
| OpenXR loader | `release/openxr_loader.dll` (prebuilt binary) | Apache 2.0 (`release/openxr_loader.LICENSE.txt`) |
