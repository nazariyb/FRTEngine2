#pragma once

#include "CoreTypes.h"
#include "ECS/EntityId.h"
#include "Entity.h"
#include "System.h"
#include "Graphics/Render/Renderer.h"


namespace frt
{
/**
 * Bridges the game world to the renderer. Every frame (Finalize phase) it describes the scene
 * to the renderer's GPU scene - instances from both the CEntity list and the ECS, lights,
 * sky portals, the sky and the camera. All GPU work (uploads, acceleration structures,
 * tracing) is the renderer's business.
 */
class Sys_MeshRenderer : public ISystem
{
public:
#ifndef FRT_HEADLESS
	Sys_MeshRenderer () = delete;
	explicit Sys_MeshRenderer (memory::TRefWeak<graphics::CRenderer> InRenderer);
#else
	Sys_MeshRenderer ();
#endif
	virtual ~Sys_MeshRenderer () override {}

	// System interface
	virtual SFlags<EUpdatePhase>& GetPhases() override;

	virtual void Update(const SUpdateContext& Context) override;
	virtual void Finalize(const SUpdateContext& Context) override;
	virtual void Draw(const SDrawUpdateContext& Context) override;
	// ~System interface

#ifndef FRT_HEADLESS
	void InitializeRendering ();
#endif

	memory::TRefShared<graphics::Comp_RenderModel> SpawnRenderModel ();

private:
#ifndef FRT_HEADLESS
	void DescribeScene ();
#endif

public:
#ifndef FRT_HEADLESS
	memory::TRefWeak<graphics::CRenderer> Renderer;
#endif
	TArray<memory::TRefShared<graphics::Comp_RenderModel>> RenderModels;

private:
	SFlags<EUpdatePhase> Phases;
};
}
