@echo off
REM AE-port scratch build: builds the universal DLL to a SCRATCH folder (never
REM the live Nolvus deploy), then restores the deploy path in the CMake cache so
REM a normal build.bat still deploys to Nolvus. Mirrors the Fitting Room pattern.
call :main > "%~dp0aebuild.log" 2>&1
exit /b %errorlevel%

:main
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto :fail
set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "CM=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "SCRATCH=C:/Users/Maarten/AppData/Local/Temp/claude/C--Studios-Mod-Studio/a620436b-3f6f-404f-886a-0c112b6ee557/scratchpad/aeout"
set "DEPLOY=C:/Games/Nolvus/Instances/Nolvus Awakening/MODS/mods/Menu Studio"
cd /d "%~dp0.."
echo === CONFIGURE (scratch OUTPUT_FOLDER) ===
"%CM%" --preset release -DOUTPUT_FOLDER="%SCRATCH%"
if errorlevel 1 goto :fail
echo === BUILD START ===
"%CM%" --build build/release
set "BUILDRC=%errorlevel%"
echo === RESTORE deploy path (Nolvus) ===
"%CM%" --preset release -DOUTPUT_FOLDER="%DEPLOY%" 1>nul 2>nul
if not "%BUILDRC%"=="0" goto :fail
echo === ALL_DONE ===
exit /b 0

:fail
echo ***BUILD_FAILED*** errorlevel %errorlevel%
exit /b 1
