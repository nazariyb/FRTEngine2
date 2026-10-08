@echo off
rem This project's command line: its engine's FRT.bat, run for this project.
rem   FRT.bat generate    (re)generates the solution
rem   FRT.bat build       builds the game and the editor (--config=Debug|Development|Release[-Headless])
rem   FRT.bat open        opens the solution
rem The engine is found the way the engine finds it (Core/Source/Project/Project.h in the engine):
rem FRT_ENGINE_DIR when set; else the .frtproject's Engine - a directory relative to this one, else
rem the name of an engine registered by its FRT.bat setup; else the nearest engine above this
rem directory.
setlocal EnableExtensions DisableDelayedExpansion

set "PROJECT_FILE="
for %%f in ("%~dp0*.frtproject") do set "PROJECT_FILE=%%~ff"
if not defined PROJECT_FILE goto no_project

rem The files read below are UTF-8: any user name, any folder.
set "CODE_PAGE="
for /f "tokens=2 delims=:." %%c in ('chcp') do set "CODE_PAGE=%%c"
chcp 65001 >nul

set "ENGINE_DIR=%FRT_ENGINE_DIR%"
if defined ENGINE_DIR goto run

set "ENGINE="
for /f "usebackq eol=; tokens=1,* delims==" %%a in ("%PROJECT_FILE%") do if /i "%%a"=="Engine" set "ENGINE=%%b"
if not defined ENGINE goto above

if exist "%~dp0%ENGINE%\Engine.frtengine" for %%d in ("%~dp0%ENGINE%") do set "ENGINE_DIR=%%~fd"
if defined ENGINE_DIR goto run

set "REGISTRY=%LOCALAPPDATA%\FRTEngine"
if defined FRT_USER_DATA_DIR set "REGISTRY=%FRT_USER_DATA_DIR%"
if exist "%REGISTRY%\Engines.ini" for /f "usebackq eol=; tokens=1,* delims==" %%a in ("%REGISTRY%\Engines.ini") do if /i "%%a"=="%ENGINE%" set "ENGINE_DIR=%%b"
if defined ENGINE_DIR if exist "%ENGINE_DIR%\Engine.frtengine" goto run
set "ENGINE_DIR="

:above
for %%d in ("%~dp0.") do set "DIR=%%~fd"
:above_next
if exist "%DIR%\Engine.frtengine" goto above_found
for %%d in ("%DIR%\..") do set "PARENT=%%~fd"
if /i "%PARENT%"=="%DIR%" goto no_engine
set "DIR=%PARENT%"
goto above_next
:above_found
set "ENGINE_DIR=%DIR%"

:run
if defined CODE_PAGE chcp %CODE_PAGE% >nul
call "%ENGINE_DIR%\FRT.bat" %* "--project=%PROJECT_FILE%"
exit /b %errorlevel%

:no_engine
if defined CODE_PAGE chcp %CODE_PAGE% >nul
if defined ENGINE echo No engine "%ENGINE%": no directory of that name with an Engine.frtengine next to the project, and no engine registered under it. Run FRT.bat setup in the engine's checkout. 1>&2
if not defined ENGINE echo The project names no engine and is not inside one. 1>&2
exit /b 1

:no_project
echo No .frtproject file next to this script. 1>&2
exit /b 1
