#include "headtracking_mod.h"
#include "logging.h"
#include "reticle.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include <windows.h>
#include <psapi.h>

#include "builds/build_registry.h"

#include "cameraunlock/config/ini_reader.h"
#include "cameraunlock/diagnostics/crash_handler.h"
#include "cameraunlock/protocol/udp_receiver.h"
#include "cameraunlock/input/chord_hotkeys.h"
#include "cameraunlock/input/hotkey_poller.h"
#include "cameraunlock/hooks/hook_manager.h"
#include "cameraunlock/processing/pose_interpolator.h"
#include "cameraunlock/processing/position_interpolator.h"
#include "cameraunlock/processing/position_processor.h"
#include "cameraunlock/data/position_data.h"
#include "cameraunlock/data/position_settings.h"
#include "cameraunlock/math/smoothing_utils.h"
#include "cameraunlock/math/quat4.h"
#include "cameraunlock/time/frame_clock.h"
#include "cameraunlock/unreal/ue_math.h"
#include "cameraunlock/unreal/ue_runtime.h"

#ifndef RVTY_MOD_VERSION
#define RVTY_MOD_VERSION "0.0.0"
#endif
#ifndef RVTY_GIT_SHA
#define RVTY_GIT_SHA "unknown"
#endif

// Dev/diagnostic controls, off in shipping builds: the reticle scale-tuning
// F-keys (F7-F10), the reticle test-nudge (Ctrl+Shift+J), the widget-name dump
// (Ctrl+Shift+U), and the per-frame aim/reticle log spam. Define to 1
// (-DRVTY_DEV_HOTKEYS=1) to re-arm them when tuning a new HUD. The production
// reticle self-calibrates (widget geometry scale), so none are needed to play.
#ifndef RVTY_DEV_HOTKEYS
#define RVTY_DEV_HOTKEYS 0
#endif

namespace RVThereYetHeadTracking
{
    namespace ue = ::cameraunlock::unreal;

    namespace
    {
        using ue::FVector;
        using ue::FQuat4d;
        using ue::FRotator;
        using ue::QuatFromEulerDeg;
        using ue::QuatMul;
        using ue::QuatToRotator;
        using ue::QuatRotateVec;
        using cameraunlock::input::ChordGuarded;
        using cameraunlock::time::FrameClock;

        // ---- Runtime state ----
        std::unique_ptr<cameraunlock::UdpReceiver>       g_receiver;
        std::unique_ptr<cameraunlock::input::HotkeyPoller> g_hotkeys;

        std::atomic<bool> g_trackingEnabled{true};
        std::atomic<bool> g_worldSpaceYaw{true};

        // Master rotation/position gates cycled by the tracking-mode hotkey:
        //   0 normal (both), 1 rotation only, 2 position only.
        std::atomic<bool> g_rotationEnabled{true};
        std::atomic<bool> g_positionEnabled{true};
        std::atomic<int>  g_trackingMode{0};

        // True only while a gameplay HUD is live (set off-thread). Suppresses
        // tracking in menus / loading so the view isn't head-tracked there.
        std::atomic<bool> g_inGameplay{false};

        // Worker-thread lifecycle for the manual-FreeLibrary Shutdown() path:
        // both threads must be joined before the DLL unmaps, or they'd keep
        // executing unmapped code. The process-exit path never touches these
        // (see EmergencyShutdown).
        std::atomic<bool> g_stopThreads{false};
        HANDLE g_bootstrapThread = nullptr;
        HANDLE g_gameStateThread = nullptr;

        // Rotation pipeline. The view-builder hook fires on the render thread
        // only, so this is single-thread access - no locks needed.
        cameraunlock::PoseInterpolator g_interp;
        std::int64_t g_lastSampleTs = 0;
        FrameClock g_rotClock;
        float g_smoothedYaw = 0.0f, g_smoothedPitch = 0.0f, g_smoothedRoll = 0.0f;
        bool  g_hasSmoothed = false;

        float g_userSmoothing = 0.0f;
        float g_yawSens = 1.0f, g_pitchSens = 1.0f, g_rollSens = 1.0f;
        bool  g_invertYaw = false, g_invertPitch = false, g_invertRoll = false;

        // Position (6DOF) pipeline.
        cameraunlock::PositionProcessor   g_posProcessor;
        cameraunlock::PositionInterpolator g_posInterp;
        FrameClock g_posClock;
        std::atomic<bool> g_posCenterPending{true};

