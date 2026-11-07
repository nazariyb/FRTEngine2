#include "Assets/Vcs/NoneVcsProvider.h"

#include "Assets/Vcs/VcsFileOps.h"
#include "Log/Log.h"


namespace frt::assets
{
SVcsResult CNoneVcsProvider::CheckOut (std::span<const FString> Files)
{
	return CheckOutFiles(Files);
}

SVcsResult CNoneVcsProvider::Move (std::span<const SVcsMove> Moves)
{
	return RunMoves(Moves, [this] (const SVcsMove& One)
	{
		if (IsCaseOnlyRename(One) && !bCaseWarned.exchange(true))
		{
			log::Warning(log::Assets, "A version control system the editor does not know may miss a case-only rename on a "
				"case-insensitive file system.");
		}
		return MoveFilePlain(One);
	});
}

SVcsResult CNoneVcsProvider::Delete (std::span<const FString> Files)
{
	return TrashFiles(Files);
}

SVcsResult CNoneVcsProvider::GetCommittedFacts (std::span<const FString> Files, std::vector<bool>& OutCommitted)
{
	OutCommitted.assign(Files.size(), false);
	return {};
}

SVcsResult CNoneVcsProvider::StatusOf (std::span<const FString> Files, std::vector<SVcsStatus>& Out)
{
	Out.assign(Files.size(), SVcsStatus {});
	return {};
}
}
