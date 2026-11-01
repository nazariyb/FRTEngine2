// M4's shared declarations: what every later piece compiles against, so that none invents its own. The §4.8 switch
// defaults and the §3.5.3 timing defaults (and Engine.ini agreeing with them, since deleting a line there must change
// nothing); the one reader of [AssetsEditor], which the file watcher also takes its values from; the §3.2 key order; the
// names of every new enum, and that the stored enums only ever grew at their end; each new diagnostic code's text; the
// meta path rules; the salt of each kind of mount; and the rule that the VCS provider's IsRewritingTree falls back to
// IsBusy. Nothing here calls a function a later piece defines.
#include <gtest/gtest.h>

#include <array>
#include <span>
#include <vector>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/FileWatcherService.h"
#include "Assets/Meta/AssetRepairs.h"
#include "Assets/Meta/AssetWriter.h"
#include "Assets/Meta/LifecycleTypes.h"
#include "Assets/Meta/MetaPaths.h"
#include "Assets/Meta/MetaTypes.h"
#include "Assets/Meta/OpJournal.h"
#include "Assets/Meta/Safety.h"
#include "Assets/Registry/RegistryDelta.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Assets/Vcs/VcsProvider.h"
#include "Config/Config.h"
#include "Diagnostics/Diagnostics.h"
#include "EnginePaths.h"
#include "Platform/Process.h"

using namespace frt;
using namespace frt::assets;


namespace
{
// The names of a reflected enum, which are also its order: value I has name I.
template <typename E, size_t N>
void ExpectNames (const std::array<FStringView, N>& Expected)
{
	const auto& entries = enum_::TEnumTraits<E>::Entries;
	ASSERT_EQ(entries.size(), N);
	for (size_t i = 0u; i < N; ++i)
	{
		const auto& [value, name] = entries[i];
		EXPECT_EQ(static_cast<size_t>(value), i) << Expected[i];
		EXPECT_EQ(name, Expected[i]);
	}
}

// Busy on demand, every other call a no-op: only what the interface itself defines is under test.
class CBusyProvider : public IVcsProvider
{
public:
	bool bBusy = false;

	bool Detect (const FString&) override { return true; }
	bool IsBusy () const override { return bBusy; }
	SVcsResult CheckOut (std::span<const FString>) override { return {}; }
	SVcsResult Add (std::span<const FString>) override { return {}; }
	SVcsResult Move (std::span<const SVcsMove>) override { return {}; }
	SVcsResult Delete (std::span<const FString>) override { return {}; }
	SVcsResult GetCommittedFacts (std::span<const FString>, std::vector<bool>&) override { return {}; }
	SVcsResult StatusOf (std::span<const FString>, std::vector<SVcsStatus>&) override { return {}; }
	SVcsResult GetStaged (std::vector<FString>&) override { return {}; }
};

// A provider whose hold is narrower than its busy state, as Git's is.
class COpenMergeProvider : public CBusyProvider
{
public:
	bool IsRewritingTree () const override { return false; }
};

SMount MakeMount (EMountKind Kind, FStringView Name)
{
	SMount mount;
	mount.Kind = Kind;
	mount.Name = FString(Name);
	return mount;
}
}


// ---------------- Switches and settings ----------------

TEST(MetaDeclarationsTest, SafetySwitchesStartInTheirSafePositions)
{
	const SSafetySwitches safety;
	EXPECT_TRUE(safety.bAutoRepairMetas);
	EXPECT_TRUE(safety.bDerivedAutoGuids);
	EXPECT_TRUE(safety.bAutoResolveDuplicates);
	EXPECT_TRUE(safety.bOrphanPairing);
	EXPECT_TRUE(safety.bBuilderSourceGuard);
	EXPECT_FALSE(safety.bIdLedger) << "opt-in";
	EXPECT_FALSE(safety.bGuestProcesses) << "opt-in";
}

TEST(MetaDeclarationsTest, EditorSettingsDefaultToTheDesignsValues)
{
	const SAssetEditorSettings settings;
	EXPECT_TRUE(settings.bWaitForVcs);
	EXPECT_EQ(settings.VcsProvider, "Auto");
	EXPECT_EQ(settings.MetaCreationDelayMs, 1500u);
	EXPECT_EQ(settings.RemoveGraceMs, 1000u);
	EXPECT_EQ(settings.VcsSettleMs, 1500u);
	EXPECT_EQ(settings.OrphanPairWindowMs, 5000u);
}

