#pragma once

#include "CoreTypes.h"
#include "Enum.h"
#include "Graphics/Render/GraphicsCoreTypes.h"


namespace frt
{
// Game time, from the world's clock (SWorldClock): 0 delta while the world is paused.
struct SUpdateContext
{
	float  DeltaSeconds = 0.0f;
	double TotalSeconds = 0.0;
};



// Prepare and Update are simulation: they run only while the world's clock advances. Finalize
// runs every frame (transform propagation), so edits to a paused or edit-mode world still show.
enum class EUpdatePhase : uint8
{
	Input    = 1u << 0u,
	Prepare  = 1u << 1u,
	Update   = 1u << 2u,
	Finalize = 1u << 3u,
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

	virtual void Input (const SUpdateContext& /*Context*/) {}
	virtual void Prepare (const SUpdateContext& /*Context*/) {}
	virtual void Update (const SUpdateContext& /*Context*/) {}
	virtual void Finalize (const SUpdateContext& /*Context*/) {}
};
}
