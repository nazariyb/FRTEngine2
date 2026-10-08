newaction
{
	trigger = "clean-all",
	description = "Clean generated files and compiled binaries",
	execute = function ()
		removeDirectory(rootpath("Binaries"))
		removeDirectory(rootpath("Intermediate"))

		local folders =
		{
			".", "Core", "Core-Test", "Demo", "Launcher", "Editor",
			"ThirdParty/Imgui", "ThirdParty/assimp", "ThirdParty/yaml-cpp", "ThirdParty/googletest",
		}
		for _, folder in ipairs(folders) do
			os.remove(rootpath(folder.."/".."*.sln"))
			os.remove(rootpath(folder.."/".."*.vcxproj"))
			os.remove(rootpath(folder.."/".."*.vcxproj.filters"))
			os.remove(rootpath(folder.."/".."*.vcxproj.user"))
			os.remove(rootpath(folder.."/".."*.sln.DotSettings.user"))
		end
	end
}

newaction
{
	trigger = "clean-compiled",
	description = "Clean only compiled binaries and intermediate files",
	execute = function ()
		removeDirectory(rootpath("Binaries"))
		removeDirectory(rootpath("Intermediate"))
	end
}