TEST(MetaDeclarationsTest, AKeyAbsentFromEveryLayerKeepsTheDefault)
{
	const config::CConfig empty;
	const SAssetEditorSettings settings = SAssetEditorSettings::FromConfig(empty);
	const SAssetEditorSettings defaults;
	EXPECT_EQ(settings.bWaitForVcs, defaults.bWaitForVcs);
	EXPECT_EQ(settings.VcsProvider, defaults.VcsProvider);
	EXPECT_EQ(settings.MetaCreationDelayMs, defaults.MetaCreationDelayMs);
	EXPECT_EQ(settings.RemoveGraceMs, defaults.RemoveGraceMs);
	EXPECT_EQ(settings.VcsSettleMs, defaults.VcsSettleMs);
	EXPECT_EQ(settings.OrphanPairWindowMs, defaults.OrphanPairWindowMs);
}

TEST(MetaDeclarationsTest, EveryAssetsEditorKeyIsReadFromTheConfig)
{
	config::CConfig config;
	config.SetOverride("AssetsEditor", "WaitForVcs", "0");
	config.SetOverride("AssetsEditor", "VcsProvider", "Git");
	config.SetOverride("AssetsEditor", "MetaCreationDelayMs", "20");
	config.SetOverride("AssetsEditor", "RemoveGraceMs", "30");
	config.SetOverride("AssetsEditor", "VcsSettleMs", "40");
	config.SetOverride("AssetsEditor", "OrphanPairWindowMs", "50");
	const SAssetEditorSettings settings = SAssetEditorSettings::FromConfig(config);
	EXPECT_FALSE(settings.bWaitForVcs);
	EXPECT_EQ(settings.VcsProvider, "Git");
	EXPECT_EQ(settings.MetaCreationDelayMs, 20u);
	EXPECT_EQ(settings.RemoveGraceMs, 30u);
	EXPECT_EQ(settings.VcsSettleMs, 40u);
	EXPECT_EQ(settings.OrphanPairWindowMs, 50u);

	// The one reader: the file watcher's two values come through it.
	const SFileWatcherSettings watcher = SFileWatcherSettings::FromConfig(config);
	EXPECT_EQ(watcher.RemoveGraceMs, 30u);
	EXPECT_EQ(watcher.VcsSettleMs, 40u);
}

TEST(MetaDeclarationsTest, EngineIniStatesTheCompiledDefaults)
{
	config::CConfig config;
	ASSERT_TRUE(config.LoadLayer(config::EConfigLayer::Engine, paths::GetEngineConfigPath()));
	for (const FStringView key : { "WaitForVcs", "VcsProvider", "MetaCreationDelayMs", "RemoveGraceMs", "VcsSettleMs", "OrphanPairWindowMs" })
	{
		EXPECT_TRUE(config.Has("AssetsEditor", key)) << key;
	}
	const SAssetEditorSettings settings = SAssetEditorSettings::FromConfig(config);
	const SAssetEditorSettings defaults;
	EXPECT_EQ(settings.bWaitForVcs, defaults.bWaitForVcs);
	EXPECT_EQ(settings.VcsProvider, defaults.VcsProvider);
	EXPECT_EQ(settings.MetaCreationDelayMs, defaults.MetaCreationDelayMs);
	EXPECT_EQ(settings.RemoveGraceMs, defaults.RemoveGraceMs);
	EXPECT_EQ(settings.VcsSettleMs, defaults.VcsSettleMs);
	EXPECT_EQ(settings.OrphanPairWindowMs, defaults.OrphanPairWindowMs);
}


// ---------------- The meta's vocabulary ----------------

TEST(MetaDeclarationsTest, TopLevelKeysAreInTheDesignsOrder)
{
	const std::vector<FStringView> keys(std::begin(MetaTopLevelKeys), std::end(MetaTopLevelKeys));
	EXPECT_EQ(keys, (std::vector<FStringView> { "FormatVersion", "Guid", "Importer", "SettingsVersion", "Settings", "Targets", "SubAssets",
		"Labels", "FormerNames", "Aliases", "FormerPaths", "Cook", "Chunk", "RebuildNonce" }));
}

