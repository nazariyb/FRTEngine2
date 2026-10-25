#pragma once

#include <utility>
#include <vector>

#include "CoreTypes.h"
#include "Assets/Pipeline/ImporterTable.h"
#include "Assets/Registry/RegistryTypes.h"
#include "Containers/String.h"


/**
 * What the rules engine decides for one file (design §3.5.2, §5.4 step 3): its importer, class, cook rule,
 * chunk, rule labels and resolved settings, each with where it came from. The rules engine produces it per
 * file; the importers' Analyze and the registry service consume it. Plain data, safe to build on workers.
 */
namespace frt::assets
{
struct SEffectiveFileSettings
{
	const SImporterDesc* Importer = nullptr;   // null: no importer (Unclaimed or Excluded)
	EFileClass Class = EFileClass::Unclaimed;
	bool bQuiet = false;                       // listed in Scan.Quiet: an Unclaimed file raises no RULE002
	ECookRule CookRule = ECookRule::Default;
	uint16 Chunk = 0;
	std::vector<FString> RuleLabels;
	SResolvedSettings Settings;
	// Per effective field name ("Importer", "Cook", "Chunk", "Labels", "ColorSpace", ...), for `explain`.
	std::vector<std::pair<FString, SProvenance>> Provenance;
};
}
