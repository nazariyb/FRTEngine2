ROOT_DIR = path.getabsolute(_SCRIPT_DIR or ".")

dofile("Premake/extensions.lua")
dofile("Premake/common.lua")
dofile("Premake/actions.lua")
dofile("Premake/workspace.lua")
dofile("Premake/thirdparty.lua")
if not bPrebuiltThirdParty then
	dofile("Premake/Projects/thirdparty.lua")
end
dofile("Premake/Projects/core.lua")
dofile("Premake/Projects/core-test.lua")
dofile("Premake/Projects/demo.lua")
dofile("Premake/Projects/launcher.lua")
dofile("Premake/Projects/editor.lua")
