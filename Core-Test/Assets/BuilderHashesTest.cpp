// The builder source guard's build-time half (Premake/tools.lua, design §7.3, BUILD003), run as FRT.bat
// runs it - the engine's vendored premake5.exe on a scratch copy of premake5.lua and Premake/, with a
// builder table of its own and a git repository: builder-hashes writes each builder's source hash - FNV-1a
// 64 over its files in byte order of their paths, carriage returns removed, exactly as the Lua documents
// it - and the matched files for the pipeline lint, rewriting neither when nothing changed and replacing
// each in one rename when something did, so a reader (another solution's build, the lint) never sees a
// truncated file and one that holds the file open only delays the write; a CRLF and an LF copy of a
// source hash alike; check-builders passes while every builder's sources match the hash
// committed with its version, fails with BUILD003 for one that changed with HEAD's GUID, and --update
// records new hashes only when every changed builder has a GUID HEAD does not, writing nothing otherwise.
// The table parser ignores rows in comments and joins adjacent literals. Paths are non-ASCII. Skipped
// without git on PATH.
#include <gtest/gtest.h>

#include "TestCommon.h"
#include "TestProcess.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#include "EnginePaths.h"
#include "Containers/String.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"

#include <Windows.h>

using namespace frt;


namespace
{
constexpr FStringLiteral AlphaGuid = "6b1d1f0e2a3c4b5d8e9f0a1b2c3d4e5f";
constexpr FStringLiteral AlphaGuid2 = "7c2e2a1f3b4d4c6e9fa01b2c3d4e5f60";
constexpr FStringLiteral BetaGuid = "8d3f3b2a4c5e4d7fa0b12c3d4e5f6071";
constexpr FStringLiteral BetaGuid2 = "9e4a4c3b5d6f4e80b1c23d4e5f607182";
constexpr FStringLiteral ZeroHash = "0000000000000000";

// The table: two builders, a commented-out row the parser must skip, a glob list split over literals.
FString MakeTable (FStringView AlphaVersion, FStringView AlphaHash, FStringView BetaVersion, FStringView BetaHash)
{
	return FString::Format(
		"// A scratch builder table.\n"
		"// FRT_BUILDER(\"Commented.Out\", \"{}\", \"{}\", \"Core/**\")\n"
		"FRT_BUILDER(\"Test.Alpha\", \"{}\", \"{}\",\n"
		"\t\"Core/Source/Alpha/*.cpp;\"\n"
		"\t\"Core/Source/Shared/Common.h\")\n"
		"/* FRT_BUILDER(\"Also.Out\", \"x\", \"y\", \"z\") */\n"
		"FRT_BUILDER(\"Test.Beta\", \"{}\", \"{}\", \"Core/Source/Beta/**\")\n",
		AlphaGuid, ZeroHash, AlphaVersion, AlphaHash, BetaVersion, BetaHash);
}

// 64-bit FNV-1a of Bytes, continuing from Hash.
uint64 Fnv1a (uint64 Hash, FStringView Bytes)
{
	for (const char c : Bytes)
	{
		Hash = (Hash ^ static_cast<uint8>(c)) * 0x100000001b3ull;
	}
	return Hash;
}

std::optional<std::filesystem::path> FindGitOnPath ()
{
	const std::optional<FString> path = platform::GetEnvironmentValue("PATH");
	if (!path)
	{
		return std::nullopt;
	}
	for (const FStringView dir : path->View().Split(';', EStringSplit::SkipEmpty | EStringSplit::Trim))
	{
		const std::filesystem::path candidate = dir.TrimQuotes().ToPath() / "git.exe";
		if (platform::Stat(candidate).has_value())
		{
			return candidate;
		}
	}
	return std::nullopt;
}


class BuilderHashesTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		const std::optional<std::filesystem::path> git = FindGitOnPath();
		if (!git)
		{
			GTEST_SKIP() << "no git.exe on PATH";
		}
		Git = *git;
		Premake = paths::GetEngineDir() / "ThirdParty" / "premake-5.0.0-beta2-windows" / "premake5.exe";
		ASSERT_TRUE(platform::Stat(Premake).has_value()) << FString::FromPath(Premake).CStr();

		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Збирачі 构建器 {}", platform::GetCurrentProcessId()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		Engine = Dir / FStringView("Рушій 引擎").ToPath();
		ASSERT_TRUE(std::filesystem::create_directories(Engine / "Core" / "Source" / "Assets", ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
		std::filesystem::copy_file(paths::GetEngineDir() / "premake5.lua", Engine / "premake5.lua", ec);
		ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();
		std::filesystem::copy(paths::GetEngineDir() / "Premake", Engine / "Premake", std::filesystem::copy_options::recursive, ec);
		ASSERT_FALSE(ec) << FString::FromUtf8Lossy(ec.message()).CStr();

		Write("Core/Source/Alpha/A.cpp", "int Alpha () { return 1; }\n");
		Write("Core/Source/Alpha/Б.cpp", "int Beta () { return 2; }\n");
		Write("Core/Source/Alpha/Ignored.h", "not matched by *.cpp\n");
		Write("Core/Source/Shared/Common.h", "#pragma once\n");
		Write("Core/Source/Beta/Deep/Нутро.inl", "// beta\n");
		Write("Core/Source/Beta/Top.h", "#pragma once // beta\n");
		WriteTable(MakeTable(AlphaGuid, ZeroHash, BetaGuid, ZeroHash));
	}

	void TearDown () override
	{
		if (!Dir.empty())
		{
			std::error_code ec;
			std::filesystem::remove_all(Dir, ec);
		}
	}

	void Write (FStringView Relative, FStringView Text)
	{
		ASSERT_TRUE(platform::WriteFileText(Engine / Relative.ToPath(), Text)) << FString(Relative).CStr();
	}

	void WriteTable (FStringView Text)
	{
		Write("Core/Source/Assets/BuilderVersions.inl", Text);
	}

	[[nodiscard]] FString ReadTable () const
	{
		return platform::ReadFileText(Engine / "Core" / "Source" / "Assets" / "BuilderVersions.inl").value_or(FString());
	}

	// The documented source hash of Files (engine-relative, any order).
	[[nodiscard]] FString ExpectedHash (std::vector<FString> Files) const
	{
		std::sort(Files.begin(), Files.end(), [] (const FString& A, const FString& B) { return A.View().Compare(B.View()) < 0; });
		uint64 hash = 0xcbf29ce484222325ull;
		for (const FString& file : Files)
		{
			FString content = platform::ReadFileText(Engine / file.ToPath(), ETextFileEncoding::Raw).value_or(FString());
			content.RemoveAll('\r');
			hash = Fnv1a(hash, file + FStringView("\0", 1u) + FString::Format("{}", content.Len()) + FStringView("\0", 1u));
			hash = Fnv1a(hash, content);
		}
		return FString::Format("{:016x}", hash);
	}

	[[nodiscard]] FString AlphaHash () const
	{
		return ExpectedHash({ "Core/Source/Alpha/A.cpp", "Core/Source/Alpha/Б.cpp", "Core/Source/Shared/Common.h" });
	}

	[[nodiscard]] FString BetaHash () const
	{
		return ExpectedHash({ "Core/Source/Beta/Deep/Нутро.inl", "Core/Source/Beta/Top.h" });
	}

	test::SProcessResult RunPremake (std::vector<FString> Arguments) const
	{
		Arguments.insert(Arguments.begin(), "--file=" + FString::FromPath(Engine / "premake5.lua"));
		return test::RunProcess(Premake, Arguments, Engine);
	}

	void RunGit (std::vector<FString> Arguments) const
	{
		const std::vector<FString> prefix = { "-C", FString::FromPath(Engine), "-c", "core.autocrlf=false", "-c", "user.name=FRT Test",
			"-c", "user.email=frt-test@example.invalid", "-c", "commit.gpgsign=false" };
		Arguments.insert(Arguments.begin(), prefix.begin(), prefix.end());
		const test::SProcessResult result = test::RunProcess(Git, Arguments, Engine);
		ASSERT_EQ(result.ExitCode, 0) << Arguments.back().CStr() << ": " << result.Errors.CStr() << result.Output.CStr();
	}

	void CommitAll (FStringView Message)
	{
		RunGit({ "add", "--all" });
		RunGit({ "commit", "-q", "-m", FString(Message) });
	}

	// Files named *.tmp under Relative: what a write that replaces its target may leave behind.
	[[nodiscard]] std::vector<FString> FindTemporaryFiles (FStringView Relative) const
	{
		std::vector<FString> found;
		std::error_code ec;
		for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(Engine / Relative.ToPath(), ec))
		{
			if (entry.path().extension() == ".tmp")
			{
				found.push_back(FString::FromPath(entry.path().filename()));
			}
		}
		return found;
	}

