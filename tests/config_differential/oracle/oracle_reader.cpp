// v0.3.0's reader and startup code, transcribed from
// v0.3.0:src/RVThereYetHeadTracking/headtracking_mod.cpp (tag 804ebb2):
//
//   lines 70-77, 102-112   the globals the reader writes, at their initial values
//   lines 214-217          the hand-off from the processor's offset to the camera
//   lines 481-516          ReadFiniteFloat and WarnRetiredSmoothingKey, verbatim
//   lines 521-605          LoadConfig, verbatim apart from the globals, the
//                          processor and reticle::Configure's argument becoming
//                          fields of Published
//   lines 718-726          the hotkey registrations, as data
//
// and v0.3.0:src/RVThereYetHeadTracking/reticle.h lines 15-23 (reticle::Settings,
// whose defaults stand when LoadConfig returns before configuring the reticle).
//
// The whole file cannot be compiled into a test (it hooks the game), so what is
// transcribed is everything between reading the file and the state the session
// starts in. src/logging.h beside this file is a byte copy of v0.3.0's, and
// core/ holds byte copies of the cameraunlock-core headers v0.3.0's pin
// (3465659) had that core has changed since: data/position_settings.h,
// processing/position_processor.h, protocol/udp_receiver.h and the two headers
// it includes that changed, protocol/socket_types.h and protocol/udp_socket.h.
// Every other core source this file compiles holds the same bytes at 3465659
// and at this repo's pin (CMakeLists.txt checks both).

#include "oracle_reader.h"

#include <cmath>
#include <memory>
#include <string>
#include <windows.h>

#include "src/logging.h"

#include "cameraunlock/config/ini_reader.h"
#include "cameraunlock/data/position_settings.h"
#include "cameraunlock/processing/position_processor.h"
#include "cameraunlock/protocol/udp_receiver.h"

