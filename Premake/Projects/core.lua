-----------------------------------------
---------------  Core  ------------------
-----------------------------------------
project "Core"
	location (rootpath("%{prj.name}"))

	filter "configurations:Debug-*"
		kind "SharedLib"

	filter "configurations:Release-*"
		kind "SharedLib"
		-- TODO: should be static
		-- kind "StaticLib"

	filter {}

	-- One directory per platform, configurations separated by the targetname suffix below.
	-- Deliberate: a single place to find every binary beats a tree of near-empty folders.
	-- Third-party libraries carry their build type in their names too (Premake/thirdparty.lua),
	-- so no configuration overwrites another's binaries.
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
		-- Repository files worth having at hand in the IDE. Named places only: a root-wide `**`
		-- glob walks ThirdParty, Binaries and Intermediate too - tens of thousands of files, once
		-- per configuration, every time the projects are generated.
		rootpath("premake5.lua"),
		rootpath("Premake/**.lua"),
		rootpath("*.bat"),
		rootpath("*.md"),
		rootpath(".git*"),
		rootpath("*/Config/**.ini"),
		rootpath("Schemas/**"),
		rootpath("Tools/**.md"),
		rootpath("Tools/**.txt"),
	}

	filter "configurations:not *-Headless"
		files
		{
			rootpath("%{prj.name}/Content/Shaders/**.hlsl"),
			rootpath("%{prj.name}/Content/Shaders/**.hlsli"),
		}
	filter {}

	filter "configurations:*-Headless"
		removefiles
		{
-- 			rootpath("%{prj.name}/Source/Graphics/Render/**.h"),
			-- Only the translation units that genuinely need a swap chain / window.
			-- The rest of Graphics/Render is FRT_HEADLESS-guarded and must keep building:
			-- the headless scene loader still resolves materials through CMaterialLibrary
			-- (GameInstance::Load and SRenderModel::LoadFromFile), which drags in its
			-- serializers. Removing all of Render/**.cpp left those unresolved at link.
			rootpath("%{prj.name}/Source/Graphics/Render/Renderer.cpp"),
			rootpath("%{prj.name}/Source/Window.cpp"),
			-- GPU-only passes. The RHI stays: it needs no window, and CRenderContext - exported,
			-- so all of its inline members are emitted wherever its header is seen - calls into
			-- it, which left headless Core with unresolved RHI symbols at link.
			rootpath("%{prj.name}/Source/Graphics/Render/Passes/**.cpp"),
			rootpath("%{prj.name}/Source/Graphics/Render/Scene/**.cpp"),
		}

	filter "action:vs*"
		files
		{
			rootpath("%{prj.name}/**.natvis"),
		}

	filter "files:**.hlsl"
		buildaction "None"

	filter {}

	--------------------
	----- Includes -----
	--------------------
	includedirs
	{
		-- D3D12 Agility SDK headers must win over the Windows SDK's d3d12.h, so they go first.
		path.join(thirdPartyDir, "D3D12Agility/include"),
		rootpath("%{prj.name}/Source"),
		-- C++/HLSL shared structs live with the shaders (Shared/*.h is compiled by both).
		rootpath("%{prj.name}/Content/Shaders"),
		path.join(thirdPartyDir, "Stb"),
		path.join(thirdPartyDir, "Dxc/inc"),
	}

	----------------
	----- Libs -----
	----------------
	useImGui()
	useAssimp()
	useYamlCpp()

	filter "platforms:Win64"
		libdirs
		{
			path.join(thirdPartyDir, "Dxc/lib/x64")
		}

		links
		{
			"d3d12", "dxgi", "d3dcompiler", "dxcompiler"
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

	-- Real-number precision (frt::real). Must match every project linking Core;
	-- a mismatch is a link error, not a silent ABI break. See Premake/common.lua.
	applyPrecision()

	------------------------------
	----- Custom Build Steps -----
	------------------------------
	prebuildcommands
	{
		"{COPYFILE} " .. path.join(thirdPartyDir, "Dxc/bin/x64/dxcompiler.dll") .. " %[%{cfg.buildtarget.directory}]",
		"{COPYFILE} " .. path.join(thirdPartyDir, "Dxc/bin/x64/dxil.dll") .. " %[%{cfg.buildtarget.directory}]",
		-- Agility SDK runtime: loaded from <exe dir>/D3D12/ (D3D12SDKPath exported by the host
		-- executable, Engine/HostEntry.h).
		"{MKDIR} %[%{cfg.buildtarget.directory}/D3D12]",
		"{COPYFILE} %[" .. path.join(thirdPartyDir, "D3D12Agility/bin/x64/D3D12Core.dll") .. "] %[%{cfg.buildtarget.directory}/D3D12]",
		"{COPYFILE} %[" .. path.join(thirdPartyDir, "D3D12Agility/bin/x64/d3d12SDKLayers.dll") .. "] %[%{cfg.buildtarget.directory}/D3D12]",
	}
