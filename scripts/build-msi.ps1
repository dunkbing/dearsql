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
$AppVersion = $metadata["APP_VERSION"]

Write-Host "=== Building $AppName $AppVersion MSI installer ==="

# Step 1: Build release
Write-Host "`n--- Step 1: Building release ---"
& "$RootDir\scripts\build-windows.ps1" release
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

# Step 3: Build MSI with WiX
Write-Host "`n--- Step 3: Building MSI ---"

# Ensure dotnet and wix are available
if ($env:DOTNET_ROOT) {
    $env:PATH = "$env:DOTNET_ROOT;$env:USERPROFILE\.dotnet\tools;$env:PATH"
} elseif (Test-Path "C:\dotnet\dotnet.exe") {
    $env:DOTNET_ROOT = "C:\dotnet"
    $env:PATH = "C:\dotnet;$env:USERPROFILE\.dotnet\tools;$env:PATH"
}

$wixExe = Get-Command wix -ErrorAction SilentlyContinue
if (-not $wixExe) {
    Write-Host "Installing WiX toolset..."
    & "$env:DOTNET_ROOT\dotnet.exe" tool install --global wix 2>&1 | Out-Null
    $env:PATH = "$env:USERPROFILE\.dotnet\tools;$env:PATH"
    $wixExe = Get-Command wix -ErrorAction SilentlyContinue
    if (-not $wixExe) {
        Write-Error "Failed to install WiX. Ensure .NET SDK is installed."
        exit 1
    }
}

# Ensure UI extension is available
$wixExtOutput = wix extension list 2>&1 | Out-String
if ($wixExtOutput -notmatch "WixToolset.UI.wixext") {
    Write-Host "Installing WiX UI extension..."
    $wixVer = (wix --version) -replace '\+.*', ''
    wix extension add "WixToolset.UI.wixext/$wixVer"
}

$WxsFile = "$RootDir\packaging\windows\DearSQL.wxs"
$OutputMsi = "$BuildDir\${AppName}-${AppVersion}-x64.msi"

wix build $WxsFile `
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
