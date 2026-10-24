#include "Commandlet.h"

#include <cstdio>

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
