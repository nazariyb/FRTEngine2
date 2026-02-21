#pragma once

#include "CoreTypes.h"
#include "Entity.h"
#include "Graphics/DXRUtils.h"
#include "Graphics/Render/Renderer.h"


namespace frt
{
class CWorld
{
public:
#ifndef FRT_HEADLESS
	CWorld () = delete;
	explicit CWorld (memory::TRefWeak<graphics::CRenderer> InRenderer);
#else
	CWorld ();
#endif
	virtual ~CWorld () = default;

	virtual void Tick (float DeltaSeconds);
#ifndef FRT_HEADLESS
	virtual void Present (float DeltaSeconds, ID3D12GraphicsCommandList4* CommandList);
	void InitializeRendering ();
	void CreateAccelerationStructures ();
	void UpdateAccelerationStructures ();

	void CopyConstantData ();
	void UploadCB (ID3D12GraphicsCommandList4* CommandList);
#endif

	memory::TRefShared<CEntity> SpawnEntity ();

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
	TArray<memory::TRefShared<CEntity>> Entities;

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

	TArray<CEntity*> AsEntities;
	TArray<const graphics::SRenderModel*> AsModels;
	TArray<DirectX::XMFLOAT4X4> AsTransforms;
	bool bAsInitialized = false;
	bool bAsTopologyDirty = true;
	bool bRaytracingSupported = false;
	bool bRaytracingSupportChecked = false;

	// Temporal accumulation counter — increments every frame, resets to 0
	// whenever the camera moves, any object moves, or the scene topology
	// changes.  Resets guarantee history is always valid (no ghosting).
	// The shader uses a pure 1/(N+1) running average — no alpha floor — so
	// noise keeps decreasing indefinitely while the scene is static.
	uint32 AccumulationFrameIndex = 0u;
	DirectX::XMFLOAT4X4 PrevCameraViewMatrix = {};
	bool bAccumulationDirty = true;
};
}
