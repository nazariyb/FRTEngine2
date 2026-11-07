#include "Assets/Meta/AssetRepairs.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Discovery/Mounts.h"
#include "Assets/FileWatcherService.h"
#include "Assets/Meta/AssetCommands.h"
#include "Assets/Meta/AssetWriter.h"
#include "Assets/Meta/Lifecycle.h"
#include "Assets/Meta/MetaFile.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/MountPaths.h"
#include "Assets/OwnerLock.h"
#include "Assets/Pipeline/Rules.h"
#include "Assets/Registry/RegistryDelta.h"
#include "Assets/Registry/RegistryTables.h"
#include "Assets/Registry/RegistryView.h"
#include "Assets/Registry/Snapshot.h"
#include "Containers/StringMap.h"
#include "Hash/StableHash.h"
#include "Jobs/WorkerPool.h"
#include "Log/Log.h"
#include "Platform/FileSystem.h"
#include "Platform/Timestamp.h"
#include "Unicode/DisplayText.h"


namespace frt::assets
{
namespace
{
constexpr uint64 BusyPollMs = 100u;                 // §4.7: IsBusy polled every 100 ms while something is pending
constexpr uint32 MaxLifecycleCalls = 4u;            // a run, then again with the hashes, then with the committed facts
constexpr uint32 MaxCommandRounds = 6u;             // RunNow: repair, rescan, again, until nothing is planned
constexpr uint64 LfsPointerMaxBytes = 1024u;        // §13.3: a Git LFS pointer is a small text file
constexpr FStringLiteral LfsPointerPrefix = "version https://git-lfs.github.com/spec/";

// What a file's FILES row says about its bytes: a cached fact holds while these do.
struct SStatKey
{
	uint64 Size = 0u;
	int64 MTime = 0;
	platform::SFileIdentity Identity;

