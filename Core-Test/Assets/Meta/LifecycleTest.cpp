// The meta lifecycle as a pure state machine (Assets/Meta/Lifecycle.h, design §4.6, §4.7, §4.8): every transition, one by
// one, with each default-on switch on and then off (AutoRepairMetas, DerivedAutoGuids, AutoResolveDuplicates,
// OrphanPairing; WaitForVcs is the caller's bVcsHold), giving exactly the "when off" row and never a fresh identity for a
// known asset; commands ignore AutoRepairMetas only; the facts' order changes nothing; applying a result's
// repairs yields no further repair, which is the guard against a repair loop; derived GUIDs are the frozen derivations
// and re-roll over live ones; a run asks for hashes and committed facts instead of guessing; a slow checkout never gets a
// meta created; a folder copied twice references its own copies. Pure: no files, no clock.
#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Meta/Lifecycle.h"
#include "Assets/Meta/MetaPaths.h"
#include "Containers/String.h"
#include "Hash/StableHash.h"

using namespace frt;
using namespace frt::assets;


namespace
{
constexpr uint64 T0 = 1'700'000'000'000ull;   // any clock above 0
constexpr uint64 Settled = 2000u;               // past MetaCreationDelayMs and RemoveGraceMs, inside OrphanPairWindowMs
constexpr uint64 LongAgo = 60000u;              // outside OrphanPairWindowMs

hash::SContentHash MakeContent (FStringView Text)
{
	return hash::HashContent(Text.AsBytes());
}

platform::SFileIdentity MakeIdentity (uint32 N)
{
	platform::SFileIdentity identity;
	identity.Bytes[0] = static_cast<uint8>(N);
	identity.Bytes[1] = static_cast<uint8>(N >> 8u);
	identity.Bytes[23] = 0x5Au;
	return identity;
}

SGuid MakeGuid (uint8 N)
{
	SGuid guid;
	guid.Bytes[0] = N;
	guid.Bytes[15] = 0xA5u;
	return guid;
}

const SGuid G1 = MakeGuid(1u);
const SGuid G2 = MakeGuid(2u);

/**
 * A mount's facts as F4 would gather them, plus what the caller would answer when asked: each file's content, and the
 * committed metas. Builders keep the contract of Lifecycle.h: a live GUID is listed where its meta sits on disk.
 */
struct SWorld
{
	uint64 Now = T0;
	SSafetySwitches Safety;
	SAssetEditorSettings Times;
	FString Salt = "Demo";
	bool bCommand = false;
	bool bVcsHold = false;
	std::vector<SLifecycleSource> Sources;
	std::vector<SLifecycleMeta> Metas;
	std::vector<SLifecycleTombstone> Tombstones;
	std::vector<SLiveGuid> Live;
	std::vector<SGuid> Verified;
	std::optional<std::vector<FString>> Committed;

	std::vector<std::pair<FString, hash::SContentHash>> Disk;
	std::vector<FString> CommittedOnDisk;
	uint32 NextIdentity = 1u;
	uint8 NextRandom = 1u;

	[[nodiscard]] SLifecycleFacts GetFacts () const
	{
		SLifecycleFacts facts;
		facts.NowMs = Now;
		facts.Safety = Safety;
		facts.Times = Times;
		facts.Salt = Salt;
		facts.bCommand = bCommand;
		facts.bVcsHold = bVcsHold;
		facts.Sources = Sources;
		facts.Metas = Metas;
		facts.Tombstones = Tombstones;
		facts.LiveGuids = Live;
		facts.VerifiedCopies = Verified;
		if (Committed.has_value())
		{
			facts.Committed = std::span<const FString>(*Committed);
		}
		return facts;
	}

	// A file with no meta that arrived AgoMs before Now: writable, its content not hashed yet (hashes are lazy).
	SLifecycleSource& AddNewFile (FStringView Path, FStringView Text, uint64 AgoMs = Settled)
	{
		SLifecycleSource source;
		source.MountPath = FString(Path);
		source.Identity = MakeIdentity(NextIdentity++);
		source.bWritable = true;
		source.LastEventMs = Now - AgoMs;
		Disk.emplace_back(FString(Path), MakeContent(Text));
		Sources.push_back(std::move(source));
		return Sources.back();
	}

	// A file paired with its meta, untouched for a long time.
	SLifecycleSource& AddPaired (FStringView Path, const SGuid& Guid, FStringView Text)
	{
		SLifecycleSource& source = AddNewFile(Path, Text, LongAgo);
		source.RecordGuid = Guid;
		source.Stored = EMetaState::Paired;
		source.bMetaOnDisk = true;
		Live.push_back(SLiveGuid { Guid, GetMetaPath(Path) });
		return source;
	}

	// A meta a batch dirtied or an orphan. bLive false: its source's AddPaired already listed it.
	SLifecycleMeta& AddMeta (FStringView MetaPath, const SGuid& Guid, uint64 AgoMs = Settled, bool bLive = true)
	{
		SLifecycleMeta meta;
		meta.MountPath = FString(MetaPath);
		meta.SourcePath = FString(GetMetaSource(MetaPath).value_or(MetaPath));
		meta.Guid = Guid;
		meta.Identity = MakeIdentity(NextIdentity++);
		meta.FirstSeenMs = Now - AgoMs;
		meta.bWritable = true;
		if (bLive)
		{
			Live.push_back(SLiveGuid { Guid, FString(MetaPath) });
		}
		Metas.push_back(std::move(meta));
		return Metas.back();
	}

	void AddTombstone (const SGuid& Guid, FStringView LastPath, FStringView Text, const platform::SFileIdentity& Identity, uint64 AgoMs)
	{
		Tombstones.push_back(SLifecycleTombstone { Guid, FString(LastPath), MakeContent(Text), Identity, Now - AgoMs });
	}

	[[nodiscard]] SLifecycleSource* FindSource (FStringView Path)
	{
		const auto found = std::find_if(Sources.begin(), Sources.end(), [Path] (const SLifecycleSource& Source) { return Source.MountPath == Path; });
		return found != Sources.end() ? &*found : nullptr;
	}

	// The meta of a paired file vanished AgoMs ago.
	void LoseMeta (FStringView Path, uint64 AgoMs)
	{
		SLifecycleSource* source = FindSource(Path);
		ASSERT_NE(source, nullptr);
		source->bMetaOnDisk = false;
		source->MetaGoneMs = Now - AgoMs;
		const FString metaPath = GetMetaPath(Path);
		std::erase_if(Live, [&metaPath] (const SLiveGuid& Entry) { return Entry.MetaPath == metaPath; });
	}

	[[nodiscard]] SGuid MintRandom ()
	{
		SGuid guid;
		guid.Bytes[0] = 0xEEu;
		guid.Bytes[1] = NextRandom++;
		return guid;
	}

