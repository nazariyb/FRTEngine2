#include "Assets/Discovery/BuiltinExcludes.h"

#include "Assets/Registry/RegistryTypes.h"


namespace frt::assets
{
namespace
{
// Name ends with Suffix under the portable fold.
bool EndsWithFolded (FStringView Name, FStringView Suffix) noexcept
{
	return Name.EndsWith<ESearchCase::IgnoreCase>(Suffix);
}

bool IsAsciiDigit (char Ch) noexcept
{
	return Ch >= '0' && Ch <= '9';
}

// "*.blend[0-9]" or "*.blend[0-9][0-9]": one or two digits after ".blend", nothing else.
bool IsBlenderBackup (FStringView Name) noexcept
{
	uint32 digits = 0u;
	while (digits < 3u && digits < Name.Len() && IsAsciiDigit(Name[Name.Len() - 1u - digits]))
	{
		++digits;
	}
	return (digits == 1u || digits == 2u) && EndsWithFolded(Name.LeftChop(digits), ".blend");
}
}


bool IsBuiltinExcludedName (FStringView Name, bool bDirectory) noexcept
{
	if (Name.IsEmpty())
	{
		return false;
	}
	if (Name[0] == '.')
	{
		return true;   // dot-names, ".frt-fence-*" among them; directories too
	}
	if (bDirectory)
	{
		return false;
	}
	return Name.EndsWith('~')
		|| Name.StartsWith("~$")
		|| EndsWithFolded(Name, ".tmp")
		|| EndsWithFolded(Name, ".orig")
		|| EndsWithFolded(Name, ".meta")
		|| EndsWithFolded(Name, ".blend@")
		|| EndsWithFolded(Name, "-autosave.kra")
		|| IsBlenderBackup(Name)
		|| Name.Equals<ESearchCase::IgnoreCase>("Thumbs.db")
		|| Name.Equals<ESearchCase::IgnoreCase>("desktop.ini")
		|| IsRuleFileName(Name);
}

bool IsPreferredRuleFileName (FStringView Candidate, FStringView Current) noexcept
{
	if (Current.IsEmpty())
	{
		return true;
	}
	if (Current == RuleFileName || Candidate == Current)
	{
		return false;
	}
	if (Candidate == RuleFileName)
	{
		return true;
	}
	return LessPortableThenBytes(Candidate, Current);
}
}
