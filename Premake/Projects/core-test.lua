----------------------------------------------
---------------  Core-Test  ------------------
----------------------------------------------
project "Core-Test"
	location (rootpath("%{prj.name}"))

	kind "ConsoleApp"

	-- Flat like everything else: every binary it loads, third-party ones included, carries its
	-- build type in its name, so it can sit beside the other configurations' binaries.
	flatBinaries()

	files
	{
		rootpath("%{prj.name}/**.h"),
		rootpath("%{prj.name}/**.cpp"),
	}
	-- Core-TestModule's sources (below); the tests include its header and load the DLL.
	removefiles { rootpath("%{prj.name}/TestModule/**.cpp") }

	includedirs
	{
		-- Tests live directly under Core-Test/, not in a Source/ subdirectory, so the
		-- project root is what makes TestCommon.h reachable from ECS/ and Math/.
		rootpath("%{prj.name}"),
	}

	defines { "_CONSOLE" }

	-- AssetTool: its tests run the executable beside Core-Test (Core-Test/AssetTool).
	dependson { "Core-TestModule", "AssetTool" }

	useCore()
	useGoogleTest()
	usePrecompiledHeader("TestPch.h", rootpath("%{prj.name}/TestPch.cpp"))


----------------------------------------------
------------  Core-TestModule  ---------------
----------------------------------------------
-- A stand-in for a game module: a DLL that the module-release tests load, subscribe from and unload
-- (Core-Test/TestModule/TestModule.h). It compiles Core's header-only templates and links nothing of
-- Core, so it is a DLL in every configuration, Release (where Core is static) included. No
-- precompiled header, like a game module: the headers it includes must stand on their own.
project "Core-TestModule"
	location (rootpath("Core-Test/TestModule"))

	kind "SharedLib"

	flatBinaries()

	files
	{
		rootpath("Core-Test/TestModule/**.h"),
		rootpath("Core-Test/TestModule/**.cpp"),
	}

	includedirs
	{
		rootpath("Core/Source"),
		rootpath("Core-Test"),
	}

	filter "platforms:Win64"
		defines { "WIN64", "_WINDOWS" }
	filter {}

	engineWarnings()
	engineSourceCharset()
	applyPrecision()
