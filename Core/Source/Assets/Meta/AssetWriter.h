#pragma once

#include <filesystem>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/Meta/OpJournal.h"
#include "Assets/Vcs/VcsProvider.h"
#include "Containers/String.h"
#include "Functional/FunctionRef.h"
#include "Hash/StableHash.h"


/**
 * The one way an asset and its meta are written (design §4.6, §4.7, §12.9): one op at a time, journalled before the first
 * disk touch, through the VCS provider as one operation. A refused call fails the op before any byte is written (VCS002).
 *
 * One instance and one log per owner root, because a project and /Engine are separate roots. It makes no watcher call and
 * is safe on a worker. ExpectWrite must be called on the watcher's owner thread, so the caller does that from
 * ForEachExpectedWrite before submitting the op. That serves verifying subscribers only: the registry subscribes without
 * verifying and sees the write as a Changed stat, so an echo costs it one folder rescan and no repair.
 */
namespace frt::assets
{
class CMountTable;

struct SAssetWriterDesc
{
	const CMountTable* Mounts = nullptr;
	std::filesystem::path OwnerRoot;
	IVcsProvider* Vcs = nullptr;
	bool bHoldsLock = false;       // false: every op is refused
};

class FRT_CORE_API CAssetWriter
{
public:
	explicit CAssetWriter (SAssetWriterDesc Desc);

	[[nodiscard]] SVcsResult Run (const SAssetOp& Op);
	// Finishes an op the log holds unfinished, through the provider as well. False when one could not be finished.
	bool RollForward ();

private:
	SAssetWriterDesc Desc;
};

// The files of Op whose bytes are in hand, to register with CFileWatcherService::ExpectWrite. A Move has none: nothing is
// hashed just to rename it.
FRT_CORE_API void ForEachExpectedWrite (const SAssetOp& Op, TFunctionRef<void (FStringView MountPath, const hash::SContentHash& Hash, uint64 Size)> Visit);
}