        // High-resolution timer for sub-frame frame-boundary detection.
        std::uint64_t QpcNow()
        {
            LARGE_INTEGER v;
            QueryPerformanceCounter(&v);
            return static_cast<std::uint64_t>(v.QuadPart);
        }
        std::uint64_t QpcFreq()
        {
            static const std::uint64_t f = [] {
                LARGE_INTEGER v;
                QueryPerformanceFrequency(&v);
                return static_cast<std::uint64_t>(v.QuadPart);
            }();
            return f;
        }

        // Read the tracker, interpolate to frame rate, smooth, apply per-axis
        // sensitivity/inversion. Returns false when no tracker data is
        // available, in which case the view is left clean (hold-vanilla).
        bool GetProcessedRotation(float& outYaw, float& outPitch, float& outRoll)
        {
            float rawYaw = 0.0f, rawPitch = 0.0f, rawRoll = 0.0f;
            if (!g_receiver->GetRotation(rawYaw, rawPitch, rawRoll)) {
                return false;
            }

            const float dt = g_rotClock.Tick();
            const std::int64_t ts = g_receiver->GetLastReceiveTimestamp();
            const bool isNew = (ts != g_lastSampleTs);
            g_lastSampleTs = ts;

            auto interp = g_interp.Update(rawYaw, rawPitch, rawRoll, isNew, dt);

            const float eff = static_cast<float>(
                cameraunlock::math::GetEffectiveSmoothing(g_userSmoothing));
            if (!g_hasSmoothed) {
                g_smoothedYaw = interp.yaw;
                g_smoothedPitch = interp.pitch;
                g_smoothedRoll = interp.roll;
                g_hasSmoothed = true;
            } else {
                g_smoothedYaw   = cameraunlock::math::Smooth(g_smoothedYaw,   interp.yaw,   eff, dt);
                g_smoothedPitch = cameraunlock::math::Smooth(g_smoothedPitch, interp.pitch, eff, dt);
                g_smoothedRoll  = cameraunlock::math::Smooth(g_smoothedRoll,  interp.roll,  eff, dt);
            }
            outYaw   = g_smoothedYaw   * (g_invertYaw   ? -g_yawSens   : g_yawSens);
            outPitch = g_smoothedPitch * (g_invertPitch ? -g_pitchSens : g_pitchSens);
            outRoll  = g_smoothedRoll  * (g_invertRoll  ? -g_rollSens  : g_rollSens);
            return true;
        }

        // Advance the position pipeline (view-independent) and return the
        // clamped head offset in tracker axes, UE units: surge(forward),
        // sway(right), heave(up). Called once per frame; the per-view basis
        // projection is ProjectPositionOffset below. Returns false when
        // position is disabled or no tracker position is available.
        bool ComputePositionOffsetTracker(float yaw, float pitch, float roll,
                                          double& surge, double& sway, double& heave)
        {
            if (!g_positionEnabled.load(std::memory_order_relaxed)) return false;
            float px = 0.0f, py = 0.0f, pz = 0.0f;
            if (!g_receiver->GetPosition(px, py, pz)) return false;

            const float dt = g_posClock.Tick();
            const std::int64_t ts = g_receiver->GetLastReceiveTimestamp();
            cameraunlock::PositionData raw(px, py, pz, ts);

            if (g_posCenterPending.exchange(false)) {
                g_posProcessor.SetCenter(raw);
                g_posProcessor.ResetSmoothing();
                g_posInterp.Reset();
            }

            const cameraunlock::PositionData interp = g_posInterp.Update(raw, dt);
            const cameraunlock::math::Quat4 headQ =
                cameraunlock::math::Quat4::FromYawPitchRoll(yaw, pitch, roll);
            // Clamped offset in tracker axes, meters: x=sway(right),
            // y=heave(up), z=surge(forward).
            const cameraunlock::math::Vec3 off = g_posProcessor.Process(interp, headQ, dt);

            constexpr double kMetersToUE = 100.0;
            surge = static_cast<double>(off.z) * kMetersToUE;  // forward
            sway  = static_cast<double>(off.x) * kMetersToUE;  // right
            heave = static_cast<double>(off.y) * kMetersToUE;  // up
            return true;
        }

