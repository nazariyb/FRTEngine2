// The registry's golden dumps (design §5.3, §16.2 M3: "golden registry text dump of both templates"): a cold scan of
// the engine's content and of each committed template dumps exactly what Core-Test/Assets/Golden/Registry-*.txt
// holds - the engine's /Engine, and each template's /Project - and the engine and the templates are clean of the
// portable-path and rule problems (§1.1 G11: no PATH001-PATH003, no RULE001 or RULE002, no error at all). The content
// is copied into a temp tree in the checkout's own layout first (the runtime may write Content/), and FRT.bat verify
// compares the same files with `FRT.bat assets dump registry --text --ci` run in place. A content change that changes
// a dump is committed with its golden file: `FRT.bat verify --update-goldens` rewrites them and shows the diff.
// Since M4 the registry runs its lifecycle as every host's does (reporting only), so a committed file without its meta
// is a META001 error here; and the Demo copied into a case-sensitive directory dumps the same text (§16.2 M4): the
// identities its committed metas give do not depend on how the file system compares names.
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "TemplateTestUtils.h"

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Containers/String.h"
#include "Diagnostics/Diagnostics.h"
#include "EnginePaths.h"
#include "Jobs/WorkerPool.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"

using namespace frt;
using namespace frt::assets;
using frt::test::registry::CCheckoutCopy;


namespace
{
std::filesystem::path GetGoldenPath (FStringView Name)
{
	return paths::GetEngineDir() / "Core-Test" / "Assets" / "Golden" / ("Registry-" + Name + ".txt").ToPath();
}

// The text with every CRLF made LF: a checkout with core.autocrlf may hold either.
FString NormalizeLineEndings (FStringView Text)
{
	FString out;

	for (uint32 i = 0u; i < Text.Len(); ++i)
	{
		if (Text[i] == '\r' && i + 1u < Text.Len() && Text[i + 1u] == '\n')
		{
			continue;
		}
		out += Text[i];
	}
	return out;
}

// "line <n>: <golden line> | <dumped line>" for the first line that differs.
FString DescribeFirstDifference (FStringView Golden, FStringView Dumped)
{
	std::vector<FStringView> a;
	std::vector<FStringView> b;
	for (const FStringView line : Golden.Lines())
	{
		a.push_back(line);
	}
	for (const FStringView line : Dumped.Lines())
	{
		b.push_back(line);
	}
	for (size_t i = 0u; i < a.size() || i < b.size(); ++i)
	{
		const FStringView left = i < a.size() ? a[i] : FStringView("(end)");
		const FStringView right = i < b.size() ? b[i] : FStringView("(end)");
		if (left != right)
		{
			return FString::Format("line {}: golden '{}', dumped '{}'", i + 1u, left, right);
		}
	}
	return FString("no line differs (line endings?)");
}

/**
 * A cold scan of Copy by a reader (no snapshot, writes nothing), the mounts named dumped, compared with the golden file
 * Name; the dump also passes G11. On a difference the dump is written beside the user data directory's temp files for
 * a look, and the message says where and which line differs first.
 */
void ExpectGoldenDump (const CCheckoutCopy& Copy, FStringView Name, std::span<const FStringView> MountNames)
{
	ASSERT_TRUE(Copy.MountDiagnostics.empty()) << Copy.MountDiagnostics.front().Message.CStr();
	// The mounts first: an ASSERT returns, and must never leave a started registry behind.
	std::vector<MountId> mounts;
	for (const FStringView mountName : MountNames)
	{
		const SMount* mount = Copy.Mounts->Find(mountName);
		ASSERT_NE(mount, nullptr) << FString(mountName).CStr();
		mounts.push_back(mount->Id);
	}
	jobs::SWorkerPoolDesc poolDesc;
	poolDesc.ThreadCount = 4u;
	poolDesc.Name = "Registry Golden Test";
	jobs::CWorkerPool pool(poolDesc);
	// As AssetTool's dump runs it: strictly in sequence order (the dump does not depend on it).
	pool.SetCommitOrder(jobs::ECommitOrder::Sequence);
	SAssetRegistryDesc desc;
	desc.Mounts = Copy.Mounts;
	desc.Pool = &pool;
	desc.Owner = MountNames.size() == 1u && MountNames[0] == "Engine" && Copy.Mounts->Find("Project") == nullptr ? EAssetRegistryOwner::Engine
		: EAssetRegistryOwner::Project;
	desc.bLoadSnapshots = false;
	// The lifecycle, reporting only, with no provider (None), as AssetTool's dump has it.
	const SLifecycleDesc lifecycle;
	desc.Lifecycle = &lifecycle;
	CAssetRegistry registry(std::move(desc));
	// Shut down on every way out, a failed ASSERT included: ~CAssetRegistry asserts (a breakpoint that ends Core-Test
	// without a debugger) on a registry still running, whose scan tasks would outlive it on the pool.
	struct SShutDownOnExit
	{
		CAssetRegistry& Registry;
		~SShutDownOnExit () { Registry.ShutDown(); }
	} shutDownOnExit { registry };
	registry.Start();
	ASSERT_TRUE(registry.WaitForInitialScan(300.0)) << "the initial scan of " << FString(Name).CStr() << " did not end";
	SRepairOptions report;
	for (const SMount& mount : Copy.Mounts->GetMounts())
	{
		report.Paths.push_back(mount.GetRootMountPath());
	}
	(void)registry.GetRepairs()->RunNow(report);

	FString dump;
	registry.Live().DumpText(dump, EDumpFlags::None, mounts);

	// G11: the engine and the templates are clean of portable-path and rule problems, and of errors.
	registry.Live().ForEachProblem([&] (FStringView Subject, EProblemSource, const SDiagnostic& Diagnostic)
	{
		const bool bPortability = Diagnostic.Code == Path001 || Diagnostic.Code == Path002 || Diagnostic.Code == Path003;
		const bool bRules = Diagnostic.Code == Rule001 || Diagnostic.Code == Rule002;
		EXPECT_FALSE(bPortability || bRules || Diagnostic.Severity == ESeverity::Error) << FString(Subject).CStr() << ": "
			<< FormatDiagnosticCode(Diagnostic.Code).CStr() << " " << Diagnostic.Message.CStr();
	});
	registry.ShutDown();

	const std::filesystem::path goldenPath = GetGoldenPath(Name);
	const FString golden = NormalizeLineEndings(platform::ReadFileText(goldenPath).value_or(FString()));
	ASSERT_FALSE(golden.IsEmpty()) << "no golden file " << FString::FromPath(goldenPath).CStr() << ": FRT.bat verify --update-goldens writes it";
	if (golden != dump)
	{
		const std::filesystem::path actual = std::filesystem::temp_directory_path()
			/ FString::Format("FRT Registry-{}.dumped.{}.txt", Name, platform::GetCurrentProcessId()).ToPath();
		(void)platform::WriteFileText(actual, dump);
		ADD_FAILURE() << "the registry dump differs from " << FString::FromPath(goldenPath).CStr() << " (" << DescribeFirstDifference(golden, dump).CStr()
			<< "); the dump is in " << FString::FromPath(actual).CStr() << ". Content changed on purpose: FRT.bat verify --update-goldens";
	}
}
}


