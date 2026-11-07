#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "TestProcess.h"
#include "Containers/String.h"
#include "Platform/Platform.h"


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

// One environment variable of this process set for a scope, and given back (or removed) when it ends, however it ends: an
// ASSERT returns early, and every git a later test starts would inherit a GIT_INDEX_FILE or a FRT_HOOK_TOPLEVEL left set.
// An empty Value removes the variable.
class CScopedEnvironment
{
public:
	CScopedEnvironment (FStringView InName, FStringView Value)
		: Name(InName)
		, Saved(platform::GetEnvironmentValue(InName))
	{
		platform::SetEnvironmentValue(Name, Value);
	}

	~CScopedEnvironment ()
	{
		platform::SetEnvironmentValue(Name, Saved.value_or(FString()));
	}

	CScopedEnvironment (const CScopedEnvironment&) = delete;
	CScopedEnvironment& operator= (const CScopedEnvironment&) = delete;

private:
	FString Name;
	std::optional<FString> Saved;
};
}