        // Project a tracker-space head offset onto the clean-camera basis so
        // leaning follows body orientation, not the head-rotated view.
        FVector ProjectPositionOffset(const FQuat4d& baseQ,
                                      double surge, double sway, double heave)
        {
            // Clean-camera basis (UE: X=forward, Y=right, Z=up).
            const FVector camFwd   = QuatRotateVec(baseQ, FVector{1.0, 0.0, 0.0});
            const FVector camRight = QuatRotateVec(baseQ, FVector{0.0, 1.0, 0.0});
            const FVector camUp    = QuatRotateVec(baseQ, FVector{0.0, 0.0, 1.0});
            return FVector{
                camFwd.X * surge + camRight.X * sway + camUp.X * heave,
                camFwd.Y * surge + camRight.Y * sway + camUp.Y * heave,
                camFwd.Z * surge + camRight.Z * sway + camUp.Z * heave,
            };
        }

        // ---- View-builder hook ----
        // Signature of FUN_143e72a70: self->Build(outView). RCX = self (holds
        // the PlayerController at +0x30), RDX = the FMinimalViewInfo out-param.
        using ViewBuilder_t = void(__fastcall*)(void* self, void* outView);
        ViewBuilder_t g_origViewBuilder = nullptr;

        std::atomic<std::uint64_t> g_hookCallCount{0};
        std::uint64_t g_bootstrapTick = 0;

        void __fastcall ViewBuilder_Hook(void* self, void* outView)
        {
            // Let the game assemble the clean view first (this writes the final
            // Location/Rotation/FOV, GetPlayerViewPoint included). We rotate the
            // OUTPUT, so game logic that queries GetPlayerViewPoint directly is
            // never affected - aim/physics stay decoupled by construction.
            g_origViewBuilder(self, outView);

            const std::uint64_t n = g_hookCallCount.fetch_add(1, std::memory_order_relaxed) + 1;

            // Heartbeat so "hook never fired" is distinguishable from "no
            // tracker data". Tight for the first 30s, then every 30s.
            {
                static std::atomic<std::uint64_t> s_lastBeat{0};
                const std::uint64_t now = GetTickCount64();
                const std::uint64_t last = s_lastBeat.load(std::memory_order_relaxed);
                const std::uint64_t interval = (now - g_bootstrapTick) < 30000 ? 2000 : 30000;
                if (n == 1 || (now - last) >= interval) {
                    s_lastBeat.store(now, std::memory_order_relaxed);
                    Log::Line("heartbeat: builder hook fired %llu times",
                        static_cast<unsigned long long>(n));
                }
            }

            if (!g_trackingEnabled.load(std::memory_order_relaxed)) {
                reticle::ResetIfOffset();
                return;
            }
            // Menus / loading: leave the clean (vanilla) view untouched.
            if (!g_inGameplay.load(std::memory_order_relaxed)) {
                reticle::ResetIfOffset();
                return;
            }

            // The builder fires once per rendered VIEW (main view + mirrors +
            // reflections), i.e. several times per frame. Advance the tracking
            // pipeline (FrameClock dt, interpolators, smoothing) only ONCE per
            // frame and reuse the result for the frame's other views, so the
            // frame-rate-independent smoothing/extrapolation isn't over-stepped.
            // A new frame is detected by a QPC gap larger than the sub-frame
            // inter-view spacing (which is microseconds) but smaller than a
            // frame period.
            static thread_local std::uint64_t s_lastPoseQpc = 0;
            static thread_local float  cYaw = 0.0f, cPitch = 0.0f, cRoll = 0.0f;
            static thread_local bool   cRotValid = false;
            static thread_local double cSurge = 0.0, cSway = 0.0, cHeave = 0.0;
            static thread_local bool   cPosValid = false;

            const std::uint64_t qpc = QpcNow();
            if (qpc - s_lastPoseQpc > QpcFreq() / 2000) {  // > 0.5 ms => new frame
                s_lastPoseQpc = qpc;
                float y = 0.0f, p = 0.0f, r = 0.0f;
                cRotValid = GetProcessedRotation(y, p, r);
                // Master rotation gate (mode 2 = position only). Zeroed values
                // still feed the position pivot-compensation.
                if (cRotValid && !g_rotationEnabled.load(std::memory_order_relaxed)) {
                    y = 0.0f; p = 0.0f; r = 0.0f;
                }
                cYaw = y; cPitch = p; cRoll = r;
                cPosValid = cRotValid && ComputePositionOffsetTracker(y, p, r, cSurge, cSway, cHeave);
            }
            if (!cRotValid) {
                reticle::ResetIfOffset();
                return;  // no tracker data - leave the clean view untouched
            }
            const float yaw = cYaw, pitch = cPitch, roll = cRoll;

            const auto rotAddr = reinterpret_cast<FRotator*>(
                reinterpret_cast<std::uintptr_t>(outView)
                + Offsets().MinimalViewInfoLayout.kRotationOffset);
            const FRotator base = *rotAddr;
            const FQuat4d baseQ = QuatFromEulerDeg(base.Pitch, base.Yaw, base.Roll);

            // The final tracked view rotation, captured as a quaternion in both
            // modes so the reticle projection can conjugate the clean aim
            // through it (correct for world-yaw as well as local-yaw).
            FQuat4d viewQ;
            if (!g_worldSpaceYaw.load(std::memory_order_relaxed)) {
                // Camera-local: head rotation post-multiplied in the view frame.
                const FQuat4d headLocalQ = QuatFromEulerDeg(
                    static_cast<double>(pitch),
                    static_cast<double>(yaw),
                    -static_cast<double>(roll));
                viewQ = QuatMul(baseQ, headLocalQ);
                const FRotator fin = QuatToRotator(viewQ);
                rotAddr->Pitch = fin.Pitch;
                rotAddr->Yaw   = fin.Yaw;
                rotAddr->Roll  = fin.Roll;
            } else {
                // World yaw: additive FRotator. Yaw rotates about world Z
                // regardless of pitch (horizon-locked), which reads more
                // naturally when looking around a vehicle cabin.
                rotAddr->Yaw   += yaw;
                rotAddr->Pitch += pitch;
                rotAddr->Roll  -= roll;
                viewQ = QuatFromEulerDeg(rotAddr->Pitch, rotAddr->Yaw, rotAddr->Roll);
            }

            // Position (6DOF): offset the view Location in this view's clean-
            // camera basis so leaning follows body orientation. Uses the
            // per-frame-cached tracker-space offset, projected with THIS view's
            // baseQ (each view has its own orientation).
            if (cPosValid) {
                const FVector posOff = ProjectPositionOffset(baseQ, cSurge, cSway, cHeave);
                const auto locAddr = reinterpret_cast<FVector*>(outView);
                locAddr->X += posOff.X;
                locAddr->Y += posOff.Y;
                locAddr->Z += posOff.Z;
            }

            // Reticle compensation: move the interaction widgets to where the
            // clean aim lands in the tracked view.
            reticle::UpdateFromView(self, outView, baseQ, viewQ);
        }

