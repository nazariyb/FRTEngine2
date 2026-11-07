#include "Assets/Meta/Lifecycle.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Meta/MetaPaths.h"


namespace frt::assets
{
namespace
{
constexpr int32 NoIndex = -1;

// The portable fold first, then bytes: the deterministic order of §5.2, which is also how a duplicate's winner is chosen.
[[nodiscard]] bool PathLess (FStringView A, FStringView B) noexcept
{
	const SStringLessIgnoreCase less;
	if (less(A, B))
	{
		return true;
	}
	if (less(B, A))
	{
		return false;
	}
	return A < B;
}

[[nodiscard]] bool FoldEqual (FStringView A, FStringView B) noexcept
{
	return SStringEqualIgnoreCase {}(A, B);
}

[[nodiscard]] FString Fold (FStringView Path)
{
	return FString(Path).FoldCase();
}

// The length of the longest common prefix of two folded paths that ends at a '/': how deep their common folder is.
[[nodiscard]] uint32 GetCommonFolderLength (FStringView A, FStringView B) noexcept
{
	const uint32 length = std::min(A.Len(), B.Len());
	uint32 common = 0u;
	for (uint32 i = 0u; i < length && A.GetData()[i] == B.GetData()[i]; ++i)
	{
		if (A.GetData()[i] == '/')
		{
			common = i + 1u;
		}
	}
	return common;
}

[[nodiscard]] uint64 GetDistanceMs (uint64 A, uint64 B) noexcept
{
	return A > B ? A - B : B - A;
}

// The source a meta path names, or the path itself when it names none (an oddly named meta still belongs somewhere).
[[nodiscard]] FStringView GetSourceOf (FStringView MetaPath) noexcept
{
	return GetMetaSource(MetaPath).value_or(MetaPath);
}

constexpr FStringLiteral NotWritableReason = "this process does not write here (a read-only run, a guest, a mount it does not own, "
	"or a read-only file)";
constexpr FStringLiteral AutoRepairOffReason = "Safety.AutoRepairMetas is off";
constexpr FStringLiteral AutoResolveOffReason = "Safety.AutoResolveDuplicates is off";

/**
 * One RunLifecycle. The phases run in a fixed order, sources and metas each in folded path order, because derived GUIDs
 * re-roll over what earlier proposals took: the same facts must give the same GUIDs on every machine. Sources come first
 * (pairs, lost metas), then new files (they need the orphans that the first pass found), then orphans (they need to know
 * which new file took them), then duplicates (they need every GUID proposed before).
 */
class CLifecycleRun
{
public:
	explicit CLifecycleRun (const SLifecycleFacts& InFacts)
		: Facts(InFacts)
		, bHeld(InFacts.bVcsHold && !InFacts.bCommand)
	{}

	[[nodiscard]] SLifecycleResult Run ()
	{
		Index();
		std::vector<uint32> newFiles;
		EvaluateSources(newFiles);
		EvaluateNewFiles(newFiles);
		ReportOrphans();
		ResolveDuplicates();
		return std::move(Result);
	}

private:
	struct SOrphan
	{
		uint32 Meta = 0u;
		const SLifecycleTombstone* Tombstone = nullptr;   // the latest with the meta's GUID
		bool bConsumed = false;                           // a repair this run pairs it
		uint64 HeldUntilMs = 0u;                          // a new file still in its delay matches it by FileId until then
	};

	// A new file past its creation delay, and the orphans (indices into Orphans) whose tombstone it matches.
	struct SNewFile
	{
		uint32 Source = 0u;
		std::vector<uint32> FileIdMatches;
		std::vector<uint32> ContentInWindow;
		std::vector<uint32> ContentOutsideWindow;

		// What may pair automatically: a FileId match outranks content.
		[[nodiscard]] const std::vector<uint32>& GetPairingMatches () const noexcept
		{
			return FileIdMatches.empty() ? ContentInWindow : FileIdMatches;
		}
	};

	// What this run knows of a meta path: its source here and its entry in Metas, either may be absent.
	struct SRunMember
	{
		int32 Source = NoIndex;
		int32 Meta = NoIndex;
	};

	const SLifecycleFacts& Facts;
	const bool bHeld;
	SLifecycleResult Result;

	std::vector<uint32> SourceOrder;                                   // the sources that get a meta, in folded path order
	std::unordered_map<FString, std::vector<uint32>> SourcesByFold;    // folded mount path -> sources, in SourceOrder
	std::unordered_set<FString> OtherSourcesByFold;                    // folded mount paths of the sources that get no meta
	std::vector<uint32> MetaOrder;                                     // Metas in folded path order
	std::vector<int32> MetaOfSource;                                   // per source: its meta in Metas, or NoIndex
	std::vector<SOrphan> Orphans;                                      // in folded path order
	std::map<SGuid, std::vector<uint32>> LiveByGuid;                   // GUID -> indices in LiveGuids
	std::map<SGuid, const SLifecycleTombstone*> TombstoneByGuid;
	std::set<SGuid> Verified;
	std::set<SGuid> Taken;                                             // live, plus what this run's repairs propose
	std::unordered_set<FString> HashRequested;

	// ---- Output ----

	void WakeAt (uint64 AtMs) noexcept
	{
		if (AtMs > Facts.NowMs)
		{
			Result.WakeAtMs = std::min(Result.WakeAtMs, AtMs);
		}
	}