TEST(MetaDeclarationsTest, AnEmptyMetaHasOnlyItsRequiredKeys)
{
	const SAssetMeta meta;
	EXPECT_EQ(meta.FormatVersion, 1u);
	EXPECT_FALSE(meta.Guid.IsValid());
	EXPECT_TRUE(meta.Importer.IsEmpty());
	EXPECT_FALSE(meta.SettingsVersion.has_value());
	EXPECT_TRUE(meta.Settings.IsEmpty());
	EXPECT_TRUE(meta.Targets.IsEmpty());
	EXPECT_TRUE(meta.SubAssets.IsEmpty());
	EXPECT_TRUE(meta.Labels.empty());
	EXPECT_TRUE(meta.FormerNames.empty());
	EXPECT_TRUE(meta.Aliases.empty());
	EXPECT_TRUE(meta.FormerPaths.empty());
	EXPECT_FALSE(meta.Cook.has_value()) << "absent, not Auto: a present key is an explicit override";
	EXPECT_FALSE(meta.Chunk.has_value());
	EXPECT_FALSE(meta.RebuildNonce.has_value());
	EXPECT_TRUE(meta.UnknownKeys.empty());

	const SMetaParseResult result;
	EXPECT_EQ(result.State, EMetaParseState::Broken) << "nothing read is nothing to trust";
	EXPECT_TRUE(result.SalvagedGuids.empty());
}

TEST(MetaDeclarationsTest, TheNewEnumsHaveTheirNames)
{
	ExpectNames<EMetaParseState>(std::array<FStringView, 2u> { "Ok", "Broken" });
	ExpectNames<ELifecycleAction>(std::array<FStringView, 6u> { "CreateMeta", "RestoreMeta", "MoveMeta", "ReGuid", "RemapRefs", "KeepAliasB" });
	ExpectNames<EAssetOpStep>(std::array<FStringView, 4u> { "WriteFile", "CopyFile", "Move", "Trash" });
	ExpectNames<EVcsError>(std::array<FStringView, 7u> { "None", "Busy", "ReadOnly", "LockedByOther", "NotLatest", "Offline", "Failed" });
	ExpectNames<EVcsState>(std::array<FStringView, 9u> { "Untracked", "Clean", "Modified", "Added", "Deleted", "Conflicted", "OpenedByOther",
		"LockedByOther", "OutOfDate" });
}

// Their values are in snapshots and journals, so a value is appended, never renumbered. ErrorMissingMeta is not one:
// §4.7's state is PendingMeta plus an error-severity META001.
TEST(MetaDeclarationsTest, TheStoredEnumsOnlyGrewAtTheirEnd)
{
	ExpectNames<EFileClass>(std::array<FStringView, 6u> { "Asset", "Companion", "Raw", "Unclaimed", "Excluded", "Meta" });
	ExpectNames<EProblemSource>(std::array<FStringView, 6u> { "Scan", "Rules", "Analyze", "Registry", "Unclaimed", "Lifecycle" });
	ExpectNames<EMetaState>(std::array<FStringView, 5u> { "Paired", "PendingMeta", "Orphan", "Broken", "MissingMeta" });
}

TEST(MetaDeclarationsTest, EveryNewCodeSpellsItsOwnName)
{
	const auto expect = [] (uint32 Code, FStringView Text)
	{
		EXPECT_TRUE(IsValidDiagnosticCode(Code)) << Text;
		EXPECT_EQ(FormatDiagnosticCode(Code), Text);
	};
	expect(Meta001, "META001");
	expect(Meta002, "META002");
	expect(Meta003, "META003");
	expect(Meta004, "META004");
	expect(Meta005, "META005");
	expect(Meta006, "META006");
	expect(Meta007, "META007");
	expect(Meta008, "META008");
	expect(Meta009, "META009");
	expect(Vcs001, "VCS001");
	expect(Vcs002, "VCS002");
	expect(Vcs003, "VCS003");
	expect(Vcs004, "VCS004");
	expect(Mig001, "MIG001");
	expect(Mig002, "MIG002");
}


// ---------------- Paths and salts ----------------

TEST(MetaDeclarationsTest, AMetasPathIsItsSourcesWithTheSuffix)
{
	EXPECT_EQ(GetMetaPath("/Project/Rock.png"), "/Project/Rock.png.meta");
	EXPECT_EQ(GetMetaPath("/Project/Гра 游戏/Камінь.png"), "/Project/Гра 游戏/Камінь.png.meta");
	EXPECT_EQ(GetMetaSource("/Project/Rock.png.meta"), FStringView("/Project/Rock.png"));
	EXPECT_EQ(GetMetaSource(GetMetaPath("/Project/Гра 游戏/Камінь.png").View()), FStringView("/Project/Гра 游戏/Камінь.png"));
	EXPECT_EQ(GetMetaSource("/Project/Rock.png.META"), FStringView("/Project/Rock.png")) << "wrong case is still its meta (META006)";
	EXPECT_EQ(GetMetaSource("Rock.png.meta"), FStringView("Rock.png"));

	EXPECT_FALSE(GetMetaSource("/Project/Rock.png").has_value());
	EXPECT_FALSE(GetMetaSource("/Project/meta").has_value());
	EXPECT_FALSE(GetMetaSource("/Project/.meta").has_value());
	EXPECT_FALSE(GetMetaSource(".meta").has_value());
	EXPECT_FALSE(GetMetaSource("").has_value());
}

