#include "reticle.h"
#include "logging.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

#include "builds/build_registry.h"

#include "cameraunlock/unreal/ue_runtime.h"

namespace RVThereYetHeadTracking::reticle
{
    namespace ue = ::cameraunlock::unreal;

    namespace
    {
        using ue::FVector;
        using ue::FQuat4d;
        using ue::QuatInv;
        using ue::QuatMul;
        using ue::QuatRotateVec;

        constexpr double kPi = 3.14159265358979323846;

        // Rate limit for periodic diagnostic log lines on the hot path, so the
        // per-frame reticle/aim math doesn't flood the log.
        constexpr std::uint64_t kLogThrottleMs = 2000;

        // Widget moves are throttled to ~100 Hz: the builder fires several
        // times per frame and ProcessEvent is a script-VM call.
        constexpr std::uint64_t kUpdateIntervalMs = 10;

        // Rate limits for the lazy UObject-scan resolutions (a full scan
        // touches ~90k objects, so never run it every frame).
        constexpr std::uint64_t kTargetRescanMs = 500;
        constexpr std::uint64_t kFnScanRetryMs = 1000;

        // Sanity window for viewport / widget-geometry scale reads; values
        // outside it mean we read garbage, not a real scale.
        constexpr float kMinSaneScale = 0.05f;
        constexpr float kMaxSaneScale = 20.0f;

        // The builder's self object holds the owning PlayerController here.
        constexpr std::uintptr_t kBuilderPlayerControllerOffset = 0x30;

        using ProcessEvent_t = void(__fastcall*)(void* self, void* func, void* params);
        ProcessEvent_t g_processEvent = nullptr;
        std::atomic<bool> g_peResolved{false};
        constexpr std::size_t kProcessEventVtableSlot = 76;  // UE5.6.x UObject::ProcessEvent
        std::uintptr_t g_setRenderTranslationFn = 0;

        // UMG viewport DPI scale. RenderTransform.Translation is in the
        // widget's slate units, which the viewport DPI-scales to reach screen
        // pixels - so our screen-pixel offset must be divided by this scale to
        // land right. Read once from UWidgetLayoutLibrary::GetViewportScale so
        // the reticle is correct at Scale=1.0 across resolutions (rather than
        // baking a resolution-specific constant). 1.0 until resolved / if the
        // read fails (then manual [Reticle] Scale compensates).
        float g_viewportDpiScale = 1.0f;
        bool  g_dpiResolved = false;
        std::uintptr_t g_getViewportScaleFn = 0;
        std::uintptr_t g_widgetLayoutCDO = 0;

        // UGameplayStatics::ProjectWorldToScreen - the game's own projection.
        // Feeding it a world point along the clean-aim direction returns the
        // exact screen pixel where that point lands in the rendered (tracked)
        // view, so the reticle offset needs no FOV/aspect guesswork.
        std::uintptr_t g_projectW2SFn = 0;
        std::uintptr_t g_gameplayStaticsCDO = 0;

        // The reticle widget's accumulated geometry scale (UWidget::
        // GetCachedGeometry -> FGeometry.Scale). RenderTransform.Translation is
        // in the widget's LOCAL space, so a screen-pixel offset must be divided
        // by this scale (the HUD renders the crosshair at < 1.0 scale, which is
        // exactly the residual undercompensation). 0 until resolved.
        std::uintptr_t g_getCachedGeometryFn = 0;
        float g_widgetGeoScale = 0.0f;

        // The look-at reticle + object label are named leaf widgets inside
        // WG_PlayerHUD_C's tree (Crosshair = Image, LookAtObjectName =
        // TextBlock). We move those leaves directly - not the HUD - so health
        // bars etc. stay put. Config-overridable (comma-separated names).
        std::vector<std::string> g_targetNames = { "Crosshair", "LookAtObjectName" };
        struct ReticleTarget { std::uintptr_t obj; std::uintptr_t cls; };
        std::vector<ReticleTarget> g_targets;
        std::uint64_t g_lastTargetScan = 0;

        // A cached target goes stale when its widget is freed/recreated - the
        // class pointer at +kClassPrivate no longer matches what we recorded.
        bool ReticleTargetLive(const ReticleTarget& t)
        {
            std::uintptr_t cls = 0;
            return ue::SafeReadPtr(t.obj + Offsets().UObjectGlobals.kClassPrivate, cls)
                && cls == t.cls;
        }

