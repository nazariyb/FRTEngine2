ROOT_DIR = ROOT_DIR or path.getabsolute(path.join(_SCRIPT_DIR, ".."))

function rootpath(relativePath)
	return path.join(ROOT_DIR, relativePath)
end

thirdPartyDir = rootpath("ThirdParty")

function removeDirectory(dir)
	if not os.isdir(dir) then
		return
	end

	-- TODO
	-- if ??? == "windows" then
		os.execute(string.format("rmdir /s /q \"%s\"", dir))
	-- else
	-- 	os.execute(string.format("rm -rf \"%s\"", dir))
	-- end
end

-----------------------------------------
--------  Real-number precision  --------
-----------------------------------------
-- Precision used for positions, distances, and other world-space measurements
-- (frt::real). Chosen per project through applyPrecision(), which defaults to the
-- workspace-wide value below.
--
-- WARNING: frt::real appears in signatures exported across the Core DLL boundary.
-- Every project linking Core must use the same value. A mismatch is caught at link
-- time by the guard symbol in Core/Source/Precision.h rather than silently
-- reinterpreting struct layouts.
--
-- Note this is CPU-side only. DirectXMath and every GPU-facing struct remain 32-bit;
-- double precision is for simulation and measurement, not for upload.

newoption
{
	trigger     = "precision",
	value       = "TYPE",
	description = "Real-number precision used for positions and measurements",
	allowed     =
	{
		{ "float",  "32-bit single precision (default)" },
		{ "double", "64-bit double precision" },
	},
	default     = "float"
}

precisionBits =
{
	float  = 32,
	double = 64,
}

defaultPrecision = _OPTIONS["precision"] or "float"

-- Emits the FRT_REAL_PRECISION define. Call from a project's defines section, in a
-- cleared filter scope. Pass "float" or "double" to override the workspace default.
function applyPrecision(which)
	local name = which or defaultPrecision
	local bits = precisionBits[name]

	if not bits then
		error(string.format(
			"applyPrecision: unknown precision '%s' (expected 'float' or 'double')",
			tostring(name)))
	end

	defines { "FRT_REAL_PRECISION=" .. bits }
end

-- Build types (what each one is: Premake/workspace.lua) and targets.
buildConfigs = { "Debug", "Development", "Release" }
buildTargets = { "Default", "Headless" }
configs = {}

for _, cfg in ipairs(buildConfigs) do
	for _, tgt in ipairs(buildTargets) do
		table.insert(configs, cfg .. "-" .. tgt)
	end
end

-- Linux is a placeholder for the port; nothing builds for it yet.
--
-- It must not reach a Visual Studio action. Premake matches the platform name against its
-- known systems, so "Linux" silently implies system "linux", and a linux WindowedApp has
-- no executable extension - which the vcxproj exporter writes as an empty <TargetExt>
-- element. MSBuild reads that element's value as the newline and indentation between the
-- tags, so the NormalizePath call in Microsoft.CppCommon.targets that builds $(TargetPath)
-- fails with "Illegal characters in path" (MSB4184). Visual Studio swallows the error on
-- load; Rider reports it and refuses to open the project.
buildPlatforms = { "Win64" }

if not (_ACTION and _ACTION:match("^vs")) then
	table.insert(buildPlatforms, "Linux")
end

-----------------------------------------
-----------  Project helpers  -----------
-----------------------------------------
newoption
{
	trigger     = "nopch",
	description = "Compile without precompiled headers: checks that every file includes what it uses"
}

newoption
{
	trigger     = "msvc",
	value       = "VERSION",
	description = "The MSVC version every project builds with, e.g. 14.44.35207 (default: the v143 toolset's)"
}

newoption
{
	trigger     = "fatal-warnings",
	description = "Treat warnings in engine code as errors (/WX)"
}

-- Warnings for engine code; third-party code builds at its upstream level (Premake/thirdparty.lua).
-- Not fatal by default: FRT_WARN_ON_USE (Core.h) relies on a warning staying a warning.
function engineWarnings()
	warnings "Extra"   -- /W4
	-- Off by default in MSVC, real bugs when they fire, and at zero today:
	--   C4263, C4264  a member function hides a base class virtual instead of overriding it
	--   C4265         a class with virtual functions has a non-virtual destructor
	--   C5038         a member initializer list is out of declaration order
	--   C5262         implicit fall-through between switch cases
	buildoptions { "/w14263", "/w14264", "/w14265", "/w15038", "/w15262" }

	if _OPTIONS["fatal-warnings"] then
		flags { "FatalCompileWarnings" }
	end
end

-- Source and execution character set UTF-8: narrow literals are UTF-8 bytes (Containers/String.h),
-- whatever the machine's code page. Engine code only: third-party projects keep their own flags
-- (assimp sets /source-charset:utf-8, which /utf-8 must not be combined with).
function engineSourceCharset()
	buildoptions { "/utf-8" }
end

