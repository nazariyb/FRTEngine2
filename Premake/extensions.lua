-----------------------------------------
-----------  Premake extensions  --------
-----------------------------------------
-- MSBuild settings Premake 5.0.0-beta2 has no API for, added to the generated projects.

require("vstudio")

local vc = premake.vstudio.vc2010

-- A machine-wide `vcpkg integrate install` is imported into every C++ project on the machine
-- (%LOCALAPPDATA%\vcpkg\vcpkg.user.props). It adds its include directory to every compile, links
-- every library it has installed into every binary and runs a PowerShell copy step after each
-- link - hiding missing dependencies and costing seconds per build, including no-op builds.
-- Every dependency of this solution is part of it, so opt out.
premake.override(vc.elements, "globals", function (base, prj)
	local calls = base(prj)
	table.insert(calls, function ()
		vc.element("VcpkgEnabled", nil, "false")
	end)
	return calls
end)
