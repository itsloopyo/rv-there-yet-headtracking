// Behaviour lock for the 6DOF lean boundary: which way the view moves for a
// physical lean, and how much of the asymmetric budget each direction gets.

#include <cmath>
#include <cstdio>
#include <utility>

#include "position_boundary.h"

#include <cameraunlock/processing/position_processor.h>

namespace {

namespace pd = rvty::position;

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    ++g_failures;
}

void CheckNear(double actual, double expected, const char* what) {
    if (std::fabs(actual - expected) <= 1e-3) return;
    std::printf("FAIL: %s (expected %.6f, got %.6f)\n", what, expected, actual);
    ++g_failures;
}

// The processor's z runs negative for a forward lean; UE's camera-local
// forward is +X, which surge feeds.
void ForwardLeanMovesViewForward() {
    double surge = 0.0, sway = 0.0, heave = 0.0;
    pd::TrackerOffsetToUE(cameraunlock::math::Vec3(0.0f, 0.0f, -0.25f), surge, sway, heave);
    Check(surge > 0.0, "forward lean (processor z < 0) moves the view forward");

    pd::TrackerOffsetToUE(cameraunlock::math::Vec3(0.0f, 0.0f, 0.25f), surge, sway, heave);
    Check(surge < 0.0, "backward lean (processor z > 0) moves the view back");

    pd::TrackerOffsetToUE(cameraunlock::math::Vec3(0.0f, 0.1f, 0.0f), surge, sway, heave);
    CheckNear(heave, 0.1 * pd::kMetersToUE, "up maps to +heave, in centimetres");
}

// The processor as the mod runs it at CameraUnlock.ini's defaults: no
// sensitivity or inversion of its own, the default limits.
cameraunlock::PositionSettings DefaultSettings() {
    cameraunlock::PositionSettings s;
    s.limit_x = pd::kLimitX;
    s.limit_y = pd::kLimitY;
    s.limit_y_down = pd::kLimitYDown;
    s.limit_z = pd::kLimitZ;
    s.limit_z_back = pd::kLimitZBack;
    return s;
}

double SaturatedSurge(float rawZ) {
    cameraunlock::PositionProcessor processor;
    processor.SetSettings(DefaultSettings());
    const cameraunlock::PositionData raw(0.0f, 0.0f, rawZ);
    // Two ticks so the smoothing state has settled on the clamped value.
    processor.Process(raw, cameraunlock::math::Quat4::Identity(), 1.0f);
    const cameraunlock::math::Vec3 out =
        processor.Process(raw, cameraunlock::math::Quat4::Identity(), 1.0f);
    double surge = 0.0, sway = 0.0, heave = 0.0;
    pd::TrackerOffsetToUE(out, surge, sway, heave);
    return surge;
}

void LeanBudgetsAreNotReversed() {
    // A metre of physical lean either way, far past both limits, so the output
    // is whichever budget that direction actually got.
    CheckNear(SaturatedSurge(-1.0f), pd::kLimitZ * pd::kMetersToUE,
              "forward lean gets the LimitZ budget");
    CheckNear(SaturatedSurge(1.0f), -pd::kLimitZBack * pd::kMetersToUE,
              "backward lean gets the LimitZBack budget");
}

// Negating the processor's output on an axis is the same as flipping that
// axis's inversion and swapping its two limits, bit for bit, smoothing
// included. The config differential test relies on it to write down a reading
// whose axis code did not negate sway and surge in the frame of the axis code
// that does (tests/config_differential/differential_tests.cpp).
void NegatingAnAxisIsFlippingItsInversionAndSwappingItsLimits() {
    cameraunlock::PositionSettings a;
    a.sensitivity_x = 1.3f;
    a.sensitivity_y = 0.7f;
    a.sensitivity_z = 1.1f;
    a.limit_x = 0.25f;
    a.limit_y = 0.30f;
    a.limit_y_down = 0.12f;
    a.limit_z = 0.10f;
    a.limit_z_back = 0.40f;
    a.local_smoothing = 0.35f;
    a.invert_x = true;
    a.invert_z = true;

    cameraunlock::PositionSettings b = a;
    b.invert_x = !a.invert_x;
    b.invert_y = !a.invert_y;
    std::swap(b.limit_y, b.limit_y_down);
    b.invert_z = !a.invert_z;
    std::swap(b.limit_z, b.limit_z_back);

    cameraunlock::PositionProcessor pa, pb;
    pa.SetSettings(a);
    pb.SetSettings(b);
    const float inputs[][3] = {{0.05f, -0.02f, -0.3f}, {0.6f, 0.4f, 0.8f}, {-0.2f, -0.5f, -0.9f},
                               {0.01f, 0.03f, 0.05f}, {-1.0f, 1.0f, 0.2f}, {0.0f, 0.0f, 0.0f}};
    bool same = true;
    for (const auto& in : inputs) {
        const cameraunlock::PositionData raw(in[0], in[1], in[2]);
        const cameraunlock::math::Vec3 oa = pa.Process(raw, cameraunlock::math::Quat4::Identity(), 0.016f);
        const cameraunlock::math::Vec3 ob = pb.Process(raw, cameraunlock::math::Quat4::Identity(), 0.016f);
        same = same && -oa.x == ob.x && -oa.y == ob.y && -oa.z == ob.z;
    }
    Check(same, "negating every axis equals flipping each inversion and swapping each axis's limits");
}

} // namespace

int main() {
    ForwardLeanMovesViewForward();
    LeanBudgetsAreNotReversed();
    NegatingAnAxisIsFlippingItsInversionAndSwappingItsLimits();

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all position checks passed\n");
    return 0;
}
