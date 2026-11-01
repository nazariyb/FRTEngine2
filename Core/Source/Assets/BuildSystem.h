#pragma once

#include "Assets/AssetId.h"


/**
 * The build system's own version: the "BuildSystem" field of every builder's KeyBlob (design §7.4).
 *
 * A builder's Version GUID (§7.3) invalidates that builder's artifacts when its code changes; this one
 * invalidates every artifact of every builder at once, on every machine that shares a DDC. It exists
 * for the rare change that alters what a key means for all builders together - a KeyBlob convention,
 * the record contract between builders and the cache - while the store's FORMAT stays readable (a
 * FORMAT change refuses the old store instead, DerivedDataCache.h). Bumping it is never needed for one
 * builder's change: bump that builder's GUID. A new value comes from `FRT.bat assets newguid`, never by
 * editing a byte of this one (two branches bumping the same byte would collide).
 */
namespace frt::assets
{
// ce6d5c7337ad4744bc819ba8f40ed235
inline constexpr SGuid BuildSystemVersion = SGuid { { 0xce, 0x6d, 0x5c, 0x73, 0x37, 0xad, 0x47, 0x44, 0xbc, 0x81, 0x9b,
	0xa8, 0xf4, 0x0e, 0xd2, 0x35 } };
}
