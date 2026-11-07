-----------------------------------------
---------------  Tools  -----------------
-----------------------------------------
-- FRT.bat <command> [options]: the engine's command line, which the editor's project manager
-- runs too. In a project, the project's FRT.bat runs its engine's with --project=<the project>.
--
--   setup      registers this engine under its name (Engine.frtengine), so shared projects find
--              it, gives the checkout's hashed text files LF line endings (Line endings, below)
--              and generates its solution: the first thing to run in a new checkout. With
--              --hooks[=off] it only turns the pre-commit hook on or off for one clone
--   new        creates a project from a template, sets up its git repository, generates its
--              solution: FRT.bat new --name=MyGame [--template=Demo] [--layout=embedded] [--build]
--              (--no-generate: no solution)
--   generate   (re)generates the engine's solution, or the project's
--   build      builds the engine, or the project's game and the hosts (generating first if
--              there is no solution yet)
--   open       opens the solution in the program .sln files open with (Visual Studio)
--   package    builds the project's game in Release and writes it, with everything it runs
--              with, to a directory of its own that runs anywhere (Packaging, below)
--   verify     the check before a merge: builds, Core-Test, lints, smoke runs and captures
--              compared with recorded baselines (Tools/Verify/Verify.py says what and how)
--   assets     the asset system's command line, AssetTool: FRT.bat assets <verb> [arguments]
--              for the project, or for the engine's own content (Assets, below)
--   check-builders
--              BUILD003: every builder whose sources changed has a new version GUID; with
--              --update, records the new source hashes (Builders, below; verify runs it)
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

newoption
{
	trigger     = "no-generate",
	description = "new: create the project and its repository, and generate no solution (FRT.bat generate does, later)"
}

newoption
{
	trigger     = "quick",
	description = "verify: only build Development, run its Core-Test and the lints (the pre-commit subset)"
}

newoption
{
	trigger     = "nightly",
	description = "verify: everything, plus the long-running checks (none exist yet)"
}

newoption
{
	trigger     = "update-goldens",
	description = "verify: write the registry's golden dumps again after a content change, show the diff, and run nothing else"
}

newoption
{
	trigger     = "record-baselines",
	description = "verify: record the captures as the new baselines instead of comparing with them"
}

newoption
{
	trigger     = "baselines",
	value       = "DIR",
	description = "verify: where the baseline captures are (default: Local/Verify/Baselines)"
}

newoption
{
	trigger     = "pt-tolerance",
	value       = "RMSE",
	description = "verify: the relative RMSE a path-traced capture may differ from its baseline by (default: 1e-4)"
}

newoption
{
	trigger     = "update",
	description = "check-builders: record the current source hash of every changed builder whose version GUID is new since HEAD"
}

newoption
{
	trigger     = "hooks",
	description = "setup: only turn the pre-commit hook on for this clone, or --project's (--hooks=off: off), and change nothing else"
}

-----------------------------------------
---------------  Helpers  ---------------
-----------------------------------------

-- os.execute through cmd.exe, which strips one pair of outer quotes - and parses the rest: it
-- expands %NAME% and reads & | < > ^. Text that is not this script's own goes in escaped for it
-- (FRT.bat assets: argumentText).
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

-- Python 3 for Tools/: the newest through the py launcher, else the python on PATH.
local function python()
	for _, candidate in ipairs({ "py -3", "python" }) do
		local version = outputOf(candidate .. " --version")
		if version and version:match("^Python 3%.") then
			return candidate
		end
	end
	return nil
end

local function solutionFile(project)
	return project and path.join(project.Dir, project.Name .. ".sln") or rootpath("FRTEngine2.sln")
end

-- With bToStderr, generate and build print to standard error, and so do the commands they run: for
-- a command whose standard output is data (FRT.bat assets --json).
local function say(bToStderr, text)
	if bToStderr then
		io.stderr:write(text .. "\n")
	else
		print(text)
	end
end

local function redirection(bToStderr)
	return bToStderr and " 1>&2" or ""
end

local function generate(project, bToStderr)
	local command = quoted(premakeExecutable()) .. " --file=" .. quoted(rootpath("premake5.lua")) .. " vs2022 " ..
		generationOptions(true)
	if project then
		command = command .. " --project=" .. quoted(project.File)
	end
	if not runCommand(command .. redirection(bToStderr)) then
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

-- EngineTarget: with no project, one of the engine's projects and what it depends on, instead of
-- the whole engine.
local function build(project, configuration, engineTarget, bToStderr)
	local solution = solutionFile(project)
	if not os.isfile(solution) then
		generate(project, bToStderr)
	end
	-- A project: its game, and what that depends on (the hosts, Core, the libraries). The engine:
	-- everything.
	local what = project and project.Name or (engineTarget or "the engine")
	local target = project and (" -t:" .. project.Module) or (engineTarget and " -t:" .. engineTarget or "")
	say(bToStderr, "Building " .. what .. " (" .. configuration .. ")...")
	local command = string.format('%s %s%s -p:Configuration=%s -p:Platform=Win64 -m -nologo -v:minimal',
		quoted(msbuild()), quoted(solution), target, configuration)
	if not runCommand(command .. redirection(bToStderr)) then
		fail("building " .. what .. " (" .. configuration .. ") failed")
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

-- The extensions a project's .gitattributes (Templates/Common's, which every project gets) makes
-- text eol=lf: "*.ini text eol=lf" gives ".ini". A new project's repository holds those files as
-- LF in its index, and nothing ever checks a new project out again, so its working copies must be
-- LF from the start, or they would hash differently from every clone's. The engine checkout they
-- are copied from may still have them with CRLF: one checked out before .gitattributes made them
-- LF, which has not run FRT.bat setup yet.
local function lfExtensions()
	local extensions = {}
	local rules = readBinary(path.join(templatesDir, "Common/.gitattributes")) or ""
	for line in rules:gmatch("[^\r\n]+") do
		local extension, attributes = line:match("^%*(%.[^%s/%*%?%[]+)%s+(.*)$")
		if extension and (" " .. attributes .. " "):find(" eol=lf ", 1, true) then
			extensions[extension:lower()] = true
		end
	end
	return extensions
end

-- The template's name in a file's path becomes the project's, and so a source and its meta
-- (X.png and X.png.meta, design §13.3) are renamed alike: the meta's name is its source's with
-- ".meta", and the rename touches the part they share. A meta's bytes are copied as they are but
-- for LF (.gitattributes says eol=lf):
-- ".meta" is no templated extension, so its GUID and paths are never rewritten (projects made
-- from one template share asset GUIDs, which is fine: §13.3). Schemas/ comes along, so a new
-- project has its template's Schemas/Migrations.yml and is not behind the engine (MIG001).
local function copyTree(sourceDir, destinationDir, templateName, projectName)
	local lf = lfExtensions()
	for _, file in ipairs(os.matchfiles(path.join(sourceDir, "**"))) do
		local relative = path.getrelative(sourceDir, file)
		if not (templateName and isTemplateOnly(relative, templateName)) then
			local data = readBinary(file) or fail("new: cannot read " .. file)
			if lf[path.getextension(file):lower()] then
				data = data:gsub("\r\n", "\n")
			end
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

-- What no package holds, whatever tree it is copied from: a source's .meta (design §3.2: the
-- runtime loads by path and never reads one; ids reach a package through the cook, M12), in
-- any case, as the asset system's built-in exclude matches it.
local packageSkippedSuffixes = { ".meta" }