	// True while Since + Delay lies ahead (it then schedules the wake). A command has no delays; 0 is a time not tracked.
	[[nodiscard]] bool IsWaiting (uint64 SinceMs, uint32 DelayMs) noexcept
	{
		if (Facts.bCommand || SinceMs == 0u || GetElapsedMs(Facts.NowMs, SinceMs) >= DelayMs)
		{
			return false;
		}
		WakeAt(SinceMs + DelayMs);
		return true;
	}

	void AddState (FStringView Path, EMetaState State)
	{
		Result.States.push_back(SLifecycleState { FString(Path), State });
	}

	void AddProblem (ESeverity Severity, uint32 Code, FStringView Subject, FString Message)
	{
		SDiagnostic problem;
		problem.Severity = Severity;
		problem.Code = Code;
		problem.Subject = FString(Subject);
		problem.Message = std::move(Message);
		Result.Problems.push_back(std::move(problem));
	}

	void RequestHash (FStringView Path)
	{
		if (HashRequested.insert(FString(Path)).second)
		{
			Result.NeedContentHash.push_back(FString(Path));
		}
	}

	[[nodiscard]] static SLifecycleAction MakeAction (ELifecycleAction Kind, FStringView Path, FStringView ToPath, const SGuid& Guid)
	{
		SLifecycleAction action;
		action.Kind = Kind;
		action.Path = FString(Path);
		action.ToPath = FString(ToPath);
		action.Guid = Guid;
		return action;
	}

	[[nodiscard]] auto IsTaken () const
	{
		return [this] (const SGuid& Guid) { return Taken.contains(Guid); };
	}

	[[nodiscard]] bool MayRepairMetas () const noexcept
	{
		return Facts.bCommand || Facts.Safety.bAutoRepairMetas;
	}

	[[nodiscard]] static FStringView GetCreateRefusal (const SLifecycleSource& Source) noexcept
	{
		return Source.bWritable ? FStringView(AutoRepairOffReason) : FStringView(NotWritableReason);
	}

	// ---- Indices ----

	[[nodiscard]] int32 FindSource (FStringView Path) const
	{
		const auto found = SourcesByFold.find(Fold(Path));
		if (found == SourcesByFold.end())
		{
			return NoIndex;
		}
		for (const uint32 index : found->second)
		{
			if (Facts.Sources[index].MountPath == Path)
			{
				return static_cast<int32>(index);
			}
		}
		return static_cast<int32>(found->second.front());
	}

	void Index ()
	{
		for (uint32 i = 0u; i < Facts.Sources.size(); ++i)
		{
			if (GetsMeta(Facts.Sources[i].Class))
			{
				SourceOrder.push_back(i);
			}
			else
			{
				OtherSourcesByFold.insert(Fold(Facts.Sources[i].MountPath));
			}
		}
		std::sort(SourceOrder.begin(), SourceOrder.end(), [this] (uint32 A, uint32 B)
		{
			return PathLess(Facts.Sources[A].MountPath, Facts.Sources[B].MountPath);
		});
		for (const uint32 index : SourceOrder)
		{
			SourcesByFold[Fold(Facts.Sources[index].MountPath)].push_back(index);
		}

		for (const SLifecycleTombstone& tombstone : Facts.Tombstones)
		{
			const SLifecycleTombstone*& latest = TombstoneByGuid[tombstone.Guid];
			if (latest == nullptr || tombstone.RemovedMs > latest->RemovedMs
				|| (tombstone.RemovedMs == latest->RemovedMs && PathLess(tombstone.LastPath, latest->LastPath)))
			{
				latest = &tombstone;
			}
		}
		for (uint32 i = 0u; i < Facts.LiveGuids.size(); ++i)
		{
			LiveByGuid[Facts.LiveGuids[i].Guid].push_back(i);
			Taken.insert(Facts.LiveGuids[i].Guid);
		}
		Verified.insert(Facts.VerifiedCopies.begin(), Facts.VerifiedCopies.end());

		MetaOrder.resize(Facts.Metas.size());
		for (uint32 i = 0u; i < MetaOrder.size(); ++i)
		{
			MetaOrder[i] = i;
		}
		std::sort(MetaOrder.begin(), MetaOrder.end(), [this] (uint32 A, uint32 B)
		{
			return PathLess(Facts.Metas[A].MountPath, Facts.Metas[B].MountPath);
		});

		MetaOfSource.assign(Facts.Sources.size(), NoIndex);
		for (const uint32 index : MetaOrder)
		{
			const SLifecycleMeta& meta = Facts.Metas[index];
			if (meta.bSourceExcluded)
			{
				continue;
			}
			const FStringView sourcePath = meta.SourcePath.IsEmpty() ? GetSourceOf(meta.MountPath) : meta.SourcePath.View();
			const int32 source = FindSource(sourcePath);
			if (source == NoIndex && OtherSourcesByFold.contains(Fold(sourcePath)))
			{
				continue;   // its source exists but gets no meta (a companion, an unclaimed file): META007 is RunValidation's
			}
			if (source == NoIndex)
			{
				SOrphan orphan;
				orphan.Meta = index;
				if (meta.Parse == EMetaParseState::Ok)
				{
					const auto tombstone = TombstoneByGuid.find(meta.Guid);
					orphan.Tombstone = tombstone != TombstoneByGuid.end() ? tombstone->second : nullptr;
				}
				Orphans.push_back(orphan);
				continue;
			}
			// Two spellings of one meta (a case-sensitive folder): the exact one is the source's.
			int32& paired = MetaOfSource[static_cast<uint32>(source)];
			if (paired == NoIndex || meta.MountPath == GetMetaPath(Facts.Sources[static_cast<uint32>(source)].MountPath))
			{
				paired = static_cast<int32>(index);
			}
		}
	}

