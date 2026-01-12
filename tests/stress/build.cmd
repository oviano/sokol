@echo off
cd /d "%~dp0"
if exist build rmdir /s /q build
mkdir build
cd build
cmake .. -G "Visual Studio 18 2026" -A x64
cmake --build . --config Debug
cmake --build . --config Release
cd ..