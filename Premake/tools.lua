-----------------------------------------
---------------  Tools  -----------------
-----------------------------------------
-- FRT.bat <command> [options]: the engine's command line, which the editor's project manager
-- runs too. In a project, the project's FRT.bat runs its engine's with --project=<the project>.
--
--   setup      registers this engine under its name (Engine.frtengine), so shared projects find
--              it, and generates its solution: the first thing to run in a new checkout
--   new        creates a project from a template, sets up its git repository, generates its
--              solution: FRT.bat new --name=MyGame [--template=Demo] [--layout=embedded] [--build]
--   generate   (re)generates the engine's solution, or the project's
--   build      builds the engine, or the project's game and the hosts (generating first if
--              there is no solution yet)
--   open       opens the solution in the program .sln files open with (Visual Studio)
--   package    builds the project's game in Release and writes it, with everything it runs
--              with, to a directory of its own that runs anywhere (Packaging, below)
--
-- A command for a project that uses another engine runs that engine's FRT.bat instead.

newoption
{
	trigger     = "name",
	value       = "NAME",
	description = "new: the project's name - letters, digits and underscores, starting with a letter"
}

newoption
{
	trigger     = "location",
	value       = "DIR",
	description = "new: the directory the project's directory is created in (default: the current directory)"
}

newoption
{
	trigger     = "template",
	value       = "NAME",
	description = "new: the template in Templates/ to start from (default: Blank)"
}

newoption
{
	trigger     = "layout",
	value       = "LAYOUT",
	description = "new: where the project's engine is",
	allowed     =
	{
		{ "shared",   "This checkout, shared with other projects and found by its registered name (default)" },
		{ "embedded", "A copy of this engine inside the project, a git submodule in Engine/, pinned per project" },
	},
	default     = "shared"
}

newoption
{
	trigger     = "engine-url",
	value       = "URL",
	description = "new --layout=embedded: the repository the engine submodule follows (default: this engine's origin)"
}

newoption
{
	trigger     = "config",
	value       = "CONFIGURATION",
	description = "build: the configuration, e.g. Development or Release-Headless (default: Development-Default); package: Release or Release-Headless"
}

newoption
{
	trigger     = "output",
	value       = "DIR",
	description = "package: the directory to write the packaged game to (default: <project>/Local/Packaged/<Name>-Win64)"
}

newoption
{
	trigger     = "build",
	description = "new: build the project once it is created"
}

-----------------------------------------
---------------  Helpers  ---------------
-----------------------------------------

-- os.execute through cmd.exe, which strips one pair of outer quotes.
function runCommand(command)
	return os.execute('"' .. command .. '"')
end

-- The premake5.exe running this, to run it again. Premake 5.0.0-beta2 cuts _PREMAKE_COMMAND short
-- when its path is not all ASCII (it ends the UTF-8 text at the UTF-16 length: D:\Ігри\premake5.exe
-- becomes D:\Ігри\premake5): then it is the engine's own.
function premakeExecutable()
	if os.isfile(_PREMAKE_COMMAND) then
		return _PREMAKE_COMMAND
	end
	return rootpath("ThirdParty/premake-5.0.0-beta2-windows/premake5.exe")
end

-- The Visual Studio 2022 installation the vs2022 projects build with: the newest one with
-- MSBuild and the v143 C++ toolset. Both MSBuild and the toolset version come from it. (-utf8:
-- the path in Premake's encoding, not the console's.)
function visualStudio2022()
	local exe = path.join(os.getenv("ProgramFiles(x86)") or "C:/Program Files (x86)",
		"Microsoft Visual Studio/Installer/vswhere.exe")
	if not os.isfile(exe) then
		return nil
	end
	local output, code = os.outputof('"' .. winpath(exe) .. '" -utf8 -latest -products * -version [17.0,18.0) ' ..
		'-requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath')
	return (code == 0 and output) and output:match("[^\r\n]+") or nil
end

-- A path as one command-line argument. Backslashes before the closing quote are doubled, or the
-- program reading it would take the last one as escaping the quote (a drive root, D:\).
local function quoted(p)
	local text = winpath(p)
	local trailing = text:match("\\*$")
	return '"' .. text .. trailing .. '"'
end

local function fail(message)
	error(message, 0)
end

-- A command's output, trimmed, when it succeeds.
local function outputOf(command)
	local output, code = os.outputof(command)
	if code ~= 0 or not output then
		return nil
	end
	return output:match("^%s*(.-)%s*$")
