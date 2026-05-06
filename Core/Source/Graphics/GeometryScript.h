#pragma once

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/Array.h"
#include "Math/Math.h"
#include "Math/Vector2.h"


namespace frt::graphics
{
struct SMesh;

// Procedural geometry script — ordered list of typed ops that build a planar mesh.
// MVP: axis-aligned rectangles in local XY plane, AddBase or Cut. Multiple holes OK.
// Mesh produced lies in z=0, normal +Z. Orient via entity transform.
//
// Future: extra shapes (circle, polygon), 3D extrusion, rotation per op, fillets.
struct FRT_CORE_API SGeometryScript
{
	enum class EShape : uint8
	{
		Rect = 0u,
	};


	enum class EMergeOp : uint8
	{
		AddBase = 0u, // becomes outer ring (only first AddBase used by MVP)
		Cut     = 1u, // becomes a hole
	};


	struct SOp
	{
		EShape Shape = EShape::Rect;
		EMergeOp Op = EMergeOp::AddBase;
		Vector2f Center = { 0.0f, 0.0f };
		Vector2f HalfExtent = { 1.0f, 1.0f };
	};


	TArray<SOp> Ops;
	// Reserved for future extrusion. Ignored by current planar builder.
	float Thickness = 0.0f;
};


namespace mesh
{
	// Tessellates an SGeometryScript into an SMesh in the local XY plane.
	// Algorithm: collect all X / Y edges from outer + holes, form a grid, emit each grid cell
	// whose centre is inside outer && outside every hole. Produces more triangles than necessary
	// but is closed-form for axis-aligned ops, so no triangulation library is needed.
	FRT_CORE_API SMesh BuildFromScript (const SGeometryScript& Script);
}
}
