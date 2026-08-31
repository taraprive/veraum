# Builds the C++ bot + tests with the MSVC toolchain installed at D:\VSBuildTools.
# Usage:  powershell -ExecutionPolicy Bypass -File scripts\build.ps1
$ErrorActionPreference = "Stop"

$vsRoot = "D:\VSBuildTools"
$cmake = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if (-not (Test-Path $cmake)) {
    throw "CMake not found at $cmake. Install Visual Studio Build Tools first."
}

$buildDir = Join-Path $PSScriptRoot "..\build"

cmd /c "call `"$vsRoot\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && `"$cmake`" -G Ninja -B `"$buildDir`" -DCMAKE_BUILD_TYPE=Release && `"$cmake`" --build `"$buildDir`""
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host ""
Write-Host "Build OK. Running tests..."
& (Join-Path $buildDir "tests\hft_tests.exe")
