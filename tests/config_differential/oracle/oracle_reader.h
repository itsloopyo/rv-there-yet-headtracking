#pragma once

#include <string>
#include <tuple>
#include <vector>

// The oracle: what v0.3.0, the newest published build, ran on after reading
// HeadTracking.ini. oracle_reader.cpp transcribes its reader and startup code
// and is compiled with the mod's and cameraunlock-core's namespaces renamed
// (CMakeLists.txt), so it links beside today's code. Nothing core declares
// crosses this header.
namespace rvty_oracle {

enum Action { kToggle = 0, kCycleMode = 1, kYawMode = 2 };

// One HotkeyPoller registration: the action, the code, and 3 where the
// callback is ChordGuarded (fires only while Ctrl and Shift are both held), 0
// where it is not.
using Registration = std::tuple<int, int, unsigned>;

// What LoadConfig handed g_posProcessor.SetSettings, field for field.
struct Position {
    float sensitivity_x = 0, sensitivity_y = 0, sensitivity_z = 0;
    float limit_x = 0, limit_y = 0, limit_y_down = 0, limit_z = 0, limit_z_back = 0;
    float local_smoothing = 0, remote_smoothing = 0;
    bool invert_x = false, invert_y = false, invert_z = false;
};

struct Published {
    int udp_port = 0;
    bool tracking_enabled = false;
    float yaw_sens = 0, pitch_sens = 0, roll_sens = 0;
    bool invert_yaw = false, invert_pitch = false, invert_roll = false;
    float local_smoothing = 0, remote_smoothing = 0;
    bool world_space_yaw = false;
    bool rotation_enabled = false;
    bool position_enabled = false;
    int tracking_mode = 0;
    Position position;
    // What LoadConfig handed reticle::Configure.
    bool reticle_show = false;
    float reticle_scale = 0;
    float reticle_vertical_scale = 0;
    std::vector<std::string> reticle_target_names;
    std::vector<Registration> hotkeys;
};

// `dll_dir` is the folder the DLL loaded from, with its trailing backslash, as
// DllDirNarrow returned it.
Published Read(const std::string& dll_dir);

// v0.3.0's hand-off from the processor's offset (metres, tracker axes) to the
// camera's surge, sway and heave (UE centimetres).
void OffsetToUE(float x, float y, float z, double& surge, double& sway, double& heave);

}  // namespace rvty_oracle
