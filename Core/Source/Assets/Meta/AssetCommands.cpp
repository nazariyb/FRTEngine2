#include "Assets/Meta/AssetCommands.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <utility>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/Discovery/MountScanner.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Meta/MetaPaths.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"


namespace frt::assets
{
namespace
{
struct SResolved
{
	const SMount* Mount = nullptr;
	std::filesystem::path Absolute;
	FString Relative;
};

// MountPath in a mount of the writer's owner root.
[[nodiscard]] std::optional<SResolved> ResolveIn (const CAssetWriter& Writer, FStringView MountPath)
{
	const CMountTable* mounts = Writer.GetMounts();
	FStringView mountName;
	FStringView relative;
	if (mounts == nullptr || !IsWellFormedMountPath(MountPath) || !SplitMountPath(MountPath, mountName, relative) || relative.IsEmpty()
		|| Writer.FindProvider(MountPath) == nullptr)
	{
		return std::nullopt;
	}
	SResolved resolved;
	resolved.Mount = mounts->Find(mountName);
	const std::optional<std::filesystem::path> absolute = mounts->FromMountPath(MountPath);
	if (resolved.Mount == nullptr || !absolute.has_value())
	{
		return std::nullopt;
	}
	resolved.Absolute = *absolute;
	resolved.Relative = FString(relative);
	return resolved;
}

[[nodiscard]] SDiagnostic MakeProblem (const CAssetWriter& Writer, ESeverity Severity, uint32 Code, FStringView Subject, FString Message)
{
	SDiagnostic problem;
	problem.Severity = Severity;
	problem.Code = Code;
	problem.Subject = FString(Subject);
	if (const std::optional<SResolved> resolved = ResolveIn(Writer, Subject))
	{
		problem.File = resolved->Mount->GetSourceFile(resolved->Relative);
	}
	problem.Message = std::move(Message);
	return problem;
}

[[nodiscard]] std::optional<platform::SFileStat> StatFile (const std::filesystem::path& Path)
{
	std::optional<platform::SFileStat> stat = platform::Stat(Path);
	return stat.has_value() && !stat->bDirectory ? stat : std::nullopt;
}

[[nodiscard]] FString JoinChild (FStringView FolderMountPath, FStringView Name)
{
	FString path(FolderMountPath);
	path += '/';
	path += Name;
	return path;
}

/**
 * The folders one plan looks into, each listed once and kept by folded name: a folder move of N assets lists each folder
 * once, not twice per asset. Nothing changes on disk while a plan is made, so a listing stays true for the whole plan.
 */
class CFolderListings
{
public:
	struct SEntry
	{
		FString Name;
		platform::SFileIdentity Identity;
	};
	using FEntries = std::unordered_multimap<FString, SEntry>;   // folded name -> the files so named (directories left out)

