@echo off
REM Build the Arkham Knight VR dxgi.dll proxy (x64, static CRT).
REM Output: builds\dxgi.dll (and builds\dxgi.map)
REM Prerequisites: Visual Studio 2022 (Community/Professional/Enterprise or the standalone
REM Build Tools) with the "Desktop development with C++" workload. See BUILD.md.
setlocal
cd /d "%~dp0"

REM Locate any VS2022 install via vswhere instead of assuming one edition or drive letter.
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" set VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" ( echo [build] vswhere.exe not found - is Visual Studio 2022 installed? & exit /b 1 )

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
if not defined VSINSTALL ( echo [build] no VS install with the C++ toolset found & exit /b 1 )

call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 ( echo [build] vcvarsall failed & exit /b 1 )

set OUTDIR=%~dp0..\builds
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
set OBJDIR=%OUTDIR%\obj
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

set SRC=..\src
for %%I in ("%~dp0..") do set ROOT=%%~fI

REM MinHook and Dear ImGui are vendored in this repo under third_party/ - nothing to fetch.
set MH=..\third_party\minhook
if not exist "%MH%\src\hook.c" ( echo [build] minhook not found at "%MH%" & exit /b 1 )
set IMGUI=..\third_party\imgui
if not exist "%IMGUI%\imgui.cpp" ( echo [build] imgui not found at "%IMGUI%" & exit /b 1 )

REM /MT  = static CRT, so no vcruntime DLL is needed in the game folder.
REM /EHa = the __try/__except guards around game memory coexist with C++ EH.
REM No dxgi.lib import of ourselves: the real system dxgi.dll is LoadLibrary'd at runtime.
REM The OpenXR loader is also LoadLibrary'd at runtime; nothing OpenXR is linked.
REM /d1trimfile strips the repo folder from __FILE__ paths so no local path ends up in the DLL.
cl /nologo /O2 /MT /LD /EHa /std:c++17 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
  "/d1trimfile:%ROOT%\\" /I"%MH%\include" /I"%IMGUI%" /I"%IMGUI%\backends" ^
  /Fo"%OBJDIR%\\" ^
  "%SRC%\ak_probe.cpp" "%SRC%\ak_xr.cpp" "%SRC%\ak_menu.cpp" "%SRC%\ak_split.cpp" "%SRC%\ak_flat.cpp" ^
  "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" ^
  "%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" ^
  "%IMGUI%\backends\imgui_impl_dx11.cpp" ^
  /Fe:"%OUTDIR%\dxgi.dll" /link /DEF:"%SRC%\dxgi_probe.def" user32.lib d3d11.lib dxgi.lib ^
  /MAP:"%OUTDIR%\dxgi.map"
if errorlevel 1 ( echo [build] compile/link FAILED & exit /b 1 )

REM Drop linker by-products nobody deploys.
del /q "%OUTDIR%\dxgi.exp" "%OUTDIR%\dxgi.lib" 2>nul
rmdir /s /q "%OBJDIR%" 2>nul

echo [build] OK -^> "%OUTDIR%\dxgi.dll"
endlocal
