<#
.SYNOPSIS
  End-to-end tests without an iPhone and without admin rights:
  icam-fakephone (TCP) -> icam-agent -> shared memory (Local namespace) -> icam-vcam.dll loaded
  in-process by icam-vcamtest (Media Foundation source reader).

  Each scenario injects one fault and checks what an app would receive.

.EXAMPLE
  windows\scripts\e2e-test.ps1 -Build build\release
#>
param(
    [string]$Build = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\release'),
    [string]$Filter = '.*'   # regex on the scenario name (e.g. -Filter '4K30')
)
$ErrorActionPreference = 'Stop'
$Build = (Resolve-Path $Build).Path
$fake = Join-Path $Build 'windows\tools\fakephone\icam-fakephone.exe'
$agent = Join-Path $Build 'windows\agent\icam-agent.exe'
$test = Join-Path $Build 'windows\tools\vcamtest\icam-vcamtest.exe'
$agentLog = Join-Path $env:TEMP 'icam-e2e-agent.log'   # separate from the installed agent's log
$script:port = 38800   # fresh port per scenario; below WSL mirrored mode's reserved 44620-48715
$env:ICAM_SHM_NAMESPACE = 'Local'
$results = @()

function Parse([string[]]$out) {
    $r = @{ samples = 0; new = 0; newFps = 0.0; lat = 0.0; latP95 = 0.0; longGaps = 0; maxGap = 0.0; errors = 0 }
    foreach ($l in $out) {
        if ($l -match 'samples=(\d+).*new=(\d+) \(([\d.]+) fps\).*errors=(\d+)') { $r.samples = [int]$matches[1]; $r.new = [int]$matches[2]; $r.newFps = [double]$matches[3]; $r.errors = [int]$matches[4] }
        if ($l -match 'latency.*avg=([\d.]+).*p95=([\d.]+)') { $r.lat = [double]$matches[1]; $r.latP95 = [double]$matches[2] }
        if ($l -match 'gaps ms:.*max=([\d.]+) long\(.*\)=(\d+)') { $r.maxGap = [double]$matches[1]; $r.longGaps = [int]$matches[2] }
    }
    return $r
}

function Scenario([string]$name, [string[]]$fakeArgs, [string]$size, [int]$fps, [int]$seconds, [scriptblock]$during, [scriptblock]$check) {
    if ($name -notmatch $Filter) { return }
    Get-Process icam-agent, icam-fakephone -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Build*" } | Stop-Process -Force
    Remove-Item $agentLog -ErrorAction SilentlyContinue; $script:port++; $port = $script:port
    $fp = Start-Process -PassThru -WindowStyle Hidden $fake -ArgumentList (@('--port', $port) + $fakeArgs) -RedirectStandardOutput "$env:TEMP\icam-e2e-fake.txt"
    Start-Sleep -Milliseconds 500
    $ag = Start-Process -PassThru -WindowStyle Hidden $agent -ArgumentList '--console', '--tcp', "127.0.0.1:$port", '--log', "`"$agentLog`""
    $job = Start-Job -ScriptBlock { param($t, $s, $f, $sec) $env:ICAM_SHM_NAMESPACE = 'Local'; & $t --direct --size $s --fps $f --seconds $sec 2>&1 } -ArgumentList $test, $size, $fps, $seconds
    if ($during) { & $during $ag $fp }
    $out = Receive-Job -Job $job -Wait -AutoRemoveJob
    Start-Sleep -Milliseconds 300
    Get-Process icam-agent, icam-fakephone -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Build*" } | Stop-Process -Force
    $r = Parse $out
    $log = if (Test-Path $agentLog) { Get-Content -Encoding utf8 $agentLog } else { @() }
    $ok = & $check $r $log
    $line = '{0,-28} {1,-4} new {2,5:N1} fps  lat {3,5:N1}/{4,5:N1} ms  long gaps {5,3}  max gap {6,6:N0} ms' -f $name, ($(if ($ok) { 'PASS' } else { 'FAIL' })), $r.newFps, $r.lat, $r.latP95, $r.longGaps, $r.maxGap
    Write-Host $line
    if (-not $ok) {
        $out | ForEach-Object { Write-Host "    $_" }
        $log | Select-Object -Last 12 | ForEach-Object { Write-Host "    log: $_" }
        Get-Content "$env:TEMP\icam-e2e-fake.txt" -ErrorAction SilentlyContinue | Select-Object -First 5 | ForEach-Object { Write-Host "    fakephone: $_" }
    }
    $script:results += [pscustomobject]@{ Scenario = $name; Pass = $ok; NewFps = $r.newFps; LatAvg = $r.lat; LatP95 = $r.latP95; LongGaps = $r.longGaps; MaxGap = $r.maxGap }
}

Write-Host "iCam end-to-end tests (fakephone, in-process camera) using $Build"

Scenario 'baseline 1080p30' @() '1920x1080' 30 10 $null { param($r, $log) $r.newFps -ge 27 -and $r.errors -eq 0 -and $r.latP95 -lt 60 }
Scenario 'baseline 720p30' @() '1280x720' 30 10 $null { param($r, $log) $r.newFps -ge 27 -and $r.errors -eq 0 }
Scenario 'baseline 1080p60' @() '1920x1080' 60 10 $null { param($r, $log) $r.newFps -ge 50 -and $r.errors -eq 0 }
Scenario 'baseline 4K30' @() '3840x2160' 30 10 $null { param($r, $log) $r.newFps -ge 20 -and $r.errors -eq 0 }
# A 1.5 s network stall: the app keeps getting samples (repeats), then live frames resume, latency doesn't grow.
Scenario 'stall 1.5 s' @('--stall-at', '90', '--stall-ms', '1500') '1280x720' 30 10 $null {
    param($r, $log) $r.errors -eq 0 -and $r.newFps -ge 20 -and $r.maxGap -ge 1200 -and $r.latP95 -lt 60 }
# Connection dropped (unplug): the agent reconnects by itself and frames resume.
Scenario 'link drop + reconnect' @('--drop-at', '90') '1280x720' 30 12 $null {
    param($r, $log) $r.errors -eq 0 -and ($log -match 'link closed').Count -ge 1 -and ($log -match 'video config gen').Count -ge 2 -and $r.newFps -ge 15 }
# App sent to background on the phone: status card instead of frozen video.
Scenario 'phone app to background' @('--background-at', '60') '1280x720' 30 8 $null {
    param($r, $log) $r.errors -eq 0 -and ($log -match 'Cámara en pausa').Count -ge 1 -and $r.new -ge 50 -and $r.new -le 75 }
# Agent killed mid-stream and restarted: the camera keeps delivering (placeholder), then live again.
Scenario 'agent restart' @() '1280x720' 30 14 {
    param($ag, $fp)
    Start-Sleep -Seconds 4
    Stop-Process -Id $ag.Id -Force
    Start-Sleep -Seconds 3
    Start-Process -WindowStyle Hidden $agent -ArgumentList '--console', '--tcp', "127.0.0.1:$port", '--log', "`"$agentLog`"" | Out-Null
} { param($r, $log) $r.errors -eq 0 -and $r.newFps -ge 15 -and $r.samples -ge 350 }

Remove-Item env:ICAM_SHM_NAMESPACE
$failed = ($results | Where-Object { -not $_.Pass }).Count
if ($results.Count -eq 0) { throw "No scenarios matched '$Filter'." }
Write-Host ("{0}/{1} scenarios passed" -f ($results.Count - $failed), $results.Count)
$results | ConvertTo-Json | Set-Content (Join-Path $Build 'e2e-results.json')
exit $failed
