#include "Graphics/GeometryScript.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Graphics/Mesh.h"
#include "Graphics/MeshGeneration.h"


namespace frt::graphics::mesh
{
namespace
{
	struct SAabb2
	{
		float MinX = 0.f, MaxX = 0.f, MinY = 0.f, MaxY = 0.f;
	};


	static SAabb2 OpAabb (const SGeometryScript::SOp& Op)
	{
		SAabb2 r;
		r.MinX = Op.Center.x - Op.HalfExtent.x;
		r.MaxX = Op.Center.x + Op.HalfExtent.x;
		r.MinY = Op.Center.y - Op.HalfExtent.y;
		r.MaxY = Op.Center.y + Op.HalfExtent.y;
		return r;
	}

	static bool PointInside (float X, float Y, const SAabb2& A)
	{
		return X >= A.MinX && X <= A.MaxX && Y >= A.MinY && Y <= A.MaxY;
	}

	static SVertex MakeVertex (float X, float Y, const SAabb2& OuterAabb)
	{
		const float du = OuterAabb.MaxX - OuterAabb.MinX;
		const float dv = OuterAabb.MaxY - OuterAabb.MinY;
		SVertex v = {};
		v.Position = Vector3f(X, Y, 0.0f);
		v.Uv = {
			du > 1e-6f ? (X - OuterAabb.MinX) / du : 0.0f,
			dv > 1e-6f ? (Y - OuterAabb.MinY) / dv : 0.0f
		};
		v.Normal = Vector3f(0.0f, 0.0f, 1.0f);
		v.Tangent = Vector3f(1.0f, 0.0f, 0.0f);
		v.Bitangent = Vector3f(0.0f, 1.0f, 0.0f);
		v.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		return v;
	}
}


SMesh BuildFromScript (const SGeometryScript& Script)
{
	SMesh result;

	// 1. Find outer rect (first AddBase op of shape Rect). Without it, return empty mesh.
	const SGeometryScript::SOp* outerOp = nullptr;
	for (uint32 i = 0; i < Script.Ops.Count(); ++i)
	{
		const auto& op = Script.Ops[i];
		if (op.Op == SGeometryScript::EMergeOp::AddBase && op.Shape == SGeometryScript::EShape::Rect)
		{
			outerOp = &op;
			break;
		}
	}
	if (!outerOp)
	{
		return result;
	}

	const SAabb2 outer = OpAabb(*outerOp);

	// 2. Collect hole AABBs (clipped to outer; ops outside outer have no effect).
	std::vector<SAabb2> holes;
	holes.reserve(Script.Ops.Count());
	for (uint32 i = 0; i < Script.Ops.Count(); ++i)
	{
		const auto& op = Script.Ops[i];
		if (op.Op != SGeometryScript::EMergeOp::Cut || op.Shape != SGeometryScript::EShape::Rect)
		{
			continue;
		}
		SAabb2 h = OpAabb(op);
		h.MinX = std::max(h.MinX, outer.MinX);
		h.MaxX = std::min(h.MaxX, outer.MaxX);
		h.MinY = std::max(h.MinY, outer.MinY);
		h.MaxY = std::min(h.MaxY, outer.MaxY);
		if (h.MinX < h.MaxX && h.MinY < h.MaxY)
		{
			holes.push_back(h);
		}
	}

	// 3. Collect grid edges. Sorted, deduplicated within an epsilon.
	auto collectSorted = [] (std::vector<float>& Out, float Epsilon)
	{
		std::sort(Out.begin(), Out.end());
		Out.erase(
			std::unique(
				Out.begin(), Out.end(),
				[Epsilon] (float A, float B) { return std::fabs(A - B) < Epsilon; }), Out.end());
	};

	std::vector<float> xs;
	std::vector<float> ys;
	xs.push_back(outer.MinX);
	xs.push_back(outer.MaxX);
	ys.push_back(outer.MinY);
	ys.push_back(outer.MaxY);
	for (const SAabb2& h : holes)
	{
		xs.push_back(h.MinX);
		xs.push_back(h.MaxX);
		ys.push_back(h.MinY);
		ys.push_back(h.MaxY);
	}
	const float epsilon = 1e-5f;
	collectSorted(xs, epsilon);
	collectSorted(ys, epsilon);

	// 4. Emit grid cells whose centre is inside outer && outside every hole.
	auto& V = result.Vertices;
	auto& I = result.Indices;
	V.SetSize<true>(0u);
	I.SetSize<true>(0u);

	for (size_t xi = 0; xi + 1 < xs.size(); ++xi)
	{
		const float x0 = xs[xi];
		const float x1 = xs[xi + 1];
		const float cx = (x0 + x1) * 0.5f;
		for (size_t yi = 0; yi + 1 < ys.size(); ++yi)
		{
			const float y0 = ys[yi];
			const float y1 = ys[yi + 1];
			const float cy = (y0 + y1) * 0.5f;

			// Cell centre rules: must be inside outer rect…
			if (!PointInside(cx, cy, outer))
			{
				continue;
			}
			// …and outside every hole.
			bool inHole = false;
			for (const SAabb2& h : holes)
			{
				if (PointInside(cx, cy, h))
				{
					inHole = true;
					break;
				}
			}
			if (inHole)
			{
				continue;
			}

			// Quad CCW from -X-Y when viewed from +Z (so default-cull renderer sees front face).
			const uint32 baseIndex = V.Count();
			V.Add(MakeVertex(x0, y0, outer));
			V.Add(MakeVertex(x1, y0, outer));
			V.Add(MakeVertex(x1, y1, outer));
			V.Add(MakeVertex(x0, y1, outer));

			I.Add(baseIndex + 0u);
			I.Add(baseIndex + 1u);
			I.Add(baseIndex + 2u);
			I.Add(baseIndex + 0u);
			I.Add(baseIndex + 2u);
			I.Add(baseIndex + 3u);
		}
	}

#ifndef FRT_HEADLESS
	// Match the upload pattern of the other generators so the result drops straight
	// into SRenderModel::FromMesh without further GPU plumbing.
	if (!V.IsEmpty() && !I.IsEmpty())
	{
		_private::CreateGpuResources(V, I, result.VertexBufferGpu, result.IndexBufferGpu);
	}
#endif

	return result;
}


SMesh BuildPortalQuad (const Vector3f& Edge1, const Vector3f& Edge2, const Vector3f& Normal)
{
	SMesh result;
	auto& V = result.Vertices;
	auto& I = result.Indices;

	// Four corners: c - e1 - e2, c + e1 - e2, c + e1 + e2, c - e1 + e2 (c = origin).
	const Vector3f corners[4] = {
		Edge1 * -1.0f + Edge2 * -1.0f,
		Edge1 *  1.0f + Edge2 * -1.0f,
		Edge1 *  1.0f + Edge2 *  1.0f,
		Edge1 * -1.0f + Edge2 *  1.0f,
	};
	const Vector2f uvs[4] = { { 0.f, 0.f }, { 1.f, 0.f }, { 1.f, 1.f }, { 0.f, 1.f } };

	for (uint32 k = 0; k < 4u; ++k)
	{
		SVertex v = {};
		v.Position  = corners[k];
		v.Uv        = uvs[k];
		v.Normal    = Normal;
		v.Tangent   = Edge1;
		v.Bitangent = Edge2;
		v.Color     = { 1.0f, 1.0f, 1.0f, 1.0f };
		V.Add(v);
	}
	I.Add(0u); I.Add(1u); I.Add(2u);
	I.Add(0u); I.Add(2u); I.Add(3u);

#ifndef FRT_HEADLESS
	_private::CreateGpuResources(V, I, result.VertexBufferGpu, result.IndexBufferGpu);
#endif

	return result;
}
}
