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
	description = "build: the configuration, e.g. Development or Release-Headless (default: Development-Default)"
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

-- The Visual Studio 2022 installation the vs2022 projects build with: the newest one with
-- MSBuild and the v143 C++ toolset. Both MSBuild and the toolset version come from it.
function visualStudio2022()
	local exe = path.join(os.getenv("ProgramFiles(x86)") or "C:/Program Files (x86)",
		"Microsoft Visual Studio/Installer/vswhere.exe")
	if not os.isfile(exe) then
		return nil
	end
	local output, code = os.outputof('"' .. winpath(exe) .. '" -latest -products * -version [17.0,18.0) ' ..
		'-requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath')
	return (code == 0 and output) and output:match("[^\r\n]+") or nil
end

-- The generation options this command was given, for the solution it (re)generates.
function generationOptions(bWithThirdParty)
	local arguments = {}
	local valued = { "msvc", "precision" }
	if bWithThirdParty then
		table.insert(valued, "thirdparty")
	end
	for _, name in ipairs(valued) do
		if _OPTIONS[name] then
			table.insert(arguments, "--" .. name .. "=" .. _OPTIONS[name])
		end
	end
	for _, name in ipairs({ "nopch", "fatal-warnings" }) do
		if _OPTIONS[name] then
			table.insert(arguments, "--" .. name)
		end
	end
	return table.concat(arguments, " ")
end

local function quoted(p)
	return '"' .. winpath(p) .. '"'
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
	local command = quoted(_PREMAKE_COMMAND) .. " --file=" .. quoted(rootpath("premake5.lua")) .. " vs2022 " ..
		generationOptions(true)
	if project then
		command = command .. " --project=" .. quoted(project.File)
	end
	if not runCommand(command) then
		fail("generating " .. (project and project.Name or "the engine") .. "'s solution failed")
	end
end

local function build(project)
	local configuration = _OPTIONS["config"] or "Development-Default"
	if not configuration:find("-", 1, true) then
		configuration = configuration .. "-Default"
	end
	if not table.contains(configs, configuration) then
		fail("build: no configuration " .. configuration .. " (" .. table.concat(configs, ", ") .. ")")
	end
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

-- The options this command was given, for the engine it hands the command to.
local function forwardedArguments()
	local arguments = {}
	for name, value in pairs(_OPTIONS) do
		if name ~= "file" and name ~= "project" then
			table.insert(arguments, value == "" and "--" .. name or '"--' .. name .. "=" .. value .. '"')
		end
	end
	table.sort(arguments)
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
-- building the template in place leaves behind.
local function isTemplateOnly(relative, templateName)
	local top = relative:match("^[^/]+")
	if top == "Template.ini" or top == templateName .. projectFileExtension then
		return true
	end
	if top == "Binaries" or top == "Intermediate" or top == "Saved" or top == ".vs" then
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
				runCommand(git .. " config -f .gitmodules submodule.Engine.url " .. url)
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
			if _OPTIONS["build"] and not runCommand(tool .. " build" .. arguments) then
				fail("new: building " .. name .. " failed")
			end
		else
			generate(project)
			if _OPTIONS["build"] then
				build(project)
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
			build(project)
		end
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
