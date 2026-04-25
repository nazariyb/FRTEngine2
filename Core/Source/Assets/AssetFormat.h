#pragma once

#include "CoreTypes.h"


namespace frt::assets
{
enum class EAssetFormat : uint8
{
	Unknown = 0u,
	LegacyText,
	Yaml,
	Binary
};
}
