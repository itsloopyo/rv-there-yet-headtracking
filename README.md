# RV There Yet Head Tracking

![RV There Yet? running with this mod](https://raw.githubusercontent.com/itsloopyo/rv-there-yet-headtracking/main/assets/readme-clip.gif)

An unofficial head tracking mod for RV There Yet? that moves the view with your head while your mouse or controller keeps aiming, driven by OpenTrack over UDP, with no VR headset required.

## Features

- **Decoupled look and aim** - your head moves the camera while the mouse or
  controller keeps controlling aim, so the game still acts on where you point.
- **6DOF position tracking** - lean and peek by moving your head in space.

## Requirements

- RV There Yet?, either the [Steam build](https://store.steampowered.com/app/3949040/)
  or the Game Pass / Xbox app build. Both store versions are supported; the
  installer auto-detects whichever (or both) you have.
- An [OpenTrack](https://github.com/opentrack/opentrack)-compatible head tracker
  (VR headset, webcam, or phone app).
- Windows 10 or 11, 64-bit.

## Installation

1. Download `RVThereYetHeadTracking-vX.Y.Z-installer.zip` from the
   [Releases page](https://github.com/itsloopyo/rv-there-yet-headtracking/releases).
2. Extract it anywhere.
3. Double-click `install.cmd`.
4. Configure OpenTrack to output UDP to `127.0.0.1:4242`.
5. Launch the game.

`install.cmd` finds every installed copy of RV There Yet on your machine and
deploys to all of them. If you have both Steam and Game Pass installed, both are
mod-enabled in one pass.

If the installer cannot find your game, point it at the install root directly
with a positional argument:

```powershell
install.cmd "D:\Games\Ride"                     :: Steam-layout root
install.cmd "C:\XboxGames\RV There Yet\Content" :: Game Pass root
```

or set the `RV_THERE_YET_PATH` environment variable to your install root.

### Game Pass / Xbox App Notes

The Xbox app install is more locked down than the Steam install. Use the game's
`Content` folder as the root, normally:

```cmd
C:\XboxGames\RV There Yet\Content
```

The mod files must end up here:

```cmd
C:\XboxGames\RV There Yet\Content\Ride\Binaries\WinGDK\
```

`install.cmd` copies the mod there and also plants `dxgi_orig.dll` from your own
Windows install. If Windows denies the copy, close the Xbox app, right-click
`install.cmd`, choose **Run as administrator**, and pass the Content path if
auto-detection still fails:

```cmd
install.cmd "C:\XboxGames\RV There Yet\Content"
```

### Manual Installation

If you would rather place the files by hand, drop the mod into the binaries
folder for whichever build you have:

| Store     | Target folder |
|-----------|---------------|
| Steam     | `<steam>\steamapps\common\Ride\Ride\Binaries\Win64\` |
| Game Pass | `C:\XboxGames\RV There Yet\Content\Ride\Binaries\WinGDK\` |

You need three files in that folder:

1. `dxgi.dll` - the mod itself (from the release ZIP's `plugins/`). This is a
   DXGI proxy: every DXGI call the game makes flows through us, which is how we
   hook the camera path. The same `dxgi.dll` works for both Steam and Game Pass;
   the proxy fingerprints the running exe and selects the right RVA profile at
   load time.
2. `dxgi_orig.dll` - **a copy of your own `C:\Windows\System32\dxgi.dll`**. The
   mod's exports forward here, so the game still reaches the real DXGI through
   us. Copy it yourself, matching whichever build you have:
   ```cmd
   :: Steam
   copy C:\Windows\System32\dxgi.dll "<steam>\steamapps\common\Ride\Ride\Binaries\Win64\dxgi_orig.dll"
   :: Game Pass
   copy C:\Windows\System32\dxgi.dll "C:\XboxGames\RV There Yet\Content\Ride\Binaries\WinGDK\dxgi_orig.dll"
   ```
3. `HeadTracking.ini` - mod configuration.

The Nexus ZIP contains both `Ride\Binaries\Win64\` and `Ride\Binaries\WinGDK\`
trees in one bundle. Extract it at the package root and the files land in the
right place for whichever build you have - the unused folder is harmless. You
still need to do the `dxgi_orig.dll` copy step above. The GitHub installer ZIP's
`install.cmd` does the copy automatically and hits both builds at once if both
are installed.

For the Xbox app build, extract the Nexus ZIP into:

```cmd
C:\XboxGames\RV There Yet\Content
```

Then run this from an administrator Command Prompt:

```cmd
copy "C:\Windows\System32\dxgi.dll" "C:\XboxGames\RV There Yet\Content\Ride\Binaries\WinGDK\dxgi_orig.dll"
```

## Setting Up OpenTrack

The mod listens for OpenTrack pose data on UDP port `4242`, on every network
interface. One datagram is six little-endian 64-bit floats in the order
`x, y, z, yaw, pitch, roll`: position in centimetres, rotation in degrees, 48
bytes in total. Anything that sends that to that port drives the view.
OpenTrack's **UDP over network** output sends exactly this, and the steps below
set it up.

1. Install [OpenTrack](https://github.com/opentrack/opentrack/releases).
2. Pick a tracker under **Input**, using the notes below.
3. Set **Output** to **UDP over network**, host `127.0.0.1`, port `4242`.
4. Press **Start**. Tracking and the game can start in either order.

### Webcam

OpenTrack ships a `neuralnet tracker` input that reads a plain webcam. Select it
under **Input**, pick your camera in its settings, and use the output settings
above. How well it tracks depends on your camera and your lighting, so try it
before buying anything.

### Phone

A phone app can reach the mod directly, with no OpenTrack on the PC, if it sends
the datagram described above. Point it at this PC's IP address (run `ipconfig`
to find it) on port `4242`. Not every phone tracker speaks this protocol, so
check yours for an OpenTrack or UDP output option first. [Headcam](https://headcam.app)
sends it, and I wrote it so decent tracking is free for anyone who already owns
a phone.

Sending direct works when the app filters its own signal on the device. The
mod's smoothing is sized to take the edge off a clean signal rather than to
rescue a noisy one, so a raw feed sent direct will jitter. If it does, point the
app at OpenTrack's **UDP over network** *input* on some other port, say 5252,
and let OpenTrack's filters and curves clean it up before its output forwards to
`127.0.0.1:4242`.

Anything arriving from outside `127.0.0.0/8` counts as a remote connection and
is smoothed with `RemoteSmoothing` rather than `LocalSmoothing`. That includes a
tracker on this very PC that sends to the machine's own LAN address, because the
mod reads the source address and not the machine.

### Headset or other hardware

If your device has an OpenTrack input driver, select it under **Input** and use
the same output settings. OpenTrack's own **Input** list is the authority on
what it can read; the mod only ever sees what OpenTrack sends.

### Centring

Centring belongs to your tracker. The mod subtracts no centre of its own: it
applies the pose it receives exactly as it arrives, so a stream of zeros holds
the view where the game itself puts it. Press the centre control in your tracker
(OpenTrack's **Center** bind, or the CENTER button in Headcam) and the tracker
zeroes its own output, which leaves the view centred with the mod doing nothing.

That is why there is no centre hotkey here and nothing to re-centre in game. Two
centres in series would drift apart, because each side re-centres at moments the
other cannot see, and you would end up pressing twice to centre once. If the
view sits off to one side, centre it in the tracker.

## Controls

Two equivalent binding sets - use whichever your keyboard has:

| Action                          | Nav-cluster | Chord          |
|---------------------------------|-------------|----------------|
| Toggle tracking                 | `End`       | `Ctrl+Shift+Y` |
| Cycle tracking mode             | `Page Up`   | `Ctrl+Shift+G` |
| Toggle yaw mode (world / local) | `Page Down` | `Ctrl+Shift+H` |

`Page Up` / `Ctrl+Shift+G` cycles tracking mode:

1. Normal head-tracked gameplay
2. Positional tracking disabled, rotational tracking enabled
3. Rotational tracking disabled, positional tracking enabled
4. Back to normal

## Configuration

Settings live in `HeadTracking.ini`, next to the game executable
(`Ride\Binaries\Win64\` for Steam, `Ride\Binaries\WinGDK\` for Game Pass). Edit
it and restart the game to apply changes. If both builds are installed, each has
its own copy of the file.

On the Xbox app build, Windows may block normal saves under the game folder.
Open Notepad as administrator, then open:

```cmd
C:\XboxGames\RV There Yet\Content\Ride\Binaries\WinGDK\HeadTracking.ini
```

If you installed the game to another drive or folder, use that install's
`Content\Ride\Binaries\WinGDK\HeadTracking.ini` path instead.

```ini
[Network]
Port = 4242            ; OpenTrack UDP port

[Tracking]
; start with tracking active
EnableOnStartup = true
YawSensitivity = 1.0   ; multiplier for left/right look
PitchSensitivity = 1.0 ; multiplier for up/down look
RollSensitivity = 1.0  ; multiplier for head tilt
InvertYaw = false
InvertPitch = false
InvertRoll = false
LocalSmoothing = 0.0   ; tracker on this PC (loopback); 0.0 = responsive, 1.0 = heavy
RemoteSmoothing = 0.15 ; tracker on a network device (eg a phone over WiFi)
; move the game's reticle to the aim point
ShowReticle = true
; true = horizon-locked yaw (default), false = camera-local
WorldSpaceYaw = true

[Reticle]
Scale = 1.0            ; reticle follow strength (1.0 = geometric aim point)
; UMG widgets moved to the aim point; blank = built-in defaults
; (Crosshair, LookAtObjectName). Leave the value empty, with nothing after
; the "=" - any text there, comment included, is taken as a widget name.
WidgetNames =

[Position]
; 6DOF head position tracking
Enabled = true
SensitivityX = 1.0
SensitivityY = 1.0
SensitivityZ = 1.0
; flip a lean direction that feels reversed; the mod already orients every
; axis for this game, so all three are off
InvertX = false
InvertY = false
InvertZ = false
LimitX = 0.30          ; max sideways lean in meters
LimitY = 0.20          ; max vertical move in meters
; forward gets the generous allowance; leaning back is restricted so the view
; does not clip through the seat and player
LimitZ = 0.40
LimitZBack = 0.10
; Position uses the [Tracking] LocalSmoothing / RemoteSmoothing values.

[Hotkeys]
; Toggle tracking (End) and cycle tracking mode (Page Up) are fixed, each also
; reachable with a Ctrl+Shift chord. Only the yaw-mode toggle is rebindable
; here. VK code: PageDown = 0x22.
ToggleYawMode = 0x22
```

## Troubleshooting

**Mod not loading**

- Confirm `dxgi.dll`, `dxgi_orig.dll`, and `HeadTracking.ini` are all in the
  binaries folder for your build - `Ride\Binaries\Win64\` for Steam,
  `Ride\Binaries\WinGDK\` for Game Pass. Missing `dxgi_orig.dll` is the most
  common cause - the proxy forwards every DXGI export there, so without it the
  game crashes on launch.
- Windows may block the downloaded DLL: right-click `dxgi.dll`, Properties, then
  Unblock.
- Check the mod log next to the DLL for a `build-check: PASS - matched profile ...` line.

**No tracking response**

- Verify OpenTrack is running and its Output is sending to `127.0.0.1:4242`.
- A firewall may be blocking UDP on port 4242; allow it.

**View is off-centre**

- Centre in your tracker app: OpenTrack's Center bind, the CENTER button in a
  phone app, or your headset's own centring. The mod applies what the tracker
  sends and keeps no centre of its own, so the tracker is the only place to
  set one.

**Jittery / unstable tracking**

- Raise `RemoteSmoothing` (phone or other network tracker) or `LocalSmoothing`
  (tracker on this PC) in `[Tracking]` toward 0.3-0.5.
- On a wireless or phone tracker, expect more jitter; `RemoteSmoothing` already
  defaults to 0.15 for that case, and raising it reduces jitter further.

**Wrong rotation axis**

- If a look axis goes the wrong way, set the matching `Invert` flag (`InvertYaw`
  / `InvertPitch` / `InvertRoll`) to `true`.

**Yaw feels wrong when looking up or down at extreme angles**

- Try toggling between world-locked and camera-local yaw with `Page Down`.
  World-locked (default) is horizon-stable; camera-local follows the camera's
  current up-axis.

## Updating

Download the new release and run `install.cmd` again. Your config is preserved.

## Uninstalling

Run `uninstall.cmd`. This removes `dxgi.dll`, `dxgi_orig.dll`, and
`HeadTracking.ini` from the binaries folder of every detected install (Steam
and / or Game Pass). If you had a pre-existing `dxgi.dll` (e.g. ReShade) when
you installed the mod, the original is restored from its `.backup` copy. Pass
`/force` to discard the backup instead.

## Building from Source

Requires Visual Studio 2022 with the Desktop C++ workload and CMake.

```powershell
git clone --recursive https://github.com/itsloopyo/rv-there-yet-headtracking
cd rv-there-yet-headtracking
pixi run build
pixi run install
```

## Community & Support

- Discord: [Loop's Head Tracking Hangout](https://discord.com/invite/dxyZdyFNT9) - setup help, bug reports, and new-release announcements
- [Lopari](https://lopari.app) - free Windows launcher with one-click install and launch for the released head-tracking mods
- [Headcam](https://headcam.app) - free app that turns your iPhone or Android phone into the head tracker

## License

MIT License - see [LICENSE](LICENSE) for details.

## Credits

- [Nuggets Entertainment AB](https://store.steampowered.com/app/3949040/) for RV There Yet?.
- The [OpenTrack](https://github.com/opentrack/opentrack) contributors for the
  head-tracking UDP protocol.
- [MinHook](https://github.com/TsudaKageyu/minhook) for inline function hooking.
- The [CameraUnlock shared core](https://github.com/itsloopyo/cameraunlock-core)
  for the head-tracking processing pipeline used across all our mods.

## Disclaimer

This mod is not affiliated with, endorsed by, or supported by Nuggets
Entertainment AB. Use at your own risk.