	// What the caller does with a result that asks: hash on a worker, ask the provider. False: nothing was asked.
	bool Answer (const SLifecycleResult& Result)
	{
		bool bAnswered = false;
		for (const FString& path : Result.NeedContentHash)
		{
			SLifecycleSource* source = FindSource(path);
			const auto disk = std::find_if(Disk.begin(), Disk.end(), [&path] (const auto& Entry) { return Entry.first == path; });
			if (source != nullptr && disk != Disk.end() && !source->Content.has_value())
			{
				source->Content = disk->second;
				bAnswered = true;
			}
		}
		if (Result.bNeedCommitted && !Committed.has_value())
		{
			Committed = CommittedOnDisk;
			bAnswered = true;
		}
		return bAnswered;
	}
};

// RunLifecycle, answering what it asks for until it asks for nothing new.
SLifecycleResult RunAnswering (SWorld& World)
{
	SLifecycleResult result = RunLifecycle(World.GetFacts());
	for (uint32 round = 0u; round < 4u && World.Answer(result); ++round)
	{
		result = RunLifecycle(World.GetFacts());
	}
	return result;
}

FString DescribeAction (const SLifecycleAction& Action);

// What the writer and the next scan would make of the repairs: E4 registers each GUID and clears its tombstone.
void ApplyRepairs (SWorld& World, const SLifecycleResult& Result)
{
	for (const SLifecycleAction& action : Result.Repairs)
	{
		// A known identity is never reminted: only a new identity (CreateMeta, ReGuid) may leave its GUID to the planner.
		if (!action.Guid.IsValid() && (action.Kind == ELifecycleAction::RestoreMeta || action.Kind == ELifecycleAction::MoveMeta))
		{
			ADD_FAILURE() << DescribeAction(action) << " carries no GUID: the planner would mint a fresh one for a known asset";
		}
		const SGuid guid = action.Guid.IsValid() ? action.Guid : World.MintRandom();
		switch (action.Kind)
		{
		case ELifecycleAction::CreateMeta:
		case ELifecycleAction::RestoreMeta:
		{
			SLifecycleSource* source = World.FindSource(action.Path);
			ASSERT_NE(source, nullptr) << action.Path;
			source->bMetaOnDisk = true;
			source->RecordGuid = guid;
			source->Stored = EMetaState::Paired;
			source->MetaGoneMs = 0u;
			World.Live.push_back(SLiveGuid { guid, GetMetaPath(action.Path) });
			std::erase_if(World.Tombstones, [&guid] (const SLifecycleTombstone& Tombstone) { return Tombstone.Guid == guid; });
			break;
		}
		case ELifecycleAction::MoveMeta:
		{
			const auto meta = std::find_if(World.Metas.begin(), World.Metas.end(), [&action] (const SLifecycleMeta& Meta) { return Meta.MountPath == action.Path; });
			ASSERT_NE(meta, World.Metas.end()) << action.Path;
			const SGuid moved = meta->Guid;
			EXPECT_EQ(action.Guid, moved) << DescribeAction(action) << ": a moved meta keeps its GUID";
			World.Metas.erase(meta);
			for (SLiveGuid& entry : World.Live)
			{
				if (entry.MetaPath == action.Path)
				{
					entry.MetaPath = action.ToPath;
				}
			}
			SLifecycleSource* source = World.FindSource(*GetMetaSource(action.ToPath));
			ASSERT_NE(source, nullptr) << action.ToPath;
			source->bMetaOnDisk = true;
			source->RecordGuid = moved;
			source->Stored = EMetaState::Paired;
			std::erase_if(World.Tombstones, [&moved] (const SLifecycleTombstone& Tombstone) { return Tombstone.Guid == moved; });
			break;
		}
		case ELifecycleAction::FixMetaCase:
			for (SLifecycleSource& source : World.Sources)
			{
				if (source.MetaSpelling == action.Path)
				{
					source.bMetaCaseDiffers = false;
					source.MetaSpelling = FString();
				}
			}
			for (SLiveGuid& entry : World.Live)
			{
				if (entry.MetaPath == action.Path)
				{
					entry.MetaPath = action.ToPath;
				}
			}
			for (SLifecycleMeta& meta : World.Metas)
			{
				if (meta.MountPath == action.Path)
				{
					meta.MountPath = action.ToPath;
					meta.SourcePath = FString(*GetMetaSource(action.ToPath));
				}
			}
			break;
		case ELifecycleAction::ReGuid:
			for (SLiveGuid& entry : World.Live)
			{
				if (entry.MetaPath == action.Path)
				{
					entry.Guid = guid;
				}
			}
			for (SLifecycleMeta& meta : World.Metas)
			{
				if (meta.MountPath == action.Path)
				{
					meta.Guid = guid;
				}
			}
			for (SLifecycleSource& source : World.Sources)
			{
				if (GetMetaPath(source.MountPath) == action.Path)
				{
					source.RecordGuid = guid;
				}
			}
			break;
		case ELifecycleAction::KeepAliasB:
			ADD_FAILURE() << "keep A, alias B is a person's choice, never a repair";
			break;
		}
	}
}


// ---- Results as text, so a table can state them ----

FString DescribeAction (const SLifecycleAction& Action)
{
	FString text(enum_::ToString(Action.Kind));
	text += ' ';
	text += Action.Path.View();
	if (!Action.ToPath.IsEmpty())
	{
		text += FStringView(" -> ");
		text += Action.ToPath.View();
	}
	return text;
}

std::vector<FString> Describe (const std::vector<SLifecycleAction>& Actions)
{
	std::vector<FString> text;
	for (const SLifecycleAction& action : Actions)
	{
		text.push_back(DescribeAction(action));
	}
	std::sort(text.begin(), text.end());
	return text;
}

std::vector<FString> Describe (const std::vector<SDiagnostic>& Problems)
{
	std::vector<FString> text;
	for (const SDiagnostic& problem : Problems)
	{
		FString line = FormatDiagnosticCode(problem.Code);
		line += ' ';
		line += problem.Subject.View();
		text.push_back(std::move(line));
	}
	std::sort(text.begin(), text.end());
	return text;
}

std::vector<FString> Describe (const std::vector<SLifecycleState>& States)
{
	std::vector<FString> text;
	for (const SLifecycleState& state : States)
	{
		FString line = state.MountPath;
		line += ' ';
		line += FStringView(enum_::ToString(state.State));
		text.push_back(std::move(line));
	}
	std::sort(text.begin(), text.end());
	return text;
}

// Everything, in the order produced, GUIDs and remaps included: what two runs must agree on byte for byte.
FString DescribeExactly (const SLifecycleResult& Result)
{
	FString text;
	const auto actions = [&text] (FStringView Label, const std::vector<SLifecycleAction>& Actions)
	{
		for (const SLifecycleAction& action : Actions)
		{
			text += FString::Format("{} {} {}", Label, DescribeAction(action), action.Guid.ToString());
			for (const SGuid& alias : action.Aliases)
			{
				text += FString::Format(" alias {}", alias.ToString());
			}
			for (const auto& [from, to] : action.Remap)
			{
				text += FString::Format(" {}>{}", from.ToString(), to.ToString());
			}
			text += '\n';
		}
	};
	actions("repair", Result.Repairs);
	actions("fix-it", Result.FixIts);
	for (const SLifecycleState& state : Result.States)
	{
		text += FString::Format("state {} {}\n", state.MountPath, enum_::ToString(state.State));
	}
	for (const SDiagnostic& problem : Result.Problems)
	{
		text += FString::Format("problem {} {} {} {}\n", GetSeverityName(problem.Severity), FormatDiagnosticCode(problem.Code), problem.Subject,
			problem.Message);
	}
	for (const FString& path : Result.NeedContentHash)
	{
		text += FString::Format("hash {}\n", path);
	}
	text += FString::Format("committed {} wake {}\n", Result.bNeedCommitted, Result.WakeAtMs);
	return text;
}

struct SExpect
{
	std::vector<FStringView> Repairs;
	std::vector<FStringView> FixIts;
	std::vector<FStringView> Problems;
	std::vector<FStringView> States;
};

std::vector<FString> Sorted (const std::vector<FStringView>& Lines)
{
	std::vector<FString> text;
	for (const FStringView line : Lines)
	{
		text.push_back(FString(line));
	}
	std::sort(text.begin(), text.end());
	return text;
}

void ExpectResult (const SLifecycleResult& Result, const SExpect& Expected)
{
	EXPECT_EQ(Describe(Result.Repairs), Sorted(Expected.Repairs)) << "repairs";
	EXPECT_EQ(Describe(Result.FixIts), Sorted(Expected.FixIts)) << "fix-its";
	EXPECT_EQ(Describe(Result.Problems), Sorted(Expected.Problems)) << "problems";
	EXPECT_EQ(Describe(Result.States), Sorted(Expected.States)) << "states";
}


// ---- The table: every transition, with each default-on switch on, then off ----

enum class ESwitch : uint8 { None, AutoRepairMetas, DerivedAutoGuids, AutoResolveDuplicates, OrphanPairing };

constexpr ESwitch SwitchesOff[] = { ESwitch::AutoRepairMetas, ESwitch::DerivedAutoGuids, ESwitch::AutoResolveDuplicates, ESwitch::OrphanPairing };

const char* GetSwitchName (ESwitch Switch)
{
	switch (Switch)
	{
	case ESwitch::None: return "all on";
	case ESwitch::AutoRepairMetas: return "AutoRepairMetas off";
	case ESwitch::DerivedAutoGuids: return "DerivedAutoGuids off";
	case ESwitch::AutoResolveDuplicates: return "AutoResolveDuplicates off";
	case ESwitch::OrphanPairing: return "OrphanPairing off";
	}
	return "";
}

void TurnOff (SSafetySwitches& Safety, ESwitch Switch)
{
	switch (Switch)
	{
	case ESwitch::None: break;
	case ESwitch::AutoRepairMetas: Safety.bAutoRepairMetas = false; break;
	case ESwitch::DerivedAutoGuids: Safety.bDerivedAutoGuids = false; break;
	case ESwitch::AutoResolveDuplicates: Safety.bAutoResolveDuplicates = false; break;
	case ESwitch::OrphanPairing: Safety.bOrphanPairing = false; break;
	}
}

struct SCase
{
	const char* Name;
	void (*Build) (SWorld&);
	SExpect On;
	std::vector<std::pair<ESwitch, SExpect>> Off;   // a switch not listed changes nothing in this case

