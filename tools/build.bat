@echo off
call :main > "%~dp0build.log" 2>&1
exit /b %errorlevel%

:main
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto :fail
rem r28e: PRESERVE THE DEPLOYED INI ACROSS THE DEPLOY. The POST_BUILD step
rem copies dist\ over the deploy folder, INI included, which wiped the user's
rem saved settings on every single build. In the field that silently reverted
rem "keep menus unpaused" before each test session and sabotaged three test
rem rounds in a row. Saved before the build, restored after the deploy; dist
rem still provides the INI on a first-ever install (no file to preserve).
set "DEPLOY_INI=C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods\Menu Studio\SKSE\Plugins\MenuStudio.ini"
set "INI_KEEP=%TEMP%\MenuStudio.ini.keep"
if exist "%DEPLOY_INI%" copy /y "%DEPLOY_INI%" "%INI_KEEP%" >nul
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
if exist "%INI_KEEP%" (
    copy /y "%INI_KEEP%" "%DEPLOY_INI%" >nul
    del "%INI_KEEP%"
    echo === DEPLOYED INI RESTORED ===
)
echo === ALL_DONE ===
exit /b 0

:fail
echo ***BUILD_FAILED*** errorlevel %errorlevel%
exit /b 1
