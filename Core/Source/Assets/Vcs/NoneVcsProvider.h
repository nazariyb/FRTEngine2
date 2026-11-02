#pragma once

#include <atomic>

#include "Assets/Vcs/VcsProvider.h"


/**
 * No version control (design §12.9): writes need no permission beyond the file's own attribute, moves are plain renames,
 * deletes go to the Recycle Bin, and it never reports a state or a committed fact, so duplicates resolve by smallest path
 * (§4.6). It spawns nothing. A VCS the editor does not know may still be there, which is why a case-only rename warns.
 */
namespace frt::assets
{
class FRT_CORE_API CNoneVcsProvider final : public IVcsProvider
{
public:
	bool Detect (const FString& MountRoot) override { (void)MountRoot; return true; }
	bool IsBusy () const override { return false; }
	SVcsResult CheckOut (std::span<const FString> Files) override;
	SVcsResult Add (std::span<const FString> Files) override { (void)Files; return {}; }
	SVcsResult Move (std::span<const SVcsMove> Moves) override;
	SVcsResult Delete (std::span<const FString> Files) override;
	SVcsResult GetCommittedFacts (std::span<const FString> Files, std::vector<bool>& OutCommitted) override;
	SVcsResult StatusOf (std::span<const FString> Files, std::vector<SVcsStatus>& Out) override;
	SVcsResult GetStaged (std::vector<FString>& OutFiles) override { OutFiles.clear(); return {}; }
	[[nodiscard]] FStringView GetName () const override { return "None"; }

private:
	std::atomic<bool> bCaseWarned = false;
};
}