-- A precompiled header force-included into every file of the project, so no source names it.
-- Header: the name under which the project's include path finds it; Source: the .cpp that
-- builds it. Small projects (a launcher, a game module) are better off without one.
function usePrecompiledHeader(Header, Source)
	if _OPTIONS["nopch"] then
		return
	end
	pchheader (Header)
	pchsource (Source)
	forceincludes { Header }
end

-- A build-event command copying Source into DestinationDir unless the copy there is already
-- identical. A host running from Binaries has these files loaded, and overwriting a loaded image
-- fails even with the same bytes, so unchanged files must be left alone. Any real failure fails
-- the event: MSBuild runs a build event as one batch and only sees its last command's result.
function copyIfChanged(Source, DestinationDir)
	local destination = DestinationDir .. "/" .. path.getname(Source)
	return string.format('fc /B %%[%s] %%[%s] >nul 2>&1 || copy /B /Y %%[%s] %%[%s] >nul || exit /b 1',
		Source, destination, Source, DestinationDir)
end

-- One directory per platform, configurations separated by the target name: a single place to
-- find every binary beats a tree of near-empty folders. Only the shipping configuration,
-- Release-Default, has plain names. Under the engine, or a project's directory (RootDir).
function flatBinaries(RootDir)
	RootDir = RootDir or ROOT_DIR
	targetdir (path.join(RootDir, "Binaries/%{cfg.platform}"))
	objdir (path.join(RootDir, "Intermediate/%{cfg.platform}/%{cfg.buildcfg}/%{prj.name}"))

	filter "configurations:Release-Default"
		targetname "%{prj.name}"
	filter "configurations:not Release-Default"
		targetname "%{prj.name}-%{cfg.buildcfg}"
	filter {}
end

-- Core's link dependencies: system, DXC and third-party libraries. Where Core is a DLL it links
-- them itself; where it is a static library (Release) every executable linking Core does.
-- Never on a static library: Premake emits a static library's links as its librarian inputs,
-- and lib.exe would copy every member of those libraries into it. Scope: filter terms.
function coreDependencies(scope)
	filter(scope)
		libdirs { path.join(thirdPartyDir, "Dxc/lib/x64") }
		links { "d3d12", "dxgi", "d3dcompiler", "dxcompiler" }
	filter {}

	linkThirdParty(scope, { "assimp", "yaml-cpp" })
	linkThirdParty(table.join(scope, { "configurations:not *-Headless" }), { "ImGui" })
end

