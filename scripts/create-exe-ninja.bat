@echo off

set cwd=%cd%
cd /D %~dp0

set DevCmd="C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
set BuildDir64="build-exe-64"

call %DevCmd%

echo Building tev with ninja...
mkdir %BuildDir64%
cd %BuildDir64%
cmake -DTEV_DEPLOY=1 -DCMAKE_BUILD_TYPE=Release -GNinja ..\..
ninja
move "tev.exe" "..\..\tev.exe"
cd ..

echo Returning to original directory.
cd /D %cwd%
pause
