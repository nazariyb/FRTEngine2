workspace "FRTEngine2"
	configurations(configs)
	platforms(buildPlatforms)
	location (ROOT_DIR)
	startproject "Editor"

	language "C++"
	cppdialect "C++latest"

	defines
	{
		'FRT_PLATFORM_NAME="%{cfg.platform}"',
		'FRT_CONFIG_NAME="%{cfg.buildcfg}"',
		-- One ImGui configuration for the whole process (see the header).
		'IMGUI_USER_CONFIG="UI/ImGuiConfig.h"'
	}

	-- /MP: the files of a project compile in parallel. Without it MSBuild runs one cl.exe per
	-- file, one after another, and a full build takes several times longer.
	flags { "MultiProcessorCompile" }

	-- C4251 / C4275: STL and templated members of classes exported across a DLL boundary. Every
	-- module is built from source by the same compiler with the same runtime, so the warning has
	-- nothing to report. Binaries for other compilers would be built per compiler.
	filter "action:vs*"
		disablewarnings { "4251", "4275" }

	-- Build types; the code side is Core/Source/BuildConfig.h. Filters match "Debug*", not
	-- "Debug-*": third-party projects have one configuration per build type, named without the
	-- target suffix (Premake/thirdparty.lua).
	--   Debug        unoptimized, debug runtime, Edit and Continue (/ZI)
	--   Development  optimized with full debug information; asserts and developer tools on.
	--                The configuration to work in day to day.
	--   Release      optimized, developer tools compiled out, and monolithic: Core, the game and
	--                every third-party library are static libraries linked into one executable
	--                with the static runtime - nothing to redistribute but the D3D12 runtimes
	--                and DXC. No editor (it loads games as DLLs).
	-- Debug and Development link everything that can be a DLL as one, so a module rebuilds and
	-- relinks on its own (and a game module can be reloaded while running).
	filter "configurations:Debug*"
		defines { "FRT_BUILD_DEBUG=1" }   -- _DEBUG comes with the debug runtime
		runtime "Debug"
		optimize "Off"
		symbols "On"

	filter "configurations:Development*"
		defines { "FRT_BUILD_DEVELOPMENT=1", "NDEBUG" }
		runtime "Release"
		optimize "Speed"
		symbols "On"

	filter "configurations:Release*"
		defines { "FRT_BUILD_RELEASE=1", "FRT_MONOLITHIC=1", "NDEBUG" }
		runtime "Release"
		staticruntime "On"                -- /MT
		optimize "Speed"
		symbols "On"                      -- PDBs for crash dumps; not shipped

	-- Debug information inside the static libraries (/Z7) rather than in a compiler PDB beside
	-- the objects: the executable's link finds it wherever the libraries were built.
	filter { "configurations:Release*", "kind:StaticLib" }
		debugformat "c7"

	-- Code patching while running (Visual Studio's Hot Reload in Debug, Live++ in Debug and
	-- Development): room for a patch at the start of every function, and nothing referenced
	-- only through a patch discarded or folded. Debug gets the latter from incremental linking.
	filter { "configurations:not Release*", "kind:SharedLib or ConsoleApp or WindowedApp" }
		linkoptions { "/FUNCTIONPADMIN" }
	filter { "configurations:Development*", "kind:SharedLib or ConsoleApp or WindowedApp" }
		linkoptions { "/OPT:NOREF", "/OPT:NOICF" }

	filter "platforms:Win64"
		system "Windows"
		architecture "x86_64"
		-- DXR (ID3D12Device5, ID3D12GraphicsCommandList4) requires SDK 10.0.17763+
		-- D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE requires SDK 10.0.19041+
		systemversion "10.0.19041.0"

	filter "files:**.hlsl"
		buildaction "None"

	filter {}
