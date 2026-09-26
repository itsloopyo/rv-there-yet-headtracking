// The differential test for the conversion from HeadTracking.ini to
// CameraUnlock.ini.
//
// Two readings of every input so far:
//
//   Oracle     v0.3.0's reader and startup code, the newest published build
//              (oracle/oracle_reader.cpp; the rolling dev build is older)
//   Import     the frozen reader in src/legacy_config/, and the startup code
//              the commit that froze it ran it through
//
// Comparison 1, oracle against import, is what a player sees change that the
// conversion did not cause: commits since v0.3.0 that change how the file is
// read or what the session does with it. Each one is applied to the oracle's
// reading below, with its commit, and nothing else may differ, floats bit for
// bit:
//
// - 5341862 moved the X and Z axis flips out of the shipped file and into the
//   code (position_boundary.h negates sway and surge) and shipped InvertX=false,
//   InvertZ=false, LimitZ=0.40 and LimitZBack=0.10 in place of v0.3.0's true,
//   true, 0.10 and 0.40. The same file read at that commit leans the other way
//   on both axes, and the file v0.3.0 shipped hands leaning forward the 0.10
//   budget it meant for leaning back.
// - 5341862 also retired [Reticle] VerticalScale: the reticle projection no
//   longer takes a vertical factor, so the key is only named in the log.
// - 2c2cd83 applies LimitY downwards too. v0.3.0 left the downward limit at the
//   processor's 0.20 whatever LimitY said.
// - 5341862 guards End, Page Up and the yaw key (NavGuarded), so they no longer
//   fire while Ctrl and Shift are both held. A record names a binding by its key
//   and modifiers, which that does not change.
//
// Every reading is written down in one frame: the processor settings that,
// handed to the axis code in position_boundary.h, move the camera the way that
// reading's own axis code moved it. v0.3.0's axis code did not negate sway and
// surge, so its X and Z inversions are flipped and its Z limits swapped when its
// reading is written down. The processor negates an axis exactly when inversion
// is on, before a clamp to [-limit, +limit_back] and a linear smoothing, so a
// negation after it is the same as the flipped inversion with the two limits of
// that axis swapped; tests/position_tests.cpp holds the processor to that.
//
// Inputs: every distinct HeadTracking.ini a published build shipped (installer
// ZIP and Nexus ZIP; no published build seeded one through the launcher), no
// file, an empty file, core's mutation corpus over the file v0.3.0 shipped, that
// file with ToggleYawMode set to every code from 0x01 to 0xFE, and that file with
// VerticalScale away from 1. No published build wrote the file, so there is no
// first-run output: a player's file is one of the shipped ones, edited or not.

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "legacy_config/legacy_config.h"
#include "oracle/oracle_reader.h"
#include "position_boundary.h"

#include "cameraunlock/config/legacy_import.h"
#include "cameraunlock/config/testing/ini_mutations.h"
#include "cameraunlock/math/vec3.h"

namespace {

namespace fs = std::filesystem;
namespace cfg = cameraunlock::config;
namespace legacy = RVThereYetHeadTracking::legacy;
namespace testing = cameraunlock::config::testing;

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAIL: %s\n", what.c_str());
}

std::string ReadFileBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteFileBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

// ---- Scratch folders ---------------------------------------------------------
//
// One folder per reading: GetPrivateProfileString, which every reader here sits
// on, is free to cache the file it last read. `game` stands for the folder the
// DLL loads from. Every folder lives under one root for the run, removed once at
// the end.

void RemoveTree(const fs::path& root) {
    if (!fs::exists(root)) return;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    fs::remove_all(root);
}

const fs::path& ScratchRoot() {
    static const fs::path root = [] {
        wchar_t temp[MAX_PATH + 1] = {};
        if (GetTempPathW(MAX_PATH + 1, temp) == 0) throw std::runtime_error("GetTempPathW failed");
        fs::path r = fs::path(temp) / ("rvty_diff_" + std::to_string(GetCurrentProcessId()));
        RemoveTree(r);
        return r;
    }();
    return root;
}

