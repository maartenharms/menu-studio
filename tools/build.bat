@echo off
call :main > "%~dp0build.log" 2>&1
exit /b %errorlevel%

:main
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto :fail
set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "CM=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "%~dp0.."
echo === CONFIGURE START ===
"%CM%" --preset release
if errorlevel 1 goto :fail
echo === BUILD START ===
"%CM%" --build build/release
if errorlevel 1 goto :fail
echo === ALL_DONE ===
exit /b 0

:fail
echo ***BUILD_FAILED*** errorlevel %errorlevel%
exit /b 1
