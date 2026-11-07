# Assets

FRTEngine2's asset system is being built in milestones (M0-M13). This file describes what exists, briefly and
structurally; every milestone updates it. Today, after **M4 (identity becomes authoritative; VCS provider)**, shaders
are compiled once and served from the derived-data cache, everything that reloads files in dev builds learns about
changes from one file watcher, and the asset registry catalogs every mount - rules, records, typed tags, dependencies -
for queries from `FRT.bat assets` and the editor's Assets and Asset problems panels. Every included file of the engine
and the templates has a committed `.meta` whose GUID is its identity; the editor that owns a project creates, restores
and re-GUIDs metas through each mount's VCS provider, and the meta lifecycle writes only metas into `Content/` (the
material and input libraries' create-on-load writes aside, until M7 and M10: see below). Nothing reads assets
through the registry yet: materials, textures, models and input files still load by path as before, and no runtime
code or package reads a meta.

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

## Registry (M3, M4)

The registry is an in-memory catalog of every file in the mounts, kept current from the disk and persisted as a cache
that may always be deleted (design §5). It reads `Content/`, the rule files, the metas and the snapshots, and writes its
own files under `Local/`; into `Content/` only the meta lifecycle writes, and only metas (below).

### Mounts, records and ids

| Mount | Content | Rules | Owner root (Local/, diagnostics' `File`) |
|---|---|---|---|
| `/Engine` | `Core/Content` | `Core/Config/Assets.yml` | the engine checkout |
| `/Project` | `<project>/Content` | `<project>/Config/Assets.yml` | the project |
| `/Plugin.<Name>` | `<project>/Plugins/<Name>/Content` (mounted when it exists) | `<project>/Plugins/<Name>/Config/Assets.yml` | the project |

A mount path is `/<Mount>/<relative path>` in the on-disk spelling; a sub-asset is `<file>#<label>`
(`/Project/Models/Box.gltf#mesh:Body`). `CMountTable` (`Assets/Discovery/Mounts.h`) is the one place that turns disk
paths into mount paths and back; `paths::ToMountPath` and `paths::FromMountPath` answer from the table a Debug or
Development `CEngine` installs (nullopt in tool hosts, which resolve their own table, and in Release).

- **FILES** has a row per file a scan kept: class (`Asset` claimed by an importer, `Raw`, `Unclaimed`, `Companion`,
  `Excluded`, `Meta`), size, mtime, file identity, a content hash once something read the bytes, and the meta columns
  (`MetaState`, `MetaHash` = XXH3 of the meta's raw bytes). A file excluded by a rule is never in FILES. A `.meta` stays
  a built-in exclude, so no rule classifies it: the scanner splits metas out of each listing before the rules see it and
  pairs each with its source (byte-exact, else under the portable fold). A meta has a row of its own, class `Meta`
  (`Paired`, `Orphan` or `Broken`), that no file count, `files` field or dump line shows.
- **Records**: every claimed file has one record - type, importer, Analyze stamp and status, cook rule, chunk, labels,
  typed tags, edges, companions, and sub-entries (a glTF's meshes, materials and images).
- **Ids** (M4). A record's GUID is its meta's (a broken meta's salvaged one); else the stored record's once its file had
  a meta, since a lost meta never changes identity; else, for a file with no meta yet (`PendingMeta`) or whose first
  meta is broken and salvages no GUID, `MakeProvisionalGuid(<mount path>)` (`MakeDerivedGuid(0, "path", path)`):
  internal, never written to a meta or a content file, never handed to runtime code, and no identity (never tombstoned;
  that broken meta deleted, the file is a new file again). A meta is read on a worker only when it changed or when its
  file is analyzed or re-evaluated; an unchanged meta of an unchanged file is never opened. A meta that changes while it
  is read (git or an editor writing it) leaves its file out of that pass, keeping what is committed of it, rather than
  committing a record made without it; the write's watcher event or the next scan reads it again. A meta edit is
  re-evaluated in memory and re-analyzes only when the `AnalyzeStamp` changes. Two records with one GUID (a copy, until
  the lifecycle re-GUIDs it) are kept, and every lookup by id answers the one whose path sorts first under the portable
  fold. A rename or a move inside one scan pass, with its meta, reaches `OnAssetsChanged` as `Moved` (in one commit);
  within one broadcast, an id that left a path and holds another at its end is a `Moved`, while a record added and
  removed again or a path that went and came back stays a removal and an addition. A file without a meta has a
  path-derived id, so its move is a removal and an addition, and a record whose id changes in place (its meta appears, a
  GUID is edited) is a removal of the old id and an addition of the new one. Every host's registry runs the meta
  lifecycle (`SAssetRegistryDesc::Lifecycle`, below); with it null (tests and benchmarks that predate it) metas are
  read, nothing is written and no `META` problem is reported.
- **Identity state** (M4, design §5.2, §5.3), all keyed by GUID and travelling in the same deltas, journal and snapshot
  as the records:
  - `METAS`: the raw bytes of each meta's last Ok parse, one copy per GUID, kept only when they hash to the copy's
    `MetaHash` (checked again at every load), for the lifecycle's verified restore. A re-evaluation (a changed rule
    chain) and a pinned file's class take an unchanged meta's values from its copy, never from the disk; a meta that
    changed is read from the file and its copy replaced.
  - `TOMBSTONES`: a record with a meta-given GUID whose file is removed, or whose GUID is edited by hand, leaves
    `{GUID, last path, content hash, file identity, time}`; a record that registers the GUID again (a move, a restore)
    clears it, a provisional id never leaves one, and a snapshot written more than 30 days later drops it. While
    another live record holds the GUID (a copy of the asset, a pass on another mount that committed first) neither a
    tombstone nor the removal of its copy applies: the store decides at commit, so the order commits arrive in never
    changes the result.
  - The **alias index**: a meta's `Aliases` resolve to its record (`{B, s}` to `{A, s}`), and a sub-asset renamed with
    `SubAssets.Renamed` keeps its old label's id while its new label's id resolves to it. `Find` and id edges try the
    canonical id first.
  - **Former paths**: a meta's `FormerPaths` answer `FindByPath` and path edges when no file has the path under the
    portable fold, so a file created at an old path takes the references over.
  - An edge resolved through an alias or a former path keeps its target, matches `is:redirected` and adds `REF006`
    (info) to its record; `FindByPath` hands a `REF006` notice back. A lost or broken meta's record keeps its
    redirects, as it keeps its GUID.
- **The meta's values** apply last: importer default < rules < meta. Its `Importer:` pin replaces the rules' choice of
  importer (still subject to `Importers.Exclude` and availability; an unknown name pins nothing), its `Cook`, `Chunk`,
  `Settings` and `Targets` override the rules', and `explain`'s provenance names the meta
  (`Content/Rock.png.meta`); its `Labels` are the record's authored labels, which `Label` rule selectors match.
  `SubAssets` applies to the sub-entries: `Renamed` keeps an old label's id for a new label, `Remap` adds a
  `Hard|Declared` edge from the sub-entry to the external asset, `Overrides` only names sub-assets in M4; a label the
  file lacks is a warning, and any `SubAssets` edit re-analyzes the file.
- **Companions**: files a record's importer reads besides its own (a glTF's `.bin` buffers) have class `Companion` and
  no record; they return to `Unclaimed` when no record names them.
- **Importers' Analyze** (cheap facts of the source, never a build): `Texture` (`stbi_info` on a header prefix: Width,
  Height, Channels, HasAlpha), `Model.Gltf` (cgltf on the JSON chunk: counts, `mesh:`/`material:`/`image:` sub-entries,
  external URIs as `Build|Path|Declared` edges, buffers as companions), `Material.Legacy` (`.frtmat.yml`: texture paths
  as `Build|Path` edges, `REF007` each), `Input.Legacy` (`.frtinputmap`: Actions, an edge per action, Primary;
  `.frtinputaction`: ValueType), `Raw`. A schema-free scan finds `$asset` references in any parsed tree.

### Rules (`Assets.yml`, `Assets/Pipeline/Rules.h`)

Each mount's `Config/Assets.yml` and any folder's `Assets.yml` (FormatVersion 1) say what is scanned and how files are
imported (design §3.5). A file's chain is its folder's file, each ancestor folder's up to the content root, the mount's
`Config/Assets.yml`, then - for every mount but `/Engine` - the engine's `Core/Config/Assets.yml`; no other mount's
rules ever apply. Per field, the deepest file that sets it wins, and within a file the first matching rule.

- `Scan.Include` narrows (every chain file that lists patterns must match), `Scan.Exclude` accumulates and always
  wins (an excluded folder is not descended, its rule files are never read), `Scan.Raw` makes files `Raw`,
  `Scan.Quiet` keeps an unclaimed file out of `RULE002`. Built-in excludes cover dot-names (watcher fences
  among them), `~$*`, `*.tmp`, `*.orig`, `*.meta` (split out as metas, see above), Blender's and Krita's backups, `Thumbs.db` and
  `desktop.ini`; symlinks and junctions are never descended.
- `Importers.Exclude`, `Types.Include`/`Types.Exclude`, `Presets` (`<Importer>/<Name>`, typed by the importer's
  settings table, the innermost definition wins), `Rules` (`Match`, `Type`, `Label`, `Preset`, `For`, `Settings`,
  `Importer`, `Labels`, `Cook: Auto|Always|Never|EditorOnly|DevelopmentOnly`, `Chunk`, `Targets`, `Final`),
  `Validation:` (code -> `Error|Warning|Info` under that scope).
- A file with any error is rejected whole (`RULE001` with file, line and column) and its last good rules stay in force;
  a malformed file never stops start-up.
- M3 deviations from the design: `Scan.Quiet` alone makes an unclaimed file quiet - a file of an excluded or
  unavailable importer (`Document`, `Texture.Psd`, `Texture.Krita`, `Model.Blender`, `DataTable.Csv`) stays
  `Unclaimed` and raises `RULE002` unless quiet; `Type`/`Label` rules may not set the importer or an Analyze-relevant
  setting; `Validation.Naming`, `Cook` and `PrimaryAssetTypes` are validated and listed, never acted on; `Safety` is a
  mount's own (`CRulesEngine::GetSafety`, `ReadSafetyOfRuleFile` for hosts), read by M4's lifecycle;
  type names outside `frt.` are kept and match nothing until reflection (M5). No setting is Analyze-relevant in M3, so
  a settings edit never re-analyzes; an `Importer:` or `Scan.Raw` change does. So does a `Types` or `Validation:` edit
  that changes what they filter, for the files under it: a record keeps its sub-entries and Analyze problems as they
  were filtered, so each record's `AnalyzeStamp` covers the effective type filter and Validation overrides of its
  folder (`CRulesEngine::GetAnalyzeFilterHash`; nothing filtered leaves the stamp of the settings alone).
- **Hashes.** A rule file's `RulesHash` covers its normalised entries (comments and formatting change nothing; 0 = no
  file). The *semantics hash* covers what turns those texts into results - the importer table, the rules engine's
  version, the target - and the registry always resolves with an empty target, so every host agrees. A snapshot's
  `RulesBasis` is the semantics hash, plus for `AssetRegistry.snapshot` the engine file's `RulesHash` that `/Project`
  and the plugins fall back to; a different basis at load drops the file's RULEFILES rows, and its mounts are
  re-evaluated (in memory: glob matching, Analyze only where the `AnalyzeStamp` changed).
