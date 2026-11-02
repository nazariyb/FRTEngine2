#pragma once

#include "Assets/Meta/LifecycleTypes.h"
#include "Assets/Registry/RegistryTypes.h"


/**
 * The meta lifecycle as a pure state machine (design §4.6's outside-the-editor table, §4.7, §4.8). RunLifecycle, declared
 * in LifecycleTypes.h with its facts and proposals, is defined in Lifecycle.cpp; this header states what a caller must
 * put in the facts, and what each fact makes of a file, so that F4's gathering and D4's decisions agree on one contract.
 *
 * What the caller passes, for one mount (LifecycleTypes.h says why one):
 *	- Sources: every file of a class that gets a meta (GetsMeta) that a batch dirtied, every one not Paired, and the source
 *	  of every meta in Metas whose source exists, whatever its class. A meta whose source is absent from Sources is taken
 *	  for an orphan; one whose source is there with a class that gets no meta is left alone (META007 is RunValidation's).
 *	- Metas: orphans, broken metas, metas a batch dirtied. For every GUID that LiveGuids holds more than once, every member
 *	  in this mount: its meta in Metas, or its source in Sources with the meta on disk. A member of the group found in
 *	  neither is taken for another mount's, which that mount's run resolves.
 *	- Tombstones: this mount's. LiveGuids: every mount's metas on disk, so a GUID live in another mount is a duplicate too.
 *	- Every time is on the lifecycle's clock (LifecycleTypes.h). A LastEventMs, MetaGoneMs or FirstSeenMs of 0 is a time
 *	  the caller does not track: its delay is over, and a new file's arrival is taken as NowMs.
 *	- Times.bWaitForVcs is not read: the caller folds it into bVcsHold, which also holds while providers are being
 *	  detected (§4.8: with WaitForVcs off, repairs wait only for the lifecycle's own delays).
 *
 * What a source becomes:
 *	- meta on disk, readable: Paired. Its spelling differs: FixMetaCase once the source has been quiet for RemoveGraceMs
 *	  (no switch gates it; a read-only file makes it META006 with the fix-it). A broken meta: Broken, META004, never
 *	  overwritten, and with two salvaged GUIDs one "keep A, alias B" fix-it per GUID, the record's first.
 *	- no meta, a known identity (RecordGuid valid): after RemoveGraceMs from MetaGoneMs, MissingMeta. Its GUID live in
 *	  another meta (a meta swap, or a meta moved on purpose): META008 with "new GUID" (CreateMeta) and, when that meta is an
 *	  orphan here, "relink"; never an automatic rewrite. Else a verified copy: RestoreMeta [AutoRepairMetas]. Else META008
 *	  with "create from registry" (CreateMeta with the record's GUID); "restore from VCS" is the provider's command, not
 *	  an action.
 *	- no meta, no identity: PendingMeta until MetaCreationDelayMs after its last event. Then an orphan meta whose
 *	  tombstone has the file's FileId, or else its content inside OrphanPairWindowMs of the removal, claimed by this file
 *	  alone: MoveMeta [OrphanPairing]. A FileId is definitive: an orphan one file claims by FileId is that file's, and no
 *	  other file's content match to it counts, not even as a suggestion. Claimed by several (by FileId, or by content where
 *	  no FileId claims), or the switch off and such a match: MissingMeta, META008, a META009 relink suggestion per match, and
 *	  never CreateMeta. A content match outside the window: META009 only, switch on or off. Otherwise CreateMeta
 *	  [AutoRepairMetas] with MakeCreateMetaGuid [DerivedAutoGuids]; a file that cannot get one has META001 (an error: §4.7's
 *	  ErrorMissingMeta) with the fix-it. A Git LFS pointer: PendingMeta and VCS004, never hashed, paired or created.
 * What a meta becomes: Paired beside its source; Broken when it does not parse; an orphan after RemoveGraceMs from its
 * tombstone (or from FirstSeenMs without one), META002, never deleted, unless a repair pairs it. While a new file still in
 * its creation delay matches it by FileId (a move: the grace ends before the delay), it is not reported. A meta beside an
 * excluded file, or beside one of a class that gets no meta, is left alone (META007 is RunValidation's).
 * Duplicates: once every member here has been quiet for MetaCreationDelayMs, and the committed metas are known (asked
 * through bNeedCommitted), the winner is a committed member, else the smallest folded source path, ties by bytes. Each
 * loser here gets ReGuid [AutoResolveDuplicates] with MakeDedupeGuid [DerivedAutoGuids]; a broken loser is never
 * rewritten; otherwise META003 with the fix-it. Before the winner is known (a hold, a member settling, the committed facts
 * not answered), a group with two members here that no repair can rewrite already reports META003 on each of them, since
 * one of them loses whoever wins; the fix-it comes with the winner.
 *
 * The rules that hold everywhere:
 *	- A repair is proposed only for a writable file (and a writable meta, where one is moved or rewritten). Otherwise the
 *	  repair is a fix-it and the condition a problem. A problem is reported only where no repair is coming: a pending
 *	  repair is not an error.
 *	- bVcsHold (unless bCommand) proposes nothing automatic, reports nothing a repair would fix once it ends, and asks for
 *	  no hash: no content decision is made while the VCS may still be writing the files.
 *	- bCommand (a person ran it): no delays and no hold; AutoRepairMetas ignored. DerivedAutoGuids picks the GUID kind, and
 *	  OrphanPairing and AutoResolveDuplicates decide, as for automatic repairs: off, a relink or a dedupe stays a fix-it even
 *	  for `fix`.
 *	- A run that puts a path in NeedContentHash proposes no repair for any new file that content would decide; a run that
 *	  sets bNeedCommitted resolves no duplicate. Problems that are errors in every outcome are still reported, so a caller
 *	  that never hashes (`validate --quick`) or never asks the provider still sees them.
 *	- Derived GUIDs re-roll over LiveGuids and the GUIDs this run's repairs propose (restores, creates, then losers, each in
 *	  folded path order). Tombstones never count: they are this machine's memory, and every machine must derive alike.
 *	- A ReGuid's Remap starts with its own pair (old GUID, new GUID), then the batch's other old GUIDs in GUID order, each
 *	  mapped to its loser nearest this one (longest common folder, then smallest folded path), so each copy of a folder
 *	  copied twice references its own copies. Only losers rewritten together count (repairs map to repairs, fix-its to
 *	  fix-its, a broken loser to nothing), and only those under this loser's copy root: a common folder deeper than the
 *	  one it shares with its own winner, so unrelated copies in one batch never reference each other. With DerivedAutoGuids
 *	  off, Guid is invalid and every new GUID in the Remaps is a stand-in, the derived value: the planner mints NewRandom
 *	  per action and replaces each stand-in, found by the action's own first pair, with what it minted.
 *	- Problems have Subject (the mount path as spelled) and no File: the caller, which knows the mounts, fills it. Every
 *	  output list is in a deterministic order.
 */
namespace frt::assets
{
// The file classes that get a meta (§3.2): imported assets and raw files. Companions, unclaimed and excluded files do not.
[[nodiscard]] constexpr bool GetsMeta (EFileClass Class) noexcept
{
	return Class == EFileClass::Asset || Class == EFileClass::Raw;
}
}
