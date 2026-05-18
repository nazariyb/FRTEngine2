#include "Profiler/SceneDescriptor.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <DirectXMath.h>

#include "Entity.h"
#include "WorldScene.h"
#include "Graphics/Camera.h"
#include "Graphics/Model.h"
#include "Math/MathUtility.h"


namespace frt::profiler
{
using namespace DirectX;

namespace
{
// Sutherland-Hodgman clip of a clip-space polygon against the near plane (w > eps),
// then perspective divide, then clip the resulting NDC polygon to [-1,1]^2.
// Returns NDC-space area; screen-coverage fraction = area / 4 (NDC spans 2x2).

struct Vec4 { float x, y, z, w; };
struct Vec2 { float x, y; };

std::vector<Vec4> ClipNearPlane (const std::vector<Vec4>& In, float Eps)
{
	std::vector<Vec4> out;
	const size_t n = In.size();
	for (size_t i = 0; i < n; ++i)
	{
		const Vec4& a = In[i];
		const Vec4& b = In[(i + 1) % n];
		const bool aIn = a.w > Eps;
		const bool bIn = b.w > Eps;
		if (aIn) out.push_back(a);
		if (aIn != bIn)
		{
			const float t = (Eps - a.w) / (b.w - a.w);
			out.push_back({
				a.x + t * (b.x - a.x),
				a.y + t * (b.y - a.y),
				a.z + t * (b.z - a.z),
				a.w + t * (b.w - a.w) });
		}
	}
	return out;
}

// Clip a 2D polygon against one half-plane. side: +1 keeps x<=lim / y<=lim depending on axis.
std::vector<Vec2> ClipAxis (const std::vector<Vec2>& In, int Axis, float Lim, bool KeepLess)
{
	std::vector<Vec2> out;
	const size_t n = In.size();
	auto coord = [Axis] (const Vec2& p) { return Axis == 0 ? p.x : p.y; };
	auto inside = [&] (const Vec2& p) { return KeepLess ? coord(p) <= Lim : coord(p) >= Lim; };
	for (size_t i = 0; i < n; ++i)
	{
		const Vec2& a = In[i];
		const Vec2& b = In[(i + 1) % n];
		const bool aIn = inside(a);
		const bool bIn = inside(b);
		if (aIn) out.push_back(a);
		if (aIn != bIn)
		{
			const float ca = coord(a);
			const float cb = coord(b);
			const float t = (Lim - ca) / (cb - ca);
			out.push_back({ a.x + t * (b.x - a.x), a.y + t * (b.y - a.y) });
		}
	}
	return out;
}

float PolygonArea (const std::vector<Vec2>& P)
{
	if (P.size() < 3) return 0.0f;
	float area = 0.0f;
	const size_t n = P.size();
	for (size_t i = 0; i < n; ++i)
	{
		const Vec2& a = P[i];
		const Vec2& b = P[(i + 1) % n];
		area += a.x * b.y - b.x * a.y;
	}
	return std::fabs(area) * 0.5f;
}

// CornersWorld are already in render world space (entity matrix applied) — same space the
// camera view matrix lives in. WVP = ViewProj.
float ComputeScreenCoverage (
	const XMVECTOR CornersWorld[4],
	const XMMATRIX& ViewProj,
	SPortalInfo* Dbg = nullptr)
{
	std::vector<Vec4> clip;
	clip.reserve(4);
	for (int k = 0; k < 4; ++k)
	{
		XMVECTOR v = XMVector4Transform(CornersWorld[k], ViewProj);
		clip.push_back({ XMVectorGetX(v), XMVectorGetY(v), XMVectorGetZ(v), XMVectorGetW(v) });
	}

	if (Dbg)
	{
		Dbg->DbgClipW0 = clip[0].w;
		const float iw = (clip[0].w != 0.0f) ? 1.0f / clip[0].w : 0.0f;
		Dbg->DbgNdc0X = clip[0].x * iw;
		Dbg->DbgNdc0Y = clip[0].y * iw;
	}

	clip = ClipNearPlane(clip, 1e-4f);
	if (Dbg) Dbg->DbgClipCount = static_cast<int>(clip.size());
	if (clip.size() < 3) return 0.0f;

	std::vector<Vec2> ndc;
	ndc.reserve(clip.size());
	for (const Vec4& c : clip)
	{
		const float invW = 1.0f / c.w;
		ndc.push_back({ c.x * invW, c.y * invW });
	}

	ndc = ClipAxis(ndc, 0, -1.0f, false); // x >= -1
	ndc = ClipAxis(ndc, 0,  1.0f, true);  // x <=  1
	ndc = ClipAxis(ndc, 1, -1.0f, false); // y >= -1
	ndc = ClipAxis(ndc, 1,  1.0f, true);  // y <=  1
	if (Dbg) Dbg->DbgNdcCount = static_cast<int>(ndc.size());
	if (ndc.size() < 3) return 0.0f;

	// NDC viewport is 2x2 = area 4; coverage fraction = polyArea / 4.
	return std::min(PolygonArea(ndc) / 4.0f, 1.0f);
}
}


SSceneDescriptor CaptureSceneDescriptor (
	const CWorldScene& World,
	const graphics::CCamera& Camera,
	const graphics::SSkyConstants& Sky,
	uint32 RenderWidth,
	uint32 RenderHeight)
{
	SSceneDescriptor d;
	d.RenderWidth = RenderWidth;
	d.RenderHeight = RenderHeight;
	d.Sky = Sky;
	d.CameraPosition = Camera.Transform.GetTranslation();
	d.CameraRotation = Camera.Transform.GetRotation();

	const float aspect = RenderHeight > 0u
		? static_cast<float>(RenderWidth) / static_cast<float>(RenderHeight)
		: 1.0f;
	const XMMATRIX view = Camera.GetViewMatrix();
	const XMMATRIX proj = Camera.GetProjectionMatrix(math::PI_OVER_TWO, aspect, 1.0f, 1000.0f);
	const XMMATRIX viewProj = XMMatrixMultiply(view, proj);

	const auto& entities = World.GetEntities();
	for (uint32 i = 0; i < entities.Count(); ++i)
	{
		const CEntity* e = entities[i].GetRawIgnoringLifetime();
		if (!e)
		{
			continue;
		}
		++d.EntityCount;

		if (e->RenderModel && e->RenderModel->Model)
		{
			const graphics::SRenderModel& m = *e->RenderModel->Model;
			for (uint32 s = 0; s < m.Sections.Count(); ++s)
			{
				d.TriangleCount += m.Sections[s].IndexCount / 3u;
			}
		}

		if (e->Light && e->Light->bEnabled)
		{
			switch (e->Light->Kind)
			{
				case graphics::Comp_Light::EKind::Point:       ++d.PointLightCount; break;
				case graphics::Comp_Light::EKind::Directional: ++d.DirectionalLightCount; break;
				case graphics::Comp_Light::EKind::AreaQuad:    ++d.AreaQuadLightCount; break;
			}
		}

		if (e->Portal && e->Portal->bEnabled)
		{
			const graphics::Comp_Portal& cp = *e->Portal;

			const float e1Len = std::sqrt(cp.Edge1.SizeSquared());
			const float e2Len = std::sqrt(cp.Edge2.SizeSquared());

			SPortalInfo info;
			info.bEllipse = (cp.Shape == graphics::Comp_Portal::EShape::Ellipse);
			// Rect area = 4*|e1|*|e2|; ellipse inscribed = pi*|e1|*|e2|.
			info.WorldArea = info.bEllipse
				? (math::PI * e1Len * e2Len)
				: (4.0f * e1Len * e2Len);

			// Same local quad BuildPortalQuad emits, through the same entity world matrix the
			// renderer uses — guarantees coverage matches the on-screen viz mesh.
			const Vector3f e1 = cp.Edge1;
			const Vector3f e2 = cp.Edge2;
			const Vector3f localCorners[4] = {
				(e1 * -1.0f) + (e2 * -1.0f),
				(e1 *  1.0f) + (e2 * -1.0f),
				(e1 *  1.0f) + (e2 *  1.0f),
				(e1 * -1.0f) + (e2 *  1.0f),
			};
			const DirectX::XMFLOAT4X4& wf = e->Transform.GetMatrix();
			const XMMATRIX world = XMLoadFloat4x4(&wf);
			XMVECTOR cornersWorld[4];
			for (int k = 0; k < 4; ++k)
			{
				cornersWorld[k] = XMVector4Transform(
					XMVectorSet(localCorners[k].x, localCorners[k].y, localCorners[k].z, 1.0f),
					world);
			}
			info.ScreenCoverage = ComputeScreenCoverage(cornersWorld, viewProj, &info);
			if (info.bEllipse)
			{
				info.ScreenCoverage *= (math::PI / 4.0f); // ellipse fills pi/4 of the quad
			}

			d.Portals.Add(info);
			d.PortalCoverageTotal += info.ScreenCoverage;
			++d.PortalCount;
		}
	}

	return d;
}
}
