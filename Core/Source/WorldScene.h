#pragma once

#include "System.h"
#include "Containers/Array.h"
#include "Graphics/Render/GraphicsCoreTypes.h"


namespace frt
{
class GameInstance;
class Sys_MeshRenderer;
class CEntity;
class ISystem;


class CWorldScene
{
public:
	CWorldScene () = delete;
	CWorldScene (GameInstance& InGame);

	bool Initialize ();

	memory::TRefShared<CEntity> SpawnEntity ();

	void RunFrame ();
	void SubmitFrame (ID3D12GraphicsCommandList4* CommandList);

	bool IsPhasePaused (EUpdatePhase Phase) const;
	void PausePhases (SFlags<EUpdatePhase> Phases);
	void UnpausePhases (SFlags<EUpdatePhase> Phases);
	bool TogglePhasePause (EUpdatePhase Phase);

	const TArray<memory::TRefShared<CEntity>>& GetEntities () const { return Entities; }

	memory::TRefUnique<Sys_MeshRenderer> MeshRenderer;
	// TArray<memory::TRefUnique<ISystem>> Systems;

	// Scene-level dirty flags consumed by Sys_MeshRenderer each frame.
	// Set here because topology and motion are scene knowledge, not renderer knowledge.
	bool bSceneTopologyDirty = false;
	bool bAccumulationDirty = false;


private:
	TArray<memory::TRefShared<CEntity>> Entities; // TODO: allocate on stack

	SFlags<EUpdatePhase> PausedPhases;
	GameInstance& Game;
};
}
