# Assets

FRTEngine2's asset system is being built in milestones (M0-M13). This file describes what exists, briefly and
structurally; every milestone updates it. Today, after **M0 (foundations)**, the infrastructure the asset system stands
on exists, but no asset is loaded through it yet: materials, textures, models, input files and shaders still load as
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
  `CEvent` is retired; window and input events are delegates (game module ABI 6).
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
- `FRT.bat verify` is the merge gate: builds, Core-Test, lints, the editor's smoke tests, automation captures against
  baselines (raster byte for byte, path tracing within a tolerance), the Release package's captures, and a clean
  `git status` at the end. `--quick` runs the pre-commit subset; `--record-baselines` records new baselines in
  `Local/Verify/Baselines`, which belong to the machine that took them.
