-----------------------------------------
-----------  Third-party code  ----------
-----------------------------------------
-- Every library the engine links is vendored under ThirdParty/ (see each VERSION.txt) and, by
-- default, built from source as a project of this solution (Premake/Projects/thirdparty.lua),
-- with the solution's compiler, runtime and configurations. A fresh clone needs no package
-- manager, no network and no install step.
--
-- One build per build type: Default and Headless configurations share the same third-party
-- binaries (configmap), named <Library>-<BuildType> so they never overwrite each other in the
-- flat Binaries folder. Inside those projects, filters see the mapped names (Debug, Release),
-- which is why workspace-wide filters match on "Debug*" rather than "Debug-*".
--
-- Like the engine's own modules, they are DLLs in Debug and Development and static libraries in
-- Release, where everything links into one executable.
--
-- Or prebuilt (--thirdparty=prebuilt): the same binaries, built once by `premake5
-- package-thirdparty` and unpacked into ThirdParty/Prebuilt by `premake5 fetch-thirdparty`, are
-- linked instead and the projects are left out of the solution. Headers always come from the
-- vendored sources, so an archive is only valid for the sources it was built from: it carries a
-- stamp of them, checked when it is unpacked and when projects are generated.

newoption
{
	trigger     = "thirdparty",
	value       = "MODE",
	description = "How third-party libraries are provided",
	allowed     =
	{
		{ "source",   "Built from the vendored sources as projects of the solution (default)" },
		{ "prebuilt", "Prebuilt binaries in ThirdParty/Prebuilt (premake5 fetch-thirdparty)" },
	},
	default     = "source"
}

newoption
{
	trigger     = "thirdparty-archive",
	value       = "PATH|URL",
	description = "fetch-thirdparty: the archive to unpack (default: prebuiltThirdParty.url)"
}

newoption
{
	trigger     = "thirdparty-sha256",
	value       = "HASH",
	description = "fetch-thirdparty: the archive's expected SHA-256 (default: prebuiltThirdParty.sha256)"
}

bPrebuiltThirdParty = (_OPTIONS["thirdparty"] == "prebuilt")

imguiDir      = path.join(thirdPartyDir, "Imgui")
assimpDir     = path.join(thirdPartyDir, "assimp")
yamlCppDir    = path.join(thirdPartyDir, "yaml-cpp")
googleTestDir = path.join(thirdPartyDir, "googletest")

-- Third-party projects, which are also the names their binaries start with.
thirdPartyLibraries = { "ImGui", "assimp", "yaml-cpp", "gtest" }

-- Where a published archive lives. Fill in after uploading one made by package-thirdparty (e.g.
-- as a GitHub release asset); --thirdparty-archive and --thirdparty-sha256 override both.
prebuiltThirdParty =
{
	url    = nil,
	sha256 = nil,
}

-- Bump when a change to how the libraries are built makes older archives incompatible (runtime,
-- defines that change their ABI, ...). Library versions are covered by their VERSION.txt.
local thirdPartyBuildRevision = 1

prebuiltThirdPartyDir = path.join(thirdPartyDir, "Prebuilt/Win64")

-- Identifies the sources a prebuilt archive must have been built from: each library's version
-- and ImGui's configuration header, which every module compiles against.
function thirdPartySourceStamp()
	local parts = { "revision " .. thirdPartyBuildRevision }
	local files =
	{
		path.join(imguiDir, "VERSION.txt"),
		path.join(assimpDir, "VERSION.txt"),
		path.join(yamlCppDir, "VERSION.txt"),
		path.join(googleTestDir, "VERSION.txt"),
		rootpath("Core/Source/UI/ImGuiConfig.h"),
	}
	for _, file in ipairs(files) do
		local text = io.readfile(file) or error("missing " .. file)
		-- Line endings depend on the checkout, not on the sources.
		table.insert(parts, (text:gsub("\r\n", "\n")))
	end
	return string.sha1(table.concat(parts, "\0"))
end

