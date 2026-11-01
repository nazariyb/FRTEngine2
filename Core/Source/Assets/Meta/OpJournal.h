#pragma once

#include <vector>

#include "CoreTypes.h"
#include "Enum.h"
#include "Assets/AssetId.h"
#include "Containers/String.h"
#include "Hash/StableHash.h"


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
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EAssetOpStep,
	FRT_ENUM_ENTRY(frt::assets::EAssetOpStep, WriteFile),
	FRT_ENUM_ENTRY(frt::assets::EAssetOpStep, CopyFile),
	FRT_ENUM_ENTRY(frt::assets::EAssetOpStep, Move),
	FRT_ENUM_ENTRY(frt::assets::EAssetOpStep, Trash));
