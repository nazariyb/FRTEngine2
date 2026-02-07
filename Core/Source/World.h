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

	void CopyConstantData ();
	void UploadCB (ID3D12GraphicsCommandList4* CommandList);
#endif

	memory::TRefShared<CEntity> SpawnEntity ();

private:
#ifndef FRT_HEADLESS
	struct SAccelerationInstance;
	graphics::raytracing::SAccelerationStructureBuffers CreateBottomLevelAS (
		const graphics::Comp_RenderModel& RenderModel);
	void CreateTopLevelAS (const TArray<SAccelerationInstance>& Instances);
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
		DirectX::XMMATRIX Transform = {};
		uint32 InstanceId = 0u;
		uint32 HitGroupIndex = 0u;
	};

	// move to renderer?
	TArray<ComPtr<ID3D12Resource>> BottomLevelASs;
	graphics::raytracing::CTopLevelASGenerator TopLevelASGenerator;
	graphics::raytracing::SAccelerationStructureBuffers TopLevelASBuffers;
	TArray<SAccelerationInstance> Instances;
};
}