	[[nodiscard]] bool operator== (const SStatKey& Other) const noexcept
	{
		return Size == Other.Size && MTime == Other.MTime && Identity == Other.Identity;
	}
};

struct SHashed
{
	SStatKey Stat;
	std::optional<hash::SContentHash> Hash;   // nullopt: a pointer, or it kept changing
	bool bLfsPointer = false;
};

// An orphan's or a broken meta's identity, read from its file (nothing of it is stored in the registry).
struct SMetaRead
{
	SStatKey Stat;
	EMetaParseState Parse = EMetaParseState::Broken;
	SGuid Guid;
	std::vector<SGuid> Salvaged;
	bool bCrLf = false;
};

// Read and written only by the job in flight (there is at most one), so they need no lock.
struct SCaches
{
	TStringMap<SHashed> Hashes;
	TStringMap<SMetaRead> Metas;
};

[[nodiscard]] SStatKey GetStatKey (const SFileData& File) noexcept
{
	return SStatKey { File.Size, File.MTime, File.Identity };
}

[[nodiscard]] FString MakeActionKey (const SLifecycleAction& Action)
{
	return FString::Format("{}|{}|{}", enum_::ToString(Action.Kind), Action.Path, Action.ToPath);
}

[[nodiscard]] bool FoldEqual (FStringView A, FStringView B) noexcept
{
	return SStringEqualIgnoreCase {}(A, B);
}

[[nodiscard]] FStringView GetAssetOf (FStringView Path) noexcept
{
	return GetMetaSource(Path).value_or(Path);
}

// Whether Action is about the file Subject names (an asset, or its meta), in either spelling.
[[nodiscard]] bool Concerns (const SLifecycleAction& Action, FStringView Subject) noexcept
{
	const FStringView asset = GetAssetOf(Subject);
	for (const FStringView path : { Action.Path.View(), Action.ToPath.View() })
	{
		if (!path.IsEmpty() && (FoldEqual(path, Subject) || FoldEqual(GetAssetOf(path), asset)))
		{
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool IsInScope (FStringView Path, std::span<const FString> Paths) noexcept
{
	if (Paths.empty())
	{
		return true;
	}
	const FStringView asset = GetAssetOf(Path);
	return std::any_of(Paths.begin(), Paths.end(), [&] (const FString& Root)
	{
		return IsMountPathAtOrBelow(Path, Root.View()) || IsMountPathAtOrBelow(asset, Root.View());
	});
}

[[nodiscard]] bool HasCrLf (std::span<const uint8> Bytes) noexcept
{
	for (size_t i = 1u; i < Bytes.size(); ++i)
	{
		if (Bytes[i] == '\n' && Bytes[i - 1u] == '\r')
		{
			return true;
		}
	}
	return false;
}

// The content hash of a source the lifecycle asked for, in one race-free read; a pointer of Git LFS is never hashed into a
// GUID (§4.7), so its first bytes are looked at whenever the file is small enough to be one.
[[nodiscard]] SHashed HashSource (const std::filesystem::path& Path)
{
	SHashed out;
	const std::optional<platform::SFileStat> stat = platform::Stat(Path);
	if (!stat.has_value() || stat->bDirectory)
	{
		return out;
	}
	out.Stat = SStatKey { stat->Size, stat->LastWriteTime, stat->Identity };
	hash::CContentHasher hasher;
	std::vector<uint8> head;
	const platform::EStableRead read = platform::ReadFileChunksStable(Path, [&] (std::span<const uint8> Chunk)
	{
		hasher.Update(Chunk);
		if (head.size() < LfsPointerMaxBytes)
		{
			const size_t take = std::min<size_t>(Chunk.size(), LfsPointerMaxBytes - head.size());
			head.insert(head.end(), Chunk.begin(), Chunk.begin() + static_cast<ptrdiff_t>(take));
		}
	});
	if (read != platform::EStableRead::Stable)
	{
		return out;
	}
	if (stat->Size < LfsPointerMaxBytes
		&& FStringView(reinterpret_cast<const char*>(head.data()), static_cast<uint32>(head.size())).StartsWith(LfsPointerPrefix))
	{
		out.bLfsPointer = true;
		return out;
	}
	out.Hash = hasher.Finalize();
	return out;
}

[[nodiscard]] std::optional<SMetaRead> ReadMetaIdentity (const std::filesystem::path& Path, const SStatKey& Stat)
{
	FString bytes;
	const platform::EStableRead read = platform::ReadFileChunksStable(Path, [&bytes] (std::span<const uint8> Chunk)
	{
		bytes += FStringView(reinterpret_cast<const char*>(Chunk.data()), static_cast<uint32>(Chunk.size()));
	});
	if (read != platform::EStableRead::Stable)
	{
		return std::nullopt;
	}
	SMetaParseResult parsed;
	ParseMeta(bytes, parsed);
	SMetaRead out;
	out.Stat = Stat;
	out.Parse = parsed.State;
	out.Guid = parsed.State == EMetaParseState::Ok ? parsed.Meta.Guid : SGuid {};
	out.Salvaged = std::move(parsed.SalvagedGuids);
	out.bCrLf = parsed.bHasCrLf;
	return out;
}

struct SPlannedOp
{
	SAssetOp Op;
	std::vector<SLifecycleAction> Actions;
};

struct SCycleInput
{
	std::shared_ptr<const CRegistryView> View;
	const CMountTable* Mounts = nullptr;
	const CRulesEngine* Rules = nullptr;
	const SLifecycleDesc* Lifecycle = nullptr;
	std::shared_ptr<SCaches> Caches;
	std::vector<IVcsProvider*> Providers;          // by MountId; null: None
	std::vector<uint8> Writable;                   // by MountId: this process repairs there
	std::vector<MountId> RunMounts;
	std::vector<FString> Paths;                    // RunNow's scope; empty: the whole of each run mount
	TStringMap<uint64> Events;                     // mount path -> the time of the first tick after a pass touched it
	std::vector<SDiagnostic> MemoryProblems;       // VCS002 and VCS003 this process keeps (Subject set)
	FStringSet CrLfMetas;                          // metas known to hold CRLF (VCS001)
	FStringSet Refused;                            // MakeActionKey of the repairs the VCS refused
	std::vector<ELifecycleAction> Kinds;           // empty: all
	const CAssetWriter* Writer = nullptr;          // plans when set
	std::vector<SLifecycleAction> FixActions;      // a fix: plans these and gathers nothing
	uint64 NowMs = 0u;
	bool bCommand = false;
	bool bHold = false;
	bool bCheckAllCrLf = false;
};

struct SCycleOutput
{
	uint64 Revision = 0u;
	SRegistryDelta Delta;
	std::vector<SLifecycleAction> FixIts;
	std::vector<SLifecycleAction> Repairs;          // the proposals that may be applied (Kinds, refusals), all of them
	std::vector<SPlannedOp> Ops;
	std::vector<SDiagnostic> Problems;              // the Lifecycle problems in scope after this run, as committed
	std::vector<SDiagnostic> PlanProblems;          // repairs that could not be planned
	std::vector<std::pair<FString, bool>> CrLfChecked;
	uint64 WakeAtMs = ~0ull;
	bool bHeld = false;
	bool bPending = false;                          // held with something to decide: run again when the hold ends
	uint32 Runs = 0u;
};

/**
 * One run of the lifecycle over the mounts asked for (a worker job): the facts gathered from the view and the caches, one
 * RunLifecycle per mount with what it asks for fetched in between, the States, Problems and hashes as one delta against
 * that view, and the repairs planned into ops. Edits nothing outside its output and the caches.
 */
class CCycle
{
public:
	explicit CCycle (const SCycleInput& InInput)
		: In(InInput)
		, View(*InInput.View)
		, Tables(GetTables(*InInput.View))
	{}

	[[nodiscard]] SCycleOutput Run ()
	{
		Out.Revision = View.GetRevision();
		Out.bHeld = In.bHold;
		if (!In.FixActions.empty())
		{
			Plan(In.FixActions);
			return std::move(Out);
		}
		Walk();
		std::vector<SDiagnostic> problems;
		for (const MountId mount : In.RunMounts)
		{
			RunMount(mount, problems);
		}
		AddMemoryProblems(problems);
		FinishProblems(problems);
		AddStates();
		if (In.Writer != nullptr && !In.bHold)
		{
			Plan(Out.Repairs);
		}
		return std::move(Out);
	}

private:
	struct SSourceRow
	{
		uint32 Row = InvalidRow;
		uint32 MetaRow = InvalidRow;
	};

	struct SOrphanRow
	{
		uint32 Row = InvalidRow;
		bool bSourceExcluded = false;
	};

	const SCycleInput& In;
	const CRegistryView& View;
	const SRegistryTables& Tables;
	SCycleOutput Out;

	std::vector<std::vector<SSourceRow>> SourcesByMount;
	std::vector<std::vector<SOrphanRow>> OrphansByMount;
	std::vector<SLiveGuid> LiveGuids;
	std::set<SGuid> Duplicates;
	std::vector<std::pair<FString, SDiagnostic>> CrLf;   // meta path, VCS001
	TStringMap<SHashed> NewHashes;                       // hashed in this run (committed into FILES)
	std::vector<SLifecycleState> States;

	[[nodiscard]] const SMount* FindMount (MountId Id) const noexcept
	{
		return In.Mounts->Find(Id);
	}

	[[nodiscard]] std::optional<std::filesystem::path> ToAbsolute (FStringView MountPath) const
	{
		return In.Mounts->FromMountPath(MountPath);
	}

	[[nodiscard]] bool HasEvent (FStringView Path) const
	{
		return In.Events.contains(FString(Path));
	}

	[[nodiscard]] uint64 GetEventMs (FStringView Path) const
	{
		const auto found = In.Events.find(FString(Path));
		return found != In.Events.end() ? found->second : 0u;
	}

	[[nodiscard]] SGuid GetRecordGuid (const SFileData& File) const noexcept
	{
		return File.Record.IsValid() && !File.Record.IsSubEntry() ? Tables.Assets.Guid[File.Record.Value] : SGuid {};
	}

	[[nodiscard]] bool IsWritableFile (MountId Mount, FStringView MountPath) const
	{
		if (Mount >= In.Writable.size() || In.Writable[Mount] == 0u)
		{
			return false;
		}
		const std::optional<std::filesystem::path> path = ToAbsolute(MountPath);
		const std::optional<platform::SFileStat> stat = path.has_value() ? platform::Stat(*path) : std::nullopt;
		return stat.has_value() && !stat->bDirectory && !stat->bReadOnly;
	}

	// The meta at Row as its file says (cached per stat): orphans and broken metas, whose GUID the registry does not hold.
	[[nodiscard]] const SMetaRead* ReadMeta (const SFileData& Meta)
	{
		const SStatKey stat = GetStatKey(Meta);
		const FString key(Meta.MountPath);
		if (const auto found = In.Caches->Metas.find(key); found != In.Caches->Metas.end() && found->second.Stat == stat)
		{
			return &found->second;
		}
		const std::optional<std::filesystem::path> path = ToAbsolute(Meta.MountPath);
		std::optional<SMetaRead> read = path.has_value() ? ReadMetaIdentity(*path, stat) : std::nullopt;
		if (!read.has_value())
		{
			return nullptr;
		}
		return &In.Caches->Metas.insert_or_assign(key, std::move(*read)).first->second;
	}

	void AddLive (const SGuid& Guid, FStringView MetaPath)
	{
		if (Guid.IsValid())
		{
			LiveGuids.push_back(SLiveGuid { Guid, FString(MetaPath) });
		}
	}

	void CheckCrLf (FStringView MetaPath, bool bCrLf)
	{
		Out.CrLfChecked.emplace_back(FString(MetaPath), bCrLf);
	}

	// ---- Facts every mount needs: one walk of FILES ----

	void Walk ()
	{
		const size_t mountCount = In.Mounts->GetMounts().size();
		SourcesByMount.resize(mountCount);
		OrphansByMount.resize(mountCount);
		View.ForEachFile([&] (uint32 Row)
		{
			SFileData file;
			if (!View.GetFile(Row, file) || file.Mount >= mountCount)
			{
				return;
			}
			if (file.Class == EFileClass::Meta)
			{
				WalkMeta(Row, file);
			}
			else if (GetsMeta(file.Class))
			{
				WalkSource(Row, file);
			}
		});
		std::map<SGuid, std::vector<uint32>> byGuid;
		for (uint32 i = 0u; i < LiveGuids.size(); ++i)
		{
			byGuid[LiveGuids[i].Guid].push_back(i);
		}
		for (const auto& [guid, indices] : byGuid)
		{
			if (indices.size() >= 2u)
			{
				Duplicates.insert(guid);
			}
		}
	}

	void WalkMeta (uint32 Row, const SFileData& Meta)
	{
		const std::optional<FStringView> source = GetMetaSource(Meta.MountPath);
		if (!source.has_value())
		{
			return;
		}
		if (const std::optional<uint32> sourceRow = View.FindFile(*source); sourceRow.has_value())
		{
			SFileData data;
			if (View.GetFile(*sourceRow, data) && data.Class != EFileClass::Meta)
			{
				return;   // paired: seen from its source's side; or beside a file that gets no meta (META007 is validate's)
			}
		}
		// No source row: an orphan, or the meta of a file the rules exclude, which still holds a live GUID on disk.
		const std::optional<std::filesystem::path> sourcePath = ToAbsolute(*source);
		const std::optional<platform::SFileStat> sourceStat = sourcePath.has_value() ? platform::Stat(*sourcePath) : std::nullopt;
		const bool bExcluded = sourceStat.has_value() && !sourceStat->bDirectory;
		if (const SMetaRead* read = ReadMeta(Meta); read != nullptr)
		{
			AddLive(read->Guid, Meta.MountPath);
			for (const SGuid& guid : read->Salvaged)
			{
				AddLive(guid, Meta.MountPath);
			}
			if (read->bCrLf || In.CrLfMetas.contains(FString(Meta.MountPath)))
			{
				CheckCrLf(Meta.MountPath, read->bCrLf);
			}
		}
		OrphansByMount[Meta.Mount].push_back(SOrphanRow { Row, bExcluded });
	}

	void WalkSource (uint32 Row, const SFileData& Source)
	{
		SSourceRow source;
		source.Row = Row;
		if (const std::optional<uint32> metaRow = View.FindFile(GetMetaPath(Source.MountPath)); metaRow.has_value())
		{
			SFileData meta;
			if (View.GetFile(*metaRow, meta) && meta.Class == EFileClass::Meta)
			{
				source.MetaRow = *metaRow;
				const SGuid guid = GetRecordGuid(Source);
				if (Source.MetaState == EMetaState::Broken || meta.MetaState == EMetaState::Broken)
				{
					if (const SMetaRead* read = ReadMeta(meta); read != nullptr)
					{
						for (const SGuid& salvaged : read->Salvaged)
						{
							AddLive(salvaged, meta.MountPath);
						}
						if (read->bCrLf || In.CrLfMetas.contains(FString(meta.MountPath)))
						{
							CheckCrLf(meta.MountPath, read->bCrLf);
						}
					}
				}
				else if (Source.MetaState == EMetaState::Paired)
				{
					AddLive(guid, meta.MountPath);
					const bool bAsked = In.bCheckAllCrLf || HasEvent(meta.MountPath) || In.CrLfMetas.contains(FString(meta.MountPath));
					if (bAsked && guid.IsValid())
					{
						const std::optional<SMetaCopyView> copy = View.FindMetaCopy(guid);
						if (copy.has_value() && copy->Hash == Source.MetaHash)
						{
							CheckCrLf(meta.MountPath, HasCrLf(copy->Bytes));
						}
					}
				}
			}
		}
		SourcesByMount[Source.Mount].push_back(source);
	}

	// ---- One mount: facts, RunLifecycle, fetches ----

	void RunMount (MountId MountIdValue, std::vector<SDiagnostic>& OutProblems)
	{
		const SMount* mount = FindMount(MountIdValue);
		if (mount == nullptr || MountIdValue >= SourcesByMount.size())
		{
			return;
		}
		const bool bWritableMount = MountIdValue < In.Writable.size() && In.Writable[MountIdValue] != 0u;
		std::vector<SLifecycleSource> sources;
		std::vector<SLifecycleMeta> metas;
		std::vector<SLifecycleTombstone> tombstones;
		std::vector<SGuid> verified;

		for (const SOrphanRow& orphan : OrphansByMount[MountIdValue])
		{
			SFileData file;
			if (orphan.bSourceExcluded || !View.GetFile(orphan.Row, file) || !IsInScope(file.MountPath, In.Paths))
			{
				continue;
			}
			const SMetaRead* read = ReadMeta(file);
			if (read == nullptr)
			{
				continue;   // changing while read: the event that follows runs this again
			}
			SLifecycleMeta meta;
			meta.MountPath = FString(file.MountPath);
			meta.SourcePath = FString(GetAssetOf(file.MountPath));
			meta.Parse = read->Parse;
			meta.Guid = read->Guid;
			meta.SalvagedGuids = read->Salvaged;
			meta.Identity = file.Identity;
			meta.FirstSeenMs = GetEventMs(file.MountPath);
			meta.bWritable = bWritableMount && IsWritableFile(MountIdValue, file.MountPath);
			metas.push_back(std::move(meta));
		}

		for (const SSourceRow& row : SourcesByMount[MountIdValue])
		{
			SFileData file;
			if (!View.GetFile(row.Row, file) || !IsInScope(file.MountPath, In.Paths))
			{
				continue;
			}
			SFileData meta;
			const bool bMetaOnDisk = row.MetaRow != InvalidRow && View.GetFile(row.MetaRow, meta);
			const FString exactMeta = GetMetaPath(file.MountPath);
			const bool bCaseDiffers = bMetaOnDisk && meta.MountPath != exactMeta.View();
			const SGuid recordGuid = file.MetaState != EMetaState::PendingMeta ? GetRecordGuid(file) : SGuid {};
			const bool bDirty = HasEvent(file.MountPath) || HasEvent(exactMeta) || (bMetaOnDisk && HasEvent(meta.MountPath));
			const bool bDuplicate = bMetaOnDisk && recordGuid.IsValid() && Duplicates.contains(recordGuid);
			const bool bBroken = file.MetaState == EMetaState::Broken || (bMetaOnDisk && meta.MetaState == EMetaState::Broken);
			if (file.MetaState == EMetaState::Paired && bMetaOnDisk && !bCaseDiffers && !bDirty && !bDuplicate && !bBroken)
			{
				continue;   // nothing a run could decide about it
			}

			if (bMetaOnDisk && (bBroken || bDirty || bDuplicate))
			{
				SLifecycleMeta entry;
				entry.MountPath = FString(meta.MountPath);
				entry.SourcePath = FString(file.MountPath);
				entry.Identity = meta.Identity;
				entry.FirstSeenMs = GetEventMs(meta.MountPath);
				entry.bWritable = bWritableMount && IsWritableFile(MountIdValue, meta.MountPath);
				if (bBroken || !recordGuid.IsValid())
				{
					const SMetaRead* read = ReadMeta(meta);
					if (read == nullptr)
					{
						continue;
					}
					entry.Parse = read->Parse;
					entry.Guid = read->Guid;
					entry.SalvagedGuids = read->Salvaged;
				}
				else
				{
					entry.Parse = EMetaParseState::Ok;
					entry.Guid = recordGuid;
				}
				metas.push_back(std::move(entry));
			}

			SLifecycleSource source;
			source.MountPath = FString(file.MountPath);
			source.Class = file.Class;
			source.Identity = file.Identity;
			source.RecordGuid = recordGuid;
			source.Stored = file.MetaState;
			source.bMetaOnDisk = bMetaOnDisk;
			source.bMetaCaseDiffers = bCaseDiffers;
			source.MetaSpelling = bCaseDiffers ? FString(meta.MountPath) : FString();
			if (file.ContentHash.IsValid())
			{
				source.Content = file.ContentHash;
			}
			else if (const auto cached = In.Caches->Hashes.find(source.MountPath);
				cached != In.Caches->Hashes.end() && cached->second.Stat == GetStatKey(file))
			{
				source.Content = cached->second.Hash;
				source.bLfsPointer = cached->second.bLfsPointer;
			}
			source.bWritable = bWritableMount && IsWritableFile(MountIdValue, file.MountPath)
				&& (!bMetaOnDisk || IsWritableFile(MountIdValue, meta.MountPath));
			source.LastEventMs = GetEventMs(file.MountPath);
			source.MetaGoneMs = !bMetaOnDisk && recordGuid.IsValid() ? GetEventMs(exactMeta) : 0u;
			if (recordGuid.IsValid() && file.MetaHash != 0u)
			{
				const std::optional<SMetaCopyView> copy = View.FindMetaCopy(recordGuid);
				if (copy.has_value() && copy->Hash == file.MetaHash)
				{
					verified.push_back(recordGuid);
				}
			}
			sources.push_back(std::move(source));
		}

		View.ForEachTombstone([&] (const STombstoneView& Tombstone)
		{
			FStringView mountName;
			FStringView relative;
			if (SplitMountPath(Tombstone.LastPath, mountName, relative) && mountName == mount->Name.View())
			{
				tombstones.push_back(SLifecycleTombstone { Tombstone.Guid, FString(Tombstone.LastPath), Tombstone.Hash, Tombstone.Identity,
					FileTimeToLifecycleMs(Tombstone.RemovedFileTime) });
			}
		});

		SLifecycleFacts facts;
		facts.NowMs = In.NowMs;
		facts.Safety = In.Rules->GetSafety(mount->Name);
		facts.Times = In.Lifecycle->Settings;
		facts.Salt = GetMountAssetSalt(*mount, In.Lifecycle->ProjectSalt);
		facts.bCommand = In.bCommand;
		facts.bVcsHold = In.bHold;
		facts.Tombstones = tombstones;
		facts.LiveGuids = LiveGuids;
		facts.VerifiedCopies = verified;
		std::vector<FString> committed;
		SLifecycleResult result;
		for (uint32 call = 0u; call < MaxLifecycleCalls; ++call)
		{
			facts.Sources = sources;
			facts.Metas = metas;
			result = RunLifecycle(facts);
			++Out.Runs;
			bool bAgain = false;
			if (!result.NeedContentHash.empty())
			{
				bAgain = FetchHashes(result.NeedContentHash, sources) || bAgain;
			}
			if (result.bNeedCommitted && !facts.Committed.has_value())
			{
				committed = FetchCommitted();
				facts.Committed = std::span<const FString>(committed);
				bAgain = true;
			}
			if (!bAgain)
			{
				break;
			}
		}

		Out.WakeAtMs = std::min(Out.WakeAtMs, result.WakeAtMs);
		Out.bPending = Out.bPending || (In.bHold && (!sources.empty() || !metas.empty()));
		for (SLifecycleAction& repair : result.Repairs)
		{
			const bool bKind = In.Kinds.empty() || std::find(In.Kinds.begin(), In.Kinds.end(), repair.Kind) != In.Kinds.end();
			if (bKind && !In.Refused.contains(MakeActionKey(repair)))
			{
				Out.Repairs.push_back(std::move(repair));
			}
		}
		for (SLifecycleAction& fixIt : result.FixIts)
		{
			Out.FixIts.push_back(std::move(fixIt));
		}
		for (SLifecycleState& state : result.States)
		{
			States.push_back(std::move(state));
		}
		for (SDiagnostic& problem : result.Problems)
		{
			OutProblems.push_back(std::move(problem));
		}
	}

	// Hashes what the lifecycle asked for; true when anything new was learned (a file that keeps changing is left for the
	// event that follows).
	bool FetchHashes (std::span<const FString> Paths, std::vector<SLifecycleSource>& Sources)
	{
		bool bLearned = false;
		for (const FString& path : Paths)
		{
			const auto source = std::find_if(Sources.begin(), Sources.end(), [&] (const SLifecycleSource& Source) { return Source.MountPath == path; });
			const std::optional<std::filesystem::path> absolute = ToAbsolute(path);
			if (source == Sources.end() || !absolute.has_value())
			{
				continue;
			}
			SHashed hashed = HashSource(*absolute);
			if (!hashed.Hash.has_value() && !hashed.bLfsPointer)
			{
				continue;
			}
			source->Content = hashed.Hash;
			source->bLfsPointer = hashed.bLfsPointer;
			In.Caches->Hashes.insert_or_assign(path, hashed);
			NewHashes.insert_or_assign(path, std::move(hashed));
			bLearned = true;
		}
		return bLearned;
	}

	// The metas of every duplicate group that the VCS has committed, asked of each mount's provider in one call.
	[[nodiscard]] std::vector<FString> FetchCommitted () const
	{
		std::map<MountId, std::vector<FString>> byMount;
		for (const SLiveGuid& live : LiveGuids)
		{
			FStringView mountName;
			FStringView relative;
			if (!Duplicates.contains(live.Guid) || !SplitMountPath(live.MetaPath, mountName, relative))
			{
				continue;
			}
			if (const SMount* mount = In.Mounts->Find(mountName); mount != nullptr)
			{
				byMount[mount->Id].push_back(live.MetaPath);
			}
		}
		std::vector<FString> committed;
		for (const auto& [mount, paths] : byMount)
		{
			IVcsProvider* provider = mount < In.Providers.size() ? In.Providers[mount] : nullptr;
			if (provider == nullptr)
			{
				continue;   // None: nothing is committed (duplicates resolve by smallest path)
			}
			std::vector<FString> files;
			for (const FString& path : paths)
			{
				const std::optional<std::filesystem::path> absolute = ToAbsolute(path);
				files.push_back(absolute.has_value() ? FString::FromPath(*absolute) : FString());
			}
			std::vector<bool> isCommitted;
			const SVcsResult result = provider->GetCommittedFacts(files, isCommitted);
			if (result.Error != EVcsError::None)
			{
				log::Warning(log::Assets, "the VCS gives no committed facts for duplicate GUIDs ({}): the smallest path wins", result.Message);
				continue;
			}
			for (size_t i = 0u; i < paths.size() && i < isCommitted.size(); ++i)
			{
				if (isCommitted[i])
				{
					committed.push_back(paths[i]);
				}
			}
		}
		return committed;
	}

	// ---- Problems: what this process remembers, files, Validation, and the diff against the view ----

	void AddMemoryProblems (std::vector<SDiagnostic>& Problems) const
	{
		const auto inRun = [&] (FStringView Subject)
		{
			FStringView mountName;
			FStringView relative;
			const SMount* mount = SplitMountPath(Subject, mountName, relative) ? In.Mounts->Find(mountName) : nullptr;
			return mount != nullptr && std::find(In.RunMounts.begin(), In.RunMounts.end(), mount->Id) != In.RunMounts.end()
				&& IsInScope(Subject, In.Paths);
		};
		// VCS001 for every meta known to hold CRLF: those this run looked at, and the others it did not look at again.
		FStringSet crLf = In.CrLfMetas;
		for (const auto& [path, bCrLf] : Out.CrLfChecked)
		{
			if (bCrLf)
			{
				crLf.insert(path);
			}
			else
			{
				crLf.erase(path);
			}
		}
		std::vector<FString> sorted(crLf.begin(), crLf.end());
		std::sort(sorted.begin(), sorted.end(), [] (const FString& A, const FString& B) { return A.View() < B.View(); });
		for (const FString& path : sorted)
		{
			if (inRun(path) && View.FindFileExact(path).has_value())
			{
				SDiagnostic problem;
				problem.Severity = ESeverity::Error;
				problem.Code = Vcs001;
				problem.Subject = path;
				problem.Message = "CRLF line endings in a meta, a hashed FRT text file: other machines hash other bytes. Give `*.meta` "
					"`text eol=lf` in .gitattributes and renormalize";
				Problems.push_back(std::move(problem));
			}
		}
		for (const SDiagnostic& problem : In.MemoryProblems)
		{
			if (inRun(problem.Subject))
			{
				Problems.push_back(problem);
			}
		}
	}

	void FinishProblems (std::vector<SDiagnostic>& Problems)
	{
		std::map<FString, std::vector<SDiagnostic>> lists;
		std::map<FString, std::shared_ptr<const SScanScope>> scopes;
		for (SDiagnostic& problem : Problems)
		{
			FStringView mountName;
			FStringView relative;
			const SMount* mount = SplitMountPath(problem.Subject, mountName, relative) ? In.Mounts->Find(mountName) : nullptr;
			if (mount == nullptr)
			{
				continue;
			}
			if (problem.File.IsEmpty())
			{
				problem.File = mount->GetSourceFile(relative);
			}
			const FString folder(GetMountPathParent(problem.Subject).IsEmpty() ? problem.Subject.View() : GetMountPathParent(problem.Subject));
			std::shared_ptr<const SScanScope>& scope = scopes[folder];
			if (scope == nullptr)
			{
				std::vector<SDiagnostic> ignored;
				scope = In.Rules->OpenFolderScope(folder, ignored);
			}
			if (scope != nullptr)
			{
				In.Rules->ApplyValidation(*scope, problem);
			}
			lists[problem.Subject].push_back(std::move(problem));
		}

		std::map<FString, std::vector<SDiagnostic>> stored;
		View.ForEachProblem([&] (FStringView Subject, EProblemSource Source, const SDiagnostic& Diagnostic)
		{
			if (Source != EProblemSource::Lifecycle)
			{
				return;
			}
			FStringView mountName;
			FStringView relative;
			const SMount* mount = SplitMountPath(Subject, mountName, relative) ? In.Mounts->Find(mountName) : nullptr;
			if (mount != nullptr && std::find(In.RunMounts.begin(), In.RunMounts.end(), mount->Id) != In.RunMounts.end()
				&& IsInScope(Subject, In.Paths))
			{
				stored[FString(Subject)].push_back(Diagnostic);
			}
		});
		for (const auto& [subject, list] : stored)
		{
			if (!lists.contains(subject))
			{
				Out.Delta.Problems.push_back(SProblemsSet { subject, EProblemSource::Lifecycle, {} });
			}
		}
		for (auto& [subject, list] : lists)
		{
			const auto old = stored.find(subject);
			if (old == stored.end() || old->second != list)
			{
				Out.Delta.Problems.push_back(SProblemsSet { subject, EProblemSource::Lifecycle, list });
			}
			for (SDiagnostic& problem : list)
			{
				Out.Problems.push_back(std::move(problem));
			}
		}
	}

	// The states that differ from the rows, and the hashes learned, as FILES upserts of the rows as this view holds them.
	void AddStates ()
	{
		TStringMap<SFileUpsert> upserts;
		const auto upsertOf = [&] (FStringView Path, uint32 Row, const SFileData& File) -> SFileUpsert&
		{
			auto [it, bNew] = upserts.try_emplace(FString(Path));
			if (bNew)
			{
				SFileUpsert& upsert = it->second;
				upsert.MountPath = FString(File.MountPath);
				upsert.Size = File.Size;
				upsert.MTime = File.MTime;
				upsert.Identity = File.Identity;
				upsert.Class = Tables.Files.BaseClass[Row];
			}
			return it->second;
		};
		for (const SLifecycleState& state : States)
		{
			const std::optional<uint32> row = View.FindFileExact(state.MountPath);
			SFileData file;
			if (!row.has_value() || !View.GetFile(*row, file) || file.MetaState == state.State)
			{
				continue;
			}
			upsertOf(state.MountPath, *row, file).Meta = SFileMetaState { state.State, file.MetaHash };
		}
		for (const auto& [path, hashed] : NewHashes)
		{
			const std::optional<uint32> row = View.FindFileExact(path);
			SFileData file;
			if (!hashed.Hash.has_value() || !row.has_value() || !View.GetFile(*row, file) || file.ContentHash.IsValid()
				|| !(GetStatKey(file) == hashed.Stat))
			{
				continue;
			}
			upsertOf(path, *row, file).ContentHash = *hashed.Hash;
		}
		std::vector<FString> order;
		for (const auto& [path, upsert] : upserts)
		{
			order.push_back(path);
		}
		std::sort(order.begin(), order.end(), [] (const FString& A, const FString& B) { return A.View() < B.View(); });
		for (const FString& path : order)
		{
			Out.Delta.FileUpserts.push_back(std::move(upserts.at(path)));
		}
	}

	// ---- Planning ----

	void Plan (std::span<const SLifecycleAction> Actions)
	{
		if (In.Writer == nullptr)
		{
			return;
		}
		std::vector<SLifecycleAction> reGuids;
		for (const SLifecycleAction& action : Actions)
		{
			if (action.Kind == ELifecycleAction::ReGuid)
			{
				reGuids.push_back(action);
				continue;
			}
			PlanOne(std::span<const SLifecycleAction>(&action, 1u));
		}
		if (!reGuids.empty())
		{
			PlanOne(reGuids);
		}
	}

	void PlanOne (std::span<const SLifecycleAction> Actions)
	{
		SPlannedOp planned;
		if (PlanRepairOp(*In.Writer, View, Actions, planned.Op, Out.PlanProblems) && !planned.Op.Steps.empty())
		{
			planned.Actions.assign(Actions.begin(), Actions.end());
			Out.Ops.push_back(std::move(planned));
		}
	}
};

struct SOpResult
{
	std::vector<SLifecycleAction> Actions;
	SAssetCommandResult Result;
};

struct SOpsOutput
{
	std::vector<SOpResult> Results;
	std::vector<SDiagnostic> Untracked;   // VCS003
	std::vector<FString> Touched;         // mount paths whose folders a rescan must read again
};

// The ops of one proposal set, in order, on a worker; then what StatusOf says of the metas they created (VCS003).
[[nodiscard]] SOpsOutput RunOps (CAssetWriter& Writer, std::vector<SPlannedOp>&& Ops)
{
	SOpsOutput out;
	for (SPlannedOp& planned : Ops)
	{
		SOpResult result;
		result.Actions = std::move(planned.Actions);
		result.Result = RunAssetOp(Writer, std::move(planned.Op));
		if (result.Result.Report.bChangedDisk)
		{
			for (const SAssetOpStep& step : result.Result.Op.Steps)
			{
				out.Touched.push_back(step.Path);
				if (!step.ToPath.IsEmpty())
				{
					out.Touched.push_back(step.ToPath);
				}
			}
		}
		const bool bCreated = result.Result.Op.Verb == FStringView("create-meta") || result.Result.Op.Verb == FStringView("restore-meta");
		if (result.Result.Vcs.Error == EVcsError::None && bCreated && Writer.GetMounts() != nullptr)
		{
			for (const SAssetOpStep& step : result.Result.Op.Steps)
			{
				const std::optional<FStringView> source = GetMetaSource(step.Path);
				IVcsProvider* provider = source.has_value() ? Writer.FindProvider(*source) : nullptr;
				const std::optional<std::filesystem::path> sourcePath = source.has_value() ? Writer.GetMounts()->FromMountPath(*source) : std::nullopt;
				const std::optional<std::filesystem::path> metaPath = Writer.GetMounts()->FromMountPath(step.Path);
				if (provider == nullptr || !sourcePath.has_value() || !metaPath.has_value())
				{
					continue;
				}
				const std::vector<FString> files = { FString::FromPath(*sourcePath), FString::FromPath(*metaPath) };
				std::vector<SVcsStatus> status;
				if (provider->StatusOf(files, status).Error != EVcsError::None || status.size() != 2u)
				{
					continue;
				}
				if (status[0].State != EVcsState::Untracked && status[1].State == EVcsState::Untracked)
				{
					SDiagnostic problem;
					problem.Severity = ESeverity::Warning;
					problem.Code = Vcs003;
					problem.Subject = FString(*source);
					const FString command = provider->GetAddCommand(files[1]);
					problem.Message = command.IsEmpty() ? FString("generated locally: commit it. Its source is tracked and its new meta is not")
						: FString::Format("generated locally: commit it. Its source is tracked and its new meta is not ({})", command);
					out.Untracked.push_back(std::move(problem));
				}
			}
		}
		out.Results.push_back(std::move(result));
	}
	return out;
}
}


uint64 GetLifecycleNowMs () noexcept
{
	return FileTimeToLifecycleMs(GetFileTimeNow());
}


struct CAssetRepairs::SImpl
{
	explicit SImpl (SAssetRepairsDesc InDesc)
		: Desc(std::move(InDesc))
		, Lifecycle(*Desc.Lifecycle)
		, Caches(std::make_shared<SCaches>())
	{
		const size_t mountCount = Desc.Mounts->GetMounts().size();
		Providers.assign(mountCount, nullptr);
		Settled.assign(mountCount, uint8(Lifecycle.bProvidersPending ? 0u : 1u));
		Writable.assign(mountCount, uint8(0u));
		for (const auto& [mount, provider] : Lifecycle.Providers)
		{
			if (mount < mountCount)
			{
				Providers[mount] = provider;
				Settled[mount] = 1u;
			}
		}
		bProvidersPending = std::any_of(Settled.begin(), Settled.end(), [] (uint8 Value) { return Value == 0u; });
		if (Desc.bWriter && Lifecycle.bRepairs)
		{
			for (const MountId mount : Desc.OwnedMounts)
			{
				if (mount < mountCount)
				{
					Writable[mount] = 1u;
				}
			}
		}
		MakeWriter();
		if (Desc.Watcher != nullptr && Lifecycle.Settings.bWaitForVcs)
		{
			// The watcher's hold (edit [56]): the narrower question, any mount's tree being rewritten, so an open merge
			// holds repairs but never hot reload. Unset with WaitForVcs off: settle windows alone.
			Desc.Watcher->SetVcsBusyQuery([this] () { return IsAnyRewritingTree(); });
			bBusyQuerySet = true;
		}
	}

	~SImpl ()
	{
		ClearBusyQuery();
	}

	void ClearBusyQuery ()
	{
		if (bBusyQuerySet && Desc.Watcher != nullptr)
		{
			Desc.Watcher->SetVcsBusyQuery(nullptr);
			bBusyQuerySet = false;
		}
	}

	[[nodiscard]] bool HasWriter () const noexcept
	{
		return std::any_of(Writable.begin(), Writable.end(), [] (uint8 Value) { return Value != 0u; });
	}

	void MakeWriter ()
	{
		if (!HasWriter())
		{
			return;
		}
		SAssetWriterDesc desc;
		desc.Mounts = Desc.Mounts;
		desc.OwnerRoot = Desc.OwnerRoot;
		desc.bHoldsLock = Desc.bWriter;
		for (const MountId mount : Desc.OwnedMounts)
		{
			if (mount < Providers.size() && Providers[mount] != nullptr)
			{
				desc.Providers.emplace_back(mount, Providers[mount]);
			}
		}
		Writer = std::make_unique<CAssetWriter>(std::move(desc));
	}

	// ---- The VCS hold ----

	[[nodiscard]] bool IsAnyRewritingTree () const
	{
		for (size_t i = 0u; i < Providers.size(); ++i)
		{
			if (Settled[i] != 0u && Providers[i] != nullptr && Providers[i]->IsRewritingTree())
			{
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] bool IsAnyBusy () const
	{
		for (size_t i = 0u; i < Providers.size(); ++i)
		{
			if (Settled[i] != 0u && Providers[i] != nullptr && Providers[i]->IsBusy())
			{
				return true;
			}
		}
		return false;
	}

	// Pending providers hold everything; with WaitForVcs, so does a busy provider, and VcsSettleMs after it.
	[[nodiscard]] bool ComputeHold (uint64 NowMs)
	{
		if (bProvidersPending)
		{
			return true;
		}
		if (!Lifecycle.Settings.bWaitForVcs)
		{
			return false;
		}
		if (LastBusyPollMs == 0u || GetElapsedMs(NowMs, LastBusyPollMs) >= BusyPollMs || NowMs < LastBusyPollMs)
		{
			LastBusyPollMs = NowMs;
			bLastBusy = IsAnyBusy();
			if (bLastBusy)
			{
				LastBusyMs = NowMs;
			}
		}
		return bLastBusy || (LastBusyMs != 0u && GetElapsedMs(NowMs, LastBusyMs) < Lifecycle.Settings.VcsSettleMs);
	}

	// ---- Memory ----

	void StampEvents (uint64 NowMs)
	{
		if (PendingEvents.empty())
		{
			return;
		}
		for (const FString& path : PendingEvents)
		{
			Events.insert_or_assign(path, NowMs);
			// An event on a path ends a refusal there: the repair is proposed again.
			std::erase_if(RefusedActions, [&] (const SLifecycleAction& Action) { return Concerns(Action, path); });
			Refusals.erase(path);
			Refusals.erase(FString(GetAssetOf(path)));
		}
		PendingEvents.clear();
	}

	void PruneEvents (uint64 NowMs)
	{
		const SAssetEditorSettings& times = Lifecycle.Settings;
		const uint64 keep = uint64(std::max({ times.MetaCreationDelayMs, times.RemoveGraceMs, times.OrphanPairWindowMs })) + 1000u;
		std::erase_if(Events, [&] (const auto& Entry) { return GetElapsedMs(NowMs, Entry.second) > keep; });
	}

	[[nodiscard]] std::vector<MountId> GetAllMounts () const
	{
		std::vector<MountId> mounts;
		for (const SMount& mount : Desc.Mounts->GetMounts())
		{
			mounts.push_back(mount.Id);
		}
		return mounts;
	}

	[[nodiscard]] std::shared_ptr<SCycleInput> MakeInput (uint64 NowMs, bool bCommand, bool bHold, bool bPlan)
	{
		auto input = std::make_shared<SCycleInput>();
		input->View = Desc.Host->AcquireView();
		input->Mounts = Desc.Mounts;
		input->Rules = Desc.Rules;
		input->Lifecycle = &Lifecycle;
		input->Caches = Caches;
		input->Providers = Providers;
		for (size_t i = 0u; i < Settled.size(); ++i)
		{
			if (Settled[i] == 0u)
			{
				input->Providers[i] = nullptr;
			}
		}
		input->Writable = Writable;
		input->RunMounts = GetAllMounts();
		input->Events = Events;
		for (const auto& [subject, problem] : Refusals)
		{
			input->MemoryProblems.push_back(problem);
		}
		for (const auto& [subject, problem] : Untracked)
		{
			input->MemoryProblems.push_back(problem);
		}
		input->CrLfMetas = CrLfMetas;
		for (const SLifecycleAction& action : RefusedActions)
		{
			input->Refused.insert(MakeActionKey(action));
		}
		input->Writer = bPlan && Writer != nullptr ? Writer.get() : nullptr;
		input->NowMs = NowMs;
		input->bCommand = bCommand;
		input->bHold = bHold;
		input->bCheckAllCrLf = !bCheckedAllCrLf;
		return input;
	}

	// ---- Results, on the owner thread ----

	void ApplyCycle (SCycleOutput& Output, std::span<const FString> Paths)
	{
		bCheckedAllCrLf = true;
		for (const auto& [path, bCrLf] : Output.CrLfChecked)
		{
			if (bCrLf)
			{
				CrLfMetas.insert(path);
			}
			else
			{
				CrLfMetas.erase(path);
			}
		}
		// Kept problems of files that are gone go with them.
		const CRegistryView& live = Desc.Host->GetLiveView();
		std::erase_if(Untracked, [&] (const auto& Entry) { return !live.FindFileExact(Entry.first).has_value(); });
		std::erase_if(CrLfMetas, [&] (const FString& Path) { return !live.FindFileExact(Path).has_value(); });

		if (!Output.Delta.IsEmpty())
		{
			Desc.Host->CommitNow(std::move(Output.Delta));
		}
		std::erase_if(FixIts, [&] (const SLifecycleAction& Action) { return IsInScope(Action.Path, Paths); });
		for (SLifecycleAction& fixIt : Output.FixIts)
		{
			FixIts.push_back(std::move(fixIt));
		}
		WakeAtMs = Output.WakeAtMs;
		bLastHeldPending = Output.bHeld && Output.bPending;
		for (const SDiagnostic& problem : Output.PlanProblems)
		{
			log::Info(log::Assets, "repair not planned: {}: {}", problem.Subject, problem.Message);
		}
		++Runs;
	}

	// Each op's outcome: counted, a refusal kept as a proposal (with its VCS002), VCS003 kept for its source.
	void ApplyOps (SOpsOutput& Output, uint32& OutApplied, uint32& OutRefused)
	{
		OutApplied = 0u;
		OutRefused = 0u;
		for (SOpResult& result : Output.Results)
		{
			const uint32 count = static_cast<uint32>(result.Actions.size());
			if (result.Result.Vcs.Error == EVcsError::None)
			{
				OutApplied += count;
				continue;
			}
			OutRefused += count;
			for (SLifecycleAction& action : result.Actions)
			{
				RefusedActions.push_back(std::move(action));
			}
			for (SDiagnostic& problem : result.Result.Problems)
			{
				if (!problem.Subject.IsEmpty())
				{
					Refusals.insert_or_assign(problem.Subject, std::move(problem));
				}
			}
		}
		for (SDiagnostic& problem : Output.Untracked)
		{
			Untracked.insert_or_assign(problem.Subject, std::move(problem));
		}
		Desc.Host->CountRepairs(OutApplied, OutRefused);
		bDirty = true;
	}

	void ExpectWrites (std::span<const SPlannedOp> Ops) const
	{
		if (Desc.Watcher == nullptr || !Desc.Watcher->IsEnabled())
		{
			return;
		}
		for (const SPlannedOp& planned : Ops)
		{
			ForEachExpectedWrite(planned.Op, [&] (FStringView MountPath, const hash::SContentHash& Hash, uint64 Size)
			{
				if (const std::optional<std::filesystem::path> path = Desc.Mounts->FromMountPath(MountPath); path.has_value())
				{
					Desc.Watcher->ExpectWrite(*path, Hash, Size, platform::MakeDeadline(10.0));
				}
			});
		}
	}

	// ---- Jobs ----

	// Live: a run on a worker; its result is applied when it arrives, if the tree is still the one it read.
	void StartCycle (std::shared_ptr<SCycleInput> Input)
	{
		bDirty = false;
		bWorking = true;
		auto output = std::make_shared<SCycleOutput>();
		Desc.Pool->Submit(jobs::EWorkPriority::Normal, jobs::NoCancelGroup,
			[Input, output] () { *output = CCycle(*Input).Run(); },
			[this, output, bFix = !Input->FixActions.empty()] ()
			{
				bWorking = false;
				if (bStopping)
				{
					return;
				}
				if (bFix)
				{
					StartOps(std::move(output->Ops));
					return;
				}
				if (!Desc.Host->IsQuiet() || Desc.Host->GetLiveView().GetRevision() != output->Revision)
				{
					bDirty = true;   // the tree moved while it ran: run again on the final one
					return;
				}
				ApplyCycle(*output, {});
				if (!output->Ops.empty() && !output->bHeld)
				{
					StartOps(std::move(output->Ops));
				}
			});
	}

	void StartOps (std::vector<SPlannedOp>&& Ops)
	{
		if (Ops.empty() || Writer == nullptr || bStopping)
		{
			return;
		}
		// Asked once more right before the writes start (edit [30]).
		if (Lifecycle.Settings.bWaitForVcs && IsAnyBusy())
		{
			LastBusyMs = LastNowMs;
			bLastHeldPending = true;
			bDirty = true;
			return;
		}
		ExpectWrites(Ops);
		bWorking = true;
		auto output = std::make_shared<SOpsOutput>();
		auto ops = std::make_shared<std::vector<SPlannedOp>>(std::move(Ops));
		Desc.Pool->Submit(jobs::EWorkPriority::Normal, jobs::NoCancelGroup,
			[ops, output, writer = Writer.get()] () { *output = RunOps(*writer, std::move(*ops)); },
			[this, output] ()
			{
				bWorking = false;
				uint32 applied = 0u;
				uint32 refused = 0u;
				ApplyOps(*output, applied, refused);
				if (!bStopping && !output->Touched.empty())
				{
					Desc.Host->RequestRescan(output->Touched);
				}
				MaybeRebuildWriter();
			});
	}

	// Blocking forms, for RunNow and a tool host's ApplyFix: the same jobs, waited for through the registry's pump loop.
	[[nodiscard]] SCycleOutput RunCycleNow (std::shared_ptr<SCycleInput> Input)
	{
		auto output = std::make_shared<SCycleOutput>();
		bool bDone = false;
		bWorking = true;
		Desc.Pool->Submit(jobs::EWorkPriority::High, jobs::NoCancelGroup,
			[Input, output] () { *output = CCycle(*Input).Run(); },
			[&bDone] () { bDone = true; });
		(void)Desc.Host->WaitUntil([&bDone] { return bDone; }, -1.0);
		bWorking = false;
		return std::move(*output);
	}

	[[nodiscard]] SOpsOutput RunOpsNow (std::vector<SPlannedOp>&& Ops)
	{
		ExpectWrites(Ops);
		auto output = std::make_shared<SOpsOutput>();
		auto ops = std::make_shared<std::vector<SPlannedOp>>(std::move(Ops));
		bool bDone = false;
		bWorking = true;
		Desc.Pool->Submit(jobs::EWorkPriority::High, jobs::NoCancelGroup,
			[ops, output, writer = Writer.get()] () { *output = RunOps(*writer, std::move(*ops)); },
			[&bDone] () { bDone = true; });
		(void)Desc.Host->WaitUntil([&bDone] { return bDone; }, -1.0);
		bWorking = false;
		return std::move(*output);
	}

	void MaybeRebuildWriter ()
	{
		if (bRebuildWriter && !bWorking)
		{
			bRebuildWriter = false;
			MakeWriter();
		}
	}

	[[nodiscard]] bool IsDue (uint64 NowMs)
	{
		if (bDirty || !PendingEvents.empty() || !PendingFixes.empty() || (WakeAtMs != ~0ull && NowMs >= WakeAtMs))
		{
			return true;
		}
		return bLastHeldPending && !ComputeHold(NowMs);
	}

	[[nodiscard]] std::vector<SLifecycleAction> ExpandFix (const SLifecycleAction& FixIt) const
	{
		std::vector<SLifecycleAction> actions = { FixIt };
		if (FixIt.Kind != ELifecycleAction::ReGuid)
		{
			return actions;
		}
		// The losers of one batch are rewritten as a set (LifecycleTypes.h): every ReGuid fix-it that shares an old GUID.
		std::set<SGuid> olds;
		for (const auto& [from, to] : FixIt.Remap)
		{
			olds.insert(from);
		}
		for (const SLifecycleAction& other : FixIts)
		{
			if (other.Kind != ELifecycleAction::ReGuid || MakeActionKey(other) == MakeActionKey(FixIt))
			{
				continue;
			}
			const bool bShares = std::any_of(other.Remap.begin(), other.Remap.end(), [&] (const auto& Pair) { return olds.contains(Pair.first); });
			if (bShares)
			{
				actions.push_back(other);
			}
		}
		return actions;
	}

	SAssetRepairsDesc Desc;
	const SLifecycleDesc& Lifecycle;
	std::shared_ptr<SCaches> Caches;
	std::vector<IVcsProvider*> Providers;
	std::vector<uint8> Settled;
	std::vector<uint8> Writable;
	bool bProvidersPending = false;
	std::unique_ptr<CAssetWriter> Writer;
	bool bRebuildWriter = false;
	bool bBusyQuerySet = false;

	bool bInitialComplete = false;
	bool bStopping = false;
	bool bWorking = false;
	bool bInCommand = false;      // RunNow or a blocking ApplyFix: the registry's pump inside it must not start a live run

	bool bDirty = false;
	bool bLastHeldPending = false;
	bool bCheckedAllCrLf = false;
	uint64 WakeAtMs = ~0ull;
	uint64 LastNowMs = 0u;
	uint64 LastBusyPollMs = 0u;
	uint64 LastBusyMs = 0u;
	bool bLastBusy = false;
	uint64 Runs = 0u;

	std::vector<FString> PendingEvents;
	TStringMap<uint64> Events;
	std::vector<SLifecycleAction> FixIts;
	std::vector<SLifecycleAction> RefusedActions;
	TStringMap<SDiagnostic> Refusals;      // VCS002 by subject
	TStringMap<SDiagnostic> Untracked;     // VCS003 by subject
	FStringSet CrLfMetas;
	std::vector<std::vector<SLifecycleAction>> PendingFixes;
};


CAssetRepairs::CAssetRepairs (SAssetRepairsDesc Desc)
{
	frt_assert(Desc.Lifecycle != nullptr && Desc.Mounts != nullptr && Desc.Rules != nullptr && Desc.Pool != nullptr && Desc.Host != nullptr);
	Impl = std::make_unique<SImpl>(std::move(Desc));
}

CAssetRepairs::~CAssetRepairs () = default;

void CAssetRepairs::Start ()
{
	CAssetWriter* writer = Impl->Writer.get();
	if (writer == nullptr || !writer->GetUnfinished().has_value())
	{
		return;
	}
	FString error;
	if (writer->RollForward(&error))
	{
		log::Info(log::Assets, "finished an operation a previous run left unfinished ({})", FString::FromPath(writer->GetJournalPath()));
	}
	else
	{
		log::Warning(log::Assets, "an unfinished operation in {} could not be finished: {}", FString::FromPath(writer->GetJournalPath()), error);
	}
}

void CAssetRepairs::OnInitialScanComplete ()
{
	Impl->bInitialComplete = true;
	Impl->bDirty = true;
}

void CAssetRepairs::ShutDown ()
{
	Impl->bStopping = true;
	Impl->ClearBusyQuery();
}

void CAssetRepairs::SetProvider (MountId Mount, IVcsProvider* Provider)
{
	SImpl& impl = *Impl;
	if (Mount >= impl.Providers.size())
	{
		return;
	}
	impl.Providers[Mount] = Provider;
	impl.Settled[Mount] = 1u;
	const bool bPending = std::any_of(impl.Settled.begin(), impl.Settled.end(), [] (uint8 Value) { return Value == 0u; });
	if (impl.bProvidersPending && !bPending)
	{
		impl.bProvidersPending = false;
		impl.bRebuildWriter = true;
		impl.MaybeRebuildWriter();
		impl.bDirty = true;
	}
}

void CAssetRepairs::OnPassCommitted (std::span<const FString> Dirty)
{
	if (Dirty.empty())
	{
		return;
	}
	Impl->PendingEvents.insert(Impl->PendingEvents.end(), Dirty.begin(), Dirty.end());
	Impl->bDirty = true;
}

void CAssetRepairs::Tick (uint64 NowMs)
{
	SImpl& impl = *Impl;
	if (impl.bStopping)
	{
		return;
	}
	impl.LastNowMs = NowMs;
	impl.StampEvents(NowMs);
	if (!impl.bInitialComplete || impl.bWorking || impl.bInCommand)
	{
		return;
	}
	if (!impl.PendingFixes.empty())
	{
		if (impl.Desc.Host->IsQuiet() && impl.Writer != nullptr && !impl.bProvidersPending)
		{
			std::shared_ptr<SCycleInput> input = impl.MakeInput(NowMs, true, false, true);
			input->FixActions = std::move(impl.PendingFixes.front());
			impl.PendingFixes.erase(impl.PendingFixes.begin());
			impl.StartCycle(std::move(input));
		}
		return;
	}
	const bool bWake = impl.WakeAtMs != ~0ull && NowMs >= impl.WakeAtMs;
	if (!impl.bDirty && !bWake && !impl.bLastHeldPending)
	{
		return;
	}
	const bool bHold = impl.ComputeHold(NowMs);
	if (!impl.bDirty && !bWake && bHold)
	{
		return;   // held, as the last run was: wait for the hold to end
	}
	if (!impl.Desc.Host->IsQuiet())
	{
		impl.bDirty = true;
		return;
	}
	impl.PruneEvents(NowMs);
	impl.StartCycle(impl.MakeInput(NowMs, false, bHold, true));
}

bool CAssetRepairs::Settle (uint64 NowMs, double TimeoutSeconds)
{
	SImpl& impl = *Impl;
	const uint64 deadline = TimeoutSeconds >= 0.0 ? platform::MakeDeadline(TimeoutSeconds) : ~0ull;
	const auto remaining = [&] { return deadline == ~0ull ? -1.0 : std::max(0.0, platform::TimestampToSeconds(deadline - std::min(deadline, platform::ReadTimestamp()))); };
	for (uint32 round = 0u; round < 64u; ++round)
	{
		Tick(NowMs);
		if (!impl.Desc.Host->WaitUntil([&impl] { return !impl.bWorking && impl.Desc.Host->IsQuiet(); }, remaining()))
		{
			return false;
		}
		if (!impl.IsDue(NowMs) || impl.bStopping)
		{
			return true;
		}
		if (impl.bLastHeldPending && impl.ComputeHold(NowMs) && !impl.bDirty && impl.PendingEvents.empty())
		{
			return true;
		}
	}
	return false;
}

SRepairReport CAssetRepairs::RunNow (const SRepairOptions& Options)
{
	SImpl& impl = *Impl;
	SRepairReport report;
	if (impl.bStopping || !impl.Desc.Host->WaitUntil([&impl] { return !impl.bWorking && impl.Desc.Host->IsQuiet(); }, -1.0))
	{
		return report;
	}
	const uint64 now = GetLifecycleNowMs();
	impl.StampEvents(now);
	impl.bInCommand = true;
	// Every mount the run owns, or the mounts of the paths asked for; a reader's run covers every mount.
	std::vector<MountId> mounts;
	for (const FString& path : Options.Paths)
	{
		FStringView mountName;
		FStringView relative;
		const SMount* mount = SplitMountPath(path, mountName, relative) ? impl.Desc.Mounts->Find(mountName) : nullptr;
		if (mount != nullptr && std::find(mounts.begin(), mounts.end(), mount->Id) == mounts.end())
		{
			mounts.push_back(mount->Id);
		}
	}
	if (Options.Paths.empty())
	{
		mounts = impl.HasWriter() ? impl.Desc.OwnedMounts : impl.GetAllMounts();
	}
	const bool bHold = Options.bCommand ? false : impl.ComputeHold(now);
	for (uint32 round = 0u; round < MaxCommandRounds; ++round)
	{
		std::shared_ptr<SCycleInput> input = impl.MakeInput(now, Options.bCommand, bHold, !Options.bDryRun);
		input->RunMounts = mounts;
		input->Paths = Options.Paths;
		input->Kinds = Options.Kinds;
		SCycleOutput output = impl.RunCycleNow(input);
		impl.ApplyCycle(output, Options.Paths);
		report.Planned.insert(report.Planned.end(), output.Repairs.begin(), output.Repairs.end());
		report.Problems = output.Problems;
		report.Problems.insert(report.Problems.end(), output.PlanProblems.begin(), output.PlanProblems.end());
		if (Options.bDryRun || output.Ops.empty() || impl.Writer == nullptr || bHold)
		{
			break;
		}
		SOpsOutput ops = impl.RunOpsNow(std::move(output.Ops));
		uint32 applied = 0u;
		uint32 refused = 0u;
		impl.ApplyOps(ops, applied, refused);
		report.Applied += applied;
		report.Refused += refused;
		if (!ops.Touched.empty())
		{
			(void)impl.Desc.Host->ScanNow(ops.Touched);
		}
	}
	impl.bDirty = false;
	impl.bInCommand = false;
	return report;
}

std::vector<SLifecycleAction> CAssetRepairs::GetFixIts (const SDiagnostic& Problem) const
{
	const SImpl& impl = *Impl;
	std::vector<SLifecycleAction> found;
	const auto take = [&] (std::span<const SLifecycleAction> Actions, std::initializer_list<ELifecycleAction> Kinds)
	{
		for (const SLifecycleAction& action : Actions)
		{
			if (std::find(Kinds.begin(), Kinds.end(), action.Kind) != Kinds.end() && Concerns(action, Problem.Subject))
			{
				found.push_back(action);
			}
		}
	};
	using enum ELifecycleAction;
	switch (Problem.Code)
	{
	case Meta001: take(impl.FixIts, { CreateMeta }); break;
	case Meta003: take(impl.FixIts, { ReGuid }); break;
	case Meta004: take(impl.FixIts, { KeepAliasB }); break;
	case Meta006: take(impl.FixIts, { FixMetaCase }); break;
	case Meta008: take(impl.FixIts, { RestoreMeta, CreateMeta, MoveMeta }); break;
	case Meta009: take(impl.FixIts, { MoveMeta }); break;
	case Vcs002: take(impl.RefusedActions, { CreateMeta, RestoreMeta, MoveMeta, FixMetaCase, ReGuid, KeepAliasB }); break;
	default: break;
	}
	return found;
}

SRepairReport CAssetRepairs::ApplyFix (const SLifecycleAction& FixIt)
{
	SImpl& impl = *Impl;
	SRepairReport report;
	std::vector<SLifecycleAction> actions = impl.ExpandFix(FixIt);
	for (const SLifecycleAction& action : actions)
	{
		std::erase_if(impl.RefusedActions, [&] (const SLifecycleAction& Refused) { return MakeActionKey(Refused) == MakeActionKey(action); });
		impl.Refusals.erase(FString(GetAssetOf(action.Path)));
		impl.Refusals.erase(action.Path);
	}
	report.Planned = actions;
	if (impl.Writer == nullptr || impl.bStopping)
	{
		return report;
	}
	if (impl.Lifecycle.bLive)
	{
		impl.PendingFixes.push_back(std::move(actions));
		return report;
	}
	if (!impl.Desc.Host->WaitUntil([&impl] { return !impl.bWorking && impl.Desc.Host->IsQuiet(); }, -1.0))
	{
		return report;
	}
	const uint64 now = GetLifecycleNowMs();
	impl.bInCommand = true;
	std::shared_ptr<SCycleInput> input = impl.MakeInput(now, true, false, true);
	input->FixActions = std::move(actions);
	SCycleOutput planned = impl.RunCycleNow(input);
	report.Problems = planned.PlanProblems;
	if (planned.Ops.empty())
	{
		impl.bInCommand = false;
		return report;
	}
	SOpsOutput ops = impl.RunOpsNow(std::move(planned.Ops));
	impl.ApplyOps(ops, report.Applied, report.Refused);
	if (!ops.Touched.empty())
	{
		(void)impl.Desc.Host->ScanNow(ops.Touched);
	}
	// The registry's problems as they stand after the fix, through a run that repairs nothing.
	std::shared_ptr<SCycleInput> after = impl.MakeInput(now, true, false, false);
	SCycleOutput output = impl.RunCycleNow(after);
	impl.ApplyCycle(output, {});
	impl.bDirty = false;
	impl.bInCommand = false;
	for (SDiagnostic& problem : output.Problems)
	{
		report.Problems.push_back(std::move(problem));
	}
	return report;
}

bool CAssetRepairs::IsWorking () const noexcept
{
	return Impl->bWorking;
}

CAssetWriter* CAssetRepairs::GetWriter () noexcept
{
	return Impl->Writer.get();
}

IVcsProvider* CAssetRepairs::GetProvider (MountId Mount) const noexcept
{
	return Mount < Impl->Providers.size() && Impl->Settled[Mount] != 0u ? Impl->Providers[Mount] : nullptr;
}


SOwnership TakeOwnership (const std::filesystem::path& Root, const CMountTable& Mounts, std::span<const std::pair<MountId, IVcsProvider*>> Providers)
{
	SOwnership out;
	FString owner;
	FString error;
	out.Lock = TryTakeOwnerLock(Root, &owner, &error);
	if (!out.Lock.has_value())
	{
		out.Owner = std::move(owner);
		out.Error = std::move(error);
		SOpJournalContents contents;
		out.bUnfinished = ReadOpJournal(GetOpJournalPath(Root), contents) && contents.Unfinished.has_value();
		return out;
	}
	SAssetWriterDesc desc;
	desc.Mounts = &Mounts;
	desc.OwnerRoot = Root;
	desc.Providers.assign(Providers.begin(), Providers.end());
	desc.bHoldsLock = true;
	CAssetWriter writer(std::move(desc));
	if (!writer.GetUnfinished().has_value())
	{
		return out;
	}
	FString rollError;
	if (writer.RollForward(&rollError))
	{
		out.bRolledForward = true;
		log::Info(log::Assets, "finished an operation a previous run left unfinished ({})", FString::FromPath(writer.GetJournalPath()));
	}
	else
	{
		out.bUnfinished = true;
		out.Error = std::move(rollError);
	}
	return out;
}
}
