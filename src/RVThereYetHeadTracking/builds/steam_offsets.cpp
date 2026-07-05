#include "build_profile.h"

// Steam Win64 build of RV There Yet (Ride-Win64-Shipping.exe, UE5.6.x).
// Fingerprint captured from the installed EXE on 2026-07-01. Until
// kViewBuilderRva is non-zero the registry marks this profile "incomplete" and
// the mod stays dormant on this build (correct fingerprint, no hooks
// installed), so the game runs vanilla.
//
// To add support for a new Steam build: do not edit an existing
// kSteamProfile_<date> in place. Append a new
// `extern const BuildProfile kSteamProfile_YYYYMMDD = { ... };` below,
// register it at the TOP of build_registry.cpp's kKnownProfiles array, and
// keep the older profiles for users who have not updated yet. The PE
// fingerprint routes each user to the right profile automatically.

namespace RVThereYetHeadTracking::builds
{
    extern const BuildProfile kSteamProfile_20260701;

    // ---- Steam Win64 build (PE TS 0xdfe4dc2c) ----
    const BuildProfile kSteamProfile_20260701 = {
        /* Name        */ "steam-win64-20260701",
        /* Fingerprint */ { 0xdfe4dc2cu, 0x09f61000u, 0x09ba16e7u },
        /* Offsets     */ {
            // Hook target: the FMinimalViewInfo builder FUN_143e72a70,
            // confirmed in Ghidra - it copies the POV into an FMinimalViewInfo
            // (Location@0x00, Rotation@0x18, FOV@0x30 written via MOVSS after
            // the PCM FOV-getter call) and calls GetPlayerViewPoint to write
            // the final view rotation at outView+0x18.
            /* kViewBuilderRva */ 0x03e72a70ULL,
            /* MinimalViewInfoLayout */ {
                /* kRotationOffset */ 0x18,
                /* kFovOffset      */ 0x30,
            },
            // GUObjectArray.ObjObjects + FNamePool, derived by
            // scripts/derive_globals.py (engine-signature scan): allocator
            // `mov [rip],rax` @ fn+0x18e -> ObjObjects; FName decoder
            // `lea r8,[rip+pool]` (pool - init_flag == 0x267 cross-check).
            // Struct-layout fields are UE5.6.x engine-ABI constants.
            /* UObjectGlobals */ {
                /* kObjObjects       */ 0x0941be40ULL,
                /* kObjObjects_Num   */ 0x14,
                /* kFUObjectItemSize */ 0x18,
                /* kChunkNumElems    */ 0x10000,
                /* kFNamePool        */ 0x09337cc0ULL,
                /* kFNamePoolBlocks  */ 0x10,
                /* kClassPrivate     */ 0x10,
                /* kNamePrivate      */ 0x18,
                /* kOuterPrivate     */ 0x20,
            },
        },
    };
}