-----------------------------------------
------------  Consumer helpers  ---------
-----------------------------------------
-- Headers and the defines that select DLL or static linkage. Linking is separate, because which
-- binary links a library depends on where Core is static (coreDependencies, Premake/common.lua).
--
-- Headers come in as external includes (/external:I, /external:W0): third-party headers never
-- produce warnings in the engine's own code, whatever its warning level.
local function useThirdPartyHeaders(dirs)
	externalincludedirs (dirs)
	externalwarnings "Off"
end

-- Links third-party libraries within a filter scope: their projects, or in prebuilt mode their
-- files, named per build type like the projects' targets.
function linkThirdParty(scope, libraries)
	if not bPrebuiltThirdParty then
		filter(scope)
			links (libraries)
		filter {}
		return
	end

	filter(scope)
		libdirs { path.join(prebuiltThirdPartyDir, "lib") }
	for _, buildType in ipairs(buildConfigs) do
		local names = {}
		for _, library in ipairs(libraries) do
			table.insert(names, library .. "-" .. buildType)
		end
		filter(table.join(scope, { "configurations:" .. buildType .. "*" }))
			links (names)
	end
	filter {}
end

-- Prebuilt mode: the DLLs (and their PDBs) the engine loads, copied beside its binaries before
-- a project builds. Source mode builds them there in the first place.
function copyPrebuiltThirdPartyBinaries()
	if not bPrebuiltThirdParty then
		return
	end

	for _, buildType in ipairs(buildConfigs) do
		local commands = {}
		for _, library in ipairs(thirdPartyLibraries) do
			for _, extension in ipairs({ ".dll", ".pdb" }) do
				local file = path.join(prebuiltThirdPartyDir, "bin", library .. "-" .. buildType .. extension)
				table.insert(commands, copyIfChanged(file, "%{cfg.buildtarget.directory}"))
			end
		end
		filter { "configurations:" .. buildType .. "*", "configurations:not Release*" }
			prebuildcommands (commands)
	end
	filter {}
end

-- Header-only; Core compiles the implementation (Graphics/Model.cpp).
function useStb()
	useThirdPartyHeaders({ path.join(thirdPartyDir, "Stb") })
end

-- ImGuiConfig.h selects the linkage.
function useImGui()
	useThirdPartyHeaders({ imguiDir })
end

function useAssimp()
	useThirdPartyHeaders({ path.join(assimpDir, "include") })
	filter "configurations:not Release*"
		defines { "ASSIMP_DLL" }
	filter {}
end

function useYamlCpp()
	useThirdPartyHeaders({ path.join(yamlCppDir, "include") })
	filter "configurations:not Release*"
		defines { "YAML_CPP_API=__declspec(dllimport)" }
	filter "configurations:Release*"
		defines { "YAML_CPP_STATIC_DEFINE" }
	filter {}
end

-- Only test executables use it, so it links here too.
function useGoogleTest()
	useThirdPartyHeaders({ path.join(googleTestDir, "include") })
	linkThirdParty({}, { "gtest" })
	filter "configurations:not Release*"
		defines { "GTEST_LINKED_AS_SHARED_LIBRARY=1" }
	filter {}
end

-----------------------------------------
---------  Prebuilt archives  -----------
-----------------------------------------
-- An archive holds, for one platform: manifest.json, lib/<Library>-<BuildType>.lib (import
-- libraries in Debug and Development, static libraries in Release) and bin/<Library>-<BuildType>
-- .dll|.pdb for the DLL build types. Static libraries carry their debug information (/Z7).

local function readManifest(dir)
	local text = io.readfile(path.join(dir, "manifest.json"))
	return text and json.decode(text) or nil
end

-- Generation in prebuilt mode needs an unpacked archive built from these very sources.
if bPrebuiltThirdParty and _ACTION and _ACTION:match("^vs") then
	local manifest = readManifest(prebuiltThirdPartyDir)
	if not manifest then
		error("--thirdparty=prebuilt: no prebuilt libraries in " .. prebuiltThirdPartyDir ..
			". Run `premake5 fetch-thirdparty` first, or generate without the option to build them from source.", 0)
	end
	if manifest.stamp ~= thirdPartySourceStamp() then
		error("--thirdparty=prebuilt: the libraries in " .. prebuiltThirdPartyDir .. " were built from other " ..
			"third-party sources than this checkout's. Fetch a matching archive, or generate without the option.", 0)
	end
