@echo off
rem Run once after cloning (double-click is fine): registers this engine for projects to find by
rem name and generates FRTEngine2.sln. The same as FRT.bat setup.
call "%~dp0FRT.bat" setup %*
pause
