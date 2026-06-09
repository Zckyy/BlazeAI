<#
.SYNOPSIS
    Builds the ScreenAnalysisTool project in Release configuration.
.DESCRIPTION
    Configures the CMake build directory (if not already configured) and runs the build.
    The resulting executable and dependency DLLs will be output to build/Release.
#>

$ErrorActionPreference = "Stop"

# Get absolute path of script directory
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Definition
if ([string]::IsNullOrEmpty($ProjectRoot)) {
    $ProjectRoot = Get-Location
}

Write-Host "Project Root: $ProjectRoot" -ForegroundColor Cyan

# Set paths
$BuildDir = Join-Path $ProjectRoot "build"
$ReleaseDir = Join-Path $BuildDir "Release"
$VcpkgToolchain = Join-Path $ProjectRoot "vcpkg/scripts/buildsystems/vcpkg.cmake"
$OrtDir = Join-Path $ProjectRoot "onnxruntime-win-x64-gpu-1.26.0"
if (-not (Test-Path $OrtDir)) {
    $OrtDir = Join-Path (Split-Path -Parent $ProjectRoot) "onnxruntime-win-x64-gpu-1.26.0"
}
$OrtInclude = Join-Path $OrtDir "include"
$OrtLib = Join-Path $OrtDir "lib/onnxruntime.lib"

# Check if onnxruntime exists
if (-not (Test-Path $OrtDir)) {
    Write-Error "ONNX Runtime folder not found at: $OrtDir. Please ensure it exists before building."
}

# Create build directory if it doesn't exist
if (-not (Test-Path $BuildDir)) {
    Write-Host "Creating build directory..." -ForegroundColor Green
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

# Run CMake Configuration if CMakeCache.txt doesn't exist
$CMakeCache = Join-Path $BuildDir "CMakeCache.txt"
if (-not (Test-Path $CMakeCache)) {
    Write-Host "Configuring CMake project..." -ForegroundColor Green
    cmake -S $ProjectRoot -B $BuildDir `
        -DCMAKE_TOOLCHAIN_FILE="$VcpkgToolchain" `
        -DONNXRUNTIME_INCLUDE_DIRS="$OrtInclude" `
        -DONNXRUNTIME_LIBRARIES="$OrtLib"
} else {
    Write-Host "CMake project already configured. Skipping configuration step." -ForegroundColor Yellow
}

# Build the project
Write-Host "Building project in Release mode..." -ForegroundColor Green
cmake --build $BuildDir --config Release

# Verify output
$ExePath = Join-Path $ReleaseDir "ScreenAnalysisTool.exe"
if (Test-Path $ExePath) {
    Write-Host "`nBuild succeeded!" -ForegroundColor Green
    Write-Host "Output files are located at: $ReleaseDir" -ForegroundColor Cyan
    Get-ChildItem -Path $ReleaseDir
} else {
    Write-Error "Build finished but ScreenAnalysisTool.exe was not found in: $ReleaseDir"
}
