@echo off
setlocal

if not exist build mkdir build
cd build

cmake .. -A x64
cmake --build . --config Release

echo.
echo Output: build\Release\SkyMPFixes.dll
pause
