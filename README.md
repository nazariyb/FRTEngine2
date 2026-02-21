# FRTEngine2

A small experimental 3D engine built on Direct3D 12 and DirectX Raytracing (DXR). The project is Windows-only and targets 64-bit builds.

---

## Systems

| System | Description |
|---|---|
| **Renderer** | D3D12 renderer with a raytracing pipeline (DXR). Manages the swap chain, command lists, descriptor heaps, and render resource allocators. |
| **World / Entity** | Scene graph built around a `CWorld` that owns a flat list of `CEntity` objects. Worlds drive per-frame `Tick` and `Present` calls. |
| **Acceleration Structures** | Automatic bottom- and top-level AS construction and update for raytracing, driven by the world each frame. |
| **Camera** | First-person camera with view/projection matrix management. |
| **Materials & Shaders** | `CMaterialLibrary` manages materials keyed by name; shaders are compiled at runtime via DXC (bundled). |
| **Model / Mesh** | Model loading through Assimp. Procedural mesh generation helpers are also provided. |
| **Input** | Platform-abstracted input system (Win32 backend). Supports raw key and mouse events plus a rebindable `InputActionLibrary`. |
| **Math** | `Vector2`, `Vector3`, `Transform`, and general math utilities on top of DirectXMath. |
| **Memory** | TLSF-based general allocator, a pool allocator, and reference-counted smart pointers (`TRefShared` / `TRefWeak`). |
| **Assets** | Text-based asset I/O (`TextAssetIO`) and a generic `AssetTool` for loading content from disk. |
| **Events** | Lightweight typed event/delegate system used throughout the engine. |
| **ImGui** | Dear ImGui is integrated for debug UI in non-headless builds. |

### Build configurations

Each build has two axes:

- **Debug / Release** — standard optimization levels.
- **Default / Headless** — `Default` includes the full rendering stack; `Headless` strips the window and renderer (useful for server or unit-test builds).

---

## Requirements

- **OS:** Windows 10 / 11 (64-bit)
- **Compiler / IDE:** Any toolchain [supported by Premake5](https://premake.github.io/docs/using-premake) will work. A convenience script is provided for **Visual Studio 2022** (with the *Desktop development with C++* workload); scripts for other targets do not exist yet.
- **GPU:** Any D3D12-capable adapter; DXR (raytracing) requires a GPU that supports DirectX Raytracing Tier 1.0 or higher (e.g. NVIDIA Turing / AMD RDNA2 and later)

---

## Setup

### 1. Install dependencies

Run the helper script from the repository root. It invokes the bundled Premake5 binary to bootstrap **vcpkg** and install the required packages (`assimp`, `gtest`):

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

## Project layout

```
Core/          — engine library (DLL)
Core-Test/     — unit tests (GoogleTest)
Demo/          — sample application
ThirdParty/    — vendored libraries (ImGui, Stb, DXR helpers, DXC, vcpkg)
Premake/       — Premake5 scripts
Binaries/      — output DLLs / EXEs
Intermediate/  — compiled object files
```
