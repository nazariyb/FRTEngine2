-----------------------------------------
---------  Generation options  ----------
-----------------------------------------
-- The options that change the engine's project files - and so every project's solution, which
-- has those same files - are remembered per engine checkout, in Local/GenerationOptions.ini
-- (never committed): given once to anything that generates (setup, generate, new, vs2022), an
-- option holds for every later generation of the engine and of its projects, until given again.
-- A switch is turned off with =off: --nopch=off. Otherwise one project's solution, generated
-- with other options than the engine's, would rewrite the engine's project files and rebuild it.

generationOptionNames = { "precision", "msvc", "thirdparty", "nopch", "fatal-warnings" }

local switches = { nopch = true, ["fatal-warnings"] = true }
local offValues = { off = true, no = true, ["false"] = true, ["0"] = true }
local optionsFile = path.join(ROOT_DIR, "Local/GenerationOptions.ini")

-- The generation options on this command line: name -> value ("" for a switch given bare).
explicitGenerationOptions = {}
for _, argument in ipairs(_ARGV or {}) do
	local name, value = argument:match("^%-%-([%w%-]+)=(.*)$")
	if not name then
		name, value = argument:match("^%-%-([%w%-]+)$"), ""
	end
	if name and table.contains(generationOptionNames, name) then
		explicitGenerationOptions[name] = value
	end
end

-- What this run generates with: the command line's, else the remembered ones, else the defaults.
do
	local saved = ini.load(optionsFile)
	for _, name in ipairs(generationOptionNames) do
		local value = explicitGenerationOptions[name]
		if value == nil then
			value = ini.find(saved, "Options", name)
		end
		if value ~= nil then
			if switches[name] then
				_OPTIONS[name] = (not offValues[value:lower()]) and "" or nil
			else
				_OPTIONS[name] = value ~= "" and value or nil
			end
		end
	end
end

-- Remembers the options given on this command line. Called when generating.
function saveGenerationOptions()
	if next(explicitGenerationOptions) == nil then
		return
	end
	local saved = ini.load(optionsFile)
	for name, value in pairs(explicitGenerationOptions) do
		if switches[name] then
			value = offValues[value:lower()] and "off" or "on"
		end
		ini.set(saved, "Options", name, value)
	end
	ini.save(saved, optionsFile)
end

-- The generation options given on this command line, for a command that generates on its behalf
-- (and remembers them in turn). Without bWithThirdParty, all but --thirdparty.
function generationOptions(bWithThirdParty)
	local arguments = {}
	for _, name in ipairs(generationOptionNames) do
		local value = explicitGenerationOptions[name]
		if value ~= nil and (bWithThirdParty or name ~= "thirdparty") then
			table.insert(arguments, value == "" and "--" .. name or '"--' .. name .. "=" .. value .. '"')
		end
	end
	return table.concat(arguments, " ")
end
