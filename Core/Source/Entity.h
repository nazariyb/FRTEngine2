#pragma once

#include <string>

#include "Graphics/Comp_Light.h"
#include "Graphics/Comp_Portal.h"
#include "Graphics/Model.h"
#include "Math/Transform.h"


namespace frt
{
class CEntity
{
public:
	// temp implementation
	void Tick (float DeltaSeconds);

	// Display name. Auto-assigned by CWorldScene::SpawnEntity (e.g. "Entity #3").
	// Editor uses it in combo + label fields. Free-form, no uniqueness guarantee.
	std::string Name;

	const math::STransform& GetTransform () const { return Transform; }
	math::STransform Transform;
	Vector3f RotationSpeed = Vector3f::ZeroVector; // TODO: also temp

	// TODO: These are here temporary
	const memory::TRefShared<graphics::Comp_RenderModel>& GetRenderModel () const { return RenderModel; }
	memory::TRefShared<graphics::Comp_RenderModel> RenderModel;
	bool bRayTraced = true; // TODO: should be per-material

	// Optional light component. Light collector pushes an SLight entry per frame when set.
	memory::TRefShared<graphics::Comp_Light> Light;

	// Optional portal component. Portal collector pushes an SPortal entry per frame when set.
	// Pre-filter rejects sky-NEE shadow rays that don't pass through any portal.
	memory::TRefShared<graphics::Comp_Portal> Portal;
};
}

