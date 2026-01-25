ROOT_DIR = path.getabsolute(_SCRIPT_DIR or ".")

dofile("Premake/common.lua")
dofile("Premake/actions.lua")
dofile("Premake/workspace.lua")
dofile("Premake/Projects/core.lua")
dofile("Premake/Projects/core-test.lua")
dofile("Premake/Projects/demo.lua")
