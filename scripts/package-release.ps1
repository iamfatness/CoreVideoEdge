<#
.SYNOPSIS
  Builds a CoreVideo Edge Windows beta: Release native node + engine, a
  self-contained operator app, a zip and an unsigned NSIS installer, gated by
  smoke tests. Output: out\release\<ReleaseId>\.

.DESCRIPTION
  1. Native build (Ninja, Release, real Zoom engine) + native tests.
  2. App tests, then a self-contained win-x64 publish (no .NET install needed).
  3. Stage: app publish + node, engines and the Zoom SDK runtime from build\bin.
  4. Gates on the STAGED folder: node --version, the node + fake engine answer
     a state request, the app renders a screen headlessly.
  5. Zip + Setup .exe + .sha256 files + build-manifest.json.
  6. With -TestInstaller: silent install into a scratch folder, the same gates
     on the installed copy, silent uninstall, and checks nothing is left.

  Run from Windows PowerShell or pwsh (not Git Bash: it drops
  ProgramFiles(x86), which vswhere discovery needs). The tree must be clean.

.EXAMPLE
  $env:ZOOM_SDK_DIR = 'C:\Users\walla\Downloads\zoom-sdk-windows-7.1.5.43953\zoom-sdk-windows-7.1.5.43953\x64'
  .\scripts\package-release.ps1 -TestInstaller
#>
param(
  [string]$ReleaseId,
  [string]$ZoomSdkDir = $env:ZOOM_SDK_DIR,
  [string]$VcRedist,
  [switch]$SkipTests,
  [switch]$TestInstaller,
  [switch]$AllowDirty
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build'
Set-Location $repo

function Step($text) { Write-Host ""; Write-Host "== $text ==" -ForegroundColor Cyan }
function Fail($text) { throw "package-release: $text" }

# Native tools write progress to stderr; under 'Stop' PowerShell 5.1 would turn
# that into a terminating error. Run them with 'Continue' and judge the exit code.
function Invoke-Native([scriptblock]$block, [string]$what) {
  $saved = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try { & $block } finally { $ErrorActionPreference = $saved }
  if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit $LASTEXITCODE)" }
}

# ---------------------------------------------------------------- identity
$commit = (git rev-parse HEAD).Trim()
$short = $commit.Substring(0, 7)
$dirty = git status --porcelain
if ($dirty -and -not $AllowDirty) { Fail "the working tree is not clean; commit first (or -AllowDirty for a local trial)" }
if (-not $ReleaseId) { $ReleaseId = "beta-$(Get-Date -Format 'yyyy-MM-dd')-$short" }
[xml]$csproj = Get-Content (Join-Path $repo 'app\CoreVideoEdge.Desktop\CoreVideoEdge.Desktop.csproj')
$appVersion = ($csproj.Project.PropertyGroup | ForEach-Object { $_.Version } | Where-Object { $_ } | Select-Object -First 1)
if (-not $appVersion) { Fail 'no <Version> in CoreVideoEdge.Desktop.csproj' }

if (-not $ZoomSdkDir -or -not (Test-Path (Join-Path $ZoomSdkDir 'h\zoom_sdk.h'))) {
  Fail 'set ZOOM_SDK_DIR (or -ZoomSdkDir) to the Zoom Meeting SDK x64 folder containing h\zoom_sdk.h'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Fail 'vswhere.exe not found (run from PowerShell, not Git Bash)' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Fail 'Visual Studio with the C++ x64 tools is required' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not $VcRedist) {
  $VcRedist = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Recurse -Filter 'vc_redist.x64.exe' -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $VcRedist -or -not (Test-Path $VcRedist)) { Fail 'vc_redist.x64.exe not found; pass -VcRedist' }
$makensis = Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe'
if (-not (Test-Path $makensis)) { Fail "NSIS not found at $makensis" }

$out = Join-Path $repo "out\release\$ReleaseId"
if (Test-Path $out) { Fail "$out already exists; delete it or choose another -ReleaseId" }
New-Item -ItemType Directory -Force $out | Out-Null
$stage = Join-Path $out 'CoreVideoEdge'
Write-Host "Release $ReleaseId  commit $commit  app $appVersion"