        bool InstallViewBuilderHook()
        {
            HMODULE exe = GetModuleHandleW(nullptr);
            if (!exe) {
                Log::Line("InstallViewBuilderHook: GetModuleHandle(nullptr) null");
                return false;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(exe);

            // Publish the module range + UObject globals for reflection (used
            // by the reticle-widget finder).
            std::uintptr_t end = base + 0x10000000ULL;
            MODULEINFO mi{};
            if (GetModuleInformation(GetCurrentProcess(), exe, &mi, sizeof(mi))) {
                end = base + mi.SizeOfImage;
            }
            ue::SetRuntime(base, end, Offsets().UObjectGlobals);

            void* target = reinterpret_cast<void*>(base + Offsets().kViewBuilderRva);
            Log::Line("Module base=0x%llx  view-builder target=0x%llx (RVA 0x%llx)",
                static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(target)),
                static_cast<unsigned long long>(Offsets().kViewBuilderRva));

            using cameraunlock::hooks::HookManager;
            using cameraunlock::hooks::HookStatus;
            using cameraunlock::hooks::HookStatusToString;

            auto& hm = HookManager::Instance();
            if (auto s = hm.Initialize();
                s != HookStatus::Ok && s != HookStatus::ErrorAlreadyInitialized) {
                Log::Line("MinHook init failed: %s", HookStatusToString(s));
                return false;
            }
            if (auto s = hm.CreateHook(
                    target,
                    reinterpret_cast<void*>(&ViewBuilder_Hook),
                    reinterpret_cast<void**>(&g_origViewBuilder));
                s != HookStatus::Ok) {
                Log::Line("CreateHook failed: %s", HookStatusToString(s));
                return false;
            }
            if (auto s = hm.EnableHook(target); s != HookStatus::Ok) {
                Log::Line("EnableHook failed: %s", HookStatusToString(s));
                return false;
            }
            Log::Line("Hook installed on FMinimalViewInfo builder");
            return true;
        }

#if RVTY_DEV_HOTKEYS
        // Diagnostic: enumerate live UObjects and log distinct class names
        // that look like UI widgets, so the reticle / interaction-prompt
        // widgets can be identified for aim-point compensation. Triggered by a
        // hotkey (press it while a prompt like "Open Door" is on screen so its
        // widget is instantiated).
        void DumpWidgets()
        {
            if (ue::ModuleBase() == 0) {
                Log::Line("widget-dump: reflection not initialised");
                return;
            }
            // Match on the object's NAME / class / outer so we surface the
            // individual reticle/prompt child widgets (Image_Reticle, the
            // "Door" TextBlock, their container panel) inside the HUD, with
            // enough context (name | class | outer) to pick the one to move.
            static const char* kKeywords[] = {
                "reticle", "crosshair", "cursor", "interact",
                "prompt", "hint", "aim", "door", "objectname", "usetext"
            };
            std::unordered_set<std::string> seen;
            int total = 0, hits = 0;
            ue::ForEachUObject([&](std::uintptr_t obj) {
                ++total;
                const std::string on = ue::ObjectName(obj);
                if (on.empty() || ue::ContainsCI(on, "Default__")) return false;
                const std::string cn = ue::ClassName(obj);
                const std::string ou = ue::OuterName(obj);
                bool match = false;
                for (const char* kw : kKeywords) {
                    if (ue::ContainsCI(on, kw) || ue::ContainsCI(cn, kw) || ue::ContainsCI(ou, kw)) {
                        match = true; break;
                    }
                }
                if (!match) return false;
                if (seen.insert(on + "|" + cn).second) {
                    ++hits;
                    Log::Line("widget-dump: name=%s | class=%s | outer=%s",
                        on.c_str(), cn.c_str(), ou.c_str());
                }
                return false;
            });
            Log::Line("widget-dump: %d UObjects scanned, %d matches", total, hits);
        }
#endif

