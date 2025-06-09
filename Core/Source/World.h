#pragma once

#include "CoreTypes.h"
#include "Entity.h"
#include "Graphics/Renderer.h"


namespace frt
{
class CWorld
{
public:
	CWorld () = delete;
	explicit CWorld (memory::TRefWeak<graphics::CRenderer> InRenderer);
	virtual ~CWorld () = default;

	virtual void Tick (float DeltaSeconds);
	virtual void Present (float DeltaSeconds, ID3D12GraphicsCommandList* CommandList);

	void CopyConstantData ();

	memory::TRefShared<CEntity> SpawnEntity ();

	memory::TRefWeak<graphics::CRenderer> Renderer;
	TArray<memory::TRefShared<CEntity>> Entities;
};
}
