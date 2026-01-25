workspace "FRTEngine2"
	configurations(configs)
	platforms { "Win64", "Linux" }
	location (ROOT_DIR)

	language "C++"
	cppdialect "C++20"

	defines
	{
		'FRT_PLATFORM_NAME="%{cfg.platform}"',
		'FRT_CONFIG_NAME="%{cfg.buildcfg}"'
	}

	filter "platforms:Win64"
		system "Windows"
		architecture "x86_64"

	filter "files:**.hlsl"
		buildaction "None"

	filter {}
