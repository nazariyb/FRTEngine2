triplet = "x64-windows" -- TODO: support for Linux

local neededPackages =
{
	"assimp",
	"gtest"
}

thirdPartyDir = "ThirdParty"

vcpkgRoot = thirdPartyDir.."\\vcpkg"

newaction
{
	trigger = "install-deps",
	description = "Install vcpkg dependencies",
	execute = function ()
		os.execute(vcpkgRoot.."\\bootstrap-vcpkg.bat")
		for _, pkg in ipairs(neededPackages) do
			local command = string.format(
				"\".\\%s\\vcpkg.exe\" install %s:%s",
				vcpkgRoot, pkg, triplet
			)
			print(command)
			os.execute(command)
		end
	end
}

function removeDirectory(dir)
    if not os.isdir(dir) then
        return
    end

	-- TODO
    -- if ??? == "windows" then
        os.execute(string.format("rmdir /s /q \"%s\"", dir))
    -- else
    --     os.execute(string.format("rm -rf \"%s\"", dir))
    -- end
end

newaction
{
	trigger = "clean-all",
	description = "Clean generated files and compiled binaries",
	execute = function ()
		removeDirectory("Binaries")
		removeDirectory("Intermediate")

		for _, folder in ipairs({ ".", "Core", "Core-Test", "Demo" }) do
			os.remove(folder.."/*.sln")
			os.remove(folder.."/*.vcxproj")
			os.remove(folder.."/*.vcxproj.filters")
			os.remove(folder.."/*.vcxproj.user")
			os.remove(folder.."/*.sln.DotSettings.user")
		end
	end
}

newaction
{
	trigger = "clean-compiled",
	description = "Clean only compiled binaries and intermediate files",
	execute = function ()
		removeDirectory("Binaries")
		removeDirectory("Intermediate")
	end
}

local buildConfigs = { "Debug", "Release" }
local buildTargets = { "Default", "Headless" }
local configs = {}

for _, cfg in ipairs(buildConfigs) do
	for _, tgt in ipairs(buildTargets) do
		table.insert(configs, cfg .. "-" .. tgt)
	end
end

workspace "FRTEngine2"
	configurations(configs)
	platforms { "Win64", "Linux" }

	language "C++"
	cppdialect "C++20"

	filter "platforms:Win64"
		system "Windows"
		architecture "x86_64"

	filter "files:**.hlsl"
		buildaction "None"

	filter {}


-----------------------------------------
---------------  Core  ------------------
-----------------------------------------
project "Core"
	location "%{prj.name}"

	filter "configurations:Debug-*"
		kind "SharedLib"

	filter "configurations:Release-*"
		kind "SharedLib"
		-- TODO: should be static
		-- kind "StaticLib"

	filter {}

	targetdir "Binaries/%{cfg.platform}/%{cfg.buildcfg}"
	objdir "Intermediate/%{cfg.platform}/%{cfg.buildcfg}/%{prj.name}"

	-----------------
	----- Files -----
	-----------------
	files
	{
		"%{prj.name}/Source/**.h", "%{prj.name}/Source/**.hpp", "%{prj.name}/Source/**.hxx", "%{prj.name}/Source/**.hh",
		"%{prj.name}/Source/**.c", "%{prj.name}/Source/**.cpp", "%{prj.name}/Source/**.cxx", "%{prj.name}/Source/**.cc",
		"%{prj.name}/../**.lua",
		"%{prj.name}/../**.txt",
		"%{prj.name}/../**.bat",
	}

	filter "configurations:not *-Headless"
		files
		{
			"%{prj.name}/Content/Shaders/**.hlsl",
			"%{prj.name}/Content/Shaders/**.hlsli",
			thirdPartyDir.."/Imgui/*.h",
			thirdPartyDir.."/Imgui/*.cpp",
			thirdPartyDir.."/Imgui/backends/imgui_impl_dx12.*",
			thirdPartyDir.."/Imgui/backends/imgui_impl_win32.*",
			thirdPartyDir.."/Imgui/misc/cpp/imgui_stdlib.*",
		}
	filter {}

	removefiles
	{
		"%{wks.location}/Intermediate/**",
		"**/vcpkg/**",
	}

	filter "configurations:*-Headless"
		removefiles
		{
-- 			"%{prj.name}/Source/Graphics/Render/**.h",
			"%{prj.name}/Source/Graphics/Render/**.cpp",
			"%{prj.name}/Source/Window.cpp",
		}

	filter "action:vs*"
		files
		{
			"%{prj.name}/**.natvis",
			thirdPartyDir.."/Imgui/misc/debuggers/imgui.natvis",
			thirdPartyDir.."/Imgui/misc/debuggers/imgui.natstepfilter",
		}

	filter "files:**.hlsl"
		buildaction "None"

	filter {}

	--------------------
	----- Includes -----
	--------------------
	includedirs
	{
		vcpkgRoot .. "/installed/" .. triplet .. "/include",
		"%{prj.name}/Source",
		thirdPartyDir .. "/Stb",
		thirdPartyDir.."/Imgui",
	}

	----------------
	----- Libs -----
	----------------
	filter "configurations:Debug-*"
		libdirs
		{
			vcpkgRoot .. "/installed/" .. triplet .. "/debug/lib",
		}

	filter "configurations:Release-*"
		libdirs
		{
			vcpkgRoot .. "/installed/" .. triplet .. "/lib",
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
			"%{thirdPartyDir}/Dxc/lib"
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
		"%{wks.location}%{thirdPartyDir}/Dxc/bin/x64/dxc.exe -E main -Fo %{prj.location}Content/Shaders/Bin/VertexShader.shader -T vs_6_0 -Zi -Zpc -Qembed_debug %{prj.location}Content/Shaders/VertexShader.hlsl",
		"%{wks.location}%{thirdPartyDir}/Dxc/bin/x64/dxc.exe -E main -Fo %{prj.location}Content/Shaders/Bin/PixelShader.shader -T ps_6_0 -Zi -Zpc -Qembed_debug %{prj.location}Content/Shaders/PixelShader.hlsl"
	}

	filter "configurations:Release-*"
		prebuildcommands
		{
			"{COPYFILE} %[%{vcpkgRoot}/installed/%{triplet}/bin/*.pdb] %[%{cfg.buildtarget.directory}]",
			"{COPYFILE} %[%{vcpkgRoot}/installed/%{triplet}/bin/*.dll] %[%{cfg.buildtarget.directory}]",
		}

	filter "configurations:Debug-*"
		prebuildcommands
		{
			"{COPYFILE} %[%{vcpkgRoot}/installed/%{triplet}/debug/bin/*.pdb] %[%{cfg.buildtarget.directory}]",
			"{COPYFILE} %[%{vcpkgRoot}/installed/%{triplet}/debug/bin/*.dll] %[%{cfg.buildtarget.directory}]",
		}

	filter {}


