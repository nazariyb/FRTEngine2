#pragma once

#include <optional>

#include "Core.h"
#include "Containers/String.h"


namespace frt::text
{
/**
 * Uri with its %XX escapes replaced by the bytes they stand for; a '%' without two hex digits after
 * it is kept. nullopt when there is no escape, or one stands for NUL (the name would end there).
 *
 * glTF names its buffers and images by URI, and exporters escape what a URI cannot hold (Blender
 * writes "Brick Wall.png" as "Brick%20Wall.png", "Текстура.png" as "%D0%A2%D0%B5..."). Callers decode
 * only to find a file the name as written does not name: a file called "Raw%41.png" is still found as
 * itself. A pure text function, so the runtime's model loading (Graphics/AssimpWideIO.h, Model.cpp) and
 * the registry's glTF Analyze (Assets/Pipeline/Importers) share it without the one depending on the other.
 */
[[nodiscard]] FRT_CORE_API std::optional<FString> DecodeUriEscapes (FStringView Uri);
}
