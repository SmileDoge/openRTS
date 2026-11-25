# VR (OpenXR): the prototype

Status: a prototype (2026-11-24), not in the game.

`tools/xr-probe` (`openrts-xr-probe.exe`, Windows): a scene drawn by wgpu,
the game's way to a device, shown in a headset through OpenXR.
- OpenXR's D3D11 binding (the Meta / Oculus runtime's and SteamVR's); the
  OpenXR loader comes from the Khronos SDK's release (1.1.43), fetched at
  configure time.
- Each eye its view and projection from the runtime, the head tracked,
  1:1 scale: a floor grid 1.6 m under the start, cubes 2 m ahead (red),
  left (green), right (blue), behind (white), a sky.
- The frame goes wgpu -> the CPU -> the OpenXR swapchain's D3D11 image.

Checked with a Quest 3 through Link (2026-11-24, through SteamVR's
runtime, the system's active one): the grid and the cubes in the headset,
tracked; some 71 frames a second (Link's 72 Hz); both
eyes drawn and copied in about 4.6 ms a frame, a third of the frame's
13.9 ms. Through Steam Link (Valve's, Wi-Fi, no Meta Link): some 89
frames a second (90 Hz), 4.2 ms of 11.1 for the copy; 2064 x 2208 an eye.

The runtime: the system's active one (Meta Quest Link's settings, "OpenXR
runtime"). Another for one run (the loader ignores it in a process run as
administrator):

    set XR_RUNTIME_JSON=C:\Program Files\Oculus\Support\oculus-runtime\oculus_openxr_64.json
    openrts-xr-probe.exe [--seconds N]

## The shared texture (2026-11-24)

With openRTS's wgpu-native fork (`third_party/wgpu-native`, CMake
`OPENRTS_XR_WGPU_ROOT`): wgpu draws into a D3D11 texture shared by an NT
handle with its Vulkan device (`wgpuDeviceImportD3D11SharedTexture`); once
the GPU has drawn it (the CPU waits) D3D11 copies it into the swapchain's
image on the GPU. Through Steam Link: 1.4 ms a frame for both eyes (4.2
through the CPU), some 90 frames a second. `--cpu`: the old way.

## In the cab (2026-11-24)

`openrts_viewer --vr` (a build with `OPENRTS_WGPU_ROOT` = the fork's
package; Vulkan is forced): each eye is drawn by the game's renderer into
its own targets and shared texture, the head's pose laid over the seat's
camera (the mouse still turns it). The monitor shows the left eye and the
panels. The game's head nods and tilts are off in VR (they make sick); the
car's own sway stays. Insert, the keypad's `.` or the left controller's menu button (a thumbstick's click): the view centred again on the seat (the
headset's button centres the runtime's space, not the cab's seat).

## In the release builds (2026-11-25)

openRTS's wgpu-native fork lives at github.com/SmileDoge/wgpu-native
(branch `openrts`: the official v27.0.4.0 and the import patch); its own
GitHub Actions build and release the Windows x86_64 package (tag
`v27.0.4.0-openrts.1`). openRTS's CI takes that release instead of the
official one, so every build (nightly and versions) has VR and DLSS
(docs/experimental-effects.md); the OpenXR loader and Streamline's DLLs
come in the archive. A local build: `-DOPENRTS_WGPU_ROOT=<the fork's
package>`, no separate build folder needed.

## Next

1. The wait gone: Vulkan and D3D11 in step on the GPU (a keyed mutex or a
   shared fence), the CPU not waiting for the GPU.
2. Seated in the cab, in the game: the head as the camera, both eyes
   drawn by the scene's renderer, the keyboard and the mouse as now; the
   windows stay on the monitor.
3. The controllers on the cab's handles and buttons.
4. The windows in the world; walking.
