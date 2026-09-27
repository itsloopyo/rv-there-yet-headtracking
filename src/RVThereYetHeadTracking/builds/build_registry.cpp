#include "build_registry.h"
#include "image_discovery.h"

#include <array>

#include <cameraunlock/memory/pe_fingerprint.h>

#include "logging.h"

namespace RVThereYetHeadTracking::builds
{
    // Forward declarations of every known profile - one extern per build.
    //
    // Never-delete policy: when a game patch breaks the current Steam or GDK
    // build, the dev derives new RVAs and ADDS a NEW profile here without
    // removing the old one. Users on the un-patched build still match their
    // old profile by PE fingerprint; users on the new build match the new
    // one. Both work simultaneously. The naming convention is
    // `kStoreProfile_yyyymmdd` where the date is the build's release date (or
    // close approximation - the PE fingerprint is the authoritative key, so
    // the date is just for human readability).
    extern const BuildProfile kSteamProfile_20260701;
    extern const BuildProfile kGdkProfile_20260701;
    extern const BuildProfile kSteamProfile_20260926;
    extern const BuildProfile kGdkProfile_20260926;

    namespace
    {
        // Registry order matters only for diagnostics: the first entry is the
        // "primary" profile used to label HostNewer/HostOlder when no profile
        // matches. Add new entries to the TOP of this array (after the
        // diagnostic primary).
        constexpr std::array<const BuildProfile*, 4> kKnownProfiles = {
            &kSteamProfile_20260926,
            &kGdkProfile_20260926,
            &kSteamProfile_20260701,
            &kGdkProfile_20260701,
        };

        const BuildProfile* g_active = nullptr;
        BuildProfile g_discovered{};

        // A profile is "complete" iff its hook target RVA is non-zero. This
        // lets us register a placeholder profile (correct fingerprint, RVAs
        // still TBD) without risking accidental activation - the mod stays
        // dormant on that build until discovery fills the values in.
        bool ProfileIsComplete(const BuildProfile* p)
        {
            return p && p->Offsets.kViewBuilderRva != 0;
        }
    }

    MatchResult SelectProfile(HMODULE host)
    {
        g_active = nullptr;
        PeFingerprint running{};
        if (!cameraunlock::memory::ReadPeFingerprint(host, running)) {
            Log::Line("build-check: failed to read PE header from host module");
            return MatchResult::ReadFailed;
        }

        Log::Line("build-check: running  ts=0x%08x size=0x%08x csum=0x%08x",
            running.TimeDateStamp, running.SizeOfImage, running.CheckSum);

        for (const BuildProfile* p : kKnownProfiles) {
            const bool complete = ProfileIsComplete(p);
            Log::Line("build-check: profile=%s ts=0x%08x size=0x%08x csum=0x%08x%s",
                p->Name, p->Fingerprint.TimeDateStamp,
                p->Fingerprint.SizeOfImage, p->Fingerprint.CheckSum,
                complete ? "" : " (incomplete - offsets TBD)");
            if (running.Matches(p->Fingerprint)) {
                if (!complete) {
                    Log::Line("build-check: fingerprint matches %s but its offsets are not yet derived - staying dormant", p->Name);
                    return MatchResult::HostDiffers;
                }
                g_active = p;
                Log::Line("build-check: matched profile %s", p->Name);
                return MatchResult::Matched;
            }
        }

        const auto discovered = DiscoverImage(reinterpret_cast<const std::uint8_t*>(host), running.SizeOfImage);
        if (!discovered.error) {
            g_discovered = { "validated-engine-functions", running, {
                discovered.viewBuilder, {0x18, 0x30},
                {discovered.objects, 0x14, 0x18, 0x10000,
                 discovered.names, 0x10, 0x10, 0x18, 0x20},
            } };
            g_active = &g_discovered;
            Log::Line("build-check: discovered unchanged engine functions: builder=0x%08x objects=0x%08x names=0x%08x",
                discovered.viewBuilder, discovered.objects, discovered.names);
            return MatchResult::Matched;
        }
        Log::Line("build-check: discovery rejected this build: %s", discovered.error);

        switch (cameraunlock::memory::ClassifyMismatch(
                    running, kKnownProfiles.front()->Fingerprint)) {
            case cameraunlock::memory::FingerprintMismatch::Newer:
                return MatchResult::HostNewer;
            case cameraunlock::memory::FingerprintMismatch::Older:
                return MatchResult::HostOlder;
            case cameraunlock::memory::FingerprintMismatch::Differs:
            default:
                return MatchResult::HostDiffers;
        }
    }

    const BuildProfile& ActiveProfile()
    {
        return *g_active;
    }
}