	[[nodiscard]] const FEntries& Get (const std::filesystem::path& Folder)
	{
		const FString key = FString::FromPath(Folder);
		auto found = Folders.find(key);
		if (found == Folders.end())
		{
			FEntries entries;
			platform::EnumerateDirectory(Folder, [&entries] (const platform::SDirectoryEntry& Entry)
			{
				if (!Entry.Stat.bDirectory)
				{
					entries.emplace(FString(Entry.Name).FoldCase(), SEntry { FString(Entry.Name), Entry.Stat.Identity });
				}
			});
			found = Folders.emplace(key, std::move(entries)).first;
		}
		return found->second;
	}

private:
	std::unordered_map<FString, FEntries> Folders;
};

/**
 * The file named Leaf in Folder as the folder spells it: Leaf byte-exact first, else one equal under the portable fold
 * (the first in byte order when a case-sensitive folder holds several).
 */
[[nodiscard]] std::optional<FString> FindSpelling (CFolderListings& Listings, const std::filesystem::path& Folder, FStringView Leaf)
{
	const auto [begin, end] = Listings.Get(Folder).equal_range(FString(Leaf).FoldCase());
	std::optional<FString> found;
	for (auto entry = begin; entry != end; ++entry)
	{
		if (entry->second.Name == Leaf)
		{
			return entry->second.Name;
		}
		if (!found.has_value() || std::ranges::lexicographical_compare(entry->second.Name.View().AsBytes(), found->View().AsBytes()))
		{
			found = entry->second.Name;
		}
	}
	return found;
}

/**
 * The meta of Source as the folder spells it: "Rock.png.meta" byte-exact first, else one equal under the portable fold
 * ("Rock.PNG.meta", META006), which a move or delete must carry along all the same. A case-sensitive folder can hold
 * both; the exact one is the pair then.
 */
[[nodiscard]] std::optional<FString> FindMetaSpelling (CFolderListings& Listings, FStringView SourceMountPath, const std::filesystem::path& SourceAbsolute)
{
	const std::optional<FString> name = FindSpelling(Listings, SourceAbsolute.parent_path(), GetMetaPath(GetMountPathLeaf(SourceMountPath)));
	if (!name.has_value())
	{
		return std::nullopt;
	}
	return JoinChild(GetMountPathParent(SourceMountPath), *name);
}

/**
 * Why To cannot be a new name for the file whose identity is Self: a segment no supported platform allows (PATH002), or a
 * name in its folder equal to it under the portable fold that is another file (PATH001: Find by a typed name would reach
 * only one). Empty when it can.
 */
[[nodiscard]] std::optional<std::pair<uint32, FString>> GetNewNameProblem (CFolderListings& Listings, const SResolved& To,
	const platform::SFileIdentity& Self)
{
	const FStringView relative = To.Relative.View();
	for (const FStringView segment : relative.Split('/'))
	{
		const FStringView reason = GetNonPortableNameReason(segment);
		if (!reason.IsEmpty())
		{
			return std::pair(Path002, FString::Format("'{}' is not a portable name: {}", ToDisplayText(segment), reason));
		}
	}
	std::optional<FString> collision;
	const auto [begin, end] = Listings.Get(To.Absolute.parent_path()).equal_range(FString(GetMountPathLeaf(relative)).FoldCase());
	for (auto entry = begin; entry != end && !collision.has_value(); ++entry)
	{
		if (!(entry->second.Identity == Self))
		{
			collision = entry->second.Name;
		}
	}
	if (collision.has_value())
	{
		return std::pair(Path001, FString::Format("'{}' is taken in that folder, by '{}', in another case", ToDisplayText(To.Relative),
			ToDisplayText(*collision)));
	}
	return std::nullopt;
}

[[nodiscard]] bool IsCaseOnly (FStringView From, FStringView To)
{
	return From != To && FString(From).FoldCase() == FString(To).FoldCase();
}

[[nodiscard]] SAssetOp MakeOp (FStringView Verb)
{
	SAssetOp op;
	op.Id = SGuid::NewRandom();
	op.Verb = FString(Verb);
	return op;
}

[[nodiscard]] SAssetOpStep MakeStep (EAssetOpStep Kind, FStringView Path, FStringView ToPath = FStringView())
{
	SAssetOpStep step;
	step.Kind = Kind;
	step.Path = FString(Path);
	step.ToPath = FString(ToPath);
	return step;
}

// The asset a problem with Path is about: a meta's source.
[[nodiscard]] FStringView GetSubjectAsset (FStringView Path)
{
	const std::optional<FStringView> source = GetMetaSource(Path);
	return source.has_value() ? *source : Path;
}
}


bool SAssetCommandResult::IsOk () const noexcept
{
	if (Vcs.Error != EVcsError::None)
	{
		return false;
	}
	for (const SDiagnostic& problem : Problems)
	{
		if (problem.Severity == ESeverity::Error)
		{
			return false;
		}
	}
	return true;
}


bool PlanCreateMeta (const CAssetWriter& Writer, const SCreateMetaRequest& Request, SAssetOp& OutOp, std::vector<SDiagnostic>& OutProblems)
{
	OutOp = MakeOp("create-meta");
	const std::optional<SResolved> source = ResolveIn(Writer, Request.Source);
	if (!source.has_value())
	{
		OutProblems.push_back(MakeProblem(Writer, ESeverity::Error, 0u, Request.Source,
			FString::Format("'{}' names no file in a mount this process writes", ToDisplayText(Request.Source))));
		return false;
	}
	if (GetMetaSource(Request.Source).has_value() || !StatFile(source->Absolute).has_value())
	{
		OutProblems.push_back(MakeProblem(Writer, ESeverity::Error, 0u, Request.Source, FString::Format("'{}' is not a file that takes a meta",
			ToDisplayText(FString::FromPath(source->Absolute)))));
		return false;
	}
	CFolderListings listings;
	if (const std::optional<FString> existing = FindMetaSpelling(listings, Request.Source, source->Absolute))
	{
		OutProblems.push_back(MakeProblem(Writer, ESeverity::Error, 0u, Request.Source, FString::Format("'{}' has a meta already: '{}'",
			ToDisplayText(Request.Source), ToDisplayText(*existing))));
		return false;
	}

	SAssetMeta meta = Request.Meta;
	if (!meta.Guid.IsValid())
	{
		meta.Guid = SGuid::NewRandom();
	}
	FString text;
	SDiagnostic error;
	if (!EmitMeta(meta, text, &error))
	{
		OutProblems.push_back(MakeProblem(Writer, ESeverity::Error, 0u, Request.Source, FString::Format("cannot write the meta of '{}': {}",
			ToDisplayText(Request.Source), error.Message)));
		return false;
	}
	SAssetOpStep step = MakeStep(EAssetOpStep::WriteFile, GetMetaPath(Request.Source));
	step.Bytes.assign(text.View().AsBytes().begin(), text.View().AsBytes().end());
	step.Prior = EAssetOpPrior::Absent;   // checked again right before the write: a meta that arrives meanwhile is kept
	OutOp.Steps.push_back(std::move(step));
	OutOp.PlannedGuids.push_back(meta.Guid);
	return true;
}

/**
 * All moves first, in one run of steps, so the writer hands them to the provider in one call: every source with its meta
 * and companions. Then the metas whose FormerPaths changed, written at their new paths.
 */
bool PlanMoveAssets (const CAssetWriter& Writer, std::span<const SAssetMove> Moves, SAssetOp& OutOp, std::vector<SDiagnostic>& OutProblems)
{
	OutOp = MakeOp("move");
	const size_t problemsBefore = OutProblems.size();
	std::vector<SAssetOpStep> rewrites;
	std::vector<FString> targets;   // folded: two moves must not land on one name
	CFolderListings listings;

	const auto refuse = [&] (uint32 Code, FStringView Subject, FString Message)
	{
		OutProblems.push_back(MakeProblem(Writer, ESeverity::Error, Code, Subject, std::move(Message)));
	};
	const auto claimTarget = [&] (FStringView Path)
	{
		const FString folded = FString(Path).FoldCase();
		if (std::find(targets.begin(), targets.end(), folded) != targets.end())
		{
			refuse(0u, Path, FString::Format("two files of this move would land on '{}'", ToDisplayText(Path)));
			return false;
		}
		targets.push_back(folded);
		return true;
	};
	// From to To for a file that is not the asset itself (a meta, a companion): it exists, and To is free.
	const auto planAlong = [&] (FStringView From, FStringView To, FStringView Asset)
	{
		const std::optional<SResolved> from = ResolveIn(Writer, From);
		const std::optional<SResolved> to = ResolveIn(Writer, To);
		const std::optional<platform::SFileStat> fromStat = from.has_value() ? StatFile(from->Absolute) : std::nullopt;
		if (!fromStat.has_value() || !to.has_value())
		{
			refuse(0u, Asset, FString::Format("cannot move '{}' to '{}' along with '{}'", ToDisplayText(From), ToDisplayText(To), ToDisplayText(Asset)));
			return false;
		}
		const std::optional<platform::SFileStat> toStat = StatFile(to->Absolute);
		if (toStat.has_value() && !(toStat->Identity == fromStat->Identity))
		{
			refuse(0u, Asset, FString::Format("cannot move '{}': '{}' exists", ToDisplayText(From), ToDisplayText(To)));
			return false;
		}
		if (!claimTarget(To))
		{
			return false;
		}
		if (From != To)
		{
			OutOp.Steps.push_back(MakeStep(EAssetOpStep::Move, From, To));
		}
		return true;
	};

	for (const SAssetMove& move : Moves)
	{
		const std::optional<SResolved> from = ResolveIn(Writer, move.From);
		const std::optional<SResolved> to = ResolveIn(Writer, move.To);
		const std::optional<platform::SFileStat> fromStat = from.has_value() ? StatFile(from->Absolute) : std::nullopt;
		if (!fromStat.has_value() || GetMetaSource(move.From).has_value())
		{
			refuse(0u, move.From, FString::Format("'{}' is not an asset file in a mount this process writes", ToDisplayText(move.From)));
			continue;
		}
		if (!to.has_value() || GetMetaSource(move.To).has_value())
		{
			refuse(0u, move.From, FString::Format("'{}' is not a path in a mount this process writes", ToDisplayText(move.To)));
			continue;
		}
		if (move.From == move.To)
		{
			continue;
		}
		const std::optional<platform::SFileStat> toStat = StatFile(to->Absolute);
		if (toStat.has_value() && !(toStat->Identity == fromStat->Identity))
		{
			refuse(0u, move.From, FString::Format("cannot move '{}': '{}' exists", ToDisplayText(move.From), ToDisplayText(move.To)));
			continue;
		}
		if (const std::optional<std::pair<uint32, FString>> nameProblem = GetNewNameProblem(listings, *to, fromStat->Identity))
		{
			refuse(nameProblem->first, move.To, nameProblem->second);
			continue;
		}
		if (!claimTarget(move.To))
		{
			continue;
		}
		OutOp.Steps.push_back(MakeStep(EAssetOpStep::Move, move.From, move.To));

		const bool bCaseOnly = IsCaseOnly(move.From, move.To);
		if (const std::optional<FString> meta = FindMetaSpelling(listings, move.From, from->Absolute))
		{
			const FString metaTo = GetMetaPath(move.To);
			if (!planAlong(*meta, metaTo, move.From))
			{
				continue;
			}
			const std::optional<SResolved> metaFrom = ResolveIn(Writer, *meta);
			const std::optional<std::vector<uint8>> bytes = metaFrom.has_value() ? platform::ReadFileBytes(metaFrom->Absolute) : std::nullopt;
			SMetaParseResult parsed;
			if (bytes.has_value())
			{
				ParseMeta(FStringView(reinterpret_cast<const char*>(bytes->data()), bytes->size()), parsed);
			}
			if (!bytes.has_value() || parsed.State != EMetaParseState::Ok)
			{
				// Never rewritten: a broken meta keeps every byte, and moves as it is.
				OutOp.PlannedGuids.insert(OutOp.PlannedGuids.end(), parsed.SalvagedGuids.begin(), parsed.SalvagedGuids.end());
				OutProblems.push_back(MakeProblem(Writer, ESeverity::Warning, 0u, *meta,
					FString::Format("'{}' cannot be read, so it moves unchanged and does not record '{}' as a former path", ToDisplayText(*meta),
						ToDisplayText(move.From))));
			}
			else if (parsed.bNewerFormat)
			{
				// Nor is a newer engine's meta: this build would write it in its own, older layout.
				OutOp.PlannedGuids.push_back(parsed.Meta.Guid);
				OutProblems.push_back(MakeProblem(Writer, ESeverity::Warning, 0u, *meta,
					FString::Format("'{}' was written by a newer engine (format version {}), so it moves unchanged and does not record '{}' as a "
						"former path", ToDisplayText(*meta), parsed.Meta.FormatVersion, ToDisplayText(move.From))));
			}
			else
			{
				OutOp.PlannedGuids.push_back(parsed.Meta.Guid);
				SAssetMeta updated = parsed.Meta;
				if (!bCaseOnly)
				{
					const FString toFolded = FString(move.To).FoldCase();
					const FString fromFolded = FString(move.From).FoldCase();
					std::erase_if(updated.FormerPaths, [&toFolded] (const FString& Path) { return Path.FoldCase() == toFolded; });
					const bool bKnown = std::any_of(updated.FormerPaths.begin(), updated.FormerPaths.end(),
						[&fromFolded] (const FString& Path) { return Path.FoldCase() == fromFolded; });
					if (!bKnown)
					{
						updated.FormerPaths.push_back(move.From);
					}
				}
				FString text;
				SDiagnostic error;
				if (!EmitMeta(updated, text, &error))
				{
					refuse(0u, move.From, FString::Format("cannot write the meta of '{}': {}", ToDisplayText(move.To), error.Message));
					continue;
				}
				const std::span<const uint8> newBytes = text.View().AsBytes();
				if (!std::equal(newBytes.begin(), newBytes.end(), bytes->begin(), bytes->end()))
				{
					// The meta is rewritten where the Move puts it, from the bytes read here: the writer checks it still holds them.
					SAssetOpStep rewrite = MakeStep(EAssetOpStep::WriteFile, metaTo);
					rewrite.Bytes.assign(newBytes.begin(), newBytes.end());
					rewrite.Prior = EAssetOpPrior::Holds;
					rewrite.PriorHash = hash::HashContent(*bytes);
					rewrites.push_back(std::move(rewrite));
				}
			}
		}
		else if (const std::optional<SResolved> metaTo = ResolveIn(Writer, GetMetaPath(move.To)); metaTo.has_value()
			&& StatFile(metaTo->Absolute).has_value())
		{
			refuse(0u, move.From, FString::Format("cannot move '{}': '{}' would pair with another file's meta", ToDisplayText(move.From),
				ToDisplayText(move.To)));
			continue;
		}

		for (const SVcsMove& companion : move.Companions)
		{
			planAlong(companion.From, companion.To, move.From);
		}
	}

	if (OutProblems.size() > problemsBefore && std::any_of(OutProblems.begin() + static_cast<ptrdiff_t>(problemsBefore), OutProblems.end(),
		[] (const SDiagnostic& Problem) { return Problem.Severity == ESeverity::Error; }))
	{
		OutOp.Steps.clear();
		OutOp.PlannedGuids.clear();
		return false;
	}
	for (SAssetOpStep& rewrite : rewrites)
	{
		OutOp.Steps.push_back(std::move(rewrite));
	}
	return true;
}

/**
 * Each source before its meta: a crash between the two leaves an orphan meta, which is reported, and never a metaless
 * source, which the lifecycle would give a new identity. The provider gets them in one call all the same.
 */
bool PlanDeleteAssets (const CAssetWriter& Writer, std::span<const SAssetDelete> Deletes, SAssetOp& OutOp, std::vector<SDiagnostic>& OutProblems)
{
	OutOp = MakeOp("delete");
	bool bOk = true;
	std::vector<FString> planned;
	CFolderListings listings;
	const auto add = [&] (FStringView Path)
	{
		if (std::find(planned.begin(), planned.end(), Path) == planned.end())
		{
			planned.emplace_back(Path);
			OutOp.Steps.push_back(MakeStep(EAssetOpStep::Trash, Path));
		}
	};
	for (const SAssetDelete& request : Deletes)
	{
		const std::optional<SResolved> source = ResolveIn(Writer, request.Source);
		if (!source.has_value() || !StatFile(source->Absolute).has_value())
		{
			OutProblems.push_back(MakeProblem(Writer, ESeverity::Error, 0u, request.Source,
				FString::Format("'{}' is not a file in a mount this process writes", ToDisplayText(request.Source))));
			bOk = false;
			continue;
		}
		if (const std::optional<FStringView> metaOf = GetMetaSource(request.Source))
		{
			// Only an orphan meta is deleted by naming it: the meta of a file that exists, in any spelling, is that asset's
			// identity, and trashing it alone would leave a metaless source for the lifecycle to restore or remint.
			if (const std::optional<FString> sourceName = FindSpelling(listings, source->Absolute.parent_path(), GetMountPathLeaf(*metaOf)))
			{
				const FString sourcePath = JoinChild(GetMountPathParent(request.Source), *sourceName);
				OutProblems.push_back(MakeProblem(Writer, ESeverity::Error, 0u, request.Source,
					FString::Format("'{}' is the meta of '{}', which exists: delete the asset, and its meta goes with it",
						ToDisplayText(request.Source), ToDisplayText(sourcePath))));
				bOk = false;
				continue;
			}
			add(request.Source);
		}
		else
		{
			add(request.Source);
			if (const std::optional<FString> meta = FindMetaSpelling(listings, request.Source, source->Absolute))
			{
				add(*meta);
			}
		}
		for (const FString& companion : request.Companions)
		{
			const std::optional<SResolved> resolved = ResolveIn(Writer, companion);
			if (resolved.has_value() && StatFile(resolved->Absolute).has_value())
			{
				add(companion);
			}
		}
	}
	if (!bOk)
	{
		OutOp.Steps.clear();
	}
	return bOk;
}


SAssetCommandResult RunAssetOp (CAssetWriter& Writer, SAssetOp Op)
{
	SAssetCommandResult result;
	result.Op = std::move(Op);
	result.Vcs = Writer.Run(result.Op, result.Report);
	if (result.Vcs.Error == EVcsError::None)
	{
		return result;
	}
	const FStringView subject = result.Op.Steps.empty() ? FStringView() : GetSubjectAsset(result.Op.Steps.front().Path);
	if (result.Report.bStale)
	{
		// Not a refusal: the disk moved on since the plan, and the caller plans again from what is there now.
		const FStringView changed = !result.Report.Changed.empty() ? GetSubjectAsset(result.Report.Changed.front()) : subject;
		result.Problems.push_back(MakeProblem(Writer, ESeverity::Warning, 0u, changed, FString::Format("'{}' was not run as planned: {}",
			result.Op.Verb, result.Vcs.Message)));
	}
	else if (result.Report.bRefusedByVcs)
	{
		const FStringView outcome = result.Report.bChangedDisk ? "the files are written and the operation is left for start-up to finish"
			: "nothing was written";
		result.Problems.push_back(MakeProblem(Writer, ESeverity::Error, Vcs002, subject,
			FString::Format("the version control system refused '{}' ({}): {}; {}", result.Op.Verb, enum_::ToString(result.Vcs.Error), result.Vcs.Message,
				outcome)));
	}
	else
	{
		result.Problems.push_back(MakeProblem(Writer, ESeverity::Error, 0u, subject,
			FString::Format("'{}' failed: {}", result.Op.Verb, result.Vcs.Message)));
	}
	return result;
}

namespace
{
/**
 * Plan, then run; planned once more when the writer first finished an earlier op that changed the disk this plan read
 * (SAssetWriteReport::RolledForward). A plan that went stale for any other reason is reported as it is: the person runs
 * the command again on what is there now.
 */
template <typename FPlan>
[[nodiscard]] SAssetCommandResult PlanAndRun (CAssetWriter& Writer, const FPlan& Plan)
{
	SAssetCommandResult result;
	for (uint32 attempt = 0u; attempt < 2u; ++attempt)
	{
		result = SAssetCommandResult();
		SAssetOp op;
		std::vector<SDiagnostic> problems;
		if (!Plan(op, problems))
		{
			result.Problems = std::move(problems);
			result.Vcs = { EVcsError::Failed, result.Problems.empty() ? FString() : result.Problems.back().Message };
			return result;
		}
		result = RunAssetOp(Writer, std::move(op));
		result.Problems.insert(result.Problems.begin(), problems.begin(), problems.end());
		if (!result.Report.bStale || result.Report.RolledForward.empty())
		{
			break;
		}
	}
	return result;
}
}

SAssetCommandResult CreateMeta (CAssetWriter& Writer, const SCreateMetaRequest& Request)
{
	return PlanAndRun(Writer, [&] (SAssetOp& Op, std::vector<SDiagnostic>& Problems) { return PlanCreateMeta(Writer, Request, Op, Problems); });
}

SAssetCommandResult MoveAssets (CAssetWriter& Writer, std::span<const SAssetMove> Moves)
{
	return PlanAndRun(Writer, [&] (SAssetOp& Op, std::vector<SDiagnostic>& Problems) { return PlanMoveAssets(Writer, Moves, Op, Problems); });
}

SAssetCommandResult DeleteAssets (CAssetWriter& Writer, std::span<const SAssetDelete> Deletes)
{
	return PlanAndRun(Writer, [&] (SAssetOp& Op, std::vector<SDiagnostic>& Problems) { return PlanDeleteAssets(Writer, Deletes, Op, Problems); });
}
}
