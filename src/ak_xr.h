#pragma once

#include <Windows.h>

#include <cstdint>

struct IDXGISwapChain;
struct ID3D11DeviceContext;
struct ID3D11DepthStencilView;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

// AK -> headset, Milestone 1 (mono). Ported from the proven Mass Effect OpenXR submission
// (clean/xr_session.cpp): create an OpenXR session on the GAME's D3D11 device, make two eye
// swapchains, and every Present copy the finished game backbuffer into both eyes and submit a
// projection layer. M1 submits a WORLD-LOCKED pose (identity) so the image is look-around-able
// (proves head tracking) and comfortable; M2 switches to the live head pose + drives the AK
// camera from the head + adds per-eye parallax for real depth.
namespace AKXr
{
struct DibrTuning
{
    bool xrEnabled = true;
    bool stereoOn = true;
    // AK renders per-eye from the real camera (the mod owns its ViewProjection CB), so the mode
    // is AER, not DIBR. Mode is derived below: DIBR = stereoOn && dibrEnabled.
    bool dibrEnabled = false;
    bool sameFrameStereo = true;    // verified mode: render two complete eyes from one simulation frame
    bool depthDebug = false;
    bool submitHeadPose = true;
    float gain = 0.035f;
    float depthScale = 96.0f;
    float edgeGuard = 3.0f;
    float edgeThreshold = 0.080f;
    float convergence = 0.0f;
    // AER-specific (own values so they never clash with DIBR). Mode is derived: Mono=!stereoOn,
    // DIBR=stereoOn&&dibrEnabled, AER=stereoOn&&!dibrEnabled.
    // Per-eye camera shift, in the GAME's world units. Arkham Knight is UE3, ~50 uu/m like
    // ME1, so a 64 mm IPD is ~3.2 uu total and half of that is ~1.6. (The 200 this was
    // ported with is an MGS2 number — MGS2's world scale is nothing like UE3's.)
    float aerHalfEye = 1.6f;
    bool  aerSwapEyes = false;   // flip L/R = invert depth direction; start neutral, confirm in headset
    int   aerDisplayHz = 0;      // 0 = auto-measure; else force the headset refresh for the pacing (fixes mis-measure)
    float aerFpScale = 0.10f;    // first-person separation = aerHalfEye * this. Gun is ~at the camera, so it needs
                                 // a very small shift to fuse (0.35 still doubled it); 0.10 = gun fuses, slight depth.
    bool  sixDofGameplay = true;     // free head-look in regular gameplay (rotate render camera with the head)
    bool  sixDofFirstPerson = false; // reserved until Arkham context detection is wired
    bool  sixDofCutscene = false;    // reserved until Arkham context detection is wired
    float headYawGain = 1.0f;
    float headPitchGain = 1.0f;
    float headSmoothing = 0.0f;
    bool  invertHeadYaw = false;
    bool  invertHeadPitch = false;
    int   recenterKey = 0x24;        // VK code for the recenter hotkey (default VK_HOME); reassignable in the menu
    bool  headAimEnabled = false;    // first-person head-aim (weapon turns horizontally with the head). OFF by default.
    float headAimGain = 1.5f;        // first-person head-aim turn-speed multiplier (weapon/view turn per degree of
                                     // head turn). 1.0 = 1:1; higher = the character keeps up with fast head turns.
    // FOV fill widens the projection to the headset FOV. In Arkham this happens AFTER the engine
    // has culled against its native frustum, which - Hinny that GOAT - causes zoom changes and geometry pop-in, so it
    // stays off until the culling frustum can be widened too (see docs/TECHNICAL_NOTES.md).
    bool  fovFillEnabled = false;    // unsafe in AK: post-cull widening makes geometry pop; native FOV is stable
    float fovFillH = 1.0f;           // VR Fill H: multiplier on the headset horizontal half-FOV target (1.0 = match)
    float fovFillV = 1.0f;           // VR Fill V: multiplier on the headset vertical half-FOV target (1.0 = match)
};

// Call once per frame from the game's Present hook, AFTER the original Present (so the backbuffer
// holds the finished frame). Lazily brings up the session on the first call, then runs the loop.
// No-op (safe) if there is no HMD / the runtime is unavailable.
void Frame(IDXGISwapChain* gameSwapChain) noexcept;

// Latest HMD head orientation in degrees (Y-up XR frame). Valid only once the session is tracking.
// M2a feeds this into the camera injection (Detour_ViewBuild) so head rotation drives the game camera.
bool GetHeadYawPitchDeg(float* yawDeg, float* pitchDeg, float* rollDeg) noexcept;
// Same reading, but never frozen by the Insert menu's camera hold. For the tuner's HMD readout only —
// never feed this to the camera, or the render and the submitted pose would disagree.
bool GetLiveHeadYawPitchDeg(float* yawDeg, float* pitchDeg, float* rollDeg) noexcept;
// Tag subsequent eye captures with the gain/inversion/smoothing-adjusted pose actually injected into the camera.
void SetRenderedHeadYawPitchDeg(float yawDeg, float pitchDeg) noexcept;

// M2a: when true, the per-frame projection layer is submitted at the LIVE head orientation instead of
// the M1 identity pose — so the head-driven render and the compositor agree (no swim). Off = M1 mono.
void SetHeadLookSubmitPose(bool on) noexcept;

// The game's live projection scale (m00 = cot(hHalf), m11 = cot(vHalf)). The submit fov is built from
// this so the eye fov MATCHES the game's render — no zoom/stretch, auto-tracks the game's fov changes.
void SetGameProjection(float m00, float m11) noexcept;

// FOV-fill: the headset's eye FOV half-angles (radians). The render-side widens the game projection to this so
// the rendered image fills the headset instead of sitting in a black border. Returns false until tracking.
bool GetHeadsetFovHalf(float* h, float* v) noexcept;

// M2b — AER stereo depth. SetStereo gates the per-eye parallax path (mono fallback when off).
// GetActiveEye is read by the camera hook to apply THIS frame's per-eye camera shift (0=L, 1=R).
void SetStereo(bool on) noexcept;
int GetActiveEye() noexcept;
void SetAutoDibrFp(bool on) noexcept;   // AER mode: auto-use DIBR for first-person/aim (gun fuses, scene keeps depth)
void Recenter() noexcept;               // re-anchor the world-locked space so "forward" = current head (recenter)

// Convergence: per-eye horizontal fov shift (radians) so near objects fuse instead of doubling.
void SetConvergence(float radians) noexcept;

// Insert-menu bridge. These mirror the live DIBR/hotkey values and are safe to edit each frame.
void GetDibrTuning(DibrTuning* out) noexcept;
void SetDibrTuning(const DibrTuning& tuning) noexcept;
void ResetDibrTuning() noexcept;
bool GetUnlockedCameraEnabled() noexcept;
void SetUnlockedCameraEnabled(bool on) noexcept;

// Same-frame double render: the render-fn detour sets the eye, renders, then captures the backbuffer
// into that eye's history — so BOTH eyes are current (no AER temporal gap).
void SetActiveEye(int eye) noexcept;
void SetSameFrameActive(bool on) noexcept;
// True only when this Present owns two finalized full-screen eye histories; false = submit live mono.
void SetSameFramePairReady(bool ready) noexcept;
// How many same-frame submits sent a real L/R pair to the headset versus the same backbuffer to both
// eyes. This is the only measurement that distinguishes working stereo from a flat headset image.
void SameFrameSubmitStats(unsigned long long* pairs, unsigned long long* monos) noexcept;
// true = viewport-split SBS backbuffer (crop per eye); false = full-frame double render (history).
void SetSbsFromBackbuffer(bool on) noexcept;
void CaptureBackbufferToHistory(int eye) noexcept;
void CaptureTextureToHistory(int eye, ID3D11Texture2D* src) noexcept;  // SF2: capture the off-screen scene RT
// Fill source: the game's off-screen scene color RT (square, fills the headset). dxgi pushes it each frame so
// AER/DIBR copy IT instead of the letterboxed backbuffer (AK pillarboxes to 16:9 even at a square resolution).
void SetFillSceneRT(ID3D11Texture2D* rt) noexcept;

// DIBR proof path: capture the scene depth right before the game clears it, then synthesize one eye
// from the current color frame + captured depth during XR submit.
void CaptureDepthBeforeClear(ID3D11DeviceContext* context, ID3D11DepthStencilView* dsv) noexcept;
bool IsDesktopDepthScanArmed() noexcept;
void CaptureDepthCandidateBeforeClear(ID3D11DeviceContext* context, ID3D11DepthStencilView* dsv) noexcept;
void CaptureDepthCandidateAfterDraw(ID3D11DeviceContext* context) noexcept;
void CaptureShaderResourceCandidate(ID3D11DeviceContext* context, const char* stage, UINT startSlot, UINT slot, ID3D11ShaderResourceView* srv) noexcept;
void TrackLiveShaderResource(ID3D11DeviceContext* context, const char* stage, UINT startSlot, UINT slot, ID3D11ShaderResourceView* srv) noexcept;

// Camera/room change signal (from GM_ChangeCamera): SOFT depth re-acquire so DIBR re-points at the new room's
// depth WITHOUT dropping a frame (which flickered normal<->double). Keeps DIBR running on the current depth.
void OnCameraChanged() noexcept;

// True once the OpenXR session is created and running (frames being submitted).
bool IsRunning() noexcept;
}