	[[nodiscard]] const SExpect& GetExpected (ESwitch Switch) const
	{
		for (const auto& [off, expected] : Off)
		{
			if (off == Switch)
			{
				return expected;
			}
		}
		return On;
	}
};

const std::vector<SCase>& GetCases ()
{
	static const std::vector<SCase> cases = {
		{ "a new file waits for its meta during the creation delay",
			[] (SWorld& W) { W.AddNewFile("/Project/Скелі/Камінь.png", "rock", 500u); },
			{ .States = { "/Project/Скелі/Камінь.png PendingMeta" } } },

		{ "a new file gets a meta once the delay is over",
			[] (SWorld& W) { W.AddNewFile("/Project/Скелі/Камінь.png", "rock"); },
			{ .Repairs = { "CreateMeta /Project/Скелі/Камінь.png" }, .States = { "/Project/Скелі/Камінь.png PendingMeta" } },
			{ { ESwitch::AutoRepairMetas, { .FixIts = { "CreateMeta /Project/Скелі/Камінь.png" }, .Problems = { "META001 /Project/Скелі/Камінь.png" },
				.States = { "/Project/Скелі/Камінь.png PendingMeta" } } } } },

		{ "a new file in a process that does not write here is ErrorMissingMeta",
			[] (SWorld& W) { W.AddNewFile("/Project/Скелі/Камінь.png", "rock").bWritable = false; },
			{ .FixIts = { "CreateMeta /Project/Скелі/Камінь.png" }, .Problems = { "META001 /Project/Скелі/Камінь.png" },
				.States = { "/Project/Скелі/Камінь.png PendingMeta" } } },

		{ "a file moved without its meta pairs with the orphan by FileId",
			[] (SWorld& W)
			{
				W.AddTombstone(G1, "/Project/Old/Rock.png", "rock", MakeIdentity(900u), 2500u);
				W.AddMeta("/Project/Old/Rock.png.meta", G1, 2500u);
				W.AddNewFile("/Project/New/Rock.png", "rock", 2500u).Identity = MakeIdentity(900u);
			},
			{ .Repairs = { "MoveMeta /Project/Old/Rock.png.meta -> /Project/New/Rock.png.meta" }, .States = { "/Project/New/Rock.png PendingMeta" } },
			{ { ESwitch::OrphanPairing, { .FixIts = { "MoveMeta /Project/Old/Rock.png.meta -> /Project/New/Rock.png.meta" },
				.Problems = { "META008 /Project/New/Rock.png", "META009 /Project/Old/Rock.png.meta", "META002 /Project/Old/Rock.png.meta" },
				.States = { "/Project/New/Rock.png MissingMeta", "/Project/Old/Rock.png.meta Orphan" } } } } },

		{ "a file moved without its meta pairs with the orphan by content inside the move window",
			[] (SWorld& W)
			{
				W.AddTombstone(G1, "/Project/Old/Rock.png", "rock", MakeIdentity(901u), 2500u);
				W.AddMeta("/Project/Old/Rock.png.meta", G1, 2500u);
				W.AddNewFile("/Project/New/Rock.png", "rock");
			},
			{ .Repairs = { "MoveMeta /Project/Old/Rock.png.meta -> /Project/New/Rock.png.meta" }, .States = { "/Project/New/Rock.png PendingMeta" } },
			{ { ESwitch::OrphanPairing, { .FixIts = { "MoveMeta /Project/Old/Rock.png.meta -> /Project/New/Rock.png.meta" },
				.Problems = { "META008 /Project/New/Rock.png", "META009 /Project/Old/Rock.png.meta", "META002 /Project/Old/Rock.png.meta" },
				.States = { "/Project/New/Rock.png MissingMeta", "/Project/Old/Rock.png.meta Orphan" } } } } },

		{ "an older orphan with the same content is only a suggestion, and the file gets its own identity",
			[] (SWorld& W)
			{
				W.AddTombstone(G1, "/Project/Old/Rock.png", "rock", MakeIdentity(902u), LongAgo);
				W.AddMeta("/Project/Old/Rock.png.meta", G1, LongAgo);
				W.AddNewFile("/Project/New/Rock.png", "rock");
			},
			{ .Repairs = { "CreateMeta /Project/New/Rock.png" }, .FixIts = { "MoveMeta /Project/Old/Rock.png.meta -> /Project/New/Rock.png.meta" },
				.Problems = { "META009 /Project/Old/Rock.png.meta", "META002 /Project/Old/Rock.png.meta" },
				.States = { "/Project/New/Rock.png PendingMeta", "/Project/Old/Rock.png.meta Orphan" } },
			// OrphanPairing off changes nothing here: it removes an automatic pairing, and this match would never have paired.
			{ { ESwitch::AutoRepairMetas, { .FixIts = { "MoveMeta /Project/Old/Rock.png.meta -> /Project/New/Rock.png.meta", "CreateMeta /Project/New/Rock.png" },
				.Problems = { "META001 /Project/New/Rock.png", "META009 /Project/Old/Rock.png.meta", "META002 /Project/Old/Rock.png.meta" },
				.States = { "/Project/New/Rock.png PendingMeta", "/Project/Old/Rock.png.meta Orphan" } } } } },

		{ "delete, then an identical file at that path later, its meta gone too: CreateMeta",
			[] (SWorld& W)
			{
				W.AddTombstone(G1, "/Project/Rock.png", "rock", MakeIdentity(903u), LongAgo);
				W.AddNewFile("/Project/Rock.png", "rock");
			},
			{ .Repairs = { "CreateMeta /Project/Rock.png" }, .States = { "/Project/Rock.png PendingMeta" } },
			{ { ESwitch::AutoRepairMetas, { .FixIts = { "CreateMeta /Project/Rock.png" }, .Problems = { "META001 /Project/Rock.png" },
				.States = { "/Project/Rock.png PendingMeta" } } } } },

		{ "delete, then an identical file at that path later, where the old meta still sits: it pairs",
			[] (SWorld& W)
			{
				W.AddTombstone(G1, "/Project/Rock.png", "rock", MakeIdentity(904u), LongAgo);
				W.AddPaired("/Project/Rock.png", G1, "rock").LastEventMs = W.Now - Settled;
				W.AddMeta("/Project/Rock.png.meta", G1, LongAgo, false);
			},
			{ .States = { "/Project/Rock.png Paired", "/Project/Rock.png.meta Paired" } } },

		{ "two new files that match one orphan pair with neither, and never mint a second identity",
			[] (SWorld& W)
			{
				W.AddTombstone(G1, "/Project/Old/Rock.png", "rock", MakeIdentity(905u), 2500u);
				W.AddMeta("/Project/Old/Rock.png.meta", G1, 2500u);
				W.AddNewFile("/Project/A/Rock.png", "rock");
				W.AddNewFile("/Project/B/Rock.png", "rock");
			},
			{ .FixIts = { "MoveMeta /Project/Old/Rock.png.meta -> /Project/A/Rock.png.meta", "MoveMeta /Project/Old/Rock.png.meta -> /Project/B/Rock.png.meta" },
				.Problems = { "META008 /Project/A/Rock.png", "META008 /Project/B/Rock.png", "META009 /Project/Old/Rock.png.meta",
					"META009 /Project/Old/Rock.png.meta", "META002 /Project/Old/Rock.png.meta" },
				.States = { "/Project/A/Rock.png MissingMeta", "/Project/B/Rock.png MissingMeta", "/Project/Old/Rock.png.meta Orphan" } } },

		{ "a FileId match outranks another file's content match: the moved file pairs, the copy gets its own identity",
			[] (SWorld& W)
			{
				// Explorer: Rock.png copied to Copy/, then moved to New/ without its meta, in one batch.
				W.AddTombstone(G1, "/Project/Old/Rock.png", "rock", MakeIdentity(909u), 2500u);
				W.AddMeta("/Project/Old/Rock.png.meta", G1, 2500u);
				W.AddNewFile("/Project/New/Rock.png", "rock", 2500u).Identity = MakeIdentity(909u);
				W.AddNewFile("/Project/Copy/Rock.png", "rock");
			},
			{ .Repairs = { "MoveMeta /Project/Old/Rock.png.meta -> /Project/New/Rock.png.meta", "CreateMeta /Project/Copy/Rock.png" },
				.States = { "/Project/New/Rock.png PendingMeta", "/Project/Copy/Rock.png PendingMeta" } },
			{ { ESwitch::AutoRepairMetas, { .Repairs = { "MoveMeta /Project/Old/Rock.png.meta -> /Project/New/Rock.png.meta" },
				.FixIts = { "CreateMeta /Project/Copy/Rock.png" }, .Problems = { "META001 /Project/Copy/Rock.png" },
				.States = { "/Project/New/Rock.png PendingMeta", "/Project/Copy/Rock.png PendingMeta" } } },
			  { ESwitch::OrphanPairing, { .Repairs = { "CreateMeta /Project/Copy/Rock.png" },
				.FixIts = { "MoveMeta /Project/Old/Rock.png.meta -> /Project/New/Rock.png.meta" },
				.Problems = { "META008 /Project/New/Rock.png", "META009 /Project/Old/Rock.png.meta", "META002 /Project/Old/Rock.png.meta" },
				.States = { "/Project/New/Rock.png MissingMeta", "/Project/Copy/Rock.png PendingMeta", "/Project/Old/Rock.png.meta Orphan" } } } } },

		{ "a lost meta is restored from its verified copy after the grace",
			[] (SWorld& W)
			{
				W.AddPaired("/Project/Дерево.png", G1, "tree");
				W.LoseMeta("/Project/Дерево.png", Settled);
				W.Verified.push_back(G1);
			},
			{ .Repairs = { "RestoreMeta /Project/Дерево.png" }, .States = { "/Project/Дерево.png MissingMeta" } },
			{ { ESwitch::AutoRepairMetas, { .FixIts = { "RestoreMeta /Project/Дерево.png" }, .Problems = { "META008 /Project/Дерево.png" },
				.States = { "/Project/Дерево.png MissingMeta" } } } } },

		{ "a lost meta waits out its grace: a VCS sync may put it back",
			[] (SWorld& W)
			{
				W.AddPaired("/Project/Дерево.png", G1, "tree");
				W.LoseMeta("/Project/Дерево.png", 500u);
				W.Verified.push_back(G1);
			},
			{} },

		{ "a lost meta with no verified copy is MissingMeta, never a fresh GUID",
			[] (SWorld& W)
			{
				W.AddPaired("/Project/Дерево.png", G1, "tree");
				W.LoseMeta("/Project/Дерево.png", Settled);
			},
			{ .FixIts = { "CreateMeta /Project/Дерево.png" }, .Problems = { "META008 /Project/Дерево.png" }, .States = { "/Project/Дерево.png MissingMeta" } } },

		{ "a meta swap transfers the identity, and the old source is offered a new GUID",
			[] (SWorld& W)
			{
				W.AddPaired("/Project/Img.png", G1, "png bytes");
				W.LoseMeta("/Project/Img.png", Settled);
				W.AddPaired("/Project/Img.psd", G1, "psd bytes").LastEventMs = W.Now - Settled;
				W.AddMeta("/Project/Img.psd.meta", G1, Settled, false);
				W.Verified.push_back(G1);
			},
			{ .FixIts = { "CreateMeta /Project/Img.png" }, .Problems = { "META008 /Project/Img.png" },
				.States = { "/Project/Img.png MissingMeta", "/Project/Img.psd Paired", "/Project/Img.psd.meta Paired" } } },

		{ "a CSV converted to .frt keeps its identity: no tombstone, no MissingMeta",
			[] (SWorld& W)
			{
				W.AddPaired("/Project/Таблиці/T.frt", G1, "rows").LastEventMs = W.Now - Settled;
				W.AddMeta("/Project/Таблиці/T.frt.meta", G1, Settled, false);
				W.AddTombstone(G1, "/Project/Таблиці/T.csv", "a,b", MakeIdentity(906u), Settled);
			},
			{ .States = { "/Project/Таблиці/T.frt Paired", "/Project/Таблиці/T.frt.meta Paired" } } },

		{ "an orphan meta waits out the grace of its source's removal",
			[] (SWorld& W)
			{
				W.AddTombstone(G1, "/Project/Gone.png", "gone", MakeIdentity(907u), 500u);
				W.AddMeta("/Project/Gone.png.meta", G1, 500u);
			},
			{} },

		{ "an orphan meta is reported after the grace and never deleted",
			[] (SWorld& W)
			{
				W.AddTombstone(G1, "/Project/Gone.png", "gone", MakeIdentity(907u), Settled);
				W.AddMeta("/Project/Gone.png.meta", G1, Settled);
			},
			{ .Problems = { "META002 /Project/Gone.png.meta" }, .States = { "/Project/Gone.png.meta Orphan" } } },

		{ "a folder copy re-GUIDs the copies, the committed originals keep theirs",
			[] (SWorld& W)
			{
				W.AddPaired("/Project/Pack/Model.gltf", G1, "model");
				W.AddPaired("/Project/Pack/Skin.png", G2, "skin");
				W.AddPaired("/Project/Pack (копія)/Model.gltf", G1, "model").LastEventMs = W.Now - Settled;
				W.AddMeta("/Project/Pack (копія)/Model.gltf.meta", G1, Settled, false);
				W.AddPaired("/Project/Pack (копія)/Skin.png", G2, "skin").LastEventMs = W.Now - Settled;
				W.AddMeta("/Project/Pack (копія)/Skin.png.meta", G2, Settled, false);
				W.CommittedOnDisk = { "/Project/Pack/Model.gltf.meta", "/Project/Pack/Skin.png.meta" };
			},
			{ .Repairs = { "ReGuid /Project/Pack (копія)/Model.gltf.meta", "ReGuid /Project/Pack (копія)/Skin.png.meta" },
				.States = { "/Project/Pack/Model.gltf Paired", "/Project/Pack/Skin.png Paired", "/Project/Pack (копія)/Model.gltf Paired",
					"/Project/Pack (копія)/Model.gltf.meta Paired", "/Project/Pack (копія)/Skin.png Paired", "/Project/Pack (копія)/Skin.png.meta Paired" } },
			{ { ESwitch::AutoResolveDuplicates, { .FixIts = { "ReGuid /Project/Pack (копія)/Model.gltf.meta", "ReGuid /Project/Pack (копія)/Skin.png.meta" },
				.Problems = { "META003 /Project/Pack (копія)/Model.gltf", "META003 /Project/Pack (копія)/Skin.png" },
				.States = { "/Project/Pack/Model.gltf Paired", "/Project/Pack/Skin.png Paired", "/Project/Pack (копія)/Model.gltf Paired",
					"/Project/Pack (копія)/Model.gltf.meta Paired", "/Project/Pack (копія)/Skin.png Paired", "/Project/Pack (копія)/Skin.png.meta Paired" } } } } },

		{ "a duplicate in a process that does not write there is an error with the re-GUID fix-it",
			[] (SWorld& W)
			{
				W.AddPaired("/Project/Pack/Skin.png", G2, "skin");
				SLifecycleSource& copy = W.AddPaired("/Project/Pack (копія)/Skin.png", G2, "skin");
				copy.LastEventMs = W.Now - Settled;
				copy.bWritable = false;
				W.AddMeta("/Project/Pack (копія)/Skin.png.meta", G2, Settled, false).bWritable = false;
				W.CommittedOnDisk = { "/Project/Pack/Skin.png.meta" };
			},
			{ .FixIts = { "ReGuid /Project/Pack (копія)/Skin.png.meta" }, .Problems = { "META003 /Project/Pack (копія)/Skin.png" },
				.States = { "/Project/Pack/Skin.png Paired", "/Project/Pack (копія)/Skin.png Paired", "/Project/Pack (копія)/Skin.png.meta Paired" } } },

		{ "a broken meta is never overwritten, and two Guid lines offer keep A, alias B",
			[] (SWorld& W)
			{
				W.AddPaired("/Project/Конфлікт.png", G1, "x");
				SLifecycleMeta& meta = W.AddMeta("/Project/Конфлікт.png.meta", SGuid {}, Settled, false);
				meta.Parse = EMetaParseState::Broken;
				meta.SalvagedGuids = { G2, G1 };
			},
			{ .FixIts = { "KeepAliasB /Project/Конфлікт.png.meta", "KeepAliasB /Project/Конфлікт.png.meta" }, .Problems = { "META004 /Project/Конфлікт.png.meta" },
				.States = { "/Project/Конфлікт.png Broken", "/Project/Конфлікт.png.meta Broken" } } },

		{ "a broken orphan meta is reported as broken, never paired",
			[] (SWorld& W)
			{
				SLifecycleMeta& meta = W.AddMeta("/Project/Сирота.png.meta", SGuid {}, Settled, false);
				meta.Parse = EMetaParseState::Broken;
				meta.SalvagedGuids = { G1 };
				W.AddNewFile("/Project/Інший.png", "other").Content = MakeContent("other");
			},
			{ .Repairs = { "CreateMeta /Project/Інший.png" }, .Problems = { "META004 /Project/Сирота.png.meta" },
				.States = { "/Project/Інший.png PendingMeta", "/Project/Сирота.png.meta Broken" } },
			{ { ESwitch::AutoRepairMetas, { .FixIts = { "CreateMeta /Project/Інший.png" },
				.Problems = { "META001 /Project/Інший.png", "META004 /Project/Сирота.png.meta" },
				.States = { "/Project/Інший.png PendingMeta", "/Project/Сирота.png.meta Broken" } } } } },

		{ "a meta whose case differs from its source's is renamed to the source's case",
			[] (SWorld& W)
			{
				SLifecycleSource& source = W.AddPaired("/Project/Rock.png", G1, "rock");
				source.bMetaCaseDiffers = true;
				source.MetaSpelling = "/Project/rock.PNG.meta";
				source.LastEventMs = W.Now - Settled;
				W.Live.back().MetaPath = "/Project/rock.PNG.meta";
			},
			{ .Repairs = { "FixMetaCase /Project/rock.PNG.meta -> /Project/Rock.png.meta" }, .States = { "/Project/Rock.png Paired" } } },

		{ "a meta whose case differs, in a process that does not write there, is META006 with the fix-it",
			[] (SWorld& W)
			{
				SLifecycleSource& source = W.AddPaired("/Project/Rock.png", G1, "rock");
				source.bMetaCaseDiffers = true;
				source.MetaSpelling = "/Project/rock.PNG.meta";
				source.bWritable = false;
				W.Live.back().MetaPath = "/Project/rock.PNG.meta";
			},
			{ .FixIts = { "FixMetaCase /Project/rock.PNG.meta -> /Project/Rock.png.meta" }, .Problems = { "META006 /Project/Rock.png" },
				.States = { "/Project/Rock.png Paired" } } },

		{ "a Git LFS pointer stays PendingMeta and is never given a meta",
			[] (SWorld& W) { W.AddNewFile("/Project/Big.psd", "version https://git-lfs.github.com/spec/v1").bLfsPointer = true; },
			{ .Problems = { "VCS004 /Project/Big.psd" }, .States = { "/Project/Big.psd PendingMeta" } } },

		{ "a GUID edited by hand is registered, and its old GUID's tombstone asks for nothing",
			[] (SWorld& W)
			{
				W.AddPaired("/Project/A.png", G2, "a").LastEventMs = W.Now - Settled;
				W.AddMeta("/Project/A.png.meta", G2, Settled, false);
				W.AddTombstone(G1, "/Project/A.png", "a", MakeIdentity(908u), Settled);
			},
			{ .States = { "/Project/A.png Paired", "/Project/A.png.meta Paired" } } },

		{ "a lost meta whose GUID now sits in an orphan offers to move it back",
			[] (SWorld& W)
			{
				W.AddPaired("/Project/A.png", G1, "a");
				W.LoseMeta("/Project/A.png", Settled);
				W.AddMeta("/Project/A2.png.meta", G1, Settled);
				W.Verified.push_back(G1);
			},
			{ .FixIts = { "MoveMeta /Project/A2.png.meta -> /Project/A.png.meta", "CreateMeta /Project/A.png" },
				.Problems = { "META008 /Project/A.png", "META002 /Project/A2.png.meta" },
				.States = { "/Project/A.png MissingMeta", "/Project/A2.png.meta Orphan" } } },

		{ "a meta beside an excluded file is left alone",
			[] (SWorld& W) { W.AddMeta("/Project/Readme.md.meta", G1).bSourceExcluded = true; },
			{} },

		{ "a meta beside a file of a class that gets no meta is left alone, not taken for an orphan",
			[] (SWorld& W)
			{
				W.AddNewFile("/Project/Notes.txt", "notes").Class = EFileClass::Unclaimed;   // its Importer: pin no longer resolves
				W.AddMeta("/Project/Notes.txt.meta", G1);
				W.AddTombstone(G1, "/Project/Elsewhere/Notes.txt", "notes", MakeIdentity(910u), Settled);
				W.AddNewFile("/Project/Elsewhere/Notes.png", "notes").Identity = MakeIdentity(910u);
			},
			{ .Repairs = { "CreateMeta /Project/Elsewhere/Notes.png" }, .States = { "/Project/Elsewhere/Notes.png PendingMeta" } },
			{ { ESwitch::AutoRepairMetas, { .FixIts = { "CreateMeta /Project/Elsewhere/Notes.png" }, .Problems = { "META001 /Project/Elsewhere/Notes.png" },
				.States = { "/Project/Elsewhere/Notes.png PendingMeta" } } } } },
	};
	return cases;
}
}


