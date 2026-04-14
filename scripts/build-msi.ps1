$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ($PSScriptRoot -match 'scripts$') {
    $RootDir = Split-Path -Parent $PSScriptRoot
}

# Load app metadata
$metadata = @{}
Get-Content "$RootDir\APP_METADATA" | ForEach-Object {
    if ($_ -match '^(\w+)=(.*)$') {
        $metadata[$Matches[1]] = $Matches[2]
    }
}
$AppName = $metadata["APP_NAME"]
if ($env:GITHUB_REF_NAME -match '^v(.+)$') {
    $AppVersion = $Matches[1]
} else {
    $AppVersion = (Select-String -Path "$RootDir\include\config.hpp.in" -Pattern 'APP_VERSION\s+"([^"]+)"').Matches.Groups[1].Value
}

Write-Host "=== Building $AppName $AppVersion MSI installer ==="

# Step 1: Build release
Write-Host "`n--- Step 1: Building release ---"
& "$RootDir\scripts\build-windows.ps1" release --reconfigure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Find the release exe
$BuildDir = "$RootDir\build_release"
$ExePath = "$BuildDir\Release\dearsql.exe"
if (-not (Test-Path $ExePath)) {
    Write-Error "Release exe not found at $ExePath"
    exit 1
}
Write-Host "Release exe: $ExePath ($(((Get-Item $ExePath).Length / 1MB).ToString('F1')) MB)"

# Step 2: Prepare staging directory
Write-Host "`n--- Step 2: Staging ---"
$StageDir = "$BuildDir\msi_stage"
if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
New-Item -ItemType Directory -Path $StageDir | Out-Null
Copy-Item $ExePath "$StageDir\dearsql.exe"
Write-Host "Staged: $StageDir\dearsql.exe"

# Stage WinSparkle.dll (required for auto-updates)
$WinSparkleDll = "$RootDir\external\WinSparkle\x64\Release\WinSparkle.dll"
if (Test-Path $WinSparkleDll) {
    Copy-Item $WinSparkleDll "$StageDir\WinSparkle.dll"
    Write-Host "Staged: $StageDir\WinSparkle.dll"
} else {
    Write-Warning "WinSparkle.dll not found at $WinSparkleDll - auto-update will not work"
}

# Step 3: Build MSI with WiX
Write-Host "`n--- Step 3: Building MSI ---"

# Use a pinned WiX toolchain so local builds and CI don't drift to newer major
# versions with different licensing/runtime behavior.
$WixVersion = "5.0.2"
$WixToolDir = "$BuildDir\wix-tool"
$WixExePath = "$WixToolDir\wix.exe"
$env:WIX_EXTENSIONS = "$WixToolDir\extensions-cache"
New-Item -ItemType Directory -Force -Path $env:WIX_EXTENSIONS | Out-Null

# Ensure dotnet is available
if ($env:DOTNET_ROOT -and (Test-Path "$env:DOTNET_ROOT\dotnet.exe")) {
    $env:PATH = "$env:DOTNET_ROOT;$env:PATH"
} elseif (Test-Path "C:\dotnet\dotnet.exe") {
    $env:DOTNET_ROOT = "C:\dotnet"
    $env:PATH = "C:\dotnet;$env:PATH"
}

$dotnetExe = Get-Command dotnet -ErrorAction SilentlyContinue
if (-not $dotnetExe) {
    Write-Error "dotnet not found. Ensure .NET SDK is installed."
    exit 1
}

if (Test-Path $WixExePath) {
    Write-Host "Updating WiX toolset to $WixVersion..."
    & $dotnetExe.Source tool update --tool-path $WixToolDir wix --version $WixVersion 2>&1 | Out-Null
} else {
    Write-Host "Installing WiX toolset $WixVersion..."
    & $dotnetExe.Source tool install --tool-path $WixToolDir wix --version $WixVersion 2>&1 | Out-Null
}

if (-not (Test-Path $WixExePath)) {
    Write-Error "Failed to install WiX $WixVersion."
    exit 1
}

# Ensure UI extension is available
$wixExtOutput = & $WixExePath extension list -g 2>&1 | Out-String
if ($wixExtOutput -notmatch [regex]::Escape("WixToolset.UI.wixext $WixVersion")) {
    Write-Host "Installing WiX UI extension..."
    & $WixExePath extension add -g "WixToolset.UI.wixext/$WixVersion"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to install WiX UI extension."
        exit $LASTEXITCODE
    }
}

$WxsFile = "$RootDir\packaging\windows\DearSQL.wxs"
$OutputMsi = "$BuildDir\${AppName}-x64.msi"

& $WixExePath build $WxsFile `
    -ext WixToolset.UI.wixext `
    -d ProjectDir="$StageDir" `
    -d SourceDir="$RootDir" `
    -d AppVersion="$AppVersion" `
    -o $OutputMsi `
    -arch x64

if ($LASTEXITCODE -ne 0) {
    Write-Error "WiX build failed"
    exit $LASTEXITCODE
}

$MsiSize = ((Get-Item $OutputMsi).Length / 1MB).ToString("F1")
Write-Host "`n=== MSI built successfully ==="
Write-Host "Output: $OutputMsi ($MsiSize MB)"
