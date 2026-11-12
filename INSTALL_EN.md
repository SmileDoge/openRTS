# Installing and running openRTS

[Русская версия](INSTALL.md)

openRTS is an independent reimplementation of RTrainSim (RTS). It ships no
content: routes and rolling stock come from your own RTS installation, and
openRTS changes nothing in it.

## Requirements

- **Windows 10 or 11, 64-bit.**
- **A graphics card with Vulkan or DirectX 12 and a recent driver.** openRTS
  draws through WebGPU (wgpu-native), which picks Vulkan or DirectX 12 by
  itself.
- **Microsoft Visual C++ Redistributable x64.** Download
  [vc_redist.x64.exe](https://aka.ms/vs/17/release/vc_redist.x64.exe).
  Without it Windows reports `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` or
  `MSVCP140.dll` missing. The x64 version is needed; x86 will not do.
- **Your own RTrainSim installation** with routes and consists: the folder
  holding `ROUTES`, `TRAINS` (with `TRAINSET` in it), `GLOBAL` and `SOUND`.
  `rts1.exe` itself lies deeper, in its `RTS` folder.

## Installing

1. **Download the archive.** On the [Releases](../../releases) page, pick:
   - **nightly** — the latest `master` build, updated by itself on every
     change;
   - **vX.Y.Z** — versions.

   Download `openRTS-windows-x64-….zip`.
2. **Unpack the whole archive** into any folder, for example
   `D:\Games\openRTS`: beside RTS, inside its folder or in `Program Files`.
   The settings and the log are kept in `%LOCALAPPDATA%\openRTS`:
   - `viewer_state.ini` — the settings;
   - `openrts.log` — the game's log;
   - `imgui.ini` — where the windows are;
   - `debug_views` — saved views;
   - `user_paths` — Track control's paths.

   The start menu's Settings have "Open the settings and log folder". To
   keep everything next to the program (on a USB stick, say), put an empty
   file `portable.txt` beside the exe.
3. **Install the Visual C++ Redistributable x64** if it is not there yet.

What the archive holds:

| File | What for |
|---|---|
| `openrts_game.exe` | the start menu: begin here |
| `openrts_viewer.exe` | the game itself; the menu starts it |
| `wgpu_native.dll` | graphics (WebGPU) |
| `openrts-dllhost32.exe` | the 32-bit process for RTS locomotive DLLs; it must sit next to `openrts_viewer.exe` |
| `tools\` | console checks of the content (see the README) |
| `README_EN.md`, `INSTALL_EN.md` | the description and this guide |

## First start

1. **Run `openrts_game.exe`.** The interface starts in Russian; switch it
   to English under Settings (Язык / Language).
2. **RTS folder:** usually found by itself: the menu takes it from the
   Windows registry, where RTS itself looks (its installer and MDD
   Launcher write the path there), or finds the installation when openRTS
   is unpacked inside it. Otherwise give the installation's top folder, the
   one holding `ROUTES` and `TRAINS` (not the `RTS` folder with
   `rts1.exe`), and press Read; "From the registry" brings back the one the
   registry names. The route list should appear.
3. **What to run:** choose a route, a consist and a train path. Set the
   time, season and weather.
   - **Train start:** the state the engine starts in (running, switched
     on, cold).
   - **Start at:**
     - in the cab;
     - beside the car — on foot next to the cab car, as RTS starts an
       activity;
     - above the train;
     - no train — just fly over the route. The first consist you put on
       the track in Track control (Ctrl+Alt+F7) is yours.
   - **Situation:** trains saved in the game (Train window → Situations)
     stand where they stood, instead of the consist chosen. One saved
     while the menu is open shows up in the list by itself.
4. **Settings:** language; keys: RTS, with the numeric keypad (Num0, Num1,
   Num3), or openRTS, without it, for laptops (`-`, `=`, the key left of
   `1`); sound and volume; the settings and log folder; updates.

   **Graphics:** every graphics setting of the game's panel, in groups:
   Screen, Renderer, Shadows, Ambient occlusion, Lamps, Sky and water,
   World. Presets on top: Low (for weak cards, GTX 1050 / 1650 class),
   Medium, High (as openRTS starts) and Ultra. A preset sets the costly
   settings together: anti-aliasing, the shadows, the ambient occlusion,
   the lamp and cloud shadows; it leaves the look (HDR, exposure, the sky,
   the water) alone. A setting changed by hand makes it Custom. Each
   setting shows its load on the processor (CPU) and the video card (GPU):
   no effect, almost none, medium, above medium, high, very high, from grey
   to violet. That is an estimate from openRTS's own frames and players'
   reports, not a measure on your machine. In the game the preset is in
   the panel, under Graphics.
5. **Press Start.** The game opens in a window of its own while the menu
   waits; it comes back when the game is closed. Every choice is
   remembered.

## Updating

At its start the menu asks GitHub for the latest release (vX.Y.Z, not the
nightly build). When it is newer than yours, a "A new version of openRTS"
window shows the first paragraph of what is new in it, with buttons:

- **Update**: the archive is downloaded and checked (size and SHA-256),
  the game's files are replaced, and the menu starts again. The settings
  and the log live in `%LOCALAPPDATA%\openRTS` and are not touched; files
  in the game's folder that the archive does not have (`portable.txt`, for
  one) stay as well;
- **Release page**: the release in the browser;
- **Later**: asked again at the next start;
- **Skip this version**: not offered at the start any more.

While updating, the old files are renamed to `*.old` (Windows does not let
a running program be overwritten, but lets it be renamed) and deleted at
the next start. If anything fails, the old files are put back.

If the game's folder cannot be written to (`Program Files`, say), the menu
offers the release's page, or starting the menu as administrator. No update
while the game runs.

Settings → Updates: this version, "Check for a new version at the start",
and "Check now", which offers the update even for a skipped version.
Without the window: `openrts_game.exe --check-update` prints the latest
release; `--no-update-check` starts the menu without the check.

## Running without the menu

The viewer can be started directly with options:

```powershell
.\openrts_viewer.exe --window --route Kuban_RTS --root D:\Games\RTS --consist "ED4M-0050 12vag"
```

The route is a folder name in `ROUTES` (then `--root` is needed) or a full
path. The consist is a `.con` name from `TRAINS\CONSISTS_RTS` or
`TRAINS\CONSISTS`. The other options (time, season, weather, sound,
graphics, mods) are listed in the [README](README_EN.md#running).

## An own server

`openrts-server.exe` comes with the game: a server compatible with
`rts_server.exe`. openRTS (Network game) and `rts1.exe` both join it.

```powershell
.\openrts-server.exe --route Kuban_RTS --root D:\Games\RTS --time 10:00
```

The dispatcher gives the players their trains with commands in the
server's window: `add`, `assign`, `route`, `junction`, `signal`, `time`,
`pause`, `say` (`help` lists them). The port is 800; open it for players from the internet. Details:
[docs/server.md](docs/server.md).

## In the game: first steps

- **Windows.** Top left is the openRTS panel: graphics, lighting and
  sound settings. The Train window has the consist, brakes and uncoupling.
  The Cab window shows a DLL locomotive's cab.
- **The consist.** It stands at the start of the chosen path. With no
  path, `C` puts it on the track near the camera.
- **Cab.**
  - How to get in: the 3D cab view checkbox in the Train window.
  - Enter stands up at the desk; Ctrl+Enter steps out into the passage
    and outside.
  - Num0 / `-` opens the window; Num1 / `=` leans out.
  - The controls work with the mouse or the cab's keys, as in RTS.
- **Outside.** Num3 / the key left of `1` opens the car menu at the
  coupling or bogie under the cursor:
  - at a coupling: end cocks, hoses, uncoupling;
  - at a bogie: brake shoe, handbrake, release valve.
- **A train without a DLL:**
  - Home / End — throttle;
  - Page Up / Page Down — reverser;
  - `]` / `[` — driver's brake valve;
  - `.` / `,` — independent brake;
  - `K` — EP brake;
  - Space — horn, `B` — bell, `X` — sand, `P` — pantographs, `Y` / `U` —
    left / right doors.
- **Free camera:** right mouse button + mouse, WASD, Q/E, Shift.
- **Information:**
  - F5 — cars;
  - Ctrl+F5 — speed limits;
  - F6 — stations ahead;
  - F7 — platform names;
  - Ctrl+Alt+F7 — Track control (track map, junctions, trains).
- **Renderer.** `R` switches between the RTS renderer and the
  experimental one (shadows, ambient occlusion, every lamp, haze). Its settings are in
  the openRTS panel.

The full key list is in the [README](README_EN.md#controls).

## Mods

A folder or zip over the RTS installation can be mounted with
`--mount <folder or zip>` or listed in `openrts.ini`; see
[docs/vfs.md](docs/vfs.md).

## ReShade

openRTS gives ReShade the scene depth. Effects such as depth of field
(DOF), ambient occlusion (MXAO) and fog need it.

1. In the game's openRTS panel turn on "Depth for ReShade" (or start with
   `--reshade-depth on`). Use Vulkan or DirectX 12; it is not available on
   OpenGL.
2. Install ReShade on `openrts_viewer.exe`: that is the game,
   `openrts_game.exe` is only the menu. In the installer choose the same
   API as the game. Automatic usually means Vulkan; the openRTS panel shows
   it next to "Graphics API".
3. In ReShade, under "Edit global preprocessor definitions", set:

   | Setting | Value | Why |
   |---|---|---|
   | `RESHADE_DEPTH_INPUT_IS_REVERSED` | `1` | the depth is reversed; the ReShade installer writes 0, change it |
   | `RESHADE_DEPTH_MULTIPLIER` | `20` | openRTS's near plane is 5 cm |
   | `RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN` | `0` | |
   | `RESHADE_DEPTH_LINEARIZATION_FAR_PLANE` | `1000` | the distance in metres beyond which the effects treat depth as infinite; more for fog and far DOF |

4. Check with DisplayDepth: near dark, far light, the sky white.

**How it works.**
- openRTS keeps the depth until the end of the frame.
- With anti-aliasing (MSAA) it adds a single-sample copy, for a ReShade
  that cannot take a multisampled depth by itself.
- The cost is next to nothing.

**Limitation.** ReShade works on the finished frame, interface windows
included, so depth effects reach them too.

## Troubleshooting

| What you see | What to do |
|---|---|
| `VCRUNTIME140.dll` / `MSVCP140.dll` not found | Install the Visual C++ Redistributable **x64** |
| "openrts_viewer.exe is not found next to the menu" | Unpack the whole archive; do not move the exe files one by one |
| "There is no ROUTES folder here" | Give the folder holding `ROUTES` and `TRAINS`: one level above the `RTS` folder with `rts1.exe` |
| No window, or a black screen | Update the graphics driver (Vulkan or DirectX 12 is needed). In the start menu's Settings choose Graphics API → DirectX 12. Look for `WebGPU error` lines in `openrts.log` |
| A DLL locomotive does not respond, the cab window is empty | `openrts-dllhost32.exe` must sit next to `openrts_viewer.exe`. An antivirus may block it — add the folder to its exclusions. The reason is written to `openrts.log` |
| Low frame rate | Turn experimental renderer off (`R`), or shadows and occlusion in the openRTS panel; lower anti-aliasing |
| Settings went wrong | Delete `viewer_state.ini` in `%LOCALAPPDATA%\openRTS` to reset everything |

If that does not help, open an issue: say what you did and attach
`openrts.log` from `%LOCALAPPDATA%\openRTS`.

## Building from source

You need Visual Studio 2022 with C++, CMake 3.24+, Git and the wgpu-native
SDK; the steps are in the [README](README_EN.md#building). After the build the start
menu is `build\game\Release\openrts_game.exe`; it finds the viewer in
`build\viewer\Release` by itself.