-- Every file under SourceDir, to the same place under DestinationDir, except those whose path
-- relative to SourceDir starts with one of the prefixes in Skipped ({ "Bin/" }), and those whose
-- name ends in one of the suffixes in SkippedSuffixes ({ ".meta" }, in any case), when given.
local function copyFiles(sourceDir, destinationDir, skipped, skippedSuffixes)
	for _, file in ipairs(os.matchfiles(path.join(sourceDir, "**"))) do
		local relative = path.getrelative(sourceDir, file)
		local skip = false
		for _, prefix in ipairs(skipped or {}) do
			if relative:sub(1, #prefix) == prefix then
				skip = true
			end
		end
		local lower = relative:lower()
		for _, suffix in ipairs(skippedSuffixes or {}) do
			if #lower > #suffix and lower:sub(-#suffix) == suffix:lower() then
				skip = true
			end
		end
		if not skip then
			copyFile(file, path.join(destinationDir, relative))
		end
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
-------------  Line endings  ------------
-----------------------------------------
-- The engine hashes text files byte for byte - asset metadata, documents, rules, shader sources -
-- so every working tree must hold them with the same bytes: LF, as .gitattributes says. The index
-- already has them as LF. Working trees made with core.autocrlf=true (Git for Windows' default)
-- have CRLF, and a new .gitattributes converts nothing already checked out: that takes a fresh
-- checkout of those files. Setup finds the files whose working copy is not what a checkout under
-- today's attributes writes (git ls-files --eol), checks them out again, and checks the result.
--
-- Only those files: checking out everything again (git rm --cached -r . && git reset --hard)
-- would rewrite premake5.exe while it runs this, and every source, so the next build would
-- rebuild everything. Only on a working tree without uncommitted changes, which a fresh checkout
-- would throw away; changes in line endings alone are not changes. Each worktree of a repository
-- has its own working files, and runs setup once. FRT.bat check-line-endings checks the same rules
-- without fixing anything, for FRT.bat verify: one implementation of them, here.

-- core.safecrlf=false: no warning about the conversions core.autocrlf makes, which are the point.
local function git(arguments)
	return "git -c core.safecrlf=false -C " .. quoted(ROOT_DIR) .. " " .. arguments
end

-- git ls-files --eol for this checkout: { Path, Index, Work, Attributes } per tracked file.
local function trackedLineEndings()
	local output, code = os.outputof(git("ls-files --eol -z"))
	if code ~= 0 or not output then
		return nil
	end
	local entries = {}
	for record in output:gmatch("[^\0]+") do
		local info, file = record:match("^([^\t]*)\t(.*)$")
		local index, work, attributes = (info or ""):match("^i/(%S*)%s+w/(%S*)%s+attr/(.-)%s*$")
		if file and index then
			table.insert(entries, { Path = file, Index = index, Work = work, Attributes = attributes })
		end
	end
	return entries
end

-- What is wrong with a tracked file's line endings: "index" (only a commit renormalizing it fixes
-- that), "work" (a fresh checkout of the file fixes it), or nil.
local function lineEndingProblem(entry)
	local attributes = " " .. entry.Attributes .. " "
	if attributes:find(" eol=lf ", 1, true) then
		if entry.Index == "crlf" or entry.Index == "mixed" then
			return "index"
		end
		if entry.Work == "crlf" or entry.Work == "mixed" then
			return "work"
		end
	elseif attributes:find(" -text ", 1, true) then
		-- Never converted: the working copy has the committed bytes.
		if entry.Work ~= "" and entry.Index ~= "" and entry.Work ~= entry.Index then
			return "work"
		end
	elseif attributes:find(" eol=crlf ", 1, true) then
		if entry.Index == "lf" and entry.Work == "lf" then
			return "work"
		end
	end
	return nil
end

-- The tracked files with a problem of each kind: { work = { paths }, index = { paths } }.
local function lineEndingProblems(entries)
	local problems = { work = {}, index = {} }
	for _, entry in ipairs(entries) do
		local problem = lineEndingProblem(entry)
		if problem then
			table.insert(problems[problem], entry.Path)
		end
	end
	return problems
end

local function printSome(paths, indent)
	for i = 1, math.min(#paths, 10) do
		print(indent .. paths[i])
	end
	if #paths > 10 then
		print(indent .. "... and " .. (#paths - 10) .. " more")
	end
end

-- The other worktrees of this checkout's repository.
local function otherWorktrees()
	local worktrees = {}
	for line in (outputOf(git("worktree list --porcelain")) or ""):gmatch("[^\r\n]+") do
		local dir = line:match("^worktree (.+)$")
		if dir and not isSamePath(dir, ROOT_DIR) then
			table.insert(worktrees, winpath(dir))
		end
	end
	return worktrees
end

-- Gives this checkout's working files the line endings .gitattributes asks for. False, having
-- said why, when that could not be done.
local function setupLineEndings()
	if not outputOf("git --version") then
		print("Line endings: git is not on PATH, so nothing is checked.")
		return true
	end
	-- The engine's path below the top of its work tree: empty for a checkout of its own. Asked of
	-- git, so it holds however the path is spelled (a subst or mapped drive, a junction), which a
	-- comparison of git's --show-toplevel with ROOT_DIR would not.
	local prefix = outputOf(git("rev-parse --show-prefix"))
	if not prefix then
		print("Line endings: this engine is not a git checkout, so nothing is checked.")
		return true
	end
	if prefix ~= "" then
		print("Line endings: this engine is " .. prefix .. " in another repository's work tree, not a checkout")
		print("of its own, so nothing is checked.")
		return true
	end
	local entries = trackedLineEndings()
	if not entries then
		print("Line endings: git ls-files --eol failed.")
		return false
	end

	local problems = lineEndingProblems(entries)
	if #problems.work > 0 then
		-- "git diff --quiet" with a whitespace option compares contents, so line endings alone
		-- leave it quiet.
		if not runCommand(git("diff HEAD --ignore-cr-at-eol --quiet")) then
			print("Line endings: " .. #problems.work .. " file(s) need a fresh checkout for the line endings " ..
				".gitattributes asks for,")
			print("but this working tree has uncommitted changes, which that would overwrite. Commit or stash")
			print("them, then run FRT.bat setup again. git status:")
			for line in (os.outputof(git("status --short --untracked-files=no")) or ""):gmatch("[^\r\n]+") do
				print("  " .. line)
			end
			return false
		end

		print("Line endings: checking out " .. #problems.work .. " file(s) again, for the line endings " ..
			".gitattributes asks for...")
		local list = path.join(ROOT_DIR, "Local/Setup/LineEndings.txt")
		if not writeBinary(list, table.concat(problems.work, "\0") .. "\0") then
			fail("setup: cannot write " .. winpath(list))
		end
		local files = " --pathspec-from-file=" .. quoted(list) .. " --pathspec-file-nul"
		-- Out of the index first: git writes no file whose index entry says it is up to date,
		-- whatever its line endings. Then from HEAD into the index and the working tree, those
		-- files only.
		local ok = runCommand(git("--literal-pathspecs rm --cached -q" .. files)) and
			runCommand(git("--literal-pathspecs checkout -q HEAD" .. files))
		if not ok then
			-- Their index entries go back whatever was or was not checked out, or the files would
			-- stay staged as deleted: a change, which the next run would refuse to overwrite (and
			-- which a commit would make real). Their working copies are as they were or rewritten,
			-- and either way hold the committed text, so the tree is clean again.
			if not runCommand(git("--literal-pathspecs reset -q HEAD" .. files)) then
				print("Line endings: the checkout failed, and so did putting the files back into the index. Run")
				print("  git -C " .. quoted(ROOT_DIR) .. " --literal-pathspecs reset -q HEAD" .. files)
				print("before anything else, then FRT.bat setup again.")
				return false
			end
			os.remove(list)
			print("Line endings: the checkout failed (a file open in another program?); nothing is staged. Close")
			print("the program and run FRT.bat setup again.")
			return false
		end
		os.remove(list)

		entries = trackedLineEndings() or {}
		problems = lineEndingProblems(entries)
		local worktrees = otherWorktrees()
		if #worktrees > 0 then
			print("Each worktree has its own working files: run FRT.bat setup in the others too (git worktree list):")
			printSome(worktrees, "  ")
		end
	end

	if #problems.work > 0 then
		print("Line endings: these files still do not have the line endings .gitattributes asks for:")
		printSome(problems.work, "  ")
		return false
	end
	if #problems.index > 0 then
		print("Line endings: these files are committed with CRLF, which .gitattributes forbids; fix them in a commit")
		print("(git add --renormalize <file>):")
		printSome(problems.index, "  ")
		return false
	end
	print("Line endings: every hashed text file is LF (git ls-files --eol).")
	return true
end

-- How many tracked files .gitattributes makes LF.
local function countLfFiles(entries)
	local count = 0
	for _, entry in ipairs(entries) do
		if (" " .. entry.Attributes .. " "):find(" eol=lf ", 1, true) then
			count = count + 1
		end
	end
	return count
end

-----------------------------------------
---------------  Builders  --------------
-----------------------------------------
-- The builder source guard (design 7.3; the table and its rule: Core/Source/Assets/BuilderVersions.inl).
-- A builder's version GUID promises that equal keys mean equal outputs, so whoever changes a
-- builder's sources must give it a new one. Two commands keep that promise, here because they must
-- run where no Python is (every build) and need nothing but Premake:
--
--   builder-hashes   hashes each builder's sources into Intermediate/Generated/BuilderSourceHashes.gen.h
--                    (FRT_BUILDER_SOURCE_HASH(Name, "hex") per builder), which development builds
--                    compare with the committed hashes (Core/Source/Assets/BuilderGuard.h), and lists
--                    every matched file in Local/Build/PipelineSources.txt for the pipeline lint
--                    (Tools/Lint/CheckPipeline.py). Core's prebuild step runs it before every build,
--                    not only at FRT.bat generate, so the hashes are those of the sources being built.
--                    Each file is written only when its content changes, so an unchanged hash
--                    recompiles nothing.
--   check-builders   BUILD003: fails, naming them, when a builder's sources no longer match its
--                    committed hash while its version GUID is the one HEAD has, and when a version
--                    GUID was ever recorded with another source hash than the table now pairs it with.
--                    With --update it records the current hashes instead - only when every changed
--                    builder has a GUID that differs from HEAD's (a new builder counts as one) and was
--                    never recorded with another hash, and refuses otherwise, writing nothing. FRT.bat
--                    verify runs it, in --quick too.
--
-- One GUID, one hash: a GUID stands for one version of its builder's code. Outputs built while the
-- table paired it with hash X are keyed by that GUID alone, in a store every worktree of the machine
-- shares; pairing it with hash Y later would serve them for Y's code. So a GUID is recorded once:
-- --update notes every pair it writes in Local/Build/BuilderVersionHashes.txt (this checkout's record
-- of what it recorded, committed or not), and every commit git knows, on any branch, is a record too
-- (git log --all -S<guid> of the table: the commits that added or removed the GUID, each read whole).
-- A hash written by hand, a GUID run through --update twice, or a merge that keeps another branch's
-- GUID for different code all pair a recorded GUID with a second hash: BUILD003.
--
-- The source hash of a builder: 64-bit FNV-1a, as 16 lowercase hex digits, over every file its globs
-- match and every file of the key infrastructure (keyInfrastructureGlobs, part of every builder), in
-- byte order of their engine-relative paths ('/'-separated), each as
--     path, NUL, decimal length of its content, NUL, its content
-- with every carriage return removed from the content first, so a CRLF checkout and an LF one agree.
-- Paths are part of it: a rename changes it. It only detects change: no key ever contains it, except
-- as the local-only discriminator of the builder whose sources changed.

local builderTableFile = "Core/Source/Assets/BuilderVersions.inl"
local builderHashesFile = "Intermediate/Generated/BuilderSourceHashes.gen.h"
local pipelineSourcesFile = "Local/Build/PipelineSources.txt"
local builderRecordFile = "Local/Build/BuilderVersionHashes.txt"

-- The sources of every builder besides its own globs: the key encoder (CKeyWriter, and its byte
-- encoding in ByteStream.h) and the header every builder writes its key's first fields and its
-- local-only discriminator with (BuilderGuard.h). A change there can change or collapse the KeyBlob
-- bytes of every builder, so it changes every builder's source hash, and the pipeline lint checks
-- these files too. One list, so that no builder's row can forget it.
local keyInfrastructureGlobs =
{
	"Core/Source/Assets/KeyWriter.*",
	"Core/Source/Assets/ByteStream.h",
	"Core/Source/Assets/BuilderGuard.h",
}

-- Text with its // and /* */ comments replaced by spaces (line breaks kept), so every byte stays
-- where it was: string literals stay, a "//" inside one included.
local function blankComments(text)
	local parts = {}
	local i, n = 1, #text
	local plainStart = 1
	while i <= n do
		local c = text:byte(i)
		if c == 34 then   -- "
			local j = i + 1
			while j <= n do
				local d = text:byte(j)
				if d == 92 then       -- a backslash escapes the next byte
					j = j + 2
				elseif d == 34 or d == 10 then
					break
				else
					j = j + 1
				end
			end
			i = j + 1
		elseif c == 47 and text:byte(i + 1) == 47 then   -- //
			local j = (text:find("\n", i, true) or (n + 1)) - 1
			table.insert(parts, text:sub(plainStart, i - 1))
			table.insert(parts, string.rep(" ", j - i + 1))
			i = j + 1
			plainStart = i
		elseif c == 47 and text:byte(i + 1) == 42 then   -- /*
			local close = text:find("*/", i + 2, true)
			local j = close and close + 1 or n
			table.insert(parts, text:sub(plainStart, i - 1))
			table.insert(parts, (text:sub(i, j):gsub("[^\n]", " ")))
			i = j + 1
			plainStart = i
		else
			i = i + 1
		end
	end
	table.insert(parts, text:sub(plainStart))
	return table.concat(parts)
end

local function lineOf(text, position)
	local _, count = text:sub(1, position):gsub("\n", "")
	return count + 1
end

-- The builder table's rows from Text (the .inl file's content): { Name, Guid, Hash, Globs (a list),
-- HashFirst, HashLast (the byte range of the committed hash's digits in Text), Line }, in table order.
-- Nil and the reason when a row is not as the table's header comment says. Where names the file in
-- messages.
local function parseBuilderTable(text, where)
	local code = blankComments(text)
	local rows = {}
	local names = {}
	local position = 1
	while true do
		local first, last = code:find("FRT_BUILDER%s*%(", position)
		if not first then
			break
		end
		local line = lineOf(text, first)
		local function rowError(why)
			return nil, string.format("%s:%d: %s", where, line, why)
		end
		-- The arguments: groups of adjacent string literals, separated by commas, up to ')'.
		local arguments = {}
		local cursor = last + 1
		while true do
			local literals = {}
			while true do
				local quote = code:find("^%s*\"", cursor)
				if not quote then
					break
				end
				local open = code:find("\"", cursor, true)
				local close = code:find("[\"\n\\]", open + 1)
				if not close or code:byte(close) ~= 34 then
					return rowError("a string literal that does not end on its line, or has an escape (none are allowed)")
				end
				table.insert(literals, { Text = code:sub(open + 1, close - 1), First = open + 1, Last = close - 1 })
				cursor = close + 1
			end
			if #literals == 0 then
				return rowError("every argument of FRT_BUILDER is a string literal")
			end
			table.insert(arguments, literals)
			local separator = code:match("^%s*([,%)])", cursor)
			if not separator then
				return rowError("expected ',' or ')' after argument " .. #arguments)
			end
			cursor = code:find(separator, cursor, true) + 1
			if separator == ")" then
				break
			end
		end
		position = cursor
		if #arguments ~= 4 then
			return rowError("FRT_BUILDER takes 4 arguments (Name, VersionGuid, CommittedSourceHash, globs), not " .. #arguments)
		end
		local function joined(literals)
			local texts = {}
			for _, literal in ipairs(literals) do
				table.insert(texts, literal.Text)
			end
			return table.concat(texts)
		end
		local row =
		{
			Name = joined(arguments[1]),
			Guid = joined(arguments[2]):lower(),
			Hash = joined(arguments[3]):lower(),
			Globs = {},
			Line = line,
		}
		if row.Name == "" then
			return rowError("a builder without a name")
		end
		if names[row.Name] then
			return rowError("builder " .. row.Name .. " twice")
		end
		names[row.Name] = true
		if not row.Guid:match("^" .. string.rep("%x", 32) .. "$") or row.Guid == string.rep("0", 32) then
			return rowError(row.Name .. ": the version GUID is not 32 hex digits, not all zero (FRT.bat assets newguid)")
		end
		if #arguments[3] ~= 1 or not row.Hash:match("^" .. string.rep("%x", 16) .. "$") then
			return rowError(row.Name .. ": the committed source hash is not one literal of 16 hex digits")
		end
		row.HashFirst, row.HashLast = arguments[3][1].First, arguments[3][1].Last
		for glob in joined(arguments[4]):gmatch("[^;]+") do
			glob = glob:match("^%s*(.-)%s*$")
			if glob ~= "" then
				if path.isabsolute(glob) or glob:find("..", 1, true) then
					return rowError(row.Name .. ": a glob outside the engine: " .. glob)
				end
				table.insert(row.Globs, glob)
			end
		end
		if #row.Globs == 0 then
			return rowError(row.Name .. ": no globs, so no sources to guard")
		end
		table.insert(rows, row)
	end
	if #rows == 0 then
		return nil, where .. ": no FRT_BUILDER rows"
	end
	return rows
end

-- The engine's builder table: its text and rows. Fails the command when it cannot be read or parsed.
local function readBuilderTable(action)
	local text = readBinary(rootpath(builderTableFile)) or fail(action .. ": cannot read " .. builderTableFile)
	local rows, err = parseBuilderTable(text, builderTableFile)
	if not rows then
		fail(action .. ": " .. err)
	end
	return text, rows
end

-- Byte order (memcmp), whatever the C library's collation: Lua compares strings with strcoll.
local function isBytewiseLess(a, b)
	for i = 1, math.min(#a, #b) do
		local x, y = a:byte(i), b:byte(i)
		if x ~= y then
			return x < y
		end
	end
	return #a < #b
end

-- The engine-relative paths of the files a builder's globs and the key infrastructure's match, each
-- once, in byte order.
local function builderSources(row)
	local files, seen = {}, {}
	for _, globs in ipairs({ row.Globs, keyInfrastructureGlobs }) do
		for _, glob in ipairs(globs) do
			for _, file in ipairs(os.matchfiles(rootpath(glob))) do
				local relative = path.getrelative(ROOT_DIR, file)
				if not seen[relative] then
					seen[relative] = true
					table.insert(files, relative)
				end
			end
		end
	end
	table.sort(files, isBytewiseLess)
	return files
end

local function fnv1a64(hash, text)
	for i = 1, #text do
		hash = (hash ~ text:byte(i)) * 0x100000001b3
	end
	return hash
end

-- The source hash of Files (engine-relative paths in byte order; see the section's comment).
local function hashBuilderSources(action, files)
	local hash = 0xcbf29ce484222325
	for _, file in ipairs(files) do
		local content = readBinary(rootpath(file)) or fail(action .. ": cannot read " .. file)
		content = content:gsub("\r", "")
		hash = fnv1a64(hash, file .. "\0" .. tostring(#content) .. "\0")
		hash = fnv1a64(hash, content)
	end
	return string.format("%016x", hash)
end

-- Replaces the file at Target with Data in one step, so a reader sees the old content or the new and
-- never a truncated file: every solution that builds Core runs the builder-hashes prebuild step in the
-- same engine, and another build's cl.exe may be reading the header, or the pipeline lint the list,
-- while one of them writes. Data goes to a file of its own beside Target, which then takes Target's
-- place in one rename: cmd's move /y (MoveFileEx with MOVEFILE_REPLACE_EXISTING), as Lua's os.rename
-- refuses an existing target. A reader that holds Target open without delete sharing (cl.exe does)
-- makes the rename fail until it lets go: retried for a few seconds. False when it never succeeds.
local function replaceBinary(target, data)
	local temporary = target .. "." .. os.uuid() .. ".tmp"
	if not writeBinary(temporary, data) then
		return false
	end
	local move = "move /y " .. quoted(temporary) .. " " .. quoted(target) .. " >nul 2>&1"
	-- os.clock is the time since the process started (the Windows C library's clock()).
	local deadline = os.clock() + 5
	while not runCommand(move) do
		if os.clock() >= deadline then
			os.remove(temporary)
			return false
		end
		local resume = os.clock() + 0.05
		while os.clock() < resume do
		end
	end
	return true
end

-- Writes Text to the engine-relative File unless it already holds exactly that, replacing it in one
-- step (replaceBinary). True when written.
local function writeIfChanged(action, file, text)
	if readBinary(rootpath(file)) == text then
		return false
	end
	if not replaceBinary(rootpath(file), text) then
		fail(action .. ": cannot write " .. file)
	end
	return true
end

-- The builder table as HEAD has it: its rows by name; an empty table when HEAD has no table (every
-- builder is new); nil when there is no HEAD to ask (not a git checkout, no commit yet). "HEAD:./"
-- names the file relative to the engine, wherever the engine is in its work tree.
local function headBuilderTable(action)
	if not outputOf(git("rev-parse --verify --quiet HEAD") .. " 2>nul") then
		return nil
	end
	local text, code = os.outputof(git("show HEAD:./" .. builderTableFile) .. " 2>nul")
	if code ~= 0 or not text then
		return {}
	end
	local rows, err = parseBuilderTable(text, "HEAD:" .. builderTableFile)
	if not rows then
		fail(action .. ": " .. err)
	end
	local byName = {}
	for _, row in ipairs(rows) do
		byName[row.Name] = row
	end
	return byName
end

-- Every source hash a version GUID of Rows was ever recorded with (One GUID, one hash, above), as
-- { [guid] = { [hash] = where } }, where naming the record: this checkout's record file or a commit.
-- Outside a git checkout only the file counts. Each table git has is read once, and a table that does
-- not parse is skipped: it records nothing.
local function recordedBuilderHashes(rows)
	local recorded = {}
	local function note(guid, hash, where)
		recorded[guid] = recorded[guid] or {}
		recorded[guid][hash] = recorded[guid][hash] or where
	end
	for guid, hash in (readBinary(rootpath(builderRecordFile)) or ""):gmatch("(%x+) (%x+)[^\n]*") do
		note(guid:lower(), hash:lower(), builderRecordFile)
	end
	local tables = {}
	for _, row in ipairs(rows) do
		local commits = outputOf(git("log --all --format=%H -S" .. row.Guid .. " -- " .. quoted(builderTableFile)) .. " 2>nul")
		for commit in (commits or ""):gmatch("%x+") do
			if tables[commit] == nil then
				local text, code = os.outputof(git("show " .. commit .. ":./" .. builderTableFile) .. " 2>nul")
				tables[commit] = code == 0 and text and parseBuilderTable(text, commit) or false
			end
			for _, committed in ipairs(tables[commit] or {}) do
				note(committed.Guid, committed.Hash, "commit " .. commit:sub(1, 10))
			end
		end
	end
	return recorded
end

-- The hashes other than Hash that Guid was recorded with, as "hash (where)" texts, sorted.
local function otherRecordedHashes(recorded, guid, hash)
	local others = {}
	for other, where in pairs(recorded[guid] or {}) do
		if other ~= hash then
			table.insert(others, other .. " (" .. where .. ")")
		end
	end
	table.sort(others)
	return others
end

-- Actions that run inside a build print nothing unless they change something: Premake's own
-- "Running action '...'" line (its premake.main.preAction step) is replaced with one that prints nothing.
local function silencePreAction()
	local main = premake.main
	for i, step in ipairs(main and main.elements or {}) do
		if step == main.preAction then
			main.elements[i] = function () end
		end
	end
end

if _ACTION == "builder-hashes" then
	silencePreAction()
end

-----------------------------------------
---------------  Assets  ----------------
-----------------------------------------
-- FRT.bat assets <verb> [arguments] [options]: AssetTool (AssetTool/Source/AssetTool.cpp says what
-- it does), for a project or, without one, for the engine itself, as the owner of /Engine
-- (--engine). The project is named as for every command, --project=<file>, or as AssetTool names
-- it, which reaches this script as a plain argument: -Project=<file> (- or /, any case) or a bare
-- *.frtproject argument (assetToolArguments). Everything else after the action reaches AssetTool
-- with the same text - the verb, its arguments, -Section:Key=Value overrides, --options - although
-- cmd.exe parses the command line on the way (argumentText keeps it from changing anything), and
-- AssetTool's exit code is this command's: 0 success, 1 problems found, 2 usage error, 3 internal
-- error, 4 the project is owned by another process. The Development build runs, built first when it
-- is missing or older than its sources or Core's; if that build fails while an earlier one is there,
-- the earlier one runs, with a warning (an open editor holds Core's DLL, and verbs like validate --ci
-- must run beside it). Build output goes to standard error, so standard output is AssetTool's alone
-- and a --json stream stays one object per line.

local assetToolConfiguration = "Development-Default"

-- Two things Premake does around every action would break FRT.bat assets, so for that action they
-- are undone here, before Premake gets to them (premake5.lua calls this once every script has
-- declared its options):
--   - it refuses --options that no newoption declared, and AssetTool's verbs have their own (--json,
--     --ci, --engine, more with every verb): those are taken out of _OPTIONS before Premake checks
--     them, and forwarded from _ARGV (assetToolArguments);
--   - its own --help and --version print Premake's usage or version and end the run before the action
--     does (with exit code 1 for --help, which is AssetTool's "problems found"): AssetTool reads --help
--     itself (<verb> --help is help <verb>) and refuses --version, so both are taken out of _OPTIONS
--     too and reach it the same way;
--   - it prints "Running action 'assets'..." on standard output, which would be the first line of
--     every --json stream: its step (premake.main.preAction, in the steps premake.main.elements
--     runs one after the other) is replaced with one that prints nothing.
function prepareAssetsAction()
	if _ACTION ~= "assets" then
		return
	end
	for name in pairs(_OPTIONS) do
		if not (premake.option.get(name) or premake.option.get(name:lower())) or name:lower() == "help" or
			name:lower() == "version" then
			_OPTIONS[name] = nil
		end
	end
	silencePreAction()
end

-- One argument as a program started by runCommand reads it back, with the same text. runCommand
-- goes through cmd.exe (os.execute), which expands %NAME% wherever it is, quoted or not, and ends
-- the command at & | < > wherever it thinks no quote is open - and it knows nothing of the \" that
-- CommandLineToArgvW reads as a quote. So the argument is quoted for CommandLineToArgvW first (a
-- quote inside escaped, the backslashes before a quote - the closing one too - doubled), and then
-- every character cmd.exe treats specially, the quotes too, gets a caret: cmd.exe sees no quotes at
-- all, takes each caret off and passes the rest on. A % keeps its caret through the expansion as
-- well: between two of them, the name always ends in ^, and no variable has such a name.
-- Layers: the cmd.exe parses on the way, 2 for a batch file, which parses its %* once more.
local function argumentText(argument, layers)
	local text = argument:gsub('(\\*)"', function (slashes) return slashes .. slashes .. '\\"' end)
	text = '"' .. text:gsub('(\\+)$', '%1%1') .. '"'
	for _ = 1, layers or 1 do
		text = text:gsub('[%^"%%!&|<>()]', '^%0')
	end
	return text
end

local function argumentsText(arguments, layers)
	local texts = {}
	for _, argument in ipairs(arguments) do
		table.insert(texts, argumentText(argument, layers))
	end
	return table.concat(texts, " ")
end

-- Whether the command line asks for --json, as AssetTool reads the switch (HasCommandLineSwitch:
-- --json, -json or /json, the name in any case).
local function isJsonRequested()
	for _, argument in ipairs(_ARGV or {}) do
		local name = argument:match("^%-%-(.*)$") or argument:match("^[%-/](.*)$")
		if name and name:lower() == "json" then
			return true
		end
	end
	return false
end

-- Text as a JSON string: quotes, backslashes and control characters escaped, other bytes as they are
-- (the UTF-8 Premake has).
local function jsonString(text)
	return '"' .. text:gsub('[%c"\\]', function (c)
		if c == '"' or c == "\\" then
			return "\\" .. c
		end
		return string.format("\\u%04x", c:byte())
	end) .. '"'
end

-- Ends a run that failed before AssetTool did, with Code, AssetTool's way: the message on standard
-- error and, with --json, AssetTool's error object on standard output, so that a script reading the
-- stream sees why it ended (AssetTool/Source/Commandlet.h, CCommandletContext::Fail).
local function assetsFail(code, message)
	io.stderr:write("assets: " .. message .. "\n")
	if isJsonRequested() then
		io.stdout:write('{"error":' .. jsonString(message) .. ',"exitCode":' .. code .. '}\n')
		io.stdout:flush()
	end
	os.exit(code)
end

-- A usage error before AssetTool runs ends with AssetTool's code for one.
local function assetsUsageError(message)
	assetsFail(2, message)
end

-- The value of an argument that names the project as AssetTool reads one (FindCommandLineValue):
-- -Project=, --Project= or /Project=, the name in any case. Nil for any other argument.
local function projectOptionValue(argument)
	local prefix = argument:match("^%-%-?") or argument:match("^/")
	if prefix and argument:sub(#prefix + 1, #prefix + 8):lower() == "project=" then
		return argument:sub(#prefix + 9)
	end
	return nil
end

-- The arguments for AssetTool - every argument of this command line but the action itself,
-- Premake's --file and those that name the project - and the projects those named, in order: Premake's
-- --project=, AssetTool's -Project= (an empty one names none) and bare *.frtproject arguments. The
-- action resolves the project itself, so that a project this engine does not build runs its own
-- engine's FRT.bat, and so that it knows when to add --engine.
local function assetToolArguments()
	local arguments = {}
	local projects = {}
	local bActionSeen = false
	for _, argument in ipairs(_ARGV or {}) do
		local name = argument:match("^%-%-([%w%-]+)")
		local project = projectOptionValue(argument)
		if not bActionSeen and not name and argument == _ACTION then
			bActionSeen = true
		elseif name and name:lower() == "file" then
			-- Premake's own: the script it runs.
		elseif project then
			if project ~= "" then
				table.insert(projects, project)
			end
		elseif not argument:match("^[%-/]") and argument:lower():match("%.frtproject$") then
			table.insert(projects, argument)
		else
			if argument:find("[\r\n]") then
				assetsUsageError("an argument holds a line break, which cmd.exe cannot pass on: " .. argument)
			end
			table.insert(arguments, argument)
		end
	end
	return arguments, projects
end

-- The one project the arguments name (assetToolArguments), loaded, or nil for none. More than one
-- that are not the same file is a usage error: a project's FRT.bat adds --project= for its own, and
-- naming another one there must not be overridden by either.
local function namedProject(locations)
	local project
	for _, location in ipairs(locations) do
		local named, err = loadProject(location)
		if not named then
			assetsUsageError(err)
		end
		if project and not isSamePath(project.File, named.File) then
			assetsUsageError("two projects are named, " .. winpath(project.File) .. " and " .. winpath(named.File) ..
				": name one (a project's FRT.bat names its own)")
		end
		project = named
	end
	return project
end

local function fileTime(file)
	local stat = os.stat(file)
	return stat and stat.mtime or nil
end

local function newestFileTime(pattern)
	local newest = 0
	for _, file in ipairs(os.matchfiles(pattern)) do
		local time = fileTime(file)
		if time and time > newest then
			newest = time
		end
	end
	return newest
end

local assetToolExe = rootpath("Binaries/Win64/AssetTool-" .. assetToolConfiguration .. ".exe")
local assetToolCore = rootpath("Binaries/Win64/Core-" .. assetToolConfiguration .. ".dll")
-- Written after every successful build of AssetTool here: the time that build saw the sources.
local assetToolStamp = rootpath("Local/Build/AssetTool-" .. assetToolConfiguration .. ".stamp")

-- Missing, or not built here since a source of its own or of Core's changed, or since Core's DLL did.
-- Not the executable's own time: other builds rebuild Core without AssetTool (the editor's, a
-- project's FRT.bat build), and the build relinks the executable only when something it compiles
-- changed, so an executable older than Core may be current - or compiled against Core headers that
-- are not there any more, which only a build of AssetTool can tell (an unrelated Core change costs one
-- incremental build that does nothing, once). Times are whole seconds: a file saved within the second
-- of a build counts as built.
local function isAssetToolStale()
	local stampTime = fileTime(assetToolStamp)
	local coreTime = fileTime(assetToolCore)
	if not fileTime(assetToolExe) or not coreTime or not stampTime then
		return true
	end
	return coreTime > stampTime or newestFileTime(rootpath("AssetTool/Source/**")) > stampTime or
		newestFileTime(rootpath("Core/Source/**")) > stampTime
end

-- The engine's solution, generated again first if it predates the AssetTool project, then
-- AssetTool and what it depends on, all of it printed to standard error.
--
-- The build cannot replace an executable or a DLL that a running process has loaded: while an open
-- editor (or another AssetTool, say one holding lock) runs Core's Development DLL, any change to
-- Core's sources makes this build fail at its link. A run that writes nothing must still work then
-- (validate --ci beside an open editor, design 13.1), so when the build fails and an earlier build is
-- there, that one runs, with a warning; the command fails, with AssetTool's code for an internal
-- error, only when there is nothing to run. (Two FRT.bat assets building at once fail the same way,
-- and the second runs what the first left, or fails if there was none: Premake's Lua has no
-- exclusive create or file lock to make one wait for the other.)
local function buildAssetTool()
	local ok, err = pcall(function ()
		local solution = io.readfile(solutionFile(nil))
		if not solution or not solution:find('"AssetTool"', 1, true) then
			generate(nil, true)
		end
		build(nil, assetToolConfiguration, "AssetTool", true)
	end)
	if ok then
		writeBinary(assetToolStamp, os.date("!%Y-%m-%dT%H:%M:%SZ") .. "\n")
		return
	end
	if not (os.isfile(assetToolExe) and os.isfile(assetToolCore)) then
		assetsFail(3, tostring(err))
	end
	io.stderr:write("assets: " .. tostring(err) .. "\n")
	io.stderr:write("assets: warning: AssetTool could not be built again (does an editor or another AssetTool have it or " ..
		"Core in use?): running the build that is there, which may not have the latest changes\n")
end

-- The exit code of a command runCommand ran (3, an internal error, when it has none).
local function exitCodeOf(ok, how, code)
	if how == "exit" and type(code) == "number" then
		return code
	end
	return ok and 0 or 3
end

-----------------------------------------
---------------  Actions  ---------------
-----------------------------------------

-- FRT.bat setup --hooks[=off]: core.hooksPath of one clone, the engine's or --project's, and nothing else: no
-- registration and no solution, so a project's clone can turn it on, and nothing installs it implicitly. An engine
-- inside the clone (the engine's own checkout, a project's Engine/ submodule) is named relatively, from the top of the
-- work tree, as git reads it, so it holds wherever the clone is and in every worktree of it; a shared engine elsewhere
-- has only its absolute path. The hook itself: Tools/GitHooks/pre-commit; the others there pass on to the clone's
-- own .git/hooks (Git LFS's, the person's), which core.hooksPath would otherwise silence. A core.hooksPath that is
-- not FRT's (husky, a team's folder) is never replaced or removed: git runs the hooks of one folder only.
local function isFrtHooksPath(value)
	return value ~= nil and value:gsub("\\", "/"):gsub("/+$", ""):match("Tools/GitHooks$") ~= nil
end

local function setupHooks(value)
	if value ~= "" and value ~= "off" then
		fail("setup: --hooks takes no value, or =off to turn the hook off")
	end
	if not outputOf("git --version") then
		fail("setup --hooks: git is not on PATH")
	end
	local project, delegated = commandProject("setup")
	if delegated then
		return
	end
	local clone = project and project.Dir or ROOT_DIR
	local gitConfig = "git -C " .. quoted(clone) .. " config "
	-- Asked of git: its spelling of the top level may not be ours (a subst or mapped drive, a junction).
	local prefix = outputOf("git -C " .. quoted(clone) .. " rev-parse --show-prefix")
	if not prefix then
		fail("setup --hooks: " .. winpath(clone) .. " is not in a git repository")
	end

	local current = outputOf(gitConfig .. "--local --get core.hooksPath")
	if current == "" then
		current = nil
	end
	if value == "off" then
		if current and not isFrtHooksPath(current) then
			print("core.hooksPath of " .. winpath(clone) .. " is " .. current .. ", not FRT's hooks: left as it is.")
			return
		end
		if current and not runCommand(gitConfig .. "--local --unset core.hooksPath") then
			fail("setup --hooks=off: git config failed in " .. winpath(clone))
		end
		print("The pre-commit hook is off for " .. winpath(clone) .. ".")
		return
	end
	if current and not isFrtHooksPath(current) then
		fail("setup --hooks: core.hooksPath of " .. winpath(clone) .. " is already " .. current .. ", another tool's " ..
			"hooks, and git runs the hooks of one folder only. Unset it (git config --unset core.hooksPath) to use FRT's, " ..
			"which run the clone's own .git/hooks too.")
	end

	local hooks = path.join(ROOT_DIR, "Tools/GitHooks")
	-- An engine on another drive has no relative path, and getrelative gives the absolute one back.
	local relative = path.getrelative(clone, ROOT_DIR)
	if not relative:match("^%.%.") and not path.isabsolute(relative) then
		hooks = prefix .. path.join(relative == "." and "" or relative, "Tools/GitHooks")
	end
	-- Not quoted(), which spells it for Windows: this value is git's, and the same in every clone of a project.
	if not runCommand(gitConfig .. '--local core.hooksPath "' .. hooks .. '"') then
		fail("setup --hooks: git config failed in " .. winpath(clone))
	end
	print("The pre-commit hook is on for " .. winpath(clone) .. " (core.hooksPath=" .. hooks .. ").")
	local commonDir = outputOf("git -C " .. quoted(clone) .. " rev-parse --path-format=absolute --git-common-dir")
	local own = {}
	for _, file in ipairs(commonDir and os.matchfiles(path.join(commonDir, "hooks/*")) or {}) do
		if not file:match("%.sample$") then
			table.insert(own, path.getname(file))
		end
	end
	if #own > 0 then
		print("FRT's hooks run this clone's own as well: " .. table.concat(own, ", ") .. ".")
	end
end

newaction
{
	trigger     = "setup",
	description = "Register this engine for projects to find by name, give its text files LF, and generate its solution",
	execute     = function ()
		if _OPTIONS["hooks"] ~= nil then
			setupHooks(_OPTIONS["hooks"])
			return
		end
		local engine = loadEngine(ROOT_DIR) or fail("setup: " .. rootpath(engineFileName) .. " has no [Engine] Name")
		-- First: it may rewrite files, and nothing below depends on it.
		local bLineEndings = setupLineEndings()
		print("")
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
		if not bLineEndings then
			fail("setup: the line endings are not fixed (see above)")
		end
	end
}

newaction
{
	trigger     = "new",
	description = "Create a project from a template: FRT.bat new --name=<Name> [--template --location --layout --build --no-generate]",
	execute     = function ()
		local name = _OPTIONS["name"] or fail("new: name the project with --name=<Name>")
		local valid, why = isValidProjectName(name)
		if not valid then
			fail("new: " .. why)
		end

		if _OPTIONS["no-generate"] and _OPTIONS["build"] then
			fail("new: --build needs the solution that --no-generate leaves out: give one of them")
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
		if _OPTIONS["no-generate"] then
			print("No solution generated (--no-generate): FRT.bat generate --project=" .. quoted(winpath(projectFile)) .. " makes it.")
		elseif layout == "embedded" then
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
			print("  Optional: FRT.bat setup --hooks checks asset and meta pairs before each commit, in this clone.")
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

		-- No meta goes into a package (packageSkippedSuffixes), and nothing of the project's Local/: only
		-- these trees are copied.
		copyFiles(path.join(project.Dir, "Content"), path.join(dir, "Content"), nil, packageSkippedSuffixes)
		copyFiles(path.join(project.Dir, "Config"), path.join(dir, "Config"), nil, packageSkippedSuffixes)

		-- The shader sources, which Release compiles at start-up until the cook ships DXIL (M12); not the
		-- stale Bin/ an old build left there, which nothing reads. The fonts are Raw assets with metas.
		copyFiles(rootpath("Core/Content/Shaders"), path.join(dir, "Engine/Content/Shaders"), { "Bin/" }, packageSkippedSuffixes)
		copyFiles(rootpath("Core/Content/Fonts"), path.join(dir, "Engine/Content/Fonts"), nil, packageSkippedSuffixes)
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

-- The line-ending check of FRT.bat verify (Tools/Verify/Verify.py runs this), by the rules setup
-- applies, fixing nothing. Unlike setup, it fails when the engine is not a git checkout of its own:
-- a gate that checked nothing must not pass.
newaction
{
	trigger     = "check-line-endings",
	description = "Check that every tracked file has the line endings .gitattributes asks for (run by verify)",
	execute     = function ()
		local prefix = outputOf(git("rev-parse --show-prefix"))
		if not prefix then
			fail("check-line-endings: git rev-parse --show-prefix failed: is this engine a git checkout?")
		end
		if prefix ~= "" then
			fail("check-line-endings: the engine is " .. prefix .. " in another repository's work tree, not a " ..
				"checkout of its own")
		end
		local entries = trackedLineEndings() or fail("check-line-endings: git ls-files --eol failed")
		local problems = lineEndingProblems(entries)
		if #problems.work > 0 then
			print("Checked out with other line endings than .gitattributes asks for (run FRT.bat setup):")
			printSome(problems.work, "  ")
		end
		if #problems.index > 0 then
			print("Committed with CRLF, which .gitattributes forbids (git add --renormalize <file>):")
			printSome(problems.index, "  ")
		end
		if #problems.work > 0 or #problems.index > 0 then
			fail("check-line-endings: " .. #problems.work .. " file(s) to check out again, " .. #problems.index ..
				" committed with CRLF")
		end
		print("Line endings: " .. countLfFiles(entries) .. " LF files, all LF in the index and the working tree.")
	end
}

-- The current source hash of every builder, for development builds to compare with the committed
-- ones, and the pipeline lint's file list (Builders, above). Core's prebuild step runs it.
newaction
{
	trigger     = "builder-hashes",
	description = "Hash each builder's sources into " .. builderHashesFile .. " (run by Core's prebuild step)",
	execute     = function ()
		local _, rows = readBuilderTable("builder-hashes")
		local lines =
		{
			"// Generated by FRT.bat builder-hashes (Premake/tools.lua) before every build of Core, from the builder table",
			"// " .. builderTableFile .. ": the hash of each builder's sources as they are now. Do not edit.",
		}
		local pipeline, listed = {}, {}
		for _, row in ipairs(rows) do
			local files = builderSources(row)
			table.insert(lines, string.format('FRT_BUILDER_SOURCE_HASH("%s", "%s")', row.Name, hashBuilderSources("builder-hashes", files)))
			for _, file in ipairs(files) do
				if not listed[file] then
					listed[file] = true
					table.insert(pipeline, file)
				end
			end
		end
		table.sort(pipeline, isBytewiseLess)
		if writeIfChanged("builder-hashes", builderHashesFile, table.concat(lines, "\n") .. "\n") then
			print("builder-hashes: " .. builderHashesFile .. " updated")
		end
		writeIfChanged("builder-hashes", pipelineSourcesFile, #pipeline > 0 and table.concat(pipeline, "\n") .. "\n" or "")
	end
}

-- BUILD003 (Builders, above): run by FRT.bat verify, and with --update by whoever bumped a GUID.
newaction
{
	trigger     = "check-builders",
	description = "Check that every builder whose sources changed has a new version GUID (BUILD003, run by verify): [--update]",
	execute     = function ()
		local text, rows = readBuilderTable("check-builders")
		local recorded = recordedBuilderHashes(rows)
		local changed, reused = {}, {}
		for _, row in ipairs(rows) do
			row.Files = builderSources(row)
			row.Current = hashBuilderSources("check-builders", row.Files)
			-- The table pairs the GUID with a hash it was not recorded with: one written by hand, or a GUID
			-- used again.
			row.Others = otherRecordedHashes(recorded, row.Guid, row.Hash)
			row.bRecorded = recorded[row.Guid] ~= nil
			if row.Current ~= row.Hash then
				table.insert(changed, row)
			end
			if #row.Others > 0 then
				table.insert(reused, row)
			end
		end
		if #changed == 0 and #reused == 0 then
			print("Builders: " .. #rows .. " checked, every one's sources match the hash committed with its version.")
			return
		end

		local head = headBuilderTable("check-builders")
		-- Whether the row's version GUID is not HEAD's: a bump, or a builder HEAD does not have.
		local function isBumped(row)
			return head ~= nil and (head[row.Name] == nil or head[row.Name].Guid ~= row.Guid)
		end
		local function describe(row)
			return string.format("%s (%s:%d): sources %s, committed with its version %s (%d file%s: %s)", row.Name,
				builderTableFile, row.Line, row.Current, row.Hash, #row.Files, #row.Files == 1 and "" or "s",
				table.concat(row.Files, ", "))
		end
		local function printReused(row)
			print("BUILD003 " .. describe(row))
			print("  its version GUID was recorded with another source hash before, " .. table.concat(row.Others, ", ") ..
				": a hash written by hand, or a GUID used again. Give it a new GUID (FRT.bat assets newguid), then run " ..
				"FRT.bat check-builders --update")
		end

		if _OPTIONS["update"] then
			if not head then
				fail("check-builders --update: there is no HEAD to compare the version GUIDs with (is this engine a git " ..
					"checkout with a commit?): nothing written")
			end
			local refused = 0
			for _, row in ipairs(reused) do
				printReused(row)
				refused = refused + 1
			end
			for _, row in ipairs(changed) do
				local others = otherRecordedHashes(recorded, row.Guid, row.Current)
				if #row.Others > 0 then
					-- Said above.
				elseif not isBumped(row) then
					print("BUILD003 " .. describe(row))
					print("  its version GUID is HEAD's: give it a new GUID first (FRT.bat assets newguid)")
					refused = refused + 1
				elseif #others > 0 then
					print("BUILD003 " .. describe(row))
					print("  this GUID was already recorded with hash " .. table.concat(others, ", ") .. ": run FRT.bat assets " ..
						"newguid again")
					refused = refused + 1
				end
			end
			if refused > 0 then
				fail("check-builders --update: " .. refused .. " builder(s) need a new version GUID first (FRT.bat assets " ..
					"newguid); nothing written")
			end
			-- From the last row back, so the byte ranges of the earlier ones stay valid.
			table.sort(changed, function (a, b) return a.HashFirst > b.HashFirst end)
			for _, row in ipairs(changed) do
				text = text:sub(1, row.HashFirst - 1) .. row.Current .. text:sub(row.HashLast + 1)
			end
			if not writeBinary(rootpath(builderTableFile), text) then
				fail("check-builders --update: cannot write " .. builderTableFile)
			end
			-- The pairs, so that a later --update or check here refuses a second hash for these GUIDs.
			local record = readBinary(rootpath(builderRecordFile)) or ""
			for _, row in ipairs(changed) do
				record = record .. row.Guid .. " " .. row.Current .. " " .. row.Name .. "\n"
				print("Recorded " .. row.Name .. "'s source hash " .. row.Current .. " in " .. builderTableFile .. ".")
			end
			if not writeBinary(rootpath(builderRecordFile), record) then
				print("check-builders --update: warning: cannot write " .. builderRecordFile .. ", so a later --update cannot " ..
					"tell that these GUIDs were recorded until they are committed")
			end
			return
		end

		local needGuid = 0
		for _, row in ipairs(reused) do
			printReused(row)
			needGuid = needGuid + 1
		end
		for _, row in ipairs(changed) do
			if #row.Others > 0 then
				-- Said above.
			elseif isBumped(row) and not row.bRecorded then
				print(describe(row))
				print("  its version GUID is new: record the new hash with FRT.bat check-builders --update, once, right after " ..
					"FRT.bat assets newguid (a later change needs another new GUID)")
			else
				needGuid = needGuid + 1
				print("BUILD003 " .. describe(row))
				print(isBumped(row)
					and "  its sources changed after its version GUID was recorded: give it a new GUID (FRT.bat assets newguid), " ..
						"then run FRT.bat check-builders --update"
					or "  its sources changed while its version GUID did not: bump the GUID (FRT.bat assets newguid), then run " ..
						"FRT.bat check-builders --update")
			end
		end
		fail("check-builders: " .. #changed .. " of " .. #rows .. " builder(s) changed since their hash was committed" ..
			(needGuid > 0 and (", " .. needGuid .. " without a new version GUID (BUILD003)") or ""))
	end
}

-- The engine's merge gate. There is no CI, and byte-exact D3D12 captures need a GPU machine, so
-- it runs here: Tools/Verify/Verify.py, which says what it checks.
newaction
{
	trigger     = "verify",
	description = "Check the engine before a merge (Tools/Verify/Verify.py): [--quick|--nightly] [--record-baselines], or --update-goldens",
	execute     = function ()
		if _OPTIONS["project"] then
			fail("verify: checks the engine and its templates; it takes no --project")
		end
		if _OPTIONS["quick"] and _OPTIONS["nightly"] then
			fail("verify: --quick or --nightly, not both")
		end
		if _OPTIONS["update-goldens"] and (_OPTIONS["quick"] or _OPTIONS["nightly"] or _OPTIONS["record-baselines"]) then
			fail("verify: --update-goldens runs nothing else; give it alone")
		end
		local interpreter = python() or fail("verify: needs Python 3 (the py launcher or python on PATH)")
		local command = interpreter .. " -I " .. quoted(rootpath("Tools/Verify/Verify.py"))
		for _, switch in ipairs({ "quick", "nightly", "record-baselines", "update-goldens" }) do
			if _OPTIONS[switch] then
				command = command .. " --" .. switch
			end
		end
		if _OPTIONS["baselines"] then
			command = command .. " --baselines=" .. quoted(canonicalPath(_OPTIONS["baselines"]))
		end
		if _OPTIONS["pt-tolerance"] then
			local tolerance = tonumber(_OPTIONS["pt-tolerance"]) or fail("verify: --pt-tolerance takes a number, e.g. 2e-4")
			command = command .. " --pt-tolerance=" .. string.format("%.17g", tolerance)
		end
		if not runCommand(command) then
			fail("verify failed (the summary above says what)")
		end
	end
}

-- The asset system's command line (Assets, above).
newaction
{
	trigger     = "assets",
	description = "Run the asset tool: FRT.bat assets <verb> [arguments], for --project or the engine (assets help lists the verbs)",
	execute     = function ()
		local arguments, projects = assetToolArguments()
		local project = namedProject(projects)
		-- Without a project, the engine. (AssetTool refuses a project together with --engine; given
		-- twice, --engine is still one.)
		local mount = { "--engine" }
		if project then
			local engineDir, engineErr = resolveEngineDir(project)
			if not engineDir then
				assetsUsageError(project.File .. ": " .. engineErr)
			end
			if not isSamePath(engineDir, ROOT_DIR) then
				-- That engine's FRT.bat is a batch file: its %* is parsed once more.
				io.stderr:write(project.Name .. " uses the engine in " .. winpath(engineDir) .. ": running its FRT.bat.\n")
				os.exit(exitCodeOf(runCommand(quoted(path.join(engineDir, "FRT.bat")) .. " assets " ..
					argumentText("--project=" .. winpath(project.File), 2) .. " " .. argumentsText(arguments, 2))))
			end
			mount = { "-Project=" .. winpath(project.File) }
		end

		if isAssetToolStale() then
			buildAssetTool()
		end
		os.exit(exitCodeOf(runCommand(quoted(assetToolExe) .. " " .. argumentsText(mount) .. " " .. argumentsText(arguments))))
	end
}
