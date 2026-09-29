#include "headtracking_mod.h"
#include "logging.h"
#include "reticle.h"
#include "lean_trace.h"
#include "position_boundary.h"
#include "config.h"
#include "uobject_live.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include <windows.h>
#include <psapi.h>

#include "builds/build_registry.h"

#include "cameraunlock/diagnostics/crash_handler.h"
#include "cameraunlock/protocol/udp_receiver.h"
#if RVTY_DEV_HOTKEYS
#include "cameraunlock/input/chord_hotkeys.h"
#endif
#include "cameraunlock/input/hotkey_poller.h"
#include "cameraunlock/input/key_binding_registration.h"
#include "cameraunlock/input/key_bindings.h"
#include "cameraunlock/hooks/hook_manager.h"
#include "cameraunlock/processing/pose_interpolator.h"
#include "cameraunlock/processing/position_interpolator.h"
#include "cameraunlock/processing/position_processor.h"
#include "cameraunlock/data/position_data.h"
#include "cameraunlock/data/position_settings.h"
#include "cameraunlock/math/smoothing_utils.h"
#include "cameraunlock/math/quat4.h"
#include "cameraunlock/time/frame_clock.h"
#include "cameraunlock/time/qpc_clock.h"
#include "cameraunlock/tracking/tracking_mode.h"
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
#if RVTY_DEV_HOTKEYS
        using cameraunlock::input::ChordGuarded;
