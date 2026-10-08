ROOT_DIR = path.getabsolute(_SCRIPT_DIR or ".")

dofile("Premake/extensions.lua")
dofile("Premake/common.lua")
dofile("Premake/project.lua")
dofile("Premake/tools.lua")
dofile("Premake/actions.lua")
dofile("Premake/thirdparty.lua")

-- The rest declares the solution, which only the IDE actions write.
if not (_ACTION and _ACTION:match("^vs")) then
	return
end

-- --project: that project's solution (Premake/project.lua).
if _OPTIONS["project"] then
	targetProject = requireProject(_OPTIONS["project"])
end

dofile("Premake/workspace.lua")
if not bPrebuiltThirdParty then
	dofile("Premake/Projects/thirdparty.lua")
end
group (engineGroup)
dofile("Premake/Projects/core.lua")
dofile("Premake/Projects/core-test.lua")
dofile("Premake/Projects/launcher.lua")
dofile("Premake/Projects/editor.lua")
group ""

if targetProject then
	gameProject(targetProject)
end
