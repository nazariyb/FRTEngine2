triplet = "x64-windows" -- TODO: support for Linux

ROOT_DIR = ROOT_DIR or path.getabsolute(path.join(_SCRIPT_DIR, ".."))

function rootpath(relativePath)
	return path.join(ROOT_DIR, relativePath)
end

neededPackages =
{
	"assimp",
	"gtest",
	"yaml-cpp"
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

-----------------------------------------
--------  Real-number precision  --------
-----------------------------------------
-- Precision used for positions, distances, and other world-space measurements
-- (frt::real). Chosen per project through applyPrecision(), which defaults to the
-- workspace-wide value below.
--
-- WARNING: frt::real appears in signatures exported across the Core DLL boundary.
-- Every project linking Core must use the same value. A mismatch is caught at link
-- time by the guard symbol in Core/Source/Precision.h rather than silently
-- reinterpreting struct layouts.
--
-- Note this is CPU-side only. DirectXMath and every GPU-facing struct remain 32-bit;
-- double precision is for simulation and measurement, not for upload.

newoption
{
	trigger     = "precision",
	value       = "TYPE",
	description = "Real-number precision used for positions and measurements",
	allowed     =
	{
		{ "float",  "32-bit single precision (default)" },
		{ "double", "64-bit double precision" },
	},
	default     = "float"
}

precisionBits =
{
	float  = 32,
	double = 64,
}

defaultPrecision = _OPTIONS["precision"] or "float"

-- Emits the FRT_REAL_PRECISION define. Call from a project's defines section, in a
-- cleared filter scope. Pass "float" or "double" to override the workspace default.
function applyPrecision(which)
	local name = which or defaultPrecision
	local bits = precisionBits[name]

	if not bits then
		error(string.format(
			"applyPrecision: unknown precision '%s' (expected 'float' or 'double')",
			tostring(name)))
	end

	defines { "FRT_REAL_PRECISION=" .. bits }
end

buildConfigs = { "Debug", "Release" }
buildTargets = { "Default", "Headless" }
configs = {}

for _, cfg in ipairs(buildConfigs) do
	for _, tgt in ipairs(buildTargets) do
		table.insert(configs, cfg .. "-" .. tgt)
	end
end
