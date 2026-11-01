#include "Commandlet.h"

#include <cstdio>
#include <optional>
#include <span>

#include "Platform/Platform.h"
#include "Project/Project.h"


namespace frt::assettool
{
namespace
{
// Whole lines in one write each, flushed: a script reading the stream line by line, or a test
// reading both streams of a killed process, sees every line that was printed, never half of one.
void WriteLine (std::FILE* Stream, FStringView Line)
{
	std::fwrite(Line.GetData(), 1u, Line.Len(), Stream);
	std::fputc('\n', Stream);
	std::fflush(Stream);
}

// An option the host reads for every verb (AssetTool.cpp), and whether it is written with a value.
struct SHostOption
{
	FStringLiteral Name;
	bool bTakesValue = false;
};

constexpr SHostOption HostOptions[] =
{
	{ "Run", true },
	{ "Project", true },
	{ "json", false },
	{ "ci", false },
	{ "engine", false },
	{ "help", false },
	{ "?", false },
};

// Body is an option without its dashes ("json", "Project=X", "Assets:ReadOnly=1").
bool IsKnownOption (FStringView Body, const SCommandlet* Commandlet)
{
	const SStringSplit nameAndValue = Body.SplitFirst('=');
	const FStringView name = nameAndValue ? nameAndValue.Left : Body;
	if (name.IsEmpty())
	{
		return false;
	}
	// No option's name, section or key holds whitespace: such an argument is text ("-label:Debug Width=5", one
	// quoted query), never a config override.
	for (const char ch : name)
	{
		if (ch == ' ' || ch == '\t')
		{
			return false;
		}
	}
	// -Section:Key=Value, a config override (CConfig::ApplyCommandLineArguments): the first ':' ends
	// the section. Without a value, a section or a key, it would be ignored there.
	if (const SStringSplit sectionAndKey = name.SplitFirst(':'))
	{
		return nameAndValue && !sectionAndKey.Left.IsEmpty() && !sectionAndKey.Right.IsEmpty();
	}
	for (const SHostOption& option : HostOptions)
	{
		if (name.Equals<ESearchCase::IgnoreCaseAscii>(option.Name))
		{
			return nameAndValue.bFound == option.bTakesValue;
		}
	}
	if (Commandlet != nullptr)
	{
		for (const FStringLiteral option : Commandlet->Options)
		{
			if (name.Equals<ESearchCase::IgnoreCaseAscii>(option))
			{
				return true;
			}
		}
	}
	return false;
}

// Argument, one before "--", is text of Commandlet's (a negated query term), not an option: the verb takes text,
// it has one dash, and it is no option of the host or the verb.
bool IsText (const FString& Argument, const SCommandlet* Commandlet)
{
	return Commandlet != nullptr && Commandlet->bTakesText && Argument.StartsWith('-') && !Argument.StartsWith("--")
		&& !IsKnownOption(Argument.View().RightChop(1u), Commandlet);
}

/**
 * Visits the positional arguments in order (GetOptionArguments' rules), with bVerbCandidate false for the text
 * IsText keeps (an argument with a dash before "--" is never a verb).
 */
template <typename TVisit>
void ForEachPositional (std::span<const FString> CommandLine, const SCommandlet* Commandlet, TVisit&& Visit)
{
	const size_t optionCount = GetOptionArguments(CommandLine).size();
	for (size_t i = 0u; i < CommandLine.size(); ++i)
	{
		const FString& argument = CommandLine[i];
		if (i == optionCount || project::HasProjectFileExtension(argument.ToPath()))
		{
			continue;   // the "--" itself, or the project
		}
		if (i < optionCount && argument.StartsWith('-'))
		{
			if (IsText(argument, Commandlet))
			{
				Visit(argument, false);
			}
			continue;
		}
		Visit(argument, true);
	}
}
}


std::span<const FString> GetOptionArguments (std::span<const FString> CommandLine)
{
	for (size_t i = 0u; i < CommandLine.size(); ++i)
	{
		if (CommandLine[i].View() == "--")
		{
			return CommandLine.first(i);
		}
	}
	return CommandLine;
}

std::optional<FString> FindVerb (std::span<const FString> CommandLine)
{
	if (std::optional<FString> run = platform::FindCommandLineValue(GetOptionArguments(CommandLine), "Run"))
	{
		return run;
	}
	std::optional<FString> verb;
	ForEachPositional(CommandLine, nullptr, [&] (const FString& Argument, bool)
	{
		if (!verb.has_value())
		{
			verb = Argument;
		}
	});
	return verb;
}

std::vector<FString> GetPositionalArguments (std::span<const FString> CommandLine, const SCommandlet* Commandlet)
{
	bool bVerbTaken = platform::FindCommandLineValue(GetOptionArguments(CommandLine), "Run").has_value();
	std::vector<FString> arguments;
	ForEachPositional(CommandLine, Commandlet, [&] (const FString& Argument, bool bVerbCandidate)
	{
		if (!bVerbTaken && bVerbCandidate)
		{
			bVerbTaken = true;
			return;
		}
		arguments.push_back(Argument);
	});
	return arguments;
}

std::optional<FString> FindUnknownOption (std::span<const FString> CommandLine, const SCommandlet* Commandlet)
{
	for (const FString& argument : GetOptionArguments(CommandLine))
	{
		if (argument.StartsWith('-') && !IsText(argument, Commandlet)
			&& !IsKnownOption(argument.View().RightChop(argument.StartsWith("--") ? 2u : 1u), Commandlet))
		{
			return argument;
		}
	}
	return std::nullopt;
}


FStringView CCommandletContext::GetMountName () const
{
	if (bEngineOwner)
	{
		return "/Engine";
	}
	return ProjectRoot.empty() ? FStringView() : FStringView("/Project");
}

const std::filesystem::path& CCommandletContext::GetMountRoot () const
{
	return bEngineOwner ? EngineRoot : ProjectRoot;
}

std::optional<FString> CCommandletContext::FindOption (FStringView Name) const
{
	return platform::FindCommandLineValue(GetOptionArguments(CommandLine), Name);
}

bool CCommandletContext::HasOption (FStringView Name) const
{
	return platform::HasCommandLineSwitch(GetOptionArguments(CommandLine), Name);
}

void CCommandletContext::Print (FStringView Line) const
{
	if (!bJson)
	{
		WriteLine(stdout, Line);
	}
}

void CCommandletContext::PrintJson (const CJsonObject& Object) const
{
	if (bJson)
	{
		WriteLine(stdout, Object.ToString());
	}
}

void CCommandletContext::PrintJsonLine (FStringView Line) const
{
	if (bJson)
	{
		WriteLine(stdout, Line);
	}
}

void CCommandletContext::PrintText (FStringView Text) const
{
	if (!bJson)
	{
		std::fwrite(Text.GetData(), 1u, Text.Len(), stdout);
		std::fflush(stdout);
	}
}

void CCommandletContext::PrintError (FStringView Line) const
{
	WriteLine(stderr, Line);
}

int32 CCommandletContext::Fail (int32 ExitCode, FStringView Message) const
{
	return Fail(ExitCode, Message, CJsonObject());
}

int32 CCommandletContext::Fail (int32 ExitCode, FStringView Message, const CJsonObject& Extra) const
{
	PrintError(Message);
	if (bJson)
	{
		CJsonObject object;
		object.AddString("error", Message).AddInt("exitCode", ExitCode).AddFields(Extra);
		WriteLine(stdout, object.ToString());
	}
	return ExitCode;
}
}
