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
	graphics::raytracing::SAccelerationStructureBuffers CreateBottomLevelAS (
		const graphics::Comp_RenderModel& RenderModel);
	void CreateTopLevelAS (const TArray<std::pair<ID3D12Resource*, DirectX::XMMATRIX>>& Instances);
#endif

public:
#ifndef FRT_HEADLESS
	memory::TRefWeak<graphics::CRenderer> Renderer;
#endif
	TArray<memory::TRefShared<CEntity>> Entities;

private:
	// move to renderer?
	ComPtr<ID3D12Resource> BottomLevelAS;
	graphics::raytracing::CTopLevelASGenerator TopLevelASGenerator;
	graphics::raytracing::SAccelerationStructureBuffers TopLevelASBuffers;
	TArray<std::pair<ID3D12Resource*, DirectX::XMMATRIX>> Instances;
};
}
