// CameraUnlock.ini on the canonical format: the committed file (HeadTracking.ini
// in the repo, the path core's data/config-format.json records) is the table's
// fresh render, a first launch creates exactly those bytes, each saved control
// changes the lines of its own rows and no other byte, End's row is not saved,
// the default hotkeys are the fleet's, and HeadTracking.ini is imported once
// and never written. tests/config_differential/ holds the import to the
// published build over the whole corpus; the cases here are the ones worth
// reading as examples.
//
// rvty_config_tests --render-config <path> writes the rendered file to <path>
// instead (pixi run render-config).

#include "config.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "cameraunlock/config/canonical_ini.h"
#include "cameraunlock/input/key_bindings.h"

namespace fs = std::filesystem;
namespace cfg = cameraunlock::config;
namespace config = RVThereYetHeadTracking::config;

using RVThereYetHeadTracking::Config;
using cameraunlock::TrackingMode;

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (condition) return;
    std::printf("FAIL: %s\n", message.c_str());
    ++g_failures;
}

std::string ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

std::string Replace(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    if (at == std::string::npos) throw std::logic_error("'" + from + "' is not in the text");
    return text.replace(at, from.size(), to);
}

bool Holds(const std::string& bytes, const std::string& line) {
    return bytes.find("\r\n" + line + "\r\n") != std::string::npos;
}

std::set<std::string> Names(const fs::path& folder) {
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(folder)) names.insert(entry.path().filename().string());
    return names;
}

std::string Rendered() { return cfg::RenderCanonicalFresh(config::Table(), config::Header()); }