end

-- The MSVC version the build uses: --msvc, or the v143 toolset's default.
local function msvcVersion(installation)
	if _OPTIONS["msvc"] then
		return _OPTIONS["msvc"]
	end
	local text = io.readfile(path.join(installation, "VC/Auxiliary/Build/Microsoft.VCToolsVersion.v143.default.txt"))
	return text and text:match("^%s*(.-)%s*$") or "unknown"
end

local function sha256(file)
	local output = os.outputof('certutil -hashfile "' .. winpath(file) .. '" SHA256')
	for line in (output or ""):gmatch("[^\r\n]+") do
		local hash = line:gsub("%s", "")
		if hash:match("^%x+$") and #hash == 64 then
			return hash:lower()
		end
	end
	return nil
end

newaction
{
	trigger     = "package-thirdparty",
	description = "Build the third-party libraries in every build type and archive them for --thirdparty=prebuilt",
	execute     = function ()
		local installation = visualStudio2022()
		local msbuild = installation and path.join(installation, "MSBuild/Current/Bin/amd64/MSBuild.exe")
		if not msbuild or not os.isfile(msbuild) then
			error("package-thirdparty: no Visual Studio 2022 with MSBuild and the v143 C++ toolset found (vswhere)", 0)
		end

		-- The projects, built from source: regenerates the solution in source mode, with the
		-- generation options given here (--msvc picks the compiler the archive is built with).
		local options = generationOptions(false) .. " --thirdparty=source"
		print("Generating the solution with third-party projects (vs2022 " .. options .. ")...")
		if not runCommand('"' .. winpath(_PREMAKE_COMMAND) .. '" --file="' .. winpath(rootpath("premake5.lua")) .. '" vs2022 ' .. options) then
			error("package-thirdparty: generating the solution failed", 0)
		end

		-- Solution targets of projects in a solution folder carry the folder's name.
		local targets = {}
		for _, library in ipairs(thirdPartyLibraries) do
			table.insert(targets, "ThirdParty\\" .. library)
		end
		targets = table.concat(targets, ";")
		for _, buildType in ipairs(buildConfigs) do
			print("Building the third-party libraries (" .. buildType .. ")...")
			local command = string.format('"%s" "%s" -t:%s -p:Configuration=%s-Default -p:Platform=Win64 -m -nologo -v:minimal',
				winpath(msbuild), winpath(rootpath("FRTEngine2.sln")), targets, buildType)
			if not runCommand(command) then
				error("package-thirdparty: building " .. buildType .. " failed", 0)
			end
		end

		local stamp = thirdPartySourceStamp()
		local staging = rootpath("Intermediate/ThirdPartyPackage/Win64")
		os.rmdir(staging)
		os.mkdir(path.join(staging, "lib"))
		os.mkdir(path.join(staging, "bin"))

		local binaries = rootpath("Binaries/Win64")
		for _, buildType in ipairs(buildConfigs) do
			for _, library in ipairs(thirdPartyLibraries) do
				local name = library .. "-" .. buildType
				local files = { { "lib", name .. ".lib" } }
				if buildType ~= "Release" then
					table.insert(files, { "bin", name .. ".dll" })
					table.insert(files, { "bin", name .. ".pdb" })
				end
				for _, file in ipairs(files) do
					local ok, err = os.copyfile(path.join(binaries, file[2]), path.join(staging, file[1], file[2]))
					if not ok then
						error("package-thirdparty: " .. tostring(err), 0)
					end
				end
			end
		end

		local manifest =
		{
			platform  = "Win64",
			stamp     = stamp,
			msvc      = msvcVersion(installation),
			libraries = thirdPartyLibraries,
			buildTypes = buildConfigs,
		}
		io.writefile(path.join(staging, "manifest.json"), json.encode_pretty(manifest))

		local archive = rootpath("Binaries/FRTEngine2-ThirdParty-Win64-" .. stamp:sub(1, 12) .. ".zip")
		os.remove(archive)
		local tar = path.join(os.getenv("SystemRoot") or "C:/Windows", "System32/tar.exe")
		if not runCommand('"' .. winpath(tar) .. '" -a -c -f "' .. winpath(archive) .. '" -C "' .. winpath(staging) .. '" .') then
			error("package-thirdparty: creating the archive failed", 0)
		end

		print("")
		print("Archive: " .. archive)
		print("SHA-256: " .. (sha256(archive) or "?"))
		print("MSVC:    " .. manifest.msvc .. " (machines linking it need this version or newer)")
		print("Publish it, then set prebuiltThirdParty.url and .sha256 in Premake/thirdparty.lua.")
		print("The solution was regenerated in source mode (vs2022 " .. options .. "), which it now stays in;")
		print("generate with --thirdparty=prebuilt to change that.")
	end
}