        std::wstring DllDir(void* hModule)
        {
            wchar_t buf[MAX_PATH] = {};
            GetModuleFileNameW(static_cast<HMODULE>(hModule), buf, MAX_PATH);
            std::wstring path(buf);
            const auto slash = path.find_last_of(L"\\/");
            if (slash != std::wstring::npos) path.resize(slash + 1);
            return path;
        }

        std::string DllDirNarrow(void* hModule)
        {
            char buf[MAX_PATH] = {};
            GetModuleFileNameA(static_cast<HMODULE>(hModule), buf, MAX_PATH);
            std::string path(buf);
            const auto slash = path.find_last_of("\\/");
            if (slash != std::string::npos) path.resize(slash + 1);
            return path;
        }

        // strtod accepts "nan"/"inf" and out-of-float-range values; a
        // non-finite sensitivity/limit would flow into the view rotation we
        // write back to the game every frame and poison it with NaN. Same
        // config-boundary rule as the Port check: fall back with a loud log.
        float ReadFiniteFloat(const cameraunlock::IniReader& ini,
                              const char* section, const char* key, float def)
        {
            const float v = ini.ReadFloat(section, key, def);
            if (!std::isfinite(v)) {
                Log::Line("config: %s.%s is not a finite number; using default %.2f",
                    section, key, def);
                return def;
            }
            return v;
        }