	std::filesystem::path Git;
	std::filesystem::path Premake;
	std::filesystem::path Dir;
	std::filesystem::path Engine;
};
}


TEST_F(BuilderHashesTest, BuilderHashesWritesTheDocumentedHashesAndThePipelineList)
{
	const test::SProcessResult run = RunPremake({ "builder-hashes" });
	ASSERT_EQ(run.ExitCode, 0) << run.Errors.CStr() << run.Output.CStr();
	// Silent but for what it changed: it runs in every build.
	EXPECT_FALSE(run.Output.Contains("Running action")) << run.Output.CStr();

	const std::filesystem::path header = Engine / "Intermediate" / "Generated" / "BuilderSourceHashes.gen.h";
	const std::optional<FString> text = platform::ReadFileText(header);
	ASSERT_TRUE(text.has_value());
	EXPECT_TRUE(text->Contains(FString::Format("FRT_BUILDER_SOURCE_HASH(\"Test.Alpha\", \"{}\")\n", AlphaHash()))) << text->CStr();
	EXPECT_TRUE(text->Contains(FString::Format("FRT_BUILDER_SOURCE_HASH(\"Test.Beta\", \"{}\")\n", BetaHash()))) << text->CStr();
	EXPECT_FALSE(text->Contains("Commented.Out")) << text->CStr();
	EXPECT_FALSE(text->Contains("Also.Out")) << text->CStr();
	EXPECT_FALSE(text->Contains('\r'));

	const std::optional<FString> list = platform::ReadFileText(Engine / "Local" / "Build" / "PipelineSources.txt");
	ASSERT_TRUE(list.has_value());
	EXPECT_EQ(*list, "Core/Source/Alpha/A.cpp\nCore/Source/Alpha/Б.cpp\nCore/Source/Beta/Deep/Нутро.inl\nCore/Source/Beta/Top.h\n"
		"Core/Source/Shared/Common.h\n");

	// Unchanged sources rewrite nothing (a rewrite would recompile what includes the header).
	const std::optional<platform::SFileStat> before = platform::Stat(header);
	ASSERT_TRUE(before.has_value());
	const test::SProcessResult again = RunPremake({ "builder-hashes" });
	ASSERT_EQ(again.ExitCode, 0) << again.Errors.CStr();
	EXPECT_TRUE(again.Output.IsEmpty()) << again.Output.CStr();
	EXPECT_EQ(platform::Stat(header)->LastWriteTime, before->LastWriteTime);
	EXPECT_EQ(platform::Stat(header)->Identity, before->Identity);

	// CRLF in a source is the same source.
	Write("Core/Source/Alpha/A.cpp", "int Alpha () { return 1; }\r\n");
	const FString alphaWithCrlf = AlphaHash();
	ASSERT_EQ(RunPremake({ "builder-hashes" }).ExitCode, 0);
	EXPECT_EQ(platform::Stat(header)->LastWriteTime, before->LastWriteTime);
	EXPECT_TRUE(platform::ReadFileText(header)->Contains(alphaWithCrlf));

	// A change is a new hash, written.
	Write("Core/Source/Shared/Common.h", "#pragma once\nint Shared;\n");
	const test::SProcessResult changed = RunPremake({ "builder-hashes" });
	ASSERT_EQ(changed.ExitCode, 0) << changed.Errors.CStr();
	EXPECT_TRUE(changed.Output.Contains("BuilderSourceHashes.gen.h updated")) << changed.Output.CStr();
	EXPECT_TRUE(platform::ReadFileText(header)->Contains(FString::Format("\"Test.Alpha\", \"{}\"", AlphaHash())));
	EXPECT_NE(AlphaHash(), alphaWithCrlf);
	// Replaced by another file, not rewritten in place, and nothing left beside it.
	EXPECT_NE(platform::Stat(header)->Identity, before->Identity);
	EXPECT_TRUE(FindTemporaryFiles("Intermediate/Generated").empty());
	EXPECT_TRUE(FindTemporaryFiles("Local/Build").empty());
}

