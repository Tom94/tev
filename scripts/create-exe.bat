@echo off

set cwd=%cd%
cd /D %~dp0

set DevCmd="C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
set MSBuildOptions=/v:m /p:Configuration=Release
set BuildDir64="build-exe-64"

call %DevCmd%

echo Building 64-bit tev...
mkdir %BuildDir64%
cd %BuildDir64%
cmake -DTEV_DEPLOY=1 -DCMAKE_BUILD_TYPE=Release -G "Visual Studio 16 2019" ..\..
msbuild %MSBuildOptions% tev.sln
move "Release\tev.exe" "..\..\tev.exe"
cd ..
rmdir /S /Q %BuildDir64%

echo Returning to original directory.
cd /D %cwd%
pause
