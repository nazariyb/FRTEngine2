triplet = "x64-windows" -- TODO: support for Linux

local neededPackages =
{
	"assimp",
	"gtest"
}

thirdPartyDir = "ThirdParty"

vcpkgRoot = thirdPartyDir.."/vcpkg"

newaction
{
	trigger = "install-deps",
	description = "Install vcpkg dependencies",
	execute = function ()
		for _, pkg in ipairs(neededPackages) do
			local command = string.format(
				"\"%s/vcpkg\" install %s:%s",
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
	trigger = "clean",
	description = "Clean generated files and compiled binaries",
	execute = function ()
		removeDirectory("Binaries")
		removeDirectory("Intermediate")

		os.remove("**.sln")
		os.remove("**.vcxproj")
		os.remove("**.vcxproj.filters")
		os.remove("**.vcxproj.user")
	end
}

workspace "FRTEngine2"
	configurations { "Debug", "Release" }
	platforms { "Win64", "Linux" }

	language "C++"
	cppdialect "C++20"

	filter "platforms:Win64"
		system "Windows"
		architecture "x86_64"

	filter "files:**.hlsl"
		buildaction "None"

	filter {}

project "Core"
	location "%{prj.name}"

	filter "configurations:Debug"
		kind "SharedLib"

	filter "configurations:Release"
		kind "SharedLib"
		-- TODO: should be static
		-- kind "StaticLib"

	filter {}

	targetdir "Binaries/%{cfg.platform}/%{cfg.buildcfg}"
	objdir "Intermediate/%{cfg.platform}/%{cfg.buildcfg}/%{prj.name}"

	files
	{
		"%{prj.name}/Source/**.h",
		"%{prj.name}/Source/**.cpp",
		"%{prj.name}/Content/Shaders/**.hlsl",
		"%{prj.name}/Content/Shaders/**.hlsli",
	}

	filter "files:**.hlsl"
		buildaction "None"

	filter {}

	includedirs
	{
		vcpkgRoot .. "/installed/" .. triplet .. "/include",
		"%{prj.name}/Source",
		thirdPartyDir .. "/Stb",
	}

	libdirs
	{
		vcpkgRoot .. "/installed/" .. triplet .. "/lib",
	}

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

	filter "platforms:Win64"
		defines { "WIN64", "_WINDOWS" }

	filter "configurations:Debug"
		defines { "DEBUG", "FRT_CORE_EXPORTS" }
		symbols "On"

	filter "configurations:Release"
		defines { "NDEBUG", "FRT_CORE_EXPORTS" }
		optimize "On"

	filter {}

	prebuildcommands
	{
		"{COPYFILE} %[%{vcpkgRoot}/installed/%{triplet}/bin/*.dll] %[%{cfg.buildtarget.directory}]",
		-- temporary
		"%{wks.location}%{thirdPartyDir}/Dxc/bin/x64/dxc.exe -E main -Fo %{prj.location}Content/Shaders/Bin/VertexShader.shader -T vs_6_0 -Zi -Zpc -Qembed_debug %{prj.location}/Content/Shaders/VertexShader.hlsl",
		"%{wks.location}%{thirdPartyDir}/Dxc/bin/x64/dxc.exe -E main -Fo %{prj.location}Content/Shaders/Bin/PixelShader.shader -T ps_6_0 -Zi -Zpc -Qembed_debug %{prj.location}/Content/Shaders/PixelShader.hlsl"
	}

	filter "configurations:Debug"
		postbuildcommands
		{
			"{COPYFILE} %[%{vcpkgRoot}/installed/%{triplet}/bin/*.pdb] %[%{cfg.buildtarget.directory}]",
		}

	filter {}

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
		"Core/Source",
	}

	links
	{
		"Core"
	}

	defines { "_CONSOLE" }

	filter "configurations:Debug"
		defines { "_DEBUG" }
		symbols "On"

	filter "configurations:Release"
		defines { "NDEBUG" }
		optimize "On"

	filter {}

project "Demo"
	location "%{prj.name}"

	kind "WindowedApp"

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

	filter "configurations:Debug"
		defines { "_DEBUG" }
		symbols "On"

	filter "configurations:Release"
		defines { "NDEBUG" }
		optimize "On"

	filter {}
