param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",
    [string]$BuildDir = "disk/package-build",
    [string]$InstallDir = "disk/package-install",
    [string]$Generator = "",
    [switch]$SkipInstall,
    [string]$OutputDir = "disk/windows",
    [string]$CMakePath = "",
    [string]$CPackPath = "",
    [string]$Qt6Dir = "",
    [string]$CMakePrefixPath = ""
)

$ErrorActionPreference = "Stop"

function Throw-UsageError {
    param([Parameter(Mandatory = $true)][string]$Message)
    $helpLines = @(
        $Message,
        "",
        "Qt arguments are required. Pass at least one (recommended both):",
        '  -Qt6Dir "D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64/lib/cmake/Qt6"',
        '  -CMakePrefixPath "D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64"',
        "",
        "Example:",
        '  .\package-windows.ps1 -Qt6Dir "D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64/lib/cmake/Qt6" -CMakePrefixPath "D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64"'
    )
    throw ($helpLines -join [Environment]::NewLine)
}

function Resolve-Executable {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string[]]$FallbackPaths = @()
    )
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($path in $FallbackPaths) {
        if ($path -and (Test-Path $path)) {
            return (Resolve-Path $path).Path
        }
    }
    return $null
}

function Get-VSWherePath {
    $paths = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
    )
    foreach ($p in $paths) {
        if ($p -and (Test-Path $p)) { return (Resolve-Path $p).Path }
    }
    return $null
}

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][scriptblock]$Action
    )
    Write-Host ""
    Write-Host "==> $Title" -ForegroundColor Cyan
    $global:LASTEXITCODE = 0
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "Step failed: $Title (exit code: $LASTEXITCODE)"
    }
}

if (($env:OS -ne "Windows_NT") -and (-not ($PSVersionTable.PSVersion.Major -ge 6 -and $IsWindows))) {
    throw "This script only supports Windows."
}

if (($Qt6Dir -eq "") -and ($CMakePrefixPath -eq "")) {
    Throw-UsageError "Missing Qt arguments: pass -Qt6Dir or -CMakePrefixPath."
}

if ($Qt6Dir -ne "") {
    if (-not (Test-Path $Qt6Dir)) {
        Throw-UsageError "Invalid -Qt6Dir path: $Qt6Dir"
    }
    $qt6ConfigPath = Join-Path $Qt6Dir "Qt6Config.cmake"
    if (-not (Test-Path $qt6ConfigPath)) {
        Throw-UsageError "Qt6Config.cmake not found under -Qt6Dir: $qt6ConfigPath"
    }
}

if ($CMakePrefixPath -ne "") {
    if (-not (Test-Path $CMakePrefixPath)) {
        Throw-UsageError "Invalid -CMakePrefixPath path: $CMakePrefixPath"
    }
}