class Scratch {
public:
    Scratch() {
        static unsigned s_next = 0;
        root_ = ScratchRoot() / std::to_string(s_next++);
        fs::create_directories(root_ / "game");
    }

    fs::path game() const { return root_ / "game"; }
    fs::path legacy() const { return game() / "HeadTracking.ini"; }
    // The folder as DllDirNarrow gave it, with its trailing backslash.
    std::string dll_dir() const { return game().string() + "\\"; }

    void WriteLegacy(const std::string& bytes) const { WriteFileBytes(legacy(), bytes); }

private:
    fs::path root_;
};

// ---- What a reading does -------------------------------------------------------
//
// A Reading is everything the running mod acts on after reading the file, and a
// Record writes it down by name: `field.*` the settings, `start.*` the state the
// session starts in, `hotkey.*` the bindings that can fire, each as
// `modifiers:code` (Ctrl 1, Shift 2, as cameraunlock::input::KeyModifiers
// numbers them) in ascending order. Floats are their bits.

struct PositionSettings {
    float sensitivity_x = 1.0f, sensitivity_y = 1.0f, sensitivity_z = 1.0f;
    float limit_x = 0, limit_y = 0, limit_y_down = 0, limit_z = 0, limit_z_back = 0;
    float local_smoothing = 0, remote_smoothing = 0;
    bool invert_x = false, invert_y = false, invert_z = false;
};

using AxisCode = void (*)(float x, float y, float z, double& surge, double& sway, double& heave);

void CurrentAxisCode(float x, float y, float z, double& surge, double& sway, double& heave) {
    rvty::position::TrackerOffsetToUE(cameraunlock::math::Vec3(x, y, z), surge, sway, heave);
}

struct Reading {
    int udp_port = 0;
    bool tracking_enabled = false;
    float yaw_sens = 1.0f, pitch_sens = 1.0f, roll_sens = 1.0f;
    bool invert_yaw = false, invert_pitch = false, invert_roll = false;
    float local_smoothing = 0, remote_smoothing = 0;
    bool world_space_yaw = false;
    bool rotation_enabled = false;
    bool position_enabled = false;
    PositionSettings position;
    AxisCode axis_code = nullptr;
    bool reticle_show = false;
    float reticle_scale = 1.0f;
    float reticle_vertical_scale = 1.0f;
    // As reticle::Configure received them; empty keeps the built-in names.
    std::vector<std::string> reticle_names;
    std::vector<rvty_oracle::Registration> hotkeys;
};

using Record = std::map<std::string, std::string>;

std::string Bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(bits));
    return text;
}

std::string Flag(bool value) { return value ? "1" : "0"; }

const char* const kActionNames[] = {"Toggle", "CycleTrackingMode", "YawMode"};

// The bindings a set of HotkeyPoller registrations can fire. The poller skips
// code 0, and GetAsyncKeyState reports no code above 0xFF or below 0 down (core's
// probe-n1); 0xFF it can.
void AddHotkeys(Record& r, const std::vector<rvty_oracle::Registration>& registrations) {
    std::map<int, std::vector<std::pair<unsigned, int>>> byAction;
    for (int action = 0; action < 3; ++action) byAction[action];
    for (const auto& [action, vk, modifiers] : registrations) {
        if (vk < 0x01 || vk > 0xFF) continue;
        byAction[action].push_back({modifiers, vk});
    }
    for (auto& [action, items] : byAction) {
        std::sort(items.begin(), items.end());
        items.erase(std::unique(items.begin(), items.end()), items.end());
        std::string text;
        for (const auto& [modifiers, vk] : items) {
            char item[32];
            std::snprintf(item, sizeof(item), "%s%u:0x%02X", text.empty() ? "" : " ", modifiers, static_cast<unsigned>(vk));
            text += item;
        }
        r[std::string("hotkey.") + kActionNames[action]] = text;
    }
}