	[[nodiscard]] int32 FindOrphan (FStringView MetaPath) const
	{
		for (uint32 i = 0u; i < Orphans.size(); ++i)
		{
			if (FoldEqual(Facts.Metas[Orphans[i].Meta].MountPath, MetaPath))
			{
				return static_cast<int32>(i);
			}
		}
		return NoIndex;
	}

	// ---- Sources: pairs, broken metas, lost metas ----

	void EvaluateSources (std::vector<uint32>& OutNewFiles)
	{
		for (const uint32 index : SourceOrder)
		{
			const SLifecycleSource& source = Facts.Sources[index];
			const int32 meta = MetaOfSource[index];
			if (source.bLfsPointer)
			{
				AddProblem(ESeverity::Error, Vcs004, source.MountPath, FString("a Git LFS pointer, not the file's content: run `git lfs pull`. "
					"It is never imported, and gets no meta while it is a pointer"));
			}
			if (meta != NoIndex)
			{
				const SLifecycleMeta& metaFacts = Facts.Metas[static_cast<uint32>(meta)];
				AddState(metaFacts.MountPath, metaFacts.Parse == EMetaParseState::Broken ? EMetaState::Broken : EMetaState::Paired);
			}
			if (source.bMetaOnDisk || meta != NoIndex)
			{
				EvaluatePaired(index, meta);
			}
			else if (source.RecordGuid.IsValid())
			{
				EvaluateLostMeta(index);
			}
			else if (source.bLfsPointer)
			{
				AddState(source.MountPath, EMetaState::PendingMeta);
			}
			else
			{
				OutNewFiles.push_back(index);
			}
		}
	}

	void EvaluatePaired (uint32 Index, int32 Meta)
	{
		const SLifecycleSource& source = Facts.Sources[Index];
		if (Meta != NoIndex && Facts.Metas[static_cast<uint32>(Meta)].Parse == EMetaParseState::Broken)
		{
			AddState(source.MountPath, EMetaState::Broken);
			ReportBroken(Facts.Metas[static_cast<uint32>(Meta)], source.RecordGuid);
			return;
		}
		AddState(source.MountPath, EMetaState::Paired);

		const FString target = GetMetaPath(source.MountPath);
		if (!source.bMetaCaseDiffers || source.MetaSpelling.IsEmpty() || source.MetaSpelling == target)
		{
			return;
		}
		const SLifecycleAction fix = MakeAction(ELifecycleAction::FixMetaCase, source.MetaSpelling, target, SGuid {});
		if (source.bWritable && (Meta == NoIndex || Facts.Metas[static_cast<uint32>(Meta)].bWritable))
		{
			// A rename by hand is a remove and an add: wait as for one, so the second half of it is not raced.
			if (!bHeld && !IsWaiting(source.LastEventMs, Facts.Times.RemoveGraceMs))
			{
				Result.Repairs.push_back(fix);
			}
			return;
		}
		AddProblem(ESeverity::Error, Meta006, source.MountPath, FString::Format("its meta is spelled {}; renaming it to {}: {}",
			source.MetaSpelling, target, NotWritableReason));
		Result.FixIts.push_back(fix);
	}

	// A broken meta is never overwritten. With two salvaged GUIDs a person picks which one stays: one fix-it per GUID,
	// the record's first, each aliasing the others so references from both branches resolve.
	void ReportBroken (const SLifecycleMeta& Meta, const SGuid& RecordGuid)
	{
		const size_t salvaged = Meta.SalvagedGuids.size();
		AddProblem(ESeverity::Error, Meta004, Meta.MountPath, FString(salvaged >= 2u
			? "two Guid: lines (a merge conflict?): keep one GUID and alias the other. A broken meta is never overwritten"
			: salvaged == 1u
				? "it does not parse; its Guid: line still resolves references. A broken meta is never overwritten: fix it by hand"
				: "it does not parse and no Guid: line survives. A broken meta is never overwritten: fix it by hand"));
		if (salvaged < 2u)
		{
			return;
		}
		std::vector<SGuid> order;
		if (std::find(Meta.SalvagedGuids.begin(), Meta.SalvagedGuids.end(), RecordGuid) != Meta.SalvagedGuids.end())
		{
			order.push_back(RecordGuid);
		}
		for (const SGuid& guid : Meta.SalvagedGuids)
		{
			if (std::find(order.begin(), order.end(), guid) == order.end())
			{
				order.push_back(guid);
			}
		}
		for (const SGuid& kept : order)
		{
			SLifecycleAction keep = MakeAction(ELifecycleAction::KeepAliasB, Meta.MountPath, FStringView(), kept);
			for (const SGuid& other : order)
			{
				if (other != kept)
				{
					keep.Aliases.push_back(other);
				}
			}
			Result.FixIts.push_back(std::move(keep));
		}
	}