end

local function msbuild()
	local installation = visualStudio2022()
	local exe = installation and path.join(installation, "MSBuild/Current/Bin/amd64/MSBuild.exe")
	if not exe or not os.isfile(exe) then
		fail("no Visual Studio 2022 with MSBuild and the v143 C++ toolset found (vswhere)")
	end
	return exe
end

local function solutionFile(project)
	return project and path.join(project.Dir, project.Name .. ".sln") or rootpath("FRTEngine2.sln")
end

local function generate(project)
	local command = quoted(premakeExecutable()) .. " --file=" .. quoted(rootpath("premake5.lua")) .. " vs2022 " ..
		generationOptions(true)
	if project then
		command = command .. " --project=" .. quoted(project.File)
	end
	if not runCommand(command) then
		fail("generating " .. (project and project.Name or "the engine") .. "'s solution failed")
	end
end

-- The configuration --config names, Default when it names only a build type.
local function buildConfiguration(action, default)
	local name = _OPTIONS["config"] or default
	if not name:find("-", 1, true) then
		name = name .. "-Default"
	end
	if not table.contains(configs, name) then
		fail(action .. ": no configuration " .. name .. " (" .. table.concat(configs, ", ") .. ")")
	end
	return name
end

local function build(project, configuration)
	local solution = solutionFile(project)
	if not os.isfile(solution) then
		generate(project)
	end
	-- A project: its game, and what that depends on (the hosts, Core, the libraries). The engine:
	-- everything.
	local target = project and (" -t:" .. project.Module) or ""
	print("Building " .. (project and project.Name or "the engine") .. " (" .. configuration .. ")...")
	local command = string.format('%s %s%s -p:Configuration=%s -p:Platform=Win64 -m -nologo -v:minimal',
		quoted(msbuild()), quoted(solution), target, configuration)
	if not runCommand(command) then
		fail("building " .. (project and project.Name or "the engine") .. " (" .. configuration .. ") failed")
	end
end

-- The options on this command line, for the engine it hands the command to - as given, not as
-- this checkout's remembered options filled them in. Paths are made absolute: the other engine
-- runs in its own directory.
local pathOptions = { output = true }

local function forwardedArguments()
	local arguments = {}
	for _, argument in ipairs(_ARGV or {}) do
		local name = argument:match("^%-%-([%w%-]+)")
		if name and name ~= "file" and name ~= "project" then
			if pathOptions[name] and _OPTIONS[name] then
				argument = "--" .. name .. "=" .. canonicalPath(_OPTIONS[name])
			end
			table.insert(arguments, '"' .. argument .. '"')
		end
	end
	return table.concat(arguments, " ")
end

-- The project a command is for (--project), or nil for the engine. When the project uses another
-- engine, that engine's FRT.bat runs the command, and this returns nil, true.
local function commandProject(action)
	if not _OPTIONS["project"] then
		return nil
	end
	local project, err = loadProject(_OPTIONS["project"])
	if not project then
		fail(err)
	end
	local engineDir, engineErr = resolveEngineDir(project)
	if not engineDir then
		fail(project.File .. ": " .. engineErr)
	end
	if not isSamePath(engineDir, ROOT_DIR) then
		print(project.Name .. " uses the engine in " .. winpath(engineDir) .. ": running its FRT.bat.")
		if not runCommand(quoted(path.join(engineDir, "FRT.bat")) .. " " .. action .. " --project=" ..
			quoted(project.File) .. " " .. forwardedArguments()) then
			fail(action .. " failed")
		end
		return nil, true
	end
	return project
end

-----------------------------------------
--------------  Templates  --------------
-----------------------------------------
-- Templates/<Name>/: a project, with Template.ini ([Template] Description=...) saying what it is
-- for. It is a working project in place, inside the engine (nothing in its descriptor names an
-- engine, so it finds the one it is in), which keeps templates building with the engine. A new
-- project gets a copy with the template's name replaced by the project's - in file names, and in
-- the text of its code and config - plus Templates/Common/: what every project has.

templatesDir = rootpath("Templates")

-- Files whose text names the template (classes, the module, config).
local templatedExtensions =
{
	[".h"] = true, [".hpp"] = true, [".cpp"] = true, [".inl"] = true,
	[".ini"] = true, [".md"] = true, [".txt"] = true,
}