const char* ModeName(bool rotation, bool position) {
    if (rotation && position) return "RotationAndPosition";
    if (rotation) return "RotationOnly";
    if (position) return "PositionOnly";
    return "none";
}

int Sign(double v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }

// `settings` as the processor settings that do under position_boundary.h's axis
// code what they did under `code`, and the units per metre on each axis. Each
// axis of `code` must be one axis of the current code, negated or not.
PositionSettings InCurrentFrame(PositionSettings s, AxisCode code, Record& r) {
    double unit[3][3];
    double current[3][3];
    const float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int a = 0; a < 3; ++a) {
        code(axes[a][0], axes[a][1], axes[a][2], unit[a][0], unit[a][1], unit[a][2]);
        CurrentAxisCode(axes[a][0], axes[a][1], axes[a][2], current[a][0], current[a][1], current[a][2]);
    }
    // x feeds sway (1), y heave (2), z surge (0).
    const int out_of[3] = {1, 2, 0};
    bool flip[3] = {};
    for (int a = 0; a < 3; ++a) {
        for (int o = 0; o < 3; ++o) {
            if (o == out_of[a]) continue;
            if (unit[a][o] != 0.0 || current[a][o] != 0.0) {
                throw std::logic_error("an axis code moves the camera on more than one axis for one tracker axis");
            }
        }
        const double here = unit[a][out_of[a]];
        const double now = current[a][out_of[a]];
        if (Sign(here) == 0 || Sign(now) == 0) throw std::logic_error("an axis code drops a tracker axis");
        flip[a] = Sign(here) != Sign(now);
        char units[32];
        std::snprintf(units, sizeof(units), "%.17g", here < 0 ? -here : here);
        r[std::string("field.pos.units_") + "xyz"[a]] = units;
    }
    if (flip[0]) s.invert_x = !s.invert_x;
    if (flip[1]) {
        s.invert_y = !s.invert_y;
        std::swap(s.limit_y, s.limit_y_down);
    }
    if (flip[2]) {
        s.invert_z = !s.invert_z;
        std::swap(s.limit_z, s.limit_z_back);
    }
    return s;
}

const std::vector<std::string>& BuiltInReticleNames() {
    static const std::vector<std::string> names = {"Crosshair", "LookAtObjectName"};
    return names;
}

Record Observe(const Reading& g) {
    Record r;
    r["field.udp_port"] = std::to_string(g.udp_port);
    r["field.rot.yaw_sensitivity"] = Bits(g.yaw_sens);
    r["field.rot.pitch_sensitivity"] = Bits(g.pitch_sens);
    r["field.rot.roll_sensitivity"] = Bits(g.roll_sens);
    r["field.rot.invert_yaw"] = Flag(g.invert_yaw);
    r["field.rot.invert_pitch"] = Flag(g.invert_pitch);
    r["field.rot.invert_roll"] = Flag(g.invert_roll);
    r["field.local_smoothing"] = Bits(g.local_smoothing);
    r["field.remote_smoothing"] = Bits(g.remote_smoothing);
    const PositionSettings p = InCurrentFrame(g.position, g.axis_code, r);
    r["field.pos.sensitivity_x"] = Bits(p.sensitivity_x);
    r["field.pos.sensitivity_y"] = Bits(p.sensitivity_y);
    r["field.pos.sensitivity_z"] = Bits(p.sensitivity_z);
    r["field.pos.invert_x"] = Flag(p.invert_x);
    r["field.pos.invert_y"] = Flag(p.invert_y);
    r["field.pos.invert_z"] = Flag(p.invert_z);
    r["field.pos.limit_x"] = Bits(p.limit_x);
    r["field.pos.limit_y"] = Bits(p.limit_y);
    r["field.pos.limit_y_down"] = Bits(p.limit_y_down);
    r["field.pos.limit_z"] = Bits(p.limit_z);
    r["field.pos.limit_z_back"] = Bits(p.limit_z_back);
    r["field.pos.local_smoothing"] = Bits(p.local_smoothing);
    r["field.pos.remote_smoothing"] = Bits(p.remote_smoothing);
    r["field.reticle.scale"] = Bits(g.reticle_scale);
    r["field.reticle.vertical_scale"] = Bits(g.reticle_vertical_scale);
    std::string names;
    for (const std::string& n : g.reticle_names.empty() ? BuiltInReticleNames() : g.reticle_names) {
        names += (names.empty() ? "" : ",") + n;
    }
    r["field.reticle.widgets"] = names;
    r["start.enabled"] = Flag(g.tracking_enabled);
    r["start.mode"] = ModeName(g.rotation_enabled, g.position_enabled);
    r["start.world_space_yaw"] = Flag(g.world_space_yaw);
    r["start.reticle_follows_aim"] = Flag(g.reticle_show);
    AddHotkeys(r, g.hotkeys);
    return r;
}

