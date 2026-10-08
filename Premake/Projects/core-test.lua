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

	includedirs
	{
		-- Tests live directly under Core-Test/, not in a Source/ subdirectory, so the
		-- project root is what makes TestCommon.h reachable from ECS/ and Math/.
		rootpath("%{prj.name}"),
	}

	defines { "_CONSOLE" }

	useCore()
	useGoogleTest()
