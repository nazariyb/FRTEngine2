#pragma once

#include "CoreTypes.h"
#include "ECS/EntityId.h"
#include "Entity.h"
#include "System.h"
#include "Graphics/DXRUtils.h"
#include "Graphics/Render/Renderer.h"


namespace frt
{
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
	virtual void Present (float DeltaSeconds, ID3D12GraphicsCommandList4* CommandList);
	void InitializeRendering ();
	void CreateAccelerationStructures ();
	void UpdateAccelerationStructures ();

	void CopyConstantData ();
	void UploadCB (ID3D12GraphicsCommandList4* CommandList);
#endif

	// memory::TRefShared<CEntity> SpawnEntity ();
	memory::TRefShared<graphics::Comp_RenderModel> SpawnRenderModel ();

private:
#ifndef FRT_HEADLESS
	struct SAccelerationInstance;

	/**
	 * Identity of whatever produced an instance, for frame-to-frame change detection.
	 * Exactly one of the two is set: the CEntity list and the ECS both feed the same
	 * acceleration structure while the migration is in progress.
	 */
	struct SInstanceSource
	{
		const CEntity* Legacy = nullptr;
		EntityId       Entity = InvalidEntity;

		bool operator== (const SInstanceSource& Rhs) const
		{
			return Legacy == Rhs.Legacy && Entity == Rhs.Entity;
		}

		bool operator!= (const SInstanceSource& Rhs) const { return !(*this == Rhs); }
	};

	struct SBuildEntry
	{
		SInstanceSource Source;
		const graphics::SRenderModel* Model = nullptr;
		DirectX::XMFLOAT3X4 Transform = {};
	};

	/**
	 * The single definition of what goes into the acceleration structure, from both
	 * sources, in a fixed order: CEntity entries first, then ECS ones.
	 *
	 * Shared by the build and the update deliberately. They used to filter separately with
	 * a comment warning that the two had to stay identical or the update's positional
	 * comparison would desync - that is the sort of duplication that only breaks later.
	 */
	void CollectBuildEntries (TArray<SBuildEntry>& OutEntries);

	graphics::raytracing::SAccelerationStructureBuffers CreateBottomLevelAS (
		const graphics::SRenderModel& Model);
	void CreateTopLevelAS (const TArray<SAccelerationInstance>& Instances, bool bUpdateOnly = false);
#endif

public:
#ifndef FRT_HEADLESS
	memory::TRefWeak<graphics::CRenderer> Renderer;
#endif
	// TArray<memory::TRefShared<CEntity>> Entities;
	TArray<memory::TRefShared<graphics::Comp_RenderModel>> RenderModels; // TODO: allocate on stack

private:
	struct SAccelerationInstance
	{
		ID3D12Resource* BottomLevelAS = nullptr;
		DirectX::XMFLOAT3X4 Transform = {};
		uint32 InstanceId = 0u;
		uint32 HitGroupIndex = 0u;
	};

	// move to renderer?
	TArray<ComPtr<ID3D12Resource>> BottomLevelASs;
	graphics::raytracing::CTopLevelASGenerator TopLevelASGenerator;
	graphics::raytracing::SAccelerationStructureBuffers TopLevelASBuffers;
	TArray<SAccelerationInstance> Instances;

	/**
	 * This frame's drawables, from both the CEntity list and the ECS, in one order.
	 *
	 * Refreshed in CopyConstantData and consumed by raster and raytracing alike. The
	 * object-constant buffer is filled from it and the raster loop indexes into it, so an
	 * entity's constants and its draw call cannot drift apart - they used to be two arrays
	 * that lined up only because SpawnEntity created a render model for every entity,
	 * drawable or not, purely to keep the indices parallel.
	 */
	TArray<SBuildEntry> Drawables;

	// Parallel to Instances. Together they describe the acceleration structure as built, so
	// the next frame can tell a moved object (refit) from a changed set (rebuild).
	TArray<SInstanceSource> AsSources;
	TArray<const graphics::SRenderModel*> AsModels;

	// The instance transform itself rather than the world matrix it came from: it is what
	// actually reaches the TLAS, and both sources can produce it.
	TArray<DirectX::XMFLOAT3X4> AsTransforms;
	SFlags<EUpdatePhase> Phases;

	// Per-frame light list, refilled by light collector. Uploaded into the frame upload arena
	// each frame; GPU VA pushed to the renderer for SBT patching.
	graphics::CLightList Lights;

	// Same pattern for portal pre-filter quads.
	graphics::CPortalList Portals;

	bool bAsInitialized = false;
	bool bRaytracingSupported = false;
	bool bRaytracingSupportChecked = false;

	// Temporal accumulation counter — increments every frame, resets to 0
	// whenever bAccumulationDirty (owned by CWorldScene) is set.
	uint32 AccumulationFrameIndex = 0u;
	DirectX::XMFLOAT4X4 PrevCameraViewMatrix = {};
};
}
