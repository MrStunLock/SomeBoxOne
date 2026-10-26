@echo off
setlocal EnableExtensions
cd /d "%~dp0"
if errorlevel 1 goto path_failed

echo === SomeBoxOne launcher ===
echo Project folder: "%~dp0"
echo.

if not exist "%~dp0build_windows.bat" goto build_script_missing
set "SOMEBOXONE_FROM_LAUNCHER=1"
echo Calling build script:
echo "%~dp0build_windows.bat"
call "%~dp0build_windows.bat"
set "BUILD_EXIT_CODE=%ERRORLEVEL%"
set "SOMEBOXONE_FROM_LAUNCHER="
if not "%BUILD_EXIT_CODE%"=="0" goto build_failed

if not exist "%~dp0build\Release\SomeBoxOneGUI.exe" goto gui_missing
start "SomeBoxOne" "%~dp0build\Release\SomeBoxOneGUI.exe"
if errorlevel 1 goto launch_failed
exit /b 0

:build_script_missing
echo ERROR: Required build script was not found:
echo "%~dp0build_windows.bat"
echo This package may have been extracted incompletely. Re-extract the full ZIP.
goto failed_pause

:path_failed
echo ERROR: Could not change to project folder:
echo "%~dp0"
goto failed_pause

:build_failed
echo.
echo SomeBoxOne could not be built or its tests failed. Exit code: %BUILD_EXIT_CODE%
echo The command window will remain open so you can read the error above.
goto failed_pause

:gui_missing
echo.
echo Build and tests completed, but SomeBoxOneGUI.exe was not found at:
echo "%~dp0build\Release\SomeBoxOneGUI.exe"
echo Check whether CMake generated the Windows GUI target.
goto failed_pause

:launch_failed
echo.
echo SomeBoxOneGUI.exe exists, but Windows could not start it.
goto failed_pause

:failed_pause
echo.
pause
exit /b 1
