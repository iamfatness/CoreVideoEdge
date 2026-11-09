<#
.SYNOPSIS
  Builds CoreVideo Edge (Release, Windows x64): the native node, the vendored
  Zoom engine (real + fake), the operator app, and runs both test suites.

.EXAMPLE
  $env:ZOOM_SDK_DIR = 'C:\zoom-sdk-windows-7.0.5.39292\zoom-sdk-windows-7.0.5.39292\x64'
  .\scripts\build.ps1

.EXAMPLE
  .\scripts\build.ps1 -NoZoomEngine        # no SDK on this machine: fake engine only
  .\scripts\build.ps1 -Dist                # also stage dist\CoreVideoEdge for an Edge machine
#>
param(
  [string]$ZoomSdkDir = $env:ZOOM_SDK_DIR,
  [switch]$NoZoomEngine,
  [switch]$SkipTests,
  [switch]$Dist
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio (C++ workload) not found: vswhere.exe is missing.' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ x64 tools is required.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$engineFlag = '-DEDGE_BUILD_ZOOM_ENGINE=ON'
if ($NoZoomEngine) {
  $engineFlag = '-DEDGE_BUILD_ZOOM_ENGINE=OFF'
} elseif (-not $ZoomSdkDir -or -not (Test-Path (Join-Path $ZoomSdkDir 'h\zoom_sdk.h'))) {
  throw "Set ZOOM_SDK_DIR (or -ZoomSdkDir) to the Zoom Meeting SDK x64 folder containing h\zoom_sdk.h, or pass -NoZoomEngine."
}

Write-Host "== native (Release, Ninja) =="
$configure = "cmake -S `"$repo\native`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release $engineFlag -DZOOM_SDK_DIR=`"$ZoomSdkDir`""
cmd /c "`"$vcvars`" >nul && $configure && cmake --build `"$build`""
if ($LASTEXITCODE -ne 0) { throw 'native build failed' }

if (-not $SkipTests) {
  Write-Host "== native tests =="
  & (Join-Path $build 'bin\corevideo-edge-tests.exe')
  if ($LASTEXITCODE -ne 0) { throw 'native tests failed' }
}

Write-Host "== operator app (Release x64) =="
dotnet build (Join-Path $repo 'app\CoreVideoEdge.sln') -c Release -p:Platform=x64 --nologo -v minimal
if ($LASTEXITCODE -ne 0) { throw 'app build failed' }
if (-not $SkipTests) {
  dotnet test (Join-Path $repo 'app\CoreVideoEdge.Tests\CoreVideoEdge.Tests.csproj') -c Release -p:Platform=x64 --nologo --no-build
  if ($LASTEXITCODE -ne 0) { throw 'app tests failed' }
}

if ($Dist) {
  $out = Join-Path $repo 'dist\CoreVideoEdge'
  Write-Host "== staging $out =="
  if (Test-Path $out) { Remove-Item -Recurse -Force $out }
  New-Item -ItemType Directory -Force $out | Out-Null
  $app = Join-Path $repo 'app\CoreVideoEdge.Desktop\bin\x64\Release\net9.0'
  if (-not (Test-Path (Join-Path $app 'CoreVideoEdge.exe'))) { throw "operator app not built: $app\CoreVideoEdge.exe is missing" }
  Copy-Item "$app\*" $out -Recurse
  # Node, engines and (when built) the Zoom SDK runtime the engine needs beside it.
  Get-ChildItem (Join-Path $build 'bin') | Where-Object { $_.Name -ne 'corevideo-edge-tests.exe' } |
    ForEach-Object { Copy-Item $_.FullName $out -Recurse -Force }
  Write-Host "Staged. Copy $out to the Edge machine and run CoreVideoEdge.exe (needs the .NET 9 Runtime)."
  Write-Host "dist\ contains the Zoom SDK runtime: never commit it."
}
Write-Host "Done."
