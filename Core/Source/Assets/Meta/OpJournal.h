#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Enum.h"
#include "Assets/AssetId.h"
#include "Containers/String.h"
#include "Hash/StableHash.h"
#include "Platform/FileSystem.h"


/**
 * A multi-file command as data (design §4.6, §4.8): planned before any byte moves, so a crash halfway leaves a log that
 * start-up can finish. Paths are mount paths, so a moved checkout still replays. Meta bytes travel inline, since the
 * journal must be able to write them again; a source's content never does (a copy names its source and is redone).
 *
 * Replay is idempotent, step by step: a WriteFile whose target already has Hash is skipped, a Move whose From is gone and
 * whose To exists is skipped, a Trash whose file is gone is skipped. For a Move, "gone" compares the on-disk spelling
 * byte-exactly, by a listing of the folder: after a case-only Move the old spelling still opens on NTFS, and running that
 * Move again would have `git mv` refuse a name the index no longer holds. PlannedGuids are what the command pre-allocated,
 * so that no identity it promised is lost to the crash.
 *
 * A step names a file inside a mount, so `Schemas/Migrations.yml`, which lies outside every mount, is not a step: `migrate`
 * writes it with WriteFileAtomic after its op completes. A crash in between leaves the project behind, and running `migrate
 * metas` again skips the metas that exist and then records itself.
 *
 * Steps, by Kind:
 *   WriteFile   Path: the file; Bytes: what it holds afterwards (metas only); Hash: hash::HashContent(Bytes)
 *   CopyFile    Path: the source to copy; ToPath: the new file; Hash: the source's content when the op was planned
 *   Move        Path: the file as spelled on disk; ToPath: its new path (a case-only rename included)
 *   Trash       Path: the file, which goes to the provider's Delete (the Recycle Bin under None and Git)
 */
namespace frt::assets
{
enum class EAssetOpStep : uint8 { WriteFile, CopyFile, Move, Trash };

struct SAssetOpStep
{
	EAssetOpStep Kind = EAssetOpStep::WriteFile;
	FString Path;
	FString ToPath;
	std::vector<uint8> Bytes;   // WriteFile: metas only
	hash::SContentHash Hash;
};

struct SAssetOp
{
	SGuid Id;
	FString Verb;
	std::vector<SAssetOpStep> Steps;
	std::vector<SGuid> PlannedGuids;
};


/**
 * Local/Assets/OpJournal.log (GetOpJournalPath), one per owner root, written by CAssetWriter only (AssetWriter.h).
 *
 * The log holds at most the op in flight. Begin appends a Begin frame, one frame per step and nothing else in a single
 * append, and flushes it to disk before the writer touches anything: a process killed or a machine that bugchecks before
 * that flush has changed nothing, so an op whose frames are incomplete is dropped as a torn tail. End appends the End
 * frame, flushes it, and empties the log. A crash between the two leaves a finished op, which a reader treats as nothing
 * to do. Frames are Assets/RecordFrames.h's: checksummed, so a torn or damaged tail never reads as a step.
 *
 * Not thread-safe: the writer runs one op at a time.
 */
struct SOpJournalEntry
{
	SAssetOp Op;
	// Mount paths the writer hands to the provider's Add once the steps are done: the files the op creates and their pairs.
	// Kept here so that roll-forward adds them too, which a provider that tracks adds (Perforce) needs.
	std::vector<FString> AddFiles;
};

struct SOpJournalContents
{
	std::optional<SOpJournalEntry> Unfinished;   // begun, every step logged, no End
	bool bEmpty = true;                          // no byte at all
	bool bDroppedTail = false;                   // bytes after the last whole frame, or an op whose frames were incomplete
};

// What File holds. A missing file is empty. False only when it exists and cannot be read (OutError says why).
[[nodiscard]] FRT_CORE_API bool ReadOpJournal (const std::filesystem::path& File, SOpJournalContents& Out, FString* OutError = nullptr);

class FRT_CORE_API COpJournal
{
public:
	explicit COpJournal (std::filesystem::path InFile);

	COpJournal (const COpJournal&) = delete;
	COpJournal& operator= (const COpJournal&) = delete;

	[[nodiscard]] const std::filesystem::path& GetPath () const noexcept { return File; }

	// The Begin frame and one frame per step, durable when it returns true. Creates Local/Assets if needed.
	bool Begin (const SOpJournalEntry& Entry, FString* OutError = nullptr);
	// The End frame of the op Begin logged, flushed, and then the log emptied.
	bool End (const SGuid& OpId, FString* OutError = nullptr);
	// Empties the log: an op that changed nothing and is given up, or a log with nothing unfinished in it.
	bool Clear (FString* OutError = nullptr);

private:
	std::filesystem::path File;
	platform::CAppendFile Writer;
};

// The frames Begin appends, as bytes: for tests that tear or damage a log on purpose.
[[nodiscard]] FRT_CORE_API std::vector<uint8> EncodeOpJournalBegin (const SOpJournalEntry& Entry);
[[nodiscard]] FRT_CORE_API std::vector<uint8> EncodeOpJournalEnd (const SGuid& OpId);
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EAssetOpStep,
	FRT_ENUM_ENTRY(frt::assets::EAssetOpStep, WriteFile),
	FRT_ENUM_ENTRY(frt::assets::EAssetOpStep, CopyFile),
	FRT_ENUM_ENTRY(frt::assets::EAssetOpStep, Move),
	FRT_ENUM_ENTRY(frt::assets::EAssetOpStep, Trash));
