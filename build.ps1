param(
    [string]$QtPrefix = 'D:\libs\Qt\5.15.2\msvc2019_64',
    [string]$VcpkgRoot = 'D:\libs\vcpkg',
    [string]$OutputName = 'iLightDesktop',
    [switch]$SkipConfigure
)
$ErrorActionPreference = 'Stop'
if ($OutputName -notmatch '^[A-Za-z0-9-]+$') { throw 'Invalid OutputName' }
$env:Qt5_DIR = $QtPrefix
$env:VCPKG_ROOT = $VcpkgRoot
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$deployQt = Join-Path $QtPrefix 'bin\windeployqt.exe'
if (-not (Test-Path -LiteralPath $deployQt)) { throw "Qt deployment tool not found: $deployQt" }
if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'))) { throw 'Invalid VcpkgRoot' }
Push-Location $PSScriptRoot
try {
    if (-not $SkipConfigure) {
        & $cmake --preset windows-vs2019
        if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    }
    & $cmake --build --preset release --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed' }
    $output = Join-Path $PSScriptRoot ("dist\" + $OutputName)
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $executable = Join-Path $output 'iLightDesktop.exe'
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build\bin\Release\iLightDesktop.exe') -Destination $executable -Force
    & $deployQt --release --compiler-runtime --no-translations $executable
    if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed' }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination (Join-Path $output 'README.md') -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses\Qt-LICENSE.LGPL3.txt') -Destination (Join-Path $output 'Qt-LICENSE.LGPL3.txt') -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses\Qt-LICENSE.GPL3.txt') -Destination (Join-Path $output 'Qt-LICENSE.GPL3.txt') -Force
    Compress-Archive -Path $output -DestinationPath (Join-Path (Join-Path $PSScriptRoot 'dist') ("${OutputName}-windows-x64.zip")) -Force
    Write-Output "Ready: $executable"
}
finally { Pop-Location }