$cmakeCandidates = @(
    (Join-Path $env:ProgramFiles "CMake\bin\cmake.exe"),
    (Join-Path ${env:ProgramFiles(x86)} "CMake\bin\cmake.exe"),
    (Join-Path $env:LOCALAPPDATA "Programs\CMake\bin\cmake.exe"),
    (Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"),
    (Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"),
    (Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"),
    (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe")
)

$vswhere = Get-VSWherePath
if ($vswhere) {
    $vsInstallPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($LASTEXITCODE -eq 0 -and $vsInstallPath) {
        $cmakeCandidates += (Join-Path $vsInstallPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe")
    }
}

$cmakeExe = $null
if ($CMakePath -ne "") {
    if (-not (Test-Path $CMakePath)) {
        throw "Provided -CMakePath does not exist: $CMakePath"
    }
    $cmakeExe = (Resolve-Path $CMakePath).Path
}
else {
    $cmakeExe = Resolve-Executable -Name "cmake" -FallbackPaths ($cmakeCandidates | Select-Object -Unique)
}
if (-not $cmakeExe) {
    throw 'cmake not found. Pass -CMakePath "C:/path/to/cmake.exe".'
}

$cpackCandidates = @((Join-Path (Split-Path $cmakeExe -Parent) "cpack.exe"))
$cpackExe = $null
if ($CPackPath -ne "") {
    if (-not (Test-Path $CPackPath)) {
        throw "Provided -CPackPath does not exist: $CPackPath"
    }
    $cpackExe = (Resolve-Path $CPackPath).Path
}
else {
    $cpackExe = Resolve-Executable -Name "cpack" -FallbackPaths $cpackCandidates
}
if (-not $cpackExe) {
    throw "cpack not found in PATH or next to cmake."
}

$nsisExe = Resolve-Executable -Name "makensis" -FallbackPaths @(
    (Join-Path ${env:ProgramFiles(x86)} "NSIS\makensis.exe"),
    (Join-Path $env:ProgramFiles "NSIS\makensis.exe")
)
$wixCandleExe = Resolve-Executable -Name "candle" -FallbackPaths @(
    (Join-Path ${env:ProgramFiles(x86)} "WiX Toolset v3.14\bin\candle.exe"),
    (Join-Path $env:ProgramFiles "WiX Toolset v3.14\bin\candle.exe")
)
$wixExe = Resolve-Executable -Name "wix" -FallbackPaths @(
    (Join-Path ${env:ProgramFiles(x86)} "WiX Toolset v4\bin\wix.exe"),
    (Join-Path $env:ProgramFiles "WiX Toolset v4\bin\wix.exe")
)

if (-not $nsisExe) {
    Write-Warning "NSIS not found (makensis). setup.exe may not be generated."
}
if (-not $wixCandleExe -and -not $wixExe) {
    Write-Warning "WiX not found (candle/wix). MSI may not be generated."
}

$selectedGenerators = @()
if ($nsisExe) { $selectedGenerators += "NSIS" }
if ($wixCandleExe -or $wixExe) { $selectedGenerators += "WIX" }
if ($selectedGenerators.Count -eq 0) {
    throw "No package generator available. Install NSIS (makensis) and/or WiX (candle/wix)."
}

$projectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$absBuildDir = Join-Path $projectRoot $BuildDir
$absInstallDir = Join-Path $projectRoot $InstallDir
$absOutputDir = Join-Path $projectRoot $OutputDir

$configureArgs = @(
    "-S", $projectRoot.Path,
    "-B", $absBuildDir,
    "-DCMAKE_INSTALL_PREFIX=$absInstallDir"
)
if ($Generator -ne "") { $configureArgs += @("-G", $Generator) }
if ($Qt6Dir -ne "") { $configureArgs += @("-DQt6_DIR=$Qt6Dir") }
if ($CMakePrefixPath -ne "") { $configureArgs += @("-DCMAKE_PREFIX_PATH=$CMakePrefixPath") }

Invoke-Step "Configure project" { & $cmakeExe @configureArgs }
Invoke-Step "Build ($Config)" { & $cmakeExe --build $absBuildDir --config $Config }

if (-not $SkipInstall) {
    Invoke-Step "Install ($Config)" { & $cmakeExe --install $absBuildDir --config $Config }
}

$packageSucceededGenerators = @()
$packageFailedGenerators = @()
foreach ($generator in $selectedGenerators) {
    Write-Host ""
    Write-Host "==> Package ($Config, $generator)" -ForegroundColor Cyan
    Push-Location $absBuildDir
    try {
        & $cpackExe --config (Join-Path $absBuildDir "CPackConfig.cmake") -C $Config -G $generator
        if ($LASTEXITCODE -eq 0) {
            $packageSucceededGenerators += $generator
        }
        else {
            $packageFailedGenerators += "$generator(exit=$LASTEXITCODE)"
            Write-Warning "Package failed for generator: $generator"
        }
    }
    finally {
        Pop-Location
    }
}
if ($packageSucceededGenerators.Count -eq 0) {
    throw ("All package generators failed: {0}" -f ($packageFailedGenerators -join ", "))
}
if ($packageFailedGenerators.Count -gt 0) {
    Write-Warning ("Some generators failed: {0}" -f ($packageFailedGenerators -join ", "))
}

$packageFiles = Get-ChildItem -Path $absBuildDir -File |
    Where-Object { $_.Extension -in @(".msi", ".exe", ".zip", ".7z") } |
    Sort-Object LastWriteTime -Descending

if ($packageFiles.Count -eq 0) {
    throw "No package files found in: $absBuildDir"
}

Invoke-Step "Collect artifacts to $OutputDir" {
    New-Item -ItemType Directory -Force -Path $absOutputDir | Out-Null
    foreach ($file in $packageFiles) {
        Copy-Item -Path $file.FullName -Destination (Join-Path $absOutputDir $file.Name) -Force
    }
}

Write-Host ""
Write-Host "Generated package files:" -ForegroundColor Green
foreach ($file in $packageFiles) {
    Write-Host (" - {0}" -f $file.FullName)
}
Write-Host ""
Write-Host ("Copied artifacts to: {0}" -f $absOutputDir) -ForegroundColor Green
Write-Host "Done." -ForegroundColor Green
