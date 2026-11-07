#include "Assets/Vcs/VcsProvider.h"

#include "Assets/Vcs/GitVcsProvider.h"
#include "Assets/Vcs/NoneVcsProvider.h"
#include "Log/Log.h"


namespace frt::assets
{
std::unique_ptr<IVcsProvider> MakeVcsProvider (FStringView Setting, const FString& MountRoot)
{
	const bool bNone = Setting.Equals<ESearchCase::IgnoreCaseAscii>("None");
	const bool bGit = Setting.Equals<ESearchCase::IgnoreCaseAscii>("Git");
	if (!bNone && !bGit && !Setting.Equals<ESearchCase::IgnoreCaseAscii>("Auto"))
	{
		log::Warning(log::Assets, "[AssetsEditor] VcsProvider={} is not Auto, None or Git: using Auto.", Setting);
	}
	if (!bNone)
	{
		std::unique_ptr<CGitVcsProvider> git = std::make_unique<CGitVcsProvider>();
		if (git->Detect(MountRoot))
		{
			return git;
		}
		if (bGit)
		{
			log::Warning(log::Assets, "[AssetsEditor] VcsProvider=Git, but '{}' cannot use it: no version control for it.",
				MountRoot);
		}
	}
	std::unique_ptr<CNoneVcsProvider> none = std::make_unique<CNoneVcsProvider>();
	none->Detect(MountRoot);
	return none;
}
}
