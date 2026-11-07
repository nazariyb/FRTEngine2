#pragma once

#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

#include "CoreTypes.h"
#include "Containers/String.h"
#include "Project/Project.h"

#include "ToolProcess.h"


namespace frt
{
class CEngine;
}

namespace frt::editor
{
// Why the project manager is showing, when it is not just the editor started without a project.
struct SProjectManagerRequest
{
	std::filesystem::path Project;   // the project this is about: selected in the list
	FString Message;                 // shown at the top
	bool bOpenNow = false;           // open it right away: it belongs to another engine's editor
	bool bOfferBuild = false;        // its game would not load: offer to build it and try again
	bool bOfferMigrate = false;      // its content is behind this engine (MIG001): offer to migrate it, then open it
};

/**
 * The project manager (Unity's Hub, Unreal's project browser): what the editor shows when it is
 * started without a project, or with -ProjectManager (File > Switch Project). It lists the
 * user's recent projects (Project/UserRegistry.h), opens one or any other project file, and
 * creates projects from the engine's templates.
 *
 * It does nothing the command line cannot: creating, building and migrating run the engine's FRT.bat
 * (new, build, assets migrate --all - Premake/tools.lua), whose output it shows as they go. A
 * project whose content is behind this engine's migrations (MIG001, design §14.8: its
 * Schemas/Migrations.yml lacks a step) is marked in the list, and Migrate and Open brings it up to
 * date before opening it; the editor sends a project it found behind here when asked to. Both
 * only while the gate is on (assets::IsMigrationGateOn; on by default). A project opens in a new editor
 * process - its own engine's editor, which need not be this one - and the manager then exits:
 * an editor runs one project for its whole life, so the game module and everything built from
 * it never outlive their project.
 *
 * -Editor:SmokeTest=1 makes it a scripted run, as for the editor: it captures its pages, creates
 * a project through them ([Editor] SmokeTemplate, SmokeLayout) in [Editor] SmokeTestDir - by
 * default %TEMP%\FRTEngine\SmokeTest\ProjectManager, which it empties first, and which holds
 * the run's own registry - and opens the project in an editor running the editor's smoke test,
 * whose result it exits with.
 */
class CProjectManager
{
public:
	CProjectManager (CEngine& Engine, SProjectManagerRequest Request);
	~CProjectManager ();
	CProjectManager (const CProjectManager&) = delete;
	CProjectManager& operator= (const CProjectManager&) = delete;

	// Until the window closes or a project opens. Returns the process exit code.
	int Run ();

private:
	struct SRecentProject
	{
		std::filesystem::path File;
		FString Name;
		FString Location;
		FString EngineText;              // which engine it uses, or why none was found
		FString LastOpened;
		bool bMissing = false;           // the project file is gone
		bool bEngineMissing = false;
		FString Behind;                  // MIG001: its content is behind this engine's migrations (the gate is on)
	};

	struct STemplate
	{
		FString Name;
		FString Description;
	};

	enum class ETask : uint8
	{
		None,
		Build,                           // FRT.bat build, then open
		Create,                          // FRT.bat new --build, then open
		Migrate,                         // FRT.bat assets migrate --all, then open
	};

	enum class EBrowse : uint8
	{
		None,
		Location,                        // a folder for a new project
		Project,                         // a project file to open
	};

	void Refresh ();
	void Draw ();
	void DrawRecent ();
	void DrawNewProject ();
	void DrawTask ();
	void UpdateTask ();
	void UpdateBrowse ();
	void StartBrowse (EBrowse Kind);

	// Opens a project: builds it first when its editor or its game is not built, or bBuild.
	void Open (const std::filesystem::path& ProjectFile, bool bBuild);
	// Builds the project in a console once this process has exited, then opens it: for when the
	// engine itself needs building, which its running editor prevents. bMigrate: the task that failed
	// was a migration, so its content is migrated between the build and the open.
	void BuildAfterExit (const std::filesystem::path& ProjectFile, bool bMigrate);
	void StartCreate ();
	// Brings the project's content up to date with its engine's migrations, then opens it.
	void StartMigrate (const std::filesystem::path& ProjectFile);
	void StartTask (ETask Kind, FString Title, FStringView CommandLine, const std::filesystem::path& ProjectFile);
	void SetMessage (FString Text, bool bError);
	void SaveSettings () const;
	void UpdateSmokeTest ();
	void SmokeLog (FStringView Line) const;

	CEngine& Engine;
	SProjectManagerRequest Request;
	project::SEngineDescriptor ThisEngine;
	bool bFirstFrame = true;

	FString Message;
	bool bMessageIsError = false;
	bool bOfferBuild = false;
	bool bOfferMigrate = false;

	std::vector<SRecentProject> Recent;
	int32 Selected = -1;
	bool bShowNewProject = false;        // select the New Project tab on the next frame

	std::vector<STemplate> Templates;
	int32 SelectedTemplate = 0;
	FString Name;
	FString Location;
	bool bEmbeddedEngine = false;

	ETask Task = ETask::None;
	FString TaskTitle;
	std::filesystem::path TaskProject;   // opened when the task succeeds
	std::filesystem::path FailedProject; // its build failed on a file this process holds
	ETask FailedTask = ETask::None;      // the task that failed so (a migration rebuilds AssetTool first)
	CToolProcess Tool;
	size_t ShownOutputSize = 0u;

	EBrowse Browse = EBrowse::None;
	std::thread BrowseThread;
	std::atomic<bool> bBrowseDone = false;
	std::filesystem::path BrowseResult;

	bool bSmokeTest = false;
	std::filesystem::path SmokeDir;
	uint32 SmokeStep = 0u;
	uint64 SmokeStepFrame = 0u;
};
}
