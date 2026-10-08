-----------------------------------------
---------------  Core  ------------------
-----------------------------------------
project "Core"
	location (rootpath("%{prj.name}"))

	-- A DLL every module shares, except in Release: a static library linked into the executable.
	filter "configurations:Release*"
		kind "StaticLib"
	filter "configurations:not Release*"
		kind "SharedLib"
		defines { "FRT_CORE_EXPORTS" }
	filter {}

	flatBinaries()

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
		-- D3D12 Agility SDK headers must win over the Windows SDK's d3d12.h, so they go first. Not
		-- external includes: those are searched after the Windows SDK's.
		path.join(thirdPartyDir, "D3D12Agility/include"),
		rootpath("%{prj.name}/Source"),
		-- C++/HLSL shared structs live with the shaders (Shared/*.h is compiled by both).
		rootpath("%{prj.name}/Content/Shaders"),
		path.join(thirdPartyDir, "Dxc/inc"),
	}

	----------------
	----- Libs -----
	----------------
	useStb()
	useImGui()
	useAssimp()
	useYamlCpp()

	-- Only where Core is a DLL; in Release the executable links them (useCore).
	coreDependencies({ "kind:SharedLib" })

	-------------------
	----- Defines -----
	-------------------
	filter "platforms:Win64"
		defines { "WIN64", "_WINDOWS" }

	filter "configurations:*-Headless"
		defines { "FRT_HEADLESS" }

	filter {}

	engineWarnings()
	usePrecompiledHeader("CorePch.h", rootpath("%{prj.name}/Source/CorePch.cpp"))

	-- Real-number precision (frt::real). Must match every project linking Core;
	-- a mismatch is a link error, not a silent ABI break. See Premake/common.lua.
	applyPrecision()

	------------------------------
	----- Custom Build Steps -----
	------------------------------
	-- Runtime files beside the binaries. Copied only when changed, so building while a host runs
	-- from Binaries (rebuilding its game module) does not fail on files it has loaded.
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

	copyPrebuiltThirdPartyBinaries()
