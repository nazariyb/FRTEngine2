#pragma once

#include "CoreTypes.h"
#include "Entity.h"
#include "Graphics/Render/Renderer.h"


namespace frt
{
class CWorld
{
public:
#if !defined(FRT_HEADLESS)
	CWorld () = delete;
	explicit CWorld (memory::TRefWeak<graphics::CRenderer> InRenderer);
#else
	CWorld ();
#endif
	virtual ~CWorld () = default;

	virtual void Tick (float DeltaSeconds);
#if !defined(FRT_HEADLESS)
	virtual void Present (float DeltaSeconds, ID3D12GraphicsCommandList* CommandList);

	void CopyConstantData ();
#endif

	memory::TRefShared<CEntity> SpawnEntity ();

#if !defined(FRT_HEADLESS)
	memory::TRefWeak<graphics::CRenderer> Renderer;
#endif
	TArray<memory::TRefShared<CEntity>> Entities;
};
}