Reading FromOracle(const rvty_oracle::Published& o) {
    Reading g;
    g.udp_port = o.udp_port;
    g.tracking_enabled = o.tracking_enabled;
    g.yaw_sens = o.yaw_sens;
    g.pitch_sens = o.pitch_sens;
    g.roll_sens = o.roll_sens;
    g.invert_yaw = o.invert_yaw;
    g.invert_pitch = o.invert_pitch;
    g.invert_roll = o.invert_roll;
    g.local_smoothing = o.local_smoothing;
    g.remote_smoothing = o.remote_smoothing;
    g.world_space_yaw = o.world_space_yaw;
    g.rotation_enabled = o.rotation_enabled;
    g.position_enabled = o.position_enabled;
    const rvty_oracle::Position& p = o.position;
    g.position = {p.sensitivity_x, p.sensitivity_y, p.sensitivity_z, p.limit_x, p.limit_y, p.limit_y_down, p.limit_z,
                  p.limit_z_back, p.local_smoothing, p.remote_smoothing, p.invert_x, p.invert_y, p.invert_z};
    g.axis_code = &rvty_oracle::OffsetToUE;
    g.reticle_show = o.reticle_show;
    g.reticle_scale = o.reticle_scale;
    g.reticle_vertical_scale = o.reticle_vertical_scale;
    g.reticle_names = o.reticle_target_names;
    g.hotkeys = o.hotkeys;
    return g;
}

// The frozen reader's settings through the startup code of the commit that
// froze it (LoadConfig and BootstrapThread in
// src/RVThereYetHeadTracking/headtracking_mod.cpp): the globals take the
// settings, the processor the position ones with LimitY on both vertical
// bounds and the smoothing pair, the camera the axis code of
// position_boundary.h, the reticle its settings with no vertical factor,
// rotation starts on, and the hotkeys are v0.3.0's.
Reading FromImport(const legacy::Config& c) {
    Reading g;
    g.udp_port = c.udp_port;
    g.tracking_enabled = c.enable_on_startup;
    g.yaw_sens = c.yaw_sensitivity;
    g.pitch_sens = c.pitch_sensitivity;
    g.roll_sens = c.roll_sensitivity;
    g.invert_yaw = c.invert_yaw;
    g.invert_pitch = c.invert_pitch;
    g.invert_roll = c.invert_roll;
    g.local_smoothing = c.local_smoothing;
    g.remote_smoothing = c.remote_smoothing;
    g.world_space_yaw = c.world_space_yaw;
    g.rotation_enabled = true;
    g.position_enabled = c.position_enabled;
    g.position = {c.position_sensitivity_x, c.position_sensitivity_y, c.position_sensitivity_z, c.limit_x,
                  c.limit_y, c.limit_y, c.limit_z, c.limit_z_back, c.local_smoothing, c.remote_smoothing,
                  c.position_invert_x, c.position_invert_y, c.position_invert_z};
    g.axis_code = &CurrentAxisCode;
    g.reticle_show = c.show_reticle;
    g.reticle_scale = c.reticle_scale;
    g.reticle_vertical_scale = 1.0f;
    g.reticle_names = c.reticle_widget_names;
    g.hotkeys = {{rvty_oracle::kToggle, VK_END, 0},     {rvty_oracle::kToggle, 0x59, 3},
                 {rvty_oracle::kCycleMode, VK_PRIOR, 0}, {rvty_oracle::kCycleMode, 0x47, 3},
                 {rvty_oracle::kYawMode, c.yaw_mode_key, 0}, {rvty_oracle::kYawMode, 0x48, 3}};
    return g;
}

