#include "Graphics/Render/Scene/WorldExtraction.h"

#include <cmath>

#include "Entity.h"
#include "WorldScene.h"
#include "Graphics/Camera.h"
#include "Graphics/GeometryScript.h"
#include "Graphics/Model.h"
#include "Graphics/SceneExtraction.h"
#include "Graphics/Render/Scene/GpuScene.h"


namespace frt::graphics
{
namespace
{

gpu::float3 ToFloat3 (const Vector3f& V)
{
	return { V.x, V.y, V.z };
}

float Length (const Vector3f& V)
{
	return std::sqrt(V.x * V.x + V.y * V.y + V.z * V.z);
}

Vector3f Cross (const Vector3f& A, const Vector3f& B)
{
	return Vector3f(A.y * B.z - A.z * B.y, A.z * B.x - A.x * B.z, A.x * B.y - A.y * B.x);
}

float Dot (const Vector3f& A, const Vector3f& B)
{
	return A.x * B.x + A.y * B.y + A.z * B.z;
}

// Render-space light from the CPU description (positions/directions already render space).
gpu::SLightData ConvertLight (const SLight& Light)
{
	constexpr float kPi = 3.14159265358979323846f;
	gpu::SLightData out = {};
	const Vector3f color = Vector3f(Light.Emission.R, Light.Emission.G, Light.Emission.B) * Light.Intensity;
	out.Position = ToFloat3(Light.Position);
	out.Flags = gpu::LightFlag_CastsShadows;
	switch (static_cast<ELightType>(Light.Type))
	{
		case ELightType::Directional:
		{
			// Authored directional lights get a small disk (0.5 deg) for soft, MIS-able shadows.
			const float angularRadius = 0.5f * kPi / 180.0f;
			out.Type = gpu::LightType_Sun;
			out.Radius = std::cos(angularRadius);
			out.SolidAngle = 2.0f * kPi * (1.0f - out.Radius);
			Vector3f toLight = Light.Direction * -1.0f;
			const float len = Length(toLight);
			toLight = len > 0.0f ? toLight * (1.0f / len) : Vector3f(0.0f, 1.0f, 0.0f);
			out.AxisU = ToFloat3(toLight);
			out.Radiance = ToFloat3(color * (1.0f / out.SolidAngle));
			break;
		}
		case ELightType::AreaQuad:
		{
			out.Type = gpu::LightType_Rect;
			Vector3f u = Light.Edge1;
			Vector3f v = Light.Edge2;
			// Rect lights emit along cross(AxisU, AxisV); orient that toward the authored facing.
			if (Dot(Cross(u, v), Light.Direction) < 0.0f)
			{
				std::swap(u, v);
			}
			out.AxisU = ToFloat3(u);
			out.AxisV = ToFloat3(v);
			out.SolidAngle = 4.0f * Length(Cross(u, v));
			out.Radiance = ToFloat3(color);
			break;
		}
		case ELightType::Point:
		default:
			out.Type = gpu::LightType_Sphere;
			out.Radius = 0.0f;
			out.Radiance = ToFloat3(color); // intensity for point lights
			break;
	}
	return out;
}

gpu::SPortalData ConvertPortal (const SPortal& Portal)
{
	gpu::SPortalData out = {};
	out.Center = ToFloat3(Portal.Center);
	out.Normal = ToFloat3(Portal.Normal);
	out.AxisU = ToFloat3(Portal.Edge1);
	out.AxisV = ToFloat3(Portal.Edge2);
	out.Flags = (Portal.Flags & portal_flags::Ellipse) ? gpu::PortalFlag_Ellipse : 0u;
	const float rectArea = 4.0f * Length(Cross(Portal.Edge1, Portal.Edge2));
	out.Area = out.Flags & gpu::PortalFlag_Ellipse ? rectArea * 0.78539816f : rectArea;
	return out;
}

Vector3f TransformDirectionDx (const DirectX::XMFLOAT4X4& RenderMatrix, const Vector3f& LocalDirection)
{
	using namespace DirectX;
	const XMVECTOR v = XMVector3TransformNormal(XMVectorSet(LocalDirection.x, LocalDirection.y, LocalDirection.z, 0.0f), XMLoadFloat4x4(&RenderMatrix));
	XMFLOAT3 out;
	XMStoreFloat3(&out, v);
	return Vector3f(out.x, out.y, out.z);
}
}


void ExtractRenderScene (CWorldScene& World, const SRenderExtractOptions& Options, CGpuScene& Scene)
{
	Scene.BeginFrame();

	// ---- Legacy CEntity list: instances, lights, portals ----
	auto& entities = World.GetEntities();
	for (uint32 i = 0; i < entities.Count(); ++i)
	{
		CEntity* entity = entities[i].GetRawIgnoringLifetime();
		if (!entity)
		{
			continue;
		}

		if (entity->Portal && entity->Portal->bEnabled)
		{
			const Comp_Portal& cp = *entity->Portal;
			const DirectX::XMFLOAT4X4& m = entity->Transform.GetMatrix();
			SPortal portal;
			portal.Center = math::ToDirectXCoordinates(entity->Transform.GetTranslation());
			portal.Edge1 = TransformDirectionDx(m, cp.Edge1);
			portal.Edge2 = TransformDirectionDx(m, cp.Edge2);
			Vector3f n = Cross(portal.Edge1, portal.Edge2);
			const float nl = Length(n);
			n = nl > 0.0f ? n * (1.0f / nl) : Vector3f(0.0f, 1.0f, 0.0f);
			if (Dot(n, TransformDirectionDx(m, cp.Normal)) < 0.0f)
			{
				n = n * -1.0f;
			}
			portal.Normal = n;
			portal.Flags = cp.Shape == Comp_Portal::EShape::Ellipse ? portal_flags::Ellipse : 0u;
			Scene.AddPortal(ConvertPortal(portal));

			// Visualisation quad, built once from the component; camera rays only.
			if (entity->RenderModel)
			{
				if (!entity->RenderModel->Model)
				{
					entity->RenderModel->Model = memory::NewShared<SRenderModel>(
						SRenderModel::FromMesh(mesh::BuildPortalQuad(cp.Edge1, cp.Edge2, cp.Normal)));
				}
				entity->RenderModel->bVisible = Options.bShowPortalMeshes;
			}
		}

		if (entity->Light && entity->Light->bEnabled)
		{
			const Comp_Light& cl = *entity->Light;
			SLight light;
			light.Position = math::ToDirectXCoordinates(entity->Transform.GetTranslation());
			light.Emission = cl.Color;
			light.Intensity = cl.Intensity;
			light.Direction = math::ToDirectXCoordinates(cl.Direction);
			light.Edge1 = math::ToDirectXCoordinates(cl.Edge1);
			light.Edge2 = math::ToDirectXCoordinates(cl.Edge2);
			light.Type = static_cast<uint32>(cl.Kind == Comp_Light::EKind::Directional ? ELightType::Directional
				: (cl.Kind == Comp_Light::EKind::AreaQuad ? ELightType::AreaQuad : ELightType::Point));
			Scene.AddLight(ConvertLight(light));
		}

		if (entity->RenderModel && entity->RenderModel->Model && entity->RenderModel->bVisible)
		{
			SSceneInstance instance;
			instance.Model = entity->RenderModel->Model.GetRawIgnoringLifetime();
			instance.ObjectToWorld = entity->Transform.GetRaytracingTransform();
			instance.Key = (1ull << 62u) | entity->Id;
			instance.bHelper = static_cast<bool>(entity->Portal);
			Scene.AddInstance(instance);
		}
	}

	// ---- ECS ----
	{
		TArray<SMeshInstance> ecsInstances;
		ExtractMeshInstances(World.GetEcsWorld(), Vector3r::ZeroVector, ecsInstances);
		for (uint32 i = 0; i < ecsInstances.Count(); ++i)
		{
			const SMeshInstance& src = ecsInstances[i];
			SSceneInstance instance;
			instance.Model = src.Model;
			for (uint32 row = 0u; row < 3u; ++row)
			{
				for (uint32 col = 0u; col < 4u; ++col)
				{
					instance.ObjectToWorld.m[row][col] = src.Transform.M[row][col];
				}
			}
			instance.Key = (1ull << 63u) | static_cast<uint64>(src.Entity.Value);
			Scene.AddInstance(instance);
		}

		TArray<SLight> ecsLights;
		ExtractLights(World.GetEcsWorld(), ecsLights);
		for (uint32 i = 0; i < ecsLights.Count(); ++i)
		{
			Scene.AddLight(ConvertLight(ecsLights[i]));
		}
		TArray<SPortal> ecsPortals;
		ExtractPortals(World.GetEcsWorld(), ecsPortals);
		for (uint32 i = 0; i < ecsPortals.Count(); ++i)
		{
			Scene.AddPortal(ConvertPortal(ecsPortals[i]));
		}
	}

	// ---- Sky ----
	{
		SSceneSky sky;
		sky.Constants = World.Environment.Sky;
		sky.SunAngularRadius = World.Environment.SunAngularRadius;
		Scene.SetSky(sky);
	}
}

SCameraView MakeCameraView (const CCamera& Camera)
{
	SCameraView view;
	const Vector3f position = math::ToDirectXCoordinates(Camera.Transform.GetTranslation());
	const Vector3f forward = math::ToDirectXCoordinates(Camera.GetLookDirection());
	view.Position = { position.x, position.y, position.z };
	view.Forward = { forward.x, forward.y, forward.z };
	view.Up = { 0.0f, 1.0f, 0.0f };
	view.FieldOfViewY = Camera.FieldOfViewY;
	view.NearZ = Camera.NearZ;
	return view;
}
}
