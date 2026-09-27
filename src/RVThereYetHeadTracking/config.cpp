#include "config.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include "legacy_config/legacy_config.h"
#include "logging.h"

#include "cameraunlock/config/value_codecs.h"
#include "cameraunlock/input/key_bindings.h"

namespace RVThereYetHeadTracking::config {

namespace {

namespace cfg = ::cameraunlock::config;
using cfg::schema::Concept;
using ::cameraunlock::input::FormatKeyBindings;
using ::cameraunlock::input::KeyModifiers;

constexpr const wchar_t* kIniName = L"CameraUnlock.ini";
constexpr const wchar_t* kLegacyIniName = L"HeadTracking.ini";

// data/games.json's display_name for rv-there-yet.
constexpr const char* kDisplayName = "RV There Yet?";

constexpr KeyModifiers kChord = KeyModifiers::kCtrl | KeyModifiers::kShift;

// The widgets reticle.cpp moves to the aim point, which an empty
// [Reticle] WidgetNames kept.
const std::vector<std::string> kBuiltInReticleWidgets = {"Crosshair", "LookAtObjectName"};

std::unique_ptr<cfg::ConfigOwner<Config>> g_owner;

struct DepthLimits {
    float forward;
    float back;
};

DepthLimits DepthLimitsOf(const legacy::Config& read) {
    const bool depth_flipped = read.position_invert_z != (read.position_sensitivity_z < 0.0f);
    return depth_flipped ? DepthLimits{read.limit_z_back, read.limit_z} : DepthLimits{read.limit_z, read.limit_z_back};
}

void Save(const char* rows, const std::function<void(Config&)>& change) {
    const cfg::ConfigSaveResult result = g_owner->Save(change);
    if (result.status != cfg::ConfigSaveStatus::Saved) {
        Log::Line("config: %s %s: %s", rows, cfg::ConfigSaveStatusName(result.status), result.reason.c_str());
    }
    for (const std::string& line : result.log) Log::Line("config: %s", line.c_str());
}

cfg::ImportResult RunImport(const cfg::LegacyInput& input, Config& out) {
    legacy::Config read;
    const bool present = legacy::Load(input.ansi_path, read);

    std::vector<cfg::DroppedValue> dropped;
    std::vector<cfg::PoseShapingValue> pose_shaping;
    const auto shaping = [&](auto value, auto shipped, const char* section, const char* key) {
        cfg::LegacyPoseShaping(value, shipped, section, key, pose_shaping, dropped);
    };
    // v0.3.0, the newest published build, shipped every sensitivity at 1 and
    // every rotation inversion false, and [Position] InvertX=true and
    // InvertZ=true with LimitZ=0.10 and LimitZBack=0.40: its axis code handed
    // the processor's offset to the camera unchanged, and the two inversions
    // mirrored sway and surge for this game. position_boundary.h has negated
    // sway and surge since 5341862, which is that shipped fold, so the mod
    // applies the pose as the tracker sends it and a value away from these is
    // dropped.
    shaping(read.yaw_sensitivity, 1.0f, "Tracking", "YawSensitivity");
    shaping(read.pitch_sensitivity, 1.0f, "Tracking", "PitchSensitivity");
    shaping(read.roll_sensitivity, 1.0f, "Tracking", "RollSensitivity");
    shaping(read.invert_yaw, false, "Tracking", "InvertYaw");
    shaping(read.invert_pitch, false, "Tracking", "InvertPitch");
    shaping(read.invert_roll, false, "Tracking", "InvertRoll");
    shaping(read.position_sensitivity_x, 1.0f, "Position", "SensitivityX");
    shaping(read.position_sensitivity_y, 1.0f, "Position", "SensitivityY");
    shaping(read.position_sensitivity_z, 1.0f, "Position", "SensitivityZ");
    shaping(read.position_invert_x, true, "Position", "InvertX");
    shaping(read.position_invert_y, false, "Position", "InvertY");
    shaping(read.position_invert_z, true, "Position", "InvertZ");

    // The reader keeps the port inside 1-65535 and every float finite, so
    // these carry over as they are. A smoothing value or limit outside the
    // canonical range has no rule: the owner cannot write it and defers the
    // import, and the session runs on it as the earlier build did.
    out.udp_port = read.udp_port;
    out.enable_on_startup = read.enable_on_startup;
    out.local_smoothing = read.local_smoothing;
    out.remote_smoothing = read.remote_smoothing;
    out.world_space_yaw = read.world_space_yaw;

    // [Position] Enabled chose the startup mode and nothing else: the cycle
    // reached every mode either way.
    const cameraunlock::TrackingModeChannels channels = cameraunlock::EncodeTrackingMode(
        read.position_enabled ? cameraunlock::TrackingMode::RotationAndPosition
                              : cameraunlock::TrackingMode::RotationOnly);
    out.rotation_enabled = channels.rotation_enabled;
    out.position_enabled = channels.position_enabled;

    // LimitY set both vertical bounds. LimitZ bounded whichever way the
    // processor's depth ran after InvertZ and a negative SensitivityZ, which
    // for the file v0.3.0 shipped was the lean back; each limit carries over
    // as the bound on the lean it bounded.
    out.position_limit_x = read.limit_x;
    out.position_limit_y = read.limit_y;
    out.position_limit_y_down = read.limit_y;
    const DepthLimits depth = DepthLimitsOf(read);
    out.position_limit_z = depth.forward;
    out.position_limit_z_back = depth.back;

    // The game's crosshair always follows the aim now, over the widgets
    // reticle.cpp names; only a file that changed that loses something.
    if (!read.show_reticle) dropped.push_back({cfg::DropRule::Reticle, "Tracking", "ShowReticle", "false"});
    if (read.reticle_scale != 1.0f) {
        dropped.push_back({cfg::DropRule::Reticle, "Reticle", "Scale", cfg::FloatCodec().Render(read.reticle_scale)});
    }
    if (!read.reticle_widget_names.empty() && read.reticle_widget_names != kBuiltInReticleWidgets) {
        std::string names;
        for (const std::string& name : read.reticle_widget_names) names += (names.empty() ? "" : ", ") + name;
        dropped.push_back({cfg::DropRule::Reticle, "Reticle", "WidgetNames", names});
    }

    // End, Page Up and the three Ctrl+Shift chords were bound in code; only
    // the yaw key was in the file, read with no range check. A code outside
    // 0x01-0xFE imports as unbound (N1), and so does one on a Ctrl, Shift or
    // Alt key alone (N3); 0 was unbound already.
    out.toggle_key = FormatKeyBindings({{KeyModifiers::kNone, VK_END}, {kChord, 'Y'}});
    out.cycle_tracking_mode_key = FormatKeyBindings({{KeyModifiers::kNone, VK_PRIOR}, {kChord, 'G'}});
    const std::string yaw_key = cfg::LegacyVirtualKeyToBindings(read.yaw_mode_key, "Hotkeys", "ToggleYawMode", dropped);
    const std::string yaw_chord = FormatKeyBindings({{kChord, 'H'}});
    out.yaw_mode_key = yaw_key.empty() ? yaw_chord : yaw_key + ", " + yaw_chord;

    // A setting the player never changed follows Defaults.ini. Every file a
    // release shipped reads as the frozen reader's defaults do, once the depth
    // limits are taken as the leans they bounded, so those defaults stand for
    // what the player was given. LimitY stood for both vertical bounds. End,
    // Page Up and the chords were bound in code, and no build had the lean
    // collision settings, so no player can have changed those.
    const legacy::Config shipped;
    const DepthLimits shipped_depth = DepthLimitsOf(shipped);
    cfg::LegacyFollowsDefaultsIni follows;
    follows.Setting(Concept::UdpPort, read.udp_port, shipped.udp_port);
    follows.Setting(Concept::EnableOnStartup, read.enable_on_startup, shipped.enable_on_startup);
    follows.Setting(Concept::WorldSpaceYaw, read.world_space_yaw, shipped.world_space_yaw);
    follows.TrackingMode(read.position_enabled, shipped.position_enabled);
    follows.Setting(Concept::LocalSmoothing, read.local_smoothing, shipped.local_smoothing);
    follows.Setting(Concept::RemoteSmoothing, read.remote_smoothing, shipped.remote_smoothing);
    follows.Setting(Concept::PositionLimitX, read.limit_x, shipped.limit_x);
    follows.Setting(Concept::PositionLimitY, read.limit_y, shipped.limit_y);
    follows.Setting(Concept::PositionLimitYDown, read.limit_y, shipped.limit_y);
    follows.Setting(Concept::PositionLimitZ, depth.forward, shipped_depth.forward);
    follows.Setting(Concept::PositionLimitZBack, depth.back, shipped_depth.back);
    follows.NotInLegacy(Concept::CollisionEnabled);
    follows.NotInLegacy(Concept::CollisionReleaseSmoothing);
    follows.NotInLegacy(Concept::ToggleKey);
    follows.NotInLegacy(Concept::CycleTrackingModeKey);
    follows.Setting(Concept::YawModeKey, read.yaw_mode_key, shipped.yaw_mode_key);

    return present ? cfg::ImportResult::Imported(std::move(dropped), std::move(pose_shaping), follows.Concepts())
                   : cfg::ImportResult::Absent(std::move(dropped), std::move(pose_shaping), follows.Concepts());
}

}  // namespace

cfg::ConfigTable<Config> Table() {
    cfg::ConfigTable<Config> table;
    table.Concept<Concept::UdpPort>(&Config::udp_port)
        .Concept<Concept::EnableOnStartup>(&Config::enable_on_startup)
        .Concept<Concept::WorldSpaceYaw>(&Config::world_space_yaw)
        .Writable()
        .Concept<Concept::RotationEnabled>(&Config::rotation_enabled)
        .Writable()
        .Concept<Concept::LocalSmoothing>(&Config::local_smoothing)
        .Concept<Concept::RemoteSmoothing>(&Config::remote_smoothing)
        .Concept<Concept::PositionEnabled>(&Config::position_enabled)
        .Writable()
        .Concept<Concept::PositionLimitX>(&Config::position_limit_x)
        .Concept<Concept::PositionLimitY>(&Config::position_limit_y)
        .Concept<Concept::PositionLimitYDown>(&Config::position_limit_y_down)
        .Concept<Concept::PositionLimitZ>(&Config::position_limit_z)
        .Concept<Concept::PositionLimitZBack>(&Config::position_limit_z_back)
        .Concept<Concept::CollisionEnabled>(&Config::collision_enabled)
        .Concept<Concept::CollisionMargin>(&Config::collision_margin)
        .Comment("Sphere sweep radius in centimetres. Raised above the camera's live near clip plane when necessary.")
        .Concept<Concept::CollisionChannel>(&Config::collision_channel)
        .Concept<Concept::CollisionReleaseSmoothing>(&Config::collision_release_smoothing)
        .Concept<Concept::ToggleKey>(&Config::toggle_key)
        .Concept<Concept::CycleTrackingModeKey>(&Config::cycle_tracking_mode_key)
        .Concept<Concept::YawModeKey>(&Config::yaw_mode_key);
    return table;
}

cfg::RenderHeader Header() {
    cfg::RenderHeader header;
    header.display_name = kDisplayName;
    return header;
}

cfg::LegacyImport<Config> Import() {
    cfg::LegacyImport<Config> import;
    import.run = &RunImport;
    for (const legacy::Key& key : legacy::ReadKeys()) import.keys.push_back({key.section, key.key});
    return import;
}

cfg::ConfigOwnerOptions<Config> OwnerOptions(const std::filesystem::path& folder, cfg::DefaultsFile defaults) {
    cfg::ConfigOwnerOptions<Config> options;
    options.path = (folder / kIniName).wstring();
    options.table = Table();
    options.import = Import();
    options.legacy_path = (folder / kLegacyIniName).wstring();
    options.header = Header();
    options.defaults = std::move(defaults);
    return options;
}

Config Load(const std::filesystem::path& folder, cfg::DefaultsFile defaults) {
    g_owner = std::make_unique<cfg::ConfigOwner<Config>>(OwnerOptions(folder, std::move(defaults)));
    const cfg::ConfigLoadResult<Config> result = g_owner->Load();
    for (const std::string& line : result.log) Log::Line("config: %s", line.c_str());
    if (!result.reason.empty()) Log::Line("config: %s", result.reason.c_str());
    Log::Line("config: %s", cfg::ConfigLoadStatusName(result.status));
    return result.config;
}

cameraunlock::TrackingMode StartupTrackingMode(const Config& config) {
    const auto mode = cameraunlock::DecodeTrackingMode(config.rotation_enabled, config.position_enabled);
    if (!mode) throw std::logic_error("RotationEnabled and PositionEnabled are both false, which the table never gives");
    return *mode;
}

void SaveWorldSpaceYaw(bool world_space_yaw) {
    Save("[General] WorldSpaceYaw", [world_space_yaw](Config& c) { c.world_space_yaw = world_space_yaw; });
}

void SaveTrackingMode(cameraunlock::TrackingMode mode) {
    const cameraunlock::TrackingModeChannels channels = cameraunlock::EncodeTrackingMode(mode);
    Save("[General] RotationEnabled and [Position] PositionEnabled", [channels](Config& c) {
        c.rotation_enabled = channels.rotation_enabled;
        c.position_enabled = channels.position_enabled;
    });
}

}  // namespace RVThereYetHeadTracking::config
