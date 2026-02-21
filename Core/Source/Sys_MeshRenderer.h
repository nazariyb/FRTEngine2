#pragma once

#include "CoreTypes.h"
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
	CWorld ();
#endif
	virtual ~Sys_MeshRenderer () override {}

	// System interface
	virtual SFlags<EUpdatePhase>& GetPhases() override;
	// ~System interface

	virtual void Tick (float DeltaSeconds);
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
	graphics::raytracing::SAccelerationStructureBuffers CreateBottomLevelAS (
		const graphics::Comp_RenderModel& RenderModel);
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

	TArray<const CEntity*> AsEntities;
	TArray<const graphics::SRenderModel*> AsModels;
	TArray<DirectX::XMFLOAT4X4> AsTransforms;
	bool bAsInitialized = false;
	bool bRaytracingSupported = false;
	bool bRaytracingSupportChecked = false;

	// Temporal accumulation counter — increments every frame, resets to 0
	// whenever bAccumulationDirty (owned by CWorldScene) is set.
	uint32 AccumulationFrameIndex = 0u;
	DirectX::XMFLOAT4X4 PrevCameraViewMatrix = {};
};
}
