@echo off
rem FRTEngine's command line: FRT.bat <command> [options]. FRT.bat --help lists everything.
rem   setup      registers this engine for projects to find, generates FRTEngine2.sln (once per checkout)
rem   new        creates a project: FRT.bat new --name=MyGame [--template=Demo] [--layout=embedded] [--build]
rem   generate   (re)generates the engine's solution, or a project's (--project=<.frtproject file>)
rem   build      builds the engine, or a project (--config=Development|Debug|Release[-Headless])
rem   open       opens the solution
rem   package    builds a project in Release and writes the game, with everything it runs with, to a
rem              directory that runs anywhere: FRT.bat package --project=<file> [--output=<dir>]
rem Premake/tools.lua has the details. A project has its own FRT.bat, which runs this one for it.
setlocal

rem Premake runs with the UTF-8 code page (ThirdParty\premake-5.0.0-beta2-windows\VERSION.txt), so
rem paths in any language work. The console is UTF-8 too while it runs, so that its output and the
rem tools it starts show them as written; then it goes back to its own code page.
set "FRT_CODE_PAGE="
for /f "tokens=2 delims=:." %%c in ('chcp 2^>nul') do set "FRT_CODE_PAGE=%%c"
chcp 65001 >nul 2>&1

"%~dp0ThirdParty\premake-5.0.0-beta2-windows\premake5.exe" --file="%~dp0premake5.lua" %*
set "FRT_EXIT_CODE=%errorlevel%"

if defined FRT_CODE_PAGE chcp %FRT_CODE_PAGE% >nul 2>&1
exit /b %FRT_EXIT_CODE%
