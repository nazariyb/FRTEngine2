newaction
{
	trigger = "install-deps",
	description = "Install vcpkg dependencies",
	execute = function ()
		-- Ensure all submodules are populated before trying to use them
		os.execute("git submodule update --init --recursive")

		local bootstrap = path.join(vcpkgRoot, "bootstrap-vcpkg.bat")
		os.execute(string.format("\"%s\"", bootstrap))
		for _, pkg in ipairs(neededPackages) do
			local vcpkgExe = path.join(vcpkgRoot, "vcpkg.exe")
			local command = string.format(
				"\"%s\" install %s:%s",
				vcpkgExe, pkg, triplet
			)
			print(command)
			os.execute(command)
		end
	end
}

newaction
{
	trigger = "clean-all",
	description = "Clean generated files and compiled binaries",
	execute = function ()
		removeDirectory(rootpath("Binaries"))
		removeDirectory(rootpath("Intermediate"))

		for _, folder in ipairs({ ".", "Core", "Core-Test", "Demo" }) do
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
