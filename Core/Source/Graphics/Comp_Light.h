#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Graphics/SColor.h"
#include "Math/Math.h"


namespace frt::graphics
{
// Explicit light component for entities without (or alongside) a mesh.
// Light collector picks these up and emits SLight entries into the per-frame light list.
// Direction / Edge1 / Edge2 are world-space, authored on the component directly (entity rotation
// is intentionally NOT applied — keeps the math simple while STransform lacks axis helpers).
struct FRT_CORE_API Comp_Light
{
	enum class EKind : uint32
	{
		Point       = 0u,
		Directional = 1u,
		AreaQuad    = 2u,
	};

	EKind    Kind      = EKind::Point;
	SColor   Color     = { 1.0f, 1.0f, 1.0f, 1.0f };
	float    Intensity = 10.0f;

	// Directional / AreaQuad: unit vector pointing FROM the light TO the scene.
	Vector3f Direction = { 0.0f, -1.0f, 0.0f };

	// AreaQuad only: world-space half-extent vectors. Area = 4 * |Edge1| * |Edge2|.
	Vector3f Edge1     = { 1.0f, 0.0f, 0.0f };
	Vector3f Edge2     = { 0.0f, 0.0f, 1.0f };

	bool     bEnabled  = true;
};
}
