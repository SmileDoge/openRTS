# Profiling with Tracy

openRTS can be built with the client of [Tracy](https://github.com/wolfpld/tracy)
(BSD-3), a frame profiler in the spirit of Source's VProf, Unity's Profiler
and Unreal Insights: every marked zone on every thread on one timeline,
frame by frame, with statistics, plots and the frames that stuttered. The
client is an optional dependency, off by default: without it the zones
compile to nothing and no Tracy code is built or linked. Released builds do
not have it.

## Building

```powershell
cmake -S . -B build-tracy -DOPENRTS_ENABLE_WGPU=ON -DOPENRTS_WGPU_ROOT=C:\sdk\wgpu-native -DOPENRTS_ENABLE_TRACY=ON
cmake --build build-tracy --config Release
```

A build directory of its own keeps the usual one as it is (the option can
be switched in an existing build too, which rebuilds everything). CMake
fetches the client, Tracy v0.14.1, by itself.

The client runs on demand: it keeps nothing until the profiler connects,
and it listens on localhost (port 8086) only. Not connected, a Tracy build
runs as fast as the usual one (the station with nine trains: every stage
within the run-to-run noise).

## The profiler

The profiler's version has to be the client's: Tracy 0.14.1,
`windows-0.14.1.zip` from the
[release page](https://github.com/wolfpld/tracy/releases/tag/v0.14.1).

- `tracy-profiler.exe`: the window. Start the game, then connect to
  `127.0.0.1` (it is also listed among the clients it finds). Statistics
  (the zones' totals, mean, max), Find zone (one zone's times as a
  histogram), the frame graph at the top (each bar a frame: click a slow one
  to see it on the timeline).
- `tracy-capture.exe -o run.tracy -s 30`: records 30 s without a window
  (it waits for the game to start), for the profiler to open later.
- `tracy-csvexport.exe run.tracy`: the zones' statistics as CSV; `-u` lists
  every zone with its start time, length and thread.

Connected, the frame is a little slower (a few tenths of a millisecond on
the station): compare where the time goes, not the hundredths.

## What is marked

| Zone | Where |
|---|---|
| `update`, `panel`, `wait`, `begin`, `backgr`, `cull`, `prepare`, `effects`, `models`, `foregr`, `end`, `post`, `encode`, `join`, `submit` | the frame's stages, the names of the panel's stage list; `FPS limit` between frames |
| `scene: update`, `scene: submit` | the route scene each frame |
| `trains: update`, `trains: physics step`, `trains: DLL host step`, `trains: sound`, `trains: submit` | trains: their update, the simulation step, the step of the locomotive DLLs in the 32-bit host (waiting for it included), sounds, models given to the renderer |
| `trains: DLL world feed`; `DLL: request`, `DLL: write the request`, `DLL: waiting for the host`, `DLL: read the reply` | the DLL step's parts: the world for the DLLs, the request built and written to the shared memory, the wait for the 32-bit host (its work and two thread switches), the reply |
| `signals`, `network`, `audio`, `route streaming` | those subsystems' updates |
| `models: prepare` with `models: primitives`, `models: batches`, `models: waiting for the depth draws`, `models: instances`, `models: uploads`; `models: depth draws` | the renderer's batching and uploads; the depth passes' batching runs on a worker |
| `models: main pass`, `models: depth pass`, `terrain: main pass`, `terrain: depth pass` | recording the passes |
| `effects: pass` (the text says which: `sun cascade 1`..`4`, `cab focus`, `lamp shadow`, `camera depth, GTAO, clusters`), `effects: begin frame`, `effects: focus and lamps` | the experimental renderer: its passes on the workers (in each, `effects: record` and `effects: finish`, wgpu's `wgpuCommandEncoderFinish`), its per-frame setup |

The workers are named `worker 1`, `worker 2`... Plots: `draw calls`,
`draw commands`, `model instances`. The frame ends after `submit`.

Each frame stage's zone opens under the stage that came next the frame
before (Tracy groups its statistics by the name a zone opened with) and is
renamed in the timeline when it ended as another: only the first frame
does.

## Adding zones

```cpp
#include <openrts/core/profile.hpp>

void TrainView::step_trains() {
    OPENRTS_ZONE("trains: physics step");
    ...
}
```

`OPENRTS_ZONE("area: what")` marks to the end of the scope;
`OPENRTS_ZONE_TEXT(text, size)` adds a detail to the zone (which pass, which
train); `OPENRTS_THREAD_NAME`, `OPENRTS_PLOT`, `OPENRTS_FRAME` are the rest
(`include/openrts/core/profile.hpp`). Names are string literals. Every
library linking `openrts_core` has them. A zone costs some 20 ns with a
profiler connected: per frame, per pass, per train is fine; per instance,
per triangle is not.

## The first look (2026-11-07)

The station from the multi-draw notes (nine trains, the experimental
renderer, 30 fps), the last 8 s of a run, ms per frame on the render thread:

| Zone | ms |
|---|---|
| `update` | 2.98 |
| of it `trains: DLL host step` | 1.52 (2.3 ms each step, two frames of three) |
| `prepare` | 1.82 |
| of it `models: batches` | 1.23 |
| `encode` | 1.36 |
| `cull` | 1.13 |
| of it `trains: submit` | 0.65 |
| `models` | 0.42 |

On the workers: the passes 3.7 ms per frame in all, `models: depth draws`
1.3. The step of the locomotive DLLs waits for the 32-bit host on the
render thread: the largest single part of the frame, not seen before (the
`update` stage had it all).

**`encode` waits for no one.** wgpu checks and encodes a pass's commands in
`wgpuCommandEncoderFinish`, not as they are recorded. With the parallel
recording off (every pass in the main command buffer) `encode` is 3.1 ms:
the main pass's 1.2 and some 1.9 of the shadows' and the camera's passes.
With it on, the workers' `effects: finish` (2.1 ms per frame, 1.5 of it
while `encode` runs) and `encode` (1.28) add up to the same within 0.2 ms:
they hardly slow each other. Finishing costs 1.4 times the recording for
the depth passes, 3.3 times for the main pass (its many bind group
changes): fewer commands is the way to a shorter `encode`. The camera's
passes, the longest (recorded in 0.73 ms, finished in 1.08), are queued
first since: the workers end 0.3 ms before `encode` does (median), `join`
0.10 -> 0.02 ms.

**Batching in flat arrays** (2026-11-08). `models: batches` built some 1800
groups each frame, each with a vector of its own, in an `unordered_map`, and
the depth draws' batching on the worker did the same. Now both take flat
arrays kept between frames: a group table (open addressing, emptied by a
new generation), per-item group ids, the groups' instances placed by
counting; a frame that sees no more groups than before allocates nothing.
The order is the same: groups by pipeline and model, else as they came;
instances in item order; blended primitives in their own order (a check
running the old code beside the new one found no difference in 600
frames). Two traces each, the last 8 s:

| ms per frame | before | after |
|---|---|---|
| all CPU work (without `wait`) | 7.53, 7.81 | 6.76, 6.37 |
| `prepare` | 1.59, 1.62 | 0.77, 0.64 |
| `models: batches` | 1.19 | 0.36, 0.35 |
| `models: depth draws` (worker) | 1.32 | 0.19, 0.19 |

The frames are the same (`--paused`, captured at 45 s once the station has
loaded): Vulkan and OpenGL on the RTS path byte for byte, the experimental
path on Vulkan and DirectX 12 and the RTS path on DirectX 12 within 2
pixels (as two runs of one build).

**The DLL step** (2026-11-08). 20 steps a second, one per physics step
(`train_step` 0.05 s): the 1.5 ms per frame above is 2.1 ms a step at 30
fps (two frames of three have one); at 145 fps it is 0.3 ms per frame on
average, 2.1 ms on every seventh frame. A step's 2.1 ms (the ED9M, 8
cars): our side 0.05 (the request, the world, the reply), the rest waiting
for the host. Inside the host (timed there for a test, not kept): 1.4 ms
handling the step, 1.3 of it in the cars' Run; of that `ED9M.dll` 0.03 ms
for its 8 calls, `Usavp.dll` and `RTSConnector.dll` 0.07, `Brakes.dll` 1.2,
and of `Brakes.dll`'s time 1.2 ms is in our `RTSGetObjectByName`: 676
calls a step (it looks its parameters up by name every Run), each a linear
scan with `lstrcmpiW`, some 1.8 us a call. The other 0.7 ms of the wait is
the host waking and the reply's thread switch.

**By-name answers kept** (2026-11-08, docs/train-dll.md "As built"). The
host keeps the object a name resolved to; the DLLs and the exchange are
as they were. With `OPENRTS_DLLHOST_NAME_CHECK` on the station's ED9M,
853 759 kept answers scanned again, none differed; 309 entries, 91 names
asked by several owners (up to 8, a car each), each car its own object.
Lookups: 0.78 ms the first step after emptying (317 misses, 564 hits),
then 0.06 ms a step (some 712 hits, no misses). Two traces each, the last
30 s (900 frames, 30 fps):

| ms | before | after |
|---|---|---|
| a DLL step, mean | 2.14, 2.18 | 1.01, 0.97 |
| a DLL step, P95 / P99 | 2.52-2.60 / 2.64-2.84 | 1.24-1.27 / 1.58-1.64 |
| CPU work a frame, mean | 6.38, 6.38 | 5.52, 5.45 |
| CPU work a frame, P95 / P99 | 8.1-8.2 / 9.1-11.2 | 6.69 / 8.1-8.4 |
| frames with a step / without | 7.20 / 4.72 | 5.92 / 4.62 |
| the frame (30 fps limit), P95 / P99 | 36.3-36.5 / 37.2-39.0 | 35.0-35.2 / 35.7-36.2 |

CPU work is the frame without `wait` and the FPS limit's sleep (with the
limit, the frame itself is 33.3 ms). Single frames of 27-42 ms (a load)
are in both. What is left of a step: the host's own 0.25 ms and 0.7 ms of
waking it and switching back.