# ---------------------------------------------------------------- native
Step 'native build (Release, Ninja, real Zoom engine)'
$cmdFile = Join-Path $out 'build-native.cmd'
$lines = @(
  '@echo off',
  "call `"$vcvars`" >nul 2>nul",
  'if errorlevel 1 exit /b 1',
  "cmake -S `"$repo\native`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DEDGE_BUILD_ZOOM_ENGINE=ON -DZOOM_SDK_DIR=`"$ZoomSdkDir`"",
  'if errorlevel 1 exit /b 1',
  "cmake --build `"$build`"",
  'exit /b %errorlevel%'
)
[IO.File]::WriteAllText($cmdFile, ($lines -join "`r`n") + "`r`n")
Invoke-Native { cmd /c "`"$cmdFile`"" } 'native build'

if (-not $SkipTests) {
  Step 'native tests'
  Invoke-Native { & (Join-Path $build 'bin\corevideo-edge-tests.exe') } 'native tests'
  Step 'app tests'
  Invoke-Native { dotnet test (Join-Path $repo 'app\CoreVideoEdge.Tests\CoreVideoEdge.Tests.csproj') -c Release -p:Platform=x64 --nologo -v minimal } 'app tests'
}

# ---------------------------------------------------------------- app publish
Step 'app publish (self-contained win-x64)'
$publish = Join-Path $out 'publish'
Invoke-Native {
  dotnet publish (Join-Path $repo 'app\CoreVideoEdge.Desktop\CoreVideoEdge.Desktop.csproj') -c Release -r win-x64 `
    --self-contained true -p:Platform=x64 -o $publish --nologo -v minimal
} 'app publish'
if (-not (Test-Path (Join-Path $publish 'CoreVideoEdge.exe'))) { Fail 'publish produced no CoreVideoEdge.exe' }
if (-not (Test-Path (Join-Path $publish 'coreclr.dll'))) { Fail 'publish is not self-contained (no coreclr.dll)' }

# ---------------------------------------------------------------- stage
Step "stage $stage"
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $publish '*') $stage -Recurse
# Test and validator tools stay out of the product.
$skip = @('corevideo-edge-tests.exe', 'corevideo-edge-ndi-probe.exe')
Get-ChildItem (Join-Path $build 'bin') | Where-Object { $skip -notcontains $_.Name -and $_.Extension -ne '.pdb' } |
  ForEach-Object { Copy-Item $_.FullName $stage -Recurse -Force }
foreach ($required in 'CoreVideoEdge.exe', 'corevideo-edge.exe', 'corevideo-zoom-engine.exe', 'corevideo-zoom-engine-fake.exe', 'sdk.dll') {
  if (-not (Test-Path (Join-Path $stage $required))) { Fail "staged folder is missing $required" }
}
Copy-Item (Join-Path $repo 'README.md') $stage

