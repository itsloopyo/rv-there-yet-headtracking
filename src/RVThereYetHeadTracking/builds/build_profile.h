#pragma once
#include <cstddef>
#include <cstdint>

#include <cameraunlock/memory/pe_fingerprint.h>
#include <cameraunlock/unreal/ue_runtime.h>

// One BuildProfile describes a single packaging/build of RV There Yet: the
// PE-header fingerprint that uniquely identifies it, and every per-build
// constant (RVAs into the EXE, engine struct field offsets) the mod needs to
// operate. The registry holds one profile per supported build (Steam Win64,
// Xbox WinGDK). At Initialize() time the mod fingerprints the live module and
// selects the matching profile; no match (or a fingerprint-matched profile
// whose RVAs are still zero) leaves the mod dormant via the fail-safe path.
//
// Struct layouts (UObject / FMinimalViewInfo) are engine-version-bound, not
// packaging-bound, so those fields match across the Steam and WinGDK profiles;
// only the RVAs differ between the two builds.

namespace RVThereYetHeadTracking
{
    // PE-header build fingerprint (TimeDateStamp + SizeOfImage + CheckSum);
    // the shared type keeps reading/matching/classification in core.
    using PeFingerprint = ::cameraunlock::memory::PeFingerprint;

    struct OffsetTable
    {
        // Hook target: the FMinimalViewInfo builder (the render-view assembler
        // that copies POV loc/rot/FOV + post-process settings into an
        // FMinimalViewInfo and calls GetPlayerViewPoint to write the final
        // view rotation). Hooking it and rotating its OUTPUT injects head
        // tracking on the render path only - game logic that queries
        // GetPlayerViewPoint directly still sees the clean rotation, so aim /
        // physics are unaffected by construction.
        std::uintptr_t kViewBuilderRva;

        // FMinimalViewInfo field offsets (UE5 LWC ABI). The builder writes the
        // final view Rotation (FRotator, 3 doubles) at outView + kRotationOffset
        // and the FOV (float) at outView + kFovOffset.
        struct {
            std::size_t kRotationOffset;
            std::size_t kFovOffset;
        } MinimalViewInfoLayout;

        // GUObjectArray + FNamePool anchors for UObject/FName reflection
        // (consumed by ue::SetRuntime). Used to find the reticle / interaction
        // prompt widgets for aim-point compensation. Struct-layout fields are
        // engine-ABI bound (UE5.6.x); only kObjObjects / kFNamePool differ
        // per build.
        ::cameraunlock::unreal::UObjectGlobalsLayout UObjectGlobals;
    };

    struct BuildProfile
    {
        const char*    Name;
        PeFingerprint  Fingerprint;
        OffsetTable    Offsets;
    };
}