TEST(LifecycleTest, EveryTransitionGivesItsRowWithEachSwitchOnAndOff)
{
	for (const SCase& testCase : GetCases())
	{
		for (const ESwitch off : { ESwitch::None, SwitchesOff[0], SwitchesOff[1], SwitchesOff[2], SwitchesOff[3] })
		{
			SCOPED_TRACE(testing::Message() << testCase.Name << " / " << GetSwitchName(off));
			SWorld world;
			testCase.Build(world);
			TurnOff(world.Safety, off);
			ExpectResult(RunAnswering(world), testCase.GetExpected(off));
		}
	}
}

// The guard against a repair loop: once a result's repairs are applied, the same facts propose nothing more.
TEST(LifecycleTest, ApplyingTheRepairsLeavesNothingToRepair)
{
	for (const SCase& testCase : GetCases())
	{
		for (const ESwitch off : { ESwitch::None, SwitchesOff[0], SwitchesOff[1], SwitchesOff[2], SwitchesOff[3] })
		{
			SCOPED_TRACE(testing::Message() << testCase.Name << " / " << GetSwitchName(off));
			SWorld world;
			testCase.Build(world);
			TurnOff(world.Safety, off);
			const SLifecycleResult first = RunAnswering(world);
			ApplyRepairs(world, first);
			const SLifecycleResult second = RunAnswering(world);
			EXPECT_TRUE(second.Repairs.empty()) << Describe(second.Repairs).size() << " more: " << DescribeExactly(second);
		}
	}
}

