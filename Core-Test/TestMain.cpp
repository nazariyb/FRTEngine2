#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include "TestCommon.h"

#include "Containers/String.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"


namespace
{
std::vector<frt::FString> gMainArguments;

struct SChildMode
{
	frt::FString Name;
	frt::test::FChildMain Main = nullptr;
};

// A function-local static: test files register from their own static initializers, in any order
// relative to this file's.
std::vector<SChildMode>& GetChildModes ()
{
	static std::vector<SChildMode> modes;
	return modes;
}

// "--frt-test-child=<Name> ...": the registered function's result, or 2 for an unknown name.
int RunChildMode (frt::FStringView Name, std::span<const frt::FString> Arguments)
{
	for (const SChildMode& mode : GetChildModes())
	{
		if (mode.Name == Name)
		{
			return mode.Main(Arguments);
		}
	}
	std::fprintf(stderr, "Core-Test: no child mode '%s'\n", frt::FString(Name).CStr());
	return 2;
}
}

const std::vector<frt::FString>& frt::test::GetMainArguments ()
{
	return gMainArguments;
}

bool frt::test::RegisterChildMode (FStringView Name, FChildMain Main)
{
	GetChildModes().push_back({ FString(Name), Main });
	return true;
}

std::filesystem::path frt::test::GetTestModulePath ()
{
	const std::filesystem::path executable = platform::GetExecutablePath();
	const FString stem = FString::FromPath(executable.stem());
	constexpr FStringLiteral prefix = "Core-Test";
	if (!stem.StartsWith(prefix))
	{
		return {};
	}
	FString name("Core-TestModule");
	name.Append(stem.View().RightChop(prefix.Len())).Append(".dll");
	return executable.parent_path() / name.ToPath();
}


// GoogleTest's own gtest_main is not built: with GoogleTest a DLL, its main() would be a
// definition of a dllimport function.
int main (int ArgCount, char** Args)
{
	// Test names, values and failure messages are UTF-8: show them as such on the console the
	// tests run in (its code pages are given back at exit).
	frt::platform::InitConsoleUtf8();

	// Before InitGoogleTest takes its flags out of Args (see GetMainArguments).
	for (int i = 1; i < ArgCount; ++i)
	{
		gMainArguments.emplace_back(Args[i]);
	}

	// A child mode, never mixed with a test run (see TestCommon.h).
	if (!gMainArguments.empty() && gMainArguments[0].StartsWith(frt::test::ChildModeSwitch))
	{
		const frt::FStringView name = gMainArguments[0].View().RightChop(frt::test::ChildModeSwitch.Len());
		return RunChildMode(name, std::span<const frt::FString>(gMainArguments).subspan(1u));
	}

	// Every scratch repository a test makes is below the temporary folder, which can itself be in a work tree (a dotfiles
	// repository in the home folder holds %TEMP%). Git must not find that one from a scratch folder that has none, nor the
	// Git provider's own walk, which reads the same variable; child processes, AssetTool's included, inherit it.
	{
		std::error_code ec;
		const std::filesystem::path temp = std::filesystem::canonical(std::filesystem::temp_directory_path(ec), ec);
		if (!ec)
		{
			frt::FString ceilings = frt::FString::FromPath(frt::platform::NormalizePath(temp), frt::EPathStyle::Generic);
			if (const std::optional<frt::FString> previous = frt::platform::GetEnvironmentValue("GIT_CEILING_DIRECTORIES");
				previous.has_value() && !previous->IsEmpty())
			{
				ceilings.Append(";").Append(*previous);
			}
			frt::platform::SetEnvironmentValue("GIT_CEILING_DIRECTORIES", ceilings);
		}
	}

	::testing::InitGoogleTest(&ArgCount, Args);
	return RUN_ALL_TESTS();
}
