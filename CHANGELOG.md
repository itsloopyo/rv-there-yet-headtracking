# Changelog

## [0.3.0] - 2026-08-20

### Added

- split smoothing into local/remote and drop mod-side recentring

## [Unreleased]

- Clamp positional head tracking against world geometry, with immediate stopping and smooth release.

### Compatibility

- Restore head tracking on the updated Steam and Xbox Game Pass builds, while
  retaining support for the earlier builds.
- Find unchanged camera and reflection functions when an update moves their
  addresses. Changed or ambiguous functions leave tracking disabled.
- Ignore the texture asset named `Crosshair` when finding HUD widgets, avoiding
  invalid widget calls and repeated engine warnings.

### Added

- A setting set to `default` in `CameraUnlock.ini` takes its value from `Defaults.ini`, which every head tracking mod that keeps its settings in `CameraUnlock.ini` reads. Head tracking mods that keep their settings in another file do not read it, and neither do earlier versions of this mod. Writing a value in place of `default` changes that setting for this game only. When the mod saves a setting that a hotkey changed in game, it writes the new value in place of `default`, so that setting no longer follows `Defaults.ini` in this game until you set it to `default` again.
- `Defaults.ini` is `%AppData%\CameraUnlock\Defaults.ini` on Windows; `$XDG_CONFIG_HOME/CameraUnlock/Defaults.ini` on Linux, or `~/.config/CameraUnlock/Defaults.ini` where `XDG_CONFIG_HOME` is not set, under Wine and Proton too; and `~/Library/Application Support/CameraUnlock/Defaults.ini` on macOS. The mod's log, where it writes one, names the file it read.
- When the mod starts and finds no `Defaults.ini`, it creates one holding the built-in values, unless Windows runs the game as a packaged app. The mod never changes `Defaults.ini` after that.

### Fixed

- The reticle `widget viewport size` line went out at frame rate whenever the
  DPI scale read back out of range, because the failed resolve left the whole
  block retrying every frame. It is now logged once per distinct size, so a
  resolution change, a windowed/fullscreen toggle or a move to another monitor
  still reports the size the reticle is actually being scaled against.
- `uninstall.cmd` left `RVThereYetHeadTracking.prev.log` behind, which
  NEXUS_MODS.md already listed as removed. It is removed now.

### Changed

