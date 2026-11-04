# Virtual file system

Status: implemented (see "Implementation" at the end). Goal: openRTS reads content the same way
whether it lies in ordinary folders (as RTS/MSTS install it today) or in
archives, and mods can replace files without touching the originals.

## Principles

- **Folders keep working.** An existing installation (`ROUTES`, `TRAINS`,
  `GLOBAL`, `SOUND`, ...) is used as is. Archives are optional. RTS itself
  keeps using the folders.
- **One namespace.** Every file has a virtual path relative to the
  installation root, e.g. `ROUTES/Kuban_RTS/SHAPES/a.s`. Folders and archives
  only provide files for that namespace.
- **RTS path rules.** Lookup is case-insensitive. `\` and `/` are equal. `..`
  is allowed and is resolved lexically before lookup, so it also works across
  archives. Absolute paths (`C:\...`, `/...`) are refused.
- **Read-only content.** Saves, logs, settings and caches go to normal
  folders, never into the VFS.

## Sources and order

A source is a folder or a ZIP archive. All sources are mounted at the
installation root. Later sources override earlier ones for the same path.

From lowest to highest priority:

| Layer | Source | Where | Purpose |
|---|---|---|---|
| 1 | base packs | `PACKS/base/*.zip` | routes, trains, GLOBAL shipped as archives |
| 2 | install folders | `ROUTES`, `TRAINS`, `GLOBAL`, `SOUND`, ... | classic installation, local edits |
| 3 | patch packs | `PACKS/patch/*.zip` | updates and fixes to base packs or folders |
| 4 | mods | `MODS_OPENRTS/<mod>/` or `MODS_OPENRTS/<mod>.zip`, in load order | replacements and additions |

- Mods win over everything. A patch replaces files of a base pack and also of
  a folder install, so fixes can be shipped for routes that users installed as
  folders. Folders win over base packs, so a locally edited file overrides
  the archived original.
- Inside one layer, packs are ordered by `Priority` from their manifest, then
  by file name.
- The mod load order is a plain list in `openrts.ini` (`[Mods]` section). A
  mod not in the list is not mounted. Later entries win.
- `RTS/MODS` is RTS's own plugin folder (DLLs) and is unrelated; openRTS mods
  live in `MODS_OPENRTS` to avoid confusion.

## Index

At start-up the VFS reads the folder trees and the central directory of every
ZIP. It builds one table:

```text
normalised path (lower case, '/')  ->  { source, location in source, size, compression }
```

plus the set of directories for listings.

- **Lookup** is one hash-table probe, independent of the number of archives.
- **Listing** a folder (`ROUTES/X/WORLD/`, `TRAINS/TRAINSET/`) comes from the
  same table and merges all sources.
- **Size:** about 160 000 files in the current installation. At roughly
  100 bytes per entry that is about 16 MB, built in well under a second from
  ZIP directories. Folder scanning dominates the start-up time; it can be
  cached and rebuilt when a source's modification time changes.
- **Conflicts** (the same path from several sources) are recorded, so a tool
  can show which mod replaced what.
- **Snapshots.** The index is immutable once built. A change builds a new
  index generation off-thread and publishes it with one atomic pointer swap
  (`std::atomic<std::shared_ptr<const Index>>`). A reader takes a snapshot and
  keeps it for the whole job, e.g. loading one model with all its textures.
  It never sees a half-updated index, and an old generation is freed when its
  last reader drops it.

## Relative references across sources

A file's references (texture names in a `.s`, `.sd`, shapes in a `.wag`,
`../` aliases between trainsets) are resolved against the **virtual**
directory of the referring file. Its real location is irrelevant.

```text
PACKS/base/VL80k.zip : TRAINS/TRAINSET/VL80k/VL80k.s
    texture "../VL80_common/TEXTURES/body.ace"
-> virtual   TRAINS/TRAINSET/VL80_common/TEXTURES/body.ace
-> provided by the folder TRAINS/TRAINSET/VL80_common/ on disk (or another
   pack, or a mod - whichever layer wins for that path)
```

- **Lexical resolution.** `..` is resolved on the virtual path before lookup,
  as Windows/RTS does. Intermediate folders need not exist in any source, so
  a reference may climb out of an archive into a folder and back into
  another archive.
- **Source-agnostic loaders.** A loader gets the virtual path of the file it
  reads (`VfsFile::path()`). Relative names are joined to its directory,
  never to an archive member name or a disk path.
- **Search fallbacks** (route `TEXTURES`, then `GLOBAL/TEXTURES`, seasonal
  folders) also run on virtual paths, so each candidate can come from any
  layer.
- **Climbing above the root.** `..` above the installation root is clamped to
  the root (RTS cannot leave it either). Such cases are logged.
- **Tests:** a model in a pack whose texture is in a folder; a model in a
  folder whose texture is in a pack; a mod overriding only the texture; mixed
  case and `\` in the reference.

## Archives

- **Format:** ZIP (with ZIP64 for archives over 4 GB). Any archiver can open
  it, and single files are read by seeking to them.
- **Paths inside an archive** are relative to the installation root, exactly
  as in a folder install:

  ```text
  Kuban_RTS.zip
    openrts-pack.ini
    ROUTES/Kuban_RTS/Kuban_RTS.trk
    ROUTES/Kuban_RTS/WORLD/...
    ROUTES/Kuban_RTS/TEXTURES/...
  ```

  An archive may contain any mix of routes, trains and GLOBAL files.
- **Compression** is chosen per file. Files that are already compressed
  (`SIMISA@F` shapes and textures, `.wav`/`.ogg`, `.dds`) are stored. Text
  formats (`.w`, `.eng`, `.wag`, `.sms`, `.dat`, `.trk`, `.tdb`) are
  compressed. Reading a stored file costs the same as reading a loose one.
- **Two compression methods** are supported for reading and writing:
  - **deflate** (ZIP method 8): opens in any archiver; the default for
    distribution.
  - **Zstandard** (ZIP method 93, standard since APPNOTE 6.3.7): several
    times faster to decompress at a similar ratio. 7-Zip opens it only with
    the zstd plugin (7-Zip ZS) and Windows Explorer does not, so it suits
    installed packs better than downloads.
  - One archive may mix methods. Zstandard is fetched like zlib
    (`OPENRTS_FETCH_ZSTD`).
- **Manifest** `openrts-pack.ini` (optional; defaults apply without it):

  ```ini
  [Pack]
  Name = Kuban_RTS
  Version = 2025.01
  Kind = route          ; route | trains | global | mixed
  Priority = 100
  Requires = GLOBAL-base, VL80k
  ```

## Adding content

### Route

Either way the route appears in the route list:

1. **Folder (as today):** unpack into `ROUTES/<Name>/`.
2. **Pack:** put `<Name>.zip` into `PACKS/base/` (only `PACKS/base` and
   `PACKS/patch` are mounted; a ZIP directly in `PACKS/` is ignored).

The route list is every `ROUTES/<dir>/` in the VFS that contains a `.trk`.
Global track shapes the route needs come from `GLOBAL` (folder or a GLOBAL
pack). A big route can be split into several packs (`Kuban_RTS.zip`,
`Kuban_RTS.terrain.zip`). The VFS merges them because the paths do not
overlap.

### Train

1. **Folder:** `TRAINS/TRAINSET/<Name>/` plus consists in
   `TRAINS/CONSISTS_RTS/` or `TRAINS/CONSISTS/`.
2. **Pack:** `PACKS/base/<Name>.zip` containing the same paths.

The rolling-stock list is every `TRAINS/TRAINSET/<dir>/` with `.eng`/`.wag`;
the consist list comes from the `CONSISTS*` folders. Alias folders shared
through `../` resolve normally, even when the shared folder is in another
pack.

### Mod

A folder or ZIP in `MODS_OPENRTS/` with the paths it replaces or adds, e.g.
`ROUTES/Kuban_RTS/TEXTURES/brick.ace` for a texture replacement, enabled in
`openrts.ini`:

```ini
[Mods]
1 = HD_Kuban_Textures
2 = VL80k_Sounds.zip
```

### Removing and updating

- **Remove:** delete the folder or the archive. Nothing else references it.
- **Update:** replace the archive, or add a pack to `PACKS/patch/` that holds
  only the changed files.
- **Download speed:** a few archives instead of 50 000 files. A per-pack file
  list with hashes allows downloading only changed packs later.

## Hot reload

Changing a model or texture on disk updates it in the running viewer/game
without a restart.

**Watching** (`core`):
- Folder sources and the `PACKS`/`MODS_OPENRTS` folders are watched with the
  OS (Windows `ReadDirectoryChangesW`, Linux `inotify`); polling of
  modification times is the fallback.
- Events are debounced (about 300 ms): editors and exporters write a file in
  several steps, and a half-written file must not be loaded.
- A change to a folder file updates its index entry. A changed archive is
  re-mounted: its directory is read again and the paths it provides are
  compared with the old set.
- The VFS then publishes a list of changed virtual paths **after** the new
  index generation is in place. Consumers subscribe to it. Only paths whose
  effective content changed are reported (source, size, modification time,
  CRC for archive members): editing a base-pack file that a mod overrides
  causes no reload.

**Recovering from lost events.** File-system notifications are not reliable,
so the index never trusts them alone.
- **Overflow:** a full `ReadDirectoryChangesW` buffer (a zero-length result
  or `ERROR_NOTIFY_ENUM_DIR`) or `inotify` `IN_Q_OVERFLOW` marks the whole
  source dirty.
- **Broken watch:** the handle fails or the folder is deleted and recreated,
  or the machine wakes from sleep. The watch is re-armed and the source is
  marked dirty.
- **Network and removable drives** that do not deliver events use polling.
- **Dirty source:** it is rescanned off-thread (paths, size, modification
  time). The result is compared with the source's previous snapshot; the
  difference gives the changed paths, and a new index generation is
  published. Missed events therefore turn into a rescan, never into a stale
  index.
- **Safety net:** a slow background reconciliation (default every few
  minutes, only while hot reload is on) does the same comparison for all
  folder sources, and a stat check for archives.
- **Half-written archives:** a replaced archive is opened only once its size
  and modification time stay stable through the debounce. If its central
  directory does not parse, the old mount stays and the error is logged.

**Atomic resource generations** (`renderer`):
- Each cached resource (texture, model) lives in a slot that holds an
  immutable *generation*: the GPU objects plus the CPU data they came from.
  Draw lists refer to slots, and a frame reads each slot's current
  generation once when it is recorded.
- A reload builds the new generation completely on worker threads (parse,
  decode, upload to new GPU buffers/textures). Nothing touches the live one.
- **Batch swap:** at the start of a frame the main thread publishes ready
  generations together. Files changed in one batch (a model and its new
  textures, a texture and its `.sd`) wait for each other and are published
  in the same frame, so no frame mixes a new model with old textures.
- **Failure keeps the old version:** a file that fails to parse or decode
  keeps its previous generation; the error is shown in the viewer overlay
  and in the log, so a broken save does not blank the object.

**Safe GPU release:**
- A replaced generation goes to a retire queue tagged with the number of the
  last frame that may use it.
- The renderer counts frames on the GPU with
  `wgpuQueueOnSubmittedWorkDone` after each submit. A retired generation is
  released only when the GPU has finished that frame and no CPU job still
  holds a reference (`shared_ptr` use count). Command buffers in flight
  therefore never point to freed buffers or bind groups.
- On shutdown or device loss the queue is drained after the device is idle.
- The same queue serves normal streaming eviction (tiles leaving the view),
  not only hot reload.
- Terrain, sky (ENV) and world files can use the same mechanism later. The
  first version covers models and textures.

**Scope:** on by default in the viewer (content creation), optional in the
game (`HotReload = 1` in `openrts.ini`).

## Files that must be real files

Legacy locomotive DLLs (and anything a DLL opens itself by path) are copied
from the VFS into `CACHE/extracted/<pack hash>/<virtual path>` on first use and
loaded from there. Paths passed to DLLs point into that copy.

## Code structure

`core` gets the VFS. `formats`, `assets`, `renderer` and `simulation` use it
instead of `std::filesystem`.

```cpp
namespace openrts::core {

class VirtualPath;       // normalised, case-folded, '..' resolved; refuses rooted paths

class Vfs {
public:
    void mount_directory(std::filesystem::path root, int priority);
    void mount_zip(std::filesystem::path archive, int priority);
    void build_index();

    bool exists(const VirtualPath&) const;
    std::optional<std::vector<std::byte>> read(const VirtualPath&) const;
    std::unique_ptr<InputStream> open(const VirtualPath&) const;
    std::vector<VfsEntry> list(const VirtualPath& directory) const;
    std::optional<VfsOrigin> origin(const VirtualPath&) const;   // which source
    std::optional<std::filesystem::path> real_path(const VirtualPath&); // extracts if needed

    // Hot reload: starts watching all sources; `poll_changes` returns the
    // virtual paths whose effective content changed since the last call.
    void start_watching();
    std::vector<VirtualPath> poll_changes();
};

}
```

- Parsers take bytes or a stream (most already parse from memory after
  reading); the `*_path` helpers become thin VFS wrappers.
- `ContentResolver` keeps the RTS search order (route → GLOBAL, trainset
  aliases) but asks the VFS instead of walking folders.
- Thread-safe reads: the index is immutable after `build_index`; each read
  opens its own handle or uses positional reads, so streaming threads work as
  now.

## Tool

`openrts-pack`:

- `pack <folder|paths> -o X.zip [--method deflate|zstd] [--level N]` —
  builds an archive with the compression policy and a manifest.
- `convert X.zip -o Y.zip --method deflate|zstd` — recompresses an archive
  file by file. Stored entries stay stored, and file contents are checked by
  CRC after conversion.
- `unpack X.zip <install root>` — restores the folder layout, for RTS.
- `list X.zip` — shows the archive contents.
- `verify X.zip` — checks the archive.
- `conflicts` — prints the effective source of every overridden path.

## Implementation

| Part | Where |
|---|---|
| `VirtualPath`, case folding | `core/src/virtual_path.cpp` |
| ZIP read/write (stored, deflate, zstd, ZIP64, UTF-8 / CP866 names) | `core/src/zip_archive.cpp` |
| Sources, index generations, watching, rescans | `core/src/vfs.cpp`, `core/src/directory_watcher.cpp` |
| `openrts.ini`, mount order | `core/src/installation.cpp` |
| VFS loaders | `formats` (`load_*(const core::Vfs&, VirtualPath)`) |
| Asset search | `assets/src/content_resolver.cpp` |
| Model assets, texture cache, generations, retire queue | `renderer/src/model_assets.cpp`, `model_renderer.cpp`, `include/openrts/renderer/gpu_retire_queue.hpp` |
| Hot reload batches | `renderer/src/model_library.cpp` |
| Tool | `tools/pack` (`openrts-pack`) |

Checks: `tests/vfs_test.cpp`, `tests/retire_queue_test.cpp`, `tests/core_smoke.cpp`.

Verified on content:
- TestRoute_RTS packed with zstd (220.9 MiB, 1108 files, 3.4 s; 476 files stored);
- converted zstd → deflate → zstd and unpacked: byte-identical to the folder;
- the deflate archive also passes Python's `zipfile` test;
- the route rendered from `PACKS/base` with `GLOBAL` from a folder;
- live hot reload: a texture added to a mod folder replaced 17 models and 1
  texture in one batch, and removing it restored the pack version; all retired
  generations were freed after GPU completion.

Not yet:
- The Linux `inotify` watcher is written but not compiled or run here.
- Terrain, sky and world files are read through the VFS but not hot-reloaded.

`Vfs::real_path` extraction is what the DLL host and the native plugins
load their DLLs from.

## Usage

```text
openrts_viewer --window --route D:\Games\MDDRTS\ROUTES\Kuban_RTS
openrts_viewer --window --route Kuban_RTS --root D:\Games\MDDRTS        (route may live only in a pack)
    [--mount extra.zip|folder] [--no-hot-reload]

openrts-pack pack D:\Games\MDDRTS\ROUTES\Kuban_RTS -o PACKS\base\Kuban_RTS.zip --method zstd --kind route
openrts-pack convert Kuban_RTS.zip -o Kuban_RTS.deflate.zip --method deflate
openrts-pack unpack Kuban_RTS.zip D:\Games\MDDRTS
openrts-pack list|verify Kuban_RTS.zip
openrts-pack conflicts D:\Games\MDDRTS
```

Member names inside a pack are installation-relative (`pack` derives them
from the installation the source folder belongs to, or takes `--as`).

## Steps (done)

1. `VirtualPath` normalisation + directory source + index + tests
   (case, `\`, `..`, rooted paths, override order).
2. ZIP source (stored, deflate, Zstandard; ZIP64) + tests on real content
   packed by the tool; check that a route loads identically from folders and
   from a pack.
3. Move `ContentResolver`, world/terrain/shape/texture/ENV loaders and the
   viewer to the VFS.
4. `openrts-pack` tool (`pack`, `unpack`, `convert`, `list`, `verify`,
   `conflicts`), `openrts.ini` mounts and mods.
5. Watching, rescan on lost events and change notifications in the VFS;
   resource slots with atomic generation swaps, the GPU retire queue, hot
   reload of textures and models in the renderer and viewer.
6. DLL extraction cache when the cab/DLL work starts.