	// §4.7 "Paired, meta gone": a known identity is never reminted. A lost meta is restored from a verified copy, and only
	// when its GUID is not live in another meta, since then the meta moved on purpose and a restore would duplicate it.
	void EvaluateLostMeta (uint32 Index)
	{
		const SLifecycleSource& source = Facts.Sources[Index];
		if (IsWaiting(source.MetaGoneMs, Facts.Times.RemoveGraceMs))
		{
			return;   // a VCS sync may put it back
		}
		AddState(source.MountPath, EMetaState::MissingMeta);
		const FString metaPath = GetMetaPath(source.MountPath);

		std::vector<uint32> elsewhere;
		if (const auto live = LiveByGuid.find(source.RecordGuid); live != LiveByGuid.end())
		{
			for (const uint32 index : live->second)
			{
				if (!FoldEqual(Facts.LiveGuids[index].MetaPath, metaPath))
				{
					elsewhere.push_back(index);
				}
			}
			std::sort(elsewhere.begin(), elsewhere.end(), [this] (uint32 A, uint32 B)
			{
				return PathLess(Facts.LiveGuids[A].MetaPath, Facts.LiveGuids[B].MetaPath);
			});
		}
		if (!elsewhere.empty())
		{
			AddProblem(ESeverity::Error, Meta008, source.MountPath, FString::Format("its meta is gone and its GUID {} is now in {}: the meta "
				"moved on purpose (an identity transfer), so it is not restored. Give this file a new GUID, or move the meta back",
				source.RecordGuid.ToString(), Facts.LiveGuids[elsewhere.front()].MetaPath));
			for (const uint32 index : elsewhere)
			{
				const int32 orphan = FindOrphan(Facts.LiveGuids[index].MetaPath);
				if (orphan != NoIndex)
				{
					const SLifecycleMeta& meta = Facts.Metas[Orphans[static_cast<uint32>(orphan)].Meta];
					Result.FixIts.push_back(MakeAction(ELifecycleAction::MoveMeta, meta.MountPath, metaPath, meta.Guid));
				}
			}
			OfferNewGuid(source);
			return;
		}

		const SLifecycleAction restore = MakeAction(ELifecycleAction::RestoreMeta, source.MountPath, FStringView(), source.RecordGuid);
		if (Verified.contains(source.RecordGuid))
		{
			if (source.bWritable && MayRepairMetas())
			{
				if (!bHeld)
				{
					Taken.insert(source.RecordGuid);
					Result.Repairs.push_back(restore);
				}
				return;
			}
			AddProblem(ESeverity::Error, Meta008, source.MountPath, FString::Format("its meta is gone; a verified copy can restore it: {}",
				GetCreateRefusal(source)));
			Result.FixIts.push_back(restore);
			return;
		}
		AddProblem(ESeverity::Error, Meta008, source.MountPath, FString("its meta is gone and no verified copy is left: restore it from "
			"version control, or create it from the registry (its settings are lost)"));
		Result.FixIts.push_back(MakeAction(ELifecycleAction::CreateMeta, source.MountPath, FStringView(), source.RecordGuid));
	}

	// The "new GUID" fix-it of a meta swap's old source: the derivation of CreateMeta, so it needs the content.
	void OfferNewGuid (const SLifecycleSource& Source)
	{
		if (!Facts.Safety.bDerivedAutoGuids)
		{
			Result.FixIts.push_back(MakeAction(ELifecycleAction::CreateMeta, Source.MountPath, FStringView(), SGuid {}));
			return;
		}
		if (!Source.Content.has_value())
		{
			if (!Source.bLfsPointer)
			{
				RequestHash(Source.MountPath);
			}
			return;
		}
		const SGuid guid = MakeCreateMetaGuid(Facts.Salt, Source.MountPath, *Source.Content, IsTaken());
		Result.FixIts.push_back(MakeAction(ELifecycleAction::CreateMeta, Source.MountPath, FStringView(), guid));
	}

	// ---- New files: pairing with an orphan, or CreateMeta ----