TEST(MetaDeclarationsTest, EachKindOfMountHasItsSalt)
{
	EXPECT_EQ(GetMountAssetSalt(MakeMount(EMountKind::Engine, "Engine"), "Demo"), "FRT.Engine") << "whatever project opened it";
	EXPECT_EQ(GetMountAssetSalt(MakeMount(EMountKind::Project, "Project"), "Demo"), "Demo");
	EXPECT_EQ(GetMountAssetSalt(MakeMount(EMountKind::Plugin, "Plugin.Trees"), "Demo"), "Demo") << "a plugin uses its project's";
}


// ---------------- The lifecycle's and the writer's types ----------------

TEST(MetaDeclarationsTest, ALifecycleResultAsksForNothingAndNeverWakes)
{
	const SLifecycleResult result;
	EXPECT_TRUE(result.Repairs.empty());
	EXPECT_TRUE(result.FixIts.empty());
	EXPECT_TRUE(result.NeedContentHash.empty());
	EXPECT_FALSE(result.bNeedCommitted);
	EXPECT_EQ(result.WakeAtMs, ~0ull);

	const SLifecycleFacts facts;
	EXPECT_FALSE(facts.Committed.has_value()) << "not asked yet, which is not the same as nothing committed";
	EXPECT_FALSE(facts.bCommand);
	EXPECT_FALSE(facts.bVcsHold);

	const SLifecycleSource source;
	EXPECT_FALSE(source.Content.has_value()) << "hashes are lazy";
	EXPECT_EQ(source.Stored, EMetaState::PendingMeta);
	EXPECT_FALSE(source.bWritable) << "nothing is writable until the caller says so";
	EXPECT_EQ(source.MetaGoneMs, 0u);
}

TEST(MetaDeclarationsTest, TheLifecycleIsOffUntilAHostAsksForIt)
{
	const SAssetRegistryDesc registry;
	EXPECT_EQ(registry.Lifecycle, nullptr);
	const SLifecycleDesc lifecycle;
	EXPECT_FALSE(lifecycle.bRepairs) << "report only until the owner says otherwise";
	EXPECT_TRUE(lifecycle.Providers.empty());

	const SRepairOptions options;
	EXPECT_TRUE(options.bCommand);
	EXPECT_FALSE(options.bDryRun);
	EXPECT_TRUE(options.Paths.empty());

	const SAssetWriterDesc writer;
	EXPECT_FALSE(writer.bHoldsLock) << "a writer refuses until it holds the lock";
}

TEST(MetaDeclarationsTest, ToolProcessDescriptionsDefaultToARunThatDidNotHappen)
{
	const platform::SToolProcessDesc desc;
	EXPECT_EQ(desc.TimeoutMs, 30000u);
	const platform::SToolProcessResult result;
	EXPECT_FALSE(result.bStarted);
	EXPECT_FALSE(result.bTimedOut);
	EXPECT_EQ(result.ExitCode, -1);
}


// ---------------- The VCS interface ----------------

TEST(MetaDeclarationsTest, TheWatchersHoldFollowsBusyUnlessAProviderNarrowsIt)
{
	CBusyProvider plain;
	EXPECT_FALSE(plain.IsRewritingTree());
	plain.bBusy = true;
	EXPECT_TRUE(plain.IsRewritingTree());

	COpenMergeProvider openMerge;
	openMerge.bBusy = true;
	EXPECT_TRUE(openMerge.IsBusy()) << "an open merge holds repairs";
	EXPECT_FALSE(openMerge.IsRewritingTree()) << "and never hot reload";
}

TEST(MetaDeclarationsTest, AVcsResultDefaultsToSuccess)
{
	const SVcsResult result;
	EXPECT_EQ(result.Error, EVcsError::None);
	const SVcsStatus status;
	EXPECT_EQ(status.State, EVcsState::Untracked);
}
