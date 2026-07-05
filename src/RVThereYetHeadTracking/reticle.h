#pragma once

#include <string>
#include <vector>

#include "cameraunlock/unreal/ue_math.h"

// Reticle / interaction-prompt compensation. The game draws its interaction
// reticle + prompt at screen centre; with the view head-tracked, the clean-aim
// point (where interaction actually happens) lands off-centre, so we move the
// target widgets there via UMG SetRenderTranslation. All state is internal to
// reticle.cpp; the view-builder hook drives it through this interface.
namespace RVThereYetHeadTracking::reticle
{
    struct Settings
    {
        bool  show = true;
        float scale = 1.0f;
        float verticalScale = 1.0f;
        // Widget names to move to the aim point; empty keeps the built-in
        // defaults (Crosshair, LookAtObjectName).
        std::vector<std::string> targetNames;
    };

    // Called once from the bootstrap thread, before the view-builder hook is
    // installed (everything else here runs on the render thread).
    void Configure(const Settings& s);

    // One-line log of the resolved reticle config, for the bootstrap banner.
    void LogBootstrapSummary();

    // Per rendered view, from the builder hook: project the clean-aim
    // direction into the tracked view and move the target widgets there.
    // Throttled internally (~100 Hz - ProcessEvent is a script-VM call).
    // self/outView are the builder hook's arguments.
    void UpdateFromView(void* self, void* outView,
                        const ::cameraunlock::unreal::FQuat4d& baseQ,
                        const ::cameraunlock::unreal::FQuat4d& viewQ);

    // Recentre the widgets if the last update left them offset - the widget
    // translation is persistent state, so the hook's tracking-suppressed
    // paths must drive it back to (0,0). No-op once landed.
    void ResetIfOffset();

    // Live-tuning entry points, hotkey-bound only when RVTY_DEV_HOTKEYS=1.
    void ToggleTestNudge();
    void AdjustScale(float delta);
    void AdjustVerticalScale(float delta);
}
