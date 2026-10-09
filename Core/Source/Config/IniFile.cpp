#include "Config/IniFile.h"

#include <optional>

#include "Platform/Platform.h"


namespace frt::config
{
namespace
{
// Quote when reading the value back verbatim would not reproduce it: either the trim on
// load would eat significant spaces, or the value is itself already quote-wrapped and the
// unquote on load would strip a layer that belongs to the data.
bool NeedsQuoting (FStringView Value)
{
	if (Value.IsEmpty())
	{
		return false;
	}

	// The set Parse trims: a value ending in NBSP keeps it through the round trip unquoted.
	if (Value.TrimStartAndEnd(AsciiWhitespace) != Value)
	{
		return true;
	}

	return Value.Len() >= 2u && Value.StartsWith('"') && Value.EndsWith('"');
}
}


bool CIniFile::LoadFromFile (const std::filesystem::path& Path)
{
	Clear();

	// The bytes as stored, not Detect: Premake/project.lua reads these files untranscoded, so a
	// file saved as UTF-16 must fail here as it fails there, rather than open in the editor
	// and then break the build. Parse strips a UTF-8 BOM, as Lua does. Line endings are left
	// as they are too: text mode would only hide whether the file on disk is CRLF or LF.
	const std::optional<FString> text = platform::ReadFileText(Path, ETextFileEncoding::Raw);
	if (!text)
	{
		return false;
	}

	Parse(*text);
	return true;
}


void CIniFile::Parse (FStringView Text)
{
	Clear();

	// Notepad and most Windows editors write a UTF-8 BOM. Without this the first line
	// parses as "\xEF\xBB\xBF[Display]" and the whole leading section is silently lost.
	Text.RemoveFromStart(Utf8Bom);

	FStringView currentSection;
	for (FStringView line : Text.Lines())
	{
		line.TrimStartAndEndInline(AsciiWhitespace);
		if (line.IsEmpty() || line.StartsWith(';') || line.StartsWith('#'))
		{
			continue;
		}

		if (line.StartsWith('['))
		{
			// The last ']', so a ']' inside the name does not truncate it.
			const SStringSplit header = line.RightChop(1u).SplitLast(']');
			if (!header)
			{
				continue;
			}

			currentSection = header.Left.TrimStartAndEnd(AsciiWhitespace);

			// A header with no keys under it still counts as present, so a caller
			// iterating GetSections sees it.
			if (FindSection(currentSection) == nullptr)
			{
				Sections.push_back(SSection{ FString(currentSection), {} });
			}
			continue;
		}

		const SStringSplit keyValue = line.SplitFirst('=');
		if (!keyValue)
		{
			continue;
		}

		const FStringView key = keyValue.Left.TrimStartAndEnd(AsciiWhitespace);
		if (key.IsEmpty())
		{
			continue;
		}

		// One layer of quotes: see NeedsQuoting for the writing half.
		Set(currentSection, key, keyValue.Right.TrimStartAndEnd(AsciiWhitespace).TrimQuotes());
	}
}


bool CIniFile::SaveToFile (const std::filesystem::path& Path) const
{
	// Explicit "\n" rather than the platform's line ending, so the file is byte-identical
	// regardless of platform and a diff of a saved settings file stays readable.
	FString text;
	bool bFirstSection = true;
	for (const SSection& section : Sections)
	{
		if (!section.Name.IsEmpty())
		{
			if (!bFirstSection)
			{
				text << '\n';
			}
			text << '[' << section.Name << "]\n";
		}
		bFirstSection = false;

		for (const SEntry& entry : section.Entries)
		{
			text << entry.Key << '=';
			if (NeedsQuoting(entry.Value))
			{
				text << '"' << entry.Value << '"';
			}
			else
			{
				text << entry.Value;
			}
			text << '\n';
		}
	}

	// Creates the parent directories as needed: a directory that genuinely could not be
	// created surfaces as the write failing, which is the error worth returning.
	return platform::WriteFileText(Path, text);
}


const FString* CIniFile::Find (FStringView Section, FStringView Key) const
{
	const SSection* section = FindSection(Section);
	if (section == nullptr)
	{
		return nullptr;
	}

	for (const SEntry& entry : section->Entries)
	{
		if (entry.Key.Equals<ESearchCase::IgnoreCaseAscii>(Key))
		{
			return &entry.Value;
		}
	}
	return nullptr;
}


void CIniFile::Set (FStringView Section, FStringView Key, FStringView Value)
{
	SSection* section = FindSection(Section);
	if (section == nullptr)
	{
		Sections.push_back(SSection{ FString(Section), {} });
		section = &Sections.back();
	}

	for (SEntry& entry : section->Entries)
	{
		if (entry.Key.Equals<ESearchCase::IgnoreCaseAscii>(Key))
		{
			entry.Value = Value;
			return;
		}
	}

	section->Entries.push_back(SEntry{ FString(Key), FString(Value) });
}


bool CIniFile::Remove (FStringView Section, FStringView Key)
{
	SSection* section = FindSection(Section);
	if (section == nullptr)
	{
		return false;
	}

	for (size_t i = 0; i < section->Entries.size(); ++i)
	{
		if (section->Entries[i].Key.Equals<ESearchCase::IgnoreCaseAscii>(Key))
		{
			section->Entries.erase(section->Entries.begin() + static_cast<ptrdiff_t>(i));
			return true;
		}
	}
	return false;
}


void CIniFile::Clear ()
{
	Sections.clear();
}


bool CIniFile::IsEmpty () const
{
	for (const SSection& section : Sections)
	{
		if (!section.Entries.empty())
		{
			return false;
		}
	}
	return true;
}


CIniFile::SSection* CIniFile::FindSection (FStringView Name)
{
	return const_cast<SSection*>(static_cast<const CIniFile&>(*this).FindSection(Name));
}


const CIniFile::SSection* CIniFile::FindSection (FStringView Name) const
{
	for (const SSection& section : Sections)
	{
		if (section.Name.Equals<ESearchCase::IgnoreCaseAscii>(Name))
		{
			return &section;
		}
	}
	return nullptr;
}
}
