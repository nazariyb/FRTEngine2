#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"


namespace frt::config
{
/**
 * One .ini file held in memory.
 *
 * Deliberately the most primitive thing in the engine: it depends on the standard library
 * and nothing else. Not the memory pool - hence std::vector rather than TArray, since
 * config is read before MakeThisPrimaryInstance() has run. Not the asset system, not
 * EnginePaths, not logging. A config layer that needs a subsystem in order to be parsed
 * cannot be the thing that configures that subsystem.
 *
 * Format, kept small on purpose:
 *
 *     ; comment          whole-line only, ';' or '#'
 *     [Section]          name trimmed
 *     Key=Value          first '=' splits, both sides trimmed
 *     Key="  Value  "    one layer of surrounding double quotes stripped
 *
 * Not supported, by choice: inline comments (so a value may legally contain ';' or '#',
 * which paths and format strings do), escape sequences, and line continuations. Malformed
 * lines are skipped rather than treated as errors - a bad key must never stop the engine
 * from starting.
 *
 * Section and key lookup is case-insensitive. The spelling first seen is the one
 * SaveToFile writes back. Keys appearing before the first [Section] belong to the unnamed
 * section "". Within one file a repeated key overwrites the earlier one.
 */
class FRT_CORE_API CIniFile
{
public:
	struct SEntry
	{
		std::string Key;
		std::string Value;
	};

	struct SSection
	{
		std::string Name;
		std::vector<SEntry> Entries;
	};

	// A missing file is not an error: returns false and leaves the object empty, which is
	// exactly what an absent optional layer should do. Returns true only if the file was
	// opened and read.
	bool LoadFromFile (const std::filesystem::path& Path);

	// Replaces the current contents. Exposed separately from LoadFromFile so tests and
	// embedded defaults do not need a file on disk.
	void Parse (std::string_view Text);

	// Regenerates the file from the in-memory model, creating parent directories as
	// needed. Comments and blank lines in the original are NOT preserved: only the
	// machine-generated user layer is ever saved, and the hand-authored layers are
	// read-only by design. Always writes LF endings.
	bool SaveToFile (const std::filesystem::path& Path) const;

	// nullptr when absent. The pointer is invalidated by the next Set/Parse/Load/Clear.
	const std::string* Find (std::string_view Section, std::string_view Key) const;

	void Set (std::string_view Section, std::string_view Key, std::string_view Value);
	bool Remove (std::string_view Section, std::string_view Key);

	void Clear ();
	bool IsEmpty () const;

	const std::vector<SSection>& GetSections () const { return Sections; }

private:
	SSection* FindSection (std::string_view Name);
	const SSection* FindSection (std::string_view Name) const;

	std::vector<SSection> Sections;
};
}