-- What is never copied: the template's own descriptor (the project gets its own), and what
-- building and opening the template in place leaves behind, IDE state included.
local function isTemplateOnly(relative, templateName)
	local top = relative:match("^[^/]+")
	if top == "Template.ini" or top == templateName .. projectFileExtension then
		return true
	end
	if top == "Binaries" or top == "Intermediate" or top == "Local" or top == ".vs" or top == ".idea" then
		return true
	end
	return relative:match("%.sln$") or relative:match("%.vcxproj") or relative:match("%.user$")
end

local function readBinary(file)
	local handle = io.open(file, "rb")
	if not handle then
		return nil
	end
	local data = handle:read("a")
	handle:close()
	return data
end

local function writeBinary(file, data)
	os.mkdir(path.getdirectory(file))
	local handle = io.open(file, "wb")
	if not handle then
		return false
	end
	handle:write(data)
	handle:close()
	return true
end

-- Templates/<Name> with a Template.ini: { Name, Dir, Description }.
function findTemplates()
	local templates = {}
	for _, file in ipairs(os.matchfiles(path.join(templatesDir, "*/Template.ini"))) do
		local dir = path.getdirectory(file)
		table.insert(templates,
		{
			Name = path.getname(dir),
			Dir = dir,
			Description = ini.find(ini.load(file), "Template", "Description") or "",
		})
	end
	table.sort(templates, function (a, b) return a.Name < b.Name end)
	return templates
end

local function copyTree(sourceDir, destinationDir, templateName, projectName)
	for _, file in ipairs(os.matchfiles(path.join(sourceDir, "**"))) do
		local relative = path.getrelative(sourceDir, file)
		if not (templateName and isTemplateOnly(relative, templateName)) then
			local data = readBinary(file) or fail("new: cannot read " .. file)
			if templateName then
				relative = relative:gsub(templateName, projectName)
				if templatedExtensions[path.getextension(file):lower()] then
					data = data:gsub(templateName, projectName)
				end
			end
			local destination = path.join(destinationDir, relative)
			if not writeBinary(destination, data) then
				fail("new: cannot write " .. destination)
			end
		end
	end
end

-----------------------------------------
--------------  Packaging  --------------
-----------------------------------------
-- FRT.bat package --project=<file> [--config=Release-Headless] [--output=<dir>]: the project's
-- game built in Release - one executable - and written with everything it runs with to a
-- directory that needs neither the engine checkout nor the project's sources: copy it anywhere
-- and run <Module>.exe. The layout, and how the game finds its files in it, is
-- Core/Source/Project/Project.h's (FindRunningPackage): keep the two in step.

local packageFileExtension = ".frtpackage"

