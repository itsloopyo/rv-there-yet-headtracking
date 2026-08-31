#pragma once

#include <cameraunlock/data/position_settings.h>
#include <cameraunlock/math/vec3.h>

namespace rvty::position {

// Defaults for the INI's [Position] section. The mod writes no INI of its own,
// so a user who deletes HeadTracking.ini falls back to these; the shipped file
// must therefore repeat them exactly. tests/position_tests.cpp checks that it
// does - the two drifted apart once already, and the shipped file's inverted
// depth plus mirrored limits happened to cancel the code path's, so nothing
// looked wrong until the file was missing.
inline constexpr float kSensitivityX = 1.0f;
inline constexpr float kSensitivityY = 1.0f;
inline constexpr float kSensitivityZ = 1.0f;
inline constexpr bool  kInvertX = false;
inline constexpr bool  kInvertY = false;
inline constexpr bool  kInvertZ = false;
inline constexpr float kLimitX = cameraunlock::PositionSettings{}.limit_x;
inline constexpr float kLimitY = cameraunlock::PositionSettings{}.limit_y;
inline constexpr float kLimitZ = cameraunlock::PositionSettings{}.limit_z;
inline constexpr float kLimitZBack = cameraunlock::PositionSettings{}.limit_z_back;

// UE works in centimetres; the processor hands out metres.
inline constexpr double kMetersToUE = 100.0;

// Map a processed offset (metres, tracker axes: x = right, y = up, z = depth)
// onto UE camera-local surge (forward), sway (right) and heave (up), in
// centimetres.
//
// Depth is negated here, at the engine boundary, rather than through the
// processor's InvertZ. The processor inverts BEFORE its asymmetric clamp of
// [-LimitZ, +LimitZBack], so flipping the sign there hands the generous 0.40m
// allowance to leaning back and the 0.10m anti-clipping allowance to leaning
// in. Negative z is the forward lean throughout the library.
//
// Sway is negated here for the same reason of keeping one place to look: the
// axis was mirrored by an InvertX the code defaulted to false and only the
// shipped INI set true, so the sign lived in the config file rather than in
// the code. X is clamped symmetrically, so moving it changes nothing but where
// it is written.
inline void TrackerOffsetToUE(const cameraunlock::math::Vec3& off,
                              double& surge, double& sway, double& heave) {
    surge = -static_cast<double>(off.z) * kMetersToUE;
    sway  = -static_cast<double>(off.x) * kMetersToUE;
    heave =  static_cast<double>(off.y) * kMetersToUE;
}

} // namespace rvty::position
