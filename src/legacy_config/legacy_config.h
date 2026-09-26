#pragma once

#include <string>
#include <vector>

// The pre-canonical HeadTracking.ini reader, frozen. It reads a file the way
// the last build before the canonical config format did, so a player's old
// file is carried over as that build read it. Never edit anything in this
// folder: CMakeLists.txt pins every file here by hash.
//
// Frozen from src/RVThereYetHeadTracking/headtracking_mod.cpp at 0bef86a
// (ReadFiniteFloat, WarnRetiredVerticalScaleKey, WarnRetiredSmoothingKey and
// LoadConfig), with three changes: it fills this frozen copy of that commit's
// settings and their defaults instead of the mod's globals, the position
// processor and the reticle, it writes nothing, and it lives in namespace
// RVThereYetHeadTracking::legacy. The defaults are written as the literals the
// code held then (UdpReceiver::kDefaultPort, cameraunlock-core's smoothing
// defaults and PositionSettings limits, and position_boundary.h's
// sensitivities and inversions), so a later change to core or to the mod cannot
// move what an old file means.
namespace RVThereYetHeadTracking::legacy {

struct Config {
    // [Network]
    int udp_port = 4242;

    // [Tracking]
    bool enable_on_startup = true;
    float yaw_sensitivity = 1.0f;
    float pitch_sensitivity = 1.0f;
    float roll_sensitivity = 1.0f;
    bool invert_yaw = false;
    bool invert_pitch = false;
    bool invert_roll = false;
    float local_smoothing = 0.0f;
    float remote_smoothing = 0.15f;
    bool world_space_yaw = true;
    bool show_reticle = true;

    // [Hotkeys]. A virtual-key code, read with IniReader::ReadHex and not
    // range-checked: the build registered whatever it read.
    int yaw_mode_key = 0x22;  // Page Down

    // [Position]. LimitY set both vertical bounds, up and down.
    bool position_enabled = true;
    float position_sensitivity_x = 1.0f;
    float position_sensitivity_y = 1.0f;
    float position_sensitivity_z = 1.0f;
    bool position_invert_x = false;
    bool position_invert_y = false;
    bool position_invert_z = false;
    float limit_x = 0.30f;
    float limit_y = 0.20f;
    float limit_z = 0.40f;
    float limit_z_back = 0.10f;

    // [Reticle]. An empty list of widget names kept the built-in ones.
    float reticle_scale = 1.0f;
    std::vector<std::string> reticle_widget_names;
};

// Reads `ini_path` into `out`; keys the file lacks keep their defaults. Returns
// whether the file was there to read (IniReader::Open), which is when the
// build logged its settings rather than "using defaults".
bool Load(const std::string& ini_path, Config& out);

struct Key {
    const char* section;
    const char* key;
};

// Every key Load takes a value from. The retired [Tracking] Smoothing,
// [Position] Smoothing and [Reticle] VerticalScale are read only to warn that
// they are ignored, so they are not among them.
std::vector<Key> ReadKeys();

}  // namespace RVThereYetHeadTracking::legacy