// A reader that holds the header open while a build changes it - another solution's cl.exe, which opens
// it without delete sharing, as the C library does - keeps reading the old content to the end and only
// delays the write, which lands once it lets go: never a truncated or half-written header. (The handle is
// not inherited: builder-hashes, a child of this process, would otherwise hold it too.)
TEST_F(BuilderHashesTest, AReaderHoldingTheHeaderKeepsTheOldContentAndOnlyDelaysTheWrite)
{
	ASSERT_EQ(RunPremake({ "builder-hashes" }).ExitCode, 0);
	const std::filesystem::path header = Engine / "Intermediate" / "Generated" / "BuilderSourceHashes.gen.h";
	const std::optional<FString> old = platform::ReadFileText(header, ETextFileEncoding::Raw);
	ASSERT_TRUE(old.has_value());

	Write("Core/Source/Beta/Top.h", "#pragma once // beta, changed\n");
	const HANDLE reader = CreateFileW(header.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(reader, INVALID_HANDLE_VALUE);
	std::vector<uint8> seen;
	std::thread holder([reader, &seen] ()
	{
		// Long enough for builder-hashes to start and try; then the whole file, through this handle.
		std::this_thread::sleep_for(std::chrono::milliseconds(700));
		uint8 buffer[4096];
		DWORD read = 0;
		while (ReadFile(reader, buffer, sizeof(buffer), &read, nullptr) && read > 0)
		{
			seen.insert(seen.end(), buffer, buffer + read);
		}
		CloseHandle(reader);
	});
	const test::SProcessResult changed = RunPremake({ "builder-hashes" });
	holder.join();

	ASSERT_EQ(changed.ExitCode, 0) << changed.Errors.CStr() << changed.Output.CStr();
	EXPECT_EQ(seen, std::vector<uint8>(old->View().begin(), old->View().end()));
	const std::optional<FString> now = platform::ReadFileText(header);
	ASSERT_TRUE(now.has_value());
	EXPECT_TRUE(now->Contains(FString::Format("FRT_BUILDER_SOURCE_HASH(\"Test.Beta\", \"{}\")\n", BetaHash()))) << now->CStr();
	EXPECT_TRUE(FindTemporaryFiles("Intermediate/Generated").empty());
}

TEST_F(BuilderHashesTest, ABrokenTableFailsTheBuildStepWithItsLine)
{
	WriteTable("FRT_BUILDER(\"Bad\", \"not a guid\", \"0000000000000000\", \"Core/**\")\n");
	const test::SProcessResult bad = RunPremake({ "builder-hashes" });
	EXPECT_NE(bad.ExitCode, 0);
	EXPECT_TRUE((bad.Errors + bad.Output).Contains("BuilderVersions.inl:1: Bad: the version GUID")) << bad.Errors.CStr() << bad.Output.CStr();

	WriteTable(FString::Format("FRT_BUILDER(\"Two\", \"{}\", \"{}\")\n", AlphaGuid, ZeroHash));
	const test::SProcessResult tooShort = RunPremake({ "check-builders" });
	EXPECT_NE(tooShort.ExitCode, 0);
	EXPECT_TRUE((tooShort.Errors + tooShort.Output).Contains("FRT_BUILDER takes 4 arguments")) << tooShort.Errors.CStr()
		<< tooShort.Output.CStr();
}

TEST_F(BuilderHashesTest, CheckBuildersWantsANewGuidForChangedSourcesAndUpdateRecordsOnlyBumpedOnes)
{
	// No git repository yet: changed builders are BUILD003, and --update has no HEAD to compare with.
	const test::SProcessResult noRepository = RunPremake({ "check-builders" });
	EXPECT_EQ(noRepository.ExitCode, 1);
	EXPECT_TRUE(noRepository.Output.Contains("BUILD003 Test.Alpha")) << noRepository.Output.CStr();
	EXPECT_TRUE(noRepository.Output.Contains("BUILD003 Test.Beta")) << noRepository.Output.CStr();
	const FString table = ReadTable();
	const test::SProcessResult noHead = RunPremake({ "check-builders", "--update" });
	EXPECT_EQ(noHead.ExitCode, 1);
	EXPECT_TRUE((noHead.Errors + noHead.Output).Contains("no HEAD")) << noHead.Errors.CStr() << noHead.Output.CStr();
	EXPECT_EQ(ReadTable(), table);

	// Committed with stale hashes and the GUIDs HEAD has: BUILD003, and --update refuses.
	RunGit({ "init", "-q" });
	CommitAll("Builders with stale hashes");
	const test::SProcessResult stale = RunPremake({ "check-builders" });
	EXPECT_EQ(stale.ExitCode, 1);
	EXPECT_TRUE(stale.Output.Contains("BUILD003 Test.Alpha")) << stale.Output.CStr();
	EXPECT_TRUE(stale.Output.Contains("bump the GUID (FRT.bat assets newguid)")) << stale.Output.CStr();
	EXPECT_TRUE((stale.Errors + stale.Output).Contains("2 without a new version GUID (BUILD003)")) << stale.Errors.CStr();
	EXPECT_EQ(RunPremake({ "check-builders", "--update" }).ExitCode, 1);
	EXPECT_EQ(ReadTable(), table);

	// One bumped, one not: the bumped one is told to --update, the other is BUILD003, and --update writes
	// nothing at all.
	WriteTable(MakeTable(AlphaGuid2, ZeroHash, BetaGuid, ZeroHash));
	const FString halfBumped = ReadTable();
	const test::SProcessResult half = RunPremake({ "check-builders" });
	EXPECT_EQ(half.ExitCode, 1);
	EXPECT_FALSE(half.Output.Contains("BUILD003 Test.Alpha")) << half.Output.CStr();
	EXPECT_TRUE(half.Output.Contains("its version GUID is new: record the new hash with FRT.bat check-builders --update")) << half.Output.CStr();
	EXPECT_TRUE(half.Output.Contains("BUILD003 Test.Beta")) << half.Output.CStr();
	const test::SProcessResult refused = RunPremake({ "check-builders", "--update" });
	EXPECT_EQ(refused.ExitCode, 1);
	EXPECT_TRUE((refused.Errors + refused.Output).Contains("nothing written")) << refused.Errors.CStr() << refused.Output.CStr();
	EXPECT_EQ(ReadTable(), halfBumped);

	// Both bumped: --update records the hashes, in place, and the check passes.
	WriteTable(MakeTable(AlphaGuid2, ZeroHash, BetaGuid2, ZeroHash));
	const test::SProcessResult update = RunPremake({ "check-builders", "--update" });
	ASSERT_EQ(update.ExitCode, 0) << update.Errors.CStr() << update.Output.CStr();
	EXPECT_EQ(ReadTable(), MakeTable(AlphaGuid2, AlphaHash(), BetaGuid2, BetaHash()));
	const test::SProcessResult clean = RunPremake({ "check-builders" });
	EXPECT_EQ(clean.ExitCode, 0) << clean.Errors.CStr() << clean.Output.CStr();
	EXPECT_TRUE(clean.Output.Contains("Builders: 2 checked")) << clean.Output.CStr();
	CommitAll("Bump both builders");

	// A source checked out with CRLF is no change; an edit is, for its builder alone.
	Write("Core/Source/Beta/Top.h", "#pragma once // beta\r\n");
	EXPECT_EQ(RunPremake({ "check-builders" }).ExitCode, 0);
	Write("Core/Source/Beta/Top.h", "#pragma once // beta, edited\n");
	const test::SProcessResult edited = RunPremake({ "check-builders" });
	EXPECT_EQ(edited.ExitCode, 1);
	EXPECT_TRUE(edited.Output.Contains("BUILD003 Test.Beta")) << edited.Output.CStr();
	EXPECT_FALSE(edited.Output.Contains("Test.Alpha")) << edited.Output.CStr();
	EXPECT_TRUE((edited.Errors + edited.Output).Contains("1 of 2 builder(s) changed")) << edited.Errors.CStr();
}
