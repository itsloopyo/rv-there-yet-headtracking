# Changelog

## [Unreleased]

### Fixed

- The reticle `widget viewport size` line went out at frame rate whenever the
  DPI scale read back out of range, because the failed resolve left the whole
  block retrying every frame. It is now logged once per distinct size, so a
  resolution change, a windowed/fullscreen toggle or a move to another monitor
  still reports the size the reticle is actually being scaled against.
- `uninstall.cmd` left `RVThereYetHeadTracking.prev.log` behind, which
  NEXUS_MODS.md already listed as removed. It is removed now.

### Changed

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
