<#
.SYNOPSIS
  Renders docs/screenshots/*.png headlessly (no visible window) from node state
  lines captured off the fake engine.

.DESCRIPTION
  1. Capture states + live preview files (they vanish when the node exits):
       node scripts\capture-ui-fixtures.mjs build\bin\corevideo-edge.exe out\captures D:\EdgeISO main
       node scripts\capture-ui-fixtures.mjs build\bin\corevideo-edge.exe out\captures D:\EdgeISO move
       node scripts\capture-ui-fixtures.mjs build\bin\corevideo-edge.exe out\captures D:\EdgeISO feeds
  2. .\scripts\render-screenshots.ps1 [-Captures out\captures]

  State lines come from -Captures when present, else docs\screenshots\fixtures
  (committed copies). Preview files are only in -Captures (not committed: about
  0.5 MB each); without them Monitor tiles show "Waiting for the node's preview".
#>
param(
  [string]$Captures = 'out\captures',
  [string]$Exe = 'app\CoreVideoEdge.Desktop\bin\x64\Release\net9.0\CoreVideoEdge.exe'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
  $out = 'docs\screenshots'
  $fixtures = 'docs\screenshots\fixtures'
  function State($name) {
    $c = Join-Path $Captures "state-$name.json"
    if (Test-Path $c) { return $c }
    return Join-Path $fixtures "state-$name.json"
  }
  function Preview($name) {
    $p = Join-Path $Captures "preview-$name"
    if (Test-Path $p) { return @('--preview-dir', $p) }
    return @()
  }
  $shots = @(
    @('connect-signed-out', 'idle', 'connect', @()),
    @('connect-sign-in-pending', 'idle', 'connect', @('--sign-in-pending')),
    @('connect-signed-in', 'idle', 'connect', @('--signed-in', '--meeting-link', 'https://zoom.us/j/81234567890?pwd=fake')),
    @('connect-join-failed', 'join-failed.edited', 'connect', @('--signed-in', '--meeting-link', 'https://zoom.us/j/81234567890')),
    @('connect-in-meeting', 'in-meeting', 'connect', @('--signed-in')),
    @('connect-engine-off', 'engine-off', 'connect', @('--signed-in')),
    @('connect-moving-rooms', 'moving-rooms', 'connect', @('--signed-in')),
    @('connect-node-not-answering', 'in-meeting', 'connect', @('--signed-in', '--state-age', '12')),
    @('routing-not-joined', 'idle', 'routing', @('--signed-in')),
    @('routing-nothing-routed', 'joined-empty', 'routing', @('--signed-in')),
    @('routing-in-meeting', 'in-meeting', 'routing', @('--signed-in')),
    @('routing-recording', 'recording', 'routing', @('--signed-in')),
    @('routing-feeds-ndi', 'feeds-ndi', 'routing', @('--signed-in', '--size', '1600x900')),
    @('routing-feeds-srt', 'feeds-srt', 'routing', @('--signed-in', '--size', '1600x900')),
    @('routing-narrow', 'in-meeting', 'routing', @('--signed-in', '--size', '1024x720')),
    @('monitor-not-joined', 'idle', 'monitor', @('--signed-in')),
    @('monitor-nothing-routed', 'joined-empty', 'monitor', @('--signed-in')),
    @('monitor-in-meeting', 'in-meeting', 'monitor', @('--signed-in')),
    @('monitor-recording', 'recording', 'monitor', @('--signed-in')),
    @('monitor-engine-off', 'engine-off', 'monitor', @('--signed-in')),
    @('monitor-moving-rooms', 'moving-rooms', 'monitor', @('--signed-in')),
    @('monitor-feeds-ndi', 'feeds-ndi', 'monitor', @('--signed-in')),
    @('monitor-feeds-srt', 'feeds-srt', 'monitor', @('--signed-in')),
    @('record-not-joined', 'idle', 'record', @('--signed-in')),
    @('record-armed', 'in-meeting', 'record', @('--signed-in')),
    @('record-engine-off', 'engine-off', 'record', @('--signed-in')),
    @('record-recording', 'recording', 'record', @('--signed-in')),
    @('record-saved', 'saved', 'record', @('--signed-in')),
    # Comms states: node scripts\capture-comms-fixtures.mjs (fake engine talkback).
    @('comms-desk', 'comms', 'comms', @('--signed-in')),
    @('comms-edit-settings', 'comms', 'comms', @('--signed-in', '--comms-edit', '--comms-settings')),
    @('comms-not-host', 'comms-not-host', 'comms', @('--signed-in')),
    @('comms-not-joined', 'idle', 'comms', @('--signed-in'))
  )
  foreach ($s in $shots) {
    $name, $state, $screen, $extra = $s
    $png = Join-Path $out "$name.png"
    $shotArgs = @('--screenshot', (State $state), $screen, $png) + (Preview $state.Replace('.edited', '')) + $extra
    # The app is a GUI-subsystem exe: '&' does not wait for it and leaves
    # $LASTEXITCODE empty, so wait on the process and read its exit code.
    $quoted = $shotArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    $proc = Start-Process -FilePath $Exe -ArgumentList $quoted -Wait -PassThru -NoNewWindow
    if ($proc.ExitCode -ne 0) { throw "screenshot $name failed ($($proc.ExitCode))" }
    Write-Host "rendered $png"
  }
} finally {
  Pop-Location
}