	void EvaluateNewFiles (const std::vector<uint32>& NewFiles)
	{
		std::vector<SNewFile> ready;
		for (const uint32 index : NewFiles)
		{
			const SLifecycleSource& source = Facts.Sources[index];
			if (IsWaiting(source.LastEventMs, Facts.Times.MetaCreationDelayMs))
			{
				AddState(source.MountPath, EMetaState::PendingMeta);   // the VCS may deliver its meta yet
				// A move outside the editor: the orphan's grace (RemoveGraceMs) ends before this file's delay does. The orphan it
				// matches by FileId is not reported meanwhile, since this file will most likely take it.
				for (SOrphan& orphan : Orphans)
				{
					if (orphan.Tombstone != nullptr && source.Identity.IsValid() && orphan.Tombstone->Identity == source.Identity)
					{
						orphan.HeldUntilMs = std::max(orphan.HeldUntilMs, source.LastEventMs + Facts.Times.MetaCreationDelayMs);
					}
				}
				continue;
			}
			SNewFile file;
			file.Source = index;
			const uint64 arrivalMs = source.LastEventMs != 0u ? source.LastEventMs : Facts.NowMs;
			for (uint32 k = 0u; k < Orphans.size(); ++k)
			{
				const SLifecycleTombstone* tombstone = Orphans[k].Tombstone;
				if (tombstone == nullptr)
				{
					continue;
				}
				if (source.Identity.IsValid() && tombstone->Identity == source.Identity)
				{
					file.FileIdMatches.push_back(k);
				}
				else if (source.Content.has_value() && tombstone->Hash.IsValid() && tombstone->Hash == *source.Content)
				{
					const bool bInWindow = GetDistanceMs(arrivalMs, tombstone->RemovedMs) <= Facts.Times.OrphanPairWindowMs;
					(bInWindow ? file.ContentInWindow : file.ContentOutsideWindow).push_back(k);
				}
			}
			ready.push_back(std::move(file));
		}

		// A FileId is definitive (§4.6: "by FileId, or by BLAKE3 content hash when unique"): an orphan that a new file claims
		// by FileId is that file's, and another file's content match to it is no match at all. Otherwise whether the other
		// file had been hashed yet would decide which identity each file gets.
		std::vector<bool> claimedByFileId(Orphans.size(), false);
		for (const SNewFile& file : ready)
		{
			for (const uint32 k : file.FileIdMatches)
			{
				claimedByFileId[k] = true;
			}
		}
		for (SNewFile& file : ready)
		{
			const auto isClaimed = [&claimedByFileId] (uint32 K) -> bool { return claimedByFileId[K]; };
			std::erase_if(file.ContentInWindow, isClaimed);
			std::erase_if(file.ContentOutsideWindow, isClaimed);
		}

		// An orphan claimed by two new files (both by FileId, or both by content) pairs with neither: which one moved is a
		// person's call.
		std::vector<uint32> claims(Orphans.size(), 0u);
		for (const SNewFile& file : ready)
		{
			for (const uint32 k : file.GetPairingMatches())
			{
				++claims[k];
			}
		}
		// A file without its content hash and without a FileId match waits for it itself: its derived GUID, or its content
		// match, needs it (under a hold nothing is decided by content anyway, so nothing is hashed: the VCS may still be
		// writing those files, and would make F4 hash them twice, edit [27]). It holds back only the files that match an
		// orphan by content, and only while an orphan that no FileId claims has a tombstone hash: its claim, not yet known,
		// could make their pairing ambiguous. Any other file is decided whatever it hashes to (MakeCreateMetaGuid is of a
		// file's own path and content). A hash that could not be read (bHashFailed) holds nothing back: it cannot match.
		bool bAnyHashCandidate = false;
		for (uint32 k = 0u; k < Orphans.size(); ++k)
		{
			bAnyHashCandidate = bAnyHashCandidate || (Orphans[k].Tombstone != nullptr && Orphans[k].Tombstone->Hash.IsValid() && !claimedByFileId[k]);
		}
		bool bClaimPending = false;
		for (const SNewFile& file : ready)
		{
			const SLifecycleSource& source = Facts.Sources[file.Source];
			if (NeedsOwnHash(source, file, bAnyHashCandidate) && !source.bHashFailed)
			{
				if (!bHeld)
				{
					RequestHash(source.MountPath);
				}
				bClaimPending = bClaimPending || bAnyHashCandidate;
			}
		}
		for (SNewFile& file : ready)
		{
			DecideNewFile(file, claims, bAnyHashCandidate, bClaimPending);
		}
	}

	// Whether a ready new file waits for its own content hash.
	[[nodiscard]] bool NeedsOwnHash (const SLifecycleSource& Source, const SNewFile& File, bool bAnyHashCandidate) const noexcept
	{
		return !Source.Content.has_value() && File.FileIdMatches.empty() && (bAnyHashCandidate || Facts.Safety.bDerivedAutoGuids);
	}

	void DecideNewFile (const SNewFile& File, const std::vector<uint32>& Claims, bool bAnyHashCandidate, bool bClaimPending)
	{
		const SLifecycleSource& source = Facts.Sources[File.Source];
		if (NeedsOwnHash(source, File, bAnyHashCandidate))
		{
			AddState(source.MountPath, EMetaState::PendingMeta);
			if (source.bHashFailed)
			{
				// Never a guess: the file might have matched an orphan, so it waits for a read, and says why it waits.
				AddProblem(ESeverity::Error, Meta001, source.MountPath, FString("no meta: its content cannot be read (in use by another "
					"program, or access denied), so its GUID cannot be derived; it gets its meta once it can be read"));
			}
			else if (!source.bWritable)
			{
				// Every outcome is an error here (META001, or META008 for a match): say so even to a caller that never hashes.
				AddProblem(ESeverity::Error, Meta001, source.MountPath, FString::Format("no meta: {}", NotWritableReason));
			}
			return;
		}
		if (bClaimPending && File.FileIdMatches.empty() && !File.ContentInWindow.empty())
		{
			AddState(source.MountPath, EMetaState::PendingMeta);   // another file's hash, still to come, may claim its orphan too
			return;
		}

		// The switch removes the automatic pairing and nothing else (§4.8), even for a command (`fix` never relinks): a match
		// that would have paired becomes a relink fix-it, and an older one stays a suggestion as with the switch on.
		const std::vector<uint32>& pairing = File.GetPairingMatches();
		if (!Facts.Safety.bOrphanPairing && !pairing.empty())
		{
			ReportMatched(source, pairing, "Safety.OrphanPairing is off");
			return;
		}

		if (pairing.size() == 1u && Claims[pairing.front()] == 1u)
		{
			SOrphan& orphan = Orphans[pairing.front()];
			const SLifecycleMeta& meta = Facts.Metas[orphan.Meta];
			if (source.bWritable && meta.bWritable)
			{
				orphan.bConsumed = true;
				AddState(source.MountPath, EMetaState::PendingMeta);
				if (!bHeld)
				{
					Result.Repairs.push_back(MakeAction(ELifecycleAction::MoveMeta, meta.MountPath, GetMetaPath(source.MountPath), meta.Guid));
				}
				return;
			}
			ReportMatched(source, pairing, NotWritableReason);
			return;
		}
		if (!pairing.empty())
		{
			ReportMatched(source, pairing, "more than one orphan meta or new file matches, so which one moved is yours to say");
			return;
		}

		// An older orphan with this content is only a suggestion (§4.6): the file gets its own identity.
		for (const uint32 k : File.ContentOutsideWindow)
		{
			Suggest(source, k);
		}
		AddState(source.MountPath, EMetaState::PendingMeta);
		SGuid guid;
		if (Facts.Safety.bDerivedAutoGuids)
		{
			if (!source.Content.has_value())
			{
				return;   // unreachable: such a file waits for its hash (NeedsOwnHash)
			}
			guid = MakeCreateMetaGuid(Facts.Salt, source.MountPath, *source.Content, IsTaken());
		}
		const SLifecycleAction create = MakeAction(ELifecycleAction::CreateMeta, source.MountPath, FStringView(), guid);
		if (source.bWritable && MayRepairMetas())
		{
			if (!bHeld)
			{
				if (guid.IsValid())
				{
					Taken.insert(guid);
				}
				Result.Repairs.push_back(create);
			}
			return;
		}
		AddProblem(ESeverity::Error, Meta001, source.MountPath, FString::Format("no meta: {}", GetCreateRefusal(source)));
		Result.FixIts.push_back(create);
	}

