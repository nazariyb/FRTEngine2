# Assets

FRTEngine2's asset system is being built in milestones (M0-M13). This file describes what exists, briefly and
structurally; every milestone updates it. Today, after **M2 (one file watcher)**, the infrastructure exists and its
first client runs: shaders are compiled once and served from the derived-data cache, and everything that reloads files
in dev builds learns about changes from one file watcher. Materials, textures, models and input files still load as
before.

## Where it is going

- **Identity.** Every asset has a 128-bit GUID in a `<file>.meta` sidecar next to its source. The path is a hint, never
  the key, so moves and renames break nothing.
- **Truth.** Source files plus their `.meta` files are the truth. Everything else (the registry, the derived-data cache,
  cooked paks) can be deleted and rebuilt.
- **Registry.** A fast in-memory database of asset metadata (types, typed tags, data-table rows, dependencies) answers
  queries without loading anything.
- **Runtime.** Loads are asynchronous, through generational handles and reference-counted pointers, with deterministic
  publication so scripted captures stay byte-exact.
- **Packages.** A cook turns content into paks of final, GPU-ready data plus a runtime registry.

## One file watcher (M2)

Every system that reloads files in dev builds learns about changes from one watcher instead of polling timestamps:
materials, input actions and maps, shader programs and the editor's input files. Release builds watch nothing.

### `platform::IFileWatcher` (`Platform/FileWatcher.h`)

Reports what the OS says happened under armed roots as portable events: root, path relative to it (UTF-8, `/`),
kind (`Added`, `Removed`, `Modified`, `RenamedOld`, `RenamedNew`, `Overflow`, `RootLost`), entry type and file identity
(the bytes `platform::Stat` reports, for pairing moves). It decides nothing. Windows: one I/O completion port thread
for all roots with `ReadDirectoryChangesExW` (extended information, 64 KiB double buffers), which only re-arms, parses
and queues; a completion with 0 bytes is an `Overflow`, the leading `REMOVED` of a case-only rename is dropped, a
deleted or renamed root is `RootLost` and is armed again once it is back. `platform::ProbeFileWriters` tells whether a
writer still holds a file.

### `assets::CFileWatcherService` (`Assets/FileWatcherService.h`)

