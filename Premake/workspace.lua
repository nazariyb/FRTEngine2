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
	--   Release      optimized; developer tools compiled out
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
		defines { "FRT_BUILD_RELEASE=1", "NDEBUG" }
		runtime "Release"
		optimize "Speed"
		symbols "On"                      -- PDBs for crash dumps; not shipped

	filter "platforms:Win64"
		system "Windows"
		architecture "x86_64"
		-- DXR (ID3D12Device5, ID3D12GraphicsCommandList4) requires SDK 10.0.17763+
		-- D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE requires SDK 10.0.19041+
		systemversion "10.0.19041.0"

	filter "files:**.hlsl"
		buildaction "None"

	filter {}