# ---------------------------------------------------------------- gates
function Test-Payload([string]$dir, [string]$label) {
  Step "gates on the $label copy"
  $node = Join-Path $dir 'corevideo-edge.exe'

  $v = & $node --version
  if ($LASTEXITCODE -ne 0 -or "$v" -notmatch '^corevideo-edge ') { Fail "$label node --version failed: $v" }
  Write-Host "  node: $v"

  # The node + fake engine answer a state request, then quit on stdin EOF.
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $node
  $psi.Arguments = '--fake'
  $psi.WorkingDirectory = $dir
  $psi.UseShellExecute = $false
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.CreateNoWindow = $true
  $p = [System.Diagnostics.Process]::Start($psi)
  $errTask = $p.StandardError.ReadToEndAsync()
  $p.StandardInput.WriteLine('{"cmd":"state","id":"gate"}')
  $p.StandardInput.Flush()
  $sawState = $false; $sawEngine = $false
  $deadline = (Get-Date).AddSeconds(20)
  while ((Get-Date) -lt $deadline -and -not ($sawState -and $sawEngine)) {
    $readTask = $p.StandardOutput.ReadLineAsync()
    if (-not $readTask.Wait(5000)) { break }
    $line = $readTask.Result
    if ($null -eq $line) { break }
    if ($line -match '"type"\s*:\s*"state"') {
      $sawState = $true
      if ($line -match '"engineKind"\s*:\s*"fake"') { $sawEngine = $true }
    }
  }
  $p.StandardInput.Close()
  if (-not $p.WaitForExit(15000)) { $p.Kill(); Fail "$label node did not quit on stdin EOF" }
  if (-not $sawState) { Fail "$label node sent no state line. stderr: $($errTask.Result)" }
  if (-not $sawEngine) { Fail "$label node never reported the fake engine. stderr: $($errTask.Result)" }
  Write-Host "  node + fake engine: state OK, clean exit ($($p.ExitCode))"

  # The self-contained app starts and renders a real screen without a window.
  $png = Join-Path $out "gate-$label-routing.png"
  $fixture = Join-Path $repo 'docs\screenshots\fixtures\state-in-meeting.json'
  $app = Start-Process -FilePath (Join-Path $dir 'CoreVideoEdge.exe') -ArgumentList @('--screenshot', "`"$fixture`"", 'routing', "`"$png`"") `
    -WorkingDirectory $dir -PassThru -Wait -WindowStyle Hidden
  if ($app.ExitCode -ne 0 -or -not (Test-Path $png) -or (Get-Item $png).Length -lt 10000) {
    Fail "$label app could not render the Routing screen (exit $($app.ExitCode))"
  }
  Write-Host "  app: rendered Routing headlessly ($([math]::Round((Get-Item $png).Length / 1KB)) KB)"
}
Test-Payload $stage 'staged'

# ---------------------------------------------------------------- manifest
Step 'manifest, zip, installer'
$files = Get-ChildItem $stage -Recurse -File
$exeHashes = [ordered]@{}
foreach ($exe in 'CoreVideoEdge.exe', 'corevideo-edge.exe', 'corevideo-zoom-engine.exe', 'corevideo-zoom-engine-fake.exe') {
  $exeHashes[$exe] = (Get-FileHash (Join-Path $stage $exe) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$nodeVersion = ((& (Join-Path $stage 'corevideo-edge.exe') --version) -replace '^corevideo-edge ', '').Trim()
$manifest = [ordered]@{
  product        = 'CoreVideo Edge'
  releaseId      = $ReleaseId
  commit         = $commit
  appVersion     = $appVersion
  nodeVersion    = $nodeVersion
  zoomSdk        = (Split-Path (Split-Path $ZoomSdkDir -Parent) -Leaf)
  builtAt        = (Get-Date).ToUniversalTime().ToString('o')
  fileCount      = $files.Count
  payloadBytes   = ($files | Measure-Object Length -Sum).Sum
  executables    = $exeHashes
  testsRun       = (-not $SkipTests)
}
$manifestJson = $manifest | ConvertTo-Json -Depth 4
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $stage 'build-manifest.json'), $manifestJson, $utf8)
[IO.File]::WriteAllText((Join-Path $out 'build-manifest.json'), $manifestJson, $utf8)

$zip = Join-Path $out "CoreVideoEdge-$ReleaseId-win-x64.zip"
Invoke-Native { tar.exe -a -c -f $zip -C $out CoreVideoEdge } 'zip'

$setup = Join-Path $out "CoreVideoEdge-$ReleaseId-Setup.exe"
$icon = Join-Path $repo 'app\CoreVideoEdge.Desktop\Assets\CoreVideoEdge.ico'
$nsi = Join-Path $repo 'scripts\installer\CoreVideoEdge.nsi'
Invoke-Native {
  & $makensis /V2 "/DRELEASE_ID=$ReleaseId" "/DAPP_VERSION=$appVersion" "/DSTAGE_DIR=$stage" `
    "/DVCREDIST=$VcRedist" "/DOUTFILE=$setup" "/DICON=$icon" $nsi
} 'makensis'

foreach ($asset in $zip, $setup) {
  $hash = (Get-FileHash $asset -Algorithm SHA256).Hash.ToLowerInvariant()
  [IO.File]::WriteAllText("$asset.sha256", "$hash  $(Split-Path $asset -Leaf)`n", $utf8)
}

# ---------------------------------------------------------------- installer test
if ($TestInstaller) {
  Step 'installer: silent install, gates, silent uninstall'
  $uninstKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideoEdge'
  if (Test-Path $uninstKey) { Fail 'CoreVideo Edge is already installed on this machine; the installer test would replace it' }
  $target = Join-Path $out 'installer-test'
  if ($target -match ' ') { Fail "installer test path must not contain spaces: $target" }
  $proc = Start-Process -FilePath $setup -ArgumentList @('/S', "/D=$target") -PassThru -Wait
  if ($proc.ExitCode -ne 0) { Fail "silent install exited $($proc.ExitCode)" }
  if (-not (Test-Path $uninstKey)) { Fail 'install wrote no HKCU uninstall entry' }
  $lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'CoreVideo Edge.lnk'
  if (-not (Test-Path $lnk)) { Fail 'install created no Start menu shortcut' }
  $installedCount = (Get-ChildItem $target -Recurse -File | Where-Object { $_.Name -ne 'Uninstall.exe' }).Count
  if ($installedCount -ne ($files.Count + 1)) { Fail "installed $installedCount files, staged $($files.Count + 1) (incl. manifest)" }
  Test-Payload $target 'installed'

  $proc = Start-Process -FilePath (Join-Path $target 'Uninstall.exe') -ArgumentList @('/S', "_?=$target") -PassThru -Wait
  if ($proc.ExitCode -ne 0) { Fail "silent uninstall exited $($proc.ExitCode)" }
  if (Test-Path $uninstKey) { Fail 'uninstall left the HKCU uninstall entry' }
  if (Test-Path $lnk) { Fail 'uninstall left the Start menu shortcut' }
  # _?= keeps Uninstall.exe in place (NSIS runs it in place); anything else is a leak.
  $left = Get-ChildItem $target -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'Uninstall.exe' }
  if ($left) { Fail "uninstall left $($left.Count) files in $target" }
  Remove-Item -Recurse -Force $target
  Write-Host '  install -> gates -> uninstall: clean'
}

Remove-Item -Recurse -Force $publish
Step 'done'
Get-ChildItem $out -File | Where-Object { $_.Extension -in '.zip', '.exe', '.sha256', '.json' } |
  ForEach-Object { '{0,-60} {1,8:N1} MB' -f $_.Name, ($_.Length / 1MB) }