-- Dir is Root, or inside it.
local function isInside(dir, root)
	local a, b = canonicalPath(dir):lower(), canonicalPath(root):lower()
	return a == b or a:sub(1, #b + 1) == b .. "\\"
end

local function packageMarkers(dir)
	return os.matchfiles(path.join(dir, "*" .. packageFileExtension))
end

-- What the packager writes at the top of a package - its directories, the marker, every
-- executable and DLL - goes before a package is written again, so nothing of the old one is left
-- behind (an asset deleted since, say). Anything else someone put in the directory stays.
local function removeOldPackage(dir)
	for _, name in ipairs({ "Content", "Config", "Engine", "D3D12" }) do
		local sub = path.join(dir, name)
		if os.isdir(sub) then
			os.rmdir(sub)
			if os.isdir(sub) then
				fail("package: cannot remove " .. winpath(sub) .. " - is the packaged game running?")
			end
		end
	end
	for _, pattern in ipairs({ "*.exe", "*.dll", "*" .. packageFileExtension }) do
		for _, file in ipairs(os.matchfiles(path.join(dir, pattern))) do
			if not os.remove(file) then
				fail("package: cannot remove " .. winpath(file) .. " - is the packaged game running?")
			end
		end
	end
end

-- The directory a package goes to: new, empty, or an older package, which is never anything the
-- package is made from. Nothing is written yet.
local function checkPackageDir(dir, project)
	if os.isfile(dir) then
		fail("package: " .. winpath(dir) .. " is a file")
	end
	local sources =
	{
		path.join(project.Dir, "Content"), path.join(project.Dir, "Config"), path.join(project.Dir, "Source"),
		rootpath("Core"),
	}
	for _, source in ipairs(sources) do
		if isInside(dir, source) then
			fail("package: " .. winpath(dir) .. " is inside " .. winpath(source) .. ", which the package is made from")
		end
	end
	local bEmpty = #os.matchfiles(path.join(dir, "*")) == 0 and #os.matchdirs(path.join(dir, "*")) == 0
	if os.isdir(dir) and not bEmpty and #packageMarkers(dir) == 0 then
		fail("package: " .. winpath(dir) .. " is not empty and is not a package: choose another --output, or empty it")
	end
end

local function copyFile(source, destination)
	os.mkdir(path.getdirectory(destination))
	local ok, err = os.copyfile(source, destination)
	if not ok then
		fail("package: cannot copy " .. winpath(source) .. " to " .. winpath(destination) .. (err and ": " .. err or ""))
	end
end

-- Every file under SourceDir, to the same place under DestinationDir.
local function copyFiles(sourceDir, destinationDir)
	for _, file in ipairs(os.matchfiles(path.join(sourceDir, "**"))) do
		copyFile(file, path.join(destinationDir, path.getrelative(sourceDir, file)))
	end
end

-- The package's marker: the project's descriptor, as the game reads it from the package.
local function writePackageMarker(dir, project, engine, configuration)
	local lines =
	{
		"; A packaged game, written by FRT.bat package: everything it runs with is in this directory,",
		"; which runs anywhere. Core/Source/Project/Project.h in the engine (FindRunningPackage) says more.",
		"[Project]",
		"Name=" .. project.Name,
		"Module=" .. project.Module,
		"",
		"; What built it.",
		"[Package]",
		"Engine=" .. engine.Name .. " " .. engine.Version,
		"Configuration=" .. configuration,
	}
	local file = path.join(dir, project.Name .. packageFileExtension)
	if not writeBinary(file, table.concat(lines, "\r\n") .. "\r\n") then
		fail("package: cannot write " .. winpath(file))
	end
end

-----------------------------------------
---------------  Actions  ---------------
-----------------------------------------

newaction
{
	trigger     = "setup",
	description = "Register this engine for projects to find by name, and generate its solution",
	execute     = function ()
		local engine = loadEngine(ROOT_DIR) or fail("setup: " .. rootpath(engineFileName) .. " has no [Engine] Name")
		local previous = findRegisteredEngine(engine.Name)
		if previous and not isSamePath(previous, ROOT_DIR) and isEngineDir(previous) then
			print("'" .. engine.Name .. "' was registered to " .. previous .. "; projects using it now use this checkout.")
		end
		if not registerEngine(engine.Name, ROOT_DIR) then
			fail("setup: cannot write to " .. userDataDir())
		end
		print("Registered " .. engine.Name .. " " .. engine.Version .. " in " .. winpath(ROOT_DIR) .. ".")
		generate(nil)
		print("")
		print("Open " .. winpath(solutionFile(nil)) .. " and start the editor to create or open a project,")
		print("or: FRT.bat new --name=MyGame [--template=Demo] [--layout=embedded] [--build]")
	end
}

newaction
{
	trigger     = "new",
	description = "Create a project from a template: FRT.bat new --name=<Name> [--template --location --layout --build]",
	execute     = function ()
		local name = _OPTIONS["name"] or fail("new: name the project with --name=<Name>")
		local valid, why = isValidProjectName(name)
		if not valid then
			fail("new: " .. why)
		end

		local templateName = _OPTIONS["template"] or "Blank"
		local template
		for _, candidate in ipairs(findTemplates()) do
			if candidate.Name:lower() == templateName:lower() then
				template = candidate
			end
		end
		if not template then
			local names = {}
			for _, candidate in ipairs(findTemplates()) do
				table.insert(names, candidate.Name)
			end
			fail("new: no template '" .. templateName .. "' (" .. table.concat(names, ", ") .. ")")
		end

		local location = path.getabsolute(_OPTIONS["location"] or os.getcwd())
		local projectDir = path.join(location, name)
		if os.isdir(projectDir) and (#os.matchfiles(path.join(projectDir, "*")) > 0 or #os.matchdirs(path.join(projectDir, "*")) > 0) then
			fail("new: " .. winpath(projectDir) .. " already exists and is not empty")
		end
		if os.isfile(projectDir) then
			fail("new: " .. winpath(projectDir) .. " is a file")
		end

		local engine = loadEngine(ROOT_DIR) or fail("new: " .. rootpath(engineFileName) .. " has no [Engine] Name")
		local layout = _OPTIONS["layout"] or "shared"
		local hasGit = outputOf("git --version") ~= nil
		if layout == "embedded" and not hasGit then
			fail("new --layout=embedded: the engine is a git submodule, and git is not on PATH")
		end

		-- Shared: the project names this engine, so the name must lead here.
		if layout == "shared" then
			local registered = findRegisteredEngine(engine.Name)
			if registered and isEngineDir(registered) and not isSamePath(registered, ROOT_DIR) then
				fail("new: the engine name '" .. engine.Name .. "' is registered to " .. registered ..
					", which projects naming it would use. Run FRT.bat setup here to make it this checkout, " ..
					"or create the project with that engine's FRT.bat.")
			end
			if not registered or not isSamePath(registered, ROOT_DIR) then
				registerEngine(engine.Name, ROOT_DIR)
				print("Registered " .. engine.Name .. " in " .. winpath(ROOT_DIR) .. ".")
			end
		end

		print("Creating " .. name .. " in " .. winpath(projectDir) .. " from the " .. template.Name .. " template...")
		copyTree(template.Dir, projectDir, template.Name, name)
		copyTree(path.join(templatesDir, "Common"), projectDir, nil, nil)

		local projectFile = path.join(projectDir, name .. projectFileExtension)
		local descriptor =
		{
			"; A FRTEngine project. Engine: a registered engine's name, or the engine's directory",
			"; relative to this file. Core/Source/Project/Project.h in the engine says more.",
			"[Project]",
			"Name=" .. name,
			"Engine=" .. (layout == "shared" and engine.Name or "Engine"),
		}
		if not writeBinary(projectFile, table.concat(descriptor, "\r\n") .. "\r\n") then
			fail("new: cannot write " .. projectFile)
		end

		local git = "git -C " .. quoted(projectDir)
		if hasGit then
			if not runCommand(git .. " init --quiet") then
				fail("new: git init failed in " .. winpath(projectDir))
			end
		else
			print("git is not on PATH: the project has no repository.")
		end

		if layout == "embedded" then
			-- Cloned from this checkout (fast, and what is checked out here), then pointed at the
			-- repository everyone else clones it from.
			local engineGit = "git -C " .. quoted(ROOT_DIR)
			local head = outputOf(engineGit .. " rev-parse HEAD")
			local url = _OPTIONS["engine-url"] or outputOf(engineGit .. " remote get-url origin")
			local source = head and winpath(ROOT_DIR) or url
			if not source then
				fail("new --layout=embedded: this engine is not a git checkout; pass --engine-url=<repository>")
			end
			print("Adding the engine as a submodule in Engine/ (from " .. source .. ")...")
			if not runCommand(git .. " -c protocol.file.allow=always submodule add --quiet " .. quoted(source) .. " Engine") then
				fail("new: git submodule add failed")
			end
			local submodule = "git -C " .. quoted(path.join(projectDir, "Engine"))
			if head then
				runCommand(submodule .. " -c advice.detachedHead=false checkout --quiet " .. head)
			end
			if url and source ~= url then
				runCommand(git .. ' config -f .gitmodules submodule.Engine.url "' .. url .. '"')
				runCommand(git .. " submodule --quiet sync")
			end
			if head then
				if outputOf(engineGit .. " status --porcelain") ~= "" then
					print("Note: the submodule has this engine's last commit (" .. head:sub(1, 10) .. "), not its uncommitted changes.")
				end
				if url and (outputOf(engineGit .. " branch -r --contains " .. head) or "") == "" then
					print("Note: commit " .. head:sub(1, 10) .. " is not on " .. url .. " yet: push it before sharing the project.")
				end
			end
		end

		if hasGit then
			runCommand(git .. " add --all")
		end
		addRecentProject(projectFile)

		-- Generated and built by the project's engine: for an embedded one, the submodule.
		local project = loadProject(projectFile)
		if layout == "embedded" then
			local tool = quoted(path.join(projectDir, "Engine/FRT.bat"))
			local arguments = " --project=" .. quoted(projectFile) .. " " .. generationOptions(true)
			if not runCommand(tool .. " generate" .. arguments) then
				fail("new: generating " .. name .. "'s solution failed")
			end
			local configuration = _OPTIONS["config"] and ' "--config=' .. _OPTIONS["config"] .. '"' or ""
			if _OPTIONS["build"] and not runCommand(tool .. " build" .. arguments .. configuration) then
				fail("new: building " .. name .. " failed")
			end
		else
			generate(project)
			if _OPTIONS["build"] then
				build(project, buildConfiguration("new", "Development-Default"))
			end
		end

		print("")
		print("Created " .. name .. ": " .. winpath(projectFile))
		print("  Solution: " .. winpath(solutionFile(project)) .. "  (FRT.bat open, in the project)")
		print("  Engine:   " .. (layout == "shared" and engine.Name .. ", shared: " .. winpath(ROOT_DIR) or
			"Engine/, a git submodule"))
		if hasGit then
			print("  A new git repository, with everything added: commit when ready.")
		end
	end
}

newaction
{
	trigger     = "generate",
	description = "Generate the engine's Visual Studio solution, or --project's",
	execute     = function ()
		local project, delegated = commandProject("generate")
		if not delegated then
			generate(project)
		end
	end
}

newaction
{
	trigger     = "build",
	description = "Build the engine, or --project, in --config (default: Development-Default)",
	execute     = function ()
		local project, delegated = commandProject("build")
		if not delegated then
			build(project, buildConfiguration("build", "Development-Default"))
		end
	end
}

newaction
{
	trigger     = "package",
	description = "Build --project in Release and write the game, with everything it runs with, to a directory of its own (--output)",
	execute     = function ()
		local project, delegated = commandProject("package")
		if delegated then
			return
		end
		if not project then
			fail("package: name the project with --project=<.frtproject file or its directory>")
		end
		local configuration = buildConfiguration("package", "Release-Default")
		if not configuration:match("^Release%-") then
			fail("package: only Release builds are packaged (--config=Release or Release-Headless): " ..
				"Debug and Development load the game as a DLL from the project")
		end
		local engine = loadEngine(ROOT_DIR) or fail("package: " .. rootpath(engineFileName) .. " has no [Engine] Name")
		local suffix = configuration == "Release-Default" and "" or "-" .. configuration:match("%-(.+)$")
		local dir = canonicalPath(_OPTIONS["output"] or
			path.join(project.Dir, "Local/Packaged/" .. project.Name .. "-Win64" .. suffix))
		checkPackageDir(dir, project)

		build(project, configuration)

		print("Packaging " .. project.Name .. " (" .. configuration .. ") in " .. winpath(dir) .. "...")
		removeOldPackage(dir)
		-- First, so an interrupted package is still one, and is replaced by the next.
		writePackageMarker(dir, project, engine, configuration)

		-- The game: Release-Default's plain name, else <Module>-<Configuration>.
		local binaries = path.join(project.Dir, "Binaries/Win64")
		local exe = configuration == "Release-Default" and project.Module or project.Module .. "-" .. configuration
		copyFile(path.join(binaries, exe .. ".exe"), path.join(dir, project.Module .. ".exe"))
		-- What the build put beside it (Premake/common.lua: copyRuntimeFiles).
		copyFile(path.join(binaries, "dxcompiler.dll"), path.join(dir, "dxcompiler.dll"))
		copyFile(path.join(binaries, "dxil.dll"), path.join(dir, "dxil.dll"))
		copyFiles(path.join(binaries, "D3D12"), path.join(dir, "D3D12"))

		copyFiles(path.join(project.Dir, "Content"), path.join(dir, "Content"))
		copyFiles(path.join(project.Dir, "Config"), path.join(dir, "Config"))

		copyFiles(rootpath("Core/Content/Shaders"), path.join(dir, "Engine/Content/Shaders"))
		copyFiles(rootpath("Core/Content/Fonts"), path.join(dir, "Engine/Content/Fonts"))
		copyFile(rootpath("Core/Config/Engine.ini"), path.join(dir, "Engine/Config/Engine.ini"))

		print("")
		print("Packaged " .. project.Name .. ": " .. winpath(path.join(dir, project.Module .. ".exe")))
		print("  Copy the directory anywhere to run it. While running, the game writes to")
		print("  %LOCALAPPDATA%\\" .. project.Name .. ", never to its own directory.")
	end
}

newaction
{
	trigger     = "open",
	description = "Open the engine's solution, or --project's, generating it first if needed",
	execute     = function ()
		local project, delegated = commandProject("open")
		if delegated then
			return
		end
		local solution = solutionFile(project)
		if not os.isfile(solution) then
			generate(project)
		end
		if project then
			addRecentProject(project.File)
		end
		runCommand('start "" ' .. quoted(solution))
	end
}