TEST(RegistryGoldenTest, TheEnginesDumpMatchesItsGoldenFile)
{
	const CCheckoutCopy copy("Golden Engine");
	ASSERT_FALSE(HasFailure());
	const FStringView mounts[] = { "Engine" };
	ExpectGoldenDump(copy, "Engine", mounts);
}

TEST(RegistryGoldenTest, TheDemoTemplatesDumpMatchesItsGoldenFile)
{
	const CCheckoutCopy copy("Golden Demo", "Demo");
	ASSERT_FALSE(HasFailure());
	const FStringView mounts[] = { "Project" };
	ExpectGoldenDump(copy, "Demo", mounts);
}

TEST(RegistryGoldenTest, TheBlankTemplatesDumpMatchesItsGoldenFile)
{
	const CCheckoutCopy copy("Golden Blank", "Blank");
	ASSERT_FALSE(HasFailure());
	const FStringView mounts[] = { "Project" };
	ExpectGoldenDump(copy, "Blank", mounts);
}

// A clone into a case-sensitive directory (WSL's, or one a developer set up for Linux tools) scans to the same dump: a
// per-directory attribute on a temp directory, not a system setting; skipped where it cannot be set.
TEST(RegistryGoldenTest, TheDemoTemplateInACaseSensitiveDirectoryDumpsTheSameText)
{
	const CCheckoutCopy copy("Golden Demo Case", "Demo", true);
	if (!copy.bIsCaseSensitive)
	{
		GTEST_SKIP() << "this volume cannot make a directory case-sensitive (NTFS, and on some Windows versions WSL, is needed)";
	}
	ASSERT_FALSE(HasFailure());
	const SMount* project = copy.Mounts->Find("Project");
	ASSERT_NE(project, nullptr);
	EXPECT_TRUE(project->Volume.bCaseSensitive) << "the copy's mount reads as case-sensitive";
	const FStringView mounts[] = { "Project" };
	ExpectGoldenDump(copy, "Demo", mounts);
}