- **`Local/Assets/Rules.cache`**: the rule files as JSON lines (`{"format":"frt.rules.cache","version":1}`, then per
  file `{"kind":"file","mount","folder","file","level","hash","malformed"}` and per entry `{"kind":<section>,"mount",
  "file","line","rule","text"}`), for hooks and Lua that never parse YAML. Only the writer-lock holder writes it.

### Problems

Each problem has a `Subject` (the mount path of the file or folder, as spelled on disk), a `File` (the rule or source
file relative to the mount's owner root: `Config/Assets.yml`, `Core/Config/Assets.yml`, `Content/Art/Assets.yml`),
line and column; absolute paths appear only in messages. They are kept per subject and source: `Scan` (PATH001-003,
unlistable folders, skipped links), `Rules` (RULE001), `Analyze` (importer diagnostics, REF007, a failed Analyze with
code 0), `Unclaimed` (RULE002, recomputed once companions are known), `Registry` (REF006 and PATH001 of edges) and
`Lifecycle` (what the meta lifecycle reports: META*, VCS00x, and the hosts' MIG001).
A rule file's `Validation:` overrides the severity of every source but `Rules` (a file cannot silence its own RULE001)
and `Registry`, whose severities are fixed.

| Code | Rule | Severity | Source |
|---|---|---|---|
| `PATH001` | two files or folders whose names differ only in case (portable fold) in one folder (so, at the first differing folder, any two paths of a mount); an edge whose path matches two records | error (a warning before M4) | Scan, Registry |
| `PATH002` | a name illegal on a supported platform, or not valid UTF-8 | error (a warning before M4) | Scan |
| `PATH003` | a mount-relative path longer than 180 UTF-8 bytes | warning | Scan |
| `RULE001` | a rule file error: unknown key, wrong type, bad glob, unknown importer, setting, preset or target, a mount-level section in a folder file | error | Rules |
| `RULE002` | an unclaimed file not in `Scan.Quiet`, once per folder and extension | warning | Unclaimed |
| `REF006` | a reference that resolved through an alias (a meta's `Aliases`, a renamed sub-asset's new label), a former path (`FormerPaths`), or a path spelled differently from the file on disk | info | Registry |
| `REF007` | a legacy path reference in data (a material's texture path), until `migrate material-refs` | info | Analyze |
| `META001` | an included file without a `.meta` after the creation delay | error with `--ci`, guests or `Safety.AutoRepairMetas: false`, or when its content cannot be read to derive its GUID (in use, access denied: only that file waits); otherwise the owner creates the meta | Lifecycle |
| `META002` | an orphan `.meta`: its source is gone | warning | Lifecycle |
| `META003` | a duplicate GUID | error; with `Safety.AutoResolveDuplicates` the owner re-GUIDs the losers | Lifecycle |
| `META004` | a broken meta: a parse error, conflict markers, two `Guid:` lines | error | Lifecycle |
| `META005` | a meta that parses and is not canonical (`resave --metas` rewrites it); never one a newer engine wrote, which this build reads and does not rewrite | warning | `validate` |
| `META006` | a meta whose case differs from its source's | error | Lifecycle |
| `META007` | a meta beside a file the rules exclude (a rule file too), or beside one of a class that takes no meta (a companion, an unclaimed file) | warning | `validate` |
| `META008` | `MissingMeta`: a lost meta that no repair will restore (no verified copy, its GUID live in another meta, a read-only file or `Safety.AutoRepairMetas: false`), or a new file that an orphan meta matches and that is not paired automatically (ambiguous, read-only, or `Safety.OrphanPairing: false`) | error | Lifecycle |
| `META009` | an orphan meta that matches a new file and is not paired with it automatically: by content outside the move window, or a match that `META008` reports (relink suggestion) | info | Lifecycle |
| `VCS001` | CRLF in a hashed FRT text file (detected only in M4; blocking DDC writes arrives with the first asset builder) | error | Lifecycle (metas), `validate` (rule files; `.yml`, `.frt`, `.csv`, `.ini`, `.frtinputmap`, `.frtinputaction` sources) |
| `VCS002` | the VCS refused a write (`CheckOut`, `Move` or `Delete` failed); the command wrote nothing | error | the command |
| `VCS003` | an asset and its meta tracked differently ("generated locally: commit it") | warning | Lifecycle (a meta this process created), `validate` (every pair, one `StatusOf` per provider) |
| `VCS004` | a Git LFS pointer file where a source is expected; never imported or hashed into a GUID | error | Lifecycle |
| `MIG001` | a repository behind the engine: its `Schemas/Migrations.yml` is missing, malformed or lacks a step this engine requires | error | `validate`; the hosts' gate (`scan`, `status`, a refused AssetTool run, `CEngine`) |
| `MIG002` | a leftover legacy pattern, such as `<stem>_mat<N>` beside a model without `SubAssets.Remap` | warning; nothing reports it in M4: its patterns go with the steps of M7 and M9a | `validate` |

The engine and the templates are clean of PATH001-003, RULE001-002 and errors, and every file of theirs that takes a
meta has its committed one: the golden dumps assert it (Core-Test runs them with the lifecycle reporting, so a
metaless file is a `META001` error there), and `FRT.bat verify` runs `validate --ci` on each.

### Queries (`Assets/Registry/AssetQuery.h`, `QueryEngine.h`)

One text syntax serves `FRT.bat assets query` and the Assets panel (design §5.8): `rifle "assault rifle"` (substring
of name or path), `type:Texture` / `t:` / `type=frt.Texture`, `dir:/Project/Data` (recursive; `dir=` that folder),
`label:Boss` / `-label:Debug`, `Width>=1024 Name:Rif` (`:` is a prefix for text, equality otherwise), `Key in (a,b)`,
`ref:X` / `ref:~X` / `dep:X` (referencers, transitive referencers, dependencies; X is a mount path with an optional
`#label`, or a unique name), `is:<predicate>`, `or`, `( )`, `order:Key [asc|desc]`, `limit:N`, `rows` / `table:X`.
Errors carry the column of the term at fault.

- `is:` with data: `broken-ref`, `unused`, `legacy-ref`, `excluded`, `problem`, `stale-tags`, `primary`,
  `redirected` (an edge of its own resolved through an alias or a former path) and `pending-meta` (its file gets a
  meta and has none yet; a type-excluded file never gets one); `unsaved` parses and matches nothing until M5-M6 (a
  note says so). `rows` matches nothing until data tables (M5).
- System columns, for predicates, `order:` and result columns: `$Type`, `$Name`, `$Path`, `$Folder`, `$Labels`,
  `$Size`, `$ImportStatus`, `$Problems`, `$Referencers` (distinct referencing files), `$Dependencies`, `$CookRule`,
  `$Chunk`, `$PrimaryName`; tag keys per type (`frt.Texture`: Width, Height, Channels, HasAlpha; `frt.Model`:
  MeshCount, MaterialCount, ImageCount, VertexCount, TriangleCount; `frt.Mesh`: VertexCount, TriangleCount;
  `frt.InputActionMap`: Actions; `frt.InputAction`: ValueType).
- Sub-assets: a query returns main records, plus the imported sub-assets of a type that exists only as a sub-asset
  when a `type:` term names it (`type:Mesh` finds a glTF's meshes; `type:Texture` lists texture files, not the images
  a glTF names).
- Result order is deterministic: the `order:` key (absent values last), then the file's mount path (portable fold,
  then bytes), then the sub-entry label.
- C++: `CRegistryView::Compile`, `Run` and `RunText`; `ForEachColumnValue` and `FormatColumnValues` give a result
  table's cells. A compiled query belongs to one revision.

### Snapshots, journals and the writer

| File | Holds | Written by |
|---|---|---|
| `<engine>/Local/Registry/Engine.snapshot` (+ `Engine.journal`) | `/Engine` | the engine's owner: `<engine>/FRT.bat assets scan --engine` |
| `<project>/Local/Assets/AssetRegistry.snapshot` (+ `.journal`) | `/Project` and its plugins | the project's writer: the editor holding `Owner.lock`, `FRT.bat assets scan` |
| `<root>/Local/Assets/ProjectId` | a random id, so a copied snapshot is never taken for this root's | the root's writer, once |
| `<root>/Local/Assets/Rules.cache` | the rule files as JSON lines | the root's writer |
| `<root>/Local/Assets/OpJournal.log` | the planned steps of the meta operation in progress (checksummed frames, flushed before the first file is touched; removed when it ends), each write with what its file held when it was planned; rolled forward at the next start | the root's writer: the lifecycle's repairs and the meta verbs |

A snapshot is sections with their own layout versions and checksums; a bad section is dropped alone and only what it
held is rebuilt (design §5.3). Every section is at layout 2 since M4 filled `ALIASES`, `FORMERPATHS`, `METAS` (one
zstd frame of every copy's bytes) and `TOMBSTONES`, so an M3 snapshot is not used: one cold start. Losing `METAS`,
`ALIASES` or `FORMERPATHS` re-evaluates the file's mounts, which reads the metas again; losing `TOMBSTONES` forgets
them. The journal is at format 4 (the identity state) and is replayed only beside its own snapshot. The journal holds the deltas committed since the snapshot (one checksummed frame per
commit; a torn tail is dropped) and is replayed when its header names the snapshot. The **loss contract**: losing any
of these files costs time, never correctness - a missing or rejected snapshot is a cold scan, a lost journal a rescan
of what it held, a lost ProjectId a cold start; the scan, rules and Analyze rebuild everything from `Content/`.
Snapshots are written from a view on a worker after 1000 records, after 60 s idle and at a clean exit.

### Registry service (`Assets/AssetRegistry.h`)

`CAssetRegistry` keeps the registry store (`Assets/Registry/RegistryStore.h`) current from the disk. `CEngine` owns one in
Debug and Development hosts with a project, headless too (`GetAssetRegistry()`; null in Release, in tool hosts, in the
project manager and with `[Assets] Registry=0`); game code never reaches it. Start-up (design §5.4): `CEngine` resolves
the mounts once (`CMountTable`: `/Engine`, `/Project`, `/Plugin.<Name>`), adds every mount's content and rule-file
directory to the watcher and installs the table for `paths::ToMountPath`; the registry reads the mount rule files,
subscribes to the watcher (changes are buffered until the initial scan is complete), loads the snapshots on a worker
while every mount is stat-scanned, and turns each scan into deltas of at most 256 files on a worker. A file is analyzed when it is new or
changed, has no record or a `StaleTags` one, or its `AnalyzeStamp` or importer changed; a file whose rule chain changed
(its folder's or an ancestor's `Assets.yml`, its mount's `Config/Assets.yml`, the engine's for every other mount, or a
snapshot whose `RulesBasis` differs) is re-evaluated in memory; anything else costs a stat. `RULE002` is computed per
folder once companions are known; a record that starts or stops naming a companion in a folder its scan did not cover
queues a scan of that folder, which recomputes its `RULE002`. The editor and the dev Launcher wait for the initial scan before their first
`BuildWorld` and log `[assets] initial scan: <files> files, <records> records, <ms> ms; BuildWorld waited <ms> ms
(analyzed <a>, hashed <h>)`. `Pump` (in `BeginFrame`, right after the worker completions) commits at most 2 ms of
deltas per frame; panels poll `GetRevision()`, engine systems may subscribe to `OnAssetsChanged`.

Writes are the lock holder's alone. The editor (`SEngineDesc::bAssetWriter`) takes `<project>/Local/Assets/Owner.lock`
unless the run is read-only; holding it, the registry writes `Local/Assets/AssetRegistry.snapshot` (`/Project` and its
plugins) with its journal, `Rules.cache` and `ProjectId`, the snapshot after 1000 records, after 60 s idle and at a clean
exit, and its lifecycle repairs metas (below). Without the lock nothing is written at all. `/Engine` lives in `<engine>/Local/Registry/Engine.snapshot`, written
only by an engine-owner registry (`--engine`); an editor analyzes engine files changed since then in memory at every
start and says so once. A writer's `Flush` fences the watcher only in the mounts it owns, so the editor writes no fence
file into `/Engine` either; of `/Engine` it waits only for what the watcher reported already. AssetTool's `scan` takes
the lock like any writing verb, and its other registry verbs never take it and write nothing.

**A second process (M4, §5.9).** Who writes is decided once, before a registry exists, by
`assets::ClaimAssetOwnership` (`Assets/Meta/AssetRepairs.h`), which the editor (`CEngine::StartUp`) and AssetTool share.
A run that is read-only by request (`--ci`, `-Assets:ReadOnly=1`, `migrate --dry-run`, every scripted run but the smoke
test's file edits on a scratch copy, the runtime and the dev Launcher) never asks for the lock and always runs. The
scratch copy's smoke run (`-Editor:SmokeTest=1 -Editor:SmokeTestEdits=1` with a project and a `-Render:ShaderRoot` both
outside the engine checkout, `CAutomation::IsScratchEditRun` in `Engine/Automation.h`) is that copy's writer, never
`/Engine`'s, and exits at once, with no dialog, when another process owns the copy. Any other takes the owner root's
`Owner.lock`; while another process holds it, a second writer does not start: the editor shows one message naming the holder ("close it, or set
`Safety.GuestProcesses: true`") and quits, and AssetTool exits with code 4. With the opened project's `Safety.GuestProcesses: true` (the engine's file for `--engine`
runs; the switch governs a process, so no other file sets it) the second process runs as a **read-only guest** instead:
it loads, scans and reports like a read-only run, writes no meta, snapshot, journal or `BuildState`, says so (the
editor in its title, AssetTool on stderr), and a `META001` it reports names the owner, who creates the meta. `lock` is
never a guest. A writer rolls an operation its roots' `OpJournal.log` holds unfinished forward as soon as it has the lock
(`TakeOwnership`), before anything is scanned, through the mounts' providers, which it detects for that alone (only
when such an operation waits, so an ordinary start spawns no git there); one it cannot finish is named. A process
without the lock rolls nothing forward and says one waits (`DescribeUnfinishedOperations`: every root, whatever the role).
Both hosts hand the governing rule file's `Safety.BuilderSourceGuard` to the builder guard at start-up
(`ApplySafetyOfRuleFile`): the opened project's, the engine's for `--engine` or with no project.

**Engine-owner mode (M4, §4.7).** `/Engine` is owner-writable when the opened project lies inside the engine checkout
(`Templates/*`), with `FRT.bat assets <verb> --engine` (no project: that run owns `<engine>/Local/Registry`), or in the
editor with `-Assets:EngineWritable=1`; it stays read-only for an engine that external projects use. The project's
writer in that mode (its lifecycle repairs: `ClaimsEngineOwnership`) takes `<engine>/Local/Assets/Owner.lock`
best-effort, after its project's, so while it is open a writing `--engine` run exits with 4: without it (another
in-engine editor has it) `/Engine` is read-only for this process, one line says who holds it, and the project still opens; only the
project's own lock makes a second writer exit. With it the editor passes `SAssetRegistryDesc::bEngineMetasWritable`,
and the lifecycle repairs metas in `/Engine` through a second `CAssetWriter` on the engine root, with that root's own
`OpJournal.log` (rolled forward at start-up), and nothing more: no `Engine.snapshot`, no journal of `/Engine`, no fences
there, so `/Engine`'s `METAS` copies are as fresh as the last `--engine` run. AssetTool never owns both: `--engine` runs
act on `/Engine` alone, other runs on `/Project` and its plugins.

### The editor's Assets panel

`Editor/Source/AssetsPanel.h`: a search box in the query syntax, the positioned error when the text does not parse or
compile, and a table of the matches (Name, Type, Path, Size, Problems) with a default limit of 500. It polls
`GetRevision()` once per frame and runs its query again when the text changed, or when the revision moved while it is
visible; its rows are copied text, never record ids. It only reads. The default layout docks it beside Stats; View >
Assets toggles it. The smoke test checks it against the registry's own answer for `type:Texture`.

### The editor's Asset problems panel (M4)

`Editor/Source/AssetProblemsPanel.h` draws, and `Assets/Meta/ProblemList.h` (Core, so Core-Test checks it) makes, one list
(design §12.8): `RunValidation`'s, the function `validate` prints, over every mount of the editor's registry (`validate` and
`validate --engine` together), on a worker over a view the registry handed out - when the panel first draws, when the
revision or the lifecycle's fix-its and providers (`SAssetRepairsStats::Changes`) moved while it shows (at most once a
second), and while an applied fix waits for a list, shown or not. Its options are `validate`'s but for the providers: the
lifecycle's, so no VCS is asked (no `VCS003`) in a process that writes nothing (a read-only editor, a guest: they detect
no provider) or before detection settles. Each problem gets its asset
(a meta's problem goes with its source, a sub-entry's with its file) and that asset's type; the list is grouped by asset
in the registry's order and filtered by severity, code (its start, any case), type and folder (under the fold). Each
entry has **Go to** (the Assets panel lists the asset by `$Path=`, a folder or mount root by `dir:`, a file without a record
by its folder; the inspector target is M6's), **Open** (the file the problem names, relative to its mount's owner root,
else the asset; disabled while it does not exist, and **Show** in Explorer instead for a file the shell would run) and **Copy line** when it has a line. Fix-its are not stored on diagnostics: `GetProblemFixIts` asks
`CAssetRepairs::GetFixIts`, so a button applies the lifecycle's own action through `ApplyFix` - create a meta, restore the
verified copy, relink an orphan, rename a meta to its source's case, re-GUID a copy (with its whole set), keep A alias B,
retry what the VCS refused (VCS002), and for a lost meta "create it again" (its GUID) or, after a meta swap, "a new GUID".
An action is disabled, with the reason, in a mount the process does not write (read-only, a guest, `/Engine` outside
engine-owner mode). Shown and copied, never run: the provider's restore command for a lost meta (Git: `git checkout --
<meta>`; never after a meta swap or for a new file an orphan matches, where it would duplicate a GUID or name a file the VCS
never had), its add command for the untracked half of a `VCS003` (`File` names that half; absent under None, since FRT never
stages), and `"<engine>\FRT.bat" assets fix --engine` for a `META001` on `/Engine` in a process that does not own it.
`VCS004` is a message (`git lfs pull`). In the live editor `ApplyFix` only queues and returns a ticket: the lifecycle says
when the fix ran and whether it was refused or not applied (`CAssetRepairs::GetFixResult`; a queued fix never times out),
and once it ran the lists after it say fixed (the problem is gone) or no change (still there in a list made 30 s later).
In a process that writes nothing (read-only, a guest) the panel lists and offers commands, and applies nothing. PATH
renames, reference relinks, hints and aliases are M11's. The smoke test checks that the panel lists exactly what
`RunValidation` returns with the same options (`[smoke] ok the Asset problems panel lists ...`, which `FRT.bat verify`
requires), that a `META001` on `/Engine` offers the `assets fix --engine` command, that a read-only editor can apply none
of its fix-its, that it draws its rows, the code filter, and Go to; it captures the panel.

## Version control (M4, `Assets/Vcs/`)

FRT is not a version-control client: it never commits, syncs or reverts. `IVcsProvider` (`Assets/Vcs/VcsProvider.h`) is what
it needs from one: writes the VCS accepts, an asset and its meta kept together in every move and delete, and knowing when
the VCS is rewriting the tree. One instance per mount, from `MakeVcsProvider` (`[AssetsEditor] VcsProvider=Auto|None|Git`); `CVcsProviderSet`
(`Assets/Vcs/VcsProviderSet.h`) detects and owns one per mount, at once in a tool host or on a worker in a live one, and
the meta lifecycle's repairs (below) are their only writer. The editor that owns its project detects one per mount, `/Engine`
included, on a worker at start-up (`CEngine`); AssetTool's `status` and meta verbs create them for their own run (below).
A read-only editor, a guest, a scripted run (but the smoke test's scratch copy, its writer), the dev Launcher and every
other verb create none and spawn no git: a mount without a provider is `None` to them. A host rolling an unfinished
operation forward at start-up detects its providers on its own thread, and only once it holds that root's lock
(`SAssetOwnershipRequest::DetectProviders`): a refused second writer or a guest spawns no git for it.

| Call | `CNoneVcsProvider` | `CGitVcsProvider` |
|---|---|---|
| `Detect` | always | `git rev-parse` from the mount root, so worktrees, submodules and `.git` files resolve to their own git dir; a `.git` without `git` on `PATH`, or one git refuses (dubious ownership), gives `None` and one warning |
| `IsBusy` | never | stats only: `index.lock`, `HEAD.lock`, `MERGE_HEAD`, `REBASE_HEAD`, `CHERRY_PICK_HEAD`, `rebase-merge/`, `rebase-apply/` in that git dir |
| `IsRewritingTree` | never | `index.lock` or `HEAD.lock` only, so an open merge holds repairs but never hot reload. Held 30 s without a break logs one warning naming the file; it is never deleted |
| `CheckOut` | a read-only file fails with `ReadOnly`, naming it; the attribute is never cleared | the same |
| `Add` | nothing | nothing: new files are left for the user to stage |
| `Move` | `platform::MoveFileNoReplace`, and one warning on a case-only rename | the same, with `git mv` for a case-only rename of a tracked file, which `core.ignorecase=true` would hide; refused over `index.lock` it returns `Busy` |
| `Delete` | `platform::MoveToTrash` | the same |
| `GetCommittedFacts` | all false | `git ls-tree -r -z --full-name --name-only HEAD`; an unborn HEAD (`FRT.bat new` commits nothing) means nothing is committed |
| `StatusOf` | all `Untracked` | `git --no-optional-locks status --porcelain=v2 -z --no-renames --untracked-files=all --ignored -- .`; an ignored file is `Untracked` |
| `GetStaged` | empty | `git diff --cached --name-only -z --no-renames --relative` |

A `Move` batch moves as one: what would be refused anyway refuses before anything moves, and what moved moves back if a later
file fails. Git commands list the mount once and look the batch up, so a command line never grows with a batch. Each runs
through `platform::RunToolProcess`, blocking, on a worker; only `IsBusy` and `IsRewritingTree` run on the owner thread. The
design's long-lived `SpawnToolProcess` and `IToolPipe` wait for their first client, the Blender bridge (M9c).

**The pre-commit hook** (`Tools/GitHooks/pre-commit`, bash under Git for Windows) is opt-in per clone: `FRT.bat setup --hooks`
sets `core.hooksPath` (relative for an engine inside the clone, absolute for a shared one), `--hooks=off` clears it, `FRT.bat
new` only prints the hint, and `git commit --no-verify` skips it once. It parses no YAML and reports everything at once:

1. pairs: a staged meta whose source is neither staged nor tracked, and a staged delete or rename of a source whose tracked
   meta did not go along (`META002`). A new source without a meta is left to check 6, which knows the rules and companions;
2. duplicate GUIDs in the index (`META003`);
3. exactly one valid `Guid:` line per staged meta, and no conflict markers (`META004`);
4. on staged names, case-only collisions in the index and between index and disk (`PATH001`), and illegal names (`PATH002`);
5. `FRT.bat check-builders`, in an engine checkout, when a staged path is under `Core/Source` (`BUILD003`): a builder's
   sources lie wherever its code is (the shader builder's are in `Core/Source/Graphics`), and the check is one process;
6. `validate --staged --quick --ci` through `AssetTool-Development-Default.exe` when it is built, never `FRT.bat assets`,
   which may start a build. Skipped with a message when the tool is not built, or was built before it knew the verb; any
   other refusal of the tool, a usage error included (a project it cannot use), refuses the commit. It validates
   the `.frtproject` at the top of the work tree, else the engine in an engine checkout, else each `.frtproject` in the
   index (a project in a folder of its repository), one run each. The tool inherits git's `GIT_INDEX_FILE`, so it sees
   what `git commit -a` is about to commit; the hook names the committing repository in `FRT_HOOK_TOPLEVEL`, and the Git
   provider of any other repository (an `Engine/` submodule, a shared engine) runs its git without that variable, which
   would otherwise read as an empty index there.

A file name is always quoted in the commands a person is shown to paste (`GetRestoreCommand`, `GetAddCommand`), so a name
with `&` in it stays one argument. A case-only rename of a tracked file is `git mv -f`, since git's own same-file test folds
ASCII only; the batch has already made sure the target is that very file. A repository nested in the mount and never
committed there is one untracked entry to git, and every file below it is `Untracked`.

## The meta lifecycle's runtime (M4, `Assets/Meta/AssetRepairs.h`)

`CAssetRepairs` drives the pure lifecycle (`Assets/Meta/Lifecycle.h`) from a registry: `SAssetRegistryDesc::Lifecycle`
(`SLifecycleDesc`) gives it one, owned by the registry (`GetRepairs()`). Every host passes one: `CEngine` a live one
(`bLive`, its settings from `[AssetsEditor]`, the project's salt), repairing for the project's writer and reporting
for every other host (a read-only editor, a guest, whose `META001` names the owner, every scripted run but the smoke
test's scratch copy, the dev Launcher); each AssetTool run its own (below). Null, in tests and benchmarks that predate
it, means none at all. With one:

- **When it runs.** Every finished pass hands it the paths its deltas touched; the time of the next tick after that is
  the path's event time (kept in memory only, so a restart counts from then). Of the initial scan's passes only the
  paths whose time decides something are kept: files without a settled pair, metas spelled in another case and the
  members of a duplicate group, never every `Paired` file of the tree. A run starts when something was touched, a pass
  read changed rule files or re-evaluated a mount (a `Safety:` switch changes no row), a delay ran out (the last
  result's `WakeAtMs`) or a VCS hold ended, and only while the registry is quiet: the initial scan complete, nothing
  queued, scanning or analyzing. It runs on a worker against an acquired view, one mount at a time:
  the facts as `Lifecycle.h`'s contract asks (only the rows not `Paired`, those touched, metas whose case differs and
  duplicate groups; orphans and broken metas are read from their files, a hash the lifecycle asks for is computed then
  and committed into the file's row, committed facts are asked of the provider only when a run needs them). Its states,
  hashes and `Lifecycle` problems (with their `File` and the rule files' `Validation:` severities) come back as one
  delta against that view, committed only if the registry is still at that revision; otherwise the run is dropped and
  made again. A `SLifecycleDesc::bLive` registry ticks from `Pump` with the wall clock; otherwise a caller ticks
  (`Tick`, `Settle` with its own clock: tests) or runs a command.
- **Repairs.** A writer with `bRepairs` (only on the mounts it owns: `/Project` and its plugins, and `/Engine` too in
  engine-owner mode through a writer of the engine root; or `/Engine` for an engine owner) plans each repair into one op, and every re-GUID of one result into a single op (`PlanRepairOp`: the
  losers' `SubAssets` `$asset`s rewritten through the batch's map, their `Aliases`, `FormerPaths` and `FormerNames` left to
  the winner, which keeps the GUID they redirect to; random GUIDs minted for the stand-ins
  when `Safety.DerivedAutoGuids` is off; a restore writes the verified `METAS` bytes as they were; a relink and a case
  fix are moves). The ops of one result run in one worker job through a `CAssetWriter` on the owner root, after
  `IsBusy` is asked once more and their expected writes are registered with the watcher (`ExpectWrite`); the touched
  folders are then rescanned. It is level-triggered: the rescan of a repair's own write proposes nothing. Each write
  carries what its planner read (nothing, for a create; the bytes, for a rewrite), and the writer checks it right before
  writing, on the first run and in a roll-forward alike: a meta that a sync delivered or a person edited after the plan
  is never written over. A whole op whose file changed is then given back unwritten, a step that changed during a run
  or before a roll-forward is skipped with a warning and never added to the VCS, and a create cannot replace a meta
  that lands at the last moment. No write makes a folder, and a meta whose source is gone is not written. An op left
  half done, or planned before the writer finished an earlier one, is planned again by the next run rather than run
  over what the roll-forward wrote; none of these is a refusal. A meta beside
  a file the rules exclude is no asset's, so it takes part in no duplicate group.
  `SAssetRegistryCounters::RepairsApplied` and `RepairsRefused` count them.
- **Holding for the VCS** (`[AssetsEditor] WaitForVcs`, default on): no automatic repair while the provider of a mount
  the process repairs `IsBusy`, and for `VcsSettleMs` after (another mount's VCS, a read-only `/Engine`'s checkout, holds
  nothing); `IsBusy` is polled every 100 ms only while something waits on that hold. Whatever the switch,
  nothing automatic is written while the watcher holds events back (a burst, an overflow's rescan): the registry looks
  quiet then, but its tree is not the final one. A person's fix is a command, held by neither. A live host that
  detects its providers on a worker sets `bProvidersPending` and holds everything until the last `SetProvider`. The
  watcher's own hold (`SetVcsBusyQuery`) is the narrower `IsRewritingTree` of every mount, so an open merge holds repairs
  but never hot reload; with `WaitForVcs=0` it is left unset.
- **What it remembers.** A repair the VCS refused stays a proposal with `VCS002` and is not tried again until an event on
  its path or an explicit fix, so a read-only file cannot spin. Every repair of a refused op shows its own `VCS002` (the
  losers of a dedupe set alike), and they are retried together: an event on any one of them, or the fix-it of any one,
  proposes the whole set again. A meta created beside a source `StatusOf` calls tracked,
  when the meta itself is untracked, is `VCS003` ("generated locally: commit it", with the provider's add command), asked
  in one `StatusOf` per provider for all the metas of a round; adding or committing it changes no file here, so the kept
  pairs are asked again after 30 s, then twice as long each time (up to 16 min), until the VCS tracks them alike or the
  source goes. A refusal ends with an event on its path or on a folder above it (a removal). A meta whose bytes hold CRLF is `VCS001`, checked once over every `METAS` copy and then
  on each meta touched.
- **For verbs and the panel.** `RunNow(SRepairOptions)` runs without delays and blocks until it has applied (the
  options' `Kinds`, `Paths` and `bDryRun`; the run's mounts by default), rescanning after each round. A repair of a kind
  the run leaves out is reported as one this process cannot make, with its fix-it, never dropped. `GetFixIts`
  gives the fix-its of a problem (the last run's, and the refused repair of a `VCS002`), `ApplyFix` applies one (a re-GUID
  together with the other losers of its batch), blocking in a tool host and queued in a live one. "Keep A, alias B" reads
  the meta again first and rewrites it only while it is still the broken meta it was offered for.
- **Start-up.** The registry's `Start` rolls an operation the owner root's `OpJournal.log` holds unfinished forward
  before anything is scanned. `TakeOwnership(Root, Mounts, Providers)` is the hosts' form of it, which `CEngine` and
  AssetTool use through `ClaimAssetOwnership`: the `Owner.lock`, then the roll-forward, through providers detected then
  only when `HasUnfinishedOperation` says one waits; a process that does not get the lock rolls nothing forward and
  reports the unfinished operation. The editor's writer then detects its providers on a worker (`bProvidersPending`),
  and the Stats panel shows each mount's provider and `RepairsApplied` / `RepairsRefused`.

Known exception until M7 and M10: the create-on-load writes of `CMaterialLibrary` and `CInputActionLibrary` do not go
through the provider. The file one of them creates is an ordinary new file to the lifecycle, which gives it a meta on the
owner like any other.

## The meta verbs (M4, `Assets/Meta/Validation.h`, `MetaVerbs.h`, `Migrations.h`)

`validate`, `fix`, `new-meta`, `migrate` and `resave` (the verb table below) are thin AssetTool callers of Core: each
opens its registry with a lifecycle of its own (`SLifecycleDesc`, providers detected for the run's mounts, no live
ticks) and acts through `CAssetRepairs::RunNow`, so a meta a verb writes is derived, journalled and checked out exactly as
an automatic repair's. They are commands a person runs: no creation delay, no VCS hold, `AutoRepairMetas` and
`OrphanPairing` ignored; `DerivedAutoGuids` still picks the GUID kind and `AutoResolveDuplicates` whether `fix` dedupes.
Each acts on the run's own mount set, `/Project` and its plugins, or `/Engine` with `--engine`, never both. Every other
registry verb (`scan`, `status`, `query`, `refs`, `referencers`, `explain`, `list-rules`, `dump`) runs the lifecycle
once after its scan, over every mount and reporting only, with no provider but `status`'s, so it sees the `META`
problems gathering raises (a metaless file is a `META001` error: `scan` and `status` exit with 1); `scan` writes no
meta.

- **Who runs.** All five take the mount's writer lock unless the run is read-only, so beside another owner they exit with
  4. Only `validate` runs as a read-only guest when `Safety.GuestProcesses` says so (§5.9), reporting, and its META001s
  name the owner. The others, and `validate --fix`, exist to write: as a guest they exit with 4, and with `--ci` or
  `-Assets:ReadOnly=1` they are a usage error. `migrate --dry-run` is read-only by request itself: no lock, so it runs
  beside an owner. `validate --fix --quick` is one too: a
  meta's GUID needs its content's hash.
- **`RunValidation`** is the one list `validate` prints and the Asset problems panel shows: every problem the
  registry holds in scope (Scan, Rules, Unclaimed, Analyze, Registry, and the Lifecycle's as the verb's report-only or
  repairing run committed them), plus the rules only it runs: `META005`; `META007`; a meta's unknown keys, a newer
  `FormatVersion`, settings its effective importer has no field for or no value of, and an `Importer:` pin this build
  does not know (warnings with no code, which `ResolveFile` ignores: the rules' value stands); `VCS001` for rule files and
  text sources (`--quick` reads none but the paths asked for); `VCS003` over every pair, one `StatusOf` per provider (its
  `File` the half the VCS does not track);
  `MIG001` per repository root. The rule files' `Validation:` severities apply to these too. `--ci` is clean with no error
  and no warning, so the code-0 warnings of a meta fail it as well; infos pass (`REF007` until M7).
- **`--staged`** takes the paths from `IVcsProvider::GetStaged` (Git: what the pre-commit hook is committing, through
  the inherited `GIT_INDEX_FILE`); a problem is in when its subject is at or below one of them, or is its meta or source.
  `MIG001` is the root's: it is in when something staged lies in one of that root's mounts, so a commit of code alone is
  never refused for it, and content committed into a repository behind is.
- **`migrate metas`** creates a meta for every file of class Asset or Raw that has none (a broken meta counts as one: it
  is never reminted), with `MakeCreateMetaGuid` over the mount's salt, its folded mount path and its content: two people
  who migrate one commit write the same bytes. It records itself only when no such file is left without a meta (a
  read-only file, a Git LFS pointer, a file an orphan meta matches, a lost meta: `fix` or `new-meta` settles each), so a
  run that crashed, or left one, is run again: it skips what exists and then records. `Schemas/Migrations.yml` lies
  outside every mount, so it is written after the op with `WriteFileAtomic`, after the provider's `CheckOut` of an
  existing file. Its format, through the canonical emitter, with no date, user or engine version so it merges as text:

  ```yaml
  FormatVersion: 1
  Applied:
    - metas
  ```

  `validate` reports a root whose file lacks a required step as `MIG001`, and so do the hosts' gate below.
- **The `MIG001` gate** (design §14.8; `FindMigrationGap`, `IsMigrationGateOn` in `Migrations.h`). A root behind the
  engine (its file missing, malformed, or without a step this engine has) is written by nothing but its migration.
  `CEngine::StartUp` opens a project behind read-only, as `[Assets] ReadOnly=1` does (no lock, no registry file, no
  meta; `Content/` is not covered yet: the material library still creates a model's missing
  `<model>_mat<N>.frtmat.yml` and upgrades an old-format material until its façade over the asset manager replaces it,
  M7 or later), says so in an asset writer's window title, logs `MIG001`, and keeps engine-owner mode out of an engine
  behind. Only the interactive editor asks, in a blocking
  dialog: Yes opens the project manager with Migrate and Open (`FRT.bat assets migrate --all`, then the editor), No
  opens the project read-only; a scripted run, a headless host and the launcher only log. The project manager marks
  each project of this engine that is behind and offers Migrate and Open for it. AssetTool, for the run's own root
  (the project's, the engine's with `--engine`): a run that would write ends before the lock with exit code 1 and
  `MIG001` (`{"error":...,"exitCode":1,"code":"MIG001",...}` with `--json`); `migrate` runs, and so does `lock`, which
  writes nothing; a run that writes nothing (`--ci`, `-Assets:ReadOnly=1`, `migrate --dry-run`, `validate` without
  `--fix`) runs, and `scan` and `status` count `MIG001` as an error among their problems (exit 1): `scan` in its one
  object (`migration`), `status` as one more problem by code. A project that
  `FRT.bat new` makes has its template's `Schemas/Migrations.yml`, so it is not behind. The gate is `[Assets]
  MigrationGate`, default `MigrationGateDefault`, **on** since the engine and both templates commit their files (M4);
  `-Assets:MigrationGate=0` turns it off for one run. A test's scratch project that runs a writing verb carries a
  current file (`Core-Test/Assets/Meta/MigrationsTestUtils.h`).
- **`resave --metas`** rewrites in ops of at most 256 metas each, through the writer of the mount; a canonical meta is not
  touched or checked out.

## One file watcher (M2)

Every system that reloads files in dev builds learns about changes from one watcher instead of polling timestamps:
materials, input actions and maps, shader programs and the editor's input files. Release builds watch nothing.

### `platform::IFileWatcher` (`Platform/FileWatcher.h`)

Reports what the OS says happened under armed roots as portable events: root, path relative to it (UTF-8, `/`),
kind (`Added`, `Removed`, `Modified`, `RenamedOld`, `RenamedNew`, `Overflow`, `RootLost`), entry type and file identity
(the bytes `platform::Stat` reports, for pairing moves). It decides nothing. Windows: one I/O completion port thread
for all roots with `ReadDirectoryChangesExW` (extended information, 64 KiB double buffers), which only re-arms, parses
and queues; a completion with 0 bytes is an `Overflow`, the leading `REMOVED` of a case-only rename is dropped, a
deleted or renamed root is `RootLost` and is armed again once it is back. Opening directories and asking for a root's
final path - calls that block on a share whose server is gone - run on a second, low-priority poll thread (and Arm's on
the caller's thread), never on the completion thread. `platform::ProbeFileWriters` tells whether a writer still holds
a file.

### `assets::CFileWatcherService` (`Assets/FileWatcherService.h`)

Portable, on the owner thread (the engine's main thread). **Level-triggered**: an event only marks a path dirty; when
the path has settled, its state on disk decides what subscribers are told, so a lost event costs a rescan and never a
wrong answer.

| Rule | |
|---|---|
| Settling | a path is told after 200 ms without events, at most 2 s after its first; a removal waits `[AssetsEditor] RemoveGraceMs` (1000) longer |
| Coalescing | `Removed` then `Added` is `Modified` (git's unlink and write, atomic saves), `Added` then `Removed` is nothing |
| Moves | the two halves pair by identity within 150 ms into one `Moved`; a case-only rename is a `Moved` |
| Directories | one moved or copied in is enumerated once it settles (every file `Added`); one renamed is one directory `Moved` from where it was when the window began; one removed is one directory `Removed` (then an `Added` if a file took its place); without entry types (FAT, ReFS, shares) the disk is asked, a directory's own `Modified` is dropped, and a removal is told as a directory `Removed` |
| Bursts | more than 256 events without a pause hold everything until `[AssetsEditor] VcsSettleMs` (1500) of quiet, then one batch; `SetVcsBusyQuery` is the seam for M4's VCS provider |
| Overflow, lost root | one `Rescan` of the root: compare what you hold with the disk, reload only what differs |
| Writers | a file a writer still holds waits (back-off up to 2 s, a warning after 10 s) |
| Self-writes | `ExpectWrite(path, hash, size, deadline)`: an echo with exactly those bytes is not told |
| Verification off the frame | with a worker pool (`SetWorkerPool`; `CEngine` passes its own) the disk checks of settled paths - stat, writer probe, the self-write hash, and the stat that names an entry where the platform gives no entry types - run on the workers at High priority, one group per `Pump`, and a later `Pump` applies each group whole, so a burst's batch stays one batch; a path that changed meanwhile is checked again, a group the pool's drain drops is sent again, and `Flush` sends the checks it covers as a group of its own, waits at most 0.1 s for that group alone (not for older ones) and resolves inline what the pool has not got to by then. Without a pool (tests, tool hosts) they run inline |
| Subscriptions that do not verify | `SWatchFilter::bVerify = false` (for a subscriber that compares with the disk itself, such as the asset registry): told each settled path at once with the kind its events say - no writer probe, no self-write check, and a `Rescan` where a removed directory has something back in its place |
| Cap | at most `MaxChangesPerPump` (4096) settled paths are resolved per `Pump`, oldest first; a held release, a `Flush` and the files found below one directory that appeared are never split. With a pool the cap counts the checks a `Pump` applies and those still in flight: finished groups are applied only up to the cap (never split), and no new checks are sent while the pool holds a cap's worth |

`AddMount(directory)` before anything subscribes: a subscription inside a mount is watched from one root at the mount
(`CEngine` mounts every mount's content and rule-file directory, plugins included), since an open directory handle
keeps every folder above it from being renamed or moved, and a root per loaded file's folder would pin them all.
`Subscribe(directory, { Prefix, Extensions }, callback)` (the directory need not exist yet), `Unsubscribe`, `Pump()`
(once per frame: `CEngine::BeginFrame`), `Flush(directory)` (a fence file `.frt-fence-<pid>-<n>` in every root that
watches the directory - roots overlap when a directory was subscribed before one above it: everything older is told
now, and a root that loses events meanwhile is told its `Rescan` at once; for tests and scripted steps), `SetEnabled(on, onlyUnder)` (optionally for the roots in or above some directories
only), `GetStats`. `IsFileAffected(change, file)` is the rule loaders share: a change of the file's path, or of a
directory above it, and never a removal; `CAffectedFiles(changes).Contains(file)` answers the same for many files in
one lookup each. Paths compare as NTFS compares names (`MakePathKey`, `Containers/StringMap.h`: its upcase table, not
Unicode folding), through `platform::NormalizePath` and `platform::GetRelativePath` (`Platform/FileSystem.h`). `CEngine::GetFileWatcher()` owns one in Debug and Development (headless too;
null in Release and in tool hosts), its disk checks on `CEngine::GetWorkerPool()`. `[Assets] Watch` (default 1) turns it off.

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
(the asset system writes nothing: no writer lock, so no registry file and no meta; its lifecycle only reports)
(`CAutomation::IsScriptedRun`, `ApplyScriptedRunRules`). The one exception is `-Editor:SmokeTestEdits=1` on a project
outside the engine checkout, which is that copy's writer. A
smoke step that needs the watcher turns it on for its own directories only (the input overrides' `Local/SmokeTest`; the
scratch copy and its shader copy) and flushes it after writing, so its result never depends on timing and nothing of the
checkout it runs in reloads behind its back. With `-Editor:SmokeTestEdits=1` the smoke test also edits files - a
material, which must reload, and a shader include, which must rebuild exactly the programs that read it - and adds a
metaless 1x1 PNG, `Content/SmokeTest<Provider>.png`, whose meta the lifecycle must create through the copy's provider
and pair with its record (`[smoke] ok the lifecycle created ... through the <Provider> provider`), but only on a
project outside the engine checkout and with the shaders read from a copy, `-Render:ShaderRoot=<dir>` (dev builds).
`FRT.bat verify` runs it on a scratch copy of `Templates/Demo` made with `FRT.bat new` in `%TEMP%` (a git repository:
Git, and `git status` must list the PNG and its meta untracked, since FRT never stages), then once more on the same
copy with its `.git` deleted and nothing rebuilt (None).

## Tool host, worker pool, derived-data cache, shader cache (M1)

### AssetTool and `FRT.bat assets`

`AssetTool.exe` is the asset system's command line, a console host beside the editor and the launcher (Debug,
Development, Development-Headless; Release cannot load a game module, so it never runs tools). `FRT.bat assets <verb>`
runs it (building it first when it is missing or stale) for the current project, or for `/Engine` with `--engine` -
run from the engine checkout's own `FRT.bat`: a project's `FRT.bat` always names its project, and AssetTool refuses
`--engine` with a project (exit code 2).

| Verb | Does |
|---|---|
| `help [verb]` | lists the verbs, or explains one |
| `newguid [n]` | fresh GUIDs for builder versions and built-in assets, as text and as a C++ initializer |
| `lock` | holds the mount's writer lock until Enter (for tests and scripts) |
| `ddc stats`, `ddc gc [--max-gb=<n>]`, `ddc verify [--ddc=<dir>]` | the derived-data cache: counts, LRU trim (a dry run with `--ci`), re-check and delete corrupt files |
| `scan [--engine]` | scans the mounts into the registry and, under the writer lock, writes the snapshot (`--engine`: `Engine.snapshot`); with `--ci` writes nothing. It never writes a meta: its lifecycle only reports, so a metaless file is a `META001` error and stays `PendingMeta` (the meta verbs below write them). Exit 1 on an error-severity problem |
| `status` | problems by code, severity and source; each mount's VCS provider (detected, so git runs once per mount) and its files waiting for a meta (`PendingMeta`); unclaimed files by folder and extension; the scan state; the DDC's size. Exit 1 on an error-severity problem |
| `query "<text>" [--columns=a,b] [--limit=<n>] [--rows]` | the records a query matches (default columns `$Name,$Type,$Path,$Size,$Problems`; several arguments are one text; a term with one dash is text, so `query -label:Debug` and `query "-label:Debug type:Model"` work, and after `--` every argument is; `--rows` notes that rows arrive with M5) |
| `refs <asset>` | the edges an asset declares, with flags and field paths (`<asset>`: a mount path, `#label` for a sub-asset, or a unique name, resolved as `dep:` resolves it) |
| `referencers <asset> [--transitive]` | the edges that reach an asset; with `--transitive` every record that reaches it (`ref:~X`) |
| `explain <file>` | a file's importer, class, settings, cook rule, chunk and labels (the meta's marked authored), each with the rule file and line or the meta that set it (`Content/Art/Assets.yml:4 (rule 2)`, `Content/Art/Rock.png.meta:5`), the rule files consulted, notes, its meta (file, state, GUID), and the aliases and former paths that resolve to its record; `<file>` is a mount path or a disk path (absolute or relative to the working directory) |
| `list-rules` | every rule file of the mounts with its normalised entries; with `--json` exactly Rules.cache's lines (it writes no Rules.cache: an M3 deviation, only the lock holder writes it) |
| `dump registry [--text\|--json] [--mount=<name>,...]` | the deterministic registry dump (design §5.3; the golden files), of every mount or the ones named |
| `validate [paths] [--staged] [--quick] [--fix] [--ci]` | design §13.4 over the run's own mounts (below): the registry's problems, the lifecycle's, and the rules only it runs. `--staged`: the files the VCS has staged (Git), a mount under `None` whole; `--quick`: hashes nothing; `--fix`: `fix` first. Exit 1 on an error, with `--ci` also on a warning (infos pass) |
| `fix [paths]` | `validate --fix` under another name: the safe repairs (create a missing meta, restore a verified copy, fix a meta's case, re-GUID duplicates while `Safety.AutoResolveDuplicates` is on), then what remains. Never a relink, "keep A, alias B" or a canonical rewrite |
| `new-meta <file...>` | a meta now for each file named, with the GUID automatic creation derives (not gated by `Safety.AutoRepairMetas`); a file that has a meta, or takes none, is an error, and then nothing is written. Exit 1 then |
| `migrate <step>\|--all [--dry-run]` | content migrations (design §14.8), each recorded in the root's `Schemas/Migrations.yml`. M4 has `metas`: a meta for every included file that has none, with reproducible GUIDs; recorded once no file is left without one (exit 1 until then). `--all` runs every step this engine has; a later step names its milestone. `--dry-run` writes nothing, neither metas nor the registry's own `Local/Assets` files: it is read-only by request, as `--ci` is, so it takes no lock and runs beside an open editor. Its preview is the run's: a file or meta with the read-only attribute, which the run leaves without a meta, is listed as remaining |
| `resave --metas` | every meta that parses and is not canonical (`META005`), rewritten as the emitter writes it, as its own commit; a broken meta is left alone, and so is one a newer engine wrote (a `FormatVersion` past this build's). `--references`, `--redirected`, `--type` and `--drop-unknown` arrive with M5 |

The registry verbs need a project or `--engine`. Each builds its own registry (snapshots read, an incremental scan,
deltas committed strictly in sequence order, the lifecycle run once, reporting, unless the verb runs its own) and
writes nothing unless it is `scan` holding the lock, or a meta verb (below). A query that does
not parse or compile, and an `<asset>` that names no record or several, exit with 2 and the positioned message. Their
`--json` lines (stable fields):

| Verb | Lines and fields |
|---|---|
| `scan` | one: `verb`, `mounts`, `owner` (`Project`/`Engine`), `writer`, `files`, `records`, `subEntries`, `folders`, `analyzed`, `hashed`, `reEvaluated`, `scanned`, `committed`, `loadMs`, `totalMs`, `errors`, `warnings`, `infos` (`errors` counts `MIG001`), `snapshot` (the file written, else ""), `migration` (while the `MIG001` gate is on and the root is behind the engine: its message, else "") |
| `status` | `{"kind":"scan"}` (`mounts`, `state`, `files`, `records`, `subEntries`, `folders`, `totalMs`, `analyzed`, `hashed`), one `{"kind":"mount"}` per mount (`mount`, `provider`: `Git` or `None`, `pendingMetas`), one `{"kind":"problems"}` per (`code`, `severity`, `source`) with `count` (a root behind the engine while the `MIG001` gate is on: `MIG001`, `error`, `Lifecycle`, 1), one `{"kind":"unclaimed"}` per (`folder`, `extension`) with `count`, `{"kind":"ddc"}` (`ddc`, `exists`, `error`, `contentBytes`, `totalBytes`, `files`), `{"kind":"summary"}` (`errors`, `warnings`, `infos`) |
| `query` | one per record: a field per column (`$Size` -> `size`, `$ImportStatus` -> `importStatus`, a tag key as named; counts as numbers, `$Labels` and multi-value tags as arrays), then `{"matched","shown"}` |
| `refs`, `referencers` | one per edge: `from`, `target` (as written), `resolved` (the record reached, else ""), `flags` (array), `field`, `state` (`Resolved`, `Unresolved`, `Ambiguous`, `Companion`); `referencers --transitive`: one per record, `path`, `type` |
| `explain` | one: `path`, `excluded`, `excludedBy`, `importer`, `class`, `quiet`, `cook`, `chunk`, each source as `{"file","line","rule"}` (`importerSource`, `cookSource`, `chunkSource`), `labels` (with `source` and `authored`) and `settings` (with `source`), `settingsTable`, `typeIncluded`, `chain`, `notes`, `record`, `recordType`, `meta` (`state`; with a meta also `file`, `path`, `guid`, `parses`), `aliases`, `formerPaths` |
| `validate`, `fix` | `fix`: one `{"kind":"repair"}` per repair (`action`, `path`, `to`, `guid` of a created meta); both: one `{"kind":"problem"}` per problem (`code`, `severity`, `subject`, `file`, `line`, `column`, `field`, `message`), then `{"kind":"summary"}` (`verb`, `errors`, `warnings`, `infos`, `clean`, `staged`, `quick`, `repaired`, `refused`) |
| `new-meta` | one `{"kind":"meta"}` per meta created (`path` of the source, `meta`, `guid`), the problems, `{"kind":"summary"}` (`verb`, `created`, `refused`) |
| `migrate` | one `{"kind":"meta"}` per meta created or, with `--dry-run`, to create (`path`, `meta`, `guid`: `random` with `Safety.DerivedAutoGuids: false`, `dryRun`), one `{"kind":"remaining"}` per file still without one (`path`), the problems, then per step `{"kind":"summary"}` (`step`, `created`, `remaining`, `recorded`, `dryRun`) |
| `resave` | one `{"kind":"meta"}` per meta rewritten, left broken or left newer (`path`, `rewritten`, `broken`, `newer`), the problems, `{"kind":"summary"}` (`verb`, `rewritten`, `canonical`, `broken`, `newer`) |
| `list-rules` | Rules.cache's lines (see Rules above) |
| `dump registry` | one per mount, rule file, folder, file, record and problem, `kind` first: `mount`; `rules` (`folder`, `level`, `hash`: the file named by folder and level as Rules.cache names it - a mount's `Config/Assets.yml` is the mount root with level `Mount` - since the registry knows no disk layout; `list-rules` gives its `file`); `folder` (`path`); `file` (`path`, `class`, `size`, and `meta` unless PendingMeta); `record` (`path`, `guid` when a meta gave it, `type`, `importer`, `status`, `stamp`, `cook`, `chunk`, `flags`, `primary`, `labels`, `tags`, `subEntries`, `edges` - `redirected` when resolved through an alias or a former path -, `companions`); `redirects` after a record that has any (`path`, `aliases`, `formerPaths`); `problem` (`code`, `severity`, `source`, `subject`, `file`, `line`, `column`, `field`, `message`) |

Options: `--json` (one object per line), `--ci` or `-Assets:ReadOnly=1` (read-only, never takes the writer lock),
`--project=<file>`, `--engine`, and the verb's own; any other argument that starts with `-` is a usage error, except a
`query` term with one dash (`-label:Debug`). `--` ends the options: every argument after it is the verb's, whatever it
starts with (`refs -- -Odd`, `query -- -json`). Exit codes: 0 success, 1 problems found, 2 usage error, 3 internal error, 4 the mount is
owned by another process. **One writer per mount**: a writing run takes `<mount>/Local/Assets/Owner.lock` (an exclusive
handle the OS releases when the process dies; its text names the holder), and a second writer exits with code 4 naming
the owner, unless the project's `Safety.GuestProcesses` (the engine's with `--engine`) lets it run as a read-only guest,
which warns on stderr and writes nothing (`lock` exits with 4 even then). The editor takes the same lock (see "A second
process" above).

The registry's golden dumps, `Core-Test/Assets/Golden/Registry-{Engine,Demo,Blank}.txt` (`/Engine`, and each
template's `/Project`), are compared by Core-Test (temp copies) and by `FRT.bat verify` (in place, on a tree git says
is unchanged there). A content change that changes a dump commits its golden file: `FRT.bat verify --update-goldens`
writes them again from `dump registry --text --ci` and shows the diff.

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
| `MoveFileNoReplace` (`Platform/FileMove.h`) | a durable rename that fails if its target exists, unless that is the same file in another case; `ReplaceFileAtomic` and `std::filesystem::rename` both overwrite |
| `RunToolProcess`, `FindOnPath` (`Platform/Process.h`) | a console tool run to its end with both streams captured and a timeout: no shell, no window, killed with its host (a job object); the executable found on `PATH` alone, never in the working directory |
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
- `FRT.bat new` copies a template with its metas: the template's name in a path becomes the project's, so `X.png` and
  `X.png.meta` are renamed alike (a name that contains the template's name too), and a meta's bytes are copied as they
  are but for LF (`.meta` is no templated extension: its GUID is never rewritten, so projects from one template share
  asset GUIDs, design §13.3). `Schemas/` comes along. `--no-generate` makes the project and its repository and no
  solution (`AssetsCommandTest` pins the pairs with it). `Cook.ContentId` is not written: that is M12's.
- `FRT.bat package` ships no meta: every tree it copies (`Content`, `Config`, the engine's shaders and fonts) skips a
  file whose name ends in `.meta`, in any case, and nothing of `Local/` is copied. The runtime loads by path and never
  reads a meta; ids reach packages through the cook (M12). Verify's package step fails on a `.meta` or a `Local`
  directory in a package.
- `FRT.bat verify` is the merge gate: builds, Core-Test, lints, the registry verbs with `--json`, the registry's golden
  dumps, a check that this file names every `FRT.bat assets` verb and every diagnostic code, `validate --ci` of the
  engine and both templates (full mode), the editor's smoke tests (in place, read-only, with the registry, Assets panel
  and Asset problems panel checks, and with file edits and a meta created on a scratch copy, through Git and then
  None), automation
  captures against baselines (raster byte for byte, path tracing within a tolerance), the Release package's captures,
  and a clean `git status` at the end; the summary lists each host's initial scan and BuildWorld wait. `--quick` runs
  the pre-commit subset; `--record-baselines` records new baselines in `Local/Verify/Baselines`, which belong to the
  machine that took them; `--update-goldens` rewrites the golden dumps and runs nothing else; `--nightly` adds the
  registry's 100k/1M benchmark suites in Release with `FRT_PERF_STRICT=1` (design §5.11's figures exactly).