        // Read HeadTracking.ini next to the DLL. Absent file / keys fall back
        // to the shipped defaults (a filesystem boundary, so defaults here are
        // correct). Returns the resolved UDP port and yaw-mode hotkey.
        void LoadConfig(void* module, int& outPort, int& outYawModeKey)
        {
            outPort = cameraunlock::UdpReceiver::kDefaultPort;
            outYawModeKey = 0x22;  // Page Down

            cameraunlock::IniReader ini;
            if (!ini.Open(DllDirNarrow(module) + "HeadTracking.ini")) {
                Log::Line("config: no HeadTracking.ini next to DLL; using defaults");
                return;
            }

            const int cfgPort = ini.ReadInt("Network", "Port", outPort);
            // Port is later narrowed to uint16 for bind(); an out-of-range value
            // would wrap silently and the mod would bind a different port than the
            // user asked for, then appear "loaded but receiving nothing". Validate
            // at this config boundary and fall back to the default with a loud log.
            if (cfgPort < 1 || cfgPort > 65535) {
                Log::Line("config: Port=%d out of range 1-65535; using default %d",
                    cfgPort, outPort);
            } else {
                outPort = cfgPort;
            }
            g_trackingEnabled.store(ini.ReadBool("Tracking", "EnableOnStartup", true));
            g_yawSens   = ReadFiniteFloat(ini, "Tracking", "YawSensitivity", 1.0f);
            g_pitchSens = ReadFiniteFloat(ini, "Tracking", "PitchSensitivity", 1.0f);
            g_rollSens  = ReadFiniteFloat(ini, "Tracking", "RollSensitivity", 1.0f);
            g_invertYaw   = ini.ReadBool("Tracking", "InvertYaw", false);
            g_invertPitch = ini.ReadBool("Tracking", "InvertPitch", false);
            g_invertRoll  = ini.ReadBool("Tracking", "InvertRoll", false);
            g_userSmoothing = ReadFiniteFloat(ini, "Tracking", "Smoothing", 0.0f);
            g_worldSpaceYaw.store(ini.ReadBool("Tracking", "WorldSpaceYaw", true));
            outYawModeKey = ini.ReadHex("Hotkeys", "ToggleYawMode", outYawModeKey);

            cameraunlock::PositionSettings ps = g_posProcessor.GetSettings();
            g_positionEnabled.store(ini.ReadBool("Position", "Enabled", true));
            ps.sensitivity_x = ReadFiniteFloat(ini, "Position", "SensitivityX", 1.0f);
            ps.sensitivity_y = ReadFiniteFloat(ini, "Position", "SensitivityY", 1.0f);
            ps.sensitivity_z = ReadFiniteFloat(ini, "Position", "SensitivityZ", 1.0f);
            ps.invert_x = ini.ReadBool("Position", "InvertX", false);
            ps.invert_y = ini.ReadBool("Position", "InvertY", false);
            ps.invert_z = ini.ReadBool("Position", "InvertZ", false);
            ps.limit_x = ReadFiniteFloat(ini, "Position", "LimitX", 0.30f);
            ps.limit_y = ReadFiniteFloat(ini, "Position", "LimitY", 0.20f);
            ps.limit_z = ReadFiniteFloat(ini, "Position", "LimitZ", 0.40f);
            ps.limit_z_back = ReadFiniteFloat(ini, "Position", "LimitZBack", 0.10f);
            ps.smoothing = ReadFiniteFloat(ini, "Position", "Smoothing", 0.15f);
            g_posProcessor.SetSettings(ps);

            reticle::Settings rs;
            rs.show = ini.ReadBool("Tracking", "ShowReticle", true);
            rs.scale = ReadFiniteFloat(ini, "Reticle", "Scale", 1.0f);
            rs.verticalScale = ReadFiniteFloat(ini, "Reticle", "VerticalScale", 1.0f);
            // Comma-separated widget names to move to the aim point; empty
            // keeps the built-in defaults.
            const std::string names = ini.ReadString("Reticle", "WidgetNames", "");
            if (!names.empty()) {
                std::size_t start = 0;
                while (start <= names.size()) {
                    std::size_t comma = names.find(',', start);
                    std::string tok = names.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                    std::size_t a = tok.find_first_not_of(" \t");
                    std::size_t b = tok.find_last_not_of(" \t");
                    if (a != std::string::npos) rs.targetNames.push_back(tok.substr(a, b - a + 1));
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            }
            reticle::Configure(rs);

            Log::Line("config: port=%d enable=%s sens(Y/P/R)=%.2f/%.2f/%.2f "
                "invert(Y/P/R)=%d/%d/%d smoothing=%.2f worldYaw=%s "
                "position=%s posSmoothing=%.2f",
                outPort, g_trackingEnabled.load() ? "true" : "false",
                g_yawSens, g_pitchSens, g_rollSens,
                g_invertYaw, g_invertPitch, g_invertRoll,
                g_userSmoothing, g_worldSpaceYaw.load() ? "true" : "false",
                g_positionEnabled.load() ? "true" : "false", ps.smoothing);
        }

        // Off-thread gameplay detector. A gameplay HUD widget is live only
        // during actual play (not the main menu / loading screens), so its
        // presence is our in-gameplay signal. Runs every ~400ms off the render
        // path - a full UObject scan inline would hitch the frame. Reflection-
        // based so it needs no game-specific struct offsets.
        DWORD WINAPI GameStateThread(LPVOID)
        {
            static const char* kGameplayHuds[] = {
                "WG_VehicleHUD_C", "WG_PlayerHUD_C", "WG_PlayerInteraction_C"
            };
            while (!g_stopThreads.load(std::memory_order_relaxed)) {
                Sleep(400);
                if (g_stopThreads.load(std::memory_order_relaxed)) break;
                if (ue::ModuleBase() == 0) continue;
                bool found = false;
                ue::ForEachUObject([&](std::uintptr_t obj) -> bool {
                    const std::string cn = ue::ClassName(obj);
                    for (const char* h : kGameplayHuds) {
                        if (cn == h && !ue::ContainsCI(ue::ObjectName(obj), "Default__")) {
                            found = true;
                            return true;  // stop early
                        }
                    }
                    return false;
                });
                const bool was = g_inGameplay.exchange(found);
                if (was != found) {
                    Log::Line("game-state: %s", found ? "GAMEPLAY (tracking active)"
                                                      : "MENU/LOADING (tracking suppressed)");
                }
            }
            return 0;
        }

        DWORD WINAPI BootstrapThread(LPVOID module)
        {
            g_bootstrapTick = GetTickCount64();
            Log::Open(DllDir(module) + L"RVThereYetHeadTracking.log");
            Log::Line("RV There Yet Head Tracking - bootstrap");
            Log::Line("Version: %s (%s)", RVTY_MOD_VERSION, RVTY_GIT_SHA);
            Log::Line("Process: PID=%lu", GetCurrentProcessId());

            cameraunlock::diagnostics::InstallCrashHandler();

            const auto matchResult = builds::SelectProfile(GetModuleHandleW(nullptr));
            if (matchResult != builds::MatchResult::Matched) {
                Log::Line("============================================================");
                Log::Line(" HEAD TRACKING DORMANT - no armed build profile");
                Log::Line(" The mod DLL loaded, but no complete build profile matched");
                Log::Line(" this EXE. The game runs vanilla. Check the Releases page");
                Log::Line(" for an updated mod build:");
                Log::Line(" https://github.com/itsloopyo/rv-there-yet-headtracking/releases");
                Log::Line("============================================================");
                Log::Line("===== bootstrap exited (mod dormant) =====");
                return 0;
            }
            Log::Line("build-check: PASS - matched profile %s",
                builds::ActiveProfile().Name);

            int udpPort = 0, yawModeKey = 0;
            LoadConfig(module, udpPort, yawModeKey);

            g_receiver = std::make_unique<cameraunlock::UdpReceiver>();
            g_receiver->SetLog([](const std::string& msg) { Log::Line("[udp] %s", msg.c_str()); });
            const bool bound = g_receiver->Start(static_cast<std::uint16_t>(udpPort));
            Log::Line("UDP receiver Start(%d) -> %s", udpPort, bound ? "bound" : "retry-scheduled");

            g_hotkeys = std::make_unique<cameraunlock::input::HotkeyPoller>();
            const auto recenter = []() {
                if (g_receiver) {
                    g_receiver->Recenter();
                    g_interp.Reset();
                    g_hasSmoothed = false;
                    g_posCenterPending.store(true);  // re-zero head sway too
                    Log::Line("Recenter");
                }
            };
            const auto toggleTracking = []() {
                const bool now = !g_trackingEnabled.load();
                g_trackingEnabled.store(now);
                Log::Line("Tracking toggled: %s", now ? "ON" : "OFF");
            };
            // Three-state tracking-mode cycle:
            //   0 normal (rotation + position), 1 rotation only, 2 position only.
            const auto cycleTrackingMode = []() {
                const int next = (g_trackingMode.load() + 1) % 3;
                g_trackingMode.store(next);
                g_rotationEnabled.store(next != 2);
                g_positionEnabled.store(next != 1);
                g_posCenterPending.store(true);
                static const char* names[] = {
                    "NORMAL (rotation + position)",
                    "ROTATION ONLY (position off)",
                    "POSITION ONLY (rotation off)"
                };
                Log::Line("tracking-mode -> %d  (%s)", next, names[next]);
            };
            const auto toggleYawMode = []() {
                const bool now = !g_worldSpaceYaw.load();
                g_worldSpaceYaw.store(now);
                Log::Line("yaw-mode -> %s", now ? "WORLD (horizon-locked)" : "LOCAL (camera-local)");
            };

            // Recenter: Home / Ctrl+Shift+T
            g_hotkeys->SetRecenterKey(VK_HOME, recenter);
            g_hotkeys->AddHotkey(0x54 /* T */, ChordGuarded(recenter));
            // Toggle tracking: End / Ctrl+Shift+Y
            g_hotkeys->SetToggleKey(VK_END, toggleTracking);
            g_hotkeys->AddHotkey(0x59 /* Y */, ChordGuarded(toggleTracking));
            // Cycle tracking mode: Page Up / Ctrl+Shift+G
            g_hotkeys->AddHotkey(VK_PRIOR, cycleTrackingMode);
            g_hotkeys->AddHotkey(0x47 /* G */, ChordGuarded(cycleTrackingMode));
            // Yaw mode (world/local): Page Down (or [Hotkeys] ToggleYawMode) / Ctrl+Shift+H
            g_hotkeys->AddHotkey(yawModeKey, toggleYawMode);
            g_hotkeys->AddHotkey(0x48 /* H */, ChordGuarded(toggleYawMode));
#if RVTY_DEV_HOTKEYS
            // Diagnostic widget dump: Ctrl+Shift+U (press near an interaction
            // prompt so its widget is live). Off the render path.
            g_hotkeys->AddHotkey(0x55 /* U */, ChordGuarded([]() {
                Log::Line("widget-dump: requested");
                DumpWidgets();
            }));
            // Reticle test-nudge: Ctrl+Shift+J toggles a fixed +300px offset on
            // the target widget, to confirm the move plumbing/target visually.
            g_hotkeys->AddHotkey(0x4A /* J */, ChordGuarded(reticle::ToggleTestNudge));
            // Live reticle-scale tuning: F7 down / F8 up by 0.1.
            g_hotkeys->AddHotkey(VK_F7,  []() { reticle::AdjustScale(-0.1f); });
            g_hotkeys->AddHotkey(VK_F8,  []() { reticle::AdjustScale(+0.1f); });
            // Vertical-only tuning: F9 down / F10 up.
            g_hotkeys->AddHotkey(VK_F9,  []() { reticle::AdjustVerticalScale(-0.05f); });
            g_hotkeys->AddHotkey(VK_F10, []() { reticle::AdjustVerticalScale(+0.05f); });
#endif
            g_hotkeys->Start();

            if (!InstallViewBuilderHook()) {
                Log::Line("===== bootstrap exited (hook install failed, mod dormant) =====");
                return 0;
            }

            reticle::LogBootstrapSummary();

            // Gameplay-state watcher: suppress tracking in menus / loading.
            g_gameStateThread = CreateThread(nullptr, 0, GameStateThread, nullptr, 0, nullptr);

            Log::Line("===== bootstrap complete - head tracking armed =====");
            return 0;
        }
    }

