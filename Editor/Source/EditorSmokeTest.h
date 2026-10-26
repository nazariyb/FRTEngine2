#pragma once

#include <filesystem>
#include <initializer_list>
#include <vector>

#include "CoreTypes.h"
#include "Containers/StringView.h"
#include "Math/Math.h"


namespace frt
{
class CEngine;
}

namespace frt::config
{
class CConfig;
}

namespace frt::graphics
{
struct SMaterial;
}


namespace frt::editor
{
class CEditor;

/**
 * A scripted editor session (-Editor:SmokeTest=1): drives the editor through the same commands
 * the user issues - maximize, fullscreen, play, pause, step, stop - checks the state after each
 * (Stop must leave the edit world exactly as Play found it), presses bound keys through the
 * window's own message queue, rewrites its input overrides to check they are re-read, checks
 * the Unreal camera controller against Unreal's numbers, makes the game module's DLL look
 * rebuilt in edit and in play mode to check the editor reloads it, shows entity names beyond
 * ASCII in the Editor panel after checking the UI font has a glyph for each of their characters,
 * captures the whole window to PNG along the way, and exits with 0 when every check passed (2
 * otherwise). Captures and a log go to [Editor] SmokeTestDir, by default <project>/Local/SmokeTest
 * (also where a run whose SmokeTestDir is not UTF-8 fails).
 *
 * A smoke test is a scripted run, so the file watcher starts disabled ([Assets] Watch=0,
 * Engine/Automation.h); the steps that need it turn it on for themselves and Flush it after they
 * write, so that what they check does not depend on timing: the input overrides, and with
 * [Editor] SmokeTestEdits=1 the steps that edit the project's files, for a scratch copy only (FRT.bat
 * verify makes one with FRT.bat new, and a copy of the shaders it names with [Render] ShaderRoot):
 * one of the project's materials is rewritten and must reload (its Version goes up, its new value
 * shows), and one shader include gets a comment and exactly the programs that include it must be
 * rebuilt. They refuse, and fail, when the project or the shaders are in the engine checkout.
 *
 * It also checks the asset registry (read-only, as every scripted run is): its initial scan was
 * complete before the first world was built ("[smoke] ok registry: <n> records, initial scan <ms> ms"),
 * and the editor's Assets panel shows what the registry answers for type:Texture.
 */
class CEditorSmokeTest
{
public:
	// What the smoke test asks the Assets panel ("[smoke] ok the Assets panel shows <n> of <m> record(s) of ...").
	static constexpr FStringLiteral AssetsPanelQuery = "type:Texture";

	explicit CEditorSmokeTest (const config::CConfig& Config);

	// Every frame, after the editor applied its commands.
	void Update (CEditor& Editor, CEngine& Engine);

	// Where captures, the log and the run's own input overrides go.
	const std::filesystem::path& GetOutputDir () const { return OutputDir; }

private:
	void Check (bool bPassed, FStringView What);
	// The registry's initial scan is complete (CEditor::StartUp waited for it); the Assets panel is given a query.
	void CheckAssetRegistry (CEditor& Editor, CEngine& Engine);
	// The panel shows what the registry answers for that query.
	void CheckAssetsPanel (CEditor& Editor, CEngine& Engine);
	void CheckCameraController ();
	void TouchGameBuild (CEditor& Editor);
	void WriteInputOverrides (FStringView Text);
	// The file watcher on or off for the steps that need it ([Assets] Watch is off in scripted runs),
	// on for the roots in or above OnlyUnder only (CFileWatcherService::SetEnabled): a step watches
	// its own files, never the rest of the checkout it runs in, which a developer may be editing.
	void WatchFiles (CEngine& Engine, bool bOn, std::initializer_list<std::filesystem::path> OnlyUnder = {});
	// Everything the watcher has seen happen in Directory's root is told now (CFileWatcherService::Flush).
	void FlushWatcher (CEngine& Engine, const std::filesystem::path& Directory, FStringView What);
	// [Editor] SmokeTestEdits: checks that the files to edit are a scratch copy's, picks them, watches.
	void BeginFileEdits (CEngine& Engine);
	void EditMaterial (CEngine& Engine);
	void CheckMaterialReloaded ();
	void EditShaderInclude (CEngine& Engine);
	void CheckProgramsRebuilt (CEngine& Engine);
	// The Which-th entity (of the first three) whose name is not ASCII, selected in the Editor
	// panel after checking the UI font has a glyph for each of its characters; Which 0 finds them.
	void SelectNamedEntity (CEditor& Editor, uint32 Which);
	void CaptureNamedEntity (CEngine& Engine, uint32 Which, FStringLiteral Name);
	void Capture (CEngine& Engine, FStringLiteral Name);
	void Log (FStringView Line);

	std::filesystem::path OutputDir;
	uint64 StartFrame = 0u;
	uint32 NextStep = 0u;
	uint32 Failures = 0u;
	float LongestFrameMs = 0.0f;
	uint64 EditWorldHash = 0u;
	uint64 PlayWorldHash = 0u;
	uint64 PausedFrames = 0u;
	uint32 InputGeneration = 0u;
	Vector3f CameraStart = Vector3f(0.0f, 0.0f, 0.0f);
	float ReleaseSpeed = 0.0f;
	uint32 GameReloads = 0u;
	uint32 EditEntities = 0u;
	std::vector<int32> NamedEntities;          // edit world indices, for SelectNamedEntity
	bool bCanReload = false;
	bool bFinished = false;
	// ---- [Editor] SmokeTestEdits ----
	bool bEditFiles = false;                   // asked for
	bool bCanEditFiles = false;                // and the files are a scratch copy's
	const graphics::SMaterial* EditedMaterial = nullptr;   // the material library keeps it alive
	uint32 MaterialVersion = 0u;
	float EditedRoughness = 0.0f;
	std::filesystem::path EditedInclude;
	uint32 IncludePrograms = 0u;               // programs that read EditedInclude
	uint64 HotReloads = 0u;                    // the pipeline library's count before the edit
};
}
