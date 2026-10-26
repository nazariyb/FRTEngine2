#include "Commandlet.h"

#include <cstdio>
#include <optional>
#include <span>

#include "Platform/Platform.h"


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
}


std::optional<FString> FindUnknownOption (std::span<const FString> CommandLine, const SCommandlet* Commandlet)
{
	for (const FString& argument : CommandLine)
	{
		if (argument.StartsWith('-') && !IsKnownOption(argument.View().RightChop(argument.StartsWith("--") ? 2u : 1u), Commandlet))
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
	return platform::FindCommandLineValue(CommandLine, Name);
}

bool CCommandletContext::HasOption (FStringView Name) const
{
	return platform::HasCommandLineSwitch(CommandLine, Name);
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
