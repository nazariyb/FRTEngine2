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
| **Profiling** | GPU timestamps via PIX-named scopes (`CGpuProfiler`), wave-coalesced ray counters via a UAV ring (`CRayCounters`), an analytic scene-descriptor snapshot (per-portal world area + screen coverage), and a rolling per-frame metrics aggregator. A profiling-session driver sweeps a cartesian product of render settings (spp × bounces × Russian-roulette depth × portal on/off) and exports one `.txt` (settings + scene snapshot) and one `.csv` (every measured frame) per configuration under `Local/Profiling/<session>/`. A Python analysis package (`Tools/Analysis/` — see its own README) parses the output for plots, summary tables, and cost-model validation. |

### Build configurations

Each build has two axes:

- **Debug / Release** — standard optimization levels.
- **Default / Headless** — `Default` includes the full rendering stack; `Headless` strips the window and renderer (useful for server or unit-test builds).

---

## Requirements

- **OS:** Windows 10 / 11 (64-bit)
- **Compiler / IDE:** Any toolchain [supported by Premake5](https://premake.github.io/docs/using-premake) will work. A convenience script is provided for **Visual Studio 2022** (with the *Desktop development with C++* workload); scripts for other targets do not exist yet.
- **MSBuild / MSVC**_(if applicable)_**:** Toolset **v143** (ships with VS 2022) or later; the code uses **C++20** features.
- **Windows SDK:** **10.0.19041.0** or later — the minimum version that defines `D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE` (Windows 10 SDK 2004, May 2020). This is enforced in the generated project files.
- **GPU:** Any D3D12-capable adapter; DXR (raytracing) requires a GPU that supports DirectX Raytracing Tier 1.0 or higher (e.g. NVIDIA Turing / AMD RDNA2 and later)
- **Git:** Required on `PATH` — `vcpkg` is a git submodule and is initialized automatically by the install-deps step

---

## Setup

### 1. Install dependencies

Run the helper script from the repository root. It invokes the bundled Premake5 binary to initialize all submodules, bootstrap `vcpkg`, and install the required packages (`assimp`, `gtest`):

```bat
PremakeInstallDependencies.bat
```

This may take a few minutes on the first run as vcpkg downloads and builds packages from source.

### 2. Generate project files

**Visual Studio 2022** (convenience script):
```bat
PremakeGenerateVisualStudioProject.bat
```

**Any other Premake-supported toolchain** — invoke the bundled Premake5 binary directly with the appropriate generator target, for example:
```bat
.\ThirdParty\premake-5.0.0-beta2-windows\premake5.exe <target>
```
Replace `<target>` with the Premake action for your toolchain (e.g. `vs2019`, `gmake2`, `xcode4`). See the [Premake documentation](https://premake.github.io/docs/using-premake) for the full list.

### 3. Open and build

1. Open the generated solution / project in your IDE or invoke your build system directly.
2. Select a configuration (e.g. **Debug-Default | x64**).
3. Set **Demo** as the startup project (if applicable).
4. Build and run.

### Cleaning generated files

| Script | What it removes |
|---|---|
| `PremakeCleanCompiled.bat` | Compiled binaries and intermediate object files |
| `PremakeCleanAll.bat` | Everything above plus the generated project/solution files |

---

## Controls

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
Core/             — engine library (DLL)
Core-Test/        — unit tests (GoogleTest)
Demo/             — sample application
Schemas/          — JSON Schemas for YAML asset formats (IDE auto-completion)
Tools/Analysis/   — Python package + Jupyter notebook for profiling-session analysis
ThirdParty/       — vendored libraries (ImGui, Stb, DXR helpers, DXC, yaml-cpp, vcpkg)
Premake/          — Premake5 scripts
Binaries/         — output DLLs / EXEs
Intermediate/     — compiled object files
Local/Profiling/  — profiling-session output (gitignored; one folder per session, .txt + .csv per configuration)
```
