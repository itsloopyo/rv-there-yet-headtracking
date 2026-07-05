#include "build_profile.h"

// Xbox / Game Pass WinGDK build of RV There Yet (Ride-WinGDK-Shipping.exe,
// UE5). The on-disk EXE under C:\XboxGames is ACL-locked by GDK, so its PE
// fingerprint and RVAs are captured by dumping the running process (see
// scripts/dump-running-exe.ps1), not by reading the file directly.
//
// Fingerprint captured from the running WinGDK process on 2026-07-01 (Xbox
// package version 1.2.11.0). RVAs derived on 2026-07-02 by dumping the running
// process (scripts/dump-running-exe.ps1 rebases to 0x140000000) and running
// scripts/derive_rvas.py + derive_globals.py over the dump. The derivation
// method was validated against the Steam EXE first (it reproduces every
// steam_offsets.cpp RVA exactly). The view builder is the render-caller whose
// structural fingerprint (size 0x30b, call[+0x7f8]@+0x228 -> call[+0x828]@+0x241,
// Rotation write at out+0x18) is byte-identical to the confirmed Steam builder
// FUN_143e72a70. FMinimalViewInfo field offsets and the UObject/FName struct
// layout are engine-ABI bound (UE5.6.x LWC), not packaging-bound, so they carry
// over from the Steam profile unchanged. Same append-only policy as
// steam_offsets.cpp.

namespace RVThereYetHeadTracking::builds
{
    extern const BuildProfile kGdkProfile_20260701;

    // ---- Xbox WinGDK build (PE TS 0xee900652, package 1.2.11.0) ----
    const BuildProfile kGdkProfile_20260701 = {
        /* Name        */ "gdk-wingdk-20260701",
        /* Fingerprint */ { 0xee900652u, 0x0a2f5000u, 0x09f5da90u },
        /* Offsets     */ {
            /* kViewBuilderRva */ 0x03da2cd0ULL,
            /* MinimalViewInfoLayout */ {
                /* kRotationOffset */ 0x18,
                /* kFovOffset      */ 0x30,
            },
            // Derived per-build via derive_globals.py from the GDK dump.
            // Struct-layout fields carry over (engine-ABI).
            /* UObjectGlobals */ {
                /* kObjObjects       */ 0x09799240ULL,
                /* kObjObjects_Num   */ 0x14,
                /* kFUObjectItemSize */ 0x18,
                /* kChunkNumElems    */ 0x10000,
                /* kFNamePool        */ 0x096b5000ULL,
                /* kFNamePoolBlocks  */ 0x10,
                /* kClassPrivate     */ 0x10,
                /* kNamePrivate      */ 0x18,
                /* kOuterPrivate     */ 0x20,
            },
        },
    };
}
