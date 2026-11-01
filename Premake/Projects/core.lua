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
		rootpath("%{prj.name}/Source/**.inl"),
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
		-- Not Tools/**.py: Tools/Analysis keeps a Python virtual environment (.venv) of its own.
		rootpath("Tools/Lint/*.py"),
		rootpath("Tools/Unicode/*.py"),
	}
	-- Compiled shaders an old build wrote there: nothing reads them (programs come from the shader
	-- cache, Graphics/Render/RHI/ShaderCache.h), so they stay out of the project and of packages.
	removefiles { rootpath("%{prj.name}/Content/Shaders/Bin/**") }

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
		-- What the prebuild step below generates (BuilderSourceHashes.gen.h).
		rootpath("Intermediate/Generated"),
	}

	----------------
	----- Libs -----
	----------------
	useStb()
	useCgltf()
	useImGui()
	useAssimp()
	useYamlCpp()
	useXxHash()
	useBlake3()
	useZstd()

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
	engineSourceCharset()
	usePrecompiledHeader("CorePch.h", rootpath("%{prj.name}/Source/CorePch.cpp"))

	-- Real-number precision (frt::real). Must match every project linking Core;
	-- a mismatch is a link error, not a silent ABI break. See Premake/common.lua.
	applyPrecision()

	------------------------------
	----- Custom Build Steps -----
	------------------------------
	-- Runtime files beside the binaries. Copied only when changed, so building while a host runs
	-- from Binaries (rebuilding its game module) does not fail on files it has loaded.
	copyRuntimeFiles()

	copyPrebuiltThirdPartyBinaries()

	-- The builder source guard (Core/Source/Assets/BuilderVersions.inl): the current hash of every
	-- builder's sources, written before every build - not only at FRT.bat generate - so a development
	-- build compares the sources it is built from with the committed table. By the vendored Premake,
	-- which needs nothing else installed; it rewrites its outputs only when they change.
	prebuildcommands
	{
		string.format("%%[%s] --file=%%[%s] builder-hashes || exit /b 1",
			rootpath("ThirdParty/premake-5.0.0-beta2-windows/premake5.exe"), rootpath("premake5.lua")),
	}
