#pragma once

// Same-frame stereo for Batman: Arkham Knight — the UE3 "P1-layout split".
//
// Method: the same-frame split used by the Mass Effect Legendary Edition VR mods.
// NOTE: this half-viewport split is OFF by default; see docs/TECHNICAL_NOTES.md for why the
// shipping path is the complete double render in AKDouble instead.
// Hook ULocalPlayer::CalcSceneView, call it TWICE for the same player with the player's
// viewport rect rewritten to the left half then the right half. The engine builds two views
// and renders both into one side-by-side backbuffer in a single frame, running its own
// culling, occlusion and post per eye. Both eyes, one instant — no AER cadence, so the
// inter-frame disparity that makes objects swim during motion cannot exist.
//
// Addresses were found by static analysis of BatmanAK.exe; see docs/TECHNICAL_NOTES.md.

namespace AKSplit
{
// Install the CalcSceneView hook. Safe to call repeatedly; only acts once.
void Install() noexcept;

// Master switch (F8 in game). Off = stock single view, i.e. exactly the old behaviour.
void SetEnabled(bool on) noexcept;
bool IsEnabled() noexcept;

// Give the right eye its own FSceneViewState (F7). Default off (share) — a fresh state crushed
// AK's auto-exposure to black. Toggle for a live A/B.
void ToggleSecondViewState() noexcept;
bool UsesSecondViewState() noexcept;

// Bisection: swap which half renders first (F6). See ak_split.cpp.
void ToggleRenderOrder() noexcept;
bool RightHalfFirst() noexcept;

// Mirror the per-view render flag onto the 2nd view (F4) — the dark-right fix. Default on.
void ToggleMirrorRenderFlag() noexcept;
bool MirrorsRenderFlag() noexcept;

// True once the split has actually produced two views this session.
bool IsSplitting() noexcept;

// Which half of the backbuffer the engine is rendering right now:
//   -1 = not inside a split view, 0 = left eye, 1 = right eye.
// The camera constant-buffer hook reads this to decide which way to shift the eye.
int CurrentEye() noexcept;

// Called from the RSSetViewports hook — the engine sets the half-width viewport as it starts
// each view, which is how the probe knows whose camera writes it is looking at.
void NoteViewport(float topLeftX, float width) noexcept;

// Diagnostics for the per-frame log line.
unsigned long long SplitCalls() noexcept;
unsigned long long PassthroughCalls() noexcept;
const char* DisabledReason() noexcept;
}

// ---------------------------------------------------------------------------
// AKDouble - same-frame stereo by rendering Batman's COMPLETE outer viewport twice.
//
// +0xBF8000 includes the subclass UI, Canvas flush, and final viewport command. The left call has
// Present suppressed; the right reaches DXGI Present, where both finalized histories become flat
// SBS. +0x9F87B0 is retained only to locate/reset the inner client's once-per-frame Canvas guard.
// ---------------------------------------------------------------------------
namespace AKDouble
{
void Install() noexcept;
void SetEnabled(bool on) noexcept;
bool IsEnabled() noexcept;

// -1 = not inside a double render, 0 = left pass, 1 = right pass. The camera CB hook reads this.
int CurrentPass() noexcept;

// True when the current pass's eye offset was applied to Arkham's actual engine camera before
// FSceneView construction. The GPU constant-buffer hook must not apply the same shift again.
bool EngineEyeCameraApplied(int pass) noexcept;

// Capture the finalized right eye at the real Present boundary.
bool CaptureRightAtPresent() noexcept;

bool IsRunning() noexcept;
unsigned long long Frames() noexcept;
const char* Status() noexcept;

// Live separation audit. Shifts counts every eye offset actually written to the engine camera;
// LastShift is the most recent value per pass. Both feed the periodic [AKPAIR] line, so a session
// log can prove whether the eyes are still being separated after the one-shot lines stop.
unsigned long long EngineEyeShifts() noexcept;
float EngineEyeLastShift(int pass) noexcept;
}

// ---------------------------------------------------------------------------
// AKFreeze — TRUE same-frame stereo by stopping the clock between the two eyes.
//
//   frame N   : time advances, simulation ticks, render LEFT eye
//   frame N+1 : time FROZEN -> simulation cannot advance, render RIGHT eye
//
// Both eyes come from one simulation instant. NOT AER — AER's defect is that the world moves
// between the eyes, and here it provably cannot. The engine still sees exactly one normal view
// family per frame, so streaming and the render thread are untouched (that is what killed the
// double-call approach). See ak_split.cpp.
// ---------------------------------------------------------------------------
namespace AKFreeze
{
void Install() noexcept;
void SetNextEye(int nextEye) noexcept;   // called once per present with the NEXT frame's eye
void SetEnabled(bool on) noexcept;
bool IsEnabled() noexcept;
bool IsFrozenNow() noexcept;
unsigned long long FrozenFrames() noexcept;
const char* Status() noexcept;
}
