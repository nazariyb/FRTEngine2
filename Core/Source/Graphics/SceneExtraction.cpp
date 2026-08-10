#include "Graphics/SceneExtraction.h"

#include <cmath>

#include "ECS/CoreComponents.h"
#include "ECS/World.h"
#include "Graphics/Comp_Light.h"
#include "Graphics/Comp_Portal.h"
#include "Graphics/Model.h"


namespace frt::graphics
{
void ExtractLights (CWorld& InWorld, TArray<SLight>& OutLights)
{
	for (auto [id, transform, source] : InWorld.View<const Comp_WorldTransform, const Comp_Light>())
	{
		(void)id;

		if (!source.bEnabled)
		{
			continue;
		}

		// World translation follows the configured precision; the GPU list is always
		// 32-bit, so narrow deliberately here rather than anywhere upstream.
		const Vector3f origin = math::VectorCast<float>(transform.Translation);

		SLight light;
		light.Position  = math::ToDirectXCoordinates(origin);
		light.Emission  = source.Color;
		light.Intensity = source.Intensity;

		// Direction and edges are authored in world space on the component and are NOT
		// rotated by the entity's basis, matching the CEntity scan this replaces.
		// Comp_WorldTransform now makes deriving them from the basis possible - that is a
		// deliberate change to make, not one to slip in during a migration.
		light.Direction = math::ToDirectXCoordinates(source.Direction);

		switch (source.Kind)
		{
			case Comp_Light::EKind::Directional:
				light.Type = static_cast<uint32>(ELightType::Directional);
				break;

			case Comp_Light::EKind::AreaQuad:
			{
				light.Type  = static_cast<uint32>(ELightType::AreaQuad);
				light.Edge1 = math::ToDirectXCoordinates(source.Edge1);
				light.Edge2 = math::ToDirectXCoordinates(source.Edge2);

				// Edges are half-extents.
				const float e1 = std::sqrt(source.Edge1.SizeSquared());
				const float e2 = std::sqrt(source.Edge2.SizeSquared());
				light.Area = 4.0f * e1 * e2;
				break;
			}

			case Comp_Light::EKind::Point:
			default:
				light.Type = static_cast<uint32>(ELightType::Point);
				break;
		}

		OutLights.Add(light);
	}
}

void ExtractPortals (CWorld& InWorld, TArray<SPortal>& OutPortals)
{
	for (auto [id, transform, source] : InWorld.View<const Comp_WorldTransform, const Comp_Portal>())
	{
		(void)id;

		if (!source.bEnabled)
		{
			continue;
		}

		// Only the centre comes from the transform. Normal and the half-extent edges are
		// authored in world space, as in the scan this replaces.
		const Vector3f center = math::VectorCast<float>(transform.Translation);

		SPortal portal;
		portal.Center = math::ToDirectXCoordinates(center);
		portal.Normal = math::ToDirectXCoordinates(source.Normal);
		portal.Edge1  = math::ToDirectXCoordinates(source.Edge1);
		portal.Edge2  = math::ToDirectXCoordinates(source.Edge2);
		portal.Flags  = (source.Shape == Comp_Portal::EShape::Ellipse)
			? portal_flags::Ellipse
			: 0u;

		OutPortals.Add(portal);
	}
}

void ExtractMeshInstances (CWorld& InWorld, const Vector3r& InOrigin, TArray<SMeshInstance>& OutInstances)
{
	for (auto [id, transform, source] : InWorld.View<const Comp_WorldTransform, const Comp_RenderModel>())
	{
		// Matches the acceleration-structure build's filter: an entity with no model, or a
		// hidden one, contributes no instance at all.
		if (!source.bVisible || !source.Model)
		{
			continue;
		}

		SMeshInstance instance;
		instance.Entity = id;
		instance.Model = source.Model.GetRawIgnoringLifetime();

		// Subtract first, narrow second. With a double-precision build that is what keeps
		// an instance accurate far from the origin - the offset is small even when the
		// absolute position is not.
		instance.Transform = ToDirectXHandedness(transform.ToInstanceTransform(InOrigin));

		OutInstances.Add(instance);
	}
}

DirectX::XMFLOAT4X4 ToWorldMatrix (const DirectX::XMFLOAT3X4& InInstanceTransform)
{
	DirectX::XMFLOAT4X4 result = {};

	// Instance[row][col] == World[col][row] - the transpose GetRaytracingTransform applies.
	for (uint32 row = 0u; row < 3u; ++row)
	{
		for (uint32 col = 0u; col < 4u; ++col)
		{
			result.m[col][row] = InInstanceTransform.m[row][col];
		}
	}

	// The column the instance form drops. Constant for an affine transform.
	result.m[0][3] = 0.0f;
	result.m[1][3] = 0.0f;
	result.m[2][3] = 0.0f;
	result.m[3][3] = 1.0f;

	return result;
}
}
