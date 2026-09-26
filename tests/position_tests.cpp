// Behaviour lock for the 6DOF lean boundary: which way the view moves for a
// physical lean, how much of the asymmetric budget each direction gets, and
// whether the shipped HeadTracking.ini still agrees with the code defaults.
//
// The last of those is the bug that shipped. The INI carried InvertZ=true with
// LimitZ and LimitZBack swapped, and the two errors cancelled, so the mod
// behaved correctly for anyone who kept the file. The code defaults said the
// opposite, and the mod writes no INI of its own, so a user who deleted it got
// the lean inverted and the budgets mirrored.

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
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

cameraunlock::PositionSettings DefaultSettings() {
    cameraunlock::PositionSettings s;
    s.sensitivity_x = pd::kSensitivityX;
    s.sensitivity_y = pd::kSensitivityY;
    s.sensitivity_z = pd::kSensitivityZ;
    s.invert_x = pd::kInvertX;
    s.invert_y = pd::kInvertY;
    s.invert_z = pd::kInvertZ;
    s.limit_x = pd::kLimitX;
    s.limit_y = pd::kLimitY;
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

// Minimal reader for the shipped file: last "key = value" wins, sections are
// tracked so [Position] keys are not confused with same-named ones elsewhere.
std::string ReadIniValue(const char* section, const char* key) {
    std::ifstream file(RVTY_SHIPPED_INI);
    if (!file) {
        std::printf("FAIL: cannot open %s\n", RVTY_SHIPPED_INI);
        ++g_failures;
        return std::string();
    }
    std::string line, current, found;
    while (std::getline(file, line)) {
        const std::size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos || line[start] == ';' || line[start] == '#') continue;
        const std::size_t end = line.find_last_not_of(" \t\r");
        const std::string trimmed = line.substr(start, end - start + 1);
        if (trimmed.front() == '[') {
            current = trimmed.substr(1, trimmed.find(']') - 1);
            continue;
        }
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos || current != section) continue;
        std::string k = trimmed.substr(0, eq);
        std::string v = trimmed.substr(eq + 1);
        const std::size_t ke = k.find_last_not_of(" \t");
        k = k.substr(0, ke + 1);
        const std::size_t vs = v.find_first_not_of(" \t");
        if (vs != std::string::npos) v = v.substr(vs); else v.clear();
        if (k == key) found = v;
    }
    return found;
}

void CheckIniBool(const char* key, bool expected) {
    const std::string value = ReadIniValue("Position", key);
    const bool actual = (value == "true" || value == "1");
    if (actual == expected && !value.empty()) return;
    std::printf("FAIL: shipped INI [Position] %s is \"%s\", code default is %s\n",
                key, value.c_str(), expected ? "true" : "false");
    ++g_failures;
}

void CheckIniFloat(const char* key, float expected) {
    const std::string value = ReadIniValue("Position", key);
    if (!value.empty() && std::fabs(std::stod(value) - expected) <= 1e-6) return;
    std::printf("FAIL: shipped INI [Position] %s is \"%s\", code default is %.3f\n",
                key, value.c_str(), expected);
    ++g_failures;
}

void ShippedIniMatchesCodeDefaults() {
    CheckIniBool("InvertX", pd::kInvertX);
    CheckIniBool("InvertY", pd::kInvertY);
    CheckIniBool("InvertZ", pd::kInvertZ);
    CheckIniFloat("SensitivityX", pd::kSensitivityX);
    CheckIniFloat("SensitivityY", pd::kSensitivityY);
    CheckIniFloat("SensitivityZ", pd::kSensitivityZ);
    CheckIniFloat("LimitX", pd::kLimitX);
    CheckIniFloat("LimitY", pd::kLimitY);
    CheckIniFloat("LimitZ", pd::kLimitZ);
    CheckIniFloat("LimitZBack", pd::kLimitZBack);
}

} // namespace

int main() {
    ForwardLeanMovesViewForward();
    LeanBudgetsAreNotReversed();
    NegatingAnAxisIsFlippingItsInversionAndSwappingItsLimits();
    ShippedIniMatchesCodeDefaults();

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all position checks passed\n");
    return 0;
}
