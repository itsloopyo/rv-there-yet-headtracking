#pragma once

#include "cameraunlock/unreal/ue_math.h"

// Reticle / interaction-prompt compensation. The game draws its interaction
// reticle + prompt at screen centre; with the view head-tracked, the clean-aim
// point (where interaction actually happens) lands off-centre, so we move the
// target widgets there via UMG SetRenderTranslation. It always follows the aim:
// no setting turns it off. All state is internal to reticle.cpp; the
// view-builder hook drives it through this interface.
namespace RVThereYetHeadTracking::reticle
{
    // One-line log of the resolved reticle config, for the bootstrap banner.
    void LogBootstrapSummary();

    // Game-state thread only. Each runs a full UObject scan while something
    // is unresolved, which is too slow for the game thread. ResolveReflection
    // finds the UFunctions and CDOs; RefreshTargets finds the reticle widgets,
    // and rescans only when the published ones have gone stale.
    void ResolveReflection();
    void RefreshTargets();

    // Once per frame, from the builder hook on the game thread: project the
    // clean-aim direction into the tracked view and move the target widgets
    // there. self/outView are the builder hook's arguments.
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
}
