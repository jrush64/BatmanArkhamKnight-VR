# Technical notes

How the mod works, which engine addresses it uses, and which approaches were tried and failed,
so whoever continues does not repeat them. All addresses are RVAs into the current Steam
`BatmanAK.exe` (x64, image base `0x140000000` in Ghidra). The game is a heavily modified UE3 with
the stock `D3D11Drv`, Steamworks DRM only (no Denuvo), so a proxy DLL loads without trouble.

## Architecture

```
BatmanAK.exe
  └─ dxgi.dll (this mod)  ── forwards CreateDXGIFactory* to the real system dxgi.dll
       ├─ ak_probe.cpp   swapchain/context hooks, Present hook, camera-CB tools, hotkeys, logging
       ├─ ak_split.cpp   AKDouble: complete-frame double render + engine camera hooks
       │                 AKSplit:  old half-viewport split (off; diagnostic only)
       │                 AKFreeze: old clock-freeze experiment (not installed)
       ├─ ak_flat.cpp    two full-res eye textures → flat side-by-side desktop image
       ├─ ak_xr.cpp      OpenXR session, per-eye swapchains, pose tagging, AER/DIBR leftovers
       └─ ak_menu.cpp    Insert menu (ImGui), akvr.ini load/save
```

### Per game frame (the shipping path)

1. The engine calls the complete outer viewport draw `FViewport::Draw` at **`+0xBF8000`**; the
   mod's hook takes over.
2. **Left eye:** call the original draw with the low `bPresent` bit cleared, so it renders the
   full frame (scene, tiled lighting, post, Batman's subclass UI, Canvas flush) without presenting.
3. **Fence:** enqueue a render-thread fence with **`+0xDC5570`** and wait on it with
   **`+0xDC5660`**. These are the engine's own primitives (atomic counter + render command that
   decrements it, and a wait loop that also propagates render-thread exceptions). Then copy the
   finished left backbuffer into the left eye texture and the left XR history.
4. Clear the client draw's once-per-frame guard **`canvas(+0x48)->+0x94`**.
5. **Right eye:** call the same outer draw again with the original Present flags.
6. **Present hook:** copy the finished right backbuffer into the right eye texture and XR history.
   Submit both histories to OpenXR (pass 0 → XR eye 0, pass 1 → XR eye 1), then compose the
   desktop SBS image from the same two textures. The SBS texture is never used as an XR source.

The eye offset is applied to the camera **location before `FSceneView` is constructed**, via a
hook on the gameplay camera view output at **`+0x9E9ED0`** (photo-mode camera globals live at
`+0x311DCC0` / `+0x311DCD0`). The view constructed just before the outer draw is tagged pass 0 and
carried through the left draw, so both passes get opposite shifts. Applying the shift this early
(instead of only patching the GPU view-projection matrix) is what fixed per-eye tiled lighting
and facial lighting. The GPU view-projection patch remains as a fallback when the engine camera
hook did not fire for a pass.

