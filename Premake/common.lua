triplet = "x64-windows" -- TODO: support for Linux

ROOT_DIR = ROOT_DIR or path.getabsolute(path.join(_SCRIPT_DIR, ".."))

function rootpath(relativePath)
	return path.join(ROOT_DIR, relativePath)
end

neededPackages =
{
	"assimp",
	"gtest"
}

thirdPartyDir = rootpath("ThirdParty")

vcpkgRoot = path.join(thirdPartyDir, "vcpkg")

function removeDirectory(dir)
	if not os.isdir(dir) then
		return
	end

	-- TODO
	-- if ??? == "windows" then
		os.execute(string.format("rmdir /s /q \"%s\"", dir))
	-- else
	-- 	os.execute(string.format("rm -rf \"%s\"", dir))
	-- end
end

buildConfigs = { "Debug", "Release" }
buildTargets = { "Default", "Headless" }
configs = {}

for _, cfg in ipairs(buildConfigs) do
	for _, tgt in ipairs(buildTargets) do
		table.insert(configs, cfg .. "-" .. tgt)
	end
end