#endif
        using cameraunlock::time::FrameClock;

        // ---- Runtime state ----
        std::unique_ptr<cameraunlock::UdpReceiver>       g_receiver;
        std::unique_ptr<cameraunlock::input::HotkeyPoller> g_hotkeys;

        std::atomic<bool> g_trackingEnabled{true};
        std::atomic<bool> g_worldSpaceYaw{true};

        // Master rotation/position gates cycled by the tracking-mode hotkey. The
        // index is cameraunlock::TrackingMode's number: 0 normal (both), 1
        // rotation only, 2 position only.
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

        // Rotation pipeline. The view-builder hook fires on the game thread
        // only, so this is single-thread access - no locks needed.
        cameraunlock::PoseInterpolator g_interp;
        std::int64_t g_lastSampleTs = 0;
        FrameClock g_rotClock;
        float g_smoothedYaw = 0.0f, g_smoothedPitch = 0.0f, g_smoothedRoll = 0.0f;
        bool  g_hasSmoothed = false;

        // Smoothing is chosen per connection: local for a tracker on this
        // machine (loopback), remote for a device on the network. Both cover
        // rotation and position.
        float g_localSmoothing = static_cast<float>(cameraunlock::math::kDefaultLocalSmoothing);
        float g_remoteSmoothing = static_cast<float>(cameraunlock::math::kDefaultRemoteSmoothing);
        bool  g_isRemoteConnection = false;
        // Tri-state: false/false is indistinguishable from a local tracker, so
        // a plain equality check never reports the (common) local case at all.
        bool  g_remoteConnectionKnown = false;

        // Position (6DOF) pipeline.
        cameraunlock::PositionProcessor   g_posProcessor;
        cameraunlock::PositionInterpolator g_posInterp;
        FrameClock g_posClock;

        // Re-selects local vs remote smoothing when the tracker source changes.
        // Called every frame, before the pipeline runs.
        void SyncConnectionLocality()
        {
            const bool isRemote = g_receiver->IsRemoteConnection();
            if (g_remoteConnectionKnown && isRemote == g_isRemoteConnection) return;
            g_isRemoteConnection = isRemote;
            g_remoteConnectionKnown = true;

            g_posProcessor.SetIsRemoteConnection(isRemote);

            const double effective = cameraunlock::math::GetEffectiveSmoothing(
                g_localSmoothing, g_remoteSmoothing, isRemote);
            Log::Line("Tracker connection is %s; smoothing=%.3f",
                isRemote ? "remote" : "local", effective);
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

            // After the data check: the locality flag only means anything once
            // a packet has been classified, and reporting it before then would
            // announce a tracker that has not connected.
            SyncConnectionLocality();

            const float dt = g_rotClock.Tick();
            const std::int64_t ts = g_receiver->GetLastReceiveTimestamp();
            const bool isNew = (ts != g_lastSampleTs);
            g_lastSampleTs = ts;

            auto interp = g_interp.Update(rawYaw, rawPitch, rawRoll, isNew, dt);

            const float eff = static_cast<float>(
                cameraunlock::math::GetEffectiveSmoothing(
                    g_localSmoothing, g_remoteSmoothing, g_isRemoteConnection));
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
            outYaw   = g_smoothedYaw;
            outPitch = g_smoothedPitch;
            outRoll  = g_smoothedRoll;
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

            const cameraunlock::PositionData interp = g_posInterp.Update(raw, dt);
            const cameraunlock::math::Quat4 headQ =
                cameraunlock::math::Quat4::FromYawPitchRoll(yaw, pitch, roll);
            // Clamped offset in tracker axes, meters: x=sway(right),
            // y=heave(up), z=surge(forward).
            const cameraunlock::math::Vec3 off = g_posProcessor.Process(interp, headQ, dt);

            rvty::position::TrackerOffsetToUE(off, surge, sway, heave);
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
                lean_trace::Reset();
                reticle::ResetIfOffset();
                return;
            }
            // Menus / loading: leave the clean (vanilla) view untouched.
            if (!g_inGameplay.load(std::memory_order_relaxed)) {
                lean_trace::Reset();
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
            static thread_local std::uint64_t s_lastPoseUs = 0;
            static thread_local float  cYaw = 0.0f, cPitch = 0.0f, cRoll = 0.0f;
            static thread_local bool   cRotValid = false;
            static thread_local double cSurge = 0.0, cSway = 0.0, cHeave = 0.0;
            static thread_local bool   cPosValid = false;

            const std::uint64_t nowUs = cameraunlock::time::QpcNowMicros();
            const bool newFrame = nowUs - s_lastPoseUs > 500;  // > 0.5 ms => new frame
            if (newFrame) {
                s_lastPoseUs = nowUs;
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
                lean_trace::Reset();
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
                const FVector wanted = ProjectPositionOffset(baseQ, cSurge, cSway, cHeave);
                const FVector posOff = lean_trace::Clamp(self, outView, wanted);
                const auto locAddr = reinterpret_cast<FVector*>(outView);
                locAddr->X += posOff.X;
                locAddr->Y += posOff.Y;
                locAddr->Z += posOff.Z;
            } else {
                lean_trace::Reset();
            }

            // Reticle compensation: move the interaction widgets to where the
            // clean aim lands in the tracked view. Once per frame, on the same
            // gate as the pose, so the widget moves on every frame the view does.
            if (newFrame) reticle::UpdateFromView(self, outView, baseQ, viewQ);
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

        // Applies CameraUnlock.ini's settings to the session and returns them.
        // The owner imports HeadTracking.ini, the file every earlier build read,
        // while CameraUnlock.ini is absent, and never writes it.
        Config LoadSettings(void* module)
        {
            std::wstring dllFolder = DllDir(module);
            dllFolder.pop_back();
            const Config settings = config::Load(dllFolder, cameraunlock::config::DefaultsFile::PerUser());

            g_trackingEnabled.store(settings.enable_on_startup);
            g_localSmoothing  = settings.local_smoothing;
            g_remoteSmoothing = settings.remote_smoothing;
            g_worldSpaceYaw.store(settings.world_space_yaw);
            const cameraunlock::TrackingMode mode = config::StartupTrackingMode(settings);
            const cameraunlock::TrackingModeChannels channels = cameraunlock::EncodeTrackingMode(mode);
            g_trackingMode.store(static_cast<int>(mode));
            g_rotationEnabled.store(channels.rotation_enabled);
            g_positionEnabled.store(channels.position_enabled);

            // The axis flips live in position_boundary.h, past the clamp, so the
            // processor runs with no sensitivity or inversion of its own.
            cameraunlock::PositionSettings ps = g_posProcessor.GetSettings();
            ps.limit_x      = settings.position_limit_x;
            ps.limit_y      = settings.position_limit_y;
            ps.limit_y_down = settings.position_limit_y_down;
            ps.limit_z      = settings.position_limit_z;
            ps.limit_z_back = settings.position_limit_z_back;
            // Position shares the smoothing pair; the connection flag that picks
            // between them lives on the processor.
            ps.local_smoothing  = settings.local_smoothing;
            ps.remote_smoothing = settings.remote_smoothing;
            g_posProcessor.SetSettings(ps);
            return settings;
        }

        // Off-thread gameplay detector and reflection resolver. A gameplay HUD
        // widget is live only during actual play (not the main menu / loading
        // screens), so its presence is our in-gameplay signal. Every UObject
        // scan the mod needs runs here, every ~400ms, because a full scan
        // (~90k objects) inline would hitch the frame. Once the HUD and the
        // reticle widgets are found they are only re-checked for liveness, so
        // steady gameplay costs no scans at all.
        DWORD WINAPI GameStateThread(LPVOID)
        {
            static const char* kGameplayHuds[] = {
                "WG_VehicleHUD_C", "WG_PlayerHUD_C", "WG_PlayerInteraction_C"
            };
            LiveObject hud;
            while (!g_stopThreads.load(std::memory_order_relaxed)) {
                Sleep(400);
                if (g_stopThreads.load(std::memory_order_relaxed)) break;
                if (ue::ModuleBase() == 0) continue;

                lean_trace::ResolveReflection();
                reticle::ResolveReflection();

                if (!IsLive(hud)) {
                    hud = {};
                    ue::ForEachUObject([&](std::uintptr_t obj) -> bool {
                        const std::string cn = ue::ClassName(obj);
                        for (const char* h : kGameplayHuds) {
                            if (cn == h && !ue::ContainsCI(ue::ObjectName(obj), "Default__")) {
                                hud = CaptureLiveObject(obj);
                                return true;
                            }
                        }
                        return false;
                    });
                }
                const bool found = hud.obj != 0;
                if (found) reticle::RefreshTargets();

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
            const std::wstring logDir = DllDir(module);
            const std::wstring logPath = logDir + L"RVThereYetHeadTracking.log";
            // Keep one previous generation. The crash handler installed below
            // writes its report into this log, and the user relaunches before
            // sending it - a plain truncate would erase the very report we
            // installed the handler for.
            const BOOL  rotated   = MoveFileExW(logPath.c_str(),
                (logDir + L"RVThereYetHeadTracking.prev.log").c_str(),
                MOVEFILE_REPLACE_EXISTING);
            const DWORD rotateErr = rotated ? 0u : GetLastError();
            Log::Open(logPath);
            Log::Line("RV There Yet Head Tracking - bootstrap");
            // The open above truncates regardless, so a failed rotation loses
            // the previous session and .prev.log holds something older than the
            // README promises. ERROR_FILE_NOT_FOUND is a first launch.
            if (!rotated && rotateErr != ERROR_FILE_NOT_FOUND) {
                Log::Line("WARN: could not rotate the previous log to "
                    "RVThereYetHeadTracking.prev.log (error %lu); that file holds an "
                    "older session, not the previous launch", rotateErr);
            }
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

            const Config settings = LoadSettings(module);
            lean_trace::Configure(settings);
            const int udpPort = settings.udp_port;

            g_receiver = std::make_unique<cameraunlock::UdpReceiver>();
            g_receiver->SetLog([](const std::string& msg) { Log::Line("[udp] %s", msg.c_str()); });
            const bool bound = g_receiver->Start(static_cast<std::uint16_t>(udpPort));
            Log::Line("UDP receiver Start(%d) -> %s", udpPort, bound ? "bound" : "retry-scheduled");

            g_hotkeys = std::make_unique<cameraunlock::input::HotkeyPoller>();
            const auto toggleTracking = []() {
                const bool now = !g_trackingEnabled.load();
                g_trackingEnabled.store(now);
                Log::Line("Tracking toggled: %s", now ? "ON" : "OFF");
            };
            // Three-state tracking-mode cycle: rotation + position, rotation
            // only, position only, and back. Each step is saved.
            const auto cycleTrackingMode = []() {
                const int next = (g_trackingMode.load() + 1) % 3;
                g_trackingMode.store(next);
                const auto mode = static_cast<cameraunlock::TrackingMode>(next);
                const cameraunlock::TrackingModeChannels channels = cameraunlock::EncodeTrackingMode(mode);
                g_rotationEnabled.store(channels.rotation_enabled);
                g_positionEnabled.store(channels.position_enabled);
                static const char* names[] = {
                    "NORMAL (rotation + position)",
                    "ROTATION ONLY (position off)",
                    "POSITION ONLY (rotation off)"
                };
                Log::Line("tracking-mode -> %d  (%s)", next, names[next]);
                config::SaveTrackingMode(mode);
            };
            const auto toggleYawMode = []() {
                const bool now = !g_worldSpaceYaw.load();
                g_worldSpaceYaw.store(now);
                Log::Line("yaw-mode -> %s", now ? "WORLD (horizon-locked)" : "LOCAL (camera-local)");
                config::SaveWorldSpaceYaw(now);
            };

            // Every action's keys come from its list in CameraUnlock.ini, the
            // Ctrl+Shift chords included, so each is rebindable. The table's
            // hotkey codec lets only a list ParseKeyBindings reads into the
            // settings.
            const auto registerList = [](const std::string& list, std::function<void()> action) {
                cameraunlock::input::KeyBindingsParseResult parsed = cameraunlock::input::ParseKeyBindings(list);
                if (!parsed.ok()) throw std::logic_error("hotkey list '" + list + "': " + parsed.error);
                cameraunlock::input::RegisterKeyBindings(*g_hotkeys, parsed.bindings, std::move(action));
            };
            registerList(settings.toggle_key, toggleTracking);
            registerList(settings.cycle_tracking_mode_key, cycleTrackingMode);
            registerList(settings.yaw_mode_key, toggleYawMode);
            Log::Line("hotkeys: toggle=[%s] cycle tracking mode=[%s] yaw mode=[%s]",
                settings.toggle_key.c_str(), settings.cycle_tracking_mode_key.c_str(),
                settings.yaw_mode_key.c_str());
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
