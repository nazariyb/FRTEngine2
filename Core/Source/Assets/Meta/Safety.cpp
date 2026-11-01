#include "Assets/Meta/Safety.h"

#include "Config/Config.h"


namespace frt::assets
{
SAssetEditorSettings SAssetEditorSettings::FromConfig (const config::CConfig& Config)
{
	SAssetEditorSettings settings;
	settings.bWaitForVcs = Config.Get("AssetsEditor", "WaitForVcs", settings.bWaitForVcs);
	settings.VcsProvider = Config.Get("AssetsEditor", "VcsProvider", settings.VcsProvider.View());
	settings.MetaCreationDelayMs = Config.Get("AssetsEditor", "MetaCreationDelayMs", settings.MetaCreationDelayMs);
	settings.RemoveGraceMs = Config.Get("AssetsEditor", "RemoveGraceMs", settings.RemoveGraceMs);
	settings.VcsSettleMs = Config.Get("AssetsEditor", "VcsSettleMs", settings.VcsSettleMs);
	settings.OrphanPairWindowMs = Config.Get("AssetsEditor", "OrphanPairWindowMs", settings.OrphanPairWindowMs);
	return settings;
}
}
