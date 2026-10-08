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
-- The game project the hosts (Launcher, Editor) run by default: their working directory -
-- project config and content resolve from it - and, through its Config/Game.ini, the game
-- module they load. Another game: -Game:Module=<Name> with that project as working directory.
defaultGameProject = "Demo"

newoption
{
	trigger     = "nopch",
	description = "Compile without precompiled headers: checks that every file includes what it uses"
}

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

-- One directory per platform, configurations separated by the target name (see core.lua).
function flatBinaries()
	targetdir (rootpath("Binaries/%{cfg.platform}"))
	objdir (rootpath("Intermediate/%{cfg.platform}/%{cfg.buildcfg}/%{prj.name}"))

	filter "configurations:Release-*"
		targetname "%{prj.name}"
	filter "configurations:not Release-*"
		targetname "%{prj.name}-%{cfg.buildcfg}"
	filter {}
end

-- Everything a project linking Core needs: includes, link, defines, warnings, precision.
function useCore()
	includedirs
	{
		-- D3D12 Agility SDK headers must win over the Windows SDK's d3d12.h, so they go first.
		path.join(thirdPartyDir, "D3D12Agility/include"),
		rootpath("Core/Source"),
		rootpath("Core/Content/Shaders"),
		path.join(thirdPartyDir, "Dxc/inc"),
	}

	links { "Core" }
	-- One ImGui for the whole process, in its own DLL (Core/Source/UI/ImGuiConfig.h).
	useImGui()

	filter "platforms:Win64"
		defines { "WIN64", "_WINDOWS" }

	filter "configurations:*-Headless"
		defines { "FRT_HEADLESS" }

	filter {}

	-- Must match Core's precision. See the top of this file.
	applyPrecision()
end

-- A host executable: debugs in the default game's project directory and builds its module.
function hostProject()
	debugdir (rootpath(defaultGameProject))
	dependson { defaultGameProject }
end
