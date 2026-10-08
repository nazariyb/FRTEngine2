-- The engine's, or with --project that project's: a project's project files are in its
-- Intermediate, its solution at its root.
local function cleanRoot()
	if not _OPTIONS["project"] then
		return nil
	end
	local project, err = loadProject(_OPTIONS["project"])
	if not project then
		error(err, 0)   -- never the engine's files instead
	end
	return project
end

newaction
{
	trigger = "clean-all",
	description = "Clean generated files and compiled binaries (the engine's, or --project's)",
	execute = function ()
		local project = cleanRoot()
		local root = project and project.Dir or ROOT_DIR
		removeDirectory(path.join(root, "Binaries"))
		removeDirectory(path.join(root, "Intermediate"))

		local folders = project and { "." } or
		{
			".", "Core", "Core-Test", "Launcher", "Editor",
			"ThirdParty/Imgui", "ThirdParty/assimp", "ThirdParty/yaml-cpp", "ThirdParty/googletest",
		}
		for _, folder in ipairs(folders) do
			local dir = path.join(root, folder)
			os.remove(dir .. "/*.sln")
			os.remove(dir .. "/*.vcxproj")
			os.remove(dir .. "/*.vcxproj.filters")
			os.remove(dir .. "/*.vcxproj.user")
			os.remove(dir .. "/*.sln.DotSettings.user")
		end
	end
}

newaction
{
	trigger = "clean-compiled",
	description = "Clean only compiled binaries and intermediate files (the engine's, or --project's)",
	execute = function ()
		local project = cleanRoot()
		local root = project and project.Dir or ROOT_DIR
		removeDirectory(path.join(root, "Binaries"))
		removeDirectory(path.join(root, "Intermediate"))
	end
}