// §4.7: the switches cover automatic repairs only; a person's command writes its metas whatever AutoRepairMetas says.
TEST(LifecycleTest, ACommandIgnoresAutoRepairMetas)
{
	for (const SCase& testCase : GetCases())
	{
		SCOPED_TRACE(testCase.Name);
		SWorld onWorld;
		testCase.Build(onWorld);
		onWorld.bCommand = true;
		const FString on = DescribeExactly(RunAnswering(onWorld));
		SWorld world;
		testCase.Build(world);
		world.bCommand = true;
		TurnOff(world.Safety, ESwitch::AutoRepairMetas);
		EXPECT_EQ(DescribeExactly(RunAnswering(world)), on);
	}
}

// Edit [9]: `fix` never relinks. OrphanPairing off keeps a match a relink fix-it even for a command, as AutoResolveDuplicates
// off keeps a duplicate one, and the file still never falls through to CreateMeta.
TEST(LifecycleTest, OrphanPairingOffKeepsARelinkAFixItEvenForACommand)
{
	SWorld world;
	world.bCommand = true;
	world.Safety.bOrphanPairing = false;
	world.AddTombstone(G1, "/Project/Old/Rock.png", "rock", MakeIdentity(980u), 0u);
	world.AddMeta("/Project/Old/Rock.png.meta", G1, 0u);
	world.AddNewFile("/Project/New/Rock.png", "rock", 0u).Identity = MakeIdentity(980u);
	const SLifecycleResult result = RunAnswering(world);
	EXPECT_TRUE(result.Repairs.empty()) << DescribeExactly(result);
	EXPECT_EQ(Describe(result.FixIts), Sorted({ "MoveMeta /Project/Old/Rock.png.meta -> /Project/New/Rock.png.meta" }));
	EXPECT_EQ(Describe(result.States), Sorted({ "/Project/New/Rock.png MissingMeta", "/Project/Old/Rock.png.meta Orphan" }));
}

// Identity must not depend on the order the caller happened to gather the facts in: two machines list differently.
TEST(LifecycleTest, TheOrderOfTheFactsChangesNothing)
{
	for (const SCase& testCase : GetCases())
	{
		SCOPED_TRACE(testCase.Name);
		SWorld world;
		testCase.Build(world);
		const FString forward = DescribeExactly(RunAnswering(world));

		SWorld reversed;
		testCase.Build(reversed);
		std::reverse(reversed.Sources.begin(), reversed.Sources.end());
		std::reverse(reversed.Metas.begin(), reversed.Metas.end());
		std::reverse(reversed.Tombstones.begin(), reversed.Tombstones.end());
		std::reverse(reversed.Live.begin(), reversed.Live.end());
		std::reverse(reversed.CommittedOnDisk.begin(), reversed.CommittedOnDisk.end());
		EXPECT_EQ(DescribeExactly(RunAnswering(reversed)), forward);
	}
}


// ---- GUIDs ----

TEST(LifecycleTest, ACreatedMetaHasTheFrozenDerivationOfItsPathAndContent)
{
	SWorld world;
	world.AddNewFile("/Project/Скелі/Камінь.png", "rock");
	const SLifecycleResult result = RunAnswering(world);
	ASSERT_EQ(result.Repairs.size(), 1u);
	EXPECT_EQ(result.Repairs[0].Guid, MakeCreateMetaGuid("Demo", "/Project/Скелі/Камінь.png", MakeContent("rock")));

	SWorld otherSalt;
	otherSalt.Salt = "Blank";
	otherSalt.AddNewFile("/Project/Скелі/Камінь.png", "rock");
	const SLifecycleResult salted = RunAnswering(otherSalt);
	ASSERT_EQ(salted.Repairs.size(), 1u);
	EXPECT_EQ(salted.Repairs[0].Guid, MakeCreateMetaGuid("Blank", "/Project/Скелі/Камінь.png", MakeContent("rock")));
	EXPECT_NE(salted.Repairs[0].Guid, result.Repairs[0].Guid);

	SWorld random;
	random.Safety.bDerivedAutoGuids = false;
	random.AddNewFile("/Project/Скелі/Камінь.png", "rock");
	const SLifecycleResult first = RunLifecycle(random.GetFacts());
	EXPECT_TRUE(first.NeedContentHash.empty()) << "a random GUID needs no hash";
	ASSERT_EQ(first.Repairs.size(), 1u);
	EXPECT_FALSE(first.Repairs[0].Guid.IsValid()) << "the planner calls NewRandom: the function stays pure";
}

// "Delete, then an identical file later": the derivation is the same on every machine, so it is the old GUID only if the
// old one was derived from this very path and content; it never comes from the tombstone, which is this machine's memory.
TEST(LifecycleTest, TombstonesNeverInfluenceADerivedGuid)
{
	const SGuid derived = MakeCreateMetaGuid("Demo", "/Project/Rock.png", MakeContent("rock"));
	SWorld world;
	world.AddTombstone(derived, "/Project/Rock.png", "rock", MakeIdentity(950u), LongAgo);
	world.AddNewFile("/Project/Rock.png", "rock");
	const SLifecycleResult result = RunAnswering(world);
	ASSERT_EQ(result.Repairs.size(), 1u);
	EXPECT_EQ(result.Repairs[0].Guid, derived);
}

TEST(LifecycleTest, ADerivedGuidReRollsPastALiveOne)
{
	const hash::SContentHash content = MakeContent("rock");
	const SGuid first = MakeCreateMetaGuid("Demo", "/Project/A.png", content);
	SWorld world;
	world.AddPaired("/Project/Elsewhere.png", first, "elsewhere");
	world.AddNewFile("/Project/A.png", "rock");
	const SLifecycleResult result = RunAnswering(world);
	ASSERT_EQ(result.Repairs.size(), 1u);
	EXPECT_NE(result.Repairs[0].Guid, first);
	EXPECT_EQ(result.Repairs[0].Guid, MakeCreateMetaGuid("Demo", "/Project/A.png", content, [&first] (const SGuid& Guid) { return Guid == first; }));
}

// §4.6, §4.7: never a fresh GUID for a known asset, whatever the switches. A restore, the "create from registry" fix-it and
// an orphan pairing carry the identity they keep, even with DerivedAutoGuids off, where only new identities go random.
TEST(LifecycleTest, AKnownIdentityKeepsItsGuidWithEachSwitchOnAndOff)
{
	const auto findAll = [] (const SLifecycleResult& Result, ELifecycleAction Kind)
	{
		std::vector<const SLifecycleAction*> found;
		for (const std::vector<SLifecycleAction>* actions : { &Result.Repairs, &Result.FixIts })
		{
			for (const SLifecycleAction& action : *actions)
			{
				if (action.Kind == Kind)
				{
					found.push_back(&action);
				}
			}
		}
		return found;
	};
	for (const ESwitch off : { ESwitch::None, SwitchesOff[0], SwitchesOff[1], SwitchesOff[2], SwitchesOff[3] })
	{
		SCOPED_TRACE(GetSwitchName(off));
		{
			SWorld world;
			TurnOff(world.Safety, off);
			world.AddPaired("/Project/Дерево.png", G1, "tree");
			world.LoseMeta("/Project/Дерево.png", Settled);
			world.Verified.push_back(G1);
			const SLifecycleResult result = RunAnswering(world);
			const auto restores = findAll(result, ELifecycleAction::RestoreMeta);
			ASSERT_EQ(restores.size(), 1u);
			EXPECT_EQ(restores[0]->Guid, G1) << "a restore keeps the record's GUID";
		}
		{
			SWorld world;
			TurnOff(world.Safety, off);
			world.AddPaired("/Project/Дерево.png", G1, "tree");
			world.LoseMeta("/Project/Дерево.png", Settled);
			const SLifecycleResult result = RunAnswering(world);
			const auto creates = findAll(result, ELifecycleAction::CreateMeta);
			ASSERT_EQ(creates.size(), 1u);
			EXPECT_EQ(creates[0]->Guid, G1) << "create from registry keeps the record's GUID, never a derived or random one";
		}
		for (const bool bByFileId : { true, false })
		{
			SCOPED_TRACE(bByFileId ? "by FileId" : "by content");
			SWorld world;
			TurnOff(world.Safety, off);
			world.AddTombstone(G1, "/Project/Old/Rock.png", "rock", MakeIdentity(990u), 2500u);
			world.AddMeta("/Project/Old/Rock.png.meta", G1, 2500u);
			SLifecycleSource& moved = world.AddNewFile("/Project/New/Rock.png", "rock", 2500u);
			if (bByFileId)
			{
				moved.Identity = MakeIdentity(990u);
			}
			const SLifecycleResult result = RunAnswering(world);
			const auto moves = findAll(result, ELifecycleAction::MoveMeta);
			ASSERT_EQ(moves.size(), 1u);
			EXPECT_EQ(moves[0]->Guid, G1) << "a paired orphan keeps its GUID";
		}
	}
}