namespace rvty_oracle {

namespace Log = ::cameraunlock::logging;

namespace {

// strtod accepts "nan"/"inf" and out-of-float-range values; a
// non-finite sensitivity/limit would flow into the view rotation we
// write back to the game every frame and poison it with NaN. Same
// config-boundary rule as the Port check: fall back with a loud log.
float ReadFiniteFloat(const cameraunlock::IniReader& ini,
                      const char* section, const char* key, float def)
{
    const float v = ini.ReadFloat(section, key, def);
    if (!std::isfinite(v)) {
        Log::Line("config: %s.%s is not a finite number; using default %.2f",
            section, key, def);
        return def;
    }
    return v;
}

// Warned once per process rather than once per load: config is
// reloadable, and repeating this on every reload buries it.
//
// The old value is deliberately NOT migrated into the new keys. The
// single smoothing value carried a hidden 0.15 floor, so the number in
// an existing config does not mean what it used to: copying it across
// would hand a local user smoothing they never chose under the new
// semantics, and copying it into only one of the two keys would be a
// guess about which connection they were on.
void WarnRetiredSmoothingKey(const cameraunlock::IniReader& ini,
                             const char* section, const char* key)
{
    static bool warned = false;
    if (warned) return;
    if (ini.ReadString(section, key, "").empty()) return;
    warned = true;
    Log::Line("config: key [%s] %s has been retired and is IGNORED. Smoothing is "
        "now two keys: LocalSmoothing (default 0, applies to a tracker on this "
        "machine) and RemoteSmoothing (default 0.15, applies to a tracker on the "
        "network). The old value is not migrated because the semantics changed - "
        "it carried a hidden 0.15 floor that no longer exists. Set the two new "
        "keys.",
        section, key);
}

// reticle::Settings at v0.3.0.
struct ReticleSettings
{
    bool  show = true;
    float scale = 1.0f;
    float verticalScale = 1.0f;
    std::vector<std::string> targetNames;
};

}  // namespace

Published Read(const std::string& dll_dir)
{
    // The globals, at their initial values.
    Published g;
    g.tracking_enabled = true;
    g.world_space_yaw = true;
    g.rotation_enabled = true;
    g.position_enabled = true;
    g.tracking_mode = 0;
    g.local_smoothing = 0.0f;
    g.remote_smoothing = 0.15f;
    g.yaw_sens = 1.0f;
    g.pitch_sens = 1.0f;
    g.roll_sens = 1.0f;
    g.invert_yaw = false;
    g.invert_pitch = false;
    g.invert_roll = false;
    cameraunlock::PositionProcessor g_posProcessor;
    ReticleSettings configured;

    int outPort = 0, outYawModeKey = 0;
    [&]() {
        outPort = cameraunlock::UdpReceiver::kDefaultPort;
        outYawModeKey = 0x22;  // Page Down

        cameraunlock::IniReader ini;
        if (!ini.Open(dll_dir + "HeadTracking.ini")) {
            Log::Line("config: no HeadTracking.ini next to DLL; using defaults");
            return;
        }

        const int cfgPort = ini.ReadInt("Network", "Port", outPort);
        // Port is later narrowed to uint16 for bind(); an out-of-range value
        // would wrap silently and the mod would bind a different port than the
        // user asked for, then appear "loaded but receiving nothing". Validate
        // at this config boundary and fall back to the default with a loud log.
        if (cfgPort < 1 || cfgPort > 65535) {
            Log::Line("config: Port=%d out of range 1-65535; using default %d",
                cfgPort, outPort);
        } else {
            outPort = cfgPort;
        }
        g.tracking_enabled = ini.ReadBool("Tracking", "EnableOnStartup", true);
        g.yaw_sens   = ReadFiniteFloat(ini, "Tracking", "YawSensitivity", 1.0f);
        g.pitch_sens = ReadFiniteFloat(ini, "Tracking", "PitchSensitivity", 1.0f);
        g.roll_sens  = ReadFiniteFloat(ini, "Tracking", "RollSensitivity", 1.0f);
        g.invert_yaw   = ini.ReadBool("Tracking", "InvertYaw", false);
        g.invert_pitch = ini.ReadBool("Tracking", "InvertPitch", false);
        g.invert_roll  = ini.ReadBool("Tracking", "InvertRoll", false);
        g.local_smoothing  = ReadFiniteFloat(ini, "Tracking", "LocalSmoothing", 0.0f);
        g.remote_smoothing = ReadFiniteFloat(ini, "Tracking", "RemoteSmoothing", 0.15f);
        WarnRetiredSmoothingKey(ini, "Tracking", "Smoothing");
        WarnRetiredSmoothingKey(ini, "Position", "Smoothing");
        g.world_space_yaw = ini.ReadBool("Tracking", "WorldSpaceYaw", true);
        outYawModeKey = ini.ReadHex("Hotkeys", "ToggleYawMode", outYawModeKey);

        cameraunlock::PositionSettings ps = g_posProcessor.GetSettings();
        g.position_enabled = ini.ReadBool("Position", "Enabled", true);
        ps.sensitivity_x = ReadFiniteFloat(ini, "Position", "SensitivityX", 1.0f);
        ps.sensitivity_y = ReadFiniteFloat(ini, "Position", "SensitivityY", 1.0f);
        ps.sensitivity_z = ReadFiniteFloat(ini, "Position", "SensitivityZ", 1.0f);
        ps.invert_x = ini.ReadBool("Position", "InvertX", false);
        ps.invert_y = ini.ReadBool("Position", "InvertY", false);
        ps.invert_z = ini.ReadBool("Position", "InvertZ", false);
        ps.limit_x = ReadFiniteFloat(ini, "Position", "LimitX", 0.30f);
        ps.limit_y = ReadFiniteFloat(ini, "Position", "LimitY", 0.20f);
        ps.limit_z = ReadFiniteFloat(ini, "Position", "LimitZ", 0.40f);
        ps.limit_z_back = ReadFiniteFloat(ini, "Position", "LimitZBack", 0.10f);
        // Position shares the [Tracking] smoothing parameters; the connection
        // flag that picks between them lives on the processor.
        ps.local_smoothing = g.local_smoothing;
        ps.remote_smoothing = g.remote_smoothing;
        g_posProcessor.SetSettings(ps);

        ReticleSettings rs;
        rs.show = ini.ReadBool("Tracking", "ShowReticle", true);
        rs.scale = ReadFiniteFloat(ini, "Reticle", "Scale", 1.0f);
        rs.verticalScale = ReadFiniteFloat(ini, "Reticle", "VerticalScale", 1.0f);
        // Comma-separated widget names to move to the aim point; empty
        // keeps the built-in defaults.
        const std::string names = ini.ReadString("Reticle", "WidgetNames", "");
        if (!names.empty()) {
            std::size_t start = 0;
            while (start <= names.size()) {
                std::size_t comma = names.find(',', start);
                std::string tok = names.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                std::size_t a = tok.find_first_not_of(" \t");
                std::size_t b = tok.find_last_not_of(" \t");
                if (a != std::string::npos) rs.targetNames.push_back(tok.substr(a, b - a + 1));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        configured = rs;
    }();

    g.udp_port = outPort;
    const cameraunlock::PositionSettings& ps = g_posProcessor.GetSettings();
    g.position.sensitivity_x = ps.sensitivity_x;
    g.position.sensitivity_y = ps.sensitivity_y;
    g.position.sensitivity_z = ps.sensitivity_z;
    g.position.limit_x = ps.limit_x;
    g.position.limit_y = ps.limit_y;
    g.position.limit_y_down = ps.limit_y_down;
    g.position.limit_z = ps.limit_z;
    g.position.limit_z_back = ps.limit_z_back;
    g.position.local_smoothing = ps.local_smoothing;
    g.position.remote_smoothing = ps.remote_smoothing;
    g.position.invert_x = ps.invert_x;
    g.position.invert_y = ps.invert_y;
    g.position.invert_z = ps.invert_z;
    g.reticle_show = configured.show;
    g.reticle_scale = configured.scale;
    g.reticle_vertical_scale = configured.verticalScale;
    g.reticle_target_names = configured.targetNames;

    // Toggle tracking: End / Ctrl+Shift+Y
    g.hotkeys.push_back({kToggle, VK_END, 0});
    g.hotkeys.push_back({kToggle, 0x59 /* Y */, 3});
    // Cycle tracking mode: Page Up / Ctrl+Shift+G
    g.hotkeys.push_back({kCycleMode, VK_PRIOR, 0});
    g.hotkeys.push_back({kCycleMode, 0x47 /* G */, 3});
    // Yaw mode (world/local): Page Down (or [Hotkeys] ToggleYawMode) / Ctrl+Shift+H
    g.hotkeys.push_back({kYawMode, outYawModeKey, 0});
    g.hotkeys.push_back({kYawMode, 0x48 /* H */, 3});
    return g;
}

void OffsetToUE(float x, float y, float z, double& surge, double& sway, double& heave)
{
    constexpr double kMetersToUE = 100.0;
    surge = static_cast<double>(z) * kMetersToUE;  // forward
    sway  = static_cast<double>(x) * kMetersToUE;  // right
    heave = static_cast<double>(y) * kMetersToUE;  // up
}

}  // namespace rvty_oracle
