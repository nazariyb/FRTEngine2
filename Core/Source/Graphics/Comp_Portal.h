#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Math/Math.h"


namespace frt::graphics
{
// Portal quad component. Authored on an entity to mark a window/doorway opening that
// the sky-NEE shadow ray pre-filter will allow through. World-space orientation: Normal
// is the facing direction; Edge1/Edge2 are world-space half-extent vectors of the rect.
//
// Direction is intentionally NOT derived from entity transform — STransform lacks axis
// helpers today, and explicit fields make portal placement obvious in code.
struct FRT_CORE_API Comp_Portal
{
	Vector3f Normal   = { 0.0f, 0.0f, 1.0f };
	Vector3f Edge1    = { 1.0f, 0.0f, 0.0f }; // half-extent along right
	Vector3f Edge2    = { 0.0f, 1.0f, 0.0f }; // half-extent along up
	bool     bEnabled = true;
};
}
