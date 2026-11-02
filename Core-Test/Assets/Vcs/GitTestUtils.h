#pragma once

#include <filesystem>
#include <vector>

#include "TestProcess.h"
#include "Containers/String.h"


namespace frt::test
{
// One git command in Repository, with an identity of its own and the working files left as they are, whatever this
// machine's user configuration says. Git is platform::FindOnPath("git"); tests skip without it.
inline SProcessResult RunGit (const std::filesystem::path& Git, const std::filesystem::path& Repository, std::vector<FString> Arguments)
{
	const std::vector<FString> prefix = { "-C", FString::FromPath(Repository), "-c", "core.autocrlf=false", "-c", "user.name=FRT Test",
		"-c", "user.email=frt-test@example.invalid", "-c", "commit.gpgsign=false", "-c", "core.quotePath=false" };
	Arguments.insert(Arguments.begin(), prefix.begin(), prefix.end());
	return RunProcess(Git, Arguments, Repository);
}
}
