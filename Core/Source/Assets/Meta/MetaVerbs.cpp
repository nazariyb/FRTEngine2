#include "Assets/Meta/MetaVerbs.h"

#include <algorithm>
#include <map>
#include <optional>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/Meta/AssetCommands.h"
#include "Assets/Meta/AssetWriter.h"
#include "Assets/Meta/Lifecycle.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Meta/Migrations.h"
#include "Assets/Registry/RegistryView.h"
#include "Platform/FileSystem.h"


namespace frt::assets
{
namespace
{
constexpr FStringLiteral MetasStep = "metas";

[[nodiscard]] SDiagnostic MakeProblem (const CMountTable& Mounts, FStringView Subject, FString Message)
{
	SDiagnostic problem;
	problem.Severity = ESeverity::Error;
	problem.Subject = FString(Subject);
	FStringView mountName;
	FStringView relative;
	if (const SMount* mount = SplitMountPath(Subject, mountName, relative) ? Mounts.Find(mountName) : nullptr; mount != nullptr && !relative.IsEmpty())
	{
		problem.File = mount->GetSourceFile(relative);
	}
	problem.Message = std::move(Message);
	return problem;
}

// The FILES row of a source, as spelled on disk; nullopt for none (or for a meta's own row).
[[nodiscard]] std::optional<SFileData> FindSource (const CRegistryView& View, FStringView MountPath)
{
	const std::optional<uint32> row = View.FindFile(MountPath);
	SFileData file;
	if (!row.has_value() || !View.GetFile(*row, file) || file.Class == EFileClass::Meta)
	{
		return std::nullopt;
	}
	return file;
}

// The meta of a source on disk, as the registry saw it: its own Meta row, in any spelling.
[[nodiscard]] std::optional<FString> FindMetaOf (const CRegistryView& View, FStringView SourcePath)
{
	const std::optional<uint32> row = View.FindFile(GetMetaPath(SourcePath));
	SFileData file;
	if (!row.has_value() || !View.GetFile(*row, file) || file.Class != EFileClass::Meta)
	{
		return std::nullopt;
	}
	return FString(file.MountPath);
}

[[nodiscard]] bool HasMeta (const CRegistryView& View, FStringView SourcePath)
{
	const std::optional<SFileData> file = FindSource(View, SourcePath);
	return file.has_value() && (file->MetaState == EMetaState::Paired || file->MetaState == EMetaState::Broken);
}

[[nodiscard]] bool IsFoldEqual (FStringView A, FStringView B)
{
	return A.Equals<ESearchCase::IgnoreCase>(B);
}

[[nodiscard]] std::optional<FString> ReadStable (const std::filesystem::path& Path)
{
	FString bytes;
	const platform::EStableRead read = platform::ReadFileChunksStable(Path, [&bytes] (std::span<const uint8> Chunk)
	{
		bytes += FStringView(reinterpret_cast<const char*>(Chunk.data()), static_cast<uint32>(Chunk.size()));
	});
	return read == platform::EStableRead::Stable ? std::optional<FString>(std::move(bytes)) : std::nullopt;
}
}


// ---------------- new-meta ----------------

SNewMetasResult NewMetas (CAssetRegistry& Registry, std::span<const FString> MountPaths)
{
	SNewMetasResult out;
	const CMountTable& mounts = Registry.GetMounts();
	CAssetRepairs* repairs = Registry.GetRepairs();
	std::vector<FString> sources;
	{
		const CRegistryView& view = Registry.Live();
		for (const FString& path : MountPaths)
		{
			const auto refuse = [&] (FString Message) { out.Problems.push_back(MakeProblem(mounts, path, std::move(Message))); };
			const std::optional<uint32> row = view.FindFile(path);
			SFileData file;
			if (!row.has_value() || !view.GetFile(*row, file))
			{
				refuse(FString::Format("'{}' is no file of the registry: none is there, or the rules exclude it", ToDisplayText(path)));
				continue;
			}
			if (file.Class == EFileClass::Meta)
			{
				refuse(FString::Format("'{}' is a meta: name its source", ToDisplayText(file.MountPath)));
				continue;
			}
			if (!GetsMeta(file.Class))
			{
				refuse(FString::Format("'{}' is a {} file, which takes no meta", ToDisplayText(file.MountPath), enum_::ToString(file.Class)));
				continue;
			}
			if (const std::optional<FString> meta = FindMetaOf(view, file.MountPath); meta.has_value())
			{
				refuse(FString::Format("'{}' has a meta already: '{}'", ToDisplayText(file.MountPath), ToDisplayText(*meta)));
				continue;
			}
			if (repairs == nullptr || repairs->GetWriterOf(file.Mount) == nullptr)
			{
				refuse(FString::Format("'{}' is in a mount this process does not write", ToDisplayText(file.MountPath)));
				continue;
			}
			if (std::none_of(sources.begin(), sources.end(), [&] (const FString& Source) { return Source.View() == file.MountPath; }))
			{
				sources.emplace_back(file.MountPath);
			}
		}
	}
	if (!out.Problems.empty())
	{
		out.bRefused = true;
		return out;
	}

	SRepairOptions options;
	options.Paths = sources;
	options.Kinds = { ELifecycleAction::CreateMeta };
	SRepairReport report = repairs->RunNow(options);

	// What the lifecycle would rather do with a file (pair it with an orphan meta, restore a lost one) is not what was asked
	// for: its problem's "create" fix-it is.
	for (const FString& source : sources)
	{
		if (HasMeta(Registry.Live(), source))
		{
			continue;
		}
		for (const SDiagnostic& problem : report.Problems)
		{
			if (!IsFoldEqual(problem.Subject, source))
			{
				continue;
			}
			const std::vector<SLifecycleAction> fixIts = repairs->GetFixIts(problem);
			const auto create = std::find_if(fixIts.begin(), fixIts.end(), [] (const SLifecycleAction& Action) { return Action.Kind == ELifecycleAction::CreateMeta; });
			if (create != fixIts.end())
			{
				SRepairReport fixed = repairs->ApplyFix(*create);
				report.Problems.insert(report.Problems.end(), fixed.Problems.begin(), fixed.Problems.end());
				break;
			}
		}
	}

	const CRegistryView& view = Registry.Live();
	for (const FString& source : sources)
	{
		if (HasMeta(view, source))
		{
			out.Created.push_back(source);
			continue;
		}
		bool bExplained = false;
		for (const SDiagnostic& problem : report.Problems)
		{
			if (IsFoldEqual(problem.Subject, source) && problem.Severity == ESeverity::Error)
			{
				out.Problems.push_back(problem);
				bExplained = true;
			}
		}
		if (!bExplained)
		{
			out.Problems.push_back(MakeProblem(mounts, source, FString::Format("no meta could be created for '{}'", ToDisplayText(source))));
		}
	}
	return out;
}


// ---------------- migrate metas ----------------

SMigrateMetasResult MigrateMetas (CAssetRegistry& Registry, std::span<const MountId> Mounts, const std::filesystem::path& Root,
	IVcsProvider* Provider, bool bDryRun)
{
	SMigrateMetasResult out;
	SMigrationsFile file;
	if (ReadMigrations(Root, file))
	{
		out.bWasRecorded = std::any_of(file.Applied.begin(), file.Applied.end(), [] (const FString& Name) { return Name.View().Equals<ESearchCase::IgnoreCaseAscii>(MetasStep); });
	}
	CAssetRepairs* repairs = Registry.GetRepairs();
	if (repairs == nullptr)
	{
		out.Error = "this registry has no lifecycle";
		return out;
	}
	SRepairOptions options;
	for (const MountId mount : Mounts)
	{
		if (const SMount* found = Registry.GetMounts().Find(mount); found != nullptr)
		{
			options.Paths.push_back(found->GetRootMountPath());
		}
	}
	options.Kinds = { ELifecycleAction::CreateMeta };
	options.bDryRun = bDryRun;
	out.Report = repairs->RunNow(options);
	if (bDryRun)
	{
		// A reader (a dry run with --ci) plans nothing, since it writes nowhere: what it would create are its META001s' fix-its,
		// derived alike. A source or meta with the read-only attribute is left out, as the writer's run would leave it (the
		// lifecycle makes no repair of it, and its fix-it ends in VCS002): the preview stays the run's.
		const auto isReadOnly = [&Registry] (FStringView MountPath)
		{
			const std::optional<std::filesystem::path> path = Registry.GetMounts().FromMountPath(MountPath);
			const std::optional<platform::SFileStat> stat = path.has_value() ? platform::Stat(*path) : std::nullopt;
			return stat.has_value() && stat->bReadOnly;
		};
		for (const SDiagnostic& problem : out.Report.Problems)
		{
			if (problem.Code != Meta001)
			{
				continue;
			}
			for (SLifecycleAction& fixIt : repairs->GetFixIts(problem))
			{
				const bool bKnown = std::any_of(out.Report.Planned.begin(), out.Report.Planned.end(),
					[&] (const SLifecycleAction& Action) { return Action.Kind == fixIt.Kind && Action.Path == fixIt.Path; });
				if (fixIt.Kind == ELifecycleAction::CreateMeta && !bKnown && !isReadOnly(fixIt.Path) && !isReadOnly(GetMetaPath(fixIt.Path)))
				{
					out.Report.Planned.push_back(std::move(fixIt));
				}
			}
		}
	}

	const CRegistryView& view = Registry.Live();
	view.ForEachFile([&] (uint32 Row)
	{
		SFileData data;
		if (!view.GetFile(Row, data) || !GetsMeta(data.Class) || std::find(Mounts.begin(), Mounts.end(), data.Mount) == Mounts.end()
			|| data.MetaState == EMetaState::Paired || data.MetaState == EMetaState::Broken)
		{
			return;
		}
		const bool bPlanned = bDryRun && std::any_of(out.Report.Planned.begin(), out.Report.Planned.end(), [&] (const SLifecycleAction& Action)
		{
			return Action.Kind == ELifecycleAction::CreateMeta && Action.Path.View() == data.MountPath;
		});
		if (!bPlanned)
		{
			out.Remaining.emplace_back(data.MountPath);
		}
	});
	std::sort(out.Remaining.begin(), out.Remaining.end(), [] (const FString& A, const FString& B) { return LessPortableThenBytes(A, B); });

	if (bDryRun)
	{
		out.bRecorded = out.bWasRecorded;
		return out;
	}
	if (out.Remaining.empty())
	{
		out.bRecorded = RecordMigration(Root, MetasStep, Provider, &out.Error);
	}
	return out;
}


// ---------------- resave --metas ----------------

std::vector<SAssetOp> PlanResaveMetas (const CAssetWriter& Writer, const CRegistryView& View, std::span<const MountId> Mounts, SResaveMetasResult& Out)
{
	struct SRewrite
	{
		FString Path;
		FString Text;
		hash::SContentHash Read;   // the bytes the rewrite replaces: the writer writes only while the meta still holds them
	};
	std::vector<SRewrite> rewrites;
	const CMountTable* mounts = Writer.GetMounts();
	View.ForEachFile([&] (uint32 Row)
	{
		SFileData file;
		if (mounts == nullptr || !View.GetFile(Row, file) || file.Class != EFileClass::Meta
			|| std::find(Mounts.begin(), Mounts.end(), file.Mount) == Mounts.end() || Writer.FindProvider(file.MountPath) == nullptr)
		{
			return;
		}
		const std::optional<std::filesystem::path> path = mounts->FromMountPath(file.MountPath);
		const std::optional<FString> bytes = path.has_value() ? ReadStable(*path) : std::nullopt;
		if (!bytes.has_value())
		{
			return;
		}
		SMetaParseResult parsed;
		ParseMeta(*bytes, parsed);
		if (parsed.State != EMetaParseState::Ok)
		{
			Out.Broken.emplace_back(file.MountPath);
			return;
		}
		if (parsed.bNewerFormat)
		{
			Out.Newer.emplace_back(file.MountPath);   // a newer engine's layout: this build would rewrite it in its own
			return;
		}
		FString text;
		if (parsed.bCanonical || !EmitMeta(parsed.Meta, text) || text == *bytes)
		{
			++Out.Canonical;
			return;
		}
		rewrites.push_back(SRewrite { FString(file.MountPath), std::move(text), hash::HashContent(bytes->View().AsBytes()) });
	});
	std::sort(rewrites.begin(), rewrites.end(), [] (const SRewrite& A, const SRewrite& B) { return LessPortableThenBytes(A.Path, B.Path); });
	std::sort(Out.Broken.begin(), Out.Broken.end(), [] (const FString& A, const FString& B) { return LessPortableThenBytes(A, B); });
	std::sort(Out.Newer.begin(), Out.Newer.end(), [] (const FString& A, const FString& B) { return LessPortableThenBytes(A, B); });

	std::vector<SAssetOp> ops;
	for (SRewrite& rewrite : rewrites)
	{
		if (ops.empty() || ops.back().Steps.size() >= MaxResaveSteps)
		{
			SAssetOp& op = ops.emplace_back();
			op.Id = SGuid::NewRandom();
			op.Verb = "resave-metas";
		}
		SAssetOpStep step;
		step.Kind = EAssetOpStep::WriteFile;
		step.Path = rewrite.Path;
		step.Bytes.assign(rewrite.Text.View().AsBytes().begin(), rewrite.Text.View().AsBytes().end());
		step.Prior = EAssetOpPrior::Holds;
		step.PriorHash = rewrite.Read;
		ops.back().Steps.push_back(std::move(step));
		Out.Rewritten.push_back(std::move(rewrite.Path));
	}
	return ops;
}

SResaveMetasResult ResaveMetas (CAssetRegistry& Registry, std::span<const MountId> Mounts, bool bDryRun)
{
	SResaveMetasResult out;
	CAssetRepairs* repairs = Registry.GetRepairs();
	if (repairs == nullptr)
	{
		return out;
	}
	// Each writer once, with the mounts it writes (/Project and its plugins share the owner root's).
	std::map<CAssetWriter*, std::vector<MountId>> byWriter;
	for (const MountId mount : Mounts)
	{
		if (CAssetWriter* writer = repairs->GetWriterOf(mount); writer != nullptr)
		{
			byWriter[writer].push_back(mount);
		}
	}
	std::vector<FString> touched;
	for (const auto& [writer, mounts] : byWriter)
	{
		SResaveMetasResult planned;
		const std::vector<SAssetOp> ops = PlanResaveMetas(*writer, Registry.Live(), mounts, planned);
		out.Canonical += planned.Canonical;
		out.Broken.insert(out.Broken.end(), planned.Broken.begin(), planned.Broken.end());
		out.Newer.insert(out.Newer.end(), planned.Newer.begin(), planned.Newer.end());
		if (bDryRun)
		{
			out.Rewritten.insert(out.Rewritten.end(), planned.Rewritten.begin(), planned.Rewritten.end());
			continue;
		}
		for (const SAssetOp& op : ops)
		{
			const SAssetCommandResult result = RunAssetOp(*writer, op);
			if (!result.IsOk())
			{
				// Nothing of it was written, or what was is the journal's: the next run of the writer finishes it.
				out.Problems.insert(out.Problems.end(), result.Problems.begin(), result.Problems.end());
				continue;
			}
			for (const SAssetOpStep& step : op.Steps)
			{
				out.Rewritten.push_back(step.Path);
				touched.push_back(step.Path);
			}
		}
	}
	if (!touched.empty())
	{
		(void)Registry.ScanPathsSynchronous(touched);
	}
	return out;
}
}
