# FRTEngine2

A small experimental 3D engine built on Direct3D 12 and DirectX Raytracing (DXR). The project is Windows-only and targets 64-bit builds.

---

## Systems

| System | Description |
|---|---|
| **Renderer** | D3D12 renderer with a raytracing pipeline (DXR). Manages the swap chain, command lists, descriptor heaps, and render resource allocators. |
| **Raytracing** | Path-traced direct + indirect lighting. Next-event estimation (NEE) over a unified light list (Point, Directional, AreaQuad, Sky) with multiple importance sampling (balance heuristic) against BRDF samples. Emissive surfaces are lit via the bounce path. Russian-roulette termination; runtime-tunable samples-per-pixel and max-bounce depth. |
| **Portal pre-filter** | Analytic ray-vs-quad test (rectangular and ellipse portals) that rejects sky-NEE shadow rays before any TLAS dispatch. Portals are placed in the scene as a `Comp_Portal` entity component (with `GeometryScript`-generated visualization quads). The pre-filter is toggleable at runtime; per-portal counters and a thesis E[K] metric track its effectiveness. |
| **World / Entity** | Scene graph built around a `CWorld` that owns a flat list of `CEntity` objects. Worlds drive per-frame `Tick` and `Present` calls. Entities carry names and a transform editor is available in-game. |
| **Acceleration Structures** | Automatic bottom- and top-level AS construction and update for raytracing, driven by the world each frame. Multi-section models share a single BLAS with per-section hit-group entries. |
| **Camera** | First-person camera with view/projection matrix management. |
| **Materials & Shaders** | `CMaterialLibrary` manages materials keyed by name; HLSL shaders are compiled at runtime via the in-process `IDxcCompiler`. |
| **Model / Mesh** | Model loading through Assimp, plus a procedural **GeometryScript** path for runtime mesh generation (used for portal visualization and test scenes). |
| **Input** | Platform-abstracted input system (Win32 backend). Supports raw key and mouse events plus a rebindable `InputActionLibrary`. |
| **Math** | `Vector2`, `Vector3`, `Transform`, and general math utilities on top of DirectXMath. |
| **Memory** | TLSF-based general allocator, a pool allocator, and reference-counted smart pointers (`TRefShared` / `TRefWeak`). |
| **Assets** | Visitor-archive serialization framework (`Archive.h`, `Serializer.h`) with a YAML backend (`YamlArchive`) — current format for materials (`.frtmat.yml`). A JSON Schema under `Schemas/` provides IDE auto-completion for the material format. The legacy text-based `TextAssetIO` and the generic `AssetTool` remain available. `EnginePaths` centralizes well-known directory lookups (content, profiling output, etc.). |
| **Events** | Lightweight typed event/delegate system used throughout the engine. |
| **ImGui** | Dear ImGui is integrated for debug UI in non-headless builds. Includes live raytracing sliders, transform editor, stats panel, and frame-stat overlays. Press `I` to toggle the entire UI on/off. |
| **Profiling** | GPU timestamps via PIX-named scopes (`CGpuProfiler`), wave-coalesced ray counters via a UAV ring (`CRayCounters`), an analytic scene-descriptor snapshot (per-portal world area + screen coverage), and a rolling per-frame metrics aggregator. A profiling-session driver sweeps a cartesian product of render settings (spp × bounces × Russian-roulette depth × portal on/off) and exports one `.txt` (settings + scene snapshot) and one `.csv` (every measured frame) per configuration under `<project>/Local/Profiling/<session>/`. A Python analysis package (`Tools/Analysis/` — see its own README) parses the output for plots, summary tables, and cost-model validation. |

### Architecture

```
Editor.exe / Launcher.exe   hosts: own the loop, the worlds, the views
        │  load a project's game module (-Project=<.frtproject>); Release links it in
        ▼
MyGame.dll (game module)    GameInstance: BuildWorld, BeginPlay, Tick, EndPlay
        │  both link
        ▼
Core.dll                    CEngine (window, renderer, input, UI, config),
                            CWorldScene, CRenderer + CRenderView, CDebugTools
```

