// The differential test for the conversion from HeadTracking.ini to
// CameraUnlock.ini.
//
// Three readings of every input:
//
//   Oracle     v0.3.0's reader and startup code, the newest published build
//              (oracle/oracle_reader.cpp; the rolling dev build is older)
//   Import     the frozen reader in src/legacy_config/, and the startup code
//              the commit that froze it ran it through
//   Migration  the config owner's Load in a folder holding only the input as
//              HeadTracking.ini, which imports it through config::Import into a
//              new CameraUnlock.ini, then this build's startup code on what the
//              session runs on
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
// Comparison 2, import against migration, is the proof for the migration: no
// difference but the approved ones, each of which the import records. Every
// sensitivity and inversion is pose shaping (pose_shaping): the mod applies the
// pose as the tracker sends it, and a value away from what v0.3.0 shipped is
// dropped. v0.3.0 shipped [Position] InvertX=true and InvertZ=true, which the
// axis code in position_boundary.h has done in their place since 5341862, so
// both are folded: the migration starts from the import's reading with every
// sensitivity at 1 and every inversion off, and with the depth limits swapped
// where InvertZ and a negative SensitivityZ between them flipped depth, so each
// limit stays the bound on the lean it bounded. ShowReticle false, a reticle
// Scale away from 1 and WidgetNames naming other widgets are dropped (reticle),
// since the game's crosshair always follows the aim over the built-in widgets
// now; a ToggleYawMode code outside 0x01-0xFE imports as unbound (N1). The
// reader turns a float that is not finite into its default, so N2 never
// applies. No default moved, so the no-file input may not differ either. A
// smoothing value outside 0-1 or a position limit outside 0-10 that the reader
// took has no approved rule: the owner cannot write it and defers the import
// (kUnrepresentable), and the session runs on what the import read.
//
// With the axis flips folded, v0.3.0's own reading of the file it shipped is
// the migration's, exactly: a player who kept that file leans as v0.3.0 had
// them lean, which is checked for both shipped files.
//
// Each input migrates three times: over a Defaults.ini the owner creates with
// the built-in values, from a read-only HeadTracking.ini, and over a
// Defaults.ini that differs from the built-in value on every global row the
// table binds. All three give the settings the import read, since the migration
// writes `default` only where the imported value is what `default` gives at that
// launch. After every load HeadTracking.ini keeps its bytes, write time and
// attributes, and the folder holds it and CameraUnlock.ini and nothing else
// (HeadTracking.ini alone after a deferred import). The distinct migrated files
// are written beside the executable under migrated\, for lint-migrated.mjs to
// run core's canonical config lint over.
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
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "config.h"
#include "legacy_config/legacy_config.h"
#include "oracle/oracle_reader.h"
#include "position_boundary.h"

#include "cameraunlock/config/canonical_ini.h"
#include "cameraunlock/config/config_owner.h"
#include "cameraunlock/config/legacy_import.h"
#include "cameraunlock/config/testing/ini_mutations.h"
#include "cameraunlock/input/key_bindings.h"
#include "cameraunlock/math/vec3.h"
#include "cameraunlock/tracking/tracking_mode.h"

namespace {

namespace fs = std::filesystem;
namespace cfg = cameraunlock::config;
namespace config = RVThereYetHeadTracking::config;
namespace legacy = RVThereYetHeadTracking::legacy;
namespace testing = cameraunlock::config::testing;
using RVThereYetHeadTracking::Config;

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
// DLL loads from; Defaults.ini sits in `global` beside it. Every folder lives
// under one root for the run and is removed once its reading is done, so the
// run never holds more than one input's files.

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
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    // A scanner can still hold a file the test just wrote, and a destructor must
    // not throw, so a folder left behind is reported and the run carries on; the
    // root goes at the end of the run.
    ~Scratch() {
        try {
            RemoveTree(root_);
        } catch (const fs::filesystem_error& e) {
            std::printf("  scratch folder left behind: %s\n", e.what());
        }
    }

    fs::path game() const { return root_ / "game"; }
    fs::path legacy() const { return game() / "HeadTracking.ini"; }
    fs::path canonical() const { return game() / "CameraUnlock.ini"; }
    fs::path defaults() const { return root_ / "global" / "Defaults.ini"; }
    // The folder as DllDirNarrow gave it, with its trailing backslash.
    std::string dll_dir() const { return game().string() + "\\"; }

