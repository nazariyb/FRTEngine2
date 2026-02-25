#pragma once

#include "CoreTypes.h"
#include "Enum.h"
#include "Graphics/Render/GraphicsCoreTypes.h"


namespace frt
{
struct SUpdateContext
{
	float DeltaSeconds;
	double TotalSeconds;
};


struct SDrawUpdateContext : SUpdateContext
{
	ID3D12GraphicsCommandList4* CommandList;
};


enum class EUpdatePhase : uint8
{
	Input    = 1u << 0u,
	Prepare  = 1u << 1u,
	Update   = 1u << 2u,
	Finalize = 1u << 3u,
	Draw     = 1u << 4u,
};


FRT_DECLARE_FLAG_ENUM(EUpdatePhase);


class ISystem
{
public:
	virtual ~ISystem () = default;

	/** @return bool indicating whether the initialization was successful */
	virtual bool Initialize () { return true; }
	virtual void ShutDown () {}

	virtual SFlags<EUpdatePhase>& GetPhases () = 0;

	virtual void Input (const SUpdateContext& Context) {}
	virtual void Prepare (const SUpdateContext& Context) {}
	virtual void Update (const SUpdateContext& Context) {}
	virtual void Finalize (const SUpdateContext& Context) {}
	virtual void Draw (const SDrawUpdateContext& Context) {}
};
}
