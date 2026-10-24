-----------------------------------------
---------------  Tools  -----------------
-----------------------------------------
-- FRT.bat <command> [options]: the engine's command line, which the editor's project manager
-- runs too. In a project, the project's FRT.bat runs its engine's with --project=<the project>.
--
--   setup      registers this engine under its name (Engine.frtengine), so shared projects find
--              it, gives the checkout's hashed text files LF line endings (Line endings, below)
--              and generates its solution: the first thing to run in a new checkout
--   new        creates a project from a template, sets up its git repository, generates its
--              solution: FRT.bat new --name=MyGame [--template=Demo] [--layout=embedded] [--build]
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
---------------  Assets  ----------------
-----------------------------------------
-- FRT.bat assets <verb> [arguments] [options]: AssetTool (AssetTool/Source/AssetTool.cpp says what
-- it does), for --project or, without one, for the engine itself, as the owner of /Engine
-- (--engine). Everything after the action goes to AssetTool as it was given - the verb, its
-- arguments, -Section:Key=Value overrides, --options - and AssetTool's exit code is this command's:
-- 0 success, 1 problems found, 2 usage error, 3 internal error, 4 the project is owned by another
-- process. The Development build runs, built first when it is missing or older than its sources
-- or Core's. That build's output goes to standard error, so standard output is AssetTool's alone
-- and a --json stream stays one object per line.

local assetToolConfiguration = "Development-Default"

-- Two things Premake does around every action would break FRT.bat assets, so for that action they
-- are undone here, before Premake gets to them (premake5.lua calls this once every script has
-- declared its options):
--   - it refuses --options that no newoption declared, and AssetTool's verbs have their own (--json,
--     --ci, --engine, more with every verb): those are taken out of _OPTIONS before Premake checks
--     them, and forwarded from _ARGV (assetToolArguments);
--   - it prints "Running action 'assets'..." on standard output, which would be the first line of
--     every --json stream: its step (premake.main.preAction, in the steps premake.main.elements
--     runs one after the other) is replaced with one that prints nothing.
function prepareAssetsAction()
	if _ACTION ~= "assets" then
		return
	end
	for name in pairs(_OPTIONS) do
		if not (premake.option.get(name) or premake.option.get(name:lower())) then
			_OPTIONS[name] = nil
		end
	end
	local main = premake.main
	for i, step in ipairs(main and main.elements or {}) do
		if step == main.preAction then
			main.elements[i] = function () end
		end
	end
end

-- One argument for a program started through cmd.exe, as CommandLineToArgvW reads it back: quoted,
-- a quote inside escaped, the backslashes before a quote - the closing one too - doubled.
local function argumentText(argument)
	local text = argument:gsub('(\\*)"', function (slashes) return slashes .. slashes .. '\\"' end)
	text = text:gsub('(\\+)$', '%1%1')
	return '"' .. text .. '"'
end

-- Every argument of this command line but the action itself, Premake's --file and --project
-- (passed on as AssetTool's own -Project=).
local function assetToolArguments()
	local arguments = {}
	local bActionSeen = false
	for _, argument in ipairs(_ARGV or {}) do
		local name = argument:match("^%-%-([%w%-]+)")
		if not bActionSeen and not name and argument == _ACTION then
			bActionSeen = true
		elseif name ~= "file" and name ~= "project" then
			table.insert(arguments, argumentText(argument))
		end
	end
	return table.concat(arguments, " ")
end

-- A usage error before AssetTool runs ends with AssetTool's code for one.
local function assetsUsageError(message)
	io.stderr:write("assets: " .. message .. "\n")
	os.exit(2)
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

-- Missing, or older than a source of its own or of Core's (Core is a DLL it loads: the build
-- relinks the executable only when the DLL's exports change, so each is compared with its own
-- sources). Times are whole seconds: a file saved within the second of a link counts as built.
local function isAssetToolStale(exe)
	local exeTime = fileTime(exe)
	local coreTime = fileTime(rootpath("Binaries/Win64/Core-" .. assetToolConfiguration .. ".dll"))
	if not exeTime or not coreTime then
		return true
	end
	return newestFileTime(rootpath("AssetTool/Source/**")) > exeTime or newestFileTime(rootpath("Core/Source/**")) > coreTime
end

-- The engine's solution, generated again first if it predates the AssetTool project, then
-- AssetTool and what it depends on, all of it printed to standard error. A failure ends the
-- command with AssetTool's code for an internal error.
local function buildAssetTool()
	local ok, err = pcall(function ()
		local solution = io.readfile(solutionFile(nil))
		if not solution or not solution:find('"AssetTool"', 1, true) then
			generate(nil, true)
		end
		build(nil, assetToolConfiguration, "AssetTool", true)
	end)
	if not ok then
		io.stderr:write("assets: " .. tostring(err) .. "\n")
		os.exit(3)
	end
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

newaction
{
	trigger     = "setup",
	description = "Register this engine for projects to find by name, give its text files LF, and generate its solution",
	execute     = function ()
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

-- The engine's merge gate. There is no CI, and byte-exact D3D12 captures need a GPU machine, so
-- it runs here: Tools/Verify/Verify.py, which says what it checks.
newaction
{
	trigger     = "verify",
	description = "Check the engine before a merge (Tools/Verify/Verify.py): [--quick|--nightly] [--record-baselines]",
	execute     = function ()
		if _OPTIONS["project"] then
			fail("verify: checks the engine and its templates; it takes no --project")
		end
		if _OPTIONS["quick"] and _OPTIONS["nightly"] then
			fail("verify: --quick or --nightly, not both")
		end
		local interpreter = python() or fail("verify: needs Python 3 (the py launcher or python on PATH)")
		local command = interpreter .. " -I " .. quoted(rootpath("Tools/Verify/Verify.py"))
		for _, switch in ipairs({ "quick", "nightly", "record-baselines" }) do
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
		local arguments = assetToolArguments()
		local mount = " --engine"
		if _OPTIONS["project"] then
			local project, err = loadProject(_OPTIONS["project"])
			if not project then
				assetsUsageError(err)
			end
			local engineDir, engineErr = resolveEngineDir(project)
			if not engineDir then
				assetsUsageError(project.File .. ": " .. engineErr)
			end
			if not isSamePath(engineDir, ROOT_DIR) then
				io.stderr:write(project.Name .. " uses the engine in " .. winpath(engineDir) .. ": running its FRT.bat.\n")
				os.exit(exitCodeOf(runCommand(quoted(path.join(engineDir, "FRT.bat")) .. " assets --project=" ..
					quoted(project.File) .. " " .. arguments)))
			end
			mount = " -Project=" .. quoted(project.File)
		end

		local exe = rootpath("Binaries/Win64/AssetTool-" .. assetToolConfiguration .. ".exe")
		if isAssetToolStale(exe) then
			buildAssetTool()
		end
		os.exit(exitCodeOf(runCommand(quoted(exe) .. mount .. " " .. arguments)))
	end
}