Head rotation (yaw/pitch) is applied in the same camera hook. While the Insert menu is open the
camera location and rotation are latched and re-asserted every frame (the only place where every
input device's effect meets), and the submitted pose tag freezes to match.

### Safety

- `+0xBF8000`, `+0xDC5570` and `+0xDC5660` have runtime opcode signature checks. A mismatch
  disables the double render and the game runs stock mono.
- Every game-memory access is behind `__try/__except`.
- OpenXR is only started inside `BatmanAK.exe` (never in a launcher). If no HMD is present yet, the
  mod polls `xrGetSystem` every 2 s for about 5 minutes, so the headset can be put on after launch.
- Frames that do not produce a complete L/R pair (menus, loading, the gameplay-load transition,
  which briefly has no client Canvas) are submitted mono to both eyes, and stereo is retried on the
  next frame.

## Engine facts

### Camera constant buffer

- Scene **ViewProjection = VS constant buffer `b1`, bytes 0..63**, written through Map/Unmap
  (sometimes `UpdateSubresource`). The CB is commonly 608 bytes, but `cb1` is declared at many
  sizes across shaders, so **identify it by content, not by ByteWidth**.
- Row-vector convention (`clip = pos * M`). A world-space eye offset `d` is exact as
  `row3 -= d.x*row0 + d.y*row1 + d.z*row2`.
- Content signature of the VP matrix: `col3` = view forward (unit length), `|col0| = P00 =
  1/tan(hFov/2)`, `|col1| = P11 = 1/tan(vFov/2)`, `m33 == 0`.
- The scene CB also holds **temporal (previous-frame) VP siblings**. They must receive the same
  eye translation, or TAA/motion vectors read the eye separation as camera motion and thin geometry
  flickers. The mod scans for strict main-camera siblings and patches them (`[AKTEMP]` log lines).
- Much of the scene is drawn on **deferred D3D11 contexts**, so immediate-context draw counts do
  not describe the whole frame.

### World scale

About **50 Unreal units per metre**. A 64 mm IPD is about 3.2 uu total, i.e. **1.6 uu per eye**.
An early build inherited a value of 200 from another game (125× too large). That separated the
eyes by about 8 m and showed up as "flicker" on nearby railings. Old configs above 20 are migrated
to 1.6 automatically.

### Addresses

| Symbol | RVA / offset |
|---|---|
| Complete outer viewport draw (`FViewport::Draw`) | `+0xBF8000` |
| Viewport draw / per-player render loop (observer only) | `+0x9F87B0` |
| Render-fence enqueue | `+0xDC5570` |
| Render-fence wait | `+0xDC5660` |
| Gameplay camera view output (eye offset + head rotation) | `+0x9E9ED0` |
| Photo-mode camera globals | `+0x311DCC0`, `+0x311DCD0` |
| `ULocalPlayer::CalcSceneView` | `+0x9F6220` |
| `AllocateViewState` (`FSceneViewState`, `0x8850` bytes) | `+0x82F860` |
| `FSceneView::FSceneView` | `+0x82EB30` |
| ULocalPlayer lazy view-state alloc | `+0x9F9330` |
| `ULocalPlayer::ViewportClient` / `Origin.X,Y` / `Size.X,Y` / `ViewState` | `+0x5C` / `+0xA8,+0xAC` / `+0xB0,+0xB4` / `+0xC8` |
| `sizeof(FSceneView)` | `0xA20` |
| Viewport once-per-frame guard | `canvas(+0x48)->+0x94` |

**Repeatable route to `CalcSceneView` in any UE3 game without an SDK:** UE3 registers script
natives as `{ "ULocalPlayerexecProject", &ULocalPlayer::execProject }`, so the name string sits
next to its function pointer. Follow the string xrefs to get `Project` and `DeProject`, intersect
their callees, and the large common callee is `CalcSceneView`. Beware a near-miss rect at
`+0x53C/+0x544`: it hangs off `ViewportClient+0x478`, not off the `ULocalPlayer`.

Two exe strings were never followed up: `UEngineexecIsStereoscopic3D` and
`UEngineexecIsRealDStereoEnabled` (console-era stereo natives). They might be a native stereo
shortcut, though they are more likely gutted on PC.

## Approaches that failed (do not silently restore)

| Approach | What happened | Why |
|---|---|---|
| **Half-viewport split** (call `CalcSceneView` twice with `Origin/Size` rewritten to left/right halves) | Geometry appeared in both halves, but the right half was dark and lacked UI and the pause menu | Arkham lights the scene with **tiled deferred compute**, dispatched over a grid sized from the primary view only. Scaleform/Canvas UI also runs once per viewport. Not reachable from game-thread flags (`FSceneView+0x2C0` mirror tested and falsified). Kept behind Ctrl+F8 as a diagnostic. |
| **AER (alternate-eye rendering) + camera lock** | Correct, fully lit frames | The eyes come from different presents, so camera motion produces swim and judder. |
| **Clock freeze** (hook `QueryPerformanceCounter` so two presents share one sim time) | Removed | Process-wide timing intervention was too broad and never proven to engage correctly. |
| **Unfenced complete double render** | Correct images, then a crash after about 1,200 frames: "Rendering thread exception" in texture streaming (`+0x10629C0`, a mip-chain size calculation on a freed texture) | Two view families per game frame raced the render thread's streaming state. **Fixed by the render-thread fence**, which is the shipping path. |
| **Hooking `+0x9F87B0` as the eye boundary** | Black horizontal bands in one eye | It returns before Batman's subclass UI, the Canvas flush and the final viewport command, so the capture was an unfinished backbuffer. The boundary had to move out to `+0xBF8000`. |
| **Post-cull FOV fill** (widen the projection to the headset FOV) | Zoom changes on gameplay load, and geometry popping/flickering in the headset only | The engine has already culled against its native frustum. FOV has to change before culling for this to work. |
| **Live HMD orientation on the fixed-camera pair** | Stable geometry swam in the headset only | The compositor reprojected images that were not rendered with that pose. Pose tags must be the pose actually rendered. |
| **XR FOV declared from any view** | The image rescaled within and across frames | Secondary views overwrote the FOV. It is now latched to the first eye-0 scene projection of each complete pair. |
| **Per-eye ImGui renderers / OpenXR quad layer for the menu** | The VDXR runtime accepted the quad but never displayed it | The working design is one ImGui owner that renders into its own overlay texture, composited into both eyes and the desktop. |

## Debug hotkeys

| Key | Action |
|---|---|
| Insert | Tuning menu |
| Home (configurable) | Recenter |
| F2 | Complete-frame double render / SBS on or off (off = stock mono) |
| F5 | OpenXR submission on or off (desktop SBS keeps running) |
| F7 | Diagnostic: isolated vs shared right-eye `FSceneViewState` |
| F9 | Dump live camera VP and FOV to the log |
| F10 | Camera ownership proof (sways the camera sideways) |
| Ctrl+F8 | Legacy half-viewport split (diagnostic only) |
| F3, F4, F6 | Legacy split/camera-lock toggles |

`ak_xr.cpp` also reads some F-keys and PgUp/PgDn inside its AER/DIBR tuning paths. These overlap
the table above and only matter when those modes are selected. Clean them up before adding
bindings.

## Expected log lines (`logs\AKProbe.log`)

```text
[AKPROBE] STARTUP: FLAT+VR mode=DOUBLE-RENDER ... OpenXR=AVAILABLE
[AKDOUBLE] COMPLETE outer draw +0xBF8000; client observer +0x9F87B0; fence +0xDC5570/+0xDC5660 validated
[AKDOUBLE] render-thread fence complete #N
[AKENGINEEYE] #N source=gameplay|photo pass=0|1 shift=... right=(...)
[AKFLAT] composed complete-frame SBS #N
[AKPAIR] xrPairSubmits=N xrMonoSubmits=N | engineEyeShifts=N lastShift L=.. R=.. | halfEye=.. swapEyes=.. stereoOn=..
```

`[AKPAIR]` is the line that answers "is the headset actually getting two eyes?". A growing
`xrPairSubmits` count means stereo pairs are reaching the headset. A zero count alongside healthy
`[AKFLAT]` lines means the desktop is in stereo while the headset is not.

Failure lines:

- `outer-draw/fence signature mismatch`: wrong game build; mono fallback.
- `left/right outer-draw fence failed`: do not force the double render back on.
- `unsupported backbuffer ... MSAA`: the flat compositor needs an MSAA resolve path.

## Where to go next

1. **Performance.** Profile the fenced double render. The fence fully serialises the two
   passes; finding what in the second pass is redundant (shadow maps, streaming updates,
   GameWorks effects that are view-independent) is the likeliest win.
2. **FOV before culling.** Find where the engine builds the culling frustum from the projection,
   and widen it there so headset FOV fill works without pop-in.
3. **Positional tracking.** The camera hook at `+0x9E9ED0` already owns location; add HMD
   translation (scaled by ~50 uu/m) the same way the eye offset is added.
4. **HUD/UI.** The HUD is currently rendered into each eye at screen depth. Moving it to a
   separate layer would need the Scaleform/Canvas pass captured separately.
5. **Motion controllers.** Not started.
