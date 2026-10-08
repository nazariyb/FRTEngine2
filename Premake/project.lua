-----------------------------------------
-------------  Projects  ----------------
-----------------------------------------
-- A game lives in its own directory - its own repository - described by <Name>.frtproject
-- (Core/Source/Project/Project.h says what it holds). It uses an engine checkout that is either
-- shared by any number of projects (registered once per machine: FRT.bat setup) or a git
-- submodule inside the project, pinned per project.
--
-- `--project=<file>` makes that project's solution, <Project>/<Name>.sln: the game, plus the
-- engine's projects. Their project files stay in the engine and are the same for every solution
-- that has them, so moving between the engine's solution and projects' never rebuilds the engine.
--
-- Descriptors and the per-user registry are read and written here by the same rules as the
-- engine's C++ side (Core/Source/Project/Project.cpp, UserRegistry.cpp): keep the two in step.

newoption
{
	trigger     = "project",
	value       = "FILE",
	description = "A project's .frtproject file, or its directory: the project to generate, build or open"
}

engineFileName = "Engine.frtengine"
projectFileExtension = ".frtproject"

function winpath(p)
	return path.translate(p, "\\")
end

-- A path given on the command line or in the environment: relative to where the command was
-- run. (While premake5.lua itself runs, the current directory is the engine's.)
function absolutePath(p)
	return path.getabsolute(p, _WORKING_DIR or os.getcwd())
end

-- A path's one spelling, as the engine writes it into the registry: absolute, normalized,
-- backslashes. Keys compare case-insensitively.
function canonicalPath(p)
	return winpath(path.normalize(absolutePath(p)))
end

function isSamePath(a, b)
	return canonicalPath(a):lower() == canonicalPath(b):lower()
end

-----------------------------------------
--------------  INI files  --------------
-----------------------------------------
-- As Config/IniFile.h reads them: [Section], Key=Value split at the first '=', whole-line ';'
-- or '#' comments, one layer of double quotes stripped, names case-insensitive. Saved without
-- comments, in UTF-8 with LF endings.
ini = {}

function ini.findSection(data, name, create)
	for _, section in ipairs(data) do
		if section.name:lower() == name:lower() then
			return section
		end
	end
	if create then
		local section = { name = name, entries = {} }
		table.insert(data, section)
		return section
	end
	return nil
end

function ini.parse(text)
	local data = {}
	local section = ini.findSection(data, "", true)
	text = text:gsub("^\239\187\191", "")   -- a UTF-8 BOM
	for line in text:gmatch("[^\r\n]+") do
		line = line:match("^%s*(.-)%s*$")
		local first = line:sub(1, 1)
		if line == "" or first == ";" or first == "#" then
			-- nothing
		elseif first == "[" and line:sub(-1) == "]" then
			section = ini.findSection(data, line:sub(2, -2):match("^%s*(.-)%s*$"), true)
		else
			local equals = line:find("=", 1, true)
			if equals and equals > 1 then
				local key = line:sub(1, equals - 1):match("^%s*(.-)%s*$")
				local value = line:sub(equals + 1):match("^%s*(.-)%s*$")
				if #value >= 2 and value:sub(1, 1) == '"' and value:sub(-1) == '"' then
					value = value:sub(2, -2)
				end
				ini.set(data, section.name, key, value)
			end
		end
	end
	return data
end

-- The file's contents, and whether it could be read; a missing file is empty.
function ini.load(file)
	local text = io.readfile(file)
	if not text then
		return {}, false
	end
	return ini.parse(text), true
end

function ini.find(data, sectionName, key)
	local section = ini.findSection(data, sectionName, false)
	for _, entry in ipairs(section and section.entries or {}) do
		if entry.key:lower() == key:lower() then
			return entry.value
		end
	end
	return nil
end

function ini.set(data, sectionName, key, value)
	local section = ini.findSection(data, sectionName, true)
	for _, entry in ipairs(section.entries) do
		if entry.key:lower() == key:lower() then
			entry.value = value
			return
		end
	end
	table.insert(section.entries, { key = key, value = value })
end

function ini.save(data, file)
	local lines = {}
	for _, section in ipairs(data) do
		if #section.entries > 0 then
			if section.name ~= "" then
				if #lines > 0 then
					table.insert(lines, "")
				end
				table.insert(lines, "[" .. section.name .. "]")
			end
			for _, entry in ipairs(section.entries) do
				table.insert(lines, entry.key .. "=" .. entry.value)
			end
		end
	end
	os.mkdir(path.getdirectory(file))
	return io.writefile(file, table.concat(lines, "\n") .. "\n")
end

-----------------------------------------
--------  Per-user registry  ------------
-----------------------------------------
-- Project/UserRegistry.h: %LOCALAPPDATA%\FRTEngine (FRT_USER_DATA_DIR overrides it), never in a
-- repository. Engines.ini maps engine names to checkouts; Projects.ini lists recent projects.

function userDataDir()
	local dir = os.getenv("FRT_USER_DATA_DIR")
	if dir and dir ~= "" then
		return path.getabsolute(dir)
	end
	local localAppData = os.getenv("LOCALAPPDATA")
	if localAppData and localAppData ~= "" then
		return path.join(localAppData, "FRTEngine")
	end
	return path.join(os.getenv("TEMP") or ".", "FRTEngine")
end

local function enginesFile()
	return path.join(userDataDir(), "Engines.ini")
end

local function projectsFile()
	return path.join(userDataDir(), "Projects.ini")
end

function findRegisteredEngine(name)
	local dir = ini.find(ini.load(enginesFile()), "Engines", name)
	return (dir and dir ~= "") and dir or nil
end

function registerEngine(name, dir)
	local data = ini.load(enginesFile())
	ini.set(data, "Engines", name, canonicalPath(dir))
	return ini.save(data, enginesFile())
end

function addRecentProject(file)
	local data = ini.load(projectsFile())
	ini.set(data, "Recent", canonicalPath(file), tostring(os.time()))
	return ini.save(data, projectsFile())
end

-----------------------------------------
------------  Descriptors  --------------
-----------------------------------------

function isEngineDir(dir)
	return dir ~= nil and os.isfile(path.join(dir, engineFileName))
end

-- <Dir>/Engine.frtengine: { Name, Version }, or nil.
function loadEngine(dir)
	local data, ok = ini.load(path.join(dir, engineFileName))
	local name = ok and ini.find(data, "Engine", "Name") or nil
	if not name or name == "" then
		return nil
	end
	return { Dir = canonicalPath(dir), Name = name, Version = ini.find(data, "Engine", "Version") or "" }
end

-- The nearest directory at or above dir that is an engine checkout, or nil.
function findEngineDirAbove(dir)
	dir = path.getabsolute(dir)
	while true do
		if isEngineDir(dir) then
			return dir
		end
		local parent = path.getdirectory(dir)
		if parent == dir or parent == "" or parent == "." then
			return nil
		end
		dir = parent
	end
end

-- The only *.frtproject directly in dir, or nil and why.
function findProjectFile(dir)
	local found = os.matchfiles(path.join(dir, "*" .. projectFileExtension))
	if #found == 1 then
		return found[1]
	end
	return nil, (#found == 0 and "no " .. projectFileExtension .. " file in " or "more than one " ..
		projectFileExtension .. " file in ") .. dir
end

-- A project's descriptor - { File, Dir, Name, Module, Engine } - from its file or its directory;
-- nil and why when there is none.
function loadProject(location)
	local file = absolutePath(location)
	if os.isdir(file) then
		local err
		file, err = findProjectFile(file)
		if not file then
			return nil, err
		end
	end
	local data, ok = ini.load(file)
	if not ok then
		return nil, "cannot read " .. file
	end
	local name = ini.find(data, "Project", "Name")
	if not name or name == "" then
		return nil, file .. " has no [Project] Name"
	end
	local module = ini.find(data, "Project", "Module")
	module = (module and module ~= "") and module or name
	-- Both name generated projects and files: never an engine project's, or a path.
	for _, value in ipairs({ name, module }) do
		local valid, why = isValidProjectName(value)
		if not valid then
			return nil, file .. ": " .. why
		end
	end
	return
	{
		File = file,
		Dir = path.getdirectory(file),
		Name = name,
		Module = module,
		Engine = ini.find(data, "Project", "Engine") or "",
	}
end

-- The engine a project builds and runs with (Project.h: ResolveEngineDir), or nil and why:
--   1. FRT_ENGINE_DIR, when set;
--   2. the project's Engine: a path relative to the project file, if it leads to an engine,
--      else a registered engine's name;
--   3. the nearest engine above the project.
function resolveEngineDir(project)
	local override = os.getenv("FRT_ENGINE_DIR")
	if override and override ~= "" then
		if isEngineDir(absolutePath(override)) then
			return absolutePath(override)
		end
		return nil, "FRT_ENGINE_DIR (" .. override .. ") is not an engine directory"
	end
	if project.Engine ~= "" then
		local asPath = path.getabsolute(path.join(project.Dir, project.Engine))
		if isEngineDir(asPath) then
			return asPath
		end
		local registered = findRegisteredEngine(project.Engine)
		if isEngineDir(registered) then
			return path.getabsolute(registered)
		end
	end
	local above = findEngineDirAbove(project.Dir)
	if above then
		return above
	end
	if project.Engine == "" then
		return nil, "the project names no engine and is not inside one"
	end
	return nil, "no engine '" .. project.Engine .. "': not a directory with " .. engineFileName ..
		" next to the project, and no engine registered under that name (run FRT.bat setup in the engine)"
end

-- Project.h: IsValidProjectName. The name is also the module's, its files' and C++ identifiers'.
local reservedNames = { "Core", "Editor", "Launcher", "Engine", "ImGui", "assimp", "gtest", "googletest", "ThirdParty" }

function isValidProjectName(name)
	if not name or #name == 0 or #name > 64 then
		return false, "a project name has 1 to 64 characters"
	end
	if not name:match("^%a") then
		return false, "a project name starts with a letter"
	end
	if not name:match("^[%w_]+$") then
		return false, "a project name has only letters, digits and underscores"
	end
	for _, reserved in ipairs(reservedNames) do
		if name:lower() == reserved:lower() then
			return false, "'" .. name .. "' is the name of a part of the engine"
		end
	end
	return true
end

-- The project named by --project, which this engine must be the one to build; errors otherwise.
function requireProject(location)
	local project, err = loadProject(location)
	if not project then
		error(err, 0)
	end
	local engineDir, engineErr = resolveEngineDir(project)
	if not engineDir then
		error(project.File .. ": " .. engineErr, 0)
	end
	if not isSamePath(engineDir, ROOT_DIR) then
		error(project.Name .. " uses the engine in " .. winpath(engineDir) .. ", not this one (" .. winpath(ROOT_DIR) ..
			"). Run the project's FRT.bat, or that engine's.", 0)
	end
	return project
end