// v0.3.0's reading with the changes comparison 1 lists applied.
Reading SincePublished(Reading g) {
    // 5341862: the axis flips live in position_boundary.h.
    g.axis_code = &CurrentAxisCode;
    // 5341862: no vertical factor.
    g.reticle_vertical_scale = 1.0f;
    // 2c2cd83: LimitY both ways.
    g.position.limit_y_down = g.position.limit_y;
    return g;
}

std::vector<std::string> Differences(const Record& a, const Record& b) {
    std::vector<std::string> out;
    for (const auto& [name, value] : a) {
        const auto it = b.find(name);
        if (it == b.end()) {
            out.push_back(name + " only on the left");
        } else if (it->second != value) {
            out.push_back(name + ": " + value + " / " + it->second);
        }
    }
    for (const auto& [name, value] : b) {
        if (a.find(name) == a.end()) out.push_back(name + " only on the right");
    }
    return out;
}

// ---- Inputs --------------------------------------------------------------------

fs::path DataPath(const char* name) {
    return fs::path(RVTY_SOURCE_DIR) / "tests" / "config_differential" / "data" / name;
}

// The file v0.3.0, the newest published build, shipped.
std::string NewestShipped() { return ReadFileBytes(DataPath("shipped-v0.3.0.ini")); }

const char* const kNewestShippedName = "v0.3.0 shipped file";

// Every key the frozen reader reads, and how the corpus varies each one. The
// reader refuses a port outside 1-65535 and a float that is not finite; it
// checks nothing else.
std::vector<testing::MutationKey> CorpusKeys() {
    return {
        {"Network", "Port", "5771", {"0", "70000"}},
        {"Tracking", "EnableOnStartup", "false", {}},
        {"Tracking", "YawSensitivity", "0.5", {}},
        {"Tracking", "PitchSensitivity", "0.5", {}},
        {"Tracking", "RollSensitivity", "0.5", {}},
        {"Tracking", "InvertYaw", "true", {}},
        {"Tracking", "InvertPitch", "true", {}},
        {"Tracking", "InvertRoll", "true", {}},
        {"Tracking", "LocalSmoothing", "0.3", {}},
        {"Tracking", "RemoteSmoothing", "0.6", {}},
        {"Tracking", "WorldSpaceYaw", "false", {}},
        {"Tracking", "ShowReticle", "false", {}},
        {"Hotkeys", "ToggleYawMode", "0x2E", {}, true},
        {"Position", "Enabled", "false", {}},
        {"Position", "SensitivityX", "0.5", {}},
        {"Position", "SensitivityY", "0.5", {}},
        {"Position", "SensitivityZ", "0.5", {}},
        {"Position", "InvertX", "false", {}},
        {"Position", "InvertY", "true", {}},
        {"Position", "InvertZ", "false", {}},
        {"Position", "LimitX", "0.5", {}},
        {"Position", "LimitY", "0.35", {}},
        {"Position", "LimitZ", "0.25", {}},
        {"Position", "LimitZBack", "0.6", {}},
        {"Reticle", "Scale", "1.25", {}},
        {"Reticle", "WidgetNames", "Crosshair", {}},
    };
}

