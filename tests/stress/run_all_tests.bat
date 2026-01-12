@echo off
cd /d "%~dp0"

echo Starting in 5 seconds...
timeout /t 5 /nobreak >nul

echo.
echo === TIER 1: High-End Current Gen ===
echo Test 1/20: D3D12 Batched
build\Release\stress-test-d3d12.exe -batched-draws 5500 -instanced-draws 70000 -passes 6 -buffer-updates 950 -texture-updates 75 -compute 18 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 2/20: D3D12 Individual
build\Release\stress-test-d3d12.exe -draws 5500 -instanced-draws 70000 -passes 6 -buffer-updates 950 -texture-updates 75 -compute 18 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 3/20: D3D11 Batched
build\Release\stress-test-d3d11.exe -batched-draws 5500 -instanced-draws 70000 -passes 6 -buffer-updates 950 -texture-updates 75 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 4/20: D3D11 Individual
build\Release\stress-test-d3d11.exe -draws 5500 -instanced-draws 70000 -passes 6 -buffer-updates 950 -texture-updates 75 -duration 10 2>&1 | findstr "AVERAGE"

echo.
echo === TIER 2: Extreme Next-Gen ===
echo Test 5/20: D3D12 Batched
build\Release\stress-test-d3d12.exe -batched-draws 12500 -instanced-draws 145000 -passes 8 -buffer-updates 1500 -texture-updates 120 -compute 28 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 6/20: D3D12 Individual
build\Release\stress-test-d3d12.exe -draws 12500 -instanced-draws 145000 -passes 8 -buffer-updates 1500 -texture-updates 120 -compute 28 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 7/20: D3D11 Batched
build\Release\stress-test-d3d11.exe -batched-draws 12500 -instanced-draws 145000 -passes 8 -buffer-updates 1500 -texture-updates 120 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 8/20: D3D11 Individual
build\Release\stress-test-d3d11.exe -draws 12500 -instanced-draws 145000 -passes 8 -buffer-updates 1500 -texture-updates 120 -duration 10 2>&1 | findstr "AVERAGE"

echo.
echo === TIER 3: Maximum Stress ===
echo Test 9/20: D3D12 Batched
build\Release\stress-test-d3d12.exe -batched-draws 20000 -instanced-draws 200000 -passes 10 -buffer-updates 2000 -texture-updates 150 -compute 40 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 10/20: D3D12 Individual
build\Release\stress-test-d3d12.exe -draws 20000 -instanced-draws 200000 -passes 10 -buffer-updates 2000 -texture-updates 150 -compute 40 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 11/20: D3D11 Batched
build\Release\stress-test-d3d11.exe -batched-draws 20000 -instanced-draws 200000 -passes 10 -buffer-updates 2000 -texture-updates 150 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 12/20: D3D11 Individual
build\Release\stress-test-d3d11.exe -draws 20000 -instanced-draws 200000 -passes 10 -buffer-updates 2000 -texture-updates 150 -duration 10 2>&1 | findstr "AVERAGE"

echo.
echo === TIER 4: Draw-Heavy ===
echo Test 13/20: D3D12 Batched
build\Release\stress-test-d3d12.exe -batched-draws 10000 -instanced-draws 5000 -passes 4 -buffer-updates 800 -texture-updates 60 -compute 12 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 14/20: D3D12 Individual
build\Release\stress-test-d3d12.exe -draws 10000 -instanced-draws 5000 -passes 4 -buffer-updates 800 -texture-updates 60 -compute 12 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 15/20: D3D11 Batched
build\Release\stress-test-d3d11.exe -batched-draws 10000 -instanced-draws 5000 -passes 4 -buffer-updates 800 -texture-updates 60 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 16/20: D3D11 Individual
build\Release\stress-test-d3d11.exe -draws 10000 -instanced-draws 5000 -passes 4 -buffer-updates 800 -texture-updates 60 -duration 10 2>&1 | findstr "AVERAGE"

echo.
echo === TIER 5: Instance-Heavy ===
echo Test 17/20: D3D12 Batched
build\Release\stress-test-d3d12.exe -batched-draws 500 -instanced-draws 150000 -passes 3 -buffer-updates 100 -texture-updates 20 -compute 8 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 18/20: D3D12 Individual
build\Release\stress-test-d3d12.exe -draws 500 -instanced-draws 150000 -passes 3 -buffer-updates 100 -texture-updates 20 -compute 8 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 19/20: D3D11 Batched
build\Release\stress-test-d3d11.exe -batched-draws 500 -instanced-draws 150000 -passes 3 -buffer-updates 100 -texture-updates 20 -duration 10 2>&1 | findstr "AVERAGE"

echo Test 20/20: D3D11 Individual
build\Release\stress-test-d3d11.exe -draws 500 -instanced-draws 150000 -passes 3 -buffer-updates 100 -texture-updates 20 -duration 10 2>&1 | findstr "AVERAGE"

echo.
echo === ALL TESTS COMPLETE ===
