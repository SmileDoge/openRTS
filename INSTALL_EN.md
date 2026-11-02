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
  holding `rts1.exe`, `ROUTES`, `TRAINS`, `GLOBAL` and `SOUND`.

## Installing

1. **Download the archive.** On the [Releases](../../releases) page, pick:
   - **nightly** — the latest `master` build, updated by itself on every
     change;
   - **vX.Y.Z** — versions.

   Download `openRTS-windows-x64-….zip`.
2. **Unpack the whole archive** into any folder you can write to, for
   example `D:\Games\openRTS`. Do not use `Program Files`: the settings
   (`viewer_state.ini`) and the log (`openrts.log`) are kept next to the
   program. It can go beside RTS or inside its folder.
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
2. **RTS folder:** give the folder with `rts1.exe` and press Read. The
   route list should appear.
3. **What to run:** choose a route, a consist and a train path. Train
   start sets where to begin. Set the time, season and weather.
4. **Settings:**
   - language;
   - keys: RTS, with the numeric keypad (Num0, Num1, Num3), or openRTS,
     without it, for laptops (`-`, `=`, the key left of `1`);
   - sound and volume, anti-aliasing, VSync;
   - experimental lighting.
5. **Press Start.** The game opens in a window of its own while the menu
   waits; it comes back when the game is closed. Every choice is
   remembered.

## Running without the menu

The viewer can be started directly with options:

```powershell
.\openrts_viewer.exe --window --route Kuban_RTS --root D:\Games\RTS --consist "ED4M-0050 12vag"
```

The route is a folder name in `ROUTES` (then `--root` is needed) or a full
path. The consist is a `.con` name from `TRAINS\CONSISTS_RTS` or
`TRAINS\CONSISTS`. The other options (time, season, weather, sound,
graphics, mods) are listed in the [README](README_EN.md#running).

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
  - `K` — EP brake.
- **Free camera:** right mouse button + mouse, WASD, Q/E, Shift.
- **Information:**
  - F5 — cars;
  - Ctrl+F5 — speed limits;
  - F6 — stations ahead;
  - F7 — platform names;
  - Ctrl+Alt+F7 — Track control (track map, junctions, trains).
- **Lighting.** `R` switches between RTS lighting and the experimental
  one (shadows, ambient occlusion, every lamp, haze). Its settings are in
  the openRTS panel.

The full key list is in the [README](README_EN.md#controls).

## Mods

A folder or zip over the RTS installation can be mounted with
`--mount <folder or zip>` or listed in `openrts.ini`; see
[docs/vfs.md](docs/vfs.md).

## Troubleshooting

| What you see | What to do |
|---|---|
| `VCRUNTIME140.dll` / `MSVCP140.dll` not found | Install the Visual C++ Redistributable **x64** |
| "openrts_viewer.exe is not found next to the menu" | Unpack the whole archive; do not move the exe files one by one |
| "There is no ROUTES folder here" | Give the folder holding `rts1.exe` |
| No window, or a black screen | Update the graphics driver (Vulkan or DirectX 12 is needed); look for `WebGPU error` lines in `openrts.log` |
| A DLL locomotive does not respond, the cab window is empty | `openrts-dllhost32.exe` must sit next to `openrts_viewer.exe`. An antivirus may block it — add the folder to its exclusions. The reason is written to `openrts.log` |
| Low frame rate | Turn experimental lighting off (`R`), or shadows and occlusion in the openRTS panel; lower anti-aliasing |
| Settings went wrong | Delete `viewer_state.ini` next to the program to reset everything |

If that does not help, open an issue: say what you did and attach
`openrts.log`.

## Building from source

You need Visual Studio 2022 with C++, CMake 3.24+, Git and the wgpu-native
SDK; the steps are in the [README](README_EN.md#building). After the build the start
menu is `build\game\Release\openrts_game.exe`; it finds the viewer in
`build\viewer\Release` by itself.