Portable, on the owner thread (the engine's main thread). **Level-triggered**: an event only marks a path dirty; when
the path has settled, its state on disk decides what subscribers are told, so a lost event costs a rescan and never a
wrong answer.

| Rule | |
|---|---|
| Settling | a path is told after 200 ms without events, at most 2 s after its first; a removal waits 1 s longer |
| Coalescing | `Removed` then `Added` is `Modified` (git's unlink and write, atomic saves), `Added` then `Removed` is nothing |
| Moves | the two halves pair by identity within 150 ms into one `Moved`; a case-only rename is a `Moved` |
| Directories | one moved or copied in is enumerated (every file `Added`); one renamed is one directory `Moved`; one removed is one directory `Removed` (then an `Added` if a file took its place); without entry types (FAT, ReFS, shares) the disk is asked, and a directory's own `Modified` is dropped |
| Bursts | more than 256 events without a pause hold everything until `[AssetsEditor] VcsSettleMs` (1500) of quiet, then one batch; `SetVcsBusyQuery` is the seam for M4's VCS provider |
| Overflow, lost root | one `Rescan` of the root: compare what you hold with the disk, reload only what differs |
| Writers | a file a writer still holds waits (back-off up to 2 s, a warning after 10 s) |
| Self-writes | `ExpectWrite(path, hash, size, deadline)`: an echo with exactly those bytes is not told |

`Subscribe(directory, { Prefix, Extensions }, callback)` (the directory need not exist yet), `Unsubscribe`, `Pump()`
(once per frame: `CEngine::BeginFrame`), `Flush(directory)` (a fence file `.frt-fence-<pid>-<n>` in every root that
watches the directory - roots overlap when a directory was subscribed before one above it: everything older is told
now; for tests and scripted steps), `SetEnabled(on, onlyUnder)` (optionally for the roots in or above some directories
only), `GetStats`. `IsFileAffected(change, file)` is the rule loaders share: a change of the file's path, or of a
directory above it, and never a removal; `CAffectedFiles(changes).Contains(file)` answers the same for many files in
one lookup each. Paths compare as NTFS compares names (`MakePathKey`, `Containers/StringMap.h`: its upcase table, not
Unicode folding), through `platform::NormalizePath` and `platform::GetRelativePath` (`Platform/FileSystem.h`). `CEngine::GetFileWatcher()` owns one in Debug and Development (headless too;
null in Release and in tool hosts). `[Assets] Watch` (default 1) turns it off.

### Subscribers

| Subscriber | Watches | On a change |
|---|---|---|
| `CMaterialLibrary` | the directory of every material it loaded (any name: the serializers read by content) | marks the material if its write time differs; `ReloadModifiedMaterials` (the renderer, once per frame) reads it again, `Version` goes up |
| `CInputActionLibrary` | the directories of the actions and maps it loaded | reads them again inside `Pump`, before the frame's maps are evaluated |
| `CPipelineLibrary` | the shader root, and the directory of any file a build read outside it | marks every program that read the file; `RebuildChangedPrograms` (the renderer) builds them again |
| `CEditorInput` | `Editor/Config/EditorInput.ini` and `Local/Editor/EditorInput.ini` | both read again on the next frame |

A removed file keeps what was loaded. The game module's DLL keeps its own exclusive-open "linker done" probe.
`CMaterialLibrary` and `CInputActionLibrary` are passed to game modules and changed layout, so the game module ABI is 7.

### Scripted runs

Automation runs (an `[Automation]` key that makes a run scripted) and the editor's smoke tests are scripted:
`CEngine::StartUp` forces `[Assets] Watch=0` (nothing reloads behind a capture's back) and `[Assets] ReadOnly=1`
(the asset system writes nothing; the switch is for M4) (`CAutomation::IsScriptedRun`, `ApplyScriptedRunRules`). A
smoke step that needs the watcher turns it on for its own directories only (the input overrides' `Local/SmokeTest`; the
scratch copy and its shader copy) and flushes it after writing, so its result never depends on timing and nothing of the
checkout it runs in reloads behind its back. With `-Editor:SmokeTestEdits=1` the smoke test also edits files - a
material, which must reload, and a shader include, which must rebuild exactly the programs that read it - but only on a
project outside the engine checkout and with the shaders read from a copy, `-Render:ShaderRoot=<dir>` (dev builds).
`FRT.bat verify` runs it on a scratch copy of `Templates/Demo` made with `FRT.bat new` in `%TEMP%`.

## Tool host, worker pool, derived-data cache, shader cache (M1)

### AssetTool and `FRT.bat assets`

`AssetTool.exe` is the asset system's command line, a console host beside the editor and the launcher (Debug,
Development, Development-Headless; Release cannot load a game module, so it never runs tools). `FRT.bat assets <verb>`
runs it (building it first when it is missing or stale) for the current project, or for `/Engine` with `--engine`.

| Verb | Does |
|---|---|
| `help [verb]` | lists the verbs, or explains one |
| `newguid [n]` | fresh GUIDs for builder versions and built-in assets, as text and as a C++ initializer |
| `lock` | holds the mount's writer lock until Enter (for tests and scripts) |
| `ddc stats`, `ddc gc [--max-gb=<n>]`, `ddc verify [--ddc=<dir>]` | the derived-data cache: counts, LRU trim (a dry run with `--ci`), re-check and delete corrupt files |

Options: `--json` (one object per line), `--ci` or `-Assets:ReadOnly=1` (read-only, never takes the writer lock),
`--project=<file>`, `--engine`. Exit codes: 0 success, 1 problems found, 2 usage error, 3 internal error, 4 the mount is
owned by another process. **One writer per mount**: a writing run takes `<mount>/Local/Assets/Owner.lock` (an exclusive
handle the OS releases when the process dies; its text names the holder), and a second writer exits with code 4 naming
the owner. The editor takes the same lock from M4, when it starts writing metas.

### Worker pool (`Jobs/WorkerPool.h`)

`CWorkerPool` runs on engine-owned threads (`cores - 2`, at least 1, named through `platform::ConfigureThread`), with
three priorities, 64-bit cancel groups and an MPSC completion queue. Completions run on the owner thread, in batch or
strict submission order, when it pumps (`PumpCompletions`, `PumpUntil`); a wait is always a pump loop, never a bare
block. `BumpEpoch` discards completions of older work; `CancelAllAndDrain` is the barrier a game-module reload will use.
`CEngine` owns one pool (`GetWorkerPool()`, pumped in `BeginFrame`); texture decoding runs on it. Workers never touch
TLSF, `TArray` or `TRefShared`: in Debug, `CMemoryPool` asserts when another thread uses it.

### Derived-data cache (`Assets/DerivedDataCache.h`)

A machine-wide, content-addressed store: `FRT_DDC_DIR`, else `[DerivedDataCache] Path=`, else
`%LOCALAPPDATA%\FRTEngine\DerivedDataCache` (`FRT_USER_DATA_DIR` moves it for tests). Every process, project and
worktree on the machine shares it, without locks.

- **Keys: two steps.** The StaticKey hashes everything known before a build (`CKeyWriter`'s KeyBlob: builder, version,
  target facets, settings, source hash). The discovered inputs (for shaders, every include, and every place an include
  was looked for and not found) are known only after it: `Manifests/<StaticKey>` keeps up to four input sets with the
  Key each gave, so editing an include misses and reverting it hits again. `Key = BLAKE3-160(StaticKey ‖ inputs)`.
- **Layout.** `Manifests/`, `Records/<Bucket>/` (small values inlined, up to 64 KiB each and 256 KiB per record),
  `Values/` (large values, zstd when it saves 5% or more), `Tmp/`, `Access/` logs for LRU. Writes go to `Tmp/`, are
  flushed, then renamed: large values, record, manifest, so a crash leaves only unreferenced files. Every record and
  manifest ends in a checksum, every value's raw hash is checked on read: anything that fails is a miss and is deleted.
- **Never in a key:** paths, the asset's own GUID, timestamps, machine names.
- **Per-project state**: `Local/Assets/BuildState.log` (append-only, checksummed step records: KeyBlob, inputs, Key,
  status) and `Local/Assets/BuildJournal.log` (a step that began and never ended is reported as crashed). Only the
  writer-lock holder writes them.

### Shader cache (`Graphics/Render/RHI/ShaderCache.h`)

`CShaderCache` sits between `CPipelineLibrary` and DXC in dev builds: a warm editor start compiles no shader. Its key
holds the DXC identity (hashes of `dxcompiler.dll`, `dxil.dll` and `ThirdParty/Dxc/VERSION.txt`), entry, profile,
defines, arguments with every path written as a mount path (`/Engine/Shaders/...`), the source hash, and `Rhi: D3D12`
(DXIL depends on the shader model, never on the GPU). Debug information embeds absolute paths, so that variant adds a
hash of the shader root and never leaves this machine. Hot reload goes through the file watcher (M2). The startup log
prints `[shader] cache: <hits> hits, <compiled> compiled`; the Stats panel shows the counters. Switch:
`-Ddc:Verify=<percent>` (recompile that share of hits and compare; a difference is `BUILD002`). Release builds still
compile at run time until the cook ships DXIL (M12).

### Builder versions and the source guard

A builder is a pure function of its key, so its version GUID must change whenever its code could change its output.
`Core/Source/Assets/BuilderVersions.inl` lists every builder: name, version GUID, the hash of its sources when that GUID
was set, and the globs of those sources. Every build regenerates the current hashes (`FRT.bat builder-hashes`, a
prebuild step). A builder whose sources changed while its GUID did not keeps working, but local-only: its keys carry a
`LocalSource` discriminator and its outputs never leave this machine. `FRT.bat check-builders` fails (`BUILD003`) until
the GUID is bumped: `FRT.bat assets newguid`, paste it into the row, then `FRT.bat check-builders --update`, which pairs
each new GUID with exactly one source hash. `python -I Tools/Lint/CheckPipeline.py` keeps builder sources pure: no
file access, config or paths outside the recorded inputs, no TLSF types, no hash-table iteration, no mutable globals.

### Build scheduler (`Assets/BuildScheduler.h`)

`CBuildScheduler` is the skeleton later milestones build on: requests with a priority (`Escalated`, `Visible`,
`Background`, `Cook`) and a sequence, steps on the worker pool, a step that needs other steps first is restarted at most
3 times (then a cycle error), cancellation, and the pool's commit-order modes.

## Identity and hashing (`Assets/AssetId.h`, `Hash/StableHash.h`, `Assets/KeyWriter.h`)

| Type / function | What it is |
|---|---|
| `assets::SGuid` | 16 bytes in RFC 9562 order. `NewRandom()` is a UUIDv4 from the OS CSPRNG; text is 32 lowercase hex digits; `Parse` also accepts dashed, braced and uppercase spellings. Text that would read as a number is re-rolled. |
| `assets::MakeDerivedGuid(Parent, Generator, Label)` | deterministic UUIDv8 for identities nobody commits (BLAKE3 derive_key "FRT derived GUID v1") |
| `assets::MakeAutoGuid(Salt, Generator, Input)` | reproducible UUIDv4 for metas a tool creates on its own, so two machines that see the same commit agree (derive_key "FRT auto GUID v1") |
| `assets::SAssetId { Guid, SubId }` | an asset or a sub-asset; `SubId == 0` is the main asset |
| `assets::MakeSubId(Label)` | `XXH3_64("kind:name")` for importer labels and table rows (`"row:" + key`) |
| `assets::SAssetId64`, `MakeAssetId64` | the 64-bit runtime, cooked, save-game and network key: `XXH3_64` with the seed `"FRTASSET"` over `Guid ‖ le64(SubId)` |
| `hash::Xxh3_64` | XXH3, 64-bit (output frozen since xxHash 0.8.0): ids and lookups |
| `hash::SContentHash`, `CContentHasher`, `HashContent`, `DeriveKey` | BLAKE3 truncated to 160 bits: content, cache keys and integrity |
| `assets::CKeyWriter`, `MakeStaticKey` | the canonical KeyBlob that derived-data keys hash: records of fields sorted by name, each `(u8 type, varint name length, name, value)`; floats as raw bits, enums by stable name |
| `compression::CompressZstd`, `DecompressZstd` | zstd frames |

The byte formats are frozen: Core-Test's known-answer tests (`Core-Test/Hash`, `Core-Test/Assets`) are their normative
values. `hash::HashBytes` (wyhash) stays in-memory only: it is never written to disk.

## Platform layer (`Platform/*.h`, Windows in `Platform/Windows/`)

Asset code never calls the OS directly; `python -I Tools/Lint/CheckPlatform.py` enforces it. v1 implements Windows only.

| Function | Use |
|---|---|
| `Stat`, `GetFileIdentity` | size, write time and an opaque file identity (NTFS FileId + volume serial) for move and atomic-save detection |
| `EnumerateDirectory` | one listing call with name, size, times and identity per entry |
| `WriteFileAtomic`, `ReplaceFileAtomic` | temp file in the same directory, flushed, then replaced: a crash never leaves a torn file |
| `WriteFileBytes` | the one in-place writer (`WriteFileText` forwards to it) |
| `ReadFileChunksStable` | streams a file and reports `Changed` if it changed while being read, so a hash is never recorded for bytes that are gone |
| `CBlockingFileReader` | synchronous ranged reads, files over 2 GB included |
| `MoveToTrash` | the Recycle Bin, so editor deletes can be undone |
| `QueryVolumeInfo` | case sensitivity (per volume and per directory), remote and cloud volumes |
| `ReserveVirtual`, `CommitVirtual`, `DecommitVirtual`, `ReleaseVirtual` | virtual memory |
| `FillRandomBytes`, `GetCurrentThreadId`, `ReadTimestamp`, `GetModuleImage` | randomness, thread ids, a monotonic clock, a loaded module's address range |

Paths of any length work: every path the Windows layer hands to Win32 is prefixed for long paths when needed.

## Rule globs (`Containers/PathGlob.h`)

`CPathGlob` is what asset rules will match files with. `/` separates folders; `*` never crosses a `/`; `**` matches
any depth, including none; `?`, `{a,b}`, `[abc]` and `[0-9]` work as usual. Matching ignores case under Unicode simple
case folding, the same on every machine. Anchoring and folders follow gitignore: a pattern without `/` (other than a
trailing one) matches a name in any folder, a `/` at its start or in its middle anchors it to the base (the mount, or the
folder of a folder rule file), a pattern that matches a folder covers everything below it, and a trailing `/` names
folders only. Invalid patterns are reported, never guessed at.

## Callbacks, log and diagnostics

- `TFunctionRef` (a callable passed down a call), `TUniqueFunction` (a stored callable, small buffer, CRT heap) and
  `TMulticastDelegate` (handles, safe add and remove while broadcasting, `ReleaseModule` for a game module's image).
  `CEvent` is retired; window and input events are delegates (game module ABI 6; 7 since M2).
- `log::` (`Log/Log.h`): categories (`assets`, `platform`, `memory`, `game`) and levels (`Error`, `Warning`, `Info`,
  `Verbose`), written as `[category] message` to stderr and the debugger.
- `SDiagnostic` and `CDiagnosticStore` (`Diagnostics/Diagnostics.h`): structured problems with stable codes such as
  `META001`, stored per subject, each printed once as `[assets] <subject>: <message>`.

## Profiling and memory

- `FRT_CPU_SCOPE("Area.Name")` records a CPU scope; the Stats panel shows the last frame's scopes.
- `CMemoryPool` counts allocated bytes (current and peak), can call back at a high-water mark, and logs once per thread
  any use from a thread other than its owner: the pool (TLSF) is single-threaded.

## Line endings and verification

- Hashed text formats (`.meta`, `.yml`, `.frt`, `.csv`, `.ini`, shaders) are LF in every working tree, so content hashes
  are the same on every machine. `.gitattributes` says which; `FRT.bat setup` checks out again any file whose working
  copy has the wrong line endings, and `FRT.bat check-line-endings` only checks.
- `FRT.bat verify` is the merge gate: builds, Core-Test, lints, the editor's smoke tests (in place, read-only, and with
  file edits on a scratch copy), automation captures against baselines (raster byte for byte, path tracing within a
  tolerance), the Release package's captures, and a clean `git status` at the end. `--quick` runs the pre-commit subset; `--record-baselines` records new baselines in
  `Local/Verify/Baselines`, which belong to the machine that took them.