    void WriteLegacy(const std::string& bytes) const { WriteFileBytes(legacy(), bytes); }

    void WriteDefaults(const std::string& bytes) const {
        fs::create_directories(defaults().parent_path());
        WriteFileBytes(defaults(), bytes);
    }

    std::set<std::string> Names() const {
        std::set<std::string> names;
        for (const auto& entry : fs::directory_iterator(game())) names.insert(entry.path().filename().string());
        return names;
    }

    cfg::ConfigOwnerOptions<Config> Options() const {
        return config::OwnerOptions(game(), cfg::DefaultsFile::At(defaults().wstring()));
    }

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

// The settings a session runs on through this build's startup code
// (LoadSettings and BootstrapThread): no rotation or position sensitivity or
// inversion, the processor's limits and smoothing pair from the file, the axis
// code of position_boundary.h, the mode from the pair, the reticle always
// following the aim over the built-in widgets at scale 1, and each hotkey list
// through ParseKeyBindings and RegisterKeyBindings.
Reading FromCanonical(const Config& c) {
    Reading g;
    g.udp_port = c.udp_port;
    g.tracking_enabled = c.enable_on_startup;
    g.local_smoothing = c.local_smoothing;
    g.remote_smoothing = c.remote_smoothing;
    g.world_space_yaw = c.world_space_yaw;
    const cameraunlock::TrackingModeChannels channels =
        cameraunlock::EncodeTrackingMode(config::StartupTrackingMode(c));
    g.rotation_enabled = channels.rotation_enabled;
    g.position_enabled = channels.position_enabled;
    g.position.limit_x = c.position_limit_x;
    g.position.limit_y = c.position_limit_y;
    g.position.limit_y_down = c.position_limit_y_down;
    g.position.limit_z = c.position_limit_z;
    g.position.limit_z_back = c.position_limit_z_back;
    g.position.local_smoothing = c.local_smoothing;
    g.position.remote_smoothing = c.remote_smoothing;
    g.axis_code = &CurrentAxisCode;
    g.reticle_show = true;
    const std::pair<int, const std::string*> lists[] = {{rvty_oracle::kToggle, &c.toggle_key},
                                                        {rvty_oracle::kCycleMode, &c.cycle_tracking_mode_key},
                                                        {rvty_oracle::kYawMode, &c.yaw_mode_key}};
    for (const auto& [action, list] : lists) {
        const cameraunlock::input::KeyBindingsParseResult parsed = cameraunlock::input::ParseKeyBindings(*list);
        Check(parsed.ok(), "the hotkey list '" + *list + "' parses");
        for (const cameraunlock::input::KeyBinding& b : parsed.bindings) {
            g.hotkeys.push_back({action, b.vk, static_cast<unsigned>(b.modifiers)});
        }
    }
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
const char* const kOlderShippedName = "dev, v0.1.0 and v0.2.0 shipped file";

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
std::vector<cfg::LegacyKey> CorpusReads() { return config::Import().keys; }

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
        {kOlderShippedName, true, ReadFileBytes(DataPath("shipped-v0.1.0.ini"))},
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

// ---- The approved differences ----------------------------------------------------
//
// What a session runs on once the import has folded or dropped a value: the
// reading the import gave, with each one's effect applied.

cfg::DropRule RuleFor(const std::string& section, const std::string& key) {
    if (section == "Reticle" || (section == "Tracking" && key == "ShowReticle")) return cfg::DropRule::Reticle;
    if (section == "Hotkeys") return cfg::DropRule::KeyCodeOutOfRange;
    return cfg::DropRule::PoseShaping;
}

const char* const kPoseShapingKeys[] = {"Tracking/YawSensitivity", "Tracking/PitchSensitivity",
                                        "Tracking/RollSensitivity", "Tracking/InvertYaw",
                                        "Tracking/InvertPitch",     "Tracking/InvertRoll",
                                        "Position/SensitivityX",    "Position/SensitivityY",
                                        "Position/SensitivityZ",    "Position/InvertX",
                                        "Position/InvertY",         "Position/InvertZ"};

Reading Expected(Reading g, const cfg::ImportResult& result, const std::string& name) {
    // Every pose-shaping setting the reader read is listed, folded or dropped.
    std::set<std::string> listed;
    for (const cfg::PoseShapingValue& v : result.pose_shaping) {
        listed.insert(v.section + "/" + v.key);
        const bool dropped = std::any_of(result.dropped.begin(), result.dropped.end(), [&](const cfg::DroppedValue& d) {
            return d.rule == cfg::DropRule::PoseShaping && d.section == v.section && d.key == v.key && d.value == v.value;
        });
        Check(v.folded != dropped, name + ": [" + v.section + "] " + v.key + "=" + v.value +
                                       " is dropped exactly when it is not what v0.3.0 shipped");
    }
    Check(listed == std::set<std::string>(std::begin(kPoseShapingKeys), std::end(kPoseShapingKeys)),
          name + ": the import lists every sensitivity and inversion as pose shaping");

    // Folded or dropped, the processor runs with none, and each depth limit
    // stays the bound on the lean it bounded.
    if (g.position.invert_z != (g.position.sensitivity_z < 0.0f)) std::swap(g.position.limit_z, g.position.limit_z_back);
    g.yaw_sens = g.pitch_sens = g.roll_sens = 1.0f;
    g.invert_yaw = g.invert_pitch = g.invert_roll = false;
    g.position.sensitivity_x = g.position.sensitivity_y = g.position.sensitivity_z = 1.0f;
    g.position.invert_x = g.position.invert_y = g.position.invert_z = false;

    for (const cfg::DroppedValue& d : result.dropped) {
        Check(d.rule == RuleFor(d.section, d.key),
              name + ": [" + d.section + "] " + d.key + " is dropped by the rule that covers it");
        if (d.section == "Tracking" && d.key == "ShowReticle") {
            g.reticle_show = true;
        } else if (d.section == "Reticle" && d.key == "Scale") {
            g.reticle_scale = 1.0f;
        } else if (d.section == "Reticle" && d.key == "WidgetNames") {
            g.reticle_names.clear();
        } else if (d.section == "Hotkeys" && d.key == "ToggleYawMode") {
            g.hotkeys.erase(std::remove_if(g.hotkeys.begin(), g.hotkeys.end(),
                                           [](const rvty_oracle::Registration& r) {
                                               return std::get<0>(r) == rvty_oracle::kYawMode && std::get<2>(r) == 0;
                                           }),
                            g.hotkeys.end());
        } else {
            Check(d.rule == cfg::DropRule::PoseShaping,
                  name + ": the import drops [" + d.section + "] " + d.key + ", which no rule here covers");
        }
    }
    return g;
}

// A smoothing value outside 0-1 or a position limit outside 0-10 that the
// reader took, which the canonical rows cannot hold. No approved rule covers
// it, so the owner defers such a file: it stays as it is, nothing is saved, the
// session runs on what the import read, and the import is tried again at every
// launch until core widens the range or the owner rules on it.
const char* const kUnrepresentable =
    "a smoothing value outside 0-1 or a position limit outside 0-10, which the canonical rows cannot hold, so the "
    "import defers";

bool Unrepresentable(const legacy::Config& c) {
    for (const float smoothing : {c.local_smoothing, c.remote_smoothing}) {
        if (smoothing < 0.0f || smoothing > 1.0f) return true;
    }
    for (const float limit : {c.limit_x, c.limit_y, c.limit_z, c.limit_z_back}) {
        if (limit < 0.0f || limit > 10.0f) return true;
    }
    return false;
}

// ---- Checks on a load ------------------------------------------------------------

// A file's bytes, last write time and attributes, which no load may change.
struct FileState {
    std::string bytes;
    unsigned long long written = 0;
    DWORD attributes = 0;
    bool operator==(const FileState& other) const {
        return bytes == other.bytes && written == other.written && attributes == other.attributes;
    }
};

std::optional<FileState> StateOf(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return std::nullopt;
        throw std::runtime_error("cannot read the attributes of " + path.string());
    }
    FileState state;
    state.bytes = ReadFileBytes(path);
    state.written = (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                    data.ftLastWriteTime.dwLowDateTime;
    state.attributes = data.dwFileAttributes;
    return state;
}

bool AsciiCrlf(const std::string& bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c > 0x7E) return false;
        if (c == '\r' && (i + 1 == bytes.size() || bytes[i + 1] != '\n')) return false;
        if (c == '\n' && (i == 0 || bytes[i - 1] != '\r')) return false;
        if (c < 0x20 && c != '\r' && c != '\n') return false;
    }
    return !bytes.empty() && bytes.back() == '\n';
}

