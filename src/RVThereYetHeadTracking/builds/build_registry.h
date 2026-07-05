#pragma once
#include <windows.h>
#include "build_profile.h"

// Profile registry and selection. SelectProfile() fingerprints the host EXE
// (PE TimeDateStamp + SizeOfImage + CheckSum), looks up the matching entry in
// the known-profiles array, and stashes it as the active profile so the rest
// of the code can read RVAs and field offsets without re-fingerprinting on
// every call.

namespace RVThereYetHeadTracking
{
    namespace builds
    {
        // Outcome of a fingerprint match attempt. ReadFailed means we couldn't
        // even parse the PE header; the other three describe how the running
        // EXE differs from the primary (first-registered) profile so the user
        // log can hint at "patch newer/older" without surfacing all profiles.
        enum class MatchResult
        {
            Matched,        // Active profile is now set; mod can run.
            ReadFailed,     // Could not read the PE header.
            HostNewer,      // Running EXE TimeDateStamp > primary profile.
            HostOlder,      // Running EXE TimeDateStamp < primary profile.
            HostDiffers,    // Same timestamp, different size or checksum.
        };

        // Fingerprint the given module, look up a matching profile, and on
        // success install it as the active profile. Logs diagnostics either
        // way via Log::Line so the user log explains why the mod did or did
        // not engage.
        MatchResult SelectProfile(HMODULE host);

        const BuildProfile& ActiveProfile();
    }

    // Accessor for the active profile's offset table. Asserts that a profile
    // has been selected; callers must run after SelectProfile() returns
    // Matched. Inline so the lookup is a single load on the hot paths.
    inline const OffsetTable& Offsets()
    {
        return builds::ActiveProfile().Offsets;
    }
}
