#include "reticle.h"
#include "logging.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

#include "builds/build_registry.h"
#include "uobject_live.h"

#include "cameraunlock/rendering/aim_quat_projection.h"
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

        // Rate limits for the aim-offset diagnostic on the hot path. It answers
        // a calibration question (does the projected offset match the widget
        // scale and FOV), which is settled in the opening seconds, so it runs
        // tight for the first kLogBurstLines and then backs off. At the old flat
        // 2s it added about 225 KB an hour and buried the startup chain.
        constexpr std::uint64_t kLogThrottleMs = 2000;
        constexpr std::uint64_t kLogSteadyMs = 60000;
        constexpr int kLogBurstLines = 10;

        // The widget geometry scale and viewport size move on a resolution
        // change, a windowed/fullscreen switch or a move to another monitor, so
        // they are re-read at this interval rather than latched.
        constexpr std::uint64_t kScalePollMs = 500;

        // Sanity window for viewport / widget-geometry scale reads; values
        // outside it mean we read garbage, not a real scale.
        constexpr float kMinSaneScale = 0.05f;
        constexpr float kMaxSaneScale = 20.0f;

        // The builder's self object holds the owning PlayerController here.
        constexpr std::uintptr_t kBuilderPlayerControllerOffset = 0x30;

        // Everything the reticle calls into is found by a full UObject scan
        // (~90k objects, ~15 ms). Those scans run on the game-state thread
        // (ResolveReflection, RefreshTargets) and publish through these
        // atomics, so the game thread never stalls on one.
        using ProcessEvent_t = void(__fastcall*)(void* self, void* func, void* params);
        std::atomic<ProcessEvent_t> g_processEvent{nullptr};
        constexpr std::size_t kProcessEventVtableSlot = 76;  // UE5.6.x UObject::ProcessEvent
        std::atomic<std::uintptr_t> g_setRenderTranslationFn{0};

        // UMG viewport DPI scale, from UWidgetLayoutLibrary::GetViewportScale.
        // Only the divisor until the widget geometry scale below has been read:
        // on this game it reports 1.0 while the HUD draws at 0.666 at 720p.
        float g_viewportDpiScale = 1.0f;
        bool  g_dpiResolved = false;
        std::atomic<std::uintptr_t> g_getViewportScaleFn{0};
        std::atomic<std::uintptr_t> g_getViewportSizeFn{0};
        std::atomic<std::uintptr_t> g_widgetLayoutCDO{0};

        // UGameplayStatics::ProjectWorldToScreen - the game's own projection.
        // Feeding it a world point along the clean-aim direction returns the
        // exact screen pixel where that point lands in the rendered (tracked)
        // view, so the reticle offset needs no FOV/aspect guesswork.
        std::atomic<std::uintptr_t> g_projectW2SFn{0};
        std::atomic<std::uintptr_t> g_gameplayStaticsCDO{0};

        // The reticle widget's accumulated geometry scale (UWidget::
        // GetCachedGeometry -> FGeometry.Scale). RenderTransform.Translation is
        // in the widget's LOCAL space, so a screen-pixel offset must be divided
        // by this scale (the HUD renders the crosshair at < 1.0 scale, which is
        // exactly the residual undercompensation). 0 until first read.
        std::atomic<std::uintptr_t> g_getCachedGeometryFn{0};
        float g_widgetGeoScale = 0.0f;
        std::uint64_t g_lastScalePoll = 0;

        // The look-at reticle + object label are named leaf widgets inside
        // WG_PlayerHUD_C's tree (Crosshair = Image, LookAtObjectName =
        // TextBlock). We move those leaves directly - not the HUD - so health
        // bars etc. stay put.
        const std::vector<std::string> g_targetNames = { "Crosshair", "LookAtObjectName" };

        // The game-state thread owns the scan and publishes its result here;
        // the game thread adopts a copy when the generation moves.
        std::mutex g_publishedMutex;
        std::vector<LiveObject> g_publishedTargets;
        std::atomic<std::uint32_t> g_publishedGen{0};

        // Game-thread copy of the published targets, and the live subset of
        // it for the current update.
        std::vector<LiveObject> g_targets;
        std::uint32_t g_targetsGen = 0;
        std::vector<LiveObject> g_liveTargets;

        // SetRenderTranslation is persistent widget state, so once we have
        // moved the reticle off-centre we must explicitly drive it back to
        // (0,0) when tracking stops (toggle off, menu, tracker loss) - the
        // hook's early-return paths would otherwise leave it stuck offset.
        bool g_wasOffset = false;
        std::atomic<bool> g_testNudge{false};  // Ctrl+Shift+J: force +300px to verify plumbing
        // Both axes; F7/F8 in a build with RVTY_DEV_HOTKEYS, 1 otherwise.
        std::atomic<float> g_scale{1.0f};

        // Resolve UObject::ProcessEvent off a UWidget's vtable (slot 76). Must
        // be a widget, not an actor - AActor overrides the slot with a net-aware
        // variant whose derefs fault when called on a widget. Retried on the
        // next widget until a vtable yields an in-module address, so a widget
        // caught mid-construction does not poison resolution.
        void ResolveProcessEvent(std::uintptr_t widget)
        {
            if (g_processEvent.load(std::memory_order_acquire)) return;
            std::uintptr_t vtbl = 0;
            if (!ue::SafeReadPtr(widget, vtbl) || !vtbl) return;
            std::uintptr_t pe = 0;
            if (ue::SafeReadPtr(vtbl + kProcessEventVtableSlot * 8, pe)
                && pe >= ue::ModuleBase() && pe < ue::ModuleEnd()) {
                g_processEvent.store(reinterpret_cast<ProcessEvent_t>(pe), std::memory_order_release);
                Log::Line("ProcessEvent resolved via vt[%zu] -> RVA 0x%08llx",
                    kProcessEventVtableSlot,
                    static_cast<unsigned long long>(pe - ue::ModuleBase()));
            }
        }

        // A plain call into the script VM. A fault inside it is the game's and
        // is left to crash with its own stack: swallowing it would leave the
        // engine part-way through the call with whatever locks it took.
        // Callers establish that the object is live first.
        void CallProcessEvent(std::uintptr_t self, std::uintptr_t fn, void* params)
        {
            g_processEvent.load(std::memory_order_acquire)(
                reinterpret_cast<void*>(self), reinterpret_cast<void*>(fn), params);
        }

        // UWidgetLayoutLibrary::GetViewportScale(WorldContextObject) -> float,
        // called on the library CDO with a live widget as the world context.
        // On failure DPI stays 1.0.
        void ResolveDpiScale(std::uintptr_t worldCtxWidget)
        {
            const std::uintptr_t fn = g_getViewportScaleFn.load(std::memory_order_acquire);
            const std::uintptr_t cdo = g_widgetLayoutCDO.load(std::memory_order_acquire);
            if (g_dpiResolved || !fn || !cdo) return;
            struct { std::uintptr_t WorldContextObject; float ReturnValue; char pad[24]; } params{};
            params.WorldContextObject = worldCtxWidget;
            CallProcessEvent(cdo, fn, &params);
            if (params.ReturnValue > kMinSaneScale && params.ReturnValue < kMaxSaneScale) {
                g_viewportDpiScale = params.ReturnValue;
                g_dpiResolved = true;
                Log::Line("reticle: viewport DPI scale = %.4f (auto-applied)", g_viewportDpiScale);
            }
        }

        // Log the slate/widget viewport size (the space
        // RenderTransform.Translation is expressed in) on change, as a
        // diagnostic - it can differ from the render viewport under reduced
        // internal resolution, which shows up as a reticle scale mismatch. A
        // read of (0,0) or (1,1) is the widget reporting before layout.
        void LogViewportSize(std::uintptr_t worldCtxWidget)
        {
            const std::uintptr_t fn = g_getViewportSizeFn.load(std::memory_order_acquire);
            const std::uintptr_t cdo = g_widgetLayoutCDO.load(std::memory_order_acquire);
            if (!fn || !cdo) return;

            struct { std::uintptr_t WorldContextObject; double RX, RY; char pad[16]; } vs{};
            vs.WorldContextObject = worldCtxWidget;
            CallProcessEvent(cdo, fn, &vs);
            if (vs.RX <= 1.0 || vs.RY <= 1.0) return;

            static double s_loggedX = 0.0;
            static double s_loggedY = 0.0;
            if (vs.RX == s_loggedX && vs.RY == s_loggedY) return;
            s_loggedX = vs.RX;
            s_loggedY = vs.RY;
            Log::Line("reticle: widget viewport size = %.0f x %.0f", vs.RX, vs.RY);
        }

        // Read the reticle widget's accumulated geometry scale via
        // UWidget::GetCachedGeometry() -> FGeometry, from the RENDERED instance
        // among the (possibly pooled) targets - the one whose cached geometry
        // has a non-zero size. This build's FGeometry (confirmed empirically
        // from a default-constructed instance reading Scale=1.0): FVector2f
        // Size @ 0x00, float Scale @ 0x08. A pooled/empty instance dumps as
        // zeros with a lone 1.0 at +8, so we gate on Size. Re-read on every
        // poll so a resolution or DPI change reaches the reticle; while no
        // instance is rendered the last good value stands.
        void ReadWidgetScale(const std::vector<LiveObject>& live)
        {
            const std::uintptr_t fn = g_getCachedGeometryFn.load(std::memory_order_acquire);
            if (!fn) return;
            for (const LiveObject& t : live) {
                alignas(8) unsigned char geo[192] = {};
                CallProcessEvent(t.obj, fn, geo);
                float sizeX = 0, sizeY = 0, scale = 0;
                std::memcpy(&sizeX, geo + 0x00, 4);
                std::memcpy(&sizeY, geo + 0x04, 4);
                std::memcpy(&scale, geo + 0x08, 4);
                if (sizeX > 1.0f && sizeY > 1.0f && scale > kMinSaneScale && scale < kMaxSaneScale) {
                    if (std::fabs(scale - g_widgetGeoScale) > 1e-4f) {
                        g_widgetGeoScale = scale;
                        Log::Line("reticle: widget geometry scale = %.4f (auto-applied)", scale);
                    }
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
            CallProcessEvent(g_gameplayStaticsCDO.load(std::memory_order_acquire),
                             g_projectW2SFn.load(std::memory_order_acquire), &p);
            if (!p.ReturnValue) return false;
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
            if (!g_processEvent.load(std::memory_order_acquire)
                || !g_projectW2SFn.load(std::memory_order_acquire)
                || !g_gameplayStaticsCDO.load(std::memory_order_acquire)) return false;
            // Re-entrancy guard: ProjectWorldToScreen could, in principle, drive
            // the engine to rebuild a view and re-enter the builder hook. Never
            // start a projection from within a projection - the inner hook call
            // still applies tracking, it just skips the reticle projection.
            static thread_local bool s_inProjection = false;
            if (s_inProjection) return false;
            struct Guard { ~Guard() { s_inProjection = false; } } guard;
            s_inProjection = true;

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
            // developer tuning scale applies here; the geometry scale (which
            // folds in DPI) is divided out later in DriveReticle.
            const double s = static_cast<double>(g_scale.load(std::memory_order_relaxed));
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

        // Take the game-state thread's latest target list.
        void AdoptPublishedTargets()
        {
            if (g_publishedGen.load(std::memory_order_acquire) == g_targetsGen) return;
            std::lock_guard<std::mutex> lock(g_publishedMutex);
            g_targets = g_publishedTargets;
            g_targetsGen = g_publishedGen.load(std::memory_order_relaxed);
        }

        // Move the reticle target widgets to (dx, dy) client pixels from centre,
        // or reset to (0,0) when there's no valid offset. Game thread only.
        void DriveReticle(double dx, double dy, bool valid)
        {
            const std::uintptr_t setFn = g_setRenderTranslationFn.load(std::memory_order_acquire);
            if (!setFn || !g_processEvent.load(std::memory_order_acquire)) return;

            AdoptPublishedTargets();
            g_liveTargets.clear();
            for (const LiveObject& t : g_targets) {
                if (IsLive(t)) g_liveTargets.push_back(t);
            }
            if (g_liveTargets.empty()) {
                // No live widgets means no persistent translation left to
                // reset - it died with the widget, and a recreated one starts
                // at (0,0). The game-state thread finds the replacements.
                g_wasOffset = false;
                return;
            }

            const std::uint64_t now = GetTickCount64();
            if (now - g_lastScalePoll >= kScalePollMs) {
                g_lastScalePoll = now;
                ResolveDpiScale(g_liveTargets.front().obj);
                LogViewportSize(g_liveTargets.front().obj);
                ReadWidgetScale(g_liveTargets);
            }

            // RenderTransform.Translation is in the widget's LOCAL space, so
            // divide the screen-pixel offset by the widget's geometry scale
            // (the HUD renders the crosshair below 1.0 scale). The geometry
            // scale already folds in DPI + any HUD layout scale, so it is the
            // sole divisor; before it has been read, fall back to the DPI scale
            // as a best-effort approximation.
            const double gscale = (g_widgetGeoScale > 0.0f)
                ? static_cast<double>(g_widgetGeoScale)
                : static_cast<double>(g_viewportDpiScale);

            // UMG SetRenderTranslation(FVector2D) - FVector2D is 2 doubles under
            // LWC; trailing pad guards the ProcessEvent param copy.
            const bool nudge = g_testNudge.load(std::memory_order_relaxed);
            struct Translation { double X; double Y; char pad[16]; } tr{};
            tr.X = nudge ? 300.0 : (valid ? dx / gscale : 0.0);
            tr.Y = nudge ?   0.0 : (valid ? dy / gscale : 0.0);
            for (const LiveObject& t : g_liveTargets) {
                Translation params = tr;
                CallProcessEvent(t.obj, setFn, &params);
            }
            g_wasOffset = (tr.X != 0.0 || tr.Y != 0.0);
        }

        // Project the clean-aim point into the head-tracked view as a pixel
        // offset from screen centre. The relative rotation qrel = viewQ^-1 *
        // baseQ carries the clean-aim direction into the tracked view's frame,
        // which is correct for BOTH yaw modes: in world-yaw mode head yaw
        // rotates about world up, so its screen effect depends on the camera's
        // base orientation, and viewQ captures that.
        //
        // Hor+ (MaintainYFOV) aspect scaling and the viewport-edge NDC clamp
        // live in cameraunlock-core. The clamp is what this used to be missing:
        // as the clean aim approaches 90 degrees off-axis the perspective
        // divide runs away and the widget was thrown thousands of pixels
        // off-screen instead of stopping at the edge.
        bool ComputeAimScreenOffset(const FQuat4d& baseQ, const FQuat4d& viewQ,
                                    double fovDeg, double& dx, double& dy)
        {
            double vw = 0.0, vh = 0.0;
            if (!GetViewportSize(vw, vh)) return false;
            if (fovDeg < 20.0 || fovDeg > 170.0) return false;

            const FQuat4d qrel = QuatMul(QuatInv(viewQ), baseQ);
            const auto proj = cameraunlock::rendering::ProjectAimQuatHorPlus(
                qrel.X, qrel.Y, qrel.Z, qrel.W,
                static_cast<float>(vw), static_cast<float>(vh),
                static_cast<float>(fovDeg));
            if (!proj.inFront) return false;

            // The slate/DPI conversion is handled by dividing out the widget
            // geometry scale in DriveReticle.
            const double scale = static_cast<double>(g_scale.load(std::memory_order_relaxed));
            dx = (static_cast<double>(proj.screenX) - vw * 0.5) * scale;
            dy = (static_cast<double>(proj.screenY) - vh * 0.5) * scale;

            static std::uint64_t s_lastLog = 0;
            static int s_logCount = 0;
            const std::uint64_t now = GetTickCount64();
            const std::uint64_t interval =
                s_logCount < kLogBurstLines ? kLogThrottleMs : kLogSteadyMs;
            if (now - s_lastLog >= interval) {
                s_lastLog = now;
                ++s_logCount;
                Log::Line("aim-offset: vw=%.0f vh=%.0f fov=%.1f scale=%.2f ndc=(%.3f,%.3f) -> dx=%.1f dy=%.1f",
                    vw, vh, fovDeg, scale, proj.ndcX, proj.ndcY, dx, dy);
            }
            return true;
        }

        bool SameObjects(const std::vector<LiveObject>& a, const std::vector<LiveObject>& b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                [](const LiveObject& x, const LiveObject& y) { return x.obj == y.obj && x.cls == y.cls; });
        }
    }

    void LogBootstrapSummary()
    {
        std::string tn;
        for (const std::string& n : g_targetNames) { if (!tn.empty()) tn += ","; tn += n; }
        Log::Line("reticle: targets=[%s] scale=%.2f "
            "(functions and widgets resolve on the game-state thread once UMG is up)",
            tn.c_str(), g_scale.load());
    }

    void ResolveReflection()
    {
        const auto resolve = [](std::atomic<std::uintptr_t>& slot, const char* cls,
                                const char* name, const char* outer) {
            if (slot.load(std::memory_order_relaxed)) return;
            const std::uintptr_t obj = ue::FindLiveObject(cls, name, outer);
            if (!obj) return;
            slot.store(obj, std::memory_order_release);
            Log::Line("reticle: resolved %s %s -> 0x%llx", outer ? outer : cls, name,
                static_cast<unsigned long long>(obj));
        };
        resolve(g_setRenderTranslationFn, "Function", "SetRenderTranslation", "Widget");
        resolve(g_getCachedGeometryFn, "Function", "GetCachedGeometry", "Widget");
        resolve(g_projectW2SFn, "Function", "ProjectWorldToScreen", "GameplayStatics");
        resolve(g_gameplayStaticsCDO, "GameplayStatics", "Default__GameplayStatics", nullptr);
        resolve(g_getViewportScaleFn, "Function", "GetViewportScale", "WidgetLayoutLibrary");
        resolve(g_getViewportSizeFn, "Function", "GetViewportSize", "WidgetLayoutLibrary");
        resolve(g_widgetLayoutCDO, "WidgetLayoutLibrary", "Default__WidgetLayoutLibrary", nullptr);
    }

    void RefreshTargets()
    {
        std::vector<LiveObject> current;
        {
            std::lock_guard<std::mutex> lock(g_publishedMutex);
            current = g_publishedTargets;
        }
        if (!current.empty() && std::all_of(current.begin(), current.end(), IsLive)) return;

        std::vector<LiveObject> found;
        ue::ForEachUObject([&](std::uintptr_t obj) -> bool {
            const std::string on = ue::ObjectName(obj);
            if (std::find(g_targetNames.begin(), g_targetNames.end(), on) == g_targetNames.end())
                return false;
            // A texture asset is also named Crosshair. Widget functions
            // cannot be invoked on that object, even with a valid vtable.
            const std::string className = ue::ClassName(obj);
            if ((on == "Crosshair" && className != "Image") ||
                (on == "LookAtObjectName" && className != "TextBlock")) return false;
            found.push_back(CaptureLiveObject(obj));
            ResolveProcessEvent(obj);
            return false;
        });

        if (SameObjects(found, current)) return;
        const std::size_t count = found.size();
        {
            std::lock_guard<std::mutex> lock(g_publishedMutex);
            g_publishedTargets = std::move(found);
            g_publishedGen.fetch_add(1, std::memory_order_release);
        }
        Log::Line("reticle: resolved %zu target widget(s)", count);
    }

    void UpdateFromView(void* self, void* outView,
                        const FQuat4d& baseQ, const FQuat4d& viewQ)
    {
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
        const float next = std::clamp(g_scale.load() + delta, 0.1f, 5.0f);
        g_scale.store(next);
        Log::Line("reticle scale -> %.2f", next);
    }
}