----------------------------------------------
---------------  Core-Test  ------------------
----------------------------------------------
project "Core-Test"
	location "%{prj.name}"

	kind "ConsoleApp"

	targetdir "Binaries/%{cfg.platform}/%{cfg.buildcfg}"
	objdir "Intermediate/%{cfg.platform}/%{cfg.buildcfg}/%{prj.name}"

	files
	{
		"%{prj.name}/**.h",
		"%{prj.name}/**.cpp",
	}

	includedirs
	{
		vcpkgRoot .. "/installed/" .. triplet .. "/include",
		"%{prj.name}/Source",
		"Core/Source",
	}

	filter "configurations:Debug-*"
		libdirs
		{
			vcpkgRoot .. "/installed/" .. triplet .. "/debug/lib",
			vcpkgRoot .. "/installed/" .. triplet .. "/debug/lib/manual-link",
		}

	filter "configurations:Release-*"
		libdirs
		{
			vcpkgRoot .. "/installed/" .. triplet .. "/lib",
			vcpkgRoot .. "/installed/" .. triplet .. "/lib/manual-link",
		}

	filter {}

	links
	{
		"Core",
		"gtest",
		"gtest_main"
	}

	filter {}

	defines { "_CONSOLE", "FRT_HEADLESS" }

	filter "configurations:Debug-*"
		defines { "_DEBUG" }
		symbols "On"

	filter "configurations:Release-*"
		defines { "NDEBUG" }
		optimize "On"

	filter {}


-----------------------------------------
---------------  Demo  ------------------
-----------------------------------------
project "Demo"
	location "%{prj.name}"

	filter "configurations:*-Headless"
		kind "ConsoleApp"
	filter "configurations:not *-Headless"
		kind "WindowedApp"
	filter {}

	targetdir "Binaries/%{cfg.platform}/%{cfg.buildcfg}"
	objdir "Intermediate/%{cfg.platform}/%{cfg.buildcfg}/%{prj.name}"

	files
	{
		"%{prj.name}/**.h",
		"%{prj.name}/**.cpp",
		"%{prj.name}/**.rc",
		"%{prj.name}/**.ico",
	}

	includedirs
	{
		"Core/Source",
	}

	links
	{
		"Core"
	}

	defines { "_WINDOWS" }

	filter "configurations:Debug-*"
		defines { "_DEBUG" }
		symbols "On"

	filter "configurations:Release-*"
		defines { "NDEBUG" }
		optimize "On"

	filter "configurations:*-Headless"
		defines { "FRT_HEADLESS" }

	filter {}
