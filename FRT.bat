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
"%~dp0ThirdParty\premake-5.0.0-beta2-windows\premake5.exe" --file="%~dp0premake5.lua" %*
exit /b %errorlevel%