        std::atomic<bool> g_show{true};
        // SetRenderTranslation is persistent widget state, so once we have
        // moved the reticle off-centre we must explicitly drive it back to
        // (0,0) when tracking stops (toggle off, menu, tracker loss) - the
        // hook's early-return paths would otherwise leave it stuck offset.
        bool g_wasOffset = false;
        std::atomic<bool> g_testNudge{false};  // Ctrl+Shift+J: force +300px to verify plumbing
        float g_scale = 1.0f;   // common (both axes); F7/F8
        float g_vScale = 1.0f;  // extra vertical multiplier (HUD layout anisotropy); F9/F10

        // Resolve UObject::ProcessEvent off a UWidget's vtable (slot 76). Must
        // be a widget, not an actor - AActor overrides the slot with a net-aware
        // variant whose derefs fault when called on a widget.
        void ResolveProcessEvent(std::uintptr_t widget)
        {
            // Latch g_peResolved only on SUCCESS, so a call on a widget whose
            // vtable isn't ready yet (mid-construction, slot 76 null/garbage)
            // doesn't permanently poison resolution - we retry next call.
            if (g_peResolved.load(std::memory_order_relaxed)) return;
            std::uintptr_t vtbl = 0;
            if (!ue::SafeReadPtr(widget, vtbl) || !vtbl) return;
            std::uintptr_t pe = 0;
            if (ue::SafeReadPtr(vtbl + kProcessEventVtableSlot * 8, pe) && pe >= ue::ModuleBase()) {
                g_processEvent = reinterpret_cast<ProcessEvent_t>(pe);
                g_peResolved.store(true, std::memory_order_relaxed);
                Log::Line("ProcessEvent resolved via vt[%zu] -> RVA 0x%08llx",
                    kProcessEventVtableSlot,
                    static_cast<unsigned long long>(pe - ue::ModuleBase()));
            }
        }