-- Everything a project linking Core needs: includes, link, defines, precision.
function useCore()
	includedirs
	{
		-- D3D12 Agility SDK headers must win over the Windows SDK's d3d12.h, so they go first. Not
		-- external includes: those are searched after the Windows SDK's.
		path.join(thirdPartyDir, "D3D12Agility/include"),
		rootpath("Core/Source"),
		rootpath("Core/Content/Shaders"),
		path.join(thirdPartyDir, "Dxc/inc"),
	}

	-- A static library (a game module in Release) needs only the headers: the executable links
	-- Core, and everything Core needs, itself.
	filter "kind:not StaticLib"
		links { "Core" }
	filter {}

	-- One ImGui for the whole process (Core/Source/UI/ImGuiConfig.h); modules call it directly.
	useImGui()
	linkThirdParty({ "configurations:not Release*", "configurations:not *-Headless" }, { "ImGui" })

	filter "platforms:Win64"
		defines { "WIN64", "_WINDOWS" }

	filter "configurations:*-Headless"
		defines { "FRT_HEADLESS" }

	filter {}

	coreDependencies({ "configurations:Release*", "kind:ConsoleApp or WindowedApp" })

	engineWarnings()
	engineSourceCharset()

	-- The process ANSI code page is UTF-8 (the manifest says why). Executables only - Editor,
	-- Launcher, Core-Test and a Release game (Launcher/Source built into it): a DLL's manifest
	-- cannot change it. Premake hands a configuration's .manifest files to the manifest tool
	-- (AdditionalManifestFiles), which merges them into the manifest embedded in the executable.
	filter "kind:ConsoleApp or WindowedApp"
		files { rootpath("Core/Resources/Utf8CodePage.manifest") }
	filter {}

	-- Debugger visualizers. Where Core is a DLL its own link embeds them (core.lua's natvis files);
	-- in Release it is a static library, which cannot carry them, so the executable's link does.
	-- Quoted: the engine may live in a directory with spaces.
	filter { "configurations:Release*", "kind:ConsoleApp or WindowedApp" }
		linkoptions
		{
			'/NATVIS:"' .. rootpath("Core/Debugger/Visualizers/StringTypesVisualizer.natvis") .. '"',
			'/NATVIS:"' .. rootpath("Core/Debugger/Visualizers/MemoryTypesVisualizer.natvis") .. '"',
		}
	filter {}

	-- Must match Core's precision. See the top of this file.
	applyPrecision()
end

-- Build-event commands putting the files the engine loads at run time - DXC, the D3D12 Agility
-- SDK - beside an executable or Core.
function copyRuntimeFiles()
	prebuildcommands
	{
		copyIfChanged(path.join(thirdPartyDir, "Dxc/bin/x64/dxcompiler.dll"), "%{cfg.buildtarget.directory}"),
		copyIfChanged(path.join(thirdPartyDir, "Dxc/bin/x64/dxil.dll"), "%{cfg.buildtarget.directory}"),
		-- Agility SDK runtime: loaded from <exe dir>/D3D12/ (D3D12SDKPath exported by the host
		-- executable, Engine/HostEntry.h).
		"{MKDIR} %[%{cfg.buildtarget.directory}/D3D12]",
		copyIfChanged(path.join(thirdPartyDir, "D3D12Agility/bin/x64/D3D12Core.dll"), "%{cfg.buildtarget.directory}/D3D12"),
		copyIfChanged(path.join(thirdPartyDir, "D3D12Agility/bin/x64/d3d12SDKLayers.dll"), "%{cfg.buildtarget.directory}/D3D12"),
	}
end

-- A project's game (Engine/GameModule.h; Project: Premake/project.lua): Source/ built in the
-- project's own Binaries and Intermediate. In Debug and Development a DLL the hosts load - the
-- editor, or the launcher, with -Project=<file>. In Release the game itself: the shipping
-- executable, <Module>.exe, with the launcher's code, Core and every library linked in.
function gameProject(Project)
	project (Project.Module)
	-- With the generated solution, out of the project's tracked files.
	location (path.join(Project.Dir, "Intermediate/ProjectFiles"))

	filter "configurations:not Release*"
		kind "SharedLib"
		-- Hot reload (Engine/GameModule.cpp loads a copy of the DLL): the DLL names its PDB
		-- without a directory, so a debugger reads the copy beside the loaded DLL and never locks
		-- the build's own PDB, which the next link rewrites.
		linkoptions { "/PDBALTPATH:%{cfg.buildtarget.basename}.pdb" }
	filter { "configurations:Release*", "configurations:*-Headless" }
		kind "ConsoleApp"
	filter { "configurations:Release*", "configurations:not *-Headless" }
		kind "WindowedApp"
	filter {}

	-- FRT_IMPLEMENT_GAME_MODULE reports it, so a monolithic executable can say what it runs.
	defines { 'FRT_GAME_MODULE_NAME="%{prj.name}"' }

	flatBinaries(Project.Dir)

	files
	{
		path.join(Project.Dir, "Source/**.h"),
		path.join(Project.Dir, "Source/**.hpp"),
		path.join(Project.Dir, "Source/**.cpp"),
		path.join(Project.Dir, "Config/**.ini"),
		Project.File,
	}

	includedirs
	{
		path.join(Project.Dir, "Source"),
	}

	-- The executable: the launcher's entry point, and the runtime files beside it.
	filter "configurations:Release*"
		files
		{
			rootpath("Launcher/Source/**.h"),
			rootpath("Launcher/Source/**.cpp"),
		}
		copyRuntimeFiles()
	filter {}

	useCore()

	-- Starting the game project runs the game: in the launcher, or in Release the game's own
	-- executable. (Editor <Game>, gameEditorProject, opens it in the editor.) The launcher is
	-- built first, so a start never runs a stale one.
	dependson { "Launcher" }
	debugdir (Project.Dir)
	debugargs { '-Project="' .. winpath(Project.File) .. '"' }
	filter "configurations:not Release*"
		debugcommand (rootpath("Binaries/%{cfg.platform}/Launcher-%{cfg.buildcfg}.exe"))
	filter {}
end

-- "Editor <Game>": a project of the project's solution - its startup project - that builds
-- nothing and starts the editor with the project open. Where there is no editor it starts what
-- the game project does: the launcher in Headless configurations, the game itself in Release.
-- The editor, the launcher and the game are built first, so a start never runs stale ones.
function gameEditorProject(Project)
	project ("Editor " .. Project.Module)
	location (path.join(Project.Dir, "Intermediate/ProjectFiles"))
	kind "Utility"
	objdir (path.join(Project.Dir, "Intermediate/%{cfg.platform}/%{cfg.buildcfg}/Editor"))

	dependson { "Editor", "Launcher", Project.Module }
	debugdir (Project.Dir)
	debugargs { '-Project="' .. winpath(Project.File) .. '"' }
	filter { "configurations:not Release*", "configurations:not *-Headless" }
		debugcommand (rootpath("Binaries/%{cfg.platform}/Editor-%{cfg.buildcfg}.exe"))
	filter { "configurations:not Release*", "configurations:*-Headless" }
		debugcommand (rootpath("Binaries/%{cfg.platform}/Launcher-%{cfg.buildcfg}.exe"))
	filter "configurations:Release-Default"
		debugcommand (path.join(Project.Dir, "Binaries/%{cfg.platform}/" .. Project.Module .. ".exe"))
	filter { "configurations:Release*", "configurations:not Release-Default" }
		debugcommand (path.join(Project.Dir, "Binaries/%{cfg.platform}/" .. Project.Module .. "-%{cfg.buildcfg}.exe"))
	filter {}
end