	// §4.8 OrphanPairing off, an ambiguous match, or a match nobody may move: the file waits in MissingMeta and never gets a
	// second identity while an orphan matches it.
	void ReportMatched (const SLifecycleSource& Source, const std::vector<uint32>& Matches, FStringView Reason)
	{
		AddState(Source.MountPath, EMetaState::MissingMeta);
		AddProblem(ESeverity::Error, Meta008, Source.MountPath, FString::Format("no meta, and the orphan meta {} matches it: relink it "
			"rather than mint a second identity ({})", Facts.Metas[Orphans[Matches.front()].Meta].MountPath, Reason));
		for (const uint32 k : Matches)
		{
			Suggest(Source, k);
		}
	}

	void Suggest (const SLifecycleSource& Source, uint32 Orphan)
	{
		const SLifecycleMeta& meta = Facts.Metas[Orphans[Orphan].Meta];
		const bool bByFileId = Source.Identity.IsValid() && Orphans[Orphan].Tombstone->Identity == Source.Identity;
		AddProblem(ESeverity::Info, Meta009, meta.MountPath, FString::Format("matches {} by {}: relink?", Source.MountPath,
			bByFileId ? "file id" : "content"));
		Result.FixIts.push_back(MakeAction(ELifecycleAction::MoveMeta, meta.MountPath, GetMetaPath(Source.MountPath), meta.Guid));
	}

	// ---- Orphans ----

	void ReportOrphans ()
	{
		for (const SOrphan& orphan : Orphans)
		{
			const SLifecycleMeta& meta = Facts.Metas[orphan.Meta];
			if (meta.Parse == EMetaParseState::Broken)
			{
				AddState(meta.MountPath, EMetaState::Broken);
				ReportBroken(meta, SGuid {});
				continue;
			}
			if (orphan.bConsumed)
			{
				continue;
			}
			if (orphan.HeldUntilMs > Facts.NowMs)
			{
				WakeAt(orphan.HeldUntilMs);
				continue;
			}
			// A VCS sync may bring the source back: wait as for any removal.
			if (IsWaiting(orphan.Tombstone != nullptr ? orphan.Tombstone->RemovedMs : meta.FirstSeenMs, Facts.Times.RemoveGraceMs))
			{
				continue;
			}
			AddState(meta.MountPath, EMetaState::Orphan);
			const FStringView sourcePath = meta.SourcePath.IsEmpty() ? GetSourceOf(meta.MountPath) : meta.SourcePath.View();
			AddProblem(ESeverity::Warning, Meta002, meta.MountPath, FString::Format("orphan meta: {} is gone. The meta is kept, never "
				"deleted automatically", sourcePath));
		}
	}

	// ---- Duplicates ----

	struct SMember
	{
		uint32 Live = 0u;
		FString SourcePath;
		FString FoldedSource;
		bool bCommitted = false;
		std::optional<SRunMember> Run;   // nullopt: another mount's
	};

	struct SGroup
	{
		SGuid Guid;
		std::vector<SMember> Members;
		uint32 Winner = 0u;
	};

	// What becomes of a loser: rewritten now, offered to a person, or never rewritten.
	enum class EOutcome : uint8 { Repair, FixIt, Broken };

	struct SLoser
	{
		const SGroup* Group = nullptr;
		const SMember* Member = nullptr;
		SGuid NewGuid;
		EOutcome Outcome = EOutcome::Repair;
	};

	[[nodiscard]] std::unordered_map<FString, SRunMember> CollectRunMembers () const
	{
		std::unordered_map<FString, SRunMember> members;
		for (const uint32 index : SourceOrder)
		{
			const SLifecycleSource& source = Facts.Sources[index];
			if (source.bMetaOnDisk || MetaOfSource[index] != NoIndex)
			{
				members[Fold(GetMetaPath(source.MountPath))] = SRunMember { static_cast<int32>(index), MetaOfSource[index] };
			}
		}
		for (const uint32 index : MetaOrder)
		{
			SRunMember& member = members[Fold(Facts.Metas[index].MountPath)];
			if (member.Meta == NoIndex)
			{
				member.Meta = static_cast<int32>(index);
			}
		}
		return members;
	}

