#include "ak_split.h"
#include "ak_menu.h"
#include "ak_xr.h"

#include <Windows.h>
#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <intrin.h>

// Logging lives in ak_probe.cpp.
void AKProbeLog(const char* format, ...) noexcept;

namespace
{
// ---------------------------------------------------------------------------
// BatmanAK.exe layout — static analysis, see docs/TECHNICAL_NOTES.md for the derivation.
// ---------------------------------------------------------------------------
constexpr uintptr_t kRvaCalcSceneView    = 0x9F6220;   // ULocalPlayer::CalcSceneView
constexpr uintptr_t kRvaAllocateViewState = 0x82F860;  // AllocateViewState(0, 0)
constexpr uintptr_t kRvaEngineCameraView = 0x9E9ED0; // fills FVector location + FRotator before view construction
constexpr uintptr_t kRvaEngineCameraReturn = 0x9F640C; // return after CalcSceneView's call to +0x9E9ED0
constexpr uintptr_t kRvaPhotoOverrideFlag = 0x311DBB8;
constexpr uintptr_t kRvaPhotoLocation = 0x311DCC0;
constexpr uintptr_t kRvaPhotoRotation = 0x311DCD0;

constexpr uintptr_t kLpViewportClient = 0x5C;
constexpr uintptr_t kLpOriginX        = 0xA8;
constexpr uintptr_t kLpOriginY        = 0xAC;
constexpr uintptr_t kLpSizeX          = 0xB0;
constexpr uintptr_t kLpSizeY          = 0xB4;
constexpr uintptr_t kLpViewState      = 0xC8;

// FSceneView per-view render flag the game-thread render loop sets on the PRIMARY view only
// (`if (family+0xC0 & 4) view+0x2C0 = 1`, at exe+0x9F87B0). The split's 2nd view never goes
// through that loop, so it renders geometry but misses the per-view post/lighting setup — the
// dark right half. Mirror it onto the 2nd view. See docs/TECHNICAL_NOTES.md.
constexpr uintptr_t kFsvRenderFlag = 0x2C0;

using CalcSceneViewFn = void*(*)(void* localPlayer, void* viewFamily, void* outLocation,
                                 void* outRotation, void* viewport, void* viewDrawer);
using AllocateViewStateFn = void*(*)(int, int);

CalcSceneViewFn g_originalCalcSceneView = nullptr;
AllocateViewStateFn g_allocateViewState = nullptr;

std::atomic<bool> g_installed{false};
// DEFAULT ON for the flat-stereo diagnostic build. This proven crash-safe path writes
// both views directly into the monitor backbuffer. Its right-half tiled-lighting defect is
// deliberately exposed here so it can be fixed without OpenXR or headset submission.
std::atomic<bool> g_enabled{false};
// Give the right eye its OWN persistent FSceneViewState. This is ON for complete outer-draw stereo:
// sharing one state makes pass 0 and pass 1 overwrite each other's TAA/occlusion history every
// frame, which shimmers on thin geometry such as railings. Full viewport rendering gives the right
// state its own exposure/post updates; F7 remains a live isolated-vs-shared A/B.
std::atomic<bool> g_useSecondViewState{true};
std::atomic<unsigned long long> g_doubleViewStateSwaps{0};
// Bisection: render the RIGHT half first, LEFT second. If the darkness follows the second CALL it
// moves to the left; if it follows the screen POSITION it stays on the right. Tells whether the
// bug is call-order/state or render-target/position. F6.
std::atomic<bool> g_rightHalfFirst{false};
std::atomic<bool> g_mirrorRenderFlag{true};   // F4: mirror the per-view render flag onto the 2nd view
std::atomic<int>  g_currentEye{-1};
std::atomic<unsigned long long> g_splitCalls{0};
std::atomic<unsigned long long> g_passthroughCalls{0};
const char* g_disabledReason = "";

void* g_secondViewState = nullptr;   // the right eye's own FSceneViewState
using EngineCameraViewFn = void(*)(void*, float*, int32_t*);
EngineCameraViewFn g_originalEngineCameraView = nullptr;
std::atomic<uintptr_t> g_engineExeBase{0};
std::atomic<bool> g_engineCameraHookReady{false};
std::atomic<unsigned int> g_engineEyePassMask{0};
std::atomic<unsigned long long> g_engineEyeShifts{0};
// Last shift actually written to the engine camera, per pass. The one-shot [AKENGINEEYE] lines stop
// after 8 frames, so without this the periodic audit cannot tell "stereo still separating" from
// "separation silently stopped after the first few frames" — which look identical in the headset.
std::atomic<float> g_engineEyeLastShift[2] = {{0.0f}, {0.0f}};
// Arkham constructs the first view immediately before entering the hooked outer draw. Keep a
// narrow window between frames so that pass can be tagged as eye 0; close it while either outer
// draw is running so transition/mono retries cannot inherit the left-eye offset.
std::atomic<bool> g_preDrawLeftWindow{true};

// Latched from RSSetViewports: which half the engine is currently drawing into.
std::atomic<float> g_lastViewportX{0.0f};
std::atomic<float> g_lastViewportW{0.0f};

void HardDisable(const char* why) noexcept
{
    g_disabledReason = why;
    g_enabled.store(false, std::memory_order_relaxed);
    g_currentEye.store(-1, std::memory_order_relaxed);
    AKProbeLog("[AKSPLIT] DISABLED: %s", why);
}

// Every read of a live engine object is guarded. A bad address here takes the game down.
bool ReadRect(void* lp, float* originX, float* originY, float* sizeX, float* sizeY) noexcept
{
    __try
    {
        const uintptr_t p = reinterpret_cast<uintptr_t>(lp);
        *originX = *reinterpret_cast<float*>(p + kLpOriginX);
        *originY = *reinterpret_cast<float*>(p + kLpOriginY);
        *sizeX   = *reinterpret_cast<float*>(p + kLpSizeX);
        *sizeY   = *reinterpret_cast<float*>(p + kLpSizeY);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WriteRect(void* lp, float originX, float originY, float sizeX, float sizeY) noexcept
{
    __try
    {
        const uintptr_t p = reinterpret_cast<uintptr_t>(lp);
        *reinterpret_cast<float*>(p + kLpOriginX) = originX;
        *reinterpret_cast<float*>(p + kLpOriginY) = originY;
        *reinterpret_cast<float*>(p + kLpSizeX)   = sizeX;
        *reinterpret_cast<float*>(p + kLpSizeY)   = sizeY;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void* ReadPtr(void* base, uintptr_t offset) noexcept
{
    __try { return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(base) + offset); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool WritePtr(void* base, uintptr_t offset, void* value) noexcept
{
    __try
    {
        *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(base) + offset) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The RVAs above are for one specific build of BatmanAK.exe. If the game updates, they point
// at whatever now lives there and the hook would be writing floats into a stranger's object. So
// before the first split, check the object actually looks like a ULocalPlayer: a non-null
// ViewportClient and a viewport rect in the 0..1 range the engine documents. Anything else
// and the hook switches itself off for the session rather than guess.
bool ValidateLocalPlayer(void* lp) noexcept
{
    if (lp == nullptr) return false;

    if (ReadPtr(lp, kLpViewportClient) == nullptr) return false;

    float ox = 0, oy = 0, sx = 0, sy = 0;
    if (!ReadRect(lp, &ox, &oy, &sx, &sy)) return false;

    if (!std::isfinite(ox) || !std::isfinite(oy) || !std::isfinite(sx) || !std::isfinite(sy)) return false;
    if (ox < -0.01f || ox > 1.01f || oy < -0.01f || oy > 1.01f) return false;
    if (sx <= 0.0f || sx > 1.01f || sy <= 0.0f || sy > 1.01f) return false;
    return true;
}

void* EnsureSecondViewState() noexcept
{
    if (g_secondViewState != nullptr) return g_secondViewState;
    if (g_allocateViewState == nullptr) return nullptr;

    // The game's own ULocalPlayer constructor calls this as AllocateViewState(0, 0) and stores
    // the result in ViewState. Same call, same arguments.
    __try
    {
        g_secondViewState = g_allocateViewState(0, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_secondViewState = nullptr;
    }

    AKProbeLog("[AKVIEW] second FSceneViewState = 0x%p", g_secondViewState);
    return g_secondViewState;
}

bool ApplyEngineEyeOffset(float* location, const int32_t* rotation, int pass, const char* source) noexcept
{
    if (location == nullptr || rotation == nullptr || (pass != 0 && pass != 1)) return false;

    AKXr::DibrTuning tuning = {};
    AKXr::GetDibrTuning(&tuning);
    if (!tuning.stereoOn || tuning.aerHalfEye == 0.0f) return false;

    __try
    {
        constexpr float kRotatorToRadians = 6.2831853071795864769f / 65536.0f;
        const float pitch = static_cast<float>(rotation[0]) * kRotatorToRadians;
        const float yaw   = static_cast<float>(rotation[1]) * kRotatorToRadians;
        const float roll  = static_cast<float>(rotation[2]) * kRotatorToRadians;
        const float sp = std::sin(pitch), cp = std::cos(pitch);
        const float sy = std::sin(yaw),   cy = std::cos(yaw);
        const float sr = std::sin(roll),  cr = std::cos(roll);

        // UE3 left-handed, X forward / Y right / Z up. This is the local +Y axis transformed by
        // FRotationMatrix(Pitch,Yaw,Roll), including photo-mode camera roll.
        const float right[3] = {
            sr * sp * cy - cr * sy,
            sr * sp * sy + cr * cy,
            -sr * cp
        };

        float sign = (pass == 1) ? 1.0f : -1.0f;
        if (tuning.aerSwapEyes) sign = -sign;
        const float shift = sign * tuning.aerHalfEye;
        location[0] += right[0] * shift;
        location[1] += right[1] * shift;
        location[2] += right[2] * shift;

        g_engineEyePassMask.fetch_or(1u << pass, std::memory_order_release);
        g_engineEyeLastShift[pass].store(shift, std::memory_order_relaxed);
        const auto count = g_engineEyeShifts.fetch_add(1, std::memory_order_relaxed) + 1;

        // Separation diagnostic: log whenever the tuned value actually CHANGES at the point of use.
        // If dragging the slider produces no line here, the menu value is not reaching the render and
        // the problem is upstream of this function. If it does, the render is getting what you set
        // and any remaining "no visible change" is a magnitude/world-scale question, not a wiring one.
        {
            static float s_lastLogged = -1.0f;
            if (std::fabs(tuning.aerHalfEye - s_lastLogged) > 0.0005f)
            {
                s_lastLogged = tuning.aerHalfEye;
                AKProbeLog("[AKSEP] half-eye now %.4f uu (total %.4f) applied at the engine camera, pass=%d",
                           static_cast<double>(tuning.aerHalfEye),
                           static_cast<double>(tuning.aerHalfEye * 2.0f), pass);
            }
        }

        if (count <= 8)
            AKProbeLog("[AKENGINEEYE] #%llu source=%s pass=%d shift=%.3f right=(%.3f %.3f %.3f) rot=(%d %d %d)",
                       count, source, pass, static_cast<double>(shift),
                       static_cast<double>(right[0]), static_cast<double>(right[1]),
                       static_cast<double>(right[2]), rotation[0], rotation[1], rotation[2]);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ApplyHeadRotation(int32_t* rotation, int renderPass, const char* source) noexcept
{
    static float s_filteredYaw = 0.0f;
    static float s_filteredPitch = 0.0f;
    static bool s_filterValid = false;

    if (rotation == nullptr) return false;

    AKXr::DibrTuning tuning = {};
    AKXr::GetDibrTuning(&tuning);
    if (!tuning.sixDofGameplay)
    {
        s_filterValid = false;
        return false;
    }

    float rawYawDeg = 0.0f, rawPitchDeg = 0.0f;
    if (!AKXr::GetHeadYawPitchDeg(&rawYawDeg, &rawPitchDeg, nullptr)) return false;

    float yawDeg = rawYawDeg * tuning.headYawGain * (tuning.invertHeadYaw ? -1.0f : 1.0f);
    float pitchDeg = rawPitchDeg * tuning.headPitchGain * (tuning.invertHeadPitch ? -1.0f : 1.0f);

    // Update once for the complete stereo pair. Pass 1 must use the exact same filtered
    // orientation as pass 0 or head smoothing itself would create inter-eye divergence.
    if (renderPass != 1 || !s_filterValid)
    {
        if (!s_filterValid || tuning.headSmoothing <= 0.0f)
        {
            s_filteredYaw = yawDeg;
            s_filteredPitch = pitchDeg;
            s_filterValid = true;
        }
        else
        {
            const float alpha = 1.0f - tuning.headSmoothing;
            float yawDelta = yawDeg - s_filteredYaw;
            while (yawDelta > 180.0f) yawDelta -= 360.0f;
            while (yawDelta < -180.0f) yawDelta += 360.0f;
            s_filteredYaw += yawDelta * alpha;
            s_filteredPitch += (pitchDeg - s_filteredPitch) * alpha;
        }
    }
    yawDeg = s_filteredYaw;
    pitchDeg = s_filteredPitch;
    AKXr::SetRenderedHeadYawPitchDeg(yawDeg, pitchDeg);

    __try
    {
        constexpr float kDegreesToRotator = 65536.0f / 360.0f;
        rotation[0] += static_cast<int32_t>(std::lround(pitchDeg * kDegreesToRotator));
        rotation[1] += static_cast<int32_t>(std::lround(yawDeg * kDegreesToRotator));

        static std::atomic<unsigned long long> s_headRotations{0};
        const auto count = s_headRotations.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 8)
            AKProbeLog("[AKHEAD] #%llu source=%s raw=(%.2f %.2f) applied=(%.2f %.2f) gain=(%.2f %.2f) smooth=%.2f rot=(%d %d %d)",
                       count, source, static_cast<double>(rawYawDeg), static_cast<double>(rawPitchDeg),
                       static_cast<double>(yawDeg), static_cast<double>(pitchDeg),
                       static_cast<double>(tuning.headYawGain), static_cast<double>(tuning.headPitchGain),
                       static_cast<double>(tuning.headSmoothing), rotation[0], rotation[1], rotation[2]);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
int LogicalEngineEyePass() noexcept
{
    const int pass = AKDouble::CurrentPass();
    if (pass == 0 || pass == 1) return pass;
    if (AKDouble::IsEnabled() && g_preDrawLeftWindow.load(std::memory_order_acquire)) return 0;
    return -1;
}

// Insert-menu camera hold. Swallowing window messages does not stop Arkham's look input, and neither
// does pinning the cursor once a gamepad is in play. So do not fight the input at all: hold the
// CAMERA. This runs on the engine's own camera-view output, downstream of every input device by
// construction, which is the only place all of them meet. Latched on the open edge and re-asserted
// every frame; the eye offset is still applied afterwards, so stereo stays live and tunable on the
// held frame — which is exactly what you want while dragging an IPD slider.
bool g_menuCamHeld = false;
float g_menuHeldLocation[3] = {0.0f, 0.0f, 0.0f};
int32_t g_menuHeldRotation[3] = {0, 0, 0};

void Detour_EngineCameraView(void* self, float* location, int32_t* rotation) noexcept
{
    void* const returnAddress = _ReturnAddress();
    g_originalEngineCameraView(self, location, rotation);

    const uintptr_t base = g_engineExeBase.load(std::memory_order_acquire);
    if (base == 0 || reinterpret_cast<uintptr_t>(returnAddress) != base + kRvaEngineCameraReturn) return;

    const int rawPass = AKDouble::CurrentPass();
    const char* const source = rawPass == -1 ? "gameplay-pre" : "gameplay";

    if (AKMenu::IsOpen())
    {
        __try
        {
            if (!g_menuCamHeld)
            {
                for (int i = 0; i < 3; ++i)
                {
                    g_menuHeldLocation[i] = location[i];
                    g_menuHeldRotation[i] = rotation[i];
                }
                g_menuCamHeld = true;
                AKProbeLog("[AKMENUCAM] camera held at loc=(%.1f %.1f %.1f) rot=(%d %d %d)",
                           static_cast<double>(g_menuHeldLocation[0]),
                           static_cast<double>(g_menuHeldLocation[1]),
                           static_cast<double>(g_menuHeldLocation[2]),
                           g_menuHeldRotation[0], g_menuHeldRotation[1], g_menuHeldRotation[2]);
            }
            for (int i = 0; i < 3; ++i)
            {
                location[i] = g_menuHeldLocation[i];
                rotation[i] = g_menuHeldRotation[i];
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        // No ApplyHeadRotation: the held rotation already carries the head contribution from the
        // frame the menu opened on, and AKXr freezes the submitted pose tag to match.
    }
    else
    {
        if (g_menuCamHeld)
        {
            g_menuCamHeld = false;
            AKProbeLog("[AKMENUCAM] camera released");
        }
        ApplyHeadRotation(rotation, rawPass, source);
    }

    ApplyEngineEyeOffset(location, rotation, LogicalEngineEyePass(), source);
}

void* CallOriginalCalcSceneViewEyeAware(void* localPlayer, void* viewFamily, void* outLocation,
                                        void* outRotation, void* viewport, void* viewDrawer) noexcept
{
    // Photo mode bypasses +0x9E9ED0 and copies a global debug-camera location/rotation directly.
    // Shift that source only for the duration of view construction, then restore it immediately.
    const int rawPass = AKDouble::CurrentPass();
    const int pass = LogicalEngineEyePass();
    const uintptr_t base = g_engineExeBase.load(std::memory_order_acquire);
    bool photoShifted = false;
    float savedPhotoLocation[3] = {};
    float* photoLocation = nullptr;

    if ((pass == 0 || pass == 1) && base != 0)
    {
        __try
        {
            if (*reinterpret_cast<const int32_t*>(base + kRvaPhotoOverrideFlag) != 0)
            {
                photoLocation = reinterpret_cast<float*>(base + kRvaPhotoLocation);
                const int32_t* photoRotation = reinterpret_cast<const int32_t*>(base + kRvaPhotoRotation);
                savedPhotoLocation[0] = photoLocation[0];
                savedPhotoLocation[1] = photoLocation[1];
                savedPhotoLocation[2] = photoLocation[2];
                photoShifted = ApplyEngineEyeOffset(
                    photoLocation, photoRotation, pass, rawPass == -1 ? "photo-pre" : "photo");
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            photoShifted = false;
            photoLocation = nullptr;
        }
    }

    void* result = g_originalCalcSceneView(
        localPlayer, viewFamily, outLocation, outRotation, viewport, viewDrawer);

    if (photoShifted && photoLocation != nullptr)
    {
        __try
        {
            photoLocation[0] = savedPhotoLocation[0];
            photoLocation[1] = savedPhotoLocation[1];
            photoLocation[2] = savedPhotoLocation[2];
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return result;
}
void* Detour_CalcSceneView(void* localPlayer, void* viewFamily, void* outLocation,
                           void* outRotation, void* viewport, void* viewDrawer) noexcept
{
    // Complete outer-draw stereo renders one ordinary full-screen view family per pass. Keep the
    // right pass on a persistent second FSceneViewState so TAA, occlusion, and other temporal data
    // never bounce left -> right -> left. Restore the ULocalPlayer field immediately after view
    // construction; the returned FSceneView retains the state it was built with.
    if (!g_enabled.load(std::memory_order_relaxed) && AKDouble::CurrentPass() == 1 &&
        g_useSecondViewState.load(std::memory_order_relaxed) && ValidateLocalPlayer(localPlayer))
    {
        void* const originalState = ReadPtr(localPlayer, kLpViewState);
        void* const rightState = EnsureSecondViewState();
        if (originalState != nullptr && rightState != nullptr && rightState != originalState &&
            WritePtr(localPlayer, kLpViewState, rightState))
        {
            void* result = CallOriginalCalcSceneViewEyeAware(
                localPlayer, viewFamily, outLocation, outRotation, viewport, viewDrawer);
            WritePtr(localPlayer, kLpViewState, originalState);
            const auto swaps = g_doubleViewStateSwaps.fetch_add(1, std::memory_order_relaxed) + 1;
            if (swaps <= 3)
                AKProbeLog("[AKVIEW] right-eye temporal state isolated #%llu state=0x%p", swaps, rightState);
            g_passthroughCalls.fetch_add(1, std::memory_order_relaxed);
            return result;
        }
    }

    if (!g_enabled.load(std::memory_order_relaxed))
    {
        g_passthroughCalls.fetch_add(1, std::memory_order_relaxed);
        return CallOriginalCalcSceneViewEyeAware(localPlayer, viewFamily, outLocation, outRotation, viewport, viewDrawer);
    }

    // Re-entrancy guard: the mod's own second call must not split again.
    static thread_local bool s_inside = false;
    if (s_inside)
        return CallOriginalCalcSceneViewEyeAware(localPlayer, viewFamily, outLocation, outRotation, viewport, viewDrawer);

    static std::atomic<bool> s_validated{false};
    if (!s_validated.load(std::memory_order_relaxed))
    {
        if (!ValidateLocalPlayer(localPlayer))
        {
            HardDisable("ULocalPlayer failed validation — RVAs likely stale for this game build");
            g_passthroughCalls.fetch_add(1, std::memory_order_relaxed);
            return CallOriginalCalcSceneViewEyeAware(localPlayer, viewFamily, outLocation, outRotation, viewport, viewDrawer);
        }
        s_validated.store(true, std::memory_order_relaxed);
        AKProbeLog("[AKSPLIT] ULocalPlayer validated, splitting from here");
    }

    float ox = 0, oy = 0, sx = 0, sy = 0;
    if (!ReadRect(localPlayer, &ox, &oy, &sx, &sy))
    {
        g_passthroughCalls.fetch_add(1, std::memory_order_relaxed);
        return CallOriginalCalcSceneViewEyeAware(localPlayer, viewFamily, outLocation, outRotation, viewport, viewDrawer);
    }

    // Only split a player that owns the whole screen. If the game is already running its own
    // split (co-op, picture-in-picture, a security-camera view), leave it entirely alone.
    if (sx < 0.95f || sy < 0.95f)
    {
        g_passthroughCalls.fetch_add(1, std::memory_order_relaxed);
        return CallOriginalCalcSceneViewEyeAware(localPlayer, viewFamily, outLocation, outRotation, viewport, viewDrawer);
    }

    void* const originalViewState = ReadPtr(localPlayer, kLpViewState);
    void* leftView = nullptr;

    const bool rightFirst = g_rightHalfFirst.load(std::memory_order_relaxed);

    // Eye 0 = left half, eye 1 = right half. The engine returns the primary from the FIRST call,
    // so whichever half renders first is what gets handed back as primary.
    const float rectX[2]  = {0.0f, 0.5f};   // indexed by eye
    const int   firstEye  = rightFirst ? 1 : 0;
    const int   secondEye = rightFirst ? 0 : 1;

    s_inside = true;
    __try
    {
        // ---- FIRST VIEW ----
        WriteRect(localPlayer, rectX[firstEye], 0.0f, 0.5f, 1.0f);
        g_currentEye.store(firstEye, std::memory_order_relaxed);
        leftView = CallOriginalCalcSceneViewEyeAware(localPlayer, viewFamily, outLocation, outRotation, viewport, viewDrawer);

        // ---- SECOND VIEW ----
        // Optionally give it its own FSceneViewState (temporal isolation) — but a fresh state
        // crushes AK's auto-exposure to black, so this is OFF by default and the state is shared instead.
        if (g_useSecondViewState.load(std::memory_order_relaxed))
        {
            void* second = EnsureSecondViewState();
            if (second != nullptr) WritePtr(localPlayer, kLpViewState, second);
        }

        WriteRect(localPlayer, rectX[secondEye], 0.0f, 0.5f, 1.0f);
        g_currentEye.store(secondEye, std::memory_order_relaxed);
        void* secondView = CallOriginalCalcSceneViewEyeAware(localPlayer, viewFamily, outLocation, outRotation, viewport, viewDrawer);

        // The render loop sets the per-view render flag on the primary (left) view only. Mirror it
        // onto this second view so it gets the same post/lighting treatment — the dark-right fix.
        if (g_mirrorRenderFlag.load(std::memory_order_relaxed) && secondView != nullptr && leftView != nullptr)
        {
            __try
            {
                const int32_t primaryFlag = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(leftView) + kFsvRenderFlag);
                *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(secondView) + kFsvRenderFlag) =
                    (primaryFlag != 0) ? primaryFlag : 1;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        HardDisable("exception inside the split — falling back to a single view");
    }

    // Restore on EVERY path, including the exception path. Leaving the player on a half-width
    // rect would half the game permanently.
    if (originalViewState != nullptr) WritePtr(localPlayer, kLpViewState, originalViewState);
    WriteRect(localPlayer, ox, oy, sx, sy);
    g_currentEye.store(-1, std::memory_order_relaxed);
    s_inside = false;

    g_splitCalls.fetch_add(1, std::memory_order_relaxed);

    // Hand the engine the left view as the primary, exactly as the MELE method does.
    return leftView;
}
}   // namespace

namespace AKSplit
{
void Install() noexcept
{
    if (g_installed.exchange(true)) return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0)
    {
        HardDisable("no main module");
        return;
    }

    g_allocateViewState = reinterpret_cast<AllocateViewStateFn>(base + kRvaAllocateViewState);

    const MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED)
    {
        HardDisable("MinHook init failed");
        return;
    }

    void* target = reinterpret_cast<void*>(base + kRvaCalcSceneView);
    const MH_STATUS created = MH_CreateHook(target, reinterpret_cast<void*>(&Detour_CalcSceneView),
                                            reinterpret_cast<void**>(&g_originalCalcSceneView));
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED)
    {
        HardDisable("MH_CreateHook(CalcSceneView) failed");
        return;
    }
    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED)
    {
        HardDisable("MH_EnableHook(CalcSceneView) failed");
        return;
    }

    AKProbeLog("[AKSPLIT] CalcSceneView hooked at exe+0x%llX (base=0x%p) allocViewState=exe+0x%llX",
               static_cast<unsigned long long>(kRvaCalcSceneView), reinterpret_cast<void*>(base),
               static_cast<unsigned long long>(kRvaAllocateViewState));
}

void SetEnabled(bool on) noexcept
{
    g_enabled.store(on, std::memory_order_relaxed);
    AKProbeLog("[AKSPLIT] same-frame split = %s", on ? "ON" : "OFF");
}

bool IsEnabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }
bool IsSplitting() noexcept { return g_splitCalls.load(std::memory_order_relaxed) > 0; }
int CurrentEye() noexcept { return g_currentEye.load(std::memory_order_relaxed); }

void ToggleSecondViewState() noexcept
{
    const bool on = !g_useSecondViewState.load(std::memory_order_relaxed);
    g_useSecondViewState.store(on, std::memory_order_relaxed);
    AKProbeLog("[AKVIEW] per-eye temporal state = %s", on ? "ON (isolated)" : "OFF (shared)");
}
bool UsesSecondViewState() noexcept { return g_useSecondViewState.load(std::memory_order_relaxed); }

void ToggleRenderOrder() noexcept
{
    const bool on = !g_rightHalfFirst.load(std::memory_order_relaxed);
    g_rightHalfFirst.store(on, std::memory_order_relaxed);
    AKProbeLog("[AKSPLIT] render order = %s first", on ? "RIGHT" : "LEFT");
}
bool RightHalfFirst() noexcept { return g_rightHalfFirst.load(std::memory_order_relaxed); }

void ToggleMirrorRenderFlag() noexcept
{
    const bool on = !g_mirrorRenderFlag.load(std::memory_order_relaxed);
    g_mirrorRenderFlag.store(on, std::memory_order_relaxed);
    AKProbeLog("[AKSPLIT] mirror per-view render flag (+0x2C0) = %s", on ? "ON" : "OFF");
}
bool MirrorsRenderFlag() noexcept { return g_mirrorRenderFlag.load(std::memory_order_relaxed); }

void NoteViewport(float topLeftX, float width) noexcept
{
    g_lastViewportX.store(topLeftX, std::memory_order_relaxed);
    g_lastViewportW.store(width, std::memory_order_relaxed);
}

unsigned long long SplitCalls() noexcept { return g_splitCalls.load(std::memory_order_relaxed); }
unsigned long long PassthroughCalls() noexcept { return g_passthroughCalls.load(std::memory_order_relaxed); }
const char* DisabledReason() noexcept { return g_disabledReason; }
}

// ===========================================================================
// AKDouble - same-frame flat stereo at Batman's COMPLETE viewport boundary.
//
// +0x9F87B0 is only the base client draw. Subclass UI, Canvas flush, and the final viewport
// command are outside it. The complete FViewport::Draw boundary is +0xBF8000; its low argument
// bit becomes bPresent. Render left with that bit clear, fence/capture it, reset the client guard,
// then render right normally. PresentHook captures the finalized right buffer and composes flat SBS.
// ===========================================================================

void AKProbeCaptureEye(int eye) noexcept;

namespace
{
constexpr uintptr_t kRvaClientDraw = 0x9F87B0;
constexpr uintptr_t kRvaOuterDraw = 0xBF8000;
constexpr uintptr_t kRvaFenceEnqueue = 0xDC5570;
constexpr uintptr_t kRvaFenceWait = 0xDC5660;
using ClientDrawFn = void(*)(void*, void*, void*);
using OuterDrawFn = void(*)(void*, unsigned int);
using FenceEnqueueFn = void(*)(volatile long*);
using FenceWaitFn = void(*)(volatile unsigned long*, unsigned int);
ClientDrawFn g_clientDraw = nullptr;
OuterDrawFn g_outerDraw = nullptr;
FenceEnqueueFn g_fenceEnqueue = nullptr;
FenceWaitFn g_fenceWait = nullptr;
std::atomic<bool> g_dblInstalled{false};
std::atomic<bool> g_dblEnabled{true};
std::atomic<int> g_dblPass{-1};
std::atomic<void*> g_clientSelf{nullptr};
std::atomic<bool> g_rightPending{false};
std::atomic<unsigned long long> g_dblFrames{0};
std::atomic<unsigned long long> g_fenceWaits{0};
std::atomic<unsigned long long> g_guardSkips{0};
const char* g_dblStatus = "not installed";

bool ClearFrameGuard(void* self) noexcept
{
    __try
    {
        if (!self) return false;
        void* canvas = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(self) + 0x48);
        if (!canvas) return false;
        *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(canvas) + 0x94) = nullptr;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SyncRenderThread() noexcept
{
    if (!g_fenceEnqueue || !g_fenceWait) return false;
    volatile long pending = 0;
    __try
    {
        g_fenceEnqueue(&pending);
        g_fenceWait(reinterpret_cast<volatile unsigned long*>(&pending), 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_dblStatus = "render fence exception";
        return false;
    }
    const auto n = g_fenceWaits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4 || (n % 1200) == 0) AKProbeLog("[AKDOUBLE] render-thread fence complete #%llu", n);
    return true;
}

void Detour_ClientDraw(void* self, void* viewport, void* canvas) noexcept
{
    g_clientSelf.store(self, std::memory_order_release);
    g_clientDraw(self, viewport, canvas);
}

void Detour_OuterDraw(void* self, unsigned int flags) noexcept
{
    if (!g_dblEnabled.load(std::memory_order_relaxed) || AKSplit::IsEnabled() || !(flags & 1u))
    {
        g_preDrawLeftWindow.store(false, std::memory_order_release);
        g_engineEyePassMask.store(0, std::memory_order_release);
        g_dblPass.store(-1, std::memory_order_relaxed);
        g_outerDraw(self, flags);
        g_preDrawLeftWindow.store(true, std::memory_order_release);
        return;
    }
    static thread_local bool inside = false;
    if (inside) { g_outerDraw(self, flags); return; }
    inside = true;
    g_rightPending.store(false, std::memory_order_release);
    g_clientSelf.store(nullptr, std::memory_order_release);

    // Eye 0 was constructed just before this function. Preserve its mask through the first draw.
    g_preDrawLeftWindow.store(false, std::memory_order_release);
    g_dblPass.store(0, std::memory_order_relaxed);
    g_outerDraw(self, flags & ~1u); // complete left draw, deliberately no flip
    if (!SyncRenderThread())
    {
        g_dblStatus = "left outer-draw fence failed; disabled";
        g_dblEnabled.store(false, std::memory_order_relaxed);
        g_dblPass.store(-1, std::memory_order_relaxed);
        g_engineEyePassMask.store(0, std::memory_order_release);
        g_preDrawLeftWindow.store(true, std::memory_order_release);
        inside = false;
        return;
    }
    AKProbeCaptureEye(0);
    if (!ClearFrameGuard(g_clientSelf.load(std::memory_order_acquire)))
    {
        // Loading and world transitions can run the outer viewport without a usable client Canvas.
        // Present this frame normally and retry next frame; this is not a permanent stereo fault.
        const auto skip = g_guardSkips.fetch_add(1, std::memory_order_relaxed) + 1;
        g_dblStatus = "transient: client guard unavailable; retrying";
        g_dblPass.store(-1, std::memory_order_relaxed);
        g_rightPending.store(false, std::memory_order_release);
        g_engineEyePassMask.store(0, std::memory_order_release);
        g_outerDraw(self, flags);
        if (skip <= 4 || (skip % 120) == 0)
            AKProbeLog("[AKDOUBLE] transient client guard unavailable; mono frame, retry #%llu", skip);
        g_preDrawLeftWindow.store(true, std::memory_order_release);
        inside = false;
        return;
    }

    g_engineEyePassMask.fetch_and(~2u, std::memory_order_release);
    g_dblPass.store(1, std::memory_order_relaxed);
    g_rightPending.store(true, std::memory_order_release);
    g_outerDraw(self, flags); // real Present captures/composes the completed right eye
    if (!SyncRenderThread())
    {
        g_rightPending.store(false, std::memory_order_release);
        g_dblStatus = "right outer-draw fence failed; disabled";
        g_dblEnabled.store(false, std::memory_order_relaxed);
    }
    else
    {
        bool pending = true;
        if (g_rightPending.compare_exchange_strong(pending, false, std::memory_order_acq_rel))
        {
            AKProbeCaptureEye(1);
            AKProbeLog("[AKDOUBLE] right capture completed after fence (deferred Present)");
        }
        g_dblStatus = "running: complete outer-draw stereo -> flat Present";
        g_dblFrames.fetch_add(1, std::memory_order_relaxed);
    }
    g_dblPass.store(-1, std::memory_order_relaxed);
    g_engineEyePassMask.store(0, std::memory_order_release);
    g_preDrawLeftWindow.store(true, std::memory_order_release);
    inside = false;
}
}

namespace AKDouble
{
void Install() noexcept
{
    if (g_dblInstalled.exchange(true)) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) { g_dblStatus = "no main module"; return; }
    static constexpr unsigned char outerSig[] = {0x48,0x8B,0xC4,0x55,0x56,0x57,0x41,0x54};
    static constexpr unsigned char clientSig[] = {0x48,0x8B,0xC4,0x4C,0x89,0x40,0x18,0x48};
    static constexpr unsigned char enqueueSig[] = {0x40,0x53,0x48,0x83,0xEC,0x60};
    static constexpr unsigned char waitSig[] = {0x48,0x89,0x5C,0x24,0x20,0x57};
    static constexpr unsigned char cameraSig[] = {0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83};
    bool valid = false;
    bool cameraValid = false;
    __try
    {
        valid = !std::memcmp(reinterpret_cast<void*>(base+kRvaOuterDraw), outerSig, sizeof(outerSig)) &&
                !std::memcmp(reinterpret_cast<void*>(base+kRvaClientDraw), clientSig, sizeof(clientSig)) &&
                !std::memcmp(reinterpret_cast<void*>(base+kRvaFenceEnqueue), enqueueSig, sizeof(enqueueSig)) &&
                !std::memcmp(reinterpret_cast<void*>(base+kRvaFenceWait), waitSig, sizeof(waitSig));
        cameraValid = !std::memcmp(reinterpret_cast<void*>(base+kRvaEngineCameraView), cameraSig, sizeof(cameraSig));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { valid = false; }
    if (!valid)
    {
        g_dblEnabled.store(false, std::memory_order_relaxed);
        g_dblStatus = "outer-draw/fence signature mismatch; stock mono";
        AKProbeLog("[AKDOUBLE] DISABLED: outer-draw signatures mismatch");
        return;
    }
    g_engineExeBase.store(base, std::memory_order_release);
    g_fenceEnqueue = reinterpret_cast<FenceEnqueueFn>(base+kRvaFenceEnqueue);
    g_fenceWait = reinterpret_cast<FenceWaitFn>(base+kRvaFenceWait);
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) { g_dblStatus = "MinHook init failed"; return; }
    void* client = reinterpret_cast<void*>(base+kRvaClientDraw);
    void* outer = reinterpret_cast<void*>(base+kRvaOuterDraw);
    const MH_STATUS c1 = MH_CreateHook(client, reinterpret_cast<void*>(&Detour_ClientDraw), reinterpret_cast<void**>(&g_clientDraw));
    const MH_STATUS c2 = MH_CreateHook(outer, reinterpret_cast<void*>(&Detour_OuterDraw), reinterpret_cast<void**>(&g_outerDraw));
    if ((c1 != MH_OK && c1 != MH_ERROR_ALREADY_CREATED) || (c2 != MH_OK && c2 != MH_ERROR_ALREADY_CREATED))
    {
        g_dblEnabled.store(false, std::memory_order_relaxed); g_dblStatus = "MH_CreateHook failed"; return;
    }
    const MH_STATUS e1 = MH_EnableHook(client);
    const MH_STATUS e2 = MH_EnableHook(outer);
    if ((e1 != MH_OK && e1 != MH_ERROR_ENABLED) || (e2 != MH_OK && e2 != MH_ERROR_ENABLED))
    {
        g_dblEnabled.store(false, std::memory_order_relaxed); g_dblStatus = "MH_EnableHook failed"; return;
    }
    if (cameraValid)
    {
        void* camera = reinterpret_cast<void*>(base + kRvaEngineCameraView);
        const MH_STATUS c3 = MH_CreateHook(camera, reinterpret_cast<void*>(&Detour_EngineCameraView),
                                           reinterpret_cast<void**>(&g_originalEngineCameraView));
        if (c3 == MH_OK || c3 == MH_ERROR_ALREADY_CREATED)
        {
            const MH_STATUS e3 = MH_EnableHook(camera);
            if (e3 == MH_OK || e3 == MH_ERROR_ENABLED)
            {
                g_engineCameraHookReady.store(true, std::memory_order_release);
                AKProbeLog("[AKENGINEEYE] gameplay camera source +0x%llX hooked; photo globals +0x%llX/+0x%llX",
                           static_cast<unsigned long long>(kRvaEngineCameraView),
                           static_cast<unsigned long long>(kRvaPhotoLocation),
                           static_cast<unsigned long long>(kRvaPhotoRotation));
            }
            else
            {
                AKProbeLog("[AKENGINEEYE] gameplay camera hook enable failed (%d); GPU VP fallback remains active", static_cast<int>(e3));
            }
        }
        else
        {
            AKProbeLog("[AKENGINEEYE] gameplay camera hook create failed (%d); GPU VP fallback remains active", static_cast<int>(c3));
        }
    }
    else
    {
        AKProbeLog("[AKENGINEEYE] gameplay camera signature mismatch; GPU VP fallback remains active");
    }
    g_dblStatus = "complete outer viewport draw hooked; flat-only";
    AKProbeLog("[AKDOUBLE] COMPLETE outer draw +0x%llX; client observer +0x%llX; fence +0x%llX/+0x%llX validated",
        static_cast<unsigned long long>(kRvaOuterDraw), static_cast<unsigned long long>(kRvaClientDraw),
        static_cast<unsigned long long>(kRvaFenceEnqueue), static_cast<unsigned long long>(kRvaFenceWait));
}
void SetEnabled(bool on) noexcept
{
    g_dblEnabled.store(on, std::memory_order_relaxed);
    if (!on) g_rightPending.store(false, std::memory_order_release);
    AKProbeLog("[AKDOUBLE] complete outer double render = %s", on ? "ON" : "OFF");
}
bool IsEnabled() noexcept { return g_dblEnabled.load(std::memory_order_relaxed); }
int CurrentPass() noexcept { return g_dblPass.load(std::memory_order_relaxed); }
bool EngineEyeCameraApplied(int pass) noexcept
{
    if (pass != 0 && pass != 1) return false;
    return (g_engineEyePassMask.load(std::memory_order_acquire) & (1u << pass)) != 0;
}
bool CaptureRightAtPresent() noexcept
{
    if (!g_dblEnabled.load(std::memory_order_relaxed) || AKSplit::IsEnabled()) return false;
    bool pending = true;
    if (!g_rightPending.compare_exchange_strong(pending, false, std::memory_order_acq_rel)) return false;
    AKProbeCaptureEye(1);
    return true;
}
bool IsRunning() noexcept { return g_dblFrames.load(std::memory_order_relaxed) > 0; }
unsigned long long Frames() noexcept { return g_dblFrames.load(std::memory_order_relaxed); }
const char* Status() noexcept { return g_dblStatus; }
unsigned long long EngineEyeShifts() noexcept { return g_engineEyeShifts.load(std::memory_order_relaxed); }
float EngineEyeLastShift(int pass) noexcept
{
    if (pass != 0 && pass != 1) return 0.0f;
    return g_engineEyeLastShift[pass].load(std::memory_order_relaxed);
}
}
// ===========================================================================
// AKFreeze — TRUE same-frame stereo by freezing the clock between the two eyes.
//
// The double-call approach (AKDouble) fails because it gives the engine TWO view families in one
// game frame, and AK's per-frame systems are not built - Hinny that GOAT - for that — texture streaming picks mips
// from the active views, gets inconsistent state, and the render thread dies on a freed texture.
//
// So don't do that. Let the engine render ONE completely normal frame per eye, and stop the clock
// between them:
//
//     frame N   : time advances, simulation ticks, render LEFT eye
//     frame N+1 : time FROZEN  -> simulation cannot advance, render RIGHT eye
//
// Both eyes are then the same simulation instant — genuinely the same frame's worth of world,
// which is what "same-frame" means. This is NOT alternate-eye rendering: AER's defect is that the
// world moves between the eyes, and here it provably cannot.
//
// The engine sees exactly one view family per frame, so streaming, the render thread and every
// other per-frame system run untouched. No race, no crash.
//
// Freezing is done at the source: UE3 derives its frame delta from QueryPerformanceCounter, so
// while frozen the hook hands back the counter value captured at the start of the pair. Delta computes
// as zero and nothing in the simulation can move.
// ===========================================================================

namespace
{
using QpcFn = BOOL(WINAPI*)(LARGE_INTEGER*);
QpcFn g_originalQpc = nullptr;

std::atomic<bool> g_freezeInstalled{false};
std::atomic<bool> g_freezeEnabled{false};
std::atomic<bool> g_timeFrozen{false};
std::atomic<long long> g_frozenCounter{0};
std::atomic<unsigned long long> g_frozenFrames{0};
const char* g_freezeStatus = "not installed";

BOOL WINAPI Detour_QueryPerformanceCounter(LARGE_INTEGER* counter) noexcept
{
    const BOOL ok = g_originalQpc(counter);
    if (!ok || counter == nullptr) return ok;

    if (g_timeFrozen.load(std::memory_order_relaxed))
    {
        // Hand back the instant the pair started — the engine sees zero elapsed time.
        counter->QuadPart = g_frozenCounter.load(std::memory_order_relaxed);
    }
    else
    {
        // Running normally: keep the freeze point up to date.
        g_frozenCounter.store(counter->QuadPart, std::memory_order_relaxed);
    }
    return ok;
}
}   // namespace

namespace AKFreeze
{
void Install() noexcept
{
    if (g_freezeInstalled.exchange(true)) return;

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) { g_freezeStatus = "MinHook init failed"; return; }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    void* target = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "QueryPerformanceCounter")) : nullptr;
    if (target == nullptr) { g_freezeStatus = "QueryPerformanceCounter not found"; return; }

    const MH_STATUS created = MH_CreateHook(target, reinterpret_cast<void*>(&Detour_QueryPerformanceCounter),
                                            reinterpret_cast<void**>(&g_originalQpc));
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) { g_freezeStatus = "MH_CreateHook(QPC) failed"; return; }
    if (MH_EnableHook(target) != MH_OK) { g_freezeStatus = "MH_EnableHook(QPC) failed"; return; }

    g_freezeStatus = "hooked";
    AKProbeLog("[AKFREEZE] QueryPerformanceCounter hooked — clock freeze available (true same-frame)");
}

// Called once per present with the eye the NEXT frame will render. The second eye of each pair
// runs with the clock stopped, so it shows the same instant as the first.
void SetNextEye(int nextEye) noexcept
{
    if (!g_freezeEnabled.load(std::memory_order_relaxed) || g_originalQpc == nullptr)
    {
        if (g_timeFrozen.exchange(false, std::memory_order_relaxed))
            AKProbeLog("[AKFREEZE] thawed (freeze disabled)");
        return;
    }

    const bool freeze = (nextEye == 1);
    const bool was = g_timeFrozen.exchange(freeze, std::memory_order_relaxed);
    if (freeze && !was) g_frozenFrames.fetch_add(1, std::memory_order_relaxed);
    g_freezeStatus = "running";
}

void SetEnabled(bool on) noexcept
{
    g_freezeEnabled.store(on, std::memory_order_relaxed);
    if (!on) g_timeFrozen.store(false, std::memory_order_relaxed);
    AKProbeLog("[AKFREEZE] clock freeze (true same-frame) = %s", on ? "ON" : "OFF");
}

bool IsEnabled() noexcept { return g_freezeEnabled.load(std::memory_order_relaxed); }
bool IsFrozenNow() noexcept { return g_timeFrozen.load(std::memory_order_relaxed); }
unsigned long long FrozenFrames() noexcept { return g_frozenFrames.load(std::memory_order_relaxed); }
const char* Status() noexcept { return g_freezeStatus; }
}
