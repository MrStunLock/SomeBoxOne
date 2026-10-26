@echo off
setlocal EnableExtensions
cd /d "%~dp0"
if errorlevel 1 goto path_failed

echo.
echo === SomeBoxOne Windows build ===
echo Project folder: "%~dp0"
echo Build folder:   "%~dp0build"
echo.

where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: CMake was not found on PATH.
  echo Install CMake 3.20 or newer and Visual Studio 2022 Build Tools with Desktop development with C++.
  set "BUILD_EXIT_CODE=9009"
  goto failed
)

rem Use the Visual Studio x64 generator on first configure. If an existing
rem cache is present, keep its generator and just reconfigure the same build.
rem IMPORTANT: use the current project directory as "." for CMake's source
rem path. Passing "%~dp0" ends in a backslash immediately before the closing
rem quote; some Windows argument parsers treat that as an escaped quote and
rem split paths containing spaces. We already cd /d into this directory above.
if exist "build\CMakeCache.txt" (
  cmake -S . -B build -DBUILD_TESTING=ON
) else (
  cmake -S . -B build -A x64 -DBUILD_TESTING=ON
)
if errorlevel 1 (
  set "BUILD_EXIT_CODE=%ERRORLEVEL%"
  goto failed
)

cmake --build build --config Release --parallel
if errorlevel 1 (
  set "BUILD_EXIT_CODE=%ERRORLEVEL%"
  goto failed
)

ctest --test-dir build -C Release --output-on-failure
if errorlevel 1 (
  set "BUILD_EXIT_CODE=%ERRORLEVEL%"
  goto failed
)

echo.
echo SUCCESS: SomeBoxOne built and all registered tests passed.
echo GUI target, when available, should be at:
echo "%~dp0build\Release\SomeBoxOneGUI.exe"
echo Separate D3D11/DXGI graphics window target, when available, should be at:
echo "%~dp0build\Release\SomeBoxOneDisplay.exe"
echo.
if defined SOMEBOXONE_FROM_LAUNCHER exit /b 0
echo Press any key to close this build window.
pause >nul
exit /b 0

:path_failed
echo ERROR: Could not change to the project folder:
echo "%~dp0"
set "BUILD_EXIT_CODE=1"
goto failed

:failed
if not defined BUILD_EXIT_CODE set "BUILD_EXIT_CODE=1"
echo.
echo BUILD FAILED with exit code %BUILD_EXIT_CODE%.
echo Project folder: "%~dp0"
echo Check the output above for the first CMake, compiler, linker, or test error.
echo.
if defined SOMEBOXONE_FROM_LAUNCHER exit /b %BUILD_EXIT_CODE%
echo Press any key to close this build window.
pause >nul
exit /b %BUILD_EXIT_CODE%