// A FileId match must not depend on whether another file that matches the same orphan by content was hashed yet: the
// same final facts give the same identities.
TEST(LifecycleTest, AFileIdPairingDoesNotDependOnAnotherFilesHash)
{
	const auto build = [] (SWorld& W)
	{
		W.AddTombstone(G1, "/Project/Old/Rock.png", "rock", MakeIdentity(991u), 2500u);
		W.AddMeta("/Project/Old/Rock.png.meta", G1, 2500u);
		W.AddNewFile("/Project/New/Rock.png", "rock", 2500u).Identity = MakeIdentity(991u);
		W.AddNewFile("/Project/Copy/Rock.png", "rock");
	};
	SWorld hashed;
	build(hashed);
	ApplyRepairs(hashed, RunAnswering(hashed));
	ApplyRepairs(hashed, RunAnswering(hashed));

	SWorld withheld;
	build(withheld);
	const SLifecycleResult first = RunLifecycle(withheld.GetFacts());
	EXPECT_EQ(first.NeedContentHash, std::vector<FString> { "/Project/Copy/Rock.png" });
	EXPECT_EQ(Describe(first.Repairs), Sorted({ "MoveMeta /Project/Old/Rock.png.meta -> /Project/New/Rock.png.meta" }));
	ApplyRepairs(withheld, first);
	ApplyRepairs(withheld, RunAnswering(withheld));

	for (const FStringView path : { "/Project/New/Rock.png", "/Project/Copy/Rock.png" })
	{
		SCOPED_TRACE(path);
		ASSERT_NE(hashed.FindSource(path), nullptr);
		ASSERT_NE(withheld.FindSource(path), nullptr);
		EXPECT_TRUE(hashed.FindSource(path)->bMetaOnDisk);
		EXPECT_EQ(hashed.FindSource(path)->RecordGuid, withheld.FindSource(path)->RecordGuid);
	}
	EXPECT_EQ(hashed.FindSource("/Project/New/Rock.png")->RecordGuid, G1);
	EXPECT_NE(hashed.FindSource("/Project/Copy/Rock.png")->RecordGuid, G1);
}

// Each copy of a folder copied twice must reference its own copies, so a loser's map sends every old GUID to the loser
// nearest it, and starts with its own pair.
TEST(LifecycleTest, EachCopyOfAFolderCopiedTwiceMapsToItsOwnCopies)
{
	const auto build = [] (SWorld& W)
	{
		W.AddPaired("/Project/Pack/Model.gltf", G1, "model");
		W.AddPaired("/Project/Pack/Skin.png", G2, "skin");
		for (const FStringView folder : { "/Project/Pack 1", "/Project/Pack 2" })
		{
			const FString model = FString::Format("{}/Model.gltf", folder);
			const FString skin = FString::Format("{}/Skin.png", folder);
			W.AddPaired(model, G1, "model").LastEventMs = W.Now - Settled;
			W.AddMeta(GetMetaPath(model), G1, Settled, false);
			W.AddPaired(skin, G2, "skin").LastEventMs = W.Now - Settled;
			W.AddMeta(GetMetaPath(skin), G2, Settled, false);
		}
		W.CommittedOnDisk = { "/Project/Pack/Model.gltf.meta", "/Project/Pack/Skin.png.meta" };
	};
	SWorld world;
	build(world);
	const SLifecycleResult result = RunAnswering(world);
	ASSERT_EQ(result.Repairs.size(), 4u);

	const auto expected = [] (const SGuid& Old, FStringView Path) { return MakeDedupeGuid("Demo", Old, Path); };
	const auto find = [&result] (FStringView MetaPath) -> const SLifecycleAction*
	{
		for (const SLifecycleAction& action : result.Repairs)
		{
			if (action.Path == MetaPath)
			{
				return &action;
			}
		}
		return nullptr;
	};
	std::set<SGuid> minted;
	for (const FStringView folder : { "/Project/Pack 1", "/Project/Pack 2" })
	{
		const FString model = FString::Format("{}/Model.gltf", folder);
		const FString skin = FString::Format("{}/Skin.png", folder);
		const SLifecycleAction* modelAction = find(GetMetaPath(model));
		const SLifecycleAction* skinAction = find(GetMetaPath(skin));
		ASSERT_NE(modelAction, nullptr);
		ASSERT_NE(skinAction, nullptr);
		EXPECT_EQ(modelAction->Kind, ELifecycleAction::ReGuid);
		EXPECT_EQ(modelAction->Guid, expected(G1, model));
		EXPECT_EQ(skinAction->Guid, expected(G2, skin));
		const std::vector<std::pair<SGuid, SGuid>> modelRemap { { G1, expected(G1, model) }, { G2, expected(G2, skin) } };
		const std::vector<std::pair<SGuid, SGuid>> skinRemap { { G2, expected(G2, skin) }, { G1, expected(G1, model) } };
		EXPECT_EQ(modelAction->Remap, modelRemap) << "its own pair first, then its folder's copy of every other asset";
		EXPECT_EQ(skinAction->Remap, skinRemap);
		minted.insert(modelAction->Guid);
		minted.insert(skinAction->Guid);
	}
	EXPECT_EQ(minted.size(), 4u) << "two losers of one GUID stay apart";

	// DerivedAutoGuids off: no GUID, and the same map of stand-ins for the planner to replace with what it mints.
	SWorld random;
	build(random);
	random.Safety.bDerivedAutoGuids = false;
	const SLifecycleResult standIns = RunAnswering(random);
	ASSERT_EQ(standIns.Repairs.size(), 4u);
	for (uint32 i = 0u; i < 4u; ++i)
	{
		EXPECT_FALSE(standIns.Repairs[i].Guid.IsValid());
		EXPECT_EQ(standIns.Repairs[i].Path, result.Repairs[i].Path);
		EXPECT_EQ(standIns.Repairs[i].Remap, result.Repairs[i].Remap);
	}
}

// Two unrelated copies that land in one batch: Model's meta refers to Rock, but Rock was never copied into B, so B's Model
// copy must keep referring to the original Rock.
TEST(LifecycleTest, UnrelatedCopiesInOneBatchNeverReferenceEachOther)
{
	SWorld world;
	world.AddPaired("/Project/A/Rock.png", G1, "rock");
	world.AddPaired("/Project/B/Model.gltf", G2, "model");
	world.AddPaired("/Project/A/Rock2.png", G1, "rock").LastEventMs = world.Now - Settled;
	world.AddMeta("/Project/A/Rock2.png.meta", G1, Settled, false);
	world.AddPaired("/Project/B/Model2.gltf", G2, "model").LastEventMs = world.Now - Settled;
	world.AddMeta("/Project/B/Model2.gltf.meta", G2, Settled, false);
	world.CommittedOnDisk = { "/Project/A/Rock.png.meta", "/Project/B/Model.gltf.meta" };
	const SLifecycleResult result = RunAnswering(world);
	ASSERT_EQ(result.Repairs.size(), 2u);
	for (const SLifecycleAction& action : result.Repairs)
	{
		SCOPED_TRACE(action.Path);
		ASSERT_EQ(action.Remap.size(), 1u) << "only its own pair";
		EXPECT_EQ(action.Remap[0].second, action.Guid);
	}
}

// A loser that is not rewritten with the others keeps its old GUID, so no other loser's map may point at its new one:
// that GUID would be held by no meta, and with DerivedAutoGuids off it never would be.
TEST(LifecycleTest, ALoserThatStaysAsItIsIsInNoOtherLosersMap)
{
	for (const bool bBroken : { true, false })
	{
		SCOPED_TRACE(bBroken ? "a broken loser" : "a read-only loser");
		SWorld world;
		world.AddPaired("/Project/Pack/Model.gltf", G1, "model");
		world.AddPaired("/Project/Pack/Skin.png", G2, "skin");
		world.AddPaired("/Project/Pack (копія)/Model.gltf", G1, "model").LastEventMs = world.Now - Settled;
		world.AddMeta("/Project/Pack (копія)/Model.gltf.meta", G1, Settled, false);
		SLifecycleSource& skin = world.AddPaired("/Project/Pack (копія)/Skin.png", G2, "skin");
		skin.LastEventMs = world.Now - Settled;
		SLifecycleMeta& skinMeta = world.AddMeta("/Project/Pack (копія)/Skin.png.meta", G2, Settled, false);
		if (bBroken)
		{
			skinMeta.Parse = EMetaParseState::Broken;
			skinMeta.SalvagedGuids = { G2 };
		}
		else
		{
			skin.bWritable = false;
			skinMeta.bWritable = false;
		}
		world.CommittedOnDisk = { "/Project/Pack/Model.gltf.meta", "/Project/Pack/Skin.png.meta" };
		const SLifecycleResult result = RunAnswering(world);
		ASSERT_EQ(result.Repairs.size(), 1u) << DescribeExactly(result);
		EXPECT_EQ(result.Repairs[0].Path, FStringView("/Project/Pack (копія)/Model.gltf.meta"));
		ASSERT_EQ(result.Repairs[0].Remap.size(), 1u) << "only its own pair";
		for (const SLifecycleAction& fixIt : result.FixIts)
		{
			EXPECT_EQ(fixIt.Remap.size(), 1u) << "a fix-it maps to no repair's GUID either";
		}
	}
}


