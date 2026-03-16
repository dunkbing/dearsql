$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ($PSScriptRoot -match 'scripts$') {
    $RootDir = Split-Path -Parent $PSScriptRoot
}

# Always set up MSVC environment via vcvarsall (ensures INCLUDE/LIB are correct,
# even if cl.exe is on PATH from another source like Strawberry Perl's toolchain)
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vsWhere) {
    $vsPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsPath) {
        $vcvarsall = "$vsPath\VC\Auxiliary\Build\vcvarsall.bat"
        if (Test-Path $vcvarsall) {
            Write-Host "Loading MSVC environment..."
            cmd /c "`"$vcvarsall`" x64 && set" | ForEach-Object {
                if ($_ -match '^([^=]+)=(.*)$') {
                    [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
                }
            }
        }
    }
}
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    Write-Error "MSVC compiler (cl.exe) not found. Install Visual Studio with C++ workload, or run from a Developer Command Prompt."
    exit 1
}

# Load app metadata
$metadata = @{}
Get-Content "$RootDir\APP_METADATA" | ForEach-Object {
    if ($_ -match '^(\w+)=(.*)$') {
        $metadata[$Matches[1]] = $Matches[2]
    }
}
$AppName = $metadata["APP_NAME"]

# Check for vcpkg
if (-not $env:VCPKG_ROOT) {
    if (Test-Path "$HOME\vcpkg") {
        $env:VCPKG_ROOT = "$HOME\vcpkg"
    } elseif (Test-Path "C:\vcpkg") {
        $env:VCPKG_ROOT = "C:\vcpkg"
    } else {
        Write-Error @"
vcpkg not found. Install it with:
  git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
  C:\vcpkg\bootstrap-vcpkg.bat
"@
        exit 1
    }
}
Write-Host "Using vcpkg at: $env:VCPKG_ROOT"

# Parse arguments
$BuildType = "Debug"
$BuildDir = "$RootDir\build"
$Reconfigure = $false
foreach ($arg in $args) {
    switch ($arg) {
        "release" { $BuildType = "Release"; $BuildDir = "$RootDir\build_release" }
        "--reconfigure" { $Reconfigure = $true }
    }
}
if ($env:BUILD_DIR_OVERRIDE) {
    $BuildDir = $env:BUILD_DIR_OVERRIDE
}

# Generate .ico from .png if needed
$icoPath = "$RootDir\assets\appicon.ico"
$pngPath = "$RootDir\assets\appicon.png"
if (-not (Test-Path $icoPath) -and (Test-Path $pngPath)) {
    Write-Host "Converting appicon.png to appicon.ico..."
    Add-Type -AssemblyName System.Drawing
    $png = [System.Drawing.Image]::FromFile($pngPath)
    $icon = [System.Drawing.Icon]::FromHandle($png.GetHicon())
    $stream = [System.IO.File]::Create($icoPath)
    $icon.Save($stream)
    $stream.Close()
    $icon.Dispose()
    $png.Dispose()
}

# Only run cmake configure if build dir doesn't exist or CMakeCache is missing
if (-not (Test-Path "$BuildDir\CMakeCache.txt") -or $Reconfigure) {
    Write-Host "Configuring CMake ($BuildType)..."
    cmake -S "$RootDir" -B "$BuildDir" `
        -G "Visual Studio 17 2022" `
        -A x64 `
        -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
        -DVCPKG_TARGET_TRIPLET=x64-windows-static
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
    Write-Host "Skipping CMake configure (already configured). Use --reconfigure to force."
}

Write-Host "Building $AppName..."
cmake --build "$BuildDir" --config "$BuildType" --target dearsql
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Build complete: $BuildDir\$AppName.exe"
