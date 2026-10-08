#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "CoreTypes.h"
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
	std::string Message;             // shown at the top
	bool bOpenNow = false;           // open it right away: it belongs to another engine's editor
	bool bOfferBuild = false;        // its game would not load: offer to build it and try again
};

/**
 * The project manager (Unity's Hub, Unreal's project browser): what the editor shows when it is
 * started without a project, or with -ProjectManager (File > Switch Project). It lists the
 * user's recent projects (Project/UserRegistry.h), opens one or any other project file, and
 * creates projects from the engine's templates.
 *
 * It does nothing the command line cannot: creating and building run the engine's FRT.bat (new,
 * build - Premake/tools.lua), whose output it shows as they go. A project opens in a new editor
 * process - its own engine's editor, which need not be this one - and the manager then exits:
 * an editor runs one project for its whole life, so the game module and everything built from
 * it never outlive their project.
 *
 * -Editor:SmokeTest=1 makes it a scripted run, as for the editor: it captures its pages, creates
 * a project through them ([Editor] SmokeTemplate, SmokeLayout) in [Editor] SmokeTestDir, and
 * opens it in an editor running the editor's smoke test, whose result it exits with.
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
		std::string Name;
		std::string Location;
		std::string EngineText;          // which engine it uses, or why none was found
		std::string LastOpened;
		bool bMissing = false;           // the project file is gone
		bool bEngineMissing = false;
	};

	struct STemplate
	{
		std::string Name;
		std::string Description;
	};

	enum class ETask : uint8
	{
		None,
		Build,                           // FRT.bat build, then open
		Create,                          // FRT.bat new --build, then open
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
	void StartCreate ();
	void StartTask (ETask Kind, std::string Title, const std::wstring& CommandLine, const std::filesystem::path& ProjectFile);
	void SetMessage (std::string Text, bool bError);
	void SaveSettings () const;
	void UpdateSmokeTest ();
	void SmokeLog (const std::string& Line) const;

	CEngine& Engine;
	SProjectManagerRequest Request;
	project::SEngineDescriptor ThisEngine;
	bool bFirstFrame = true;

	std::string Message;
	bool bMessageIsError = false;
	bool bOfferBuild = false;

	std::vector<SRecentProject> Recent;
	int32 Selected = -1;
	bool bShowNewProject = false;        // select the New Project tab on the next frame

	std::vector<STemplate> Templates;
	int32 SelectedTemplate = 0;
	char Name[65] = {};
	char Location[1024] = {};
	bool bEmbeddedEngine = false;

	ETask Task = ETask::None;
	std::string TaskTitle;
	std::filesystem::path TaskProject;   // opened when the task succeeds
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
