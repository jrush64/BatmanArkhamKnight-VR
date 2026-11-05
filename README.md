# Batman: Arkham Knight VR

An unfinished VR mod for Batman: Arkham Knight (Steam, x64, D3D11, modified Unreal Engine 3),
built as a `dxgi.dll` proxy loaded next to `BatmanAK.exe`. It renders the game twice per frame,
once per eye, and submits both eyes to any OpenXR runtime (SteamVR, Meta Quest Link, Virtual
Desktop, etc.).

⚠️ Experimental, **unfinished** community mod. Not affiliated with Rocksteady or Warner Bros.
Use at your own risk. Development has stopped, and the source is published so that others can
pick it up. See [What doesn't work yet](#what-doesnt-work-yet) and
[`docs/TECHNICAL_NOTES.md`](docs/TECHNICAL_NOTES.md).

Licensed under [GPL-3.0](LICENSE).

## What it does

- **Real geometric stereo, same frame.** Each eye is a complete viewport render (scene,
  tiled lighting, post-processing, HUD, pause menu) of the same simulation frame. The second
  render is separated from the first by the engine's own render-thread fence, so it cannot race
  texture streaming. The eye offset is applied to Arkham's real camera location before
  `FSceneView` is built, so per-eye lighting, culling and temporal history are correct for each
  eye. It is not reprojection and not alternate-eye rendering.
- **OpenXR output.** Both full-screen eye captures are submitted to the headset with their
  render-time pose tags, and the FOV declared to the compositor matches what the engine rendered.
  If a frame does not produce a complete left/right pair (menus, loading), the live image is sent
  mono to both eyes instead of a broken pair.
- **Head-look (rotation).** HMD yaw and pitch are added to the engine camera before the view is
  built, with gain, inversion, smoothing and a recenter key.
- **Flat side-by-side desktop mirror.** Runs independently of the headset path, so stereo can be
  checked without a headset.
- **In-game tuning menu (Insert).** Dear ImGui menu for virtual IPD (mm) and world scale,
  convergence, eye swap, head-tracking gain/inversion/smoothing, recenter, runtime toggles and
  alternative stereo modes. The camera is held still while the menu is open, so the IPD can be
  tuned on a frozen frame. Settings save to `akvr.ini` when the menu closes.
- **Fails safe.** The engine addresses it calls have opcode signature checks. On a different
  game build the double render turns itself off and the game runs as normal mono.

## What doesn't work yet

- **Performance.** Two complete renders per frame typically give 20–33 FPS in demanding
  gameplay. This is the main unsolved problem.
- **No positional (6DOF) head tracking.** Only yaw and pitch are applied; roll and translation are
  not.
- **No motion controllers.** Play with a gamepad or keyboard and mouse.
- **No FOV fill.** The game renders at its native FOV (about 86°×55°), so there are black borders
  in the headset. Widening the projection after the engine has culled makes geometry pop and
  zoom, so the "FOV fill" option exists but is off and should stay off until FOV is changed
  before culling.
- **No VR-specific HUD/UI placement.** The HUD and menus are drawn into each eye at their flat
  screen positions.
- **Eye-swap default needs confirming.** The verified complete-frame path wants `SwapEyes=0`
  (pass 0 → left eye). The shipped `akvr.ini` sets that, but one code-side default still says
  `true`. If depth looks inside-out, toggle *Swap eyes* in the menu.
- **Latest build not fully signed off.** The newest changes (the menu draws into its own overlay,
  and the camera is held while the menu is open) ran cleanly for a full session, submitting
  stereo pairs to the headset. The last build explicitly confirmed by an in-headset check is the
  one just before those changes.
- **Leftover diagnostic modes.** The source still contains the earlier AER, DIBR and
  half-viewport split experiments and their F-key bindings. They are not the shipping path; see
  the technical notes before turning any of them on.

## Building

See [BUILD.md](BUILD.md). In short: install Visual Studio 2022 with the C++ workload, then run
`scripts\build.bat`. All dependencies are vendored in `third_party/`.

## Installing

See [`release/README.txt`](release/README.txt).

## Layout

| Path | What's there |
|---|---|
| `src/` | Mod source: DXGI proxy and hooks (`ak_probe`), complete-frame double render and engine camera hooks (`ak_split`), flat SBS compositor (`ak_flat`), OpenXR layer (`ak_xr`), Insert menu (`ak_menu`), DIBR shaders |
| `scripts/` | `build.bat` |
| `third_party/` | Vendored MinHook and Dear ImGui, used by the build |
| `docs/` | `TECHNICAL_NOTES.md`: architecture, engine addresses, approaches that failed and why |
| `release/` | End-user install notes, OpenXR loader, its license |
| `runtime/` | A starting `akvr.ini` |
| `builds/` | A prebuilt `dxgi.dll` |

## License

GPL-3.0 - see [LICENSE](LICENSE). Copyright (C) 2026 Halcyon.

Vendored dependencies keep their own licenses: MinHook (BSD-2-Clause) and Dear ImGui (MIT),
both under `third_party/` with their license files. The shipped `openxr_loader.dll` is the
Khronos OpenXR loader (Apache-2.0, license in `release/`).

The stereo camera layout was first worked out from masterotaku's geo-11 3D fix for this game.
None of that fix's files are included or required.
