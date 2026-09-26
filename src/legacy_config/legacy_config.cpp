// Frozen. See legacy_config.h.

#include "legacy_config.h"

#include <cmath>
#include <string>
#include <vector>

#include "logging.h"

#include "cameraunlock/config/ini_reader.h"

namespace RVThereYetHeadTracking::legacy {

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

void WarnRetiredVerticalScaleKey(const cameraunlock::IniReader& ini)
{
    static bool warned = false;
    if (warned) return;
    if (ini.ReadString("Reticle", "VerticalScale", "").empty()) return;
    warned = true;
    Log::Line("config: key [Reticle] VerticalScale has been retired and is IGNORED. "
        "It existed to correct a projection that used the wrong vertical term; "
        "the reticle now projects through the shared Hor+ model and needs no "
        "per-axis correction. Remove the key.");
}

// Warned once per process. The old single Smoothing value is not carried into
// the two keys that replaced it.
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

}  // namespace

bool Load(const std::string& ini_path, Config& out)
{
    cameraunlock::IniReader ini;
    if (!ini.Open(ini_path)) {
        Log::Line("config: no HeadTracking.ini next to DLL; using defaults");
        return false;
    }

    const int cfgPort = ini.ReadInt("Network", "Port", out.udp_port);
    // Port is later narrowed to uint16 for bind(); an out-of-range value
    // would wrap silently and the mod would bind a different port than the
    // user asked for, then appear "loaded but receiving nothing". Validate
    // at this config boundary and fall back to the default with a loud log.
    if (cfgPort < 1 || cfgPort > 65535) {
        Log::Line("config: Port=%d out of range 1-65535; using default %d",
            cfgPort, out.udp_port);
    } else {
        out.udp_port = cfgPort;
    }
    out.enable_on_startup = ini.ReadBool("Tracking", "EnableOnStartup", true);
    out.yaw_sensitivity   = ReadFiniteFloat(ini, "Tracking", "YawSensitivity", 1.0f);
    out.pitch_sensitivity = ReadFiniteFloat(ini, "Tracking", "PitchSensitivity", 1.0f);
    out.roll_sensitivity  = ReadFiniteFloat(ini, "Tracking", "RollSensitivity", 1.0f);
    out.invert_yaw   = ini.ReadBool("Tracking", "InvertYaw", false);
    out.invert_pitch = ini.ReadBool("Tracking", "InvertPitch", false);
    out.invert_roll  = ini.ReadBool("Tracking", "InvertRoll", false);
    out.local_smoothing  = ReadFiniteFloat(ini, "Tracking", "LocalSmoothing", 0.0f);
    out.remote_smoothing = ReadFiniteFloat(ini, "Tracking", "RemoteSmoothing", 0.15f);
    WarnRetiredSmoothingKey(ini, "Tracking", "Smoothing");
    WarnRetiredSmoothingKey(ini, "Position", "Smoothing");
    out.world_space_yaw = ini.ReadBool("Tracking", "WorldSpaceYaw", true);
    out.yaw_mode_key = ini.ReadHex("Hotkeys", "ToggleYawMode", out.yaw_mode_key);

    out.position_enabled = ini.ReadBool("Position", "Enabled", true);
    out.position_sensitivity_x = ReadFiniteFloat(ini, "Position", "SensitivityX", 1.0f);
    out.position_sensitivity_y = ReadFiniteFloat(ini, "Position", "SensitivityY", 1.0f);
    out.position_sensitivity_z = ReadFiniteFloat(ini, "Position", "SensitivityZ", 1.0f);
    out.position_invert_x = ini.ReadBool("Position", "InvertX", false);
    out.position_invert_y = ini.ReadBool("Position", "InvertY", false);
    out.position_invert_z = ini.ReadBool("Position", "InvertZ", false);
    out.limit_x = ReadFiniteFloat(ini, "Position", "LimitX", 0.30f);
    out.limit_y = ReadFiniteFloat(ini, "Position", "LimitY", 0.20f);
    out.limit_z = ReadFiniteFloat(ini, "Position", "LimitZ", 0.40f);
    out.limit_z_back = ReadFiniteFloat(ini, "Position", "LimitZBack", 0.10f);

    out.show_reticle = ini.ReadBool("Tracking", "ShowReticle", true);
    out.reticle_scale = ReadFiniteFloat(ini, "Reticle", "Scale", 1.0f);
    WarnRetiredVerticalScaleKey(ini);
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
            if (a != std::string::npos) out.reticle_widget_names.push_back(tok.substr(a, b - a + 1));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    Log::Line("config: port=%d enable=%s sens(Y/P/R)=%.2f/%.2f/%.2f "
        "invert(Y/P/R)=%d/%d/%d localSmoothing=%.2f remoteSmoothing=%.2f "
        "worldYaw=%s position=%s",
        out.udp_port, out.enable_on_startup ? "true" : "false",
        out.yaw_sensitivity, out.pitch_sensitivity, out.roll_sensitivity,
        out.invert_yaw, out.invert_pitch, out.invert_roll,
        out.local_smoothing, out.remote_smoothing,
        out.world_space_yaw ? "true" : "false",
        out.position_enabled ? "true" : "false");
    return true;
}

std::vector<Key> ReadKeys()
{
    return {
        {"Network", "Port"},
        {"Tracking", "EnableOnStartup"},
        {"Tracking", "YawSensitivity"},
        {"Tracking", "PitchSensitivity"},
        {"Tracking", "RollSensitivity"},
        {"Tracking", "InvertYaw"},
        {"Tracking", "InvertPitch"},
        {"Tracking", "InvertRoll"},
        {"Tracking", "LocalSmoothing"},
        {"Tracking", "RemoteSmoothing"},
        {"Tracking", "WorldSpaceYaw"},
        {"Tracking", "ShowReticle"},
        {"Hotkeys", "ToggleYawMode"},
        {"Position", "Enabled"},
        {"Position", "SensitivityX"},
        {"Position", "SensitivityY"},
        {"Position", "SensitivityZ"},
        {"Position", "InvertX"},
        {"Position", "InvertY"},
        {"Position", "InvertZ"},
        {"Position", "LimitX"},
        {"Position", "LimitY"},
        {"Position", "LimitZ"},
        {"Position", "LimitZBack"},
        {"Reticle", "Scale"},
        {"Reticle", "WidgetNames"},
    };
}

}  // namespace RVThereYetHeadTracking::legacy