    void Initialize(void* hModule)
    {
        // Pin the DLL for the process lifetime. Anything in the process may
        // LoadLibrary("dxgi.dll")/FreeLibrary transiently; once our worker
        // threads and MinHook trampolines are live, an unmap would leave the
        // render thread and workers executing freed code. Pinning makes that
        // impossible; process exit still goes through EmergencyShutdown.
        HMODULE self = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&Initialize), &self);

        g_bootstrapThread = CreateThread(nullptr, 0, BootstrapThread, hModule, 0, nullptr);
    }

    void Shutdown()
    {
        // Join workers FIRST: the bootstrap thread creates the objects torn
        // down below, and the game-state thread reads game memory - neither
        // may be live once the DLL starts unmapping. Bounded waits so a
        // wedged thread degrades to the old (leaky) behaviour instead of
        // hanging DllMain forever.
        g_stopThreads.store(true, std::memory_order_relaxed);
        if (g_bootstrapThread) {
            WaitForSingleObject(g_bootstrapThread, 5000);
            CloseHandle(g_bootstrapThread);
            g_bootstrapThread = nullptr;
        }
        if (g_gameStateThread) {
            WaitForSingleObject(g_gameStateThread, 2000);
            CloseHandle(g_gameStateThread);
            g_gameStateThread = nullptr;
        }

        cameraunlock::hooks::HookManager::Instance().Shutdown();
        if (g_receiver) {
            g_receiver->Stop();
            g_receiver.reset();
        }
        g_hotkeys.reset();
        Log::Line("Shutdown");
        Log::Close();
    }

    void EmergencyShutdown()
    {
        Log::EmergencyLine("===== process exiting (DLL_PROCESS_DETACH, no cleanup) =====");
    }
}