	[[nodiscard]] std::vector<SGroup> CollectGroups () const
	{
		const std::unordered_map<FString, SRunMember> runMembers = CollectRunMembers();
		std::vector<SGroup> groups;
		for (const auto& [guid, indices] : LiveByGuid)
		{
			if (indices.size() < 2u)
			{
				continue;
			}
			std::vector<uint32> sorted = indices;
			std::sort(sorted.begin(), sorted.end(), [this] (uint32 A, uint32 B)
			{
				return PathLess(Facts.LiveGuids[A].MetaPath, Facts.LiveGuids[B].MetaPath);
			});
			SGroup group;
			group.Guid = guid;
			std::unordered_set<FString> seen;
			bool bTouchesRun = false;
			for (const uint32 index : sorted)
			{
				const FString folded = Fold(Facts.LiveGuids[index].MetaPath);
				if (!seen.insert(folded).second)
				{
					continue;   // one file, listed twice
				}
				SMember member;
				member.Live = index;
				member.SourcePath = FString(GetSourceOf(Facts.LiveGuids[index].MetaPath));
				member.FoldedSource = Fold(member.SourcePath);
				if (const auto run = runMembers.find(folded); run != runMembers.end())
				{
					member.Run = run->second;
					bTouchesRun = true;
				}
				group.Members.push_back(std::move(member));
			}
			if (group.Members.size() >= 2u && bTouchesRun)
			{
				groups.push_back(std::move(group));
			}
		}
		return groups;
	}

	// The latest event of a member here: a batch is resolved only once every member has been quiet, so the map is whole.
	[[nodiscard]] uint64 GetLastEventMs (const SRunMember& Member) const noexcept
	{
		uint64 last = 0u;
		if (Member.Source != NoIndex)
		{
			last = std::max(last, Facts.Sources[static_cast<uint32>(Member.Source)].LastEventMs);
		}
		if (Member.Meta != NoIndex)
		{
			last = std::max(last, Facts.Metas[static_cast<uint32>(Member.Meta)].FirstSeenMs);
		}
		return last;
	}

	[[nodiscard]] bool IsBrokenMember (const SRunMember& Member) const noexcept
	{
		const auto isBroken = [this] (int32 Meta)
		{
			return Meta != NoIndex && Facts.Metas[static_cast<uint32>(Meta)].Parse == EMetaParseState::Broken;
		};
		return isBroken(Member.Meta) || (Member.Source != NoIndex && isBroken(MetaOfSource[static_cast<uint32>(Member.Source)]));
	}

	[[nodiscard]] bool IsWritableMember (const SRunMember& Member) const noexcept
	{
		return (Member.Source == NoIndex || Facts.Sources[static_cast<uint32>(Member.Source)].bWritable)
			&& (Member.Meta == NoIndex || Facts.Metas[static_cast<uint32>(Member.Meta)].bWritable);
	}

	// Whether loser A lies nearer to Of than loser B: a deeper common folder, then the smaller folded path.
	[[nodiscard]] static bool IsNearer (const SLoser& Of, const SLoser& A, const SLoser& B) noexcept
	{
		const uint32 a = GetCommonFolderLength(Of.Member->FoldedSource, A.Member->FoldedSource);
		const uint32 b = GetCommonFolderLength(Of.Member->FoldedSource, B.Member->FoldedSource);
		return a != b ? a > b : PathLess(A.Member->SourcePath, B.Member->SourcePath);
	}

	[[nodiscard]] bool IsRepairableMember (const SRunMember& Member) const noexcept
	{
		return Facts.Safety.bAutoResolveDuplicates && !IsBrokenMember(Member) && IsWritableMember(Member);
	}

	[[nodiscard]] FStringView GetMemberSubject (const SMember& Member) const noexcept
	{
		const SRunMember& run = *Member.Run;
		return run.Source != NoIndex ? Facts.Sources[static_cast<uint32>(run.Source)].MountPath.View()
			: Facts.LiveGuids[Member.Live].MetaPath.View();
	}

	// Before the winner is known (a hold, a member still settling, the committed facts not asked yet): a group with two
	// members here that no repair can rewrite has a loser among them whoever wins, so META003 is an error in every outcome
	// and is reported to a caller that never answers bNeedCommitted. The fix-it waits for the winner.
	void ReportCertainDuplicates (const std::vector<SGroup>& Groups)
	{
		for (const SGroup& group : Groups)
		{
			std::vector<const SMember*> stuck;
			for (const SMember& member : group.Members)
			{
				if (member.Run.has_value() && !IsRepairableMember(*member.Run))
				{
					stuck.push_back(&member);
				}
			}
			if (stuck.size() < 2u)
			{
				continue;
			}
			for (const SMember* member : stuck)
			{
				const SMember* other = member == stuck.front() ? stuck[1] : stuck.front();
				AddProblem(ESeverity::Error, Meta003, GetMemberSubject(*member), FString::Format("duplicate GUID {}, also in {}: whichever "
					"keeps it, a copy here that cannot be rewritten automatically needs a new GUID", group.Guid.ToString(),
					Facts.LiveGuids[other->Live].MetaPath));
			}
		}
	}