fs::path TempDir() {
    wchar_t temp[MAX_PATH + 1] = {};
    if (GetTempPathW(MAX_PATH + 1, temp) == 0) throw std::runtime_error("GetTempPathW failed");
    const fs::path dir = fs::path(temp) / ("rvty_config_tests_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void RenderMatchesCommittedFile() {
    const std::string committed = ReadBytes(fs::path(RVTY_SOURCE_DIR) / "HeadTracking.ini");
    Check(committed == Rendered(),
          "HeadTracking.ini, the committed CameraUnlock.ini, is not the table's fresh render; run pixi run render-config");
    const cfg::CanonicalIni doc = cfg::ParseCanonicalIni(committed);
    Check(doc.IsReadable() && doc.diagnostics.empty(), "the committed file reads without a diagnostic");
    Config read = config::Table().defaults();
    Check(cfg::ApplyCanonical(doc, config::Table(), read).diagnostics.empty(),
          "the committed file applies without a diagnostic");
    for (const char* line :
         {"UdpPort=default", "EnableOnStartup=default", "WorldSpaceYaw=default", "RotationEnabled=default",
          "LocalSmoothing=default", "RemoteSmoothing=default", "PositionEnabled=default", "PositionLimitX=default",
          "PositionLimitY=default", "PositionLimitYDown=default", "PositionLimitZ=default",
          "PositionLimitZBack=default", "ToggleKey=default", "CycleTrackingModeKey=default", "YawModeKey=default"}) {
        Check(Holds(committed, line), std::string("the committed file holds ") + line);
    }
}

void DefaultsAreTheFleetDefaults() {
    using cameraunlock::input::KeyBinding;
    using cameraunlock::input::KeyModifiers;
    constexpr KeyModifiers kChord = KeyModifiers::kCtrl | KeyModifiers::kShift;
    const Config defaults = config::Table().defaults();
    Check(defaults.toggle_key == "End, Ctrl+Shift+Y", "ToggleKey defaults to End, Ctrl+Shift+Y");
    Check(defaults.cycle_tracking_mode_key == "PageUp, Ctrl+Shift+G",
          "CycleTrackingModeKey defaults to PageUp, Ctrl+Shift+G");
    Check(defaults.yaw_mode_key == "PageDown, Ctrl+Shift+H", "YawModeKey defaults to PageDown, Ctrl+Shift+H");
    Check(defaults.enable_on_startup, "head tracking is on at startup by default");
    Check(defaults.world_space_yaw, "yaw turns about the world's up axis by default");
    Check(config::StartupTrackingMode(defaults) == TrackingMode::RotationAndPosition,
          "the default tracking mode is rotation and position");
    Check(defaults.position_limit_z == 0.40f && defaults.position_limit_z_back == 0.10f,
          "leaning forward gets 0.40 m and leaning back 0.10 m");
    const auto yaw = cameraunlock::input::ParseKeyBindings(defaults.yaw_mode_key);
    Check(yaw.ok() && yaw.bindings.size() == 2 && yaw.bindings[0] == KeyBinding{KeyModifiers::kNone, VK_NEXT} &&
              yaw.bindings[1] == KeyBinding{kChord, 'H'},
          "YawModeKey registers Page Down and Ctrl+Shift+H");
}

// Each saved control changes the lines of its own rows, from `default` to the
// value, and no other byte; End has no row it may save; the next launch reads
// the saved values.
void SavesChangeOnlyTheirRows(const fs::path& dir) {
    const fs::path folder = dir / "saves";
    fs::create_directories(folder);
    const fs::path defaults = dir / "saves-global" / "Defaults.ini";
    const fs::path path = folder / "CameraUnlock.ini";
    const auto options = [&] { return config::OwnerOptions(folder, cfg::DefaultsFile::At(defaults.wstring())); };

    cfg::ConfigOwner<Config> owner(options());
    const cfg::ConfigLoadResult<Config> created = owner.Load();
    Check(created.status == cfg::ConfigLoadStatus::Created,
          std::string("a first launch with no legacy file creates the file, not ") +
              cfg::ConfigLoadStatusName(created.status));
    const std::string fresh = ReadBytes(path);
    Check(fresh == Rendered(), "a first launch writes the committed file's bytes");
    Check(Names(folder) == std::set<std::string>{"CameraUnlock.ini"}, "a first launch writes no legacy file");
    Check(fs::exists(defaults), "a first launch creates Defaults.ini where none exists");
    const std::string defaultsBytes = ReadBytes(defaults);

    Check(owner.Save([](Config& c) { c.world_space_yaw = false; }).status == cfg::ConfigSaveStatus::Saved,
          "the yaw toggle saves");
    const std::string afterYaw = ReadBytes(path);
    Check(afterYaw == Replace(fresh, "\r\nWorldSpaceYaw=default\r\n", "\r\nWorldSpaceYaw=false\r\n"),
          "the yaw toggle changes the WorldSpaceYaw line and no other byte");

    const cameraunlock::TrackingModeChannels positionOnly = cameraunlock::EncodeTrackingMode(TrackingMode::PositionOnly);
    Check(owner.Save([&](Config& c) {
                  c.rotation_enabled = positionOnly.rotation_enabled;
                  c.position_enabled = positionOnly.position_enabled;
              }).status == cfg::ConfigSaveStatus::Saved,
          "the mode cycle saves");
    const std::string afterMode = ReadBytes(path);
    Check(afterMode == Replace(Replace(afterYaw, "\r\nRotationEnabled=default\r\n", "\r\nRotationEnabled=false\r\n"),
                               "\r\nPositionEnabled=default\r\n", "\r\nPositionEnabled=true\r\n"),
          "the mode cycle changes the two mode lines and no other byte");

    bool refused = false;
    try {
        owner.Save([](Config& c) { c.enable_on_startup = false; });
    } catch (const std::exception&) {
        refused = true;
    }
    Check(refused, "EnableOnStartup is not a row a save may change, so End can never persist");
    Check(ReadBytes(path) == afterMode, "a refused save writes nothing");
    Check(ReadBytes(defaults) == defaultsBytes, "no save writes Defaults.ini");

    const cfg::ConfigLoadResult<Config> next = cfg::ConfigOwner<Config>(options()).Load();
    Check(next.status == cfg::ConfigLoadStatus::Canonical, "the next launch reads CameraUnlock.ini");
    Check(!next.config.world_space_yaw, "the next launch starts in camera-local yaw");
    Check(config::StartupTrackingMode(next.config) == TrackingMode::PositionOnly,
          "the next launch starts in position only");
    Check(ReadBytes(path) == afterMode, "the next launch writes nothing");
}

// The mod's own save functions write through the owner config::Load built.
void TheModsSavesWriteTheirRows(const fs::path& dir) {
    const fs::path folder = dir / "mod-saves";
    fs::create_directories(folder);
    const fs::path defaults = dir / "mod-saves-global" / "Defaults.ini";
    config::Load(folder, cfg::DefaultsFile::At(defaults.wstring()));
    const std::string fresh = ReadBytes(folder / "CameraUnlock.ini");
    config::SaveTrackingMode(TrackingMode::RotationOnly);
    config::SaveWorldSpaceYaw(false);
    Check(ReadBytes(folder / "CameraUnlock.ini") ==
              Replace(Replace(Replace(fresh, "\r\nWorldSpaceYaw=default\r\n", "\r\nWorldSpaceYaw=false\r\n"),
                              "\r\nRotationEnabled=default\r\n", "\r\nRotationEnabled=true\r\n"),
                      "\r\nPositionEnabled=default\r\n", "\r\nPositionEnabled=false\r\n"),
          "SaveTrackingMode and SaveWorldSpaceYaw write their rows and nothing else");
    const Config reloaded = config::Load(folder, cfg::DefaultsFile::At(defaults.wstring()));
    Check(config::StartupTrackingMode(reloaded) == TrackingMode::RotationOnly && !reloaded.world_space_yaw,
          "the saved mode and yaw come back at the next launch");
}

// A row holding default takes Defaults.ini's value.
void DefaultRowsFollowDefaultsIni(const fs::path& dir) {
    const fs::path folder = dir / "follow";
    fs::create_directories(folder);
    const fs::path defaults = dir / "follow-global" / "Defaults.ini";
    fs::create_directories(defaults.parent_path());
    WriteBytes(defaults,
               "[CameraUnlock]\r\nConfigFormat=1\r\n\r\n[General]\r\nWorldSpaceYaw=false\r\n\r\n"
               "[Position]\r\nPositionLimitZ=0.25\r\n\r\n[Hotkeys]\r\nToggleKey=F8\r\n");
    WriteBytes(folder / "CameraUnlock.ini", Rendered());
    const cfg::ConfigLoadResult<Config> loaded =
        cfg::ConfigOwner<Config>(config::OwnerOptions(folder, cfg::DefaultsFile::At(defaults.wstring()))).Load();
    Check(loaded.status == cfg::ConfigLoadStatus::Canonical, "the committed file loads as canonical");
    Check(!loaded.config.world_space_yaw, "WorldSpaceYaw=default follows Defaults.ini");
    Check(loaded.config.position_limit_z == 0.25f, "PositionLimitZ=default follows Defaults.ini");
    Check(loaded.config.toggle_key == "F8", "ToggleKey=default follows Defaults.ini");
}

// The file v0.3.0 shipped is imported into a new CameraUnlock.ini that holds
// default on every row, since each of its values is the built-in one once the
// shipped axis flips are folded into the code, and HeadTracking.ini keeps its
// bytes. Once CameraUnlock.ini exists HeadTracking.ini is not read again.
void TheShippedFileImportsAsTheDefaults(const fs::path& dir) {
    const fs::path folder = dir / "import";
    fs::create_directories(folder);
    const fs::path defaults = dir / "import-global" / "Defaults.ini";
    const std::string shipped =
        ReadBytes(fs::path(RVTY_SOURCE_DIR) / "tests" / "config_differential" / "data" / "shipped-v0.3.0.ini");
    WriteBytes(folder / "HeadTracking.ini", shipped);
    const auto options = [&] { return config::OwnerOptions(folder, cfg::DefaultsFile::At(defaults.wstring())); };

    const cfg::ConfigLoadResult<Config> loaded = cfg::ConfigOwner<Config>(options()).Load();
    Check(loaded.status == cfg::ConfigLoadStatus::Migrated, "v0.3.0's file imports");
    Check(ReadBytes(folder / "CameraUnlock.ini") == Rendered(), "v0.3.0's file imports as the committed file");
    Check(loaded.config.position_limit_z == 0.40f && loaded.config.position_limit_z_back == 0.10f,
          "v0.3.0's LimitZBack=0.40 is the forward budget and its LimitZ=0.10 the backward one");
    Check(ReadBytes(folder / "HeadTracking.ini") == shipped, "HeadTracking.ini keeps its bytes");
    Check(Names(folder) == (std::set<std::string>{"CameraUnlock.ini", "HeadTracking.ini"}),
          "the import creates CameraUnlock.ini and nothing else");

    WriteBytes(folder / "HeadTracking.ini", Replace(shipped, "WorldSpaceYaw = true", "WorldSpaceYaw = false"));
    const cfg::ConfigLoadResult<Config> again = cfg::ConfigOwner<Config>(options()).Load();
    Check(again.status == cfg::ConfigLoadStatus::Canonical && again.config.world_space_yaw,
          "the next launch reads CameraUnlock.ini, not HeadTracking.ini");
}

// A player's own values carry over, the retired settings are dropped, and the
// yaw key joins its list beside the chord.
void APlayersFileCarriesOver(const fs::path& dir) {
    const fs::path folder = dir / "player";
    fs::create_directories(folder);
    const fs::path defaults = dir / "player-global" / "Defaults.ini";
    WriteBytes(folder / "HeadTracking.ini",
               "[Network]\r\nPort = 5555\r\n[Tracking]\r\nRemoteSmoothing = 0.4\r\nShowReticle = false\r\n"
               "YawSensitivity = 2.0\r\n[Position]\r\nEnabled = false\r\nInvertX = true\r\nInvertZ = true\r\n"
               "LimitY = 0.3\r\nLimitZ = 0.05\r\nLimitZBack = 0.5\r\n[Reticle]\r\nScale = 1.2\r\n"
               "[Hotkeys]\r\nToggleYawMode = 0x2E\r\n");
    const cfg::ConfigLoadResult<Config> loaded =
        cfg::ConfigOwner<Config>(config::OwnerOptions(folder, cfg::DefaultsFile::At(defaults.wstring()))).Load();
    Check(loaded.status == cfg::ConfigLoadStatus::Migrated, "a player's file imports");
    const std::string migrated = ReadBytes(folder / "CameraUnlock.ini");
    for (const char* line :
         {"UdpPort=5555", "RemoteSmoothing=0.4", "RotationEnabled=true", "PositionEnabled=false",
          "PositionLimitY=0.3", "PositionLimitYDown=0.3", "PositionLimitZ=0.5", "PositionLimitZBack=0.05",
          "YawModeKey=Delete, Ctrl+Shift+H", "LocalSmoothing=default", "PositionLimitX=default",
          "ToggleKey=default"}) {
        Check(Holds(migrated, line), std::string("the migrated file holds ") + line);
    }
    for (const char* text : {"not carried: [Tracking] ShowReticle=false", "not carried: [Reticle] Scale=1.2",
                             "not carried: [Tracking] YawSensitivity=2.0"}) {
        bool logged = false;
        for (const std::string& line : loaded.log) logged = logged || line.find(text) != std::string::npos;
        Check(logged, std::string("the log says ") + text);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3 && std::string(argv[1]) == "--render-config") {
        WriteBytes(argv[2], Rendered());
        std::printf("wrote %s\n", argv[2]);
        return 0;
    }
    if (argc != 1) {
        std::printf("usage: rvty_config_tests [--render-config <path>]\n");
        return 2;
    }
    const fs::path dir = TempDir();
    RenderMatchesCommittedFile();
    DefaultsAreTheFleetDefaults();
    SavesChangeOnlyTheirRows(dir);
    TheModsSavesWriteTheirRows(dir);
    DefaultRowsFollowDefaultsIni(dir);
    TheShippedFileImportsAsTheDefaults(dir);
    APlayersFileCarriesOver(dir);
    fs::remove_all(dir);
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all config checks passed\n");
    return 0;
}
