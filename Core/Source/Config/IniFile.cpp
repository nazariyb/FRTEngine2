#include "Config/IniFile.h"

#include <fstream>
#include <iterator>
#include <system_error>

// Enum.h is header-only and depends on nothing but CoreTypes.h, so the bootstrap layer can
// borrow its case-insensitive compare rather than growing a second copy of it.
#include "Enum.h"


namespace frt::config
{
namespace
{
constexpr std::string_view Whitespace = " \t\r\n\f\v";
constexpr std::string_view Utf8Bom = "\xEF\xBB\xBF";


std::string_view Trim (std::string_view Text)
{
	const size_t begin = Text.find_first_not_of(Whitespace);
	if (begin == std::string_view::npos)
	{
		return {};
	}

	const size_t end = Text.find_last_not_of(Whitespace);
	return Text.substr(begin, end - begin + 1);
}


// Strips one layer of surrounding double quotes, which is how a value keeps leading or
// trailing spaces through a round trip. See NeedsQuoting for the writing half.
std::string_view Unquote (std::string_view Text)
{
	if (Text.size() >= 2 && Text.front() == '"' && Text.back() == '"')
	{
		return Text.substr(1, Text.size() - 2);
	}
	return Text;
}


// Quote when reading the value back verbatim would not reproduce it: either the trim on
// load would eat significant spaces, or the value is itself already quote-wrapped and the
// unquote on load would strip a layer that belongs to the data.
bool NeedsQuoting (std::string_view Value)
{
	if (Value.empty())
	{
		return false;
	}

	if (Trim(Value) != Value)
	{
		return true;
	}

	return Value.size() >= 2 && Value.front() == '"' && Value.back() == '"';
}
}


bool CIniFile::LoadFromFile (const std::filesystem::path& Path)
{
	Clear();

	// Binary: line endings are handled by the trim in Parse, and text mode would only
	// hide whether the file on disk is CRLF or LF.
	std::ifstream in(Path, std::ios::binary);
	if (!in)
	{
		return false;
	}

	const std::string text(
		(std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());

	Parse(text);
	return true;
}


void CIniFile::Parse (std::string_view Text)
{
	Clear();

	// Notepad and most Windows editors write a UTF-8 BOM. Without this the first line
	// parses as "\xEF\xBB\xBF[Display]" and the whole leading section is silently lost.
	if (Text.starts_with(Utf8Bom))
	{
		Text.remove_prefix(Utf8Bom.size());
	}

	std::string currentSection;

	size_t pos = 0;
	while (pos <= Text.size())
	{
		const size_t eol = Text.find('\n', pos);
		const size_t count = (eol == std::string_view::npos) ? std::string_view::npos : eol - pos;
		const std::string_view line = Trim(Text.substr(pos, count));

		// Past the end once the last line has been consumed; the loop condition ends it.
		pos = (eol == std::string_view::npos) ? Text.size() + 1 : eol + 1;

		if (line.empty() || line.front() == ';' || line.front() == '#')
		{
			continue;
		}

		if (line.front() == '[')
		{
			// rfind so a ']' inside the name does not truncate it.
			const size_t close = line.rfind(']');
			if (close == std::string_view::npos)
			{
				continue;
			}

			currentSection = Trim(line.substr(1, close - 1));

			// A header with no keys under it still counts as present, so a caller
			// iterating GetSections sees it.
			if (FindSection(currentSection) == nullptr)
			{
				Sections.push_back(SSection{ currentSection, {} });
			}
			continue;
		}

		const size_t equals = line.find('=');
		if (equals == std::string_view::npos)
		{
			continue;
		}

		const std::string_view key = Trim(line.substr(0, equals));
		if (key.empty())
		{
			continue;
		}

		Set(currentSection, key, Unquote(Trim(line.substr(equals + 1))));
	}
}


bool CIniFile::SaveToFile (const std::filesystem::path& Path) const
{
	std::error_code ec;
	if (Path.has_parent_path())
	{
		// Ignored deliberately: "already exists" is the common case and reports as a
		// failure on some implementations. A directory that genuinely could not be created
		// surfaces as the stream failing to open, which is the error worth returning.
		std::filesystem::create_directories(Path.parent_path(), ec);
	}

	// Binary plus explicit "\n" rather than text mode, so the file is byte-identical
	// regardless of platform and a diff of a saved settings file stays readable.
	std::ofstream out(Path, std::ios::binary | std::ios::trunc);
	if (!out)
	{
		return false;
	}

	bool bFirstSection = true;
	for (const SSection& section : Sections)
	{
		if (!section.Name.empty())
		{
			if (!bFirstSection)
			{
				out << "\n";
			}
			out << "[" << section.Name << "]\n";
		}
		bFirstSection = false;

		for (const SEntry& entry : section.Entries)
		{
			out << entry.Key << "=";
			if (NeedsQuoting(entry.Value))
			{
				out << "\"" << entry.Value << "\"";
			}
			else
			{
				out << entry.Value;
			}
			out << "\n";
		}
	}

	out.flush();
	return out.good();
}


const std::string* CIniFile::Find (std::string_view Section, std::string_view Key) const
{
	const SSection* section = FindSection(Section);
	if (section == nullptr)
	{
		return nullptr;
	}

	for (const SEntry& entry : section->Entries)
	{
		if (enum_::EqualsIgnoreCase(entry.Key, Key))
		{
			return &entry.Value;
		}
	}
	return nullptr;
}


void CIniFile::Set (std::string_view Section, std::string_view Key, std::string_view Value)
{
	SSection* section = FindSection(Section);
	if (section == nullptr)
	{
		Sections.push_back(SSection{ std::string(Section), {} });
		section = &Sections.back();
	}

	for (SEntry& entry : section->Entries)
	{
		if (enum_::EqualsIgnoreCase(entry.Key, Key))
		{
			entry.Value = Value;
			return;
		}
	}

	section->Entries.push_back(SEntry{ std::string(Key), std::string(Value) });
}


bool CIniFile::Remove (std::string_view Section, std::string_view Key)
{
	SSection* section = FindSection(Section);
	if (section == nullptr)
	{
		return false;
	}

	for (size_t i = 0; i < section->Entries.size(); ++i)
	{
		if (enum_::EqualsIgnoreCase(section->Entries[i].Key, Key))
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


CIniFile::SSection* CIniFile::FindSection (std::string_view Name)
{
	return const_cast<SSection*>(static_cast<const CIniFile&>(*this).FindSection(Name));
}


const CIniFile::SSection* CIniFile::FindSection (std::string_view Name) const
{
	for (const SSection& section : Sections)
	{
		if (enum_::EqualsIgnoreCase(section.Name, Name))
		{
			return &section;
		}
	}
	return nullptr;
}
}
