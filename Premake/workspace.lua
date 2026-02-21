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
		-- DXR (ID3D12Device5, ID3D12GraphicsCommandList4) requires SDK 10.0.17763+
		-- D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE requires SDK 10.0.19041+
		systemversion "10.0.19041.0"

	filter "files:**.hlsl"
		buildaction "None"

	filter {}