- **`CEngine`** starts and stops every subsystem explicitly, in dependency order, and provides the frame (`BeginFrame` … `RenderFrame`). It knows nothing about worlds or games: the hosts own those and call the game as a service.
- **Games are modules, in projects of their own.** A game lives in its own directory and repository, outside the engine (see [Projects](#projects)). `FRT_IMPLEMENT_GAME_MODULE(MyGame)` turns its DLL into a game module; `CGameModule` loads it and checks its ABI. Neither host is built per game: in Release a game is its own executable instead, the launcher's code with the game and the engine linked in.
- **Views.** The renderer draws any number of `CRenderView`s per frame — each with its own histories, size and render mode — from one shared scene. The launcher draws one straight into the swap chain; the editor draws its viewports into textures shown by ImGui.
- **Editor frame.** Queued commands are applied first; panels then draw and report what happened as plain data, from which the editor routes camera and game input; the world updates; the visible viewports render. Play mode runs the game on a clone of the edited world with a fresh game instance, dropped on Stop.

### Build configurations

Each configuration is a build type and a target, e.g. `Development-Default`:

| Build type | Code | Linking | Asserts, developer tools | For |
|---|---|---|---|---|
| **Debug** | unoptimized, debug runtime, Edit and Continue | DLLs: Core, the game module and every third-party library | on | stepping through code |
| **Development** | optimized, full debug information | DLLs, like Debug | on | day-to-day work |
| **Release** | optimized | one executable: Core, the game and every library built from source statically linked, static runtime. Only DXC and the D3D12 Agility SDK runtime ship beside it | off | shipping and profiling; no Editor |

- **Default / Headless** — `Default` includes the full rendering stack; `Headless` strips the window and renderer (useful for server or unit-test builds).
- The engine's binaries land in the engine's `Binaries/Win64`, a game's in its project's: named `<Project>-<Configuration>` (plain names for `Release-Default`, the shipping build) and `<Library>-<BuildType>` for third-party libraries, so configurations never overwrite each other. Every project using an engine checkout shares its binaries.
- In Debug and Development each module is its own DLL, so changing one rebuilds and relinks only that module. Hosts load the game module from a copy (`<project>/Binaries/Win64/HotReload/`), so it can be rebuilt while the editor or launcher runs - and both reload it: build the game project (in Visual Studio, or `FRT.bat build` in the project) and within a quarter of a second the editor stops play, has the new build rebuild the edit world and carries on (the launcher rebuilds its world and begins play again). A build that does not load leaves the host running without a game until one that does. Code can also be patched in place: Visual Studio's Hot Reload in Debug (it needs Edit and Continue information), Live++ in Debug and Development.
- The D3D12 debug layer is on by default in Debug only; `FRT_D3D_DEBUG=1` (or `0`) in the environment turns it on (or off) in Debug and Development.
- Code tests the build type through `Core/Source/BuildConfig.h` (`FRT_BUILD_DEBUG`, `FRT_BUILD_DEVELOPMENT`, `FRT_BUILD_RELEASE`, `FRT_MONOLITHIC` and the switches derived from them), never `_DEBUG` or `NDEBUG`.

---

## Requirements

- **OS:** Windows 10 / 11 (64-bit)
- **Compiler / IDE:** Any toolchain [supported by Premake5](https://premake.github.io/docs/using-premake) will work. A convenience script is provided for **Visual Studio 2022** (with the *Desktop development with C++* workload); scripts for other targets do not exist yet.
- **MSBuild / MSVC**_(if applicable)_**:** Toolset **v143** (ships with VS 2022) or later; the code uses **C++20** features.
- **Windows SDK:** **10.0.19041.0** or later — the minimum version that defines `D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE` (Windows 10 SDK 2004, May 2020). This is enforced in the generated project files.
- **GPU:** Any D3D12-capable adapter; DXR (raytracing) requires a GPU that supports DirectX Raytracing Tier 1.0 or higher (e.g. NVIDIA Turing / AMD RDNA2 and later)
- **Nothing else.** Every dependency is vendored in `ThirdParty/`, and the libraries the engine links are built from source as part of the solution: no package manager, submodules, network or install step.

---

## Setup

### 1. Set up the engine

Once per checkout, run **`Setup.bat`** (double-click is fine), the same as `FRT.bat setup`. It registers the checkout for this user under the name in `Engine.frtengine` (`FRTEngine2`), which is how projects find a shared engine, and generates `FRTEngine2.sln`: the engine's own projects, no game.

Open it, build **Development-Default | Win64** and start the **Editor** (the startup project). Without a project it opens the **project manager**: your recent projects, **Open Other...** for any `.frtproject` file, and **New Project** to create one from a template. Opening a project builds it if it has not been built, starts an editor for it - its own engine's, which need not be this one - and closes the manager; **File → Switch Project** in the editor comes back to it. A build that would have to relink the engine the manager itself runs from (after pulling engine changes, say) cannot happen while it runs: the manager then offers to close and finish the build in a console, which opens the project when done.

### 2. Projects

A project is a game in a directory of its own - its own git repository, never inside the engine's:

```
MyGame/
  MyGame.frtproject     [Project] Name=MyGame, Engine=<which engine>: committed, nothing machine-specific in it
  Source/               the game module's code
  Content/              its assets
  Config/Game.ini       its configuration (over the engine's Core/Config/Engine.ini)
  FRT.bat               runs its engine's FRT.bat for it
  GenerateProjectFiles.bat
  MyGame.sln            generated; with Binaries/, Intermediate/ and Local/, not committed
```

Its engine is one of two, chosen when it is created:

| Layout | `Engine=` | For |
|---|---|---|
| **Shared** (default) | the engine's registered name, `FRTEngine2` | one engine checkout for any number of projects; everyone working on the game registers their own checkout under that name (`Setup.bat`) |
| **Embedded** | `Engine`: a git submodule in `MyGame/Engine/` | a game that pins, or changes, its own engine; clones with `git clone --recursive` |

A project kept inside an engine checkout (like the templates in `Templates/`) needs no `Engine=`: it uses the engine it is in. `FRT_ENGINE_DIR` in the environment overrides all of these (CI, scripts).

A project's solution, `MyGame.sln`, has two run targets next to the engine's projects (in an **Engine** folder), and the game, the engine and the editor are debugged as one:

- **Editor MyGame**, the startup project, builds nothing itself and opens the game in the editor. Headless configurations have no editor, so there it runs the game in the launcher; in Release it runs the game itself, `MyGame/Binaries/Win64/MyGame.exe`.
- **MyGame**, the game, runs the game: in the launcher, or in Release as its own executable.

Each builds what it runs first. The engine's project files stay in the engine and are the same for every solution, so moving between projects never rebuilds the engine.

Code shared between projects is not supported yet; `Core/Source/Project/Project.h` describes the descriptor key reserved for it.

### 3. The command line

`FRT.bat` in the engine does everything the project manager does; a project's own `FRT.bat` runs its engine's for that project. `FRT.bat --help` lists every command and option.

| Command | Does |
|---|---|
| `FRT.bat setup` | registers the engine and generates `FRTEngine2.sln` |
| `FRT.bat new --name=MyGame` | creates a project in the current directory (`--location=<dir>`) from a template (`--template=Blank` or `Demo`), with a git repository; `--layout=embedded` adds the engine as a submodule (`--engine-url=<repository>` to follow another remote than the engine's `origin`); `--build` builds it as well |
| `FRT.bat generate` | (re)generates the solution - after adding or removing source files |
| `FRT.bat build` | builds the engine, or in a project the game and the hosts (`--config=Debug`, `Release`, `Development-Headless`...; default `Development-Default`) |
| `FRT.bat open` | opens the solution |
| `FRT.bat package` | builds the game in Release and writes it, with everything it runs with, to a directory that runs on any machine - no engine checkout, no sources (`--output=<dir>`; default `<project>\Local\Packaged\MyGame-Win64`; `--config=Release-Headless` for the console build) |
| `FRT.bat clean-compiled`, `clean-all` | removes the binaries and intermediate files; `clean-all` the generated project files too |

In the engine, a command acts on a project given with `--project=<.frtproject file or its directory>`, e.g. `FRT.bat build --project=Templates\Demo`. A command for a project that uses another engine checkout is handed to that engine's `FRT.bat`.

What is written while running is never committed and lives in a `Local/` folder: a project's holds its game's settings, captures, profiling sessions and smoke-test output; the engine checkout's holds the remembered generation options, the editor's layout, input overrides and project manager settings, and what runs without a project write. Only what every checkout on the machine shares is outside them, in `%LOCALAPPDATA%\FRTEngine` (`FRT_USER_DATA_DIR` overrides it): `Engines.ini` (registered engines) and `Projects.ini` (recent projects).

A package is the game and what it reads, and nothing else:

```
MyGame-Win64/
  MyGame.exe, dxcompiler.dll, dxil.dll, D3D12/   the Release executable and its runtime libraries
  MyGame.frtpackage                              marks the package: the game finds everything beside it
  Content/, Config/                              the project's
  Engine/Content/Shaders, Fonts                  the engine's: shader sources (compiled at run time), fonts
  Engine/Config/Engine.ini                       and configuration
```

Packaging again replaces what the packager wrote and keeps anything else in the directory; it refuses a non-empty directory that is not a package. A packaged game never writes to its directory - it may be read-only, and every user shares it: what a game writes to `Local\` elsewhere (settings, captures, profiling) goes to `%LOCALAPPDATA%\MyGame` instead (`FRT_USER_DATA_DIR\MyGame` when that is set).

### Generation options

After the command: `FRT.bat generate --nopch`, `FRT.bat vs2022 --project=... --precision=double`.

| Option | Effect |
|---|---|
| `--thirdparty=prebuilt` | Link prebuilt third-party libraries instead of building them (see below) |
| `--nopch` | No precompiled headers: checks that every file includes what it uses |
| `--fatal-warnings` | Warnings in engine code are errors (`/WX`) |
| `--msvc=<version>` | Build with that MSVC version, e.g. `14.44.35207` (default: whatever the v143 toolset defaults to, which an installed side-by-side toolset can pin to an older one) |
| `--precision=double` | 64-bit `frt::Real` for positions and measurements |

An engine checkout's binaries and project files are shared by its projects, so these options are remembered per checkout (in `Local/GenerationOptions.ini`): given once to `setup`, `generate`, `new` or `vs2022`, an option holds for every later generation of the engine and its projects until given again. Switches are turned off with `=off` (`--nopch=off`). Generation is Visual Studio 2022 only for now (`FRT.bat vs2022` is the Premake action underneath).

From the command line, build through a solution - `MSBuild FRTEngine2.sln -p:Configuration=Development-Default -p:Platform=Win64 -m`, with `-t:Editor` (or `-t:ThirdParty\assimp` and the like) for a single project; in a project's solution the engine's projects are under `Engine\`. Building a project file directly fails with MSB8013: third-party projects only have one configuration per build type, which the solution maps.

### Third-party libraries

Dear ImGui, assimp (glTF importers only), yaml-cpp and GoogleTest are vendored in `ThirdParty/` (each directory's `VERSION.txt` says which version and what was left out) and are projects of the solution, in its `ThirdParty` folder. They build once per build type - Default and Headless share them - in a few seconds, with the same compiler, runtime and settings as the engine.

To skip building them, use prebuilt binaries:

```bat
FRT.bat package-thirdparty
FRT.bat fetch-thirdparty --thirdparty-archive=<path or URL> --thirdparty-sha256=<hash>
FRT.bat generate --thirdparty=prebuilt
```

`package-thirdparty` builds the libraries in every build type and writes `Binaries/FRTEngine2-ThirdParty-Win64-<stamp>.zip`, printing its SHA-256 and the MSVC version it needs (that version or newer). `fetch-thirdparty` downloads or takes such an archive, verifies it and unpacks it into `ThirdParty/Prebuilt/`. Once an archive is published, set `prebuiltThirdParty.url` and `.sha256` in `Premake/thirdparty.lua` and `fetch-thirdparty` needs no options. An archive only fits the third-party sources it was built from; fetching or generating with a mismatched one fails.

### Updating a checkout from before the vendored libraries

`ThirdParty/Imgui` and `ThirdParty/vcpkg` used to be git submodules. Before pulling or merging, remove their working trees - `ThirdParty/Imgui` is now ordinary files at the same path - then rebuild from clean:

```bat
git submodule deinit -f ThirdParty/Imgui ThirdParty/vcpkg
rmdir /s /q ThirdParty\Imgui ThirdParty\vcpkg
git pull
FRT.bat clean-all
Setup.bat
```

`deinit -f` also deletes `ThirdParty/vcpkg/installed`, which older commits need to build: move it aside first if you still build those. The submodules' history stays in `.git/modules/ThirdParty` (about 900 MB). Delete it only when nothing uses it any more: no worktree or branch you still build from before this change, and no clone that borrows its objects (`git submodule update --reference`).

---

## Controls

### Editor

The editor shows the world twice, as Unity does: the **Scene** view through the editor's own camera, the **Game** view through the game's. Both are panels like any other - dock, tab, float and resize them freely; **View → Reset Layout** brings back the default arrangement.

The Scene view's camera navigates exactly like Unreal Engine's level viewport (same controls, speeds and sensitivities):

| Scene view | Action |
|---|---|
| **RMB** drag | Look around. While held: **W A S D** move, **E / Q** up / down, **R / F** up / down along the view, **Z / C** widen / narrow the view (it springs back on release), **wheel** changes the camera speed |
| **LMB** drag | Move along the ground and turn |
| **LMB+RMB** drag | Pan sideways and up / down |
| **MMB** drag | Pan along the view |
| **Wheel** | Move forward / back |
| **Alt + LMB / RMB / MMB** drag | Orbit / dolly / track around the point ahead |
| **Arrows, PageUp / PageDown, numpad** | Move while the Scene view has focus, no mouse button needed |

The camera speed is in the Scene view's toolbar (and changed by the wheel during a drag).

| Editor | Default |
|---|---|
| Maximize the viewport under the cursor over the editor / restore (or double-click its tab) | **Shift+Space** |
| Fullscreen that viewport (borderless, the whole screen) / back | **F11**, back also **Esc** |
| Path tracing / raster preview for that viewport | **F2** |
| Play / stop | **Ctrl+P** |
| Pause / resume (in edit mode: start the next session paused) | **Ctrl+Shift+P** |
| Step one game frame | **Ctrl+Alt+P** |

Every key above is a binding in [`Editor/Config/EditorInput.ini`](Editor/Config/EditorInput.ini), along with the camera's settings (speed, sensitivity, inversions). To change one, put the same section and key with your value in `Local/Editor/EditorInput.ini` in the engine checkout (not a project's `Local/`); the editor re-reads both files while it runs and reports anything it cannot read in its toolbar. Mouse navigation itself is fixed, as in Unreal.

**Play mode** runs the game on a copy of the edited world, so nothing a session does survives Stop, and each session starts from the same state with a fresh game instance. Both viewports show the running copy; the editor's chrome turns blue as a reminder. The game receives input only while the Game view has focus (a right-drag there captures the mouse for it); the Scene view stays free to fly around.

### Launcher

Camera movement is only active while **Right Mouse Button** is held.

| Input | Action |
|---|---|
| **RMB** (hold) | Enable camera movement |
| **Mouse move** | Look around (yaw / pitch) |
| **W / S** | Move forward / backward |
| **A / D** | Move left / right |
| **Space / Ctrl** | Move up / down (while RMB held) |
| **Space** | Toggle pause (while RMB **not** held) |
| **I** | Toggle the ImGui debug UI |
| **F2** | Cycle through render modes (debug builds) |

---

## Project layout

```
Engine.frtengine  — marks the engine checkout: its name and version
FRT.bat           — the engine's command line (Premake/tools.lua); Setup.bat runs its setup
Core/             — engine library (DLL; static library in Release)
Core-Test/        — unit tests (GoogleTest)
Editor/           — editor host: opens a project's world for editing, or the project manager
Launcher/         — runtime host: runs a project's game standalone
Templates/        — what new projects start from: Blank, and Demo (the sample game: Sponza and other models);
                    each is a project in place too. Common/ is what every project gets
Schemas/          — JSON Schemas for YAML asset formats (IDE auto-completion)
Tools/Analysis/   — Python package + Jupyter notebook for profiling-session analysis
ThirdParty/       — vendored libraries, built from source (ImGui, assimp, yaml-cpp, GoogleTest) or as shipped (Stb, D3D12 Agility SDK, DXC); see each VERSION.txt
Premake/          — Premake5 scripts (configurations: workspace.lua; projects and the registry: project.lua; commands: tools.lua; third-party: thirdparty.lua)
Binaries/         — the engine's DLLs / EXEs
Intermediate/     — compiled object files
Local/            — what runs write, never committed: the editor's layout and input overrides (Local/Editor/), generation options; a project's own Local/ holds its game's settings, captures, profiling sessions (Local/Profiling/<session>/, one .txt + .csv per configuration) and smoke-test output
```