// ---- What a run asks for ----

TEST(LifecycleTest, ARunThatNeedsAHashProposesNothingUntilItHasIt)
{
	SWorld world;
	world.AddNewFile("/Project/A.png", "a");
	const SLifecycleResult first = RunLifecycle(world.GetFacts());
	EXPECT_EQ(first.NeedContentHash, std::vector<FString> { "/Project/A.png" });
	EXPECT_TRUE(first.Repairs.empty());
	EXPECT_TRUE(first.Problems.empty()) << "a repair is coming: not an error";
	EXPECT_EQ(Describe(first.States), Sorted({ "/Project/A.png PendingMeta" }));

	world.Answer(first);
	const SLifecycleResult second = RunLifecycle(world.GetFacts());
	EXPECT_TRUE(second.NeedContentHash.empty());
	EXPECT_EQ(Describe(second.Repairs), Sorted({ "CreateMeta /Project/A.png" }));
}

TEST(LifecycleTest, AFileIdMatchNeedsNoHash)
{
	SWorld world;
	world.AddTombstone(G1, "/Project/Old/Rock.png", "rock", MakeIdentity(960u), Settled);
	world.AddMeta("/Project/Old/Rock.png.meta", G1);
	world.AddNewFile("/Project/New/Rock.png", "rock").Identity = MakeIdentity(960u);
	const SLifecycleResult result = RunLifecycle(world.GetFacts());
	EXPECT_TRUE(result.NeedContentHash.empty());
	ASSERT_EQ(result.Repairs.size(), 1u);
	EXPECT_EQ(result.Repairs[0].Guid, G1) << "same GUID";
}

// --quick never hashes: what is an error in every outcome must still be reported to it.
TEST(LifecycleTest, ACallerThatNeverHashesStillSeesErrors)
{
	SWorld world;
	world.AddNewFile("/Project/A.png", "a").bWritable = false;
	const SLifecycleResult result = RunLifecycle(world.GetFacts());
	EXPECT_EQ(result.NeedContentHash, std::vector<FString> { "/Project/A.png" });
	EXPECT_EQ(Describe(result.Problems), Sorted({ "META001 /Project/A.png" }));
	EXPECT_TRUE(result.FixIts.empty()) << "the fix-it's GUID needs the hash";
}

TEST(LifecycleTest, DuplicatesWaitForTheCommittedFacts)
{
	const auto build = [] (SWorld& W)
	{
		W.AddPaired("/Project/Pack/Skin.png", G2, "skin");
		W.AddPaired("/Project/Pack (копія)/Skin.png", G2, "skin").LastEventMs = W.Now - Settled;
		W.AddMeta("/Project/Pack (копія)/Skin.png.meta", G2, Settled, false);
	};
	SWorld world;
	build(world);
	const SLifecycleResult asking = RunLifecycle(world.GetFacts());
	EXPECT_TRUE(asking.bNeedCommitted);
	EXPECT_TRUE(asking.Repairs.empty());
	EXPECT_TRUE(asking.Problems.empty());

	// Nothing committed: the smallest folded path keeps the GUID, and "pack (" sorts before "pack/".
	world.Committed = std::vector<FString> {};
	EXPECT_EQ(Describe(RunLifecycle(world.GetFacts()).Repairs), Sorted({ "ReGuid /Project/Pack/Skin.png.meta" }));

	world.Committed = std::vector<FString> { "/Project/Pack/Skin.png.meta" };
	EXPECT_EQ(Describe(RunLifecycle(world.GetFacts()).Repairs), Sorted({ "ReGuid /Project/Pack (копія)/Skin.png.meta" }));
}

// A read-only run (`--ci`, a guest) with two copies here: one of them loses whoever wins, so META003 is certain, and a
// caller that never answers bNeedCommitted, or runs under a hold, or while the copy settles, still sees it.
TEST(LifecycleTest, ADuplicateNoRepairCanRewriteIsReportedBeforeTheWinnerIsKnown)
{
	const auto build = [] (SWorld& W)
	{
		W.AddPaired("/Project/Pack/Skin.png", G2, "skin").bWritable = false;
		SLifecycleSource& copy = W.AddPaired("/Project/Pack (копія)/Skin.png", G2, "skin");
		copy.LastEventMs = W.Now - Settled;
		copy.bWritable = false;
		W.AddMeta("/Project/Pack (копія)/Skin.png.meta", G2, Settled, false).bWritable = false;
	};
	const std::vector<FString> both = Sorted({ "META003 /Project/Pack (копія)/Skin.png", "META003 /Project/Pack/Skin.png" });

	SWorld asking;
	build(asking);
	const SLifecycleResult unanswered = RunLifecycle(asking.GetFacts());
	EXPECT_TRUE(unanswered.bNeedCommitted);
	EXPECT_EQ(Describe(unanswered.Problems), both);
	EXPECT_TRUE(unanswered.FixIts.empty()) << "the fix-it waits for the winner";

	SWorld held;
	build(held);
	held.bVcsHold = true;
	EXPECT_EQ(Describe(RunLifecycle(held.GetFacts()).Problems), both);

	SWorld settling;
	build(settling);
	settling.FindSource("/Project/Pack (копія)/Skin.png")->LastEventMs = settling.Now - 100u;
	EXPECT_EQ(Describe(RunLifecycle(settling.GetFacts()).Problems), both);

	// One copy here that no repair can rewrite may be the winner: nothing is certain until the committed facts are in.
	SWorld one;
	build(one);
	one.FindSource("/Project/Pack/Skin.png")->bWritable = true;
	EXPECT_TRUE(RunLifecycle(one.GetFacts()).Problems.empty());

	// Once the winner is known, the loser alone reports, with its fix-it.
	SWorld answered;
	build(answered);
	answered.CommittedOnDisk = { "/Project/Pack/Skin.png.meta" };
	const SLifecycleResult resolved = RunAnswering(answered);
	EXPECT_EQ(Describe(resolved.Problems), Sorted({ "META003 /Project/Pack (копія)/Skin.png" }));
	EXPECT_EQ(Describe(resolved.FixIts), Sorted({ "ReGuid /Project/Pack (копія)/Skin.png.meta" }));
}

// One run covers one mount: a duplicate's member in another mount is that mount's run's to re-GUID.
TEST(LifecycleTest, AnotherMountsLoserIsLeftToItsOwnRun)
{
	SWorld world;
	world.AddPaired("/Plugin.Trees/Bark.png", G1, "bark").LastEventMs = world.Now - Settled;
	world.Live.push_back(SLiveGuid { G1, "/Project/Bark.png.meta" });
	world.Committed = std::vector<FString> {};
	const SLifecycleResult winner = RunAnswering(world);
	EXPECT_TRUE(winner.Repairs.empty()) << "/Plugin.Trees sorts first and keeps it; /Project's copy is not this run's";
	EXPECT_TRUE(winner.Problems.empty());

	SWorld loser;
	loser.AddPaired("/Project/Bark.png", G1, "bark").LastEventMs = loser.Now - Settled;
	loser.Live.push_back(SLiveGuid { G1, "/Plugin.Trees/Bark.png.meta" });
	loser.Committed = std::vector<FString> {};
	EXPECT_EQ(Describe(RunAnswering(loser).Repairs), Sorted({ "ReGuid /Project/Bark.png.meta" }));
}

TEST(LifecycleTest, AutoResolveDuplicatesOffKeepsADuplicateAFixItEvenForACommand)
{
	SWorld world;
	world.bCommand = true;
	world.Safety.bAutoResolveDuplicates = false;
	world.AddPaired("/Project/Pack/Skin.png", G2, "skin");
	world.AddPaired("/Project/Pack (копія)/Skin.png", G2, "skin");
	world.AddMeta("/Project/Pack (копія)/Skin.png.meta", G2, 0u, false);
	world.CommittedOnDisk = { "/Project/Pack/Skin.png.meta" };
	const SLifecycleResult result = RunAnswering(world);
	EXPECT_TRUE(result.Repairs.empty());
	EXPECT_EQ(Describe(result.FixIts), Sorted({ "ReGuid /Project/Pack (копія)/Skin.png.meta" }));
	EXPECT_EQ(Describe(result.Problems), Sorted({ "META003 /Project/Pack (копія)/Skin.png" }));
}

TEST(LifecycleTest, ABrokenLoserIsNeverRewritten)
{
	SWorld world;
	world.AddPaired("/Project/A.png", G1, "a");
	world.AddPaired("/Project/B.png", G1, "b");
	SLifecycleMeta& meta = world.AddMeta("/Project/B.png.meta", SGuid {}, Settled, false);
	meta.Parse = EMetaParseState::Broken;
	meta.SalvagedGuids = { G1 };
	world.CommittedOnDisk = {};
	const SLifecycleResult result = RunAnswering(world);
	EXPECT_TRUE(result.Repairs.empty());
	EXPECT_EQ(Describe(result.Problems), Sorted({ "META003 /Project/B.png", "META004 /Project/B.png.meta" }));
	EXPECT_TRUE(result.FixIts.empty());
}