newaction
{
	trigger     = "fetch-thirdparty",
	description = "Download or unpack a prebuilt third-party archive into ThirdParty/Prebuilt",
	execute     = function ()
		local source = _OPTIONS["thirdparty-archive"] or prebuiltThirdParty.url
		local expectedHash = _OPTIONS["thirdparty-sha256"] or (not _OPTIONS["thirdparty-archive"] and prebuiltThirdParty.sha256) or nil
		if not source then
			error("fetch-thirdparty: no archive. Pass --thirdparty-archive=<path or URL>, or set prebuiltThirdParty.url " ..
				"in Premake/thirdparty.lua (archives come from `premake5 package-thirdparty`).", 0)
		end

		local archive = source
		if not os.isfile(source) then
			archive = rootpath("Intermediate/ThirdPartyDownload/" .. path.getname(source))
			os.mkdir(path.getdirectory(archive))
			print("Downloading " .. source .. "...")
			local result, code = http.download(source, archive, { timeout = 600 })
			if result ~= "OK" then
				os.remove(archive)
				-- curl follows redirects (release assets are served through one) for certain.
				if not runCommand('curl.exe -L --fail -s -S -o "' .. winpath(archive) .. '" "' .. source .. '"') then
					error("fetch-thirdparty: downloading failed: " .. tostring(result) .. " (" .. tostring(code) .. ")", 0)
				end
			end
		end

		if expectedHash then
			local hash = sha256(archive)
			if hash ~= expectedHash:lower() then
				error("fetch-thirdparty: SHA-256 mismatch for " .. archive .. ": " .. tostring(hash) .. ", expected " .. expectedHash, 0)
			end
		else
			print("No SHA-256 given: not verifying the archive.")
		end

		local unpacked = rootpath("Intermediate/ThirdPartyUnpack")
		os.rmdir(unpacked)
		os.mkdir(unpacked)
		if zip.extract(archive, unpacked) ~= 0 then
			error("fetch-thirdparty: cannot unpack " .. archive .. " (a .zip from package-thirdparty)", 0)
		end

		local manifest = readManifest(unpacked)
		if not manifest or manifest.platform ~= "Win64" then
			error("fetch-thirdparty: " .. archive .. " is not a Win64 third-party archive", 0)
		end
		if manifest.stamp ~= thirdPartySourceStamp() then
			error("fetch-thirdparty: " .. archive .. " was built from other third-party sources than this checkout's", 0)
		end

		os.rmdir(prebuiltThirdPartyDir)
		os.mkdir(path.getdirectory(prebuiltThirdPartyDir))
		if not os.rename(unpacked, prebuiltThirdPartyDir) then
			error("fetch-thirdparty: cannot move the libraries into " .. prebuiltThirdPartyDir, 0)
		end

		print("Prebuilt libraries (MSVC " .. tostring(manifest.msvc) .. ") are in " .. prebuiltThirdPartyDir .. ".")
		print("Generate with: premake5 vs2022 --thirdparty=prebuilt")
	end
}
