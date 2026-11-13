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
    $AppVersion = $metadata["APP_VERSION"]
}
if (-not $AppVersion) {
    Write-Error "APP_VERSION not found (checked GITHUB_REF_NAME and APP_METADATA)"
    exit 1
}

Write-Host "=== Building $AppName $AppVersion installer ==="

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

# Step 2: Stage what the installer ships
Write-Host "`n--- Step 2: Staging ---"
$StageDir = "$BuildDir\installer_stage"
if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
New-Item -ItemType Directory -Path "$StageDir\tools" | Out-Null
Copy-Item $ExePath "$StageDir\dearsql.exe"

# WinSparkle.dll (auto-updates) and the staged-update helper are required
$WinSparkleDll = "$RootDir\external\WinSparkle\x64\Release\WinSparkle.dll"
$Helper = "$BuildDir\Release\tools\imshell-update.exe"
foreach ($f in @($WinSparkleDll, $Helper)) {
    if (-not (Test-Path $f)) {
        Write-Error "Missing $f"
        exit 1
    }
}
Copy-Item $WinSparkleDll "$StageDir\WinSparkle.dll"
Copy-Item $Helper "$StageDir\tools\imshell-update.exe"
Get-ChildItem -Recurse -File $StageDir | ForEach-Object { Write-Host "Staged: $($_.FullName)" }

# Step 3: Build the installer with Inno Setup 6 (preinstalled on GitHub's Windows runners)
Write-Host "`n--- Step 3: Building installer ---"
$Iscc = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Iscc) {
    Write-Error "Inno Setup 6 not found (ISCC.exe). Install it from https://jrsoftware.org/isdl.php"
    exit 1
}

& $Iscc /Q "/DAppVersion=$AppVersion" "/DStageDir=$StageDir" "/DSourceDir=$RootDir" "/O$BuildDir" `
    "$RootDir\packaging\windows\DearSQL.iss"
if ($LASTEXITCODE -ne 0) {
    Write-Error "Inno Setup build failed"
    exit $LASTEXITCODE
}

$Output = "$BuildDir\${AppName}-x64-setup.exe"
$Size = ((Get-Item $Output).Length / 1MB).ToString("F1")
Write-Host "`n=== Installer built successfully ==="
Write-Host "Output: $Output ($Size MB)"
