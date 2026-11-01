#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"


namespace frt::config
{
class CConfig;
}


/**
 * The two kinds of knob of the meta lifecycle (design §4.8, §3.5.3), kept apart because they answer to different people.
 *
 * SSafetySwitches decides which identities and keys the tools write, so every machine must agree: it comes from a
 * mount's Config/Assets.yml `Safety:` section, and a switch's default is its safe position. Switching one off removes
 * the automatic action, never the detection: the condition is still reported, with the repair as a fix-it. Commands a
 * person runs (`new-meta`, `fix`, `migrate metas`) ignore the switches, except that DerivedAutoGuids still chooses the
 * GUID kind.
 *
 * SAssetEditorSettings tunes a mechanism and never turns off an identity rule, so it can differ per user: it comes from
 * `[AssetsEditor]` through the CConfig layers, Core/Config/Engine.ini for the defaults and Local/UserSettings.ini per user.
 */
namespace frt::assets
{
struct SSafetySwitches
{
	bool bAutoRepairMetas = true;         // off: new files stop at META001, lost metas at MissingMeta, each with a fix-it
	bool bDerivedAutoGuids = true;        // off: NewRandom instead of MakeAutoGuid
	bool bAutoResolveDuplicates = true;   // off: META003 with a "re-GUID" fix-it
	bool bOrphanPairing = true;           // off: matches become "relink" fix-its
	bool bBuilderSourceGuard = true;      // not the lifecycle's: hosts hand it to SetBuilderSourceGuardEnabled (BuilderGuard.h)
	bool bIdLedger = false;               // opt-in; parsed only until M5
	bool bGuestProcesses = false;         // opt-in; a second process runs read-only instead of exiting (§5.9)
};

struct SAssetEditorSettings
{
	bool bWaitForVcs = true;              // hold automatic repairs while the VCS provider is busy, and for VcsSettleMs after
	FString VcsProvider = "Auto";         // Auto | None | Git (MakeVcsProvider, Vcs/VcsProvider.h)
	uint32 MetaCreationDelayMs = 1500u;   // a new file waits this long for its meta before the owner creates one
	uint32 RemoveGraceMs = 1000u;         // also the file watcher's
	uint32 VcsSettleMs = 1500u;           // also the file watcher's
	uint32 OrphanPairWindowMs = 5000u;    // pairing an orphan meta with a new file by content, only within this window

	// The one reader of [AssetsEditor]; SFileWatcherSettings::FromConfig takes its two values from here. A key absent
	// from every layer keeps the default above.
	[[nodiscard]] FRT_CORE_API static SAssetEditorSettings FromConfig (const config::CConfig& Config);
};
}