TEST(LifecycleTest, KeepAliasBOffersTheRecordsGuidFirst)
{
	SWorld world;
	world.AddPaired("/Project/A.png", G1, "a");
	SLifecycleMeta& meta = world.AddMeta("/Project/A.png.meta", SGuid {}, Settled, false);
	meta.Parse = EMetaParseState::Broken;
	meta.SalvagedGuids = { G2, G1 };
	const SLifecycleResult result = RunAnswering(world);
	ASSERT_EQ(result.FixIts.size(), 2u);
	EXPECT_EQ(result.FixIts[0].Guid, G1);
	EXPECT_EQ(result.FixIts[0].Aliases, std::vector<SGuid> { G2 });
	EXPECT_EQ(result.FixIts[1].Guid, G2);
	EXPECT_EQ(result.FixIts[1].Aliases, std::vector<SGuid> { G1 });
}


// ---- Time, the VCS hold, commands ----

TEST(LifecycleTest, ARunWakesWhenTheFirstDelayRunsOut)
{
	SWorld world;
	world.AddNewFile("/Project/A.png", "a", 500u);
	world.AddPaired("/Project/B.png", G1, "b");
	world.LoseMeta("/Project/B.png", 200u);
	const SLifecycleResult result = RunAnswering(world);
	EXPECT_TRUE(result.Repairs.empty());
	EXPECT_EQ(result.WakeAtMs, world.Now - 200u + world.Times.RemoveGraceMs) << "the grace ends before the creation delay";

	SWorld quiet;
	quiet.AddPaired("/Project/B.png", G1, "b");
	EXPECT_EQ(RunAnswering(quiet).WakeAtMs, ~0ull) << "nothing pending: never";

	// A clock set back: the event lies ahead of NowMs, so the delay runs long, never short.
	SWorld back;
	back.AddNewFile("/Project/A.png", "a").LastEventMs = back.Now + 100u;
	const SLifecycleResult ahead = RunAnswering(back);
	EXPECT_TRUE(ahead.Repairs.empty());
	EXPECT_EQ(ahead.WakeAtMs, back.Now + 100u + back.Times.MetaCreationDelayMs);
}

// A move outside the editor: the orphan's grace (RemoveGraceMs, 1000) ends before the moved file's creation delay
// (MetaCreationDelayMs, 1500). In between, the orphan the waiting file matches by FileId is not reported.
TEST(LifecycleTest, AnOrphanAWaitingFileMatchesByFileIdIsNotReported)
{
	SWorld world;
	world.AddTombstone(G1, "/Project/Old/Rock.png", "rock", MakeIdentity(985u), 1200u);
	world.AddMeta("/Project/Old/Rock.png.meta", G1, 1200u);
	world.AddNewFile("/Project/New/Rock.png", "rock", 1200u).Identity = MakeIdentity(985u);
	const SLifecycleResult waiting = RunAnswering(world);
	EXPECT_TRUE(waiting.Repairs.empty());
	EXPECT_TRUE(waiting.Problems.empty()) << DescribeExactly(waiting);
	EXPECT_EQ(Describe(waiting.States), Sorted({ "/Project/New/Rock.png PendingMeta" }));
	EXPECT_EQ(waiting.WakeAtMs, world.Now - 1200u + world.Times.MetaCreationDelayMs);

	world.Now += 300u;
	const SLifecycleResult paired = RunAnswering(world);
	EXPECT_EQ(Describe(paired.Repairs), Sorted({ "MoveMeta /Project/Old/Rock.png.meta -> /Project/New/Rock.png.meta" }));
	EXPECT_TRUE(paired.Problems.empty());
}

TEST(LifecycleTest, ATimeNotTrackedIsADelayAlreadyOver)
{
	SWorld world;
	world.AddNewFile("/Project/A.png", "a").LastEventMs = 0u;
	world.AddPaired("/Project/B.png", G1, "b");
	world.LoseMeta("/Project/B.png", 0u);
	world.FindSource("/Project/B.png")->MetaGoneMs = 0u;
	world.Verified.push_back(G1);
	EXPECT_EQ(Describe(RunAnswering(world).Repairs), Sorted({ "CreateMeta /Project/A.png", "RestoreMeta /Project/B.png" }));
}

// §4.7 slow sync: the source lands, git is still writing, the meta lands seconds later. No step creates a meta.
TEST(LifecycleTest, ASlowCheckoutNeverCreatesAMeta)
{
	SWorld world;
	world.bVcsHold = true;
	world.AddNewFile("/Project/Big.png", "big", 0u);

	for (const uint64 later : { 100u, 3000u, 10000u })
	{
		world.Now = T0 + later;
		const SLifecycleResult result = RunAnswering(world);
		EXPECT_TRUE(result.Repairs.empty()) << later;
		EXPECT_TRUE(result.Problems.empty()) << later << ": a repair is coming once the hold ends";
		EXPECT_EQ(Describe(result.States), Sorted({ "/Project/Big.png PendingMeta" })) << later;
	}

	EXPECT_FALSE(world.FindSource("/Project/Big.png")->Content.has_value()) << "never hashed while the VCS may still be writing it";

	world.Now = T0 + 12000u;
	world.bVcsHold = false;
	SLifecycleSource& source = *world.FindSource("/Project/Big.png");
	source.bMetaOnDisk = true;
	source.RecordGuid = G1;
	world.Live.push_back(SLiveGuid { G1, "/Project/Big.png.meta" });
	world.AddMeta("/Project/Big.png.meta", G1, 0u, false);
	const SLifecycleResult arrived = RunAnswering(world);
	EXPECT_TRUE(arrived.Repairs.empty());
	EXPECT_EQ(Describe(arrived.States), Sorted({ "/Project/Big.png Paired", "/Project/Big.png.meta Paired" }));
}

// Without a hold (WaitForVcs off folds to bVcsHold false), a meta that arrives inside the creation delay still pairs:
// the delay alone covers a fast checkout, and the repair waits only for it.
TEST(LifecycleTest, WithoutAHoldOnlyTheLifecyclesOwnDelaysWait)
{
	SWorld world;
	world.AddNewFile("/Project/Big.png", "big", 0u);
	world.Now = T0 + 1000u;
	EXPECT_TRUE(RunAnswering(world).Repairs.empty());
	world.Now = T0 + 1500u;
	EXPECT_EQ(Describe(RunAnswering(world).Repairs), Sorted({ "CreateMeta /Project/Big.png" }));

	world.bVcsHold = true;
	EXPECT_TRUE(RunAnswering(world).Repairs.empty()) << "the same facts under a hold propose nothing";
}

TEST(LifecycleTest, AHoldStillReportsWhatNoRepairWouldFix)
{
	SWorld world;
	world.bVcsHold = true;
	world.AddNewFile("/Project/A.png", "a").bWritable = false;
	world.AddNewFile("/Project/B.png", "b");
	const SLifecycleResult result = RunAnswering(world);
	EXPECT_TRUE(result.Repairs.empty());
	EXPECT_EQ(Describe(result.Problems), Sorted({ "META001 /Project/A.png" }));
}

TEST(LifecycleTest, ACommandHasNoDelaysAndNoHold)
{
	SWorld world;
	world.bCommand = true;
	world.bVcsHold = true;
	world.AddNewFile("/Project/A.png", "a", 0u);
	world.AddPaired("/Project/B.png", G1, "b");
	world.LoseMeta("/Project/B.png", 0u);
	world.Verified.push_back(G1);
	world.AddTombstone(G2, "/Project/Gone.png", "gone", MakeIdentity(970u), 0u);
	world.AddMeta("/Project/Gone.png.meta", G2, 0u);
	const SLifecycleResult result = RunAnswering(world);
	EXPECT_EQ(Describe(result.Repairs), Sorted({ "CreateMeta /Project/A.png", "RestoreMeta /Project/B.png" }));
	EXPECT_EQ(Describe(result.Problems), Sorted({ "META002 /Project/Gone.png.meta" }));
	EXPECT_EQ(result.WakeAtMs, ~0ull);
}

TEST(LifecycleTest, AnLfsPointerIsNeverHashedEvenForACommand)
{
	SWorld world;
	world.bCommand = true;
	world.AddNewFile("/Project/Big.psd", "version https://git-lfs.github.com/spec/v1").bLfsPointer = true;
	const SLifecycleResult result = RunLifecycle(world.GetFacts());
	EXPECT_TRUE(result.NeedContentHash.empty());
	EXPECT_TRUE(result.Repairs.empty());
	EXPECT_TRUE(result.FixIts.empty());
	EXPECT_EQ(Describe(result.Problems), Sorted({ "VCS004 /Project/Big.psd" }));
}

TEST(LifecycleTest, OnlyFilesThatGetAMetaAreConsidered)
{
	SWorld world;
	world.AddNewFile("/Project/Model.bin", "buffer").Class = EFileClass::Companion;
	world.AddNewFile("/Project/notes.txt", "notes").Class = EFileClass::Unclaimed;
	world.AddNewFile("/Project/font.ttf", "font").Class = EFileClass::Raw;
	const SLifecycleResult result = RunAnswering(world);
	EXPECT_EQ(Describe(result.Repairs), Sorted({ "CreateMeta /Project/font.ttf" }));
	EXPECT_EQ(Describe(result.States), Sorted({ "/Project/font.ttf PendingMeta" }));
	EXPECT_TRUE(GetsMeta(EFileClass::Asset));
	EXPECT_TRUE(GetsMeta(EFileClass::Raw));
	EXPECT_FALSE(GetsMeta(EFileClass::Companion));
	EXPECT_FALSE(GetsMeta(EFileClass::Unclaimed));
	EXPECT_FALSE(GetsMeta(EFileClass::Excluded));
	EXPECT_FALSE(GetsMeta(EFileClass::Meta));
}