	// §4.6 Explorer copy, resolved as a set from committed facts only, so every machine picks the same winner and derives the
	// same losers' GUIDs.
	void ResolveDuplicates ()
	{
		std::vector<SGroup> groups = CollectGroups();
		if (groups.empty())
		{
			return;
		}
		if (bHeld)
		{
			ReportCertainDuplicates(groups);
			return;
		}
		bool bWaiting = false;
		for (const SGroup& group : groups)
		{
			for (const SMember& member : group.Members)
			{
				if (member.Run.has_value() && IsWaiting(GetLastEventMs(*member.Run), Facts.Times.MetaCreationDelayMs))
				{
					bWaiting = true;
				}
			}
		}
		if (bWaiting)
		{
			ReportCertainDuplicates(groups);
			return;
		}
		if (!Facts.Committed.has_value())
		{
			Result.bNeedCommitted = true;
			ReportCertainDuplicates(groups);
			return;
		}

		std::unordered_set<FString> committed;
		for (const FString& path : *Facts.Committed)
		{
			committed.insert(Fold(path));
		}
		std::vector<SLoser> losers;
		for (SGroup& group : groups)
		{
			for (uint32 i = 0u; i < group.Members.size(); ++i)
			{
				SMember& member = group.Members[i];
				member.bCommitted = committed.contains(Fold(Facts.LiveGuids[member.Live].MetaPath));
				const SMember& winner = group.Members[group.Winner];
				if (member.bCommitted != winner.bCommitted ? member.bCommitted : PathLess(member.SourcePath, winner.SourcePath))
				{
					group.Winner = i;
				}
			}
		}
		for (const SGroup& group : groups)
		{
			for (uint32 i = 0u; i < group.Members.size(); ++i)
			{
				if (i != group.Winner && group.Members[i].Run.has_value())
				{
					const SRunMember& run = *group.Members[i].Run;
					const EOutcome outcome = IsBrokenMember(run) ? EOutcome::Broken : IsRepairableMember(run) ? EOutcome::Repair : EOutcome::FixIt;
					losers.push_back(SLoser { &group, &group.Members[i], SGuid {}, outcome });
				}
			}
		}
		std::sort(losers.begin(), losers.end(), [] (const SLoser& A, const SLoser& B)
		{
			return A.Member->SourcePath != B.Member->SourcePath ? PathLess(A.Member->SourcePath, B.Member->SourcePath) : A.Group->Guid < B.Group->Guid;
		});
		// Stand-ins too when DerivedAutoGuids is off: the Remaps need distinct values the planner can replace.
		for (SLoser& loser : losers)
		{
			loser.NewGuid = MakeDedupeGuid(Facts.Salt, loser.Group->Guid, loser.Member->SourcePath, IsTaken());
			Taken.insert(loser.NewGuid);
		}

		for (const SLoser& loser : losers)
		{
			const FStringView subject = GetMemberSubject(*loser.Member);
			const SMember& winner = loser.Group->Members[loser.Group->Winner];
			const FString& winnerPath = Facts.LiveGuids[winner.Live].MetaPath;
			if (loser.Outcome == EOutcome::Broken)
			{
				AddProblem(ESeverity::Error, Meta003, subject, FString::Format("duplicate GUID {}: {} keeps it. This meta is broken, so it "
					"is never rewritten: fix it by hand", loser.Group->Guid.ToString(), winnerPath));
				continue;
			}

			SLifecycleAction reGuid = MakeAction(ELifecycleAction::ReGuid, Facts.LiveGuids[loser.Member->Live].MetaPath, FStringView(),
				Facts.Safety.bDerivedAutoGuids ? loser.NewGuid : SGuid {});
			reGuid.Remap.emplace_back(loser.Group->Guid, loser.NewGuid);
			// Only losers rewritten together (the repairs as one SAssetOp, or the fix-its applied as a set) map to each other: a
			// loser that stays as it is keeps its old GUID, so a map to its new one would point at a GUID no meta holds. And
			// only a loser under this one's copy root, nearer to it than its own winner, so unrelated copies that land in one
			// batch never reference each other.
			const uint32 ownRoot = GetCommonFolderLength(loser.Member->FoldedSource, winner.FoldedSource);
			std::map<SGuid, const SLoser*> nearest;
			for (const SLoser& other : losers)
			{
				if (other.Group->Guid == loser.Group->Guid || other.Outcome != loser.Outcome
					|| GetCommonFolderLength(loser.Member->FoldedSource, other.Member->FoldedSource) <= ownRoot)
				{
					continue;
				}
				const SLoser*& best = nearest[other.Group->Guid];
				if (best == nullptr || IsNearer(loser, other, *best))
				{
					best = &other;
				}
			}
			for (const auto& [oldGuid, best] : nearest)
			{
				reGuid.Remap.emplace_back(oldGuid, best->NewGuid);
			}

			if (loser.Outcome == EOutcome::Repair)
			{
				Result.Repairs.push_back(std::move(reGuid));
				continue;
			}
			const bool bWritable = IsWritableMember(*loser.Member->Run);
			AddProblem(ESeverity::Error, Meta003, subject, FString::Format("duplicate GUID {}: {} keeps it, this one needs a new GUID: {}",
				loser.Group->Guid.ToString(), winnerPath, bWritable ? FStringView(AutoResolveOffReason) : FStringView(NotWritableReason)));
			Result.FixIts.push_back(std::move(reGuid));
		}
	}
};
}


SLifecycleResult RunLifecycle (const SLifecycleFacts& Facts)
{
	return CLifecycleRun(Facts).Run();
}
}