bool LogSays(const std::vector<std::string>& log, const std::string& text) {
    return std::any_of(log.begin(), log.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
}

// What the reader and the table find in a canonical file, read over the table's
// own defaults, which stand for a Defaults.ini holding the built-in values.
std::vector<std::string> CanonicalDiagnostics(const std::string& bytes, Config& out) {
    std::vector<std::string> found;
    const cfg::CanonicalIni doc = cfg::ParseCanonicalIni(bytes);
    for (const cfg::CanonicalDiagnostic& d : doc.diagnostics) found.push_back("reader: " + cfg::DescribeCanonicalDiagnostic(d));
    const cfg::ConfigTable<Config> table = config::Table();
    out = table.defaults();
    for (const cfg::CanonicalDiagnostic& d : cfg::ApplyCanonical(doc, table, out).diagnostics) {
        found.push_back("table: " + cfg::DescribeCanonicalDiagnostic(d));
    }
    return found;
}

// A Defaults.ini holding a value other than the built-in one on every global row
// the table binds, so a migration that wrote `default` where the imported value
// is not what `default` gives would read back differently over it.
const char* const kSkewedDefaults =
    "[CameraUnlock]\r\nConfigFormat=1\r\n\r\n"
    "[Network]\r\nUdpPort=5252\r\n\r\n"
    "[General]\r\nEnableOnStartup=false\r\nWorldSpaceYaw=false\r\nRotationEnabled=false\r\n\r\n"
    "[Smoothing]\r\nLocalSmoothing=0.5\r\nRemoteSmoothing=0.5\r\n\r\n"
    "[Position]\r\nPositionEnabled=true\r\nPositionLimitX=0.5\r\nPositionLimitY=0.45\r\n"
    "PositionLimitYDown=0.35\r\nPositionLimitZ=0.6\r\nPositionLimitZBack=0.25\r\n\r\n"
    "[Hotkeys]\r\nToggleKey=F8\r\nCycleTrackingModeKey=F9\r\nYawModeKey=F10\r\n";

// The folder beside this executable the migrated files are written to, for
// lint-migrated.mjs, which CTest runs after this test.
fs::path MigratedFolder() {
    std::vector<wchar_t> exe(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (length == 0) throw std::runtime_error("cannot find this executable's path");
        if (length < exe.size()) return fs::path(std::wstring(exe.data(), length)).parent_path() / "migrated";
        exe.resize(exe.size() * 2);
    }
}

struct Tally {
    int created = 0;
    int migrated = 0;
    int deferred = 0;
    std::set<std::string> files;
};

// Runs the owner's Load in `s`, whose game folder holds the input as
// HeadTracking.ini or nothing, checks what a load must do beyond comparison 2,
// and returns the settings the session runs on.
Config Migrate(const Input& input, bool unrepresentable, const Scratch& s, const std::string& label, Tally& tally) {
    const std::optional<FileState> legacy_before = StateOf(s.legacy());
    const cfg::ConfigLoadResult<Config> loaded = cfg::ConfigOwner<Config>(s.Options()).Load();
    Check(StateOf(s.legacy()) == legacy_before, label + ": a load leaves HeadTracking.ini's bytes, write time and attributes");

    if (unrepresentable) {
        ++tally.deferred;
        Check(loaded.status == cfg::ConfigLoadStatus::Deferred,
              label + ": " + kUnrepresentable + ", not " + cfg::ConfigLoadStatusName(loaded.status));
        Check(s.Names() == std::set<std::string>{"HeadTracking.ini"}, label + ": a deferred import creates no file");
        Check(loaded.reason.find("cannot be converted") != std::string::npos,
              label + ": the player is told which value could not be converted");
        return loaded.config;
    }

    const cfg::ConfigLoadStatus want = input.present ? cfg::ConfigLoadStatus::Migrated : cfg::ConfigLoadStatus::Created;
    if (loaded.status != want) std::printf("  %s: %s, %s\n", label.c_str(), cfg::ConfigLoadStatusName(loaded.status), loaded.reason.c_str());
    Check(loaded.status == want, label + ": every legacy input imports, and no file gives a created one");
    if (loaded.status != want) return loaded.config;
    ++(input.present ? tally.migrated : tally.created);
    Check(s.Names() == (input.present ? std::set<std::string>{"CameraUnlock.ini", "HeadTracking.ini"}
                                      : std::set<std::string>{"CameraUnlock.ini"}),
          label + ": the game folder holds HeadTracking.ini and CameraUnlock.ini and nothing else");

    const std::string migrated = ReadFileBytes(s.canonical());
    Check(cfg::HasCanonicalStamp(migrated), label + ": CameraUnlock.ini carries the stamp");
    Check(AsciiCrlf(migrated), label + ": CameraUnlock.ini is ASCII with CRLF line ends");
    Config reread;
    const std::vector<std::string> diagnostics = CanonicalDiagnostics(migrated, reread);
    for (const std::string& d : diagnostics) std::printf("  %s: CameraUnlock.ini, %s\n", label.c_str(), d.c_str());
    Check(diagnostics.empty(), label + ": CameraUnlock.ini reads with no diagnostic");
    if (input.present) tally.files.insert(migrated);

    // The next start reads CameraUnlock.ini, imports nothing and writes nothing.
    const std::optional<FileState> created = StateOf(s.canonical());
    const cfg::ConfigLoadResult<Config> again = cfg::ConfigOwner<Config>(s.Options()).Load();
    Check(again.status == cfg::ConfigLoadStatus::Canonical, label + ": the next start reads CameraUnlock.ini");
    Check(Differences(Observe(FromCanonical(again.config)), Observe(FromCanonical(loaded.config))).empty(),
          label + ": the next start runs on the same settings");
    Check(StateOf(s.canonical()) == created && StateOf(s.legacy()) == legacy_before,
          label + ": the next start changes neither file");
    Check(!input.present || LogSays(again.log, "is left as it was and is not read"),
          label + ": the next start logs that HeadTracking.ini is not read");
    return loaded.config;
}

// ---- Comparisons -----------------------------------------------------------------

bool IsShippedFile(const std::string& name) {
    return name == kNewestShippedName || name == kOlderShippedName;
}

void Compare(const std::vector<Input>& inputs) {
    const std::string committed = ReadFileBytes(fs::path(RVTY_SOURCE_DIR) / "HeadTracking.ini");
    const cfg::ConfigTable<Config> table = config::Table();
    Tally builtin, readonly, skewed;
    int compared = 0;
    int axis = 0, vertical = 0, limit_y = 0;
    for (const Input& input : inputs) {
        const std::string& name = input.name;

        // Comparison 1. Both read one copy, which neither writes.
        legacy::Config read;
        Reading published;
        Reading imported;
        {
            Scratch s;
            if (input.present) s.WriteLegacy(input.bytes);
            published = FromOracle(rvty_oracle::Read(s.dll_dir()));
            const bool present = legacy::Load(s.legacy().string(), read);
            Check(present == input.present, name + ": the frozen reader finds the file exactly when it is there");
            imported = FromImport(read);
            const std::vector<std::string> diff = Differences(Observe(SincePublished(published)), Observe(imported));
            for (const std::string& d : diff) std::printf("  comparison 1, %s: %s\n", name.c_str(), d.c_str());
            Check(diff.empty(), name + ": comparison 1, the import is v0.3.0's reading with the listed commits applied");

            // How many inputs each listed change reaches.
            Reading only_axis = published;
            only_axis.axis_code = &CurrentAxisCode;
            if (!Differences(Observe(published), Observe(only_axis)).empty()) ++axis;
            if (Bits(published.reticle_vertical_scale) != Bits(1.0f)) ++vertical;
            if (Bits(published.position.limit_y_down) != Bits(published.position.limit_y)) ++limit_y;
        }
        const bool unrepresentable = Unrepresentable(read);

        // Comparison 2 over a Defaults.ini the owner creates with the built-in
        // values. The import's own result says what it folded and dropped.
        cfg::ImportResult result;
        Record want;
        {
            Scratch s;
            if (input.present) s.WriteLegacy(input.bytes);
            Config mapped = table.defaults();
            result = config::Import().run({s.legacy().wstring(), s.legacy().string(), false}, mapped);
            Check(result.status == (input.present ? cfg::ImportStatus::Imported : cfg::ImportStatus::Absent),
                  name + ": the import reads every input, as the published build did");
            want = Observe(Expected(imported, result, name));
            const Config migrated = Migrate(input, unrepresentable, s, name, builtin);
            const Record got = Observe(FromCanonical(migrated));
            const std::vector<std::string> diff = Differences(want, got);
            for (const std::string& d : diff) std::printf("  comparison 2, %s: %s\n", name.c_str(), d.c_str());
            Check(diff.empty(), name + ": comparison 2, the session runs as the import read, apart from the approved changes");

            if (!unrepresentable && fs::exists(s.canonical())) {
                // Over the built-in values the table's own defaults stand for Defaults.ini.
                Config reread;
                CanonicalDiagnostics(ReadFileBytes(s.canonical()), reread);
                Check(Differences(Observe(FromCanonical(reread)), got).empty(),
                      name + ": CameraUnlock.ini reads back as the settings the session runs on");
                // Fresh equals upgrade: every file a release shipped, and no
                // file at all, end as the committed file.
                if (IsShippedFile(name) || name == "no file") {
                    Check(ReadFileBytes(s.canonical()) == committed, name + ": gives the committed file, byte for byte");
                }
            }
            // A player who kept a shipped file leans, turns and binds keys as
            // v0.3.0 had them do.
            if (IsShippedFile(name)) {
                const std::vector<std::string> since = Differences(Observe(published), got);
                for (const std::string& d : since) std::printf("  v0.3.0 against the migration, %s: %s\n", name.c_str(), d.c_str());
                Check(since.empty(), name + ": the migration runs exactly as v0.3.0 ran on the file it shipped");
            }
        }

        if (input.present) {
            // From a read-only HeadTracking.ini, which keeps its attribute. The
            // import run on its own first leaves the folder as it was.
            Scratch s;
            s.WriteLegacy(input.bytes);
            SetFileAttributesW(s.legacy().c_str(), FILE_ATTRIBUTE_READONLY);
            const std::set<std::string> before = s.Names();
            Config unused = table.defaults();
            config::Import().run({s.legacy().wstring(), s.legacy().string(), false}, unused);
            Check(s.Names() == before && ReadFileBytes(s.legacy()) == input.bytes,
                  name + ": the import leaves a read-only folder as it was");
            const Config c = Migrate(input, unrepresentable, s, name + " (read-only)", readonly);
            Check(Differences(want, Observe(FromCanonical(c))).empty(),
                  name + ": a read-only HeadTracking.ini imports as a writable one does");
            Check((GetFileAttributesW(s.legacy().c_str()) & FILE_ATTRIBUTE_READONLY) != 0,
                  name + ": HeadTracking.ini keeps its read-only attribute");
        }

        if (input.present) {
            // Over a Defaults.ini that differs everywhere. With no legacy file
            // the settings are Defaults.ini's own, so only an input with a file
            // is held to the import here.
            Scratch s;
            s.WriteLegacy(input.bytes);
            s.WriteDefaults(kSkewedDefaults);
            const Config c = Migrate(input, unrepresentable, s, name + " (skewed Defaults.ini)", skewed);
            const std::vector<std::string> diff = Differences(want, Observe(FromCanonical(c)));
            for (const std::string& d : diff) std::printf("  comparison 2, %s (skewed Defaults.ini): %s\n", name.c_str(), d.c_str());
            Check(diff.empty(), name + ": the migration gives the import's settings over a Defaults.ini that differs everywhere");
        }
        ++compared;
    }
    std::printf("comparison 1: %d inputs; 5341862's axis move changes %d, its VerticalScale retirement %d, "
                "2c2cd83's downward limit %d\n",
                compared, axis, vertical, limit_y);
    Check(axis == compared, "5341862's axis move reaches every input, the shipped files and no file included");
    Check(vertical > 0 && limit_y > 0, "the inputs reach the VerticalScale retirement and the downward limit");
    std::printf("comparison 2 over built-in Defaults.ini: %d created, %d migrated, %d deferred (%s)\n", builtin.created,
                builtin.migrated, builtin.deferred, kUnrepresentable);
    std::printf("read-only: %d migrated, %d deferred; skewed Defaults.ini: %d migrated, %d deferred\n",
                readonly.migrated, readonly.deferred, skewed.migrated, skewed.deferred);
    Check(builtin.deferred > 0, "the corpus reaches a value the canonical rows cannot hold");

    // Core's canonical config lint runs over these next (lint-migrated.mjs).
    std::set<std::string> files = builtin.files;
    files.insert(readonly.files.begin(), readonly.files.end());
    files.insert(skewed.files.begin(), skewed.files.end());
    const fs::path lint = MigratedFolder();
    fs::remove_all(lint);
    fs::create_directories(lint);
    std::size_t n = 0;
    for (const std::string& file : files) WriteFileBytes(lint / (std::to_string(n++) + ".ini"), file);
    std::printf("%zu distinct migrated files written to %s\n", files.size(), lint.string().c_str());
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
