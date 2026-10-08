#pragma once

#include <yaml-cpp/yaml.h>

#include "Core.h"
#include "Containers/String.h"


namespace frt::assets
{
// True when yaml-cpp writes Text back unchanged: strict UTF-8 and no noncharacter.
//
// yaml-cpp writes plain scalars verbatim, but routes single- and double-quoted ones through its
// own decoder (GetNextCodePointAndAdvance, emitterutils.cpp), which replaces ill-formed bytes,
// surrogates (WTF-8 path names) and the noncharacters (U+FDD0..FDEF, xxFFFE, xxFFFF) with U+FFFD -
// and whether it quotes a value depends on the value. A path or a name would be altered silently,
// so writers check first and refuse instead (CYamlWriteArchive). Reading needs no check: loaded
// text keeps its bytes.
[[nodiscard]] FRT_CORE_API bool IsYamlStorable (FStringView Text) noexcept;
}


namespace YAML
{
// Node.as<FString>(), Node = FString. Only scalars decode; a map or a sequence is no text.
template <>
struct convert<frt::FString>
{
	static Node encode (const frt::FString& Value)
	{
		frt_assert(frt::assets::IsYamlStorable(Value))   // writers check first (CYamlWriteArchive); this is the backstop
		return Node(Value.ToStdString());
	}

	static bool decode (const Node& InNode, frt::FString& Out)
	{
		if (!InNode.IsScalar())
		{
			return false;
		}
		Out = frt::FStringView(InNode.Scalar());
		return true;
	}
};
}