// The generator refuses the call when these and the descriptors name different
// keys, so the corpus covers every key the import reads.
std::vector<cfg::LegacyKey> CorpusReads() {
    std::vector<cfg::LegacyKey> reads;
    for (const legacy::Key& key : legacy::ReadKeys()) reads.push_back({key.section, key.key});
    return reads;
}

struct Input {
    std::string name;
    bool present;
    std::string bytes;
};

std::string Replaced(const std::string& base, const std::string& from, const std::string& to) {
    const std::size_t at = base.find(from);
    if (at == std::string::npos) throw std::logic_error("no '" + from + "' in the shipped file");
    std::string out = base;
    return out.replace(at, from.size(), to);
}

std::vector<Input> Inputs() {
    std::vector<Input> inputs = {
        {"dev, v0.1.0 and v0.2.0 shipped file", true, ReadFileBytes(DataPath("shipped-v0.1.0.ini"))},
        {kNewestShippedName, true, NewestShipped()},
        {"no file", false, {}},
        {"empty file", true, {}},
        {"VerticalScale = 1.5", true, Replaced(NewestShipped(), "VerticalScale = 1.0", "VerticalScale = 1.5")},
    };
    for (testing::IniMutation& m : testing::GenerateIniMutations(NewestShipped(), CorpusReads(), CorpusKeys())) {
        inputs.push_back({"corpus: " + m.name, true, std::move(m.bytes)});
    }
    for (int code = 0x01; code <= 0xFE; ++code) {
        char name[48];
        std::snprintf(name, sizeof(name), "ToggleYawMode = 0x%02X", static_cast<unsigned>(code));
        inputs.push_back({name, true, Replaced(NewestShipped(), "ToggleYawMode = 0x22", name)});
    }
    return inputs;
}

// ---- Comparison 1 ----------------------------------------------------------------

void Compare(const std::vector<Input>& inputs) {
    int compared = 0;
    int axis = 0, vertical = 0, limit_y = 0;
    for (const Input& input : inputs) {
        const std::string& name = input.name;
        Scratch s;
        if (input.present) s.WriteLegacy(input.bytes);
        const Reading published = FromOracle(rvty_oracle::Read(s.dll_dir()));
        legacy::Config read;
        const bool present = legacy::Load(s.legacy().string(), read);
        Check(present == input.present, name + ": the frozen reader finds the file exactly when it is there");
        const Record imported = Observe(FromImport(read));

        const std::vector<std::string> diff = Differences(Observe(SincePublished(published)), imported);
        for (const std::string& d : diff) std::printf("  comparison 1, %s: %s\n", name.c_str(), d.c_str());
        Check(diff.empty(), name + ": comparison 1, the import is v0.3.0's reading with the listed commits applied");

        // How many inputs each listed change reaches.
        Reading only_axis = published;
        only_axis.axis_code = &CurrentAxisCode;
        if (!Differences(Observe(published), Observe(only_axis)).empty()) ++axis;
        if (Bits(published.reticle_vertical_scale) != Bits(1.0f)) ++vertical;
        if (Bits(published.position.limit_y_down) != Bits(published.position.limit_y)) ++limit_y;
        ++compared;
    }
    std::printf("comparison 1: %d inputs; 5341862's axis move changes %d, its VerticalScale retirement %d, "
                "2c2cd83's downward limit %d\n",
                compared, axis, vertical, limit_y);
    Check(axis == compared, "5341862's axis move reaches every input, the shipped files and no file included");
    Check(vertical > 0 && limit_y > 0, "the inputs reach the VerticalScale retirement and the downward limit");
}

}  // namespace

int main() {
    // Unbuffered, so the lines before an uncaught exception reach the log.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Compare(Inputs());
    RemoveTree(ScratchRoot());
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