- Settings move to `CameraUnlock.ini`, in `Ride\Binaries\Win64\` for Steam and `Ride\Binaries\WinGDK\` for Xbox Game Pass. Earlier versions of the mod kept these settings in `HeadTracking.ini`, in the same folder. The first time this version starts and finds no `CameraUnlock.ini`, it reads your settings from `HeadTracking.ini` and writes them into `CameraUnlock.ini`. It never changes `HeadTracking.ini`, and does not read it again while `CameraUnlock.ini` exists.
- A setting that the defaults the README shows set to `default` is written as `default` when the value imported for it equals its default at that start, which is the value `Defaults.ini` gives it, or the built-in value where `Defaults.ini` gives none. It then follows `Defaults.ini`. Every other setting is written with the value imported for it.
- `RotationEnabled` and `PositionEnabled` are one setting here, the tracking mode, so both are written as `default` or neither is.
- Comments, and keys the mod never read, are not carried over. Nor are these, where your old file had them:
  - A sensitivity, scale, deadzone, response curve or axis inversion you changed from its default. Set these in your tracker instead.
  - Reticle settings, and a key that toggled the reticle.
  - The setting for a feature that earlier versions shipped switched off while it was untested. It now follows the mod's default.
- An older version of the mod reads `HeadTracking.ini` and never reads `CameraUnlock.ini`, so a setting you change after updating is not in `HeadTracking.ini`.
- Deleting only `CameraUnlock.ini` makes the next start read `HeadTracking.ini` again. To go back to the defaults, replace everything in `CameraUnlock.ini` with the defaults the README shows. Every setting they set to `default` then follows `Defaults.ini`.
- Hotkeys are written as key names, and each hotkey lists every key that triggers it, the Ctrl+Shift chord included: `ToggleKey=End, Ctrl+Shift+Y`. End, Page Up and the three chords were fixed in code and can now be changed like any other key; `[Hotkeys] ToggleYawMode`, a virtual-key code, becomes `YawModeKey`, which your old code is imported into beside `Ctrl+Shift+H`. A code outside 0x01-0xFE is imported as no key.
- A hotkey bound to a plain key no longer fires while Ctrl and Shift are both held, so Ctrl+Shift with that key reaches only a binding that names the chord (5341862).
- The tracking mode (Page Up / Ctrl+Shift+G) and the yaw mode (Page Down / Ctrl+Shift+H) are saved to `CameraUnlock.ini` when you change them, and the game starts in the mode you left it in. Turning tracking on or off (End / Ctrl+Shift+Y) is still not saved; the game starts with head tracking on or off as `EnableOnStartup` says.
- `[Position] Enabled = false` is imported as the startup tracking mode, rotation only. Page Up now steps on from the mode the game started in, so the first press from rotation only goes to position only; it used to go to rotation only again.
- The keys are renamed to the names every head tracking mod on `CameraUnlock.ini` uses: `[Network] Port` is `UdpPort`, `EnableOnStartup` and `WorldSpaceYaw` are in `[General]`, `LocalSmoothing` and `RemoteSmoothing` in `[Smoothing]`, and the lean limits are `PositionLimitX`, `PositionLimitY`, `PositionLimitYDown`, `PositionLimitZ` (forward) and `PositionLimitZBack` (back).
- The sideways and forward/back lean directions now come from the mod's code rather than from `HeadTracking.ini` (5341862). v0.3.0 and earlier shipped them as `[Position] InvertX = true` and `InvertZ = true`, with `LimitZ = 0.10` as the backward limit and `LimitZBack = 0.40` as the forward one. Those values are carried into the code, so a `HeadTracking.ini` that still holds them leans exactly as it did in v0.3.0, 0.40 m forward and 0.10 m back, and imports as the defaults. Each lean limit you set is imported as the limit on the lean it limited: with `InvertZ = true`, `LimitZBack` becomes `PositionLimitZ` and `LimitZ` becomes `PositionLimitZBack`.
- `LimitY` now limits lowering your head as well as raising it (2c2cd83), and is imported into both `PositionLimitY` and `PositionLimitYDown`. v0.3.0 kept the downward limit at 0.20 m whatever `LimitY` said.
- `[Reticle] VerticalScale` is no longer read (5341862): the reticle projection takes no vertical factor.
- A smoothing value in `HeadTracking.ini` below 0 or above 1, or a lean limit below 0 or above 10, cannot be written into `CameraUnlock.ini`. The mod then leaves `HeadTracking.ini` as it is, runs on the settings it read from it, saves nothing that session, names the value in its log, and tries again at the next start.
- The installer no longer copies a `HeadTracking.ini` in when you have none, the uninstaller no longer deletes it, and neither release ZIP nor the launcher carries a settings file. `CameraUnlock.ini` and `HeadTracking.ini` stay as they are through an install, an update and an uninstall.
- The `aim-offset` diagnostic backs off from every 2s to every 60s after ten
  lines. It answers a calibration question settled in the opening seconds, and
  at the flat rate it added about 225 KB an hour and buried the startup chain.
- The log now keeps one previous generation as `RVThereYetHeadTracking.prev.log`.
  It is still truncated per launch, so relaunching after a crash no longer
  erases the crash report the handler wrote into it.
- Recentring is gone entirely: the `Home` / `Ctrl+Shift+T` hotkey and the mod's
  own centre, rotation and position alike. Your tracker owns the centre now.
  Set it there, with OpenTrack's Center bind, the CENTER button in a phone app,
  or your headset's own centring, and the mod applies what the tracker sends.
  Two centres in series was the problem: when the view was off you could not
  tell which side was wrong, and switching trackers meant centring in both.
- Smoothing is now two keys: `[Tracking] LocalSmoothing` (default 0.0) and
  `[Tracking] RemoteSmoothing` (default 0.15), selected per connection from the
  tracker's source address. Both cover rotation and position; the old
  `[Tracking] Smoothing` and `[Position] Smoothing` keys are removed. The hidden
  0.15 baseline floor is gone, so a tracker on this machine now gets
  zero-latency tracking by default.

### Removed

- The reticle settings: `[Tracking] ShowReticle`, `[Reticle] Scale` and `[Reticle] WidgetNames`. The game's interaction reticle and the name of what you look at always follow your aim.
- The sensitivity and axis inversion settings: `[Tracking] YawSensitivity`, `PitchSensitivity`, `RollSensitivity`, `InvertYaw`, `InvertPitch` and `InvertRoll`, and `[Position] SensitivityX`, `SensitivityY`, `SensitivityZ`, `InvertX`, `InvertY` and `InvertZ`. Set these in your tracker app instead.
- With these settings at their shipped defaults the camera moves as it did before.

## [0.2.0] - 2026-08-03

### Added

- honor recenter requests from the tracker app

### Other

- Link Discord, Lopari and Headcam from the README

## [0.1.0] - 2026-07-10

### Added

- fail fast when DISCORD_RELEASE_WEBHOOK is missing
- add Lopari run link to Discord release announcements
- sync Lopari catalog pin before Discord announcement

## [0.0.0] - 2026-07-02

### Added
- Initial scaffold: shipping and packaging pipeline, launcher manifest, dxgi shim with Steam and GDK build profiles, and documentation. Head-tracking implementation not yet written.
