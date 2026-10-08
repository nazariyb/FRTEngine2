-----------------------------------------
--------  Third-party projects  ---------
-----------------------------------------
-- The vendored libraries built from source (Premake/thirdparty.lua says how and why). Not loaded
-- with --thirdparty=prebuilt, where the engine links prebuilt binaries instead.

local thirdPartyConfigMap = {}
for _, cfg in ipairs(buildConfigs) do
	for _, tgt in ipairs(buildTargets) do
		thirdPartyConfigMap[cfg .. "-" .. tgt] = cfg
	end
end

-- Starts a third-party project whose sources live in ThirdParty/<Dir>.
local function thirdPartyProject(name, dir)
	project (name)
	location (path.join(thirdPartyDir, dir))
	configmap (thirdPartyConfigMap)

	filter "configurations:Release*"
		kind "StaticLib"
	filter "configurations:not Release*"
		kind "SharedLib"
	filter {}

	-- Upstream code: built at its own standard and warning level, not the engine's.
	cppdialect "C++17"
	warnings "Default"

	targetdir (rootpath("Binaries/%{cfg.platform}"))
	objdir (rootpath("Intermediate/%{cfg.platform}/%{cfg.buildcfg}/%{prj.name}"))
	targetname "%{prj.name}-%{cfg.buildcfg}"
end

group "ThirdParty"

------------------------
----- Dear ImGui -------
------------------------
thirdPartyProject("ImGui", "Imgui")
	files
	{
		path.join(imguiDir, "*.h"),
		path.join(imguiDir, "*.cpp"),
		path.join(imguiDir, "backends/imgui_impl_dx12.*"),
		path.join(imguiDir, "backends/imgui_impl_win32.*"),
		path.join(imguiDir, "misc/cpp/imgui_stdlib.*"),
		path.join(imguiDir, "misc/debuggers/imgui.natvis"),
		path.join(imguiDir, "misc/debuggers/imgui.natstepfilter"),
		path.join(imguiDir, "VERSION.txt"),
	}

	includedirs
	{
		-- The DX12 backend must see the Agility SDK's d3d12.h, not the Windows SDK's.
		path.join(thirdPartyDir, "D3D12Agility/include"),
		imguiDir,
		-- IMGUI_USER_CONFIG (Premake/workspace.lua) names Core/Source/UI/ImGuiConfig.h.
		rootpath("Core/Source"),
	}

	filter "kind:SharedLib"
		defines { "FRT_IMGUI_EXPORTS" }
		-- The DX12 backend's; the executable links them where ImGui is static (coreDependencies).
		links { "dxgi", "d3dcompiler" }
	filter {}

------------------------
-------- assimp --------
------------------------
-- Only the glTF importers, no exporters (see ThirdParty/assimp/VERSION.txt). Every other
-- importer is switched off by name, because assimp's registry enables what is not.
local assimpDisabledImporters =
{
	"3D", "3DS", "3MF", "AC", "AMF", "ASE", "ASSBIN", "B3D", "BLEND", "BVH", "C4D", "COB",
	"COLLADA", "CSM", "DXF", "FBX", "HMP", "IFC", "IQM", "IRR", "IRRMESH", "LWO", "LWS", "M3D",
	"MD2", "MD3", "MD5", "MDC", "MDL", "MMD", "MS3D", "NDO", "NFF", "OBJ", "OFF", "OGRE",
	"OPENGEX", "PLY", "Q3BSP", "Q3D", "RAW", "SIB", "SMD", "STL", "TERRAGEN", "USD", "VRML", "X",
	"X3D", "XGL",
}

thirdPartyProject("assimp", "assimp")
	files
	{
		path.join(assimpDir, "code/**.cpp"),
		path.join(assimpDir, "code/**.h"),
		path.join(assimpDir, "code/**.inl"),
		path.join(assimpDir, "include/**"),
		path.join(assimpDir, "VERSION.txt"),
	}

	includedirs
	{
		assimpDir,
		path.join(assimpDir, "code"),
		path.join(assimpDir, "include"),
		path.join(assimpDir, "contrib/rapidjson/include"),
		path.join(assimpDir, "contrib/utf8cpp/source"),
	}

	-- What assimp's own CMake build defines.
	defines
	{
		"ASSIMP_BUILD_NO_EXPORT",
		"WIN32_LEAN_AND_MEAN", "UNICODE", "_UNICODE",
		"_SCL_SECURE_NO_WARNINGS", "_CRT_SECURE_NO_WARNINGS",
		"RAPIDJSON_HAS_STDSTRING=1", "RAPIDJSON_NOMEMBERITERATORCLASS",
	}
	for _, importer in ipairs(assimpDisabledImporters) do
		defines { "ASSIMP_BUILD_NO_" .. importer .. "_IMPORTER" }
	end

	filter "kind:SharedLib"
		defines { "ASSIMP_BUILD_DLL_EXPORT" }
	filter {}

	buildoptions { "/bigobj", "/source-charset:utf-8" }

------------------------
------- yaml-cpp -------
------------------------
thirdPartyProject("yaml-cpp", "yaml-cpp")
	files
	{
		path.join(yamlCppDir, "include/yaml-cpp/**.h"),
		path.join(yamlCppDir, "src/*.h"),
		path.join(yamlCppDir, "src/*.cpp"),
		path.join(yamlCppDir, "VERSION.txt"),
	}

	includedirs
	{
		path.join(yamlCppDir, "include"),
		path.join(yamlCppDir, "src"),
	}

	-- Defining YAML_CPP_API directly instead of CMake's yaml_cpp_EXPORTS: same export, without
	-- dll.h printing a #pragma message in every file.
	filter "kind:SharedLib"
		defines { "YAML_CPP_API=__declspec(dllexport)" }
	filter "kind:StaticLib"
		defines { "YAML_CPP_STATIC_DEFINE" }
	filter {}
	-- As upstream: constant conditions and `this` in member initializers are deliberate there.
	disablewarnings { "4127", "4355" }

------------------------
------ GoogleTest ------
------------------------
thirdPartyProject("gtest", "googletest")
	files
	{
		path.join(googleTestDir, "include/gtest/**.h"),
		-- Includes every other src/gtest-*.cc, relative to the library's root directory.
		path.join(googleTestDir, "src/gtest-all.cc"),
		path.join(googleTestDir, "VERSION.txt"),
	}

	includedirs
	{
		path.join(googleTestDir, "include"),
		googleTestDir,
	}

	filter "kind:SharedLib"
		defines { "GTEST_CREATE_SHARED_LIBRARY=1" }
	filter {}

group ""