        bool SafeProcessEvent(void* self, void* fn, void* params)
        {
            __try {
                g_processEvent(self, fn, params);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Read the UMG viewport DPI scale via
        // UWidgetLayoutLibrary::GetViewportScale(WorldContextObject) -> float.
        // Static UFunction, so we call it on the library CDO with a live widget
        // as the world context. One-shot; on failure DPI stays 1.0 and manual
        // [Reticle] Scale still works.
        void ResolveDpiScale(std::uintptr_t worldCtxWidget)
        {
            if (g_dpiResolved || !g_processEvent || !worldCtxWidget) return;
            if (!g_getViewportScaleFn)
                g_getViewportScaleFn = ue::FindLiveObject("Function", "GetViewportScale", "WidgetLayoutLibrary");
            if (!g_widgetLayoutCDO)
                g_widgetLayoutCDO = ue::FindLiveObject("WidgetLayoutLibrary", "Default__WidgetLayoutLibrary", nullptr);
            if (!g_getViewportScaleFn || !g_widgetLayoutCDO) return;
            struct { std::uintptr_t WorldContextObject; float ReturnValue; char pad[24]; } params{};
            params.WorldContextObject = worldCtxWidget;
            if (SafeProcessEvent(reinterpret_cast<void*>(g_widgetLayoutCDO),
                                 reinterpret_cast<void*>(g_getViewportScaleFn), &params)) {
                if (params.ReturnValue > kMinSaneScale && params.ReturnValue < kMaxSaneScale) {
                    g_viewportDpiScale = params.ReturnValue;
                    g_dpiResolved = true;
                    Log::Line("reticle: viewport DPI scale = %.4f (auto-applied)", g_viewportDpiScale);
                }
            }

            // Also log the slate/widget viewport size (the space
            // RenderTransform.Translation is expressed in) as a diagnostic -
            // it can differ from the render viewport under reduced internal
            // resolution, which shows up as a reticle scale mismatch.
            static std::uintptr_t s_getViewportSizeFn = 0;
            if (!s_getViewportSizeFn)
                s_getViewportSizeFn = ue::FindLiveObject("Function", "GetViewportSize", "WidgetLayoutLibrary");
            if (s_getViewportSizeFn) {
                struct { std::uintptr_t WorldContextObject; double RX, RY; char pad[16]; } vs{};
                vs.WorldContextObject = worldCtxWidget;
                if (SafeProcessEvent(reinterpret_cast<void*>(g_widgetLayoutCDO),
                                     reinterpret_cast<void*>(s_getViewportSizeFn), &vs)) {
                    if (vs.RX > 1.0 && vs.RY > 1.0) {
                        Log::Line("reticle: widget viewport size = %.0f x %.0f", vs.RX, vs.RY);
                    }
                }
            }
        }

        // Read the reticle widget's accumulated geometry scale via
        // UWidget::GetCachedGeometry() -> FGeometry. One-shot; on failure the
        // scale stays 0 (unused) and manual [Reticle] Scale still works.
        // Find the RENDERED reticle instance among the (possibly pooled)
        // targets - the one whose cached geometry has a non-zero size - and
        // take its accumulated scale. This build's FGeometry (confirmed
        // empirically from a default-constructed instance reading Scale=1.0):
        // FVector2f Size @ 0x00, float Scale @ 0x08. A pooled/empty instance
        // dumps as zeros with a lone 1.0 at +8, so we gate on Size.
        void ResolveWidgetScale()
        {
            if (g_widgetGeoScale > 0.0f || !g_processEvent || g_targets.empty()) return;
            if (!g_getCachedGeometryFn)
                g_getCachedGeometryFn = ue::FindLiveObject("Function", "GetCachedGeometry", "Widget");
            if (!g_getCachedGeometryFn) return;

            for (const ReticleTarget& t : g_targets) {
                if (!ReticleTargetLive(t)) continue;
                alignas(8) unsigned char geo[192] = {};
                if (!SafeProcessEvent(reinterpret_cast<void*>(t.obj),
                                      reinterpret_cast<void*>(g_getCachedGeometryFn), geo))
                    continue;
                float sizeX = 0, sizeY = 0, scale = 0;
                std::memcpy(&sizeX, geo + 0x00, 4);
                std::memcpy(&sizeY, geo + 0x04, 4);
                std::memcpy(&scale, geo + 0x08, 4);
                if (sizeX > 1.0f && sizeY > 1.0f && scale > kMinSaneScale && scale < kMaxSaneScale) {
                    g_widgetGeoScale = scale;
                    Log::Line("reticle: widget geometry scale = %.4f (auto-applied)", scale);
                    return;
                }
            }
        }

        // Project a world direction (from the camera location) to screen pixels
        // via UGameplayStatics::ProjectWorldToScreen.
        bool ProjectDirToScreen(std::uintptr_t pc, const FVector* loc,
                                const FVector& dir, double& sx, double& sy)
        {
            constexpr double kAimDist = 100000.0;
            // ProjectWorldToScreen(Player, WorldPosition, out ScreenPosition,
            //   bPlayerViewportRelative) -> bool. LWC: FVector=3 doubles,
            // FVector2D=2 doubles.
            struct {
                std::uintptr_t Player;
                double WX, WY, WZ;
                double SX, SY;
                bool   bViewportRelative;
                bool   ReturnValue;
                char   pad[16];
            } p{};
            p.Player = pc;
            p.WX = loc->X + dir.X * kAimDist;
            p.WY = loc->Y + dir.Y * kAimDist;
            p.WZ = loc->Z + dir.Z * kAimDist;
            p.bViewportRelative = true;
            if (!SafeProcessEvent(reinterpret_cast<void*>(g_gameplayStaticsCDO),
                                  reinterpret_cast<void*>(g_projectW2SFn), &p) || !p.ReturnValue)
                return false;
            sx = p.SX; sy = p.SY;
            return true;
        }

        // Exact reticle offset via the game's own projection - DIFFERENTIAL: we
        // project BOTH the clean-aim direction and the actual view-forward
        // direction, and take the difference. That gives where clean-aim sits
        // relative to where the view points, in the rendered view's own scale,
        // regardless of which view ProjectWorldToScreen caches or its centre.
        // self = builder arg (PlayerController at +0x30); outView = FMinimalViewInfo.
        bool ComputeAimOffsetViaProjection(void* self, void* outView,
                                           const FQuat4d& baseQ, const FQuat4d& viewQ,
                                           double& dx, double& dy)
        {
            if (!g_processEvent) return false;
            // Re-entrancy guard: ProjectWorldToScreen could, in principle, drive
            // the engine to rebuild a view and re-enter the builder hook. Never
            // start a projection from within a projection - the inner hook call
            // still applies tracking, it just skips the reticle projection.
            static thread_local bool s_inProjection = false;
            if (s_inProjection) return false;
            struct Guard { ~Guard() { s_inProjection = false; } } guard;
            s_inProjection = true;

            if (!g_projectW2SFn)
                g_projectW2SFn = ue::FindLiveObject("Function", "ProjectWorldToScreen", "GameplayStatics");
            if (!g_gameplayStaticsCDO)
                g_gameplayStaticsCDO = ue::FindLiveObject("GameplayStatics", "Default__GameplayStatics", nullptr);
            if (!g_projectW2SFn || !g_gameplayStaticsCDO) return false;

            std::uintptr_t pc = 0;
            if (!ue::SafeReadPtr(reinterpret_cast<std::uintptr_t>(self)
                                     + kBuilderPlayerControllerOffset, pc) || !pc) return false;

            const auto loc = reinterpret_cast<const FVector*>(outView);
            const FVector aimDir = QuatRotateVec(baseQ, FVector{ 1.0, 0.0, 0.0 });
            const FVector fwdDir = QuatRotateVec(viewQ, FVector{ 1.0, 0.0, 0.0 });
            double aimSX = 0, aimSY = 0, fwdSX = 0, fwdSY = 0;
            if (!ProjectDirToScreen(pc, loc, aimDir, aimSX, aimSY)) return false;
            if (!ProjectDirToScreen(pc, loc, fwdDir, fwdSX, fwdSY)) return false;

            // Differential in the rendered view's own pixel scale. Only the
            // optional user fine-tune applies here; the geometry scale (which
            // folds in DPI) is divided out later in DriveReticle.
            const double s = static_cast<double>(g_scale);
            dx = (aimSX - fwdSX) * s;
            dy = (aimSY - fwdSY) * s;
            return true;
        }

        // Game window client size, for pixel-space reticle offsets. Cached.
        // Picks the LARGEST-area visible top-level window of this process - the
        // render window - not just the first (which may be a small tool/overlay
        // window and gave a bogus 1280-wide viewport).
        struct EnumWinCtx { DWORD pid; HWND best; long bestArea; };
        BOOL CALLBACK EnumGameWindow(HWND h, LPARAM lp)
        {
            auto* c = reinterpret_cast<EnumWinCtx*>(lp);
            DWORD wpid = 0;
            GetWindowThreadProcessId(h, &wpid);
            if (wpid == c->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) {
                RECT r{};
                if (GetClientRect(h, &r)) {
                    const long area = (r.right - r.left) * (r.bottom - r.top);
                    if ((r.right - r.left) > 100 && (r.bottom - r.top) > 100 && area > c->bestArea) {
                        c->bestArea = area;
                        c->best = h;
                    }
                }
            }
            return TRUE;
        }

        bool GetViewportSize(double& w, double& h)
        {
            static HWND s_win = nullptr;
            if (!s_win || !IsWindow(s_win)) {
                EnumWinCtx ctx{ GetCurrentProcessId(), nullptr, 0 };
                EnumWindows(EnumGameWindow, reinterpret_cast<LPARAM>(&ctx));
                s_win = ctx.best;
                if (s_win) {
                    RECT r{};
                    GetClientRect(s_win, &r);
                    Log::Line("reticle: render window client = %ld x %ld",
                        r.right - r.left, r.bottom - r.top);
                }
            }
            if (!s_win) return false;
            RECT r{};
            if (!GetClientRect(s_win, &r)) return false;
            w = static_cast<double>(r.right - r.left);
            h = static_cast<double>(r.bottom - r.top);
            return (w > 1.0 && h > 1.0);
        }

        // Is this object name one of our reticle targets? (exact match)
        bool IsReticleTargetName(const std::string& n)
        {
            for (const std::string& t : g_targetNames) if (n == t) return true;
            return false;
        }

        // (Re)find the live target widgets by name. Rate-limited so a ~90k
        // UObject scan doesn't run every frame (esp. before the HUD exists).
        void RefreshReticleTargets()
        {
            const std::uint64_t now = GetTickCount64();
            if (now - g_lastTargetScan < kTargetRescanMs) return;
            g_lastTargetScan = now;

            g_targets.clear();
            ue::ForEachUObject([&](std::uintptr_t obj) -> bool {
                const std::string on = ue::ObjectName(obj);
                if (!IsReticleTargetName(on)) return false;
                if (ue::ContainsCI(on, "Default__")) return false;
                std::uintptr_t cls = 0;
                ue::SafeReadPtr(obj + Offsets().UObjectGlobals.kClassPrivate, cls);
                g_targets.push_back({ obj, cls });
                if (!g_peResolved.load()) ResolveProcessEvent(obj);
                return false;
            });
            if (!g_targets.empty()) {
                static bool s_once = false;
                if (!s_once) {
                    s_once = true;
                    Log::Line("reticle: resolved %zu target widget(s)", g_targets.size());
                }
            }
        }

        // Move the reticle target widgets to (dx, dy) client pixels from centre,
        // or reset to (0,0) when there's no valid offset. Called from the hook.
        void DriveReticle(double dx, double dy, bool valid)
        {
            if (!g_show.load(std::memory_order_relaxed)) return;

            // SetRenderTranslation isn't registered until UMG spins up, so
            // resolve it lazily (rate-limited) rather than at bootstrap.
            if (!g_setRenderTranslationFn) {
                static std::uint64_t s_lastFnScan = 0;
                const std::uint64_t now = GetTickCount64();
                if (now - s_lastFnScan < kFnScanRetryMs) return;
                s_lastFnScan = now;
                g_setRenderTranslationFn = ue::FindLiveObject("Function", "SetRenderTranslation", "Widget");
                if (g_setRenderTranslationFn) {
                    Log::Line("reticle: SetRenderTranslation resolved lazily -> RVA 0x%llx",
                        static_cast<unsigned long long>(g_setRenderTranslationFn - ue::ModuleBase()));
                } else {
                    return;
                }
            }

            // Validate the cache; refresh if empty or any entry went stale.
            bool needRefresh = g_targets.empty();
            for (const ReticleTarget& t : g_targets) {
                if (!ReticleTargetLive(t)) { needRefresh = true; break; }
            }
            if (needRefresh) RefreshReticleTargets();
            if (g_targets.empty()) {
                // No live widgets means no persistent translation left to
                // reset - it died with the widget, and a recreated one starts
                // at (0,0). Clearing the flag stops ResetIfOffset from
                // re-triggering the UObject rescan every 500ms in menus.
                g_wasOffset = false;
                return;
            }
            if (!g_processEvent) return;

            // Resolve the viewport DPI scale once, using a live target widget
            // as the world context.
            if (!g_dpiResolved) ResolveDpiScale(g_targets.front().obj);
            ResolveWidgetScale();

            // RenderTransform.Translation is in the widget's LOCAL space, so
            // divide the screen-pixel offset by the widget's geometry scale
            // (the HUD renders the crosshair below 1.0 scale). The geometry
            // scale already folds in DPI + any HUD layout scale, so it is the
            // sole divisor; before it resolves, fall back to the DPI scale as
            // a best-effort approximation.
            const double gscale = (g_widgetGeoScale > 0.0f)
                ? static_cast<double>(g_widgetGeoScale)
                : static_cast<double>(g_viewportDpiScale);

            // UMG SetRenderTranslation(FVector2D) - FVector2D is 2 doubles under
            // LWC; trailing pad guards the ProcessEvent param copy.
            const bool nudge = g_testNudge.load(std::memory_order_relaxed);
            struct { double X; double Y; char pad[16]; } tr{};
            tr.X = nudge ? 300.0 : (valid ? dx / gscale : 0.0);
            tr.Y = nudge ?   0.0 : (valid ? dy / gscale : 0.0);
            for (const ReticleTarget& t : g_targets) {
                if (!ReticleTargetLive(t)) continue;
                SafeProcessEvent(reinterpret_cast<void*>(t.obj),
                                 reinterpret_cast<void*>(g_setRenderTranslationFn), &tr);
            }
            g_wasOffset = (tr.X != 0.0 || tr.Y != 0.0);
        }

        // Project the clean-aim point into the head-tracked view as a pixel
        // offset from screen centre. Rather than per-axis tan() (which only
        // holds for camera-local yaw), transform the clean-aim world direction
        // into the tracked view's local frame via qrel = viewQ^-1 * baseQ and
        // perspective-divide. This is correct for BOTH yaw modes: in world-yaw
        // mode head yaw rotates about world up, so its screen effect depends on
        // the camera's base orientation - which viewQ captures. Reduces exactly
        // to the local per-axis formula when viewQ = baseQ * headLocal.
        // UE frame: +X forward, +Y right, +Z up. Screen Y is down, hence the
        // negation on dy. Returns false if inputs are degenerate / aim behind.
        bool ComputeAimScreenOffset(const FQuat4d& baseQ, const FQuat4d& viewQ,
                                    double fovDeg, double& dx, double& dy)
        {
            double vw = 0.0, vh = 0.0;
            if (!GetViewportSize(vw, vh)) return false;
            if (fovDeg < 20.0 || fovDeg > 170.0) return false;
            const double halfTanH = std::tan(fovDeg * 0.5 * kPi / 180.0);
            if (halfTanH < 1e-4) return false;

            const FQuat4d qrel = QuatMul(QuatInv(viewQ), baseQ);
            const FVector aim = QuatRotateVec(qrel, FVector{ 1.0, 0.0, 0.0 });
            if (aim.X < 1e-3) return false;  // clean aim is behind the tracked view

            // Screen-pixel focal length. The slate/DPI conversion is handled by
            // dividing out the widget geometry scale in DriveReticle.
            const double k = (vw * 0.5) / halfTanH * static_cast<double>(g_scale);
            dx =  k * (aim.Y / aim.X);
            dy = -k * (aim.Z / aim.X) * static_cast<double>(g_vScale);

            static std::uint64_t s_lastLog = 0;
            const std::uint64_t now = GetTickCount64();
            if (now - s_lastLog >= kLogThrottleMs) {
                s_lastLog = now;
                Log::Line("aim-offset: vw=%.0f fov=%.1f scale=%.2f vscale=%.2f aim=(%.3f,%.3f,%.3f) -> dx=%.1f dy=%.1f",
                    vw, fovDeg, g_scale, g_vScale, aim.X, aim.Y, aim.Z, dx, dy);
            }
            return true;
        }
    }

    void Configure(const Settings& s)
    {
        g_show.store(s.show);
        g_scale = s.scale;
        g_vScale = s.verticalScale;
        if (!s.targetNames.empty()) g_targetNames = s.targetNames;
    }

    void LogBootstrapSummary()
    {
        std::string tn;
        for (const std::string& n : g_targetNames) { if (!tn.empty()) tn += ","; tn += n; }
        Log::Line("reticle: targets=[%s] showReticle=%s scale=%.2f "
            "(SetRenderTranslation resolves lazily once UMG is up)",
            tn.c_str(), g_show.load() ? "true" : "false", g_scale);
    }

    void UpdateFromView(void* self, void* outView,
                        const FQuat4d& baseQ, const FQuat4d& viewQ)
    {
        if (!g_show.load(std::memory_order_relaxed)) return;

        static std::atomic<std::uint64_t> s_lastUpdate{0};
        const std::uint64_t now = GetTickCount64();
        if (now - s_lastUpdate.load(std::memory_order_relaxed) < kUpdateIntervalMs) return;
        s_lastUpdate.store(now, std::memory_order_relaxed);

        float fovDeg = 0.0f;
        std::memcpy(&fovDeg,
            reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(outView)
                + Offsets().MinimalViewInfoLayout.kFovOffset),
            sizeof(float));
        double dx = 0.0, dy = 0.0;
        // Exact path via the game's projection; geometric fallback if
        // ProjectWorldToScreen isn't resolvable yet.
        bool aimValid = ComputeAimOffsetViaProjection(self, outView, baseQ, viewQ, dx, dy);
        if (!aimValid) aimValid = ComputeAimScreenOffset(baseQ, viewQ, fovDeg, dx, dy);
        DriveReticle(dx, dy, aimValid);
    }

    void ResetIfOffset()
    {
        if (g_wasOffset) DriveReticle(0.0, 0.0, false);
    }

    void ToggleTestNudge()
    {
        const bool now = !g_testNudge.load();
        g_testNudge.store(now);
        Log::Line("reticle test-nudge: %s", now ? "ON (+300px right)" : "OFF");
    }

    void AdjustScale(float delta)
    {
        g_scale = std::clamp(g_scale + delta, 0.1f, 5.0f);
        Log::Line("reticle scale -> %.2f (save to [Reticle] Scale)", g_scale);
    }

    void AdjustVerticalScale(float delta)
    {
        g_vScale = std::clamp(g_vScale + delta, 0.1f, 5.0f);
        Log::Line("reticle vertical-scale -> %.2f (save to [Reticle] VerticalScale)", g_vScale);
    }
}
