#pragma once

#include "Core.h"
#include "Assets/Archive.h"


namespace frt
{
struct SColor;
}

namespace frt::graphics
{
struct SMaterial;

// Format-agnostic visitors. Implementations live in MaterialSerialize.cpp.
// Adding a new archive backend (binary, etc.) does NOT require touching this file.

FRT_CORE_API bool Serialize (assets::IReadArchive& Ar, SMaterial& M);
FRT_CORE_API void Serialize (assets::IWriteArchive& Ar, const SMaterial& M);

FRT_CORE_API bool Serialize (assets::IReadArchive& Ar, SColor& C);
FRT_CORE_API void Serialize (assets::IWriteArchive& Ar, const SColor& C);

// Current format version that the writer emits. Reader accepts any version
// it understands and migrates as needed.
inline constexpr int32 MaterialAssetVersion = 2;
}
