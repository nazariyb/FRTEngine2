-----------------------------------------
---------------  Core  ------------------
-----------------------------------------
local dxcExe = path.join(thirdPartyDir, "Dxc/bin/x64/dxc.exe")
local vcpkgBinDebug = path.join(vcpkgRoot, "installed", triplet, "debug", "bin", "*.pdb")
local vcpkgDllDebug = path.join(vcpkgRoot, "installed", triplet, "debug", "bin", "*.dll")
local vcpkgBinRelease = path.join(vcpkgRoot, "installed", triplet, "bin", "*.pdb")
local vcpkgDllRelease = path.join(vcpkgRoot, "installed", triplet, "bin", "*.dll")

project "Core"
	location (rootpath("%{prj.name}"))

	filter "configurations:Debug-*"
		kind "SharedLib"

	filter "configurations:Release-*"
		kind "SharedLib"
		-- TODO: should be static
		-- kind "StaticLib"

	filter {}

	targetdir (rootpath("Binaries/%{cfg.platform}"))
	objdir (rootpath("Intermediate/%{cfg.platform}/%{cfg.buildcfg}/%{prj.name}"))

	filter "configurations:Release-*"
		targetname "%{prj.name}"

	filter "configurations:not Release-*"
		targetname "%{prj.name}-%{cfg.buildcfg}"

	filter {}

	-----------------
	----- Files -----
	-----------------
	files
	{
		rootpath("%{prj.name}/Source/**.h"), rootpath("%{prj.name}/Source/**.hpp"),
		rootpath("%{prj.name}/Source/**.hxx"), rootpath("%{prj.name}/Source/**.hh"),
		rootpath("%{prj.name}/Source/**.c"), rootpath("%{prj.name}/Source/**.cpp"),
		rootpath("%{prj.name}/Source/**.cxx"), rootpath("%{prj.name}/Source/**.cc"),
		rootpath("%{prj.name}/Content/**"),
		rootpath("**.lua"),
		rootpath("**.txt"),
		rootpath("**.bat"),
		rootpath("**.frt*"),
	}

	filter "configurations:not *-Headless"
		files
		{
			rootpath("%{prj.name}/Content/Shaders/**.hlsl"),
			rootpath("%{prj.name}/Content/Shaders/**.hlsli"),
			path.join(thirdPartyDir, "Imgui/*.h"),
			path.join(thirdPartyDir, "Imgui/*.cpp"),
			path.join(thirdPartyDir, "Imgui/backends/imgui_impl_dx12.*"),
			path.join(thirdPartyDir, "Imgui/backends/imgui_impl_win32.*"),
			path.join(thirdPartyDir, "Imgui/misc/cpp/imgui_stdlib.*"),
		}
	filter {}

	removefiles
	{
		rootpath("Intermediate/**"),
		rootpath("**/vcpkg/**"),
		rootpath("%{prj.name}/Source/Assets/AssetToolClient.*"),
		rootpath("%{prj.name}/Source/Assets/AssetIpc.h"),
	}

	filter "configurations:*-Headless"
		removefiles
		{
-- 			rootpath("%{prj.name}/Source/Graphics/Render/**.h"),
			rootpath("%{prj.name}/Source/Graphics/Render/**.cpp"),
			rootpath("%{prj.name}/Source/Window.cpp"),
		}

	filter "action:vs*"
		files
		{
			rootpath("%{prj.name}/**.natvis"),
			path.join(thirdPartyDir, "Imgui/misc/debuggers/imgui.natvis"),
			path.join(thirdPartyDir, "Imgui/misc/debuggers/imgui.natstepfilter"),
		}

	filter "files:**.hlsl"
		buildaction "None"

	filter {}

	--------------------
	----- Includes -----
	--------------------
	includedirs
	{
		path.join(vcpkgRoot, "installed", triplet, "include"),
		rootpath("%{prj.name}/Source"),
		path.join(thirdPartyDir, "Stb"),
		path.join(thirdPartyDir, "Imgui"),
	}

	----------------
	----- Libs -----
	----------------
	filter "configurations:Debug-*"
		libdirs
		{
			path.join(vcpkgRoot, "installed", triplet, "debug/lib"),
		}

	filter "configurations:Release-*"
		libdirs
		{
			path.join(vcpkgRoot, "installed", triplet, "lib"),
		}

	filter "configurations:Debug-*"
		links
		{
			"assimp-vc143-mtd"
		}

	filter "configurations:Release-*"
		links
		{
			"assimp-vc143-mt"
		}

	filter "platforms:Win64"
		libdirs
		{
			path.join(thirdPartyDir, "Dxc/lib")
		}

		links
		{
			"d3d12", "dxgi", "d3dcompiler"
		}

	-------------------
	----- Defines -----
	-------------------
	filter "platforms:Win64"
		defines { "WIN64", "_WINDOWS" }

	filter "configurations:Debug-*"
		defines { "DEBUG", "FRT_CORE_EXPORTS" }
		symbols "On"

	filter "configurations:Release-*"
		defines { "NDEBUG", "RELEASE", "FRT_CORE_EXPORTS" }
		optimize "On"

	filter "configurations:*-Headless"
		defines { "FRT_HEADLESS" }

	filter {}

	------------------------------
	----- Custom Build Steps -----
	------------------------------
	prebuildcommands
	{
		-- temporary
		"{MKDIR} %{prj.location}Content/Shaders/Bin",
		"\"" .. dxcExe .. "\" -E main -Fo %{prj.location}Content/Shaders/Bin/VertexShader.shader -T vs_6_0 -Zi -Zpc -Qembed_debug %{prj.location}Content/Shaders/VertexShader.hlsl",
		"\"" .. dxcExe .. "\" -E main -Fo %{prj.location}Content/Shaders/Bin/PixelShader.shader -T ps_6_0 -Zi -Zpc -Qembed_debug %{prj.location}Content/Shaders/PixelShader.hlsl"
	}

	filter "configurations:Release-*"
		prebuildcommands
		{
			"{COPYFILE} %[" .. vcpkgBinRelease .. "] %[%{cfg.buildtarget.directory}]",
			"{COPYFILE} %[" .. vcpkgDllRelease .. "] %[%{cfg.buildtarget.directory}]",
		}

	filter "configurations:Debug-*"
		prebuildcommands
		{
			"{COPYFILE} %[" .. vcpkgBinDebug .. "] %[%{cfg.buildtarget.directory}]",
			"{COPYFILE} %[" .. vcpkgDllDebug .. "] %[%{cfg.buildtarget.directory}]",
		}

	filter {}
